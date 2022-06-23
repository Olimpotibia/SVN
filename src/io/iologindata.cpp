/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-2022 OpenTibiaBR <opentibiabr@outlook.com>
 * <https://github.com/opentibiabr/canary>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.org/
*/

#include "otpch.h"

#include "io/iologindata.h"
#include "game/game.h"
#include "game/scheduling/scheduler.h"
#include "creatures/monsters/monster.h"
#include "io/ioprey.h"

#include <limits>
#include <iostream>
#include <iterator>
#include <ranges>

bool IOLoginData::authenticateAccountPassword(const std::string& email, const std::string& password, account::Account *account) {
	if (account::ERROR_NO != account->LoadAccountDB(email)) {
		SPDLOG_ERROR("Email {} doesn't match any account.", email);
		return false;
	}

	std::string accountPassword;
	account->GetPassword(&accountPassword);
	if (transformToSHA1(password) != accountPassword) {
		SPDLOG_ERROR("Password '{}' doesn't match any account", transformToSHA1(password));
		return false;
	}

	return true;
}

bool IOLoginData::gameWorldAuthentication(const std::string& email, const std::string& password, std::string& characterName, uint32_t *accountId)
{
	account::Account account;
	if (!IOLoginData::authenticateAccountPassword(email, password, &account)) {
		return false;
	}

	account::Player player;
	if (account::ERROR_NO != account.GetAccountPlayer(&player, characterName)) {
		SPDLOG_ERROR("Account for player with name {} not found or deleted", characterName);
		return false;
	}

	account.GetID(accountId);

	return true;
}

account::AccountType IOLoginData::getAccountType(uint32_t accountId)
{
	std::ostringstream query;
	query << "SELECT `type` FROM `accounts` WHERE `id` = " << accountId;
	DBResult_ptr result = Database::getInstance().storeQuery(query.str());
	if (!result) {
		return account::ACCOUNT_TYPE_NORMAL;
	}
	return static_cast<account::AccountType>(result->getU16("type"));
}

void IOLoginData::setAccountType(uint32_t accountId, account::AccountType accountType)
{
	std::ostringstream query;
	query << "UPDATE `accounts` SET `type` = " << static_cast<uint16_t>(accountType) << " WHERE `id` = " << accountId;
	Database::getInstance().executeQuery(query.str());
}

void IOLoginData::updateOnlineStatus(uint32_t guid, bool login)
{
	if (g_configManager().getBoolean(ALLOW_CLONES)) {
		return;
	}

	std::ostringstream query;
	if (login) {
		query << "INSERT INTO `players_online` VALUES (" << guid << ')';
	} else {
		query << "DELETE FROM `players_online` WHERE `player_id` = " << guid;
	}
	Database::getInstance().executeQuery(query.str());
}

bool IOLoginData::preloadPlayer(Player* player, const std::string& name)
{
	Database& db = Database::getInstance();

	std::ostringstream query;
	query << "SELECT `id`, `account_id`, `group_id`, `deletion`, (SELECT `type` FROM `accounts` WHERE `accounts`.`id` = `account_id`) AS `account_type`";
	if (!g_configManager().getBoolean(FREE_PREMIUM)) {
		query << ", (SELECT `premdays` FROM `accounts` WHERE `accounts`.`id` = `account_id`) AS `premium_days`";
	}
	query << " FROM `players` WHERE `name` = " << db.escapeString(name);
	DBResult_ptr result = db.storeQuery(query.str());
	if (!result) {
		return false;
	}

	if (result->getU64("deletion") != 0) {
		return false;
	}

	player->setGUID(result->getU32("id"));
	Group* group = g_game().groups.getGroup(result->getU16("group_id"));
	if (!group) {
		SPDLOG_ERROR("Player {} has group id {} whitch doesn't exist", player->name,
					 result->getU16("group_id"));
		return false;
	}
	player->setGroup(group);
	player->accountNumber = result->getU32("account_id");
	player->accountType = static_cast<account::AccountType>(result->getU16("account_type"));
	if (!g_configManager().getBoolean(FREE_PREMIUM)) {
		player->premiumDays = result->getU16("premium_days");
	} else {
		player->premiumDays = std::numeric_limits<uint16_t>::max();
	}
	return true;
}

bool IOLoginData::loadPlayerById(Player* player, uint32_t id)
{
	Database& db = Database::getInstance();
	std::ostringstream query;
	query << "SELECT * FROM `players` WHERE `id` = " << id;
	return loadPlayer(player, db.storeQuery(query.str()));
}

bool IOLoginData::loadPlayerByName(Player* player, const std::string& name)
{
	Database& db = Database::getInstance();
	std::ostringstream query;
	query << "SELECT * FROM `players` WHERE `name` = " << db.escapeString(name);
	return loadPlayer(player, db.storeQuery(query.str()));
}

bool IOLoginData::loadPlayer(Player* player, DBResult_ptr result)
{
	if (!result) {
		return false;
	}

	Database& db = Database::getInstance();

	uint32_t accountId = result->getU32("account_id");
	account::Account acc;
	acc.SetDatabaseInterface(&db);
	acc.LoadAccountDB(accountId);

	player->setGUID(result->getU32("id"));
	player->name = result->getString("name");
	acc.GetID(&(player->accountNumber));
	acc.GetAccountType(&(player->accountType));

	if (g_configManager().getBoolean(FREE_PREMIUM)) {
		player->premiumDays = std::numeric_limits<uint16_t>::max();
	} else {
		acc.GetPremiumRemaningDays(&(player->premiumDays));
	}

	acc.GetCoins(&(player->coinBalance));

	Group* group = g_game().groups.getGroup(result->getU16("group_id"));
	if (!group) {
		SPDLOG_ERROR("Player {} has group id {} whitch doesn't exist", player->name, result->getU16("group_id"));
		return false;
	}
	player->setGroup(group);

	player->setBankBalance(result->getU64("balance"));

	player->quickLootFallbackToMainContainer = result->getBoolean("quickloot_fallback");

	player->setSex(static_cast<PlayerSex_t>(result->getU16("sex")));
	player->level = std::max<uint32_t>(1, result->getU32("level"));

	uint64_t experience = result->getU64("experience");

	uint64_t currExpCount = Player::getExpForLevel(player->level);
	uint64_t nextExpCount = Player::getExpForLevel(player->level + 1);
	if (experience < currExpCount || experience > nextExpCount) {
		experience = currExpCount;
	}

	player->experience = experience;

	if (currExpCount < nextExpCount) {
		player->levelPercent = Player::getPercentLevel(player->experience - currExpCount, nextExpCount - currExpCount);
	} else {
		player->levelPercent = 0;
	}

	player->soul = result->getU16("soul");
	player->capacity = result->getU32("cap") * 100;
	for (int i = 1; i <= 8; i++) {
		std::ostringstream ss;
		ss << "blessings" << i;
		player->addBlessing(static_cast<uint8_t>(i), result->getU8(ss.str()));
	}

	unsigned long attrSize;
	const char* attr = result->getStream("conditions", attrSize);
	PropStream propStream;
	propStream.init(attr, attrSize);

	Condition* condition = Condition::createCondition(propStream);
	while (condition) {
		if (condition->unserialize(propStream)) {
			player->storedConditionList.push_front(condition);
		} else {
			delete condition;
		}
		condition = Condition::createCondition(propStream);
	}

	if (!player->setVocation(result->getU16("vocation"))) {
		SPDLOG_ERROR("Player {} has vocation id {} whitch doesn't exist",
					 player->name, result->getU16("vocation"));
		return false;
	}

	player->mana = result->getU32("mana");
	player->manaMax = result->getU32("manamax");
	player->magLevel = result->getU32("maglevel");

	uint64_t nextManaCount = player->vocation->getReqMana(player->magLevel + 1);
	uint64_t manaSpent = result->getU64("manaspent");
	if (manaSpent > nextManaCount) {
		manaSpent = 0;
	}

	player->manaSpent = manaSpent;
	player->magLevelPercent = Player::getPercentLevel(player->manaSpent, nextManaCount);

	player->health = result->get32("health");
	player->healthMax = result->get32("healthmax");

	player->defaultOutfit.lookType = result->getU16("looktype");
	if (g_configManager().getBoolean(WARN_UNSAFE_SCRIPTS) && player->defaultOutfit.lookType != 0 && !g_game().isLookTypeRegistered(player->defaultOutfit.lookType)) {
		SPDLOG_WARN("[IOLoginData::loadPlayer] An unregistered creature looktype type with id '{}' was blocked to prevent client crash.", player->defaultOutfit.lookType);
		return false;
	}
	player->defaultOutfit.lookHead = result->getU16("lookhead");
	player->defaultOutfit.lookBody = result->getU16("lookbody");
	player->defaultOutfit.lookLegs = result->getU16("looklegs");
	player->defaultOutfit.lookFeet = result->getU16("lookfeet");
	player->defaultOutfit.lookAddons = result->getU16("lookaddons");
	player->defaultOutfit.lookMountHead = result->getU16("lookmounthead");
	player->defaultOutfit.lookMountBody = result->getU16("lookmountbody");
	player->defaultOutfit.lookMountLegs = result->getU16("lookmountlegs");
	player->defaultOutfit.lookMountFeet = result->getU16("lookmountfeet");
	player->defaultOutfit.lookFamiliarsType = result->getU16("lookfamiliarstype");
	if (g_configManager().getBoolean(WARN_UNSAFE_SCRIPTS) && player->defaultOutfit.lookFamiliarsType != 0 && !g_game().isLookTypeRegistered(player->defaultOutfit.lookFamiliarsType)) {
		SPDLOG_WARN("[IOLoginData::loadPlayer] An unregistered creature looktype type with id '{}' was blocked to prevent client crash.", player->defaultOutfit.lookFamiliarsType);
		return false;
	}
	player->isDailyReward = result->getU16("isreward");
	player->currentOutfit = player->defaultOutfit;

	if (g_game().getWorldType() != WORLD_TYPE_PVP_ENFORCED) {
		const time_t skullSeconds = result->getTime("skulltime") - Game::getTimeNow();
		if (skullSeconds > 0) {
			//ensure that we round up the number of ticks
			player->skullTicks = (skullSeconds + 2);

			uint16_t skull = result->getU16("skull");
			if (skull == SKULL_RED) {
				player->skull = SKULL_RED;
			} else if (skull == SKULL_BLACK) {
				player->skull = SKULL_BLACK;
			}
		}
	}

	player->loginPosition.x = result->getU16("posx");
	player->loginPosition.y = result->getU16("posy");
	player->loginPosition.z = result->getU8("posz");

	player->addPreyCards(result->getU64("prey_wildcard"));
	player->addTaskHuntingPoints(result->getU16("task_points"));

	player->lastLoginSaved = result->getTime("lastlogin");
	player->lastLogout = result->getTime("lastlogout");

	player->offlineTrainingTime = result->get32("offlinetraining_time") * 1000;
	player->offlineTrainingSkill = result->get32("offlinetraining_skill");

	Town* town = g_game().map.towns.getTown(result->getU32("town_id"));
	if (!town) {
		SPDLOG_ERROR("Player {} has town id {} whitch doesn't exist", player->name,
					 result->getU16("town_id"));
		return false;
	}

	player->town = town;

	const Position& loginPos = player->loginPosition;
	if (loginPos.x == 0 && loginPos.y == 0 && loginPos.z == 0) {
		player->loginPosition = player->getTemplePosition();
		SPDLOG_WARN("[IOLoginData::loadPlayer] - Failed to get login position for player with name {}, setting default temple position", player->getName());
	}

	player->staminaMinutes = result->getU16("stamina");
	player->setStoreXpBoost(result->getU16("xpboost_value"));
	player->setExpBoostStamina(result->getU16("xpboost_stamina"));

	static const std::string skillNames[] = {"skill_fist", "skill_club", "skill_sword", "skill_axe", "skill_dist", "skill_shielding", "skill_fishing", "skill_critical_hit_chance", "skill_critical_hit_damage", "skill_life_leech_chance", "skill_life_leech_amount", "skill_mana_leech_chance", "skill_mana_leech_amount"};
	static const std::string skillNameTries[] = {"skill_fist_tries", "skill_club_tries", "skill_sword_tries", "skill_axe_tries", "skill_dist_tries", "skill_shielding_tries", "skill_fishing_tries", "skill_critical_hit_chance_tries", "skill_critical_hit_damage_tries", "skill_life_leech_chance_tries", "skill_life_leech_amount_tries", "skill_mana_leech_chance_tries", "skill_mana_leech_amount_tries"};
	static constexpr size_t size = sizeof(skillNames) / sizeof(std::string);
	for (uint8_t i = 0; i < size; ++i) {
		uint16_t skillLevel = result->getU16(skillNames[i]);
		uint64_t skillTries = result->getU64(skillNameTries[i]);
		uint64_t nextSkillTries = player->vocation->getReqSkillTries(i, skillLevel + 1);
		if (skillTries > nextSkillTries) {
			skillTries = 0;
		}

		player->skills[i].level = skillLevel;
		player->skills[i].tries = skillTries;
		player->skills[i].percent = Player::getPercentLevel(skillTries, nextSkillTries);
	}

	player->setManaShield(result->getU16("manashield"));
	player->setMaxManaShield(result->getU16("max_manashield"));

	std::ostringstream query;
	query << "SELECT `guild_id`, `rank_id`, `nick` FROM `guild_membership` WHERE `player_id` = " << player->getGUID();
	if ((result = db.storeQuery(query.str()))) {
		uint32_t guildId = result->getU32("guild_id");
		uint32_t playerRankId = result->getU32("rank_id");
		player->guildNick = result->getString("nick");

		Guild* guild = g_game().getGuild(guildId);
		if (!guild) {
			guild = IOGuild::loadGuild(guildId);
			g_game().addGuild(guild);
		}

		if (guild) {
			player->guild = guild;
			GuildRank_ptr rank = guild->getRankById(playerRankId);
			if (!rank) {
				query.str(std::string());
				query << "SELECT `id`, `name`, `level` FROM `guild_ranks` WHERE `id` = " << playerRankId;

				if ((result = db.storeQuery(query.str()))) {
					guild->addRank(result->getU32("id"), result->getString("name"), result->getU8("level"));
				}

				rank = guild->getRankById(playerRankId);
				if (!rank) {
					player->guild = nullptr;
				}
			}

			player->guildRank = rank;

			IOGuild::getWarList(guildId, player->guildWarVector);

			query.str(std::string());
			query << "SELECT COUNT(*) AS `members` FROM `guild_membership` WHERE `guild_id` = " << guildId;
			if ((result = db.storeQuery(query.str()))) {
				guild->setMemberCount(result->getU32("members"));
			}
		}
	}

	// Stash load items
	query.str(std::string());
	query << "SELECT `item_count`, `item_id`  FROM `player_stash` WHERE `player_id` = " << player->getGUID();
	if ((result = db.storeQuery(query.str()))) {
		do {
			player->addItemOnStash(result->getU16("item_id"), result->getU32("item_count"));
		} while (result->next());
	}

	// Bestiary charms
	query.str(std::string());
	query << "SELECT * FROM `player_charms` WHERE `player_guid` = " << player->getGUID();
	if ((result = db.storeQuery(query.str()))) {
		player->charmPoints = result->getU32("charm_points");
		player->charmExpansion = result->getBoolean("charm_expansion");
		player->charmRuneWound = result->getU16("rune_wound");
		player->charmRuneEnflame = result->getU16("rune_enflame");
		player->charmRunePoison = result->getU16("rune_poison");
		player->charmRuneFreeze = result->getU16("rune_freeze");
		player->charmRuneZap = result->getU16("rune_zap");
		player->charmRuneCurse = result->getU16("rune_curse");
		player->charmRuneCripple = result->getU16("rune_cripple");
		player->charmRuneParry = result->getU16("rune_parry");
		player->charmRuneDodge = result->getU16("rune_dodge");
		player->charmRuneAdrenaline = result->getU16("rune_adrenaline");
		player->charmRuneNumb = result->getU16("rune_numb");
		player->charmRuneCleanse = result->getU16("rune_cleanse");
		player->charmRuneBless = result->getU16("rune_bless");
		player->charmRuneScavenge = result->getU16("rune_scavenge");
		player->charmRuneGut = result->getU16("rune_gut");
		player->charmRuneLowBlow = result->getU16("rune_low_blow");
		player->charmRuneDivine = result->getU16("rune_divine");
		player->charmRuneVamp = result->getU16("rune_vamp");
		player->charmRuneVoid = result->getU16("rune_void");
		player->UsedRunesBit = result->get32("UsedRunesBit");
		player->UnlockedRunesBit = result->get32("UnlockedRunesBit");

		unsigned long attrBestSize;
		const char* Bestattr = result->getStream("tracker list", attrBestSize);
		PropStream propBestStream;
		propBestStream.init(Bestattr, attrBestSize);

		uint16_t raceid_t;
		while (propBestStream.read<uint16_t>(raceid_t)) {
			MonsterType* tmp_tt = g_monsters().getMonsterTypeByRaceId(raceid_t);
			if (tmp_tt) {
				player->addBestiaryTrackerList(tmp_tt);
			}
		}


	} else {
		query.str(std::string());
		query << "INSERT INTO `player_charms` (`player_guid`) VALUES (" << player->getGUID() << ')';
		Database::getInstance().executeQuery(query.str());
	}

	query.str(std::string());
	query << "SELECT `player_id`, `name` FROM `player_spells` WHERE `player_id` = " << player->getGUID();
	if ((result = db.storeQuery(query.str()))) {
		do {
			player->learnedInstantSpellList.emplace_front(result->getString("name"));
		} while (result->next());
	}

	//load inventory items
	ItemMap itemMap;

	query.str(std::string());
	query << "SELECT `player_id`, `time`, `target`, `unavenged` FROM `player_kills` WHERE `player_id` = " << player->getGUID();
	if ((result = db.storeQuery(query.str()))) {
		do {
			time_t killTime = result->getTime("time");
			if ((Game::getTimeNow() - killTime) <= g_configManager().getNumber(FRAG_TIME)) {
				player->unjustifiedKills.emplace_back(result->getU32("target"), killTime, result->getBoolean("unavenged"));
			}
		} while (result->next());
	}

	query.str(std::string());
	query << "SELECT `pid`, `sid`, `itemtype`, `count`, `attributes` FROM `player_items` WHERE `player_id` = " << player->getGUID() << " ORDER BY `sid` DESC";

	std::vector<std::pair<uint8_t, Container*>> openContainersList;

	if ((result = db.storeQuery(query.str()))) {
		loadItems(itemMap, result);

		for (ItemMap::const_reverse_iterator it = itemMap.rbegin(), end = itemMap.rend(); it != end; ++it) {
			const auto& [itemPair, itemIdPair] = it->second;
			const Item* item = itemPair;
			if (item == nullptr) {
				SPDLOG_ERROR("[IOLoginData::loadPlayer (1)] - Item is nullptr");
				continue;
			}

			int32_t pid = itemIdPair;
			// Casting item for send non const item, keep in mind that the cast item must never be modified, in the future we must modify the functions so that they always accept the const item
			auto castItem = std::bit_cast<Item*>(item);

			if (pid >= CONST_SLOT_FIRST && pid <= CONST_SLOT_LAST) {
				player->internalAddThing(pid, castItem);
				castItem->startDecaying();
			} else {
				ItemMap::const_iterator it2 = itemMap.find(pid);
				if (it2 == itemMap.end()) {
					continue;
				}

				Container* container = it2->second.first->getContainer();
				if (container) {
					container->internalAddThing(castItem);
					castItem->startDecaying();
				}
			}

			Container* itemContainer = castItem->getContainer();
			if (itemContainer) {
				int64_t cid = item->getIntAttr(ITEM_ATTRIBUTE_OPENCONTAINER);
				if (cid > 0) {
					openContainersList.emplace_back(std::make_pair(cid, itemContainer));
				}
				if (item->hasAttribute(ITEM_ATTRIBUTE_QUICKLOOTCONTAINER)) {
					int64_t flags = item->getIntAttr(ITEM_ATTRIBUTE_QUICKLOOTCONTAINER);
					for (uint8_t category = OBJECTCATEGORY_FIRST; category <= OBJECTCATEGORY_LAST; category++) {
						if (hasBitSet(1 << category, flags)) {
							player->setLootContainer((ObjectCategory_t)category, itemContainer, true);
						}
					}
				}
			}
		}
	}

	std::ranges::sort(openContainersList.begin(), openContainersList.end(), [](const std::pair<uint8_t, Container*> &left, const std::pair<uint8_t, Container*> &right) {
		return left.first < right.first;
	});

	for (const auto& [containerCidPair, openContainerPair] : openContainersList) {
		player->addContainer(containerCidPair - 1, openContainerPair);
		player->onSendContainer(openContainerPair);
	}

	// Store Inbox
	if (!player->inventory[CONST_SLOT_STORE_INBOX]) {
		player->internalAddThing(CONST_SLOT_STORE_INBOX, Item::CreateItem(ITEM_STORE_INBOX));
	}

	//load depot items
	itemMap.clear();

	query.str(std::string());
	query << "SELECT `pid`, `sid`, `itemtype`, `count`, `attributes` FROM `player_depotitems` WHERE `player_id` = " << player->getGUID() << " ORDER BY `sid` DESC";
	if ((result = db.storeQuery(query.str()))) {
		loadItems(itemMap, result);

		for (ItemMap::const_reverse_iterator it = itemMap.rbegin(), end = itemMap.rend(); it != end; ++it) {
			const auto& [itemPair, itemIdPair] = it->second;
			const Item* item = itemPair;
			if (item == nullptr) {
				SPDLOG_ERROR("[IOLoginData::loadPlayer (2)] - Item is nullptr");
				continue;
			}

			// Casting item for send non const item, keep in mind that the cast item must never be modified, in the future we must modify the functions so that they always accept the const item
			auto castItem = std::bit_cast<Item*>(item);
			int32_t pid = itemIdPair;
			if (pid >= 0 && pid < 100) {
				DepotChest* depotChest = player->getDepotChest(pid, true);
				if (depotChest) {
					depotChest->internalAddThing(castItem);
					castItem->startDecaying();
				}
			} else {
				ItemMap::const_iterator it2 = itemMap.find(pid);
				if (it2 == itemMap.end()) {
					continue;
				}

				Container* container = it2->second.first->getContainer();
				if (container) {
					container->internalAddThing(castItem);
					castItem->startDecaying();
				}
			}
		}
	}

	//load reward chest items
	itemMap.clear();

	query.str(std::string());
	query << "SELECT `pid`, `sid`, `itemtype`, `count`, `attributes` FROM `player_rewards` WHERE `player_id` = " << player->getGUID() << " ORDER BY `sid` DESC";
	if ((result = db.storeQuery(query.str()))) {
		loadItems(itemMap, result);

		//first loop handles the reward containers to retrieve its date attribute
		//for (ItemMap::iterator it = itemMap.begin(), end = itemMap.end(); it != end; ++it) {
		for (auto& [itemMapPairFirst, itemMapPairSecond] : itemMap) {
			const auto& [itemPair, itemIdPair] = itemMapPairSecond;
			Item* item = itemPair;
			if (item == nullptr) {
				SPDLOG_ERROR("[IOLoginData:loadPlayer] - Item is nullptr");
				continue;
			}

			int32_t pid = itemIdPair;
			if (pid >= 0 && pid < 100) {
				Reward* reward = player->getReward(item->getIntAttr(ITEM_ATTRIBUTE_DATE), true);
				if (reward) {
					itemMapPairSecond = std::pair<Item*, int32_t>(reward->getItem(), pid); //update the map with the special reward container
				}
			} else {
				break;
			}
		}

		//second loop (this time a reverse one) to insert the items in the correct order
		//for (ItemMap::const_reverse_iterator it = itemMap.rbegin(), end = itemMap.rend(); it != end; ++it) {
		for (ItemMap::const_reverse_iterator it = itemMap.rbegin(), end = itemMap.rend(); it != end; ++it) {
			const auto& [itemPair, itemIdPair] = it->second;
			Item* item = itemPair;
			if (item == nullptr) {
				SPDLOG_ERROR("[IOLoginData::loadPlayer (3)] - Item is nullptr");
				continue;
			}

			int32_t pid = itemIdPair;
			if (pid >= 0 && pid < 100) {
				break;
			}

			ItemMap::const_iterator it2 = itemMap.find(pid);
			if (it2 == itemMap.end()) {
				continue;
			}

			Container* container = it2->second.first->getContainer();
			if (container) {
				container->internalAddThing(item);
			}
		}
	}

	//load inbox items
	itemMap.clear();

	query.str(std::string());
	query << "SELECT `pid`, `sid`, `itemtype`, `count`, `attributes` FROM `player_inboxitems` WHERE `player_id` = " << player->getGUID() << " ORDER BY `sid` DESC";
	if ((result = db.storeQuery(query.str()))) {
		loadItems(itemMap, result);

		for (ItemMap::const_reverse_iterator it = itemMap.rbegin(), end = itemMap.rend(); it != end; ++it) {
			const auto& [itemPair, itemIdPair] = it->second;
			Item* item = itemPair;
			if (item == nullptr) {
				SPDLOG_ERROR("[IOLoginData::loadPlayer (4)] - Item is nullptr");
				continue;
			}

			int32_t pid = itemIdPair;

			if (pid >= 0 && pid < 100) {
				player->getInbox()->internalAddThing(item);
				item->startDecaying();
			} else {
				ItemMap::const_iterator it2 = itemMap.find(pid);

				if (it2 == itemMap.end()) {
					continue;
				}

				Container* container = it2->second.first->getContainer();
				if (container) {
					container->internalAddThing(item);
					item->startDecaying();
				}
			}
		}
	}

	//load storage map
	query.str(std::string());
	query << "SELECT `key`, `value` FROM `player_storage` WHERE `player_id` = " << player->getGUID();
	if ((result = db.storeQuery(query.str()))) {
		do {
			player->addStorageValue(result->getU32("key"), result->get32("value"), true);
		} while (result->next());
	}

	//load vip
	query.str(std::string());
	query << "SELECT `player_id` FROM `account_viplist` WHERE `account_id` = " << player->getAccount();
	if ((result = db.storeQuery(query.str()))) {
		do {
			player->addVIPInternal(result->getU32("player_id"));
		} while (result->next());
	}

	// Load prey class
	if (g_configManager().getBoolean(PREY_ENABLED)) {
		query.str(std::string());
		query << "SELECT * FROM `player_prey` WHERE `player_id` = " << player->getGUID();
		if (result = db.storeQuery(query.str())) {
			do {
				auto slot = new PreySlot(static_cast<PreySlot_t>(result->getU16("slot")));
				slot->state = static_cast<PreyDataState_t>(result->getU16("state"));
				slot->selectedRaceId = result->getU16("raceid");
				slot->option = static_cast<PreyOption_t>(result->getU16("option"));
				slot->bonus = static_cast<PreyBonus_t>(result->getU16("bonus_type"));
				slot->bonusRarity = static_cast<uint8_t>(result->getU16("bonus_rarity"));
				slot->bonusPercentage = result->getU16("bonus_percentage");
				slot->bonusTimeLeft = result->getU16("bonus_time");
				slot->freeRerollTimeStamp = result->get64("free_reroll");

				unsigned long preySize;
				const char* preyStream = result->getStream("monster_list", preySize);
				PropStream propPreyStream;
				propPreyStream.init(preyStream, preySize);

				uint16_t raceId;
				while (propPreyStream.read<uint16_t>(raceId)) {
					slot->raceIdList.push_back(raceId);
				}

				player->setPreySlotClass(slot);
			} while (result->next());
		}
	}

	// Load task hunting class
	if (g_configManager().getBoolean(TASK_HUNTING_ENABLED)) {
		query.str(std::string());
		query << "SELECT * FROM `player_taskhunt` WHERE `player_id` = " << player->getGUID();
		if (result = db.storeQuery(query.str())) {
			do {
				auto slot = new TaskHuntingSlot(static_cast<PreySlot_t>(result->getU16("slot")));
				slot->state = static_cast<PreyTaskDataState_t>(result->getU16("state"));
				slot->selectedRaceId = result->getU16("raceid");
				slot->upgrade = result->getBoolean("upgrade");
				slot->rarity = static_cast<uint8_t>(result->getU16("rarity"));
				slot->currentKills = result->getU16("kills");
				slot->disabledUntilTimeStamp = result->get64("disabled_time");
				slot->freeRerollTimeStamp = result->get64("free_reroll");

				unsigned long taskHuntSize;
				const char* taskHuntStream = result->getStream("monster_list", taskHuntSize);
				PropStream propTaskHuntStream;
				propTaskHuntStream.init(taskHuntStream, taskHuntSize);

				uint16_t raceId;
				while (propTaskHuntStream.read<uint16_t>(raceId)) {
					slot->raceIdList.push_back(raceId);
				}

				if (slot->state == PreyTaskDataState_Inactive && slot->disabledUntilTimeStamp < OTSYS_TIME()) {
					slot->state = PreyTaskDataState_Selection;
				}

				player->setTaskHuntingSlotClass(slot);
			} while (result->next());
		}
	}

	player->initializePrey();
	player->initializeTaskHunting();
	player->updateBaseSpeed();
	player->updateInventoryWeight();
	player->updateInventoryImbuement(true);
	player->updateItemsLight(true);
	return true;
}

bool IOLoginData::saveOpenContainerItems(const Item &item, const std::map<uint8_t, OpenContainer>& openContainers, std::list<ContainerPair>& queue, int32_t runningId)
{
	if (const Container* container = item.getContainer();
		container != nullptr)
	{
		// Casting item for send non const item, keep in mind that the cast item must never be modified, in the future we must refactor the Item class so that they always accept the const item
		auto setContainer = std::bit_cast<Container*>(container);
		if (container->getIntAttr(ITEM_ATTRIBUTE_OPENCONTAINER) > 0) {
			setContainer->setIntAttr(ITEM_ATTRIBUTE_OPENCONTAINER, 0);
		}

		if (openContainers.empty()) {
			SPDLOG_DEBUG("[IOLoginData::saveItems] - Player open containers is empty");
			return false;
		}

		for (const auto& [containerCidPair, openContainerPair] : openContainers) {
			if (auto openContainer = openContainerPair.container;
				openContainer == container)
			{
				setContainer->setIntAttr(ITEM_ATTRIBUTE_OPENCONTAINER, ((int)containerCidPair) + 1);
				break;
			}
		}

		queue.emplace_back(container, runningId);
	}
	return true;
}

bool IOLoginData::saveOpenSubContainerItems(const Item &item, const std::map<uint8_t, OpenContainer>& openContainers, std::list<ContainerPair>& queue, int32_t runningId)
{
	if (const Container* container = item.getContainer();
		container != nullptr)
	{
		queue.emplace_back(container, runningId);
		// Casting item for send non const item, keep in mind that the cast item must never be modified, in the future we must refactor the Item class so that they always accept the const item
		auto setContainer = std::bit_cast<Container*>(container);
		if (container->getIntAttr(ITEM_ATTRIBUTE_OPENCONTAINER) > 0) {
			setContainer->setIntAttr(ITEM_ATTRIBUTE_OPENCONTAINER, 0);
		}

		if (!openContainers.empty()) {
			for (const auto& [containerCidPair, openContainerPair] : openContainers) {
				if (auto openContainer = openContainerPair.container;
					openContainer == container)
				{
					setContainer->setIntAttr(ITEM_ATTRIBUTE_OPENCONTAINER, ((int)containerCidPair) + 1);
					break;
				}
			}
		}
	}
	return true;
}

bool IOLoginData::saveItems(const Player* player, const ItemBlockList& itemList, DBInsert& query_insert, PropWriteStream& propWriteStream)
{
	Database& db = Database::getInstance();

	std::ostringstream ss;

	std::list<ContainerPair> queue;

	int32_t runningId = 100;
	const auto& openContainers = player->getOpenContainers();
	for (const auto& [itemIdPair, itemPair] : itemList) {
		int32_t pid = itemIdPair;
		Item* item = itemPair;
		if (item == nullptr) {
			SPDLOG_ERROR("[IOLoginData::saveItems] - Item is nullptr");
			continue;
		}
		++runningId;

		if (!saveOpenContainerItems(*item, openContainers, queue, runningId)) {
			SPDLOG_DEBUG("Player not have container for save");
			continue;
		}

		propWriteStream.clear();
		item->serializeAttr(propWriteStream);

		size_t attributesSize;
		const char* attributes = propWriteStream.getStream(attributesSize);

		ss << player->getGUID() << ',' << pid << ',' << runningId << ',' << item->getID() << ',' << item->getSubType() << ',' << db.escapeBlob(attributes, attributesSize);
		if (!query_insert.addRow(ss)) {
			return false;
		}

	}

	while (!queue.empty()) {
		const ContainerPair& containerPair = queue.front();
		const Container* container = containerPair.first;
		int32_t parentId = containerPair.second;
		queue.pop_front();

		for (Item* item : container->getItemList()) {
			++runningId;

			if (!saveOpenSubContainerItems(*item, openContainers, queue, runningId)) {
				SPDLOG_DEBUG("Player not have container for save");
				continue;
			}

			propWriteStream.clear();
			item->serializeAttr(propWriteStream);

			size_t attributesSize;
			const char* attributes = propWriteStream.getStream(attributesSize);

			ss << player->getGUID() << ',' << parentId << ',' << runningId << ',' << item->getID() << ',' << item->getSubType() << ',' << db.escapeBlob(attributes, attributesSize);
			if (!query_insert.addRow(ss)) {
				return false;
			}
		}
	}
	return query_insert.execute();
}

bool IOLoginData::savePlayer(Player* player)
{
	if (player->getHealth() <= 0) {
		player->changeHealth(1);
	}
	Database& db = Database::getInstance();

	std::ostringstream query;
	query << "SELECT `save` FROM `players` WHERE `id` = " << player->getGUID();
	DBResult_ptr result = db.storeQuery(query.str());
	if (!result) {
		SPDLOG_WARN("[IOLoginData::savePlayer] - Error for select result query from player: {}", player->getName());
		return false;
	}

	if (result->getU16("save") == 0) {
		query.str(std::string());
		query << "UPDATE `players` SET `lastlogin` = " << player->lastLoginSaved << ", `lastip` = " << player->lastIP << " WHERE `id` = " << player->getGUID();
		return db.executeQuery(query.str());
	}

	//First, an UPDATE query to write the player itself
	query.str(std::string());
	query << "UPDATE `players` SET ";
	query << "`level` = " << player->level << ',';
	query << "`group_id` = " << player->group->id << ',';
	query << "`vocation` = " << player->getVocationId() << ',';
	query << "`health` = " << player->health << ',';
	query << "`healthmax` = " << player->healthMax << ',';
	query << "`experience` = " << player->experience << ',';
	query << "`lookbody` = " << static_cast<uint32_t>(player->defaultOutfit.lookBody) << ',';
	query << "`lookfeet` = " << static_cast<uint32_t>(player->defaultOutfit.lookFeet) << ',';
	query << "`lookhead` = " << static_cast<uint32_t>(player->defaultOutfit.lookHead) << ',';
	query << "`looklegs` = " << static_cast<uint32_t>(player->defaultOutfit.lookLegs) << ',';
	query << "`looktype` = " << player->defaultOutfit.lookType << ',';
	query << "`lookaddons` = " << static_cast<uint32_t>(player->defaultOutfit.lookAddons) << ',';
	query << "`lookmountbody` = " << static_cast<uint32_t>(player->defaultOutfit.lookMountBody) << ',';
	query << "`lookmountfeet` = " << static_cast<uint32_t>(player->defaultOutfit.lookMountFeet) << ',';
	query << "`lookmounthead` = " << static_cast<uint32_t>(player->defaultOutfit.lookMountHead) << ',';
	query << "`lookmountlegs` = " << static_cast<uint32_t>(player->defaultOutfit.lookMountLegs) << ',';
	query << "`lookfamiliarstype` = " << player->defaultOutfit.lookFamiliarsType << ',';
	query << "`isreward` = " << static_cast<uint16_t>(player->isDailyReward) << ',';
	query << "`maglevel` = " << player->magLevel << ',';
	query << "`mana` = " << player->mana << ',';
	query << "`manamax` = " << player->manaMax << ',';
	query << "`manaspent` = " << player->manaSpent << ',';
	query << "`soul` = " << static_cast<uint16_t>(player->soul) << ',';
	query << "`town_id` = " << player->town->getID() << ',';

	const Position& loginPosition = player->getLoginPosition();
	query << "`posx` = " << loginPosition.getX() << ',';
	query << "`posy` = " << loginPosition.getY() << ',';
	query << "`posz` = " << loginPosition.getZ() << ',';

	query << "`prey_wildcard` = " << player->getPreyCards() << ',';
	query << "`task_points` = " << player->getTaskHuntingPoints() << ',';

	query << "`cap` = " << (player->capacity / 100) << ',';
	query << "`sex` = " << static_cast<uint16_t>(player->sex) << ',';

	if (player->lastLoginSaved != 0) {
		query << "`lastlogin` = " << player->lastLoginSaved << ',';
	}

	if (player->lastIP != 0) {
		query << "`lastip` = " << player->lastIP << ',';
	}

	//serialize conditions
	PropWriteStream propWriteStream;
	for (Condition* condition : player->conditions) {
		if (condition->isPersistent()) {
			condition->serialize(propWriteStream);
			propWriteStream.write<uint8_t>(CONDITIONATTR_END);
		}
	}

	size_t attributesSize;
	const char* attributes = propWriteStream.getStream(attributesSize);

	query << "`conditions` = " << db.escapeBlob(attributes, attributesSize) << ',';

	if (g_game().getWorldType() != WORLD_TYPE_PVP_ENFORCED) {
		int64_t skullTime = 0;

		if (player->skullTicks > 0) {
			skullTime = Game::getTimeNow() + player->skullTicks;
		}

		query << "`skulltime` = " << skullTime << ',';

		Skulls_t skull = SKULL_NONE;
		if (player->skull == SKULL_RED) {
			skull = SKULL_RED;
		} else if (player->skull == SKULL_BLACK) {
			skull = SKULL_BLACK;
		}
		query << "`skull` = " << static_cast<int64_t>(skull) << ',';
	}

	query << "`lastlogout` = " << player->getLastLogout() << ',';
	query << "`balance` = " << player->bankBalance << ',';
	query << "`offlinetraining_time` = " << player->getOfflineTrainingTime() / 1000 << ',';
	query << "`offlinetraining_skill` = " << player->getOfflineTrainingSkill() << ',';
	query << "`stamina` = " << player->getStaminaMinutes() << ',';
	query << "`skill_fist` = " << player->skills[SKILL_FIST].level << ',';
	query << "`skill_fist_tries` = " << player->skills[SKILL_FIST].tries << ',';
	query << "`skill_club` = " << player->skills[SKILL_CLUB].level << ',';
	query << "`skill_club_tries` = " << player->skills[SKILL_CLUB].tries << ',';
	query << "`skill_sword` = " << player->skills[SKILL_SWORD].level << ',';
	query << "`skill_sword_tries` = " << player->skills[SKILL_SWORD].tries << ',';
	query << "`skill_axe` = " << player->skills[SKILL_AXE].level << ',';
	query << "`skill_axe_tries` = " << player->skills[SKILL_AXE].tries << ',';
	query << "`skill_dist` = " << player->skills[SKILL_DISTANCE].level << ',';
	query << "`skill_dist_tries` = " << player->skills[SKILL_DISTANCE].tries << ',';
	query << "`skill_shielding` = " << player->skills[SKILL_SHIELD].level << ',';
	query << "`skill_shielding_tries` = " << player->skills[SKILL_SHIELD].tries << ',';
	query << "`skill_fishing` = " << player->skills[SKILL_FISHING].level << ',';
	query << "`skill_fishing_tries` = " << player->skills[SKILL_FISHING].tries << ',';
	query << "`skill_critical_hit_chance` = " << player->skills[SKILL_CRITICAL_HIT_CHANCE].level << ',';
	query << "`skill_critical_hit_chance_tries` = " << player->skills[SKILL_CRITICAL_HIT_CHANCE].tries << ',';
	query << "`skill_critical_hit_damage` = " << player->skills[SKILL_CRITICAL_HIT_DAMAGE].level << ',';
	query << "`skill_critical_hit_damage_tries` = " << player->skills[SKILL_CRITICAL_HIT_DAMAGE].tries << ',';
	query << "`skill_life_leech_chance` = " << player->skills[SKILL_LIFE_LEECH_CHANCE].level << ',';
	query << "`skill_life_leech_chance_tries` = " << player->skills[SKILL_LIFE_LEECH_CHANCE].tries << ',';
	query << "`skill_life_leech_amount` = " << player->skills[SKILL_LIFE_LEECH_AMOUNT].level << ',';
	query << "`skill_life_leech_amount_tries` = " << player->skills[SKILL_LIFE_LEECH_AMOUNT].tries << ',';
	query << "`skill_mana_leech_chance` = " << player->skills[SKILL_MANA_LEECH_CHANCE].level << ',';
	query << "`skill_mana_leech_chance_tries` = " << player->skills[SKILL_MANA_LEECH_CHANCE].tries << ',';
	query << "`skill_mana_leech_amount` = " << player->skills[SKILL_MANA_LEECH_AMOUNT].level << ',';
	query << "`skill_mana_leech_amount_tries` = " << player->skills[SKILL_MANA_LEECH_AMOUNT].tries << ',';
	query << "`manashield` = " << player->getManaShield() << ',';
	query << "`max_manashield` = " << player->getMaxManaShield() << ',';
	query << "`xpboost_value` = " << player->getStoreXpBoost() << ',';
	query << "`xpboost_stamina` = " << player->getExpBoostStamina() << ',';
	query << "`quickloot_fallback` = " << (player->quickLootFallbackToMainContainer ? 1 : 0) << ',';

	if (!player->isOffline()) {
		query << "`onlinetime` = `onlinetime` + " << (Game::getTimeNow() - player->lastLoginSaved) << ',';
	}
	for (int i = 1; i <= 8; i++) {
		query << "`blessings" << i << "`" << " = " << static_cast<uint32_t>(player->getBlessingCount(i)) << ((i == 8) ? ' ' : ',');
	}
	query << " WHERE `id` = " << player->getGUID();

	DBTransaction transaction;
	if (!transaction.begin()) {
		SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to begin transaction for player with name {}", player->getName());
		return false;
	}

	if (!db.executeQuery(query.str())) {
		SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to execute query for player with name {}", player->getName());
		return false;
	}

	// Stash save items
	query.str(std::string());
	query << "DELETE FROM `player_stash` WHERE `player_id` = " << player->getGUID();
	db.executeQuery(query.str());
	for (auto [itemIdPair, itemAmountPair] : player->getStashItems()) {
		query.str(std::string());
		query << "INSERT INTO `player_stash` (`player_id`,`item_id`,`item_count`) VALUES (";
		query << player->getGUID() << ", ";
		query << itemIdPair << ", ";
		query << itemAmountPair << ")";
		db.executeQuery(query.str());
	}

	// learned spells
	query.str(std::string());
	query << "DELETE FROM `player_spells` WHERE `player_id` = " << player->getGUID();
	if (!db.executeQuery(query.str())) {
		return false;
	}

	query.str(std::string());

	DBInsert spellsQuery("INSERT INTO `player_spells` (`player_id`, `name` ) VALUES ");
	for (const std::string& spellName : player->learnedInstantSpellList) {
		query << player->getGUID() << ',' << db.escapeString(spellName);
		if (!spellsQuery.addRow(query)) {
			SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to add spells row for player with name {}", player->getName());
			return false;
		}
	}

	if (!spellsQuery.execute()) {
		SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to execute spells query for player with name {}", player->getName());
		return false;
	}

	// Player kills
	query.str(std::string());
	query << "DELETE FROM `player_kills` WHERE `player_id` = " << player->getGUID();
	if (!db.executeQuery(query.str())) {
		SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to execute query skills for player with name {}", player->getName());
		return false;
	}

	//player bestiary charms
	query.str(std::string());
	query << "UPDATE `player_charms` SET ";
	query << "`charm_points` = " << player->charmPoints << ',';
	query << "`charm_expansion` = " << ((player->charmExpansion) ? 1 : 0) << ',';
	query << "`rune_wound` = " << player->charmRuneWound << ',';
	query << "`rune_enflame` = " << player->charmRuneEnflame << ',';
	query << "`rune_poison` = " << player->charmRunePoison << ',';
	query << "`rune_freeze` = " << player->charmRuneFreeze << ',';
	query << "`rune_zap` = " << player->charmRuneZap << ',';
	query << "`rune_curse` = " << player->charmRuneCurse << ',';
	query << "`rune_cripple` = " << player->charmRuneCripple << ',';
	query << "`rune_parry` = " << player->charmRuneParry << ',';
	query << "`rune_dodge` = " << player->charmRuneDodge << ',';
	query << "`rune_adrenaline` = " << player->charmRuneAdrenaline << ',';
	query << "`rune_numb` = " << player->charmRuneNumb << ',';
	query << "`rune_cleanse` = " << player->charmRuneCleanse << ',';
	query << "`rune_bless` = " << player->charmRuneBless << ',';
	query << "`rune_scavenge` = " << player->charmRuneScavenge << ',';
	query << "`rune_gut` = " << player->charmRuneGut << ',';
	query << "`rune_low_blow` = " << player->charmRuneLowBlow << ',';
	query << "`rune_divine` = " << player->charmRuneDivine << ',';
	query << "`rune_vamp` = " << player->charmRuneVamp << ',';
	query << "`rune_void` = " << player->charmRuneVoid << ',';
	query << "`UsedRunesBit` = " << player->UsedRunesBit << ',';
	query << "`UnlockedRunesBit` = " << player->UnlockedRunesBit << ',';

	// Bestiary tracker
	PropWriteStream propBestiaryStream;
	for (MonsterType* trackedType : player->getBestiaryTrackerList()) {
		propBestiaryStream.write<uint16_t>(trackedType->info.raceid);
	}
	size_t trackerSize;
	const char* trackerList = propBestiaryStream.getStream(trackerSize);
	query << " `tracker list` = " << db.escapeBlob(trackerList, trackerSize);
	query << " WHERE `player_guid` = " << player->getGUID();

	if (!db.executeQuery(query.str())) {
		SPDLOG_WARN("[IOLoginData::savePlayer] - Error saving bestiary data from player: {}", player->getName());
		return false;
	}

	query.str(std::string());

	DBInsert killsQuery("INSERT INTO `player_kills` (`player_id`, `target`, `time`, `unavenged`) VALUES");
	for (const auto& kill : player->unjustifiedKills) {
		query << player->getGUID() << ',' << kill.target << ',' << kill.time << ',' << kill.unavenged;
		if (!killsQuery.addRow(query)) {
			SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to add kills row for player with name {}", player->getName());
			return false;
		}
	}

	if (!killsQuery.execute()) {
		SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to execute kills query for player with name {}", player->getName());
		return false;
	}

	//item saving
	query << "DELETE FROM `player_items` WHERE `player_id` = " << player->getGUID();
	if (!db.executeQuery(query.str())) {
		SPDLOG_WARN("[IOLoginData::savePlayer] - Error delete query 'player_items' from player: {}", player->getName());
		return false;
	}

	DBInsert itemsQuery("INSERT INTO `player_items` (`player_id`, `pid`, `sid`, `itemtype`, `count`, `attributes`) VALUES ");

	ItemBlockList itemList;
	for (int32_t slotId = CONST_SLOT_FIRST; slotId <= CONST_SLOT_LAST; ++slotId) {
		Item* item = player->inventory[slotId];
		if (item) {
			itemList.emplace_back(slotId, item);
		}
	}

	if (!saveItems(player, itemList, itemsQuery, propWriteStream)) {
		SPDLOG_WARN("[IOLoginData::savePlayer] - Failed for save items from player: {}", player->getName());
		return false;
	}

	if (player->lastDepotId != -1) {
		// Save depot items
		query.str(std::string());
		query << "DELETE FROM `player_depotitems` WHERE `player_id` = " << player->getGUID();

		if (!db.executeQuery(query.str())) {
			SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to execute depot query for player with name {}", player->getName());
			return false;
		}

		DBInsert depotQuery("INSERT INTO `player_depotitems` (`player_id`, `pid`, `sid`, `itemtype`, `count`, `attributes`) VALUES ");
		itemList.clear();

		for (const auto& [itemDepotIdPair, itemDepotChestPair] : player->depotChests) {
			const DepotChest* depotChest = itemDepotChestPair;
			if (depotChest == nullptr) {
				SPDLOG_ERROR("[IOLoginData::savePlayer] - Depot chest is nullptr");
				continue;
			}

			for (Item* item : depotChest->getItemList()) {
				itemList.emplace_back(itemDepotIdPair, item);
			}
		}

		if (!saveItems(player, itemList, depotQuery, propWriteStream)) {
			SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to save depot items for player with name {}", player->getName());
			return false;
		}
	}

	// Save reward items
	query.str(std::string());
	query << "DELETE FROM `player_rewards` WHERE `player_id` = " << player->getGUID();

	if (!db.executeQuery(query.str())) {
		SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to execute reward query for player with name {}", player->getName());
		return false;
	}

	std::vector<uint32_t> rewardList;
	player->getRewardList(rewardList);

	if (!rewardList.empty()) {
		DBInsert rewardQuery("INSERT INTO `player_rewards` (`player_id`, `pid`, `sid`, `itemtype`, `count`, `attributes`) VALUES ");
		itemList.clear();

		int running = 0;
		for (const auto& rewardId : rewardList) {
			Reward* reward = player->getReward(rewardId, false);
			// rewards that are empty or older than 7 days aren't stored
			if (!reward->empty() && (Game::getTimeNow() - rewardId <= 60 * 60 * 24 * 7)) {
				itemList.emplace_back(++running, reward);
			}
		}

		if (!saveItems(player, itemList, rewardQuery, propWriteStream)) {
			SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to save reward items for player with name {}", player->getName());
			return false;
		}
	}

	// Save inbox items
	query.str(std::string());
	query << "DELETE FROM `player_inboxitems` WHERE `player_id` = " << player->getGUID();
	if (!db.executeQuery(query.str())) {
		SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to execute inbox items query for player with name {}", player->getName());
		return false;
	}

	DBInsert inboxQuery("INSERT INTO `player_inboxitems` (`player_id`, `pid`, `sid`, `itemtype`, `count`, `attributes`) VALUES ");
	itemList.clear();

	for (Item* item : player->getInbox()->getItemList()) {
		itemList.emplace_back(0, item);
	}

	if (!saveItems(player, itemList, inboxQuery, propWriteStream)) {
		SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to save inbox items query for player with name {}", player->getName());
		return false;
	}

	// Save prey class
	if (g_configManager().getBoolean(PREY_ENABLED)) {
		query.str(std::string());
		query << "DELETE FROM `player_prey` WHERE `player_id` = " << player->getGUID();
		if (!db.executeQuery(query.str())) {
			SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to execute prey query for player with name {}", player->getName());
			return false;
		}

		for (uint8_t slotId = PreySlot_First; slotId <= PreySlot_Last; slotId++) {
			PreySlot* slot = player->getPreySlotById(static_cast<PreySlot_t>(slotId));
			if (slot) {
				query.str(std::string());
				query << "INSERT INTO `player_prey` (`player_id`, `slot`, `state`, `raceid`, `option`, `bonus_type`, `bonus_rarity`, `bonus_percentage`, `bonus_time`, `free_reroll`, `monster_list`) VALUES (";
				query << player->getGUID() << ", ";
				query << static_cast<uint16_t>(slot->id) << ", ";
				query << static_cast<uint16_t>(slot->state) << ", ";
				query << slot->selectedRaceId << ", ";
				query << static_cast<uint16_t>(slot->option) << ", ";
				query << static_cast<uint16_t>(slot->bonus) << ", ";
				query << static_cast<uint16_t>(slot->bonusRarity) << ", ";
				query << slot->bonusPercentage << ", ";
				query << slot->bonusTimeLeft << ", ";
				query << slot->freeRerollTimeStamp << ", ";

				PropWriteStream propPreyStream;
				std::ranges::for_each(slot->raceIdList.begin(), slot->raceIdList.end(), [&propPreyStream](uint16_t raceId)
				{
					propPreyStream.write<uint16_t>(raceId);
				});

				size_t preySize;
				const char* preyList = propPreyStream.getStream(preySize);
				query << db.escapeBlob(preyList, static_cast<uint32_t>(preySize)) << ")";

				if (!db.executeQuery(query.str())) {
					SPDLOG_WARN("[IOLoginData::savePlayer] - Error saving prey slot data from player: {}", player->getName());
					return false;
				}
			}
		}
	}

	// Save task hunting class
	if (g_configManager().getBoolean(TASK_HUNTING_ENABLED)) {
		query.str(std::string());
		query << "DELETE FROM `player_taskhunt` WHERE `player_id` = " << player->getGUID();
		if (!db.executeQuery(query.str())) {
			SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to execute player taskhunt database query for player with name {}", player->getName());
			return false;
		}

		for (uint8_t slotId = PreySlot_First; slotId <= PreySlot_Last; slotId++) {
			TaskHuntingSlot* slot = player->getTaskHuntingSlotById(static_cast<PreySlot_t>(slotId));
			if (slot) {
				query.str(std::string());
				query << "INSERT INTO `player_taskhunt` (`player_id`, `slot`, `state`, `raceid`, `upgrade`, `rarity`, `kills`, `disabled_time`, `free_reroll`, `monster_list`) VALUES (";
				query << player->getGUID() << ", ";
				query << static_cast<uint16_t>(slot->id) << ", ";
				query << static_cast<uint16_t>(slot->state) << ", ";
				query << slot->selectedRaceId << ", ";
				query << (slot->upgrade ? 1 : 0) << ", ";
				query << static_cast<uint16_t>(slot->rarity) << ", ";
				query << slot->currentKills << ", ";
				query << slot->disabledUntilTimeStamp << ", ";
				query << slot->freeRerollTimeStamp << ", ";

				PropWriteStream propTaskHuntingStream;
				std::ranges::for_each(slot->raceIdList.begin(), slot->raceIdList.end(), [&propTaskHuntingStream](uint16_t raceId)
				{
					propTaskHuntingStream.write<uint16_t>(raceId);
				});

				size_t taskHuntingSize;
				const char* taskHuntingList = propTaskHuntingStream.getStream(taskHuntingSize);
				query << db.escapeBlob(taskHuntingList, static_cast<uint32_t>(taskHuntingSize)) << ")";

				if (!db.executeQuery(query.str())) {
					SPDLOG_WARN("[IOLoginData::savePlayer] - Error saving task hunting slot data from player: {}", player->getName());
					return false;
				}
			}
		}
	}

	query.str(std::string());
	query << "DELETE FROM `player_storage` WHERE `player_id` = " << player->getGUID();
	if (!db.executeQuery(query.str())) {
		SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to execute database player storage query for player with name {}", player->getName());
		return false;
	}

	query.str(std::string());

	DBInsert storageQuery("INSERT INTO `player_storage` (`player_id`, `key`, `value`) VALUES ");
	player->genReservedStorageRange();

	for (const auto& [storageIdPair, storageIdValue] : player->storageMap) {
		query << player->getGUID() << ',' << storageIdPair << ',' << storageIdValue;
		if (!storageQuery.addRow(query)) {
			SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to add storage row for player with name {}", player->getName());
			return false;
		}
	}

	if (!storageQuery.execute()) {
		SPDLOG_ERROR("[IOLoginData::savePlayer] - Failed to execute storage query for player with name {}", player->getName());
		return false;
	}

	//End the transaction
	return transaction.commit();
}

std::string IOLoginData::getNameByGuid(uint32_t guid)
{
	std::ostringstream query;
	query << "SELECT `name` FROM `players` WHERE `id` = " << guid;
	DBResult_ptr result = Database::getInstance().storeQuery(query.str());
	if (!result) {
		return std::string();
	}
	return result->getString("name");
}

uint32_t IOLoginData::getGuidByName(const std::string& name)
{
	Database& db = Database::getInstance();

	std::ostringstream query;
	query << "SELECT `id` FROM `players` WHERE `name` = " << db.escapeString(name);
	DBResult_ptr result = db.storeQuery(query.str());
	if (!result) {
		return 0;
	}
	return result->getU32("id");
}

bool IOLoginData::getGuidByNameEx(uint32_t& guid, bool& specialVip, std::string& name)
{
	Database& db = Database::getInstance();

	std::ostringstream query;
	query << "SELECT `name`, `id`, `group_id`, `account_id` FROM `players` WHERE `name` = " << db.escapeString(name);
	DBResult_ptr result = db.storeQuery(query.str());
	if (!result) {
		return false;
	}

	name = result->getString("name");
	guid = result->getU32("id");
	const Group* group = g_game().groups.getGroup(result->getU16("group_id"));

	uint64_t flags;
	if (group) {
		flags = group->flags;
	} else {
		flags = 0;
	}

	specialVip = (flags & PlayerFlag_SpecialVIP) != 0;
	return true;
}

bool IOLoginData::formatPlayerName(std::string& name)
{
	Database& db = Database::getInstance();

	std::ostringstream query;
	query << "SELECT `name` FROM `players` WHERE `name` = " << db.escapeString(name);

	DBResult_ptr result = db.storeQuery(query.str());
	if (!result) {
		return false;
	}

	name = result->getString("name");
	return true;
}

void IOLoginData::loadItems(ItemMap& itemMap, DBResult_ptr result)
{
	do {
		uint32_t sid = result->getU32("sid");
		uint32_t pid = result->getU32("pid");
		uint16_t type = result->getU16("itemtype");
		uint16_t count = result->getU16("count");

		unsigned long attrSize;
		const char* attr = result->getStream("attributes", attrSize);

		PropStream propStream;
		propStream.init(attr, attrSize);

		Item* item = Item::CreateItem(type, count);
		if (item) {
			if (!item->unserializeAttr(propStream)) {
				SPDLOG_WARN("[IOLoginData::loadItems] - Failed to serialize");
			}

			std::pair<Item*, uint32_t> pair(item, pid);
			itemMap[sid] = pair;
		}
	} while (result->next());
}

void IOLoginData::increaseBankBalance(uint32_t guid, uint64_t bankBalance)
{
	std::ostringstream query;
	query << "UPDATE `players` SET `balance` = `balance` + " << bankBalance << " WHERE `id` = " << guid;
	Database::getInstance().executeQuery(query.str());
}

bool IOLoginData::hasBiddedOnHouse(uint32_t guid)
{
	Database& db = Database::getInstance();

	std::ostringstream query;
	query << "SELECT `id` FROM `houses` WHERE `highest_bidder` = " << guid << " LIMIT 1";
	return db.storeQuery(query.str()).get() != nullptr;
}

std::forward_list<VIPEntry> IOLoginData::getVIPEntries(uint32_t accountId)
{
	std::forward_list<VIPEntry> entries;

	std::ostringstream query;
	query << "SELECT `player_id`, (SELECT `name` FROM `players` WHERE `id` = `player_id`) AS `name`, `description`, `icon`, `notify` FROM `account_viplist` WHERE `account_id` = " << accountId;

	DBResult_ptr result = Database::getInstance().storeQuery(query.str());
	if (result) {
		do {
			entries.emplace_front(
				result->getU32("player_id"),
				result->getString("name"),
				result->getString("description"),
				result->getU32("icon"),
				result->getU16("notify") != 0
			);
		} while (result->next());
	}
	return entries;
}

void IOLoginData::addVIPEntry(uint32_t accountId, uint32_t guid, const std::string& description, uint32_t icon, bool notify)
{
	Database& db = Database::getInstance();

	std::ostringstream query;
	query << "INSERT INTO `account_viplist` (`account_id`, `player_id`, `description`, `icon`, `notify`) VALUES (" << accountId << ',' << guid << ',' << db.escapeString(description) << ',' << icon << ',' << notify << ')';
	db.executeQuery(query.str());
}

void IOLoginData::editVIPEntry(uint32_t accountId, uint32_t guid, const std::string& description, uint32_t icon, bool notify)
{
	Database& db = Database::getInstance();

	std::ostringstream query;
	query << "UPDATE `account_viplist` SET `description` = " << db.escapeString(description) << ", `icon` = " << icon << ", `notify` = " << notify << " WHERE `account_id` = " << accountId << " AND `player_id` = " << guid;
	db.executeQuery(query.str());
}

void IOLoginData::removeVIPEntry(uint32_t accountId, uint32_t guid)
{
	std::ostringstream query;
	query << "DELETE FROM `account_viplist` WHERE `account_id` = " << accountId << " AND `player_id` = " << guid;
	Database::getInstance().executeQuery(query.str());
}

void IOLoginData::addPremiumDays(uint32_t accountId, int32_t addDays)
{
	std::ostringstream query;
	query << "UPDATE `accounts` SET `premdays` = `premdays` + " << addDays << " WHERE `id` = " << accountId;
	Database::getInstance().executeQuery(query.str());
}

void IOLoginData::removePremiumDays(uint32_t accountId, int32_t removeDays)
{
	std::ostringstream query;
	query << "UPDATE `accounts` SET `premdays` = `premdays` - " << removeDays << " WHERE `id` = " << accountId;
	Database::getInstance().executeQuery(query.str());
}
