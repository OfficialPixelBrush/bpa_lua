extern "C" {
#include "addon_api.h"
}

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace bpa {
namespace {

constexpr const char* scriptsDir = "lua_scripts";

const bp_api* g_api = nullptr;
bp_world* g_world = nullptr; // Best-effort fallback world, kept fresh by any event/call that hands us one.

class LuaState {
public:
    LuaState() = default;
    explicit LuaState(lua_State* L) : L_(L) {}
    ~LuaState() { close(); }

    LuaState(const LuaState&) = delete;
    LuaState& operator=(const LuaState&) = delete;

    LuaState(LuaState&& other) noexcept : L_(other.L_) { other.L_ = nullptr; }
    LuaState& operator=(LuaState&& other) noexcept {
        if (this != &other) {
            close();
            L_ = other.L_;
            other.L_ = nullptr;
        }
        return *this;
    }

    lua_State* get() const { return L_; }
    explicit operator bool() const { return L_ != nullptr; }

private:
    void close() {
        if (L_) {
            lua_close(L_);
            L_ = nullptr;
        }
    }

    lua_State* L_ = nullptr;
};

struct Plugin {
    LuaState state;
    std::string name;
};

std::vector<Plugin> g_plugins;

void* checkHandle(lua_State* L, int idx, const char* what) {
    luaL_argcheck(L, lua_islightuserdata(L, idx), idx, what);
    return lua_touserdata(L, idx);
}

bool getGlobalFunction(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool callWithReport(lua_State* L, int nargs, int nresults) {
    if (lua_pcall(L, nargs, nresults, 0) != LUA_OK) {
        g_api->log.error(g_api, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

// Lua-callable bindings
extern "C" {

// log.*
int lua_log_info(lua_State* L) {
    g_api->log.info(g_api, luaL_checkstring(L, 1));
    return 0;
}
int lua_log_warning(lua_State* L) {
    g_api->log.warning(g_api, luaL_checkstring(L, 1));
    return 0;
}
int lua_log_error(lua_State* L) {
    g_api->log.error(g_api, luaL_checkstring(L, 1));
    return 0;
}

// server.*
int lua_server_getPlayerCount(lua_State* L) {
    lua_pushinteger(L, g_api->server.getPlayerCount(g_api));
    return 1;
}
int lua_server_getPlayerAt(lua_State* L) {
    int index = static_cast<int>(luaL_checkinteger(L, 1));
    bp_player* player = g_api->server.getPlayerAt(g_api, index);
    if (!player) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, player);
    return 1;
}

// player.*
int lua_player_sendMessage(lua_State* L) {
    bp_player* player = static_cast<bp_player*>(checkHandle(L, 1, "Expected a player handle"));
    const char* message = luaL_checkstring(L, 2);
    g_api->player.sendMessage(player, message);
    return 0;
}
int lua_player_kick(lua_State* L) {
    bp_player* player = static_cast<bp_player*>(checkHandle(L, 1, "Expected a player handle"));
    g_api->player.kick(player);
    return 0;
}
int lua_player_getUsername(lua_State* L) {
    bp_player* player = static_cast<bp_player*>(checkHandle(L, 1, "Expected a player handle"));
    lua_pushstring(L, g_api->player.getUsername(player));
    return 1;
}
int lua_player_getEntity(lua_State* L) {
    bp_player* player = static_cast<bp_player*>(checkHandle(L, 1, "Expected a player handle"));
    bp_entity* entity = g_api->player.getEntity(player);
    if (!entity) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, entity);
    return 1;
}

// entity.*
int lua_entity_getPosition(lua_State* L) {
    bp_entity* entity = static_cast<bp_entity*>(checkHandle(L, 1, "Expected an entity handle"));
    bp_vec3 pos = g_api->entity.getPosition(entity);
    lua_newtable(L);
    lua_pushnumber(L, pos.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, pos.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, pos.z); lua_setfield(L, -2, "z");
    return 1;
}
int lua_entity_setPosition(lua_State* L) {
    bp_entity* entity = static_cast<bp_entity*>(checkHandle(L, 1, "Expected an entity handle"));
    bp_vec3 pos;
    pos.x = luaL_checknumber(L, 2);
    pos.y = luaL_checknumber(L, 3);
    pos.z = luaL_checknumber(L, 4);
    g_api->entity.setPosition(entity, pos);
    return 0;
}
int lua_entity_getWorld(lua_State* L) {
    bp_entity* entity = static_cast<bp_entity*>(checkHandle(L, 1, "Expected an entity handle"));
    bp_world* world = g_api->entity.getWorld(entity);
    g_world = world; // keep the fallback fresh
    if (!world) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, world);
    return 1;
}

// world.*
int lua_world_getBlock(lua_State* L) {
    bp_world* world = static_cast<bp_world*>(checkHandle(L, 1, "Expected a world handle"));
    bp_block_pos bpos;
    bpos.x = static_cast<int32_t>(luaL_checkinteger(L, 2));
    bpos.y = static_cast<int32_t>(luaL_checkinteger(L, 3));
    bpos.z = static_cast<int32_t>(luaL_checkinteger(L, 4));

    bp_block block = g_api->world.getBlock(world, bpos);

    lua_newtable(L);
    lua_pushinteger(L, block.id); lua_setfield(L, -2, "id");
    lua_pushinteger(L, block.meta); lua_setfield(L, -2, "meta");
    return 1;
}
int lua_world_setBlock(lua_State* L) {
    bp_world* world = static_cast<bp_world*>(checkHandle(L, 1, "Expected a world handle"));
    bp_block_pos bpos;
    bpos.x = static_cast<int32_t>(luaL_checkinteger(L, 2));
    bpos.y = static_cast<int32_t>(luaL_checkinteger(L, 3));
    bpos.z = static_cast<int32_t>(luaL_checkinteger(L, 4));

    bp_block block;
    block.id = static_cast<int8_t>(luaL_checkinteger(L, 5));
    block.meta = static_cast<uint8_t>(luaL_checkinteger(L, 6));

    g_api->world.setBlock(world, bpos, block);
    return 0;
}
int lua_world_sendBlockUpdate(lua_State* L) {
    // Same signature as setBlock, but only resends the block to clients
    // without changing world state server-side.
    bp_world* world = static_cast<bp_world*>(checkHandle(L, 1, "Expected a world handle"));
    bp_block_pos bpos;
    bpos.x = static_cast<int32_t>(luaL_checkinteger(L, 2));
    bpos.y = static_cast<int32_t>(luaL_checkinteger(L, 3));
    bpos.z = static_cast<int32_t>(luaL_checkinteger(L, 4));

    bp_block block;
    block.id = static_cast<int8_t>(luaL_checkinteger(L, 5));
    block.meta = static_cast<uint8_t>(luaL_checkinteger(L, 6));

    g_api->world.sendBlockUpdate(world, bpos, block);
    return 0;
}

// data.*
int lua_data_setPlayer(lua_State* L) {
    bp_player* player = static_cast<bp_player*>(checkHandle(L, 1, "Expected a player handle"));
    luaL_checkany(L, 2);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    g_api->data.setPlayer(g_api, player, reinterpret_cast<void*>(static_cast<intptr_t>(ref)));
    return 0;
}
int lua_data_getPlayer(lua_State* L) {
    bp_player* player = static_cast<bp_player*>(checkHandle(L, 1, "Expected a player handle"));
    void* raw = g_api->data.getPlayer(g_api, player);
    if (!raw) { lua_pushnil(L); return 1; }
    lua_rawgeti(L, LUA_REGISTRYINDEX, static_cast<lua_Integer>(reinterpret_cast<intptr_t>(raw)));
    return 1;
}

} // extern "C"

const luaL_Reg lua_log_fns[] = {
    {"info", lua_log_info},
    {"warning", lua_log_warning},
    {"error", lua_log_error},
    {nullptr, nullptr}
};
const luaL_Reg lua_server_fns[] = {
    {"getPlayerCount", lua_server_getPlayerCount},
    {"getPlayerAt", lua_server_getPlayerAt},
    {nullptr, nullptr}
};
const luaL_Reg lua_player_fns[] = {
    {"sendMessage", lua_player_sendMessage},
    {"kick", lua_player_kick},
    {"getUsername", lua_player_getUsername},
    {"getEntity", lua_player_getEntity},
    {nullptr, nullptr}
};
const luaL_Reg lua_entity_fns[] = {
    {"getPosition", lua_entity_getPosition},
    {"setPosition", lua_entity_setPosition},
    {"getWorld", lua_entity_getWorld},
    {nullptr, nullptr}
};
const luaL_Reg lua_world_fns[] = {
    {"getBlock", lua_world_getBlock},
    {"setBlock", lua_world_setBlock},
    {"sendBlockUpdate", lua_world_sendBlockUpdate},
    {nullptr, nullptr}
};
const luaL_Reg lua_data_fns[] = {
    {"setPlayer", lua_data_setPlayer},
    {"getPlayer", lua_data_getPlayer},
    {nullptr, nullptr}
};

void registerApi(lua_State* L) {
    luaL_newlib(L, lua_log_fns);    lua_setglobal(L, "log");
    luaL_newlib(L, lua_server_fns); lua_setglobal(L, "server");
    luaL_newlib(L, lua_player_fns); lua_setglobal(L, "player");
    luaL_newlib(L, lua_entity_fns); lua_setglobal(L, "entity");
    luaL_newlib(L, lua_world_fns);  lua_setglobal(L, "world");
    luaL_newlib(L, lua_data_fns);   lua_setglobal(L, "data");
}

void loadPluginFile(const std::filesystem::path& path) {
    lua_State* raw = luaL_newstate();
    if (!raw) {
        g_api->log.error(g_api, "Failed to create Lua state");
        return;
    }
    LuaState state(raw);

    luaL_openlibs(state.get());
    registerApi(state.get());

    if (luaL_dofile(state.get(), path.string().c_str()) != LUA_OK) {
        g_api->log.error(g_api, lua_tostring(state.get(), -1));
        return; // `state` destructor closes the Lua state
    }

    std::string name = path.filename().string();
    std::string msg = "Loaded plugin '" + name + "'";
    g_api->log.info(g_api, msg.c_str());

    g_plugins.push_back(Plugin{std::move(state), std::move(name)});
}

void loadAllPlugins() {
    std::error_code ec;
    if (!std::filesystem::is_directory(scriptsDir, ec)) {
        g_api->log.warning(g_api, "No 'lua_scripts' directory found, no plugins loaded");
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(scriptsDir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".lua") continue;
        loadPluginFile(entry.path());
    }
}

void unloadAllPlugins() {
    for (auto& plugin : g_plugins) {
        if (getGlobalFunction(plugin.state.get(), "OnUnload")) {
            callWithReport(plugin.state.get(), 0, 0);
        }
    }
    g_plugins.clear(); // each Plugin's LuaState destructor closes its lua_State
}

// Engine events that're sent to the scripts
extern "C" {

void OnPlayerJoin(const bp_api* /*api*/, const bp_player_join_event* ev) {
    for (auto& plugin : g_plugins) {
        lua_State* L = plugin.state.get();
        if (!getGlobalFunction(L, "OnPlayerJoin")) continue;

        lua_pushlightuserdata(L, ev->player);
        callWithReport(L, 1, 0);
    }
}

void OnBlockUse(const bp_api* /*api*/, bp_block_use_event* ev) {
    g_world = ev->world; // fallback world context, kept fresh here too

    for (auto& plugin : g_plugins) {
        lua_State* L = plugin.state.get();
        if (!getGlobalFunction(L, "OnBlockUse")) continue;

        lua_pushlightuserdata(L, ev->player);
        lua_pushlightuserdata(L, ev->world);
        lua_pushinteger(L, ev->blockPos.x);
        lua_pushinteger(L, ev->blockPos.y);
        lua_pushinteger(L, ev->blockPos.z);

        if (callWithReport(L, 5, 1)) {
            // A plugin can cancel the block-use by returning `false`.
            if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
                ev->cancel = true;
            }
            lua_pop(L, 1);
        }
    }
}

void OnPlayerChat(const bp_api* /*api*/, bp_player_chat_event* ev) {
    for (auto& plugin : g_plugins) {
        lua_State* L = plugin.state.get();
        if (!getGlobalFunction(L, "OnPlayerChat")) continue;

        lua_pushlightuserdata(L, ev->player);
        lua_pushstring(L, ev->message);

        if (callWithReport(L, 2, 1)) {
            // A plugin can cancel the chat message by returning `false`.
            if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
                ev->cancel = true;
            }
            lua_pop(L, 1);
        }
    }
}

void OnLoad(const bp_api* api, const bp_addon_load* /*ev*/) {
    g_api = api;

    loadAllPlugins();

    std::string msg = "Lua initialized! (" +
        std::to_string(g_plugins.size()) + " plugin(s) loaded)";
    api->log.info(api, msg.c_str());
}

void OnUnload(const bp_api* api, const bp_addon_unload* /*ev*/) {
    unloadAllPlugins();
    api->log.info(api, "Lua uninitialized!");
}

} // extern "C"

} // namespace
} // namespace bpa

extern "C" bp_addon_info bp_addon(const bp_api* /*api*/) {
    return bp_addon_info{
        "bpa_lua",
        "Lua",
        "1.0",
        bp_addon_events{
            /* playerJoin   */ bpa::OnPlayerJoin,
            /* playerLeave  */ nullptr,
            /* playerChat   */ bpa::OnPlayerChat,
            /* playerMove   */ nullptr,
            /* itemUse      */ nullptr,
            /* blockBreak   */ nullptr,
            /* blockPlace   */ nullptr,
            /* blockUse     */ bpa::OnBlockUse,
            /* entityDamage */ nullptr,
            /* serverTick   */ nullptr,
            /* addonLoad    */ bpa::OnLoad,
            /* addonUnload  */ bpa::OnUnload,
        },
    };
}
