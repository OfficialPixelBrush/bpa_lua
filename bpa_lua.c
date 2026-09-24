#define ADDON_API_IMPLEMENTATION
#include "addon_api.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#define BPA_LUA_SCRIPTS_DIR "lua_scripts"

static const bp_api* g_api = NULL;
static bp_world* g_world = NULL; // Best-effort fallback world, kept fresh by any event/call that hands us one.

// Helpers
static char* bpa_strdup(const char* s) {
    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

static void* bpa_checkhandle(lua_State* L, int idx, const char* what) {
    luaL_argcheck(L, lua_islightuserdata(L, idx), idx, what);
    return lua_touserdata(L, idx);
}

static bool bpa_lua_getglobal_fn(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

static bool bpa_lua_pcall_report(lua_State* L, int nargs, int nresults) {
    if (lua_pcall(L, nargs, nresults, 0) != LUA_OK) {
        g_api->log.error(g_api, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

// log.*
static int lua_log_info(lua_State* L) {
    g_api->log.info(g_api, luaL_checkstring(L, 1));
    return 0;
}
static int lua_log_warning(lua_State* L) {
    g_api->log.warning(g_api, luaL_checkstring(L, 1));
    return 0;
}
static int lua_log_error(lua_State* L) {
    g_api->log.error(g_api, luaL_checkstring(L, 1));
    return 0;
}
static const luaL_Reg lua_log_fns[] = {
    {"info", lua_log_info},
    {"warning", lua_log_warning},
    {"error", lua_log_error},
    {NULL, NULL}
};

// server.*
static int lua_server_getPlayerCount(lua_State* L) {
    lua_pushinteger(L, g_api->server.getPlayerCount(g_api));
    return 1;
}
static int lua_server_getPlayerAt(lua_State* L) {
    int index = (int)luaL_checkinteger(L, 1);
    bp_player* player = g_api->server.getPlayerAt(g_api, index);
    if (!player) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, player);
    return 1;
}
static const luaL_Reg lua_server_fns[] = {
    {"getPlayerCount", lua_server_getPlayerCount},
    {"getPlayerAt", lua_server_getPlayerAt},
    {NULL, NULL}
};

// player.*
static int lua_player_sendMessage(lua_State* L) {
    bp_player* player = bpa_checkhandle(L, 1, "expected a player handle");
    const char* message = luaL_checkstring(L, 2);
    g_api->player.sendMessage(player, message);
    return 0;
}
static int lua_player_kick(lua_State* L) {
    bp_player* player = bpa_checkhandle(L, 1, "expected a player handle");
    g_api->player.kick(player);
    return 0;
}
static int lua_player_getUsername(lua_State* L) {
    bp_player* player = bpa_checkhandle(L, 1, "expected a player handle");
    lua_pushstring(L, g_api->player.getUsername(player));
    return 1;
}
static int lua_player_getEntity(lua_State* L) {
    bp_player* player = bpa_checkhandle(L, 1, "expected a player handle");
    bp_entity* entity = g_api->player.getEntity(player);
    if (!entity) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, entity);
    return 1;
}
static const luaL_Reg lua_player_fns[] = {
    {"sendMessage", lua_player_sendMessage},
    {"kick", lua_player_kick},
    {"getUsername", lua_player_getUsername},
    {"getEntity", lua_player_getEntity},
    {NULL, NULL}
};

// entity.*
static int lua_entity_getPosition(lua_State* L) {
    bp_entity* entity = bpa_checkhandle(L, 1, "expected an entity handle");
    bp_vec3 pos = g_api->entity.getPosition(entity);
    lua_newtable(L);
    lua_pushnumber(L, pos.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, pos.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, pos.z); lua_setfield(L, -2, "z");
    return 1;
}
static int lua_entity_setPosition(lua_State* L) {
    bp_entity* entity = bpa_checkhandle(L, 1, "expected an entity handle");
    bp_vec3 pos;
    pos.x = luaL_checknumber(L, 2);
    pos.y = luaL_checknumber(L, 3);
    pos.z = luaL_checknumber(L, 4);
    g_api->entity.setPosition(entity, pos);
    return 0;
}
static int lua_entity_getWorld(lua_State* L) {
    bp_entity* entity = bpa_checkhandle(L, 1, "expected an entity handle");
    bp_world* world = g_api->entity.getWorld(entity);
    g_world = world; // keep the fallback fresh
    if (!world) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, world);
    return 1;
}
static const luaL_Reg lua_entity_fns[] = {
    {"getPosition", lua_entity_getPosition},
    {"setPosition", lua_entity_setPosition},
    {"getWorld", lua_entity_getWorld},
    {NULL, NULL}
};

// world.*
static int lua_world_getBlock(lua_State* L) {
    bp_world* world = bpa_checkhandle(L, 1, "expected a world handle");
    bp_block_pos bpos;
    bpos.x = (int32_t)luaL_checkinteger(L, 2);
    bpos.y = (int32_t)luaL_checkinteger(L, 3);
    bpos.z = (int32_t)luaL_checkinteger(L, 4);

    bp_block block = g_api->world.getBlock(world, bpos);

    lua_newtable(L);
    lua_pushinteger(L, block.id); lua_setfield(L, -2, "id");
    lua_pushinteger(L, block.meta); lua_setfield(L, -2, "meta");
    return 1;
}
static int lua_world_setBlock(lua_State* L) {
    bp_world* world = bpa_checkhandle(L, 1, "expected a world handle");
    bp_block_pos bpos;
    bpos.x = (int32_t)luaL_checkinteger(L, 2);
    bpos.y = (int32_t)luaL_checkinteger(L, 3);
    bpos.z = (int32_t)luaL_checkinteger(L, 4);

    bp_block block;
    block.id = (int8_t)luaL_checkinteger(L, 5);
    block.meta = (uint8_t)luaL_checkinteger(L, 6);

    g_api->world.setBlock(world, bpos, block);
    return 0;
}
static int lua_world_sendBlockUpdate(lua_State* L) {
    // Same signature as setBlock, but only resends the block to clients
    // without changing world state server-side.
    bp_world* world = bpa_checkhandle(L, 1, "expected a world handle");
    bp_block_pos bpos;
    bpos.x = (int32_t)luaL_checkinteger(L, 2);
    bpos.y = (int32_t)luaL_checkinteger(L, 3);
    bpos.z = (int32_t)luaL_checkinteger(L, 4);

    bp_block block;
    block.id = (int8_t)luaL_checkinteger(L, 5);
    block.meta = (uint8_t)luaL_checkinteger(L, 6);

    g_api->world.sendBlockUpdate(world, bpos, block);
    return 0;
}
static const luaL_Reg lua_world_fns[] = {
    {"getBlock", lua_world_getBlock},
    {"setBlock", lua_world_setBlock},
    {"sendBlockUpdate", lua_world_sendBlockUpdate},
    {NULL, NULL}
};

// data.*
static int lua_data_setPlayer(lua_State* L) {
    bp_player* player = bpa_checkhandle(L, 1, "expected a player handle");
    luaL_checkany(L, 2);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    g_api->data.setPlayer(g_api, player, (void*)(intptr_t)ref);
    return 0;
}
static int lua_data_getPlayer(lua_State* L) {
    bp_player* player = bpa_checkhandle(L, 1, "expected a player handle");
    void* raw = g_api->data.getPlayer(g_api, player);
    if (!raw) { lua_pushnil(L); return 1; }
    lua_rawgeti(L, LUA_REGISTRYINDEX, (lua_Integer)(intptr_t)raw);
    return 1;
}
static const luaL_Reg lua_data_fns[] = {
    {"setPlayer", lua_data_setPlayer},
    {"getPlayer", lua_data_getPlayer},
    {NULL, NULL}
};

// Wires every namespace above into a freshly-created lua_State.
static void bpa_register_api(lua_State* L) {
    luaL_newlib(L, lua_log_fns);    lua_setglobal(L, "log");
    luaL_newlib(L, lua_server_fns); lua_setglobal(L, "server");
    luaL_newlib(L, lua_player_fns); lua_setglobal(L, "player");
    luaL_newlib(L, lua_entity_fns); lua_setglobal(L, "entity");
    luaL_newlib(L, lua_world_fns);  lua_setglobal(L, "world");
    luaL_newlib(L, lua_data_fns);   lua_setglobal(L, "data");
}

// --- Multi-plugin management ---
typedef struct {
    lua_State* L;
    char* name;
} bpa_lua_plugin;

static bpa_lua_plugin* g_plugins = NULL;
static size_t g_plugin_count = 0;
static size_t g_plugin_capacity = 0;

static void bpa_plugins_add(lua_State* L, const char* name) {
    if (g_plugin_count == g_plugin_capacity) {
        size_t new_cap = g_plugin_capacity ? g_plugin_capacity * 2 : 4;
        bpa_lua_plugin* grown = realloc(g_plugins, new_cap * sizeof(*grown));
        if (!grown) {
            g_api->log.error(g_api, "bpa-lua: out of memory growing plugin list");
            lua_close(L);
            return;
        }
        g_plugins = grown;
        g_plugin_capacity = new_cap;
    }
    g_plugins[g_plugin_count].L = L;
    g_plugins[g_plugin_count].name = bpa_strdup(name);
    g_plugin_count++;
}

static void bpa_plugins_clear(void) {
    for (size_t i = 0; i < g_plugin_count; i++) {
        lua_State* L = g_plugins[i].L;
        if (bpa_lua_getglobal_fn(L, "OnUnload")) {
            bpa_lua_pcall_report(L, 0, 0);
        }
        lua_close(L);
        free(g_plugins[i].name);
    }
    free(g_plugins);
    g_plugins = NULL;
    g_plugin_count = 0;
    g_plugin_capacity = 0;
}

static void bpa_load_plugin_file(const char* path, const char* name) {
    lua_State* L = luaL_newstate();
    if (!L) {
        g_api->log.error(g_api, "bpa-lua: failed to create Lua state");
        return;
    }
    luaL_openlibs(L);
    bpa_register_api(L);

    if (luaL_dofile(L, path) != LUA_OK) {
        g_api->log.error(g_api, lua_tostring(L, -1));
        lua_close(L);
        return;
    }

    bpa_plugins_add(L, name);

    char msg[256];
    snprintf(msg, sizeof(msg), "bpa-lua: loaded plugin '%s'", name);
    g_api->log.info(g_api, msg);
}

static void bpa_load_all_plugins(void) {
    DIR* dir = opendir(BPA_LUA_SCRIPTS_DIR);
    if (!dir) {
        g_api->log.warning(g_api, "bpa-lua: no '" BPA_LUA_SCRIPTS_DIR "' directory found, no plugins loaded");
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".lua") != 0) {
            continue;
        }

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", BPA_LUA_SCRIPTS_DIR, entry->d_name);
        bpa_load_plugin_file(path, entry->d_name);
    }

    closedir(dir);
}

// --- Engine events that're sent to the scripts ---

void OnPlayerJoin(const bp_api* api, const bp_player_join_event* ev) {
    (void)api;
    for (size_t i = 0; i < g_plugin_count; i++) {
        lua_State* L = g_plugins[i].L;
        if (!bpa_lua_getglobal_fn(L, "OnPlayerJoin")) continue;

        lua_pushlightuserdata(L, ev->player);
        bpa_lua_pcall_report(L, 1, 0);
    }
}

void OnBlockUse(const bp_api* api, bp_block_use_event* ev) {
    (void)api;
    g_world = ev->world; // fallback world context, kept fresh here too

    for (size_t i = 0; i < g_plugin_count; i++) {
        lua_State* L = g_plugins[i].L;
        if (!bpa_lua_getglobal_fn(L, "OnBlockUse")) continue;

        lua_pushlightuserdata(L, ev->player);
        lua_pushlightuserdata(L, ev->world);
        lua_pushinteger(L, ev->blockPos.x);
        lua_pushinteger(L, ev->blockPos.y);
        lua_pushinteger(L, ev->blockPos.z);

        if (bpa_lua_pcall_report(L, 5, 1)) {
            // A plugin can cancel the block-use by returning `false`.
            if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
                ev->cancel = true;
            }
            lua_pop(L, 1);
        }
    }
}

void OnPlayerChat(const bp_api* api, bp_player_chat_event* ev) {
    (void)api;
    for (size_t i = 0; i < g_plugin_count; i++) {
        lua_State* L = g_plugins[i].L;
        if (!bpa_lua_getglobal_fn(L, "OnPlayerChat")) continue;

        lua_pushlightuserdata(L, ev->player);
        lua_pushstring(L, ev->message);

        if (bpa_lua_pcall_report(L, 2, 1)) {
            // A plugin can cancel the chat message by returning `false`.
            if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
                ev->cancel = true;
            }
            lua_pop(L, 1);
        }
    }
}

void OnLoad(const bp_api* api, const bp_addon_load* ev) {
    (void)ev;
    g_api = api;

    bpa_load_all_plugins();

    char msg[64];
    snprintf(msg, sizeof(msg), "Lua Initialized! (%zu plugin(s) loaded)", g_plugin_count);
    api->log.info(api, msg);
}

void OnUnload(const bp_api* api, const bp_addon_unload* ev) {
    (void)ev;
    bpa_plugins_clear();
    api->log.info(api, "Lua Uninitialized!");
}

bp_addon_info bp_addon(const bp_api* api) {
    return (bp_addon_info){
        .id = "bpa-lua",
        .name = "Lua",
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
