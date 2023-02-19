/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-2022 OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.org/
 */

#ifndef SRC_LUA_FUNCTIONS_CORE_NETWORK_CORE_NETWORK_FUNCTIONS_HPP_
#define SRC_LUA_FUNCTIONS_CORE_NETWORK_CORE_NETWORK_FUNCTIONS_HPP_

#include "lua/scripts/luascript.h"
#include "lua/functions/core/network/network_message_functions.hpp"
#include "lua/functions/core/network/webhook_functions.hpp"

class CoreNetworkFunctions final : LuaScriptInterface {
	public:
		static void init(lua_State* L) {
			NetworkMessageFunctions::init(L);
			WebhookFunctions::init(L);
		}

	private:
};

#endif // SRC_LUA_FUNCTIONS_CORE_NETWORK_CORE_NETWORK_FUNCTIONS_HPP_
