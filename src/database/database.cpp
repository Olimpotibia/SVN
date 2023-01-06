/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-2022 OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.org/
*/

#include "pch.hpp"

#include "config/configmanager.h"
#include "database/database.h"

Database::~Database()
{
	if (handle != nullptr) {
		mysql_close(handle);
	}
}

bool Database::connect()
{
    // connection handle initialization
    handle = mysql_init(nullptr);
    if (!handle) {
        SPDLOG_ERROR("Failed to initialize MySQL connection handle");
		return false;
    }

    // automatic reconnect
    bool reconnect = true;
    mysql_options(handle, MYSQL_OPT_RECONNECT, &reconnect);

    // check if all required parameters have been provided
    const std::string host = g_configManager().getString(MYSQL_HOST);
	const std::string user = g_configManager().getString(MYSQL_USER);
    const std::string password = g_configManager().getString(MYSQL_PASS);
	const std::string database = g_configManager().getString(MYSQL_DB);
	const int port = g_configManager().getNumber(SQL_PORT);
	const std::string socket = g_configManager().getString(MYSQL_SOCK);
	
	if (host.empty() || user.empty() || password.empty() || database.empty() || port <= 0) {
		SPDLOG_ERROR("MySQL host, user, password, database or port not provided");
	}
	
    // connects to database
    if (!mysql_real_connect(handle, host.c_str(), user.c_str(), password.c_str(), database.c_str(), port, socket.c_str(), 0)) {
        SPDLOG_ERROR("MySQL Error Message: {}", mysql_error(handle));
		return false;
    }

    DBResult_ptr result = storeQuery("SHOW VARIABLES LIKE 'max_allowed_packet'");
    if (result) {
        maxPacketSize = result->getNumber<uint64_t>("Value");
    }
    return true;
}

bool Database::beginTransaction()
{
	if (!executeQuery("BEGIN")) {
		return false;
	}

	databaseLock.lock();
	return true;
}

bool Database::rollback()
{
	if (!handle) {
		SPDLOG_ERROR("Database not initialized!");
		return false;
	}

	if (mysql_rollback(handle) != 0) {
		SPDLOG_ERROR("Message: {}", mysql_error(handle));
		databaseLock.unlock();
		return false;
	}

	databaseLock.unlock();
	return true;
}

bool Database::commit()
{
  if (!handle) {
    SPDLOG_ERROR("Database not initialized!");
    return false;
  }

	if (mysql_commit(handle) != 0) {
		SPDLOG_ERROR("Message: {}", mysql_error(handle));
		databaseLock.unlock();
		return false;
	}

	databaseLock.unlock();
	return true;
}

bool Database::executeQuery(const std::string& query)
{
	int timeout = 0;

	// Add this line to define the queryCacheLock mutex
	static std::mutex queryCacheLock;

	// Add this line to define the queryCache map
	static std::unordered_map<std::string, bool> queryCache;

	// Add this line to define the queryCacheEnabled flag
	static const bool queryCacheEnabled = true;

	// Add this line to define the threadPool thread pool
	static ThreadPool threadPool(std::thread::hardware_concurrency());

	if (!handle) {
		SPDLOG_ERROR("Database not initialized!");
		return false;
	}

	// Check query cache first
	if (queryCacheEnabled) {
		std::lock_guard<std::mutex> lock(queryCacheLock);
		auto it = queryCache.find(query);
		if (it != queryCache.end()) {
			return it->second;
		}
	}

	// Add query to thread pool
	threadPool.enqueue([&] {
		bool success = true;
		int retryCount = 0;
		MYSQL_RES* m_res = nullptr;

		// executes the query
		databaseLock.lock();
		while (mysql_real_query(handle, query.c_str(), query.length()) != 0) {
			SPDLOG_ERROR("Query: {}", query.substr(0, 256));
			SPDLOG_ERROR("Message: {}", mysql_error(handle));
			auto error = mysql_errno(handle);
			if (error != CR_SERVER_LOST && error != CR_SERVER_GONE_ERROR && error != CR_CONN_HOST_ERROR && error != 1053/*ER_SERVER_SHUTDOWN*/ && error != CR_CONNECTION_ERROR) {
				success = false;
				break;
			}
			retryCount++;
			if (timeout > 0 && retryCount > timeout) {
				success = false;
				break;
			}
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}

		if (success) {
			m_res = mysql_use_result(handle);
		}

		databaseLock.unlock();

		if (m_res) {
			// process result set rows here using cursor-based approach
			mysql_free_result(m_res);
		}

		// Add query and result to cache
		if (queryCacheEnabled) {
			std::lock_guard<std::mutex> lock(queryCacheLock);
			// queryCache[query] = success;
		}
	});

	return true;
}

DBResult_ptr Database::storeQuery(const std::string& query)
{
	if (!handle) {
		SPDLOG_ERROR("Database not initialized!");
	}

	// Use a transaction manager to handle transactions.
	TransactionManager transactionManager(handle);
	transactionManager.startTransaction();

	MYSQL_RES* res = nullptr;
	const int MAX_RETRIES = 10;
	int retries = MAX_RETRIES;
	auto tryQuery = [&]() {
		if (mysql_query(handle, query.data()) != 0) {
			int error = mysql_errno(handle);
			if (error != CR_SERVER_LOST && error != CR_SERVER_GONE_ERROR && error != CR_CONN_HOST_ERROR && error != 1053 /* ER_SERVER_SHUTDOWN */ && error != CR_CONNECTION_ERROR) {
				SPDLOG_ERROR("MySQL Error: {}", mysql_error(handle));
			}
			if (retries == 0) {
				SPDLOG_ERROR("MySQL Error: Maximum number of retries reached");
				return false;
			}
			--retries;
			std::this_thread::sleep_for(std::chrono::seconds(1));
			return true;
		} else {
			// we should call that every time as someone would call executeQuery('SELECT...')
			// as it is described in MySQL manual: "it doesn't hurt" :P
			res = mysql_store_result(handle);
			if (res == nullptr) {
				int error = mysql_errno(handle);
				if (error != CR_SERVER_LOST && error != CR_SERVER_GONE_ERROR && error != CR_CONN_HOST_ERROR && error != 1053 /* ER_SERVER_SHUTDOWN */ && error != CR_CONNECTION_ERROR) {
					SPDLOG_ERROR("MySQL Error: {}", mysql_error(handle));
				}
				if (retries == 0) {
					SPDLOG_ERROR("MySQL Error: Maximum number of retries reached");
					return false;
				}
				--retries;
				std::this_thread::sleep_for(std::chrono::seconds(1));
				return true;
			} else {
				return false;
			}
		}
	};

	while (tryQuery());
	try {
		DBResult_ptr result = std::make_shared<DBResult>(res);
		if (!result->hasNext()) {
			return nullptr;
		}
		return result;
	} catch (const std::exception& e) {
		SPDLOG_ERROR("Error creating DBResult object: {}", e.what());
		return nullptr;
	}
}

std::string Database::escapeString(const std::string& s) const
{
	return escapeBlob(s.c_str(), s.length());
}

std::string Database::escapeBlob(const char* s, uint32_t length) const
{
	// the worst case is 2n + 1
	size_t maxLength = (length * 2) + 1;

	std::string escaped;
	escaped.reserve(maxLength + 2);
	escaped.push_back('\'');

	if (length != 0) {
		char* output = new char[maxLength];
		mysql_real_escape_string(handle, output, s, length);
		escaped.append(output);
		delete[] output;
	}

	escaped.push_back('\'');
	return escaped;
}

DBResult::DBResult(MYSQL_RES* res)
{
	handle = res;

	size_t i = 0;

	MYSQL_FIELD* field = mysql_fetch_field(handle);
	while (field) {
		listNames[field->name] = i++;
		field = mysql_fetch_field(handle);
	}

	row = mysql_fetch_row(handle);
}

DBResult::~DBResult()
{
	mysql_free_result(handle);
}

std::string DBResult::getString(const std::string& s) const
{
	auto it = listNames.find(s);
	if (it == listNames.end()) {
		SPDLOG_ERROR("Column '{}' does not exist in result set", s);
		return std::string();
	}

	if (row[it->second] == nullptr) {
		return std::string();
	}

	return std::string(row[it->second]);
}

const char* DBResult::getStream(const std::string& s, unsigned long& size) const
{
	auto it = listNames.find(s);
	if (it == listNames.end()) {
		SPDLOG_ERROR("Column '{}' doesn't exist in the result set", s);
		size = 0;
		return nullptr;
	}

	if (row[it->second] == nullptr) {
		size = 0;
		return nullptr;
	}

	size = mysql_fetch_lengths(handle)[it->second];
	return row[it->second];
}

uint8_t DBResult::getU8FromString(const std::string &string, const std::string &function) const
{
	auto result = static_cast<uint8_t>(std::atoi(string.c_str()));
	if (result > std::numeric_limits<uint8_t>::max()) {
		SPDLOG_ERROR("[{}] Failed to get number value {} for tier table result, on function call: {}", __FUNCTION__, result, function);
		return 0;
	}

	return result;
}

int8_t DBResult::getInt8FromString(const std::string &string, const std::string &function) const
{
	auto result = static_cast<int8_t>(std::atoi(string.c_str()));
	if (result > std::numeric_limits<int8_t>::max()) {
		SPDLOG_ERROR("[{}] Failed to get number value {} for tier table result, on function call: {}", __FUNCTION__, result, function);
		return 0;
	}

	return result;
}

size_t DBResult::countResults() const
{
	return static_cast<size_t>(mysql_num_rows(handle));
}

bool DBResult::hasNext() const
{
	return row != nullptr;
}

bool DBResult::next()
{
  if (!handle) {
    SPDLOG_ERROR("Database not initialized!");
    return false;
  }
	row = mysql_fetch_row(handle);
	return row != nullptr;
}

DBInsert::DBInsert(std::string insertQuery) : query(std::move(insertQuery))
{
	this->length = this->query.length();
}

bool DBInsert::addRow(const std::string& row)
{
	// adds new row to buffer
	const size_t rowLength = row.length();
	length += rowLength;
	if (length > Database::getInstance().getMaxPacketSize() && !execute()) {
		return false;
	}

	if (values.empty()) {
		values.reserve(rowLength + 2);
		values.push_back('(');
		values.append(row);
		values.push_back(')');
	} else {
		values.reserve(values.length() + rowLength + 3);
		values.push_back(',');
		values.push_back('(');
		values.append(row);
		values.push_back(')');
	}
	return true;
}

bool DBInsert::addRow(std::ostringstream& row)
{
	bool ret = addRow(row.str());
	row.str(std::string());
	return ret;
}

bool DBInsert::execute()
{
	if (values.empty()) {
		return true;
	}

	// executes buffer
	bool res = Database::getInstance().executeQuery(query + values);
	values.clear();
	length = query.length();
	return res;
}
