#define ADDON_API_IMPLEMENTATION
#include "addon_api.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <lua.h>

// TODO: Global vector of lua plugins

/*static void lua_player_sendMessage(bp_player* player, const char* message) {
    api->player.sendMessage(player, message);
}*/
static void lua_world_getBlock(lua_State *L) {
    bp_block_pos bpos;
	bpos.x = int32_t(lua_tointeger(L, 1));
	bpos.y = int32_t(lua_tointeger(L, 2));
	bpos.z = int32_t(lua_tointeger(L, 3));
    bp_block blockInfo = api->world.getBlock(world, pos);
	lua_newtable(L);
	lua_pushinteger(L, blockInfo.id);
	lua_rawseti(L, -2, 1);
	lua_pushinteger(L, blockInfo.meta);
	lua_rawseti(L, -2, 2);
}

void OnPlayerJoin(const bp_api* api, const bp_player_join_event* ev) {

}

void OnBlockUse(const bp_api* api, bp_block_use_event* ev) {

}

void OnPlayerChat(const bp_api* api, bp_player_chat_event* ev) {
    
}

void OnLoad(const bp_api* api, const bp_addon_load* ev) {
	lua_register(L, "getBlock", lua_world_getBlock);
	api->log.info(api, "Lua Integration Initialized!");
}

void OnUnload(const bp_api* api, const bp_addon_unload* ev) {

	api->log.info(api, "Lua Integration Uninitialized!");
}

bp_addon_info bp_addon(const bp_api* api) {
	return (bp_addon_info){
		.id = "bpa-lua",
		.name = "Lua Integration",
		.version = "1.0",
		.events = {
			.playerJoin = OnPlayerJoin,
			.playerChat = OnPlayerChat,
			.blockUse = OnBlockUse,
			.addonLoad = OnLoad,
			.addonUnload = OnUnload,
		},
	};
}