extern "C" {
#include "addon_api.h"
}

// Stock Lua's headers have no C++ guards, so they need the extern "C" wrapper.
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bpa {
namespace {

constexpr const char* ADDON_ID = "bpa_lua";
constexpr const char* ADDON_NAME = "Lua";
constexpr const char* ADDON_VERSION = "0.2.2"; // keep in sync with the release tag (checked by CI)

constexpr const char* scriptsDir = "lua";

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

// Each plugin owns an isolated Lua VM; scripts run directly on its main thread.
struct Plugin {
    LuaState state;
    std::string name;
};

std::vector<Plugin> g_plugins;

void* checkHandle(lua_State* L, int idx, const char* what) {
    luaL_argcheck(L, lua_islightuserdata(L, idx), idx, what);
    return lua_touserdata(L, idx);
}

// Error values aren't always strings (error({}) etc.), and lua_tostring would
// hand a null pointer to the logger.
const char* errorMessage(lua_State* L) {
    const char* msg = lua_tostring(L, -1);
    return msg ? msg : "(non-string error object)";
}

// Accepts any number and truncates it, so scripts can pass float coordinates
// straight through (luaL_checkinteger would raise on a fractional value).
int32_t checkInt32(lua_State* L, int idx) {
    lua_Number n = luaL_checknumber(L, idx);
    luaL_argcheck(L, n >= -2147483648.0 && n <= 2147483647.0, idx, "number out of range");
    return static_cast<int32_t>(n);
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
        g_api->log.error(g_api, errorMessage(L));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

void applyCancelReturn(lua_State* L, bool& cancel) {
    if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
        cancel = true;
    }
    lua_pop(L, 1);
}

void pushVec3(lua_State* L, bp_vec3 v) {
    lua_newtable(L);
    lua_pushnumber(L, v.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.z); lua_setfield(L, -2, "z");
}

void pushBlock(lua_State* L, bp_block block) {
    lua_newtable(L);
    lua_pushinteger(L, block.id);   lua_setfield(L, -2, "id");
    lua_pushinteger(L, block.meta); lua_setfield(L, -2, "meta");
}

void pushItemStack(lua_State* L, bp_item_stack item) {
    lua_newtable(L);
    lua_pushinteger(L, item.id);    lua_setfield(L, -2, "id");
    lua_pushinteger(L, item.count); lua_setfield(L, -2, "count");
    lua_pushinteger(L, item.data);  lua_setfield(L, -2, "data");
}


// Applies the return value of OnEntityDamage: `false` cancels the damage,
// a number replaces the damage amount, anything else leaves the event alone.
void applyDamageReturn(lua_State* L, bp_entity_damage_event* ev) {
    if (lua_isboolean(L, -1)) {
        if (!lua_toboolean(L, -1)) ev->cancel = true;
    } else if (lua_type(L, -1) == LUA_TNUMBER) {
        lua_Number n = lua_tonumber(L, -1);
        if (n >= -2147483648.0 && n <= 2147483647.0) ev->amount = static_cast<int>(n);
    }
    lua_pop(L, 1);
}

lua_State* mainThread(lua_State* L) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    lua_State* main = lua_tothread(L, -1);
    lua_pop(L, 1);
    return main;
}

// --- Per-player script data --------------------------------------------------
// API v4 removed bp_api::data, so `data.setPlayer/getPlayer` is now implemented
// here: one registry table per Lua VM, keyed by the player handle. Entries are
// dropped when the player leaves.
constexpr const char* PLAYER_DATA_KEY = "bpa.playerdata";

void pushPlayerDataTable(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, PLAYER_DATA_KEY);
    if (lua_istable(L, -1)) return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, PLAYER_DATA_KEY);
}

void clearPlayerData(lua_State* L, bp_player* player) {
    pushPlayerDataTable(L);
    lua_pushlightuserdata(L, player);
    lua_pushnil(L);
    lua_settable(L, -3);
    lua_pop(L, 1);
}

// --- Command support ---------------------------------------------------------


// The host may keep the `const char*` names and enum value arrays we hand it
// without copying, so everything passed to the cmd API lives for the whole
// process lifetime (deques never invalidate references on push_back).
std::deque<std::string> g_strings;
std::deque<std::vector<const char*>> g_enumValues;

const char* intern(const char* s) {
    g_strings.emplace_back(s);
    return g_strings.back().c_str();
}

// Name + null-terminated value array of an enum argument visible to an action.
using EnumScope = std::vector<std::pair<const char*, const std::vector<const char*>*>>;

struct Action {
    lua_State* L = nullptr;   // owning VM's main thread; null once detached
    int ref = LUA_NOREF;      // registry reference to the Lua callback
    EnumScope enums;          // filled in when the command tree is registered
};

// Never shrunk: the host may keep old nodes (and our userData pointers) alive
// after an unload, so Action records must outlive everything.
std::deque<Action> g_actions;
const Action* g_currentAction = nullptr; // the action being dispatched (actions can nest via commands.execute)

// Build-time bookkeeping for command nodes that haven't been registered yet.
struct NodeInfo {
    const char* name = nullptr;
    const std::vector<const char*>* enumValues = nullptr; // set for enum nodes only
    std::vector<bp_cmd_node*> children;
    Action* action = nullptr;
};
std::unordered_map<bp_cmd_node*, NodeInfo> g_nodes;

void detachActions(lua_State* L) {
    for (Action& a : g_actions) {
        if (a.L == L) {
            a.L = nullptr;
            a.ref = LUA_NOREF;
        }
    }
}

void resolveTree(bp_cmd_node* node, std::vector<bp_cmd_node*>& path, EnumScope& scope,
                 std::vector<bp_cmd_node*>& visited) {
    if (std::find(path.begin(), path.end(), node) != path.end()) return; // cycle guard
    auto it = g_nodes.find(node);
    if (it == g_nodes.end()) return;
    NodeInfo& info = it->second; // no inserts happen during the walk, so this stays valid

    visited.push_back(node);
    path.push_back(node);
    const size_t scopeSize = scope.size();
    if (info.enumValues) scope.emplace_back(info.name, info.enumValues);
    if (info.action) info.action->enums = scope;
    for (bp_cmd_node* child : info.children) resolveTree(child, path, scope, visited);
    scope.resize(scopeSize);
    path.pop_back();
}

// registerCommand consumes the nodes, so once a tree is registered we resolve
// its enum scopes and forget the nodes (their addresses may be reused later).
void finalizeCommandTree(bp_cmd_node* root) {
    std::vector<bp_cmd_node*> path, visited;
    EnumScope scope;
    resolveTree(root, path, scope, visited);
    for (bp_cmd_node* n : visited) g_nodes.erase(n);
}

const char* findEnumValue(const Action& action, const char* name, int index) {
    // Search from the innermost scope outwards so nearer arguments shadow farther ones.
    for (auto it = action.enums.rbegin(); it != action.enums.rend(); ++it) {
        if (std::strcmp(it->first, name) != 0) continue;
        const std::vector<const char*>& values = *it->second;
        const int count = static_cast<int>(values.size()) - 1; // minus the null terminator
        if (index >= 0 && index < count) return values[index];
        return nullptr;
    }
    return nullptr;
}

// Lua-callable bindings
extern "C" {

// Host -> Lua entry point for every command action.
void commandAction(const bp_api* /*api*/, const bp_cmd_action_ctx* ctx, void* userData) {
    Action* a = static_cast<Action*>(userData);
    if (!a || !a->L) return; // owning plugin has been unloaded

    const Action* previous = g_currentAction;
    g_currentAction = a;
    lua_rawgeti(a->L, LUA_REGISTRYINDEX, a->ref);
    lua_pushlightuserdata(a->L, const_cast<bp_cmd_action_ctx*>(ctx));
    callWithReport(a->L, 1, 0);
    g_currentAction = previous;
}

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
    int index = checkInt32(L, 1);
    bp_player* player = g_api->server.getPlayerAt(g_api, index);
    if (!player) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, player);
    return 1;
}
int lua_server_stop(lua_State* L) {
    // Optional timeout (the API takes a float); defaults to 0.
    float timeout = static_cast<float>(luaL_optnumber(L, 1, 0.0));
    g_api->server.stop(g_api, timeout);
    return 0;
}
int lua_server_getMsptAverage(lua_State* L) {
    lua_pushnumber(L, g_api->server.getMsptAverage(g_api));
    return 1;
}
int lua_server_getTpsAverage(lua_State* L) {
    lua_pushnumber(L, g_api->server.getTpsAverage(g_api));
    return 1;
}
int lua_server_getVersion(lua_State* L) {
    lua_pushstring(L, g_api->server.getVersion()); // NULL becomes nil
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
    const char* reason = luaL_optstring(L, 2, "Kicked");
    g_api->player.kick(g_api, player, reason);
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
    pushVec3(L, pos);
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

int lua_entity_toPlayer(lua_State* L) {
    bp_entity* entity = static_cast<bp_entity*>(checkHandle(L, 1, "Expected an entity handle"));
    bp_player* player = g_api->entity.toPlayer(entity);
    if (!player) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, player);
    return 1;
}

// world.*
int lua_world_getBlock(lua_State* L) {
    bp_world* world = static_cast<bp_world*>(checkHandle(L, 1, "Expected a world handle"));
    bp_block_pos bpos;
    bpos.x = checkInt32(L, 2);
    bpos.y = checkInt32(L, 3);
    bpos.z = checkInt32(L, 4);

    bp_block block = g_api->world.getBlock(world, bpos);
    pushBlock(L, block);
    return 1;
}
int lua_world_setBlock(lua_State* L) {
    bp_world* world = static_cast<bp_world*>(checkHandle(L, 1, "Expected a world handle"));
    bp_block_pos bpos;
    bpos.x = checkInt32(L, 2);
    bpos.y = checkInt32(L, 3);
    bpos.z = checkInt32(L, 4);

    bp_block block;
    block.id = static_cast<int8_t>(checkInt32(L, 5));
    block.meta = static_cast<uint8_t>(checkInt32(L, 6));

    g_api->world.setBlock(world, bpos, block);
    return 0;
}
int lua_world_sendBlockUpdate(lua_State* L) {
    // Same signature as setBlock, but only resends the block to clients
    // without changing world state server-side.
    bp_world* world = static_cast<bp_world*>(checkHandle(L, 1, "Expected a world handle"));
    bp_block_pos bpos;
    bpos.x = checkInt32(L, 2);
    bpos.y = checkInt32(L, 3);
    bpos.z = checkInt32(L, 4);

    bp_block block;
    block.id = static_cast<int8_t>(checkInt32(L, 5));
    block.meta = static_cast<uint8_t>(checkInt32(L, 6));

    g_api->world.sendBlockUpdate(world, bpos, block);
    return 0;
}
// Best-effort world handle for events that don't carry one directly
int lua_world_getCurrent(lua_State* L) {
    if (!g_world) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, g_world);
    return 1;
}

// data.* (implemented locally, see "Per-player script data" above)
int lua_data_setPlayer(lua_State* L) {
    bp_player* player = static_cast<bp_player*>(checkHandle(L, 1, "Expected a player handle"));
    luaL_checkany(L, 2);
    pushPlayerDataTable(L);
    lua_pushlightuserdata(L, player);
    lua_pushvalue(L, 2);
    lua_settable(L, -3);
    lua_pop(L, 1);
    return 0;
}
int lua_data_getPlayer(lua_State* L) {
    bp_player* player = static_cast<bp_player*>(checkHandle(L, 1, "Expected a player handle"));
    pushPlayerDataTable(L);
    lua_pushlightuserdata(L, player);
    lua_gettable(L, -2);
    lua_remove(L, -2);
    return 1;
}

// commands.*
int lua_commands_execute(lua_State* L) {
    // The sender is a player handle, or nil to run the command without a
    // player sender (e.g. as the server/console).
    bp_player* sender = nullptr;
    if (!lua_isnoneornil(L, 1)) {
        sender = static_cast<bp_player*>(checkHandle(L, 1, "Expected a player handle or nil"));
    }
    const char* command = luaL_checkstring(L, 2);
    lua_pushboolean(L, g_api->cmd.execute(g_api, sender, command));
    return 1;
}

// commands.unregister(name) -> true if anything was unregistered.
// (Addon commands are also removed automatically when the addon unloads.)
int lua_commands_unregister(lua_State* L) {
    lua_pushboolean(L, g_api->cmd.unregisterCommand(g_api, luaL_checkstring(L, 1)));
    return 1;
}

// Node constructors. Nodes are opaque handles; build a tree with addChild and
// hand the root to register.
int pushNode(lua_State* L, bp_cmd_node* node, const char* name,
             const std::vector<const char*>* enumValues = nullptr) {
    if (!node) { lua_pushnil(L); return 1; }
    NodeInfo info;
    info.name = name;
    info.enumValues = enumValues;
    g_nodes[node] = std::move(info); // overwrites any stale entry left at a reused address
    lua_pushlightuserdata(L, node);
    return 1;
}
int makeSimpleNode(lua_State* L, bp_cmd_node_type type) {
    const char* name = intern(luaL_checkstring(L, 1));
    return pushNode(L, g_api->cmd.makeNode(name, type), name);
}
int lua_commands_literal(lua_State* L)      { return makeSimpleNode(L, BP_CMD_LITERAL); }
int lua_commands_string(lua_State* L)       { return makeSimpleNode(L, BP_CMD_STRING); }
int lua_commands_greedyString(lua_State* L) { return makeSimpleNode(L, BP_CMD_GREEDY_STRING); }
int lua_commands_boolean(lua_State* L)      { return makeSimpleNode(L, BP_CMD_BOOLEAN); }
int lua_commands_vec3(lua_State* L)         { return makeSimpleNode(L, BP_CMD_VEC3); }

// integer(name) or integer(name, min, max)
int lua_commands_integer(lua_State* L) {
    const char* name = intern(luaL_checkstring(L, 1));
    if (lua_isnoneornil(L, 2) && lua_isnoneornil(L, 3)) {
        return pushNode(L, g_api->cmd.makeNode(name, BP_CMD_INTEGER), name);
    }
    int min = checkInt32(L, 2);
    int max = checkInt32(L, 3);
    return pushNode(L, g_api->cmd.makeIntegerEx(name, min, max), name);
}
// float(name) or float(name, min, max)
int lua_commands_float(lua_State* L) {
    const char* name = intern(luaL_checkstring(L, 1));
    if (lua_isnoneornil(L, 2) && lua_isnoneornil(L, 3)) {
        return pushNode(L, g_api->cmd.makeNode(name, BP_CMD_FLOAT), name);
    }
    float min = static_cast<float>(luaL_checknumber(L, 2));
    float max = static_cast<float>(luaL_checknumber(L, 3));
    return pushNode(L, g_api->cmd.makeFloatEx(name, min, max), name);
}
// enum(name, {"a", "b", "c"})
int lua_commands_enum(lua_State* L) {
    const char* name = intern(luaL_checkstring(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    const size_t n = lua_rawlen(L, 2);

    // Validate first: raising a Lua error while a std::vector is alive would skip its destructor.
    for (size_t i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, static_cast<int>(i));
        luaL_argcheck(L, lua_type(L, -1) == LUA_TSTRING, 2, "enum values must be strings");
        lua_pop(L, 1);
    }

    g_enumValues.emplace_back();
    std::vector<const char*>& values = g_enumValues.back();
    for (size_t i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, static_cast<int>(i));
        values.push_back(intern(lua_tostring(L, -1)));
        lua_pop(L, 1);
    }
    values.push_back(nullptr); // the API wants a null-terminated array
    return pushNode(L, g_api->cmd.makeEnum(name, values.data()), name, &values);
}

// addChild(parent, child) -> child, so calls can be nested/chained.
int lua_commands_addChild(lua_State* L) {
    bp_cmd_node* parent = static_cast<bp_cmd_node*>(checkHandle(L, 1, "Expected a command node"));
    bp_cmd_node* child = static_cast<bp_cmd_node*>(checkHandle(L, 2, "Expected a command node"));
    g_api->cmd.addChild(parent, child);
    g_nodes[parent].children.push_back(child);
    lua_pushvalue(L, 2);
    return 1;
}

// setAction(node, function(ctx) ... end)
// ctx is only valid for the duration of the callback.
int lua_commands_setAction(lua_State* L) {
    bp_cmd_node* node = static_cast<bp_cmd_node*>(checkHandle(L, 1, "Expected a command node"));
    luaL_checktype(L, 2, LUA_TFUNCTION);

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    NodeInfo& info = g_nodes[node];
    if (!info.action) {
        g_actions.emplace_back();
        info.action = &g_actions.back();
    } else if (info.action->L) {
        // Re-setting an action on the same node reuses its record.
        luaL_unref(info.action->L, LUA_REGISTRYINDEX, info.action->ref);
    }

    // Always call back on the VM's main thread, even if registered from a coroutine.
    info.action->L = mainThread(L);
    info.action->ref = ref;
    g_api->cmd.setAction(g_api, node, commandAction, info.action);
    return 0;
}

// setDescription(node, "text") -- shown in help/command listings.
int lua_commands_setDescription(lua_State* L) {
    bp_cmd_node* node = static_cast<bp_cmd_node*>(checkHandle(L, 1, "Expected a command node"));
    // The host may keep the pointer, so the string is interned for the process lifetime.
    const char* description = intern(luaL_checkstring(L, 2));
    g_api->cmd.setDescription(node, description);
    return 0;
}

// requireOperator(node) -- restricts the node to operators.
int lua_commands_requireOperator(lua_State* L) {
    bp_cmd_node* node = static_cast<bp_cmd_node*>(checkHandle(L, 1, "Expected a command node"));
    g_api->cmd.requireOperator(node);
    return 0;
}

int lua_commands_register(lua_State* L) {
    bp_cmd_node* root = static_cast<bp_cmd_node*>(checkHandle(L, 1, "Expected a command node"));
    finalizeCommandTree(root); // must run before the host consumes the nodes
    g_api->cmd.registerCommand(g_api, root);
    return 0;
}

// Action-context helpers (only valid inside an action callback)
int lua_commands_sendFeedback(lua_State* L) {
    auto* ctx = static_cast<const bp_cmd_action_ctx*>(checkHandle(L, 1, "Expected a command context"));
    g_api->cmd.sendActionFeedback(ctx, luaL_checkstring(L, 2));
    return 0;
}
// Returns an entity handle, or nil when there's no executing entity (console).
int lua_commands_getExecutor(lua_State* L) {
    auto* ctx = static_cast<const bp_cmd_action_ctx*>(checkHandle(L, 1, "Expected a command context"));
    bp_entity* entity = g_api->cmd.getActionExecutor(ctx);
    if (!entity) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, entity);
    return 1;
}

// Argument getters return the value, or nil if the argument isn't present.
int lua_commands_getStringArg(lua_State* L) {
    auto* ctx = static_cast<const bp_cmd_action_ctx*>(checkHandle(L, 1, "Expected a command context"));
    const char* out = nullptr;
    if (!g_api->cmd.getStringArg(ctx, luaL_checkstring(L, 2), &out) || !out) { lua_pushnil(L); return 1; }
    lua_pushstring(L, out);
    return 1;
}
int lua_commands_getIntegerArg(lua_State* L) {
    auto* ctx = static_cast<const bp_cmd_action_ctx*>(checkHandle(L, 1, "Expected a command context"));
    int out = 0;
    if (!g_api->cmd.getIntegerArg(ctx, luaL_checkstring(L, 2), &out)) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, out);
    return 1;
}
int lua_commands_getFloatArg(lua_State* L) {
    auto* ctx = static_cast<const bp_cmd_action_ctx*>(checkHandle(L, 1, "Expected a command context"));
    float out = 0.0f;
    if (!g_api->cmd.getFloatArg(ctx, luaL_checkstring(L, 2), &out)) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, out);
    return 1;
}
int lua_commands_getBooleanArg(lua_State* L) {
    auto* ctx = static_cast<const bp_cmd_action_ctx*>(checkHandle(L, 1, "Expected a command context"));
    bool out = false;
    if (!g_api->cmd.getBooleanArg(ctx, luaL_checkstring(L, 2), &out)) { lua_pushnil(L); return 1; }
    lua_pushboolean(L, out);
    return 1;
}
// The host reports an index into the enum's value array; we map it back to the
// string so scripts keep getting the value they declared.
int lua_commands_getEnumArg(lua_State* L) {
    auto* ctx = static_cast<const bp_cmd_action_ctx*>(checkHandle(L, 1, "Expected a command context"));
    const char* name = luaL_checkstring(L, 2);
    int index = 0;
    if (!g_api->cmd.getEnumArg(ctx, name, &index) || !g_currentAction) { lua_pushnil(L); return 1; }
    const char* value = findEnumValue(*g_currentAction, name, index);
    if (!value) { lua_pushnil(L); return 1; }
    lua_pushstring(L, value);
    return 1;
}
// Returns {x, y, z, relX, relY, relZ}; the rel* booleans say which axes were
// given as relative (~) coordinates.
int lua_commands_getVec3Arg(lua_State* L) {
    auto* ctx = static_cast<const bp_cmd_action_ctx*>(checkHandle(L, 1, "Expected a command context"));
    bp_cmd_vec3 out;
    std::memset(&out, 0, sizeof(out));
    if (!g_api->cmd.getVec3Arg(ctx, luaL_checkstring(L, 2), &out)) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushnumber(L, out.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, out.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, out.z); lua_setfield(L, -2, "z");
    lua_pushboolean(L, (out.relativeAxes >> BP_AXIS_X) & 1); lua_setfield(L, -2, "relX");
    lua_pushboolean(L, (out.relativeAxes >> BP_AXIS_Y) & 1); lua_setfield(L, -2, "relY");
    lua_pushboolean(L, (out.relativeAxes >> BP_AXIS_Z) & 1); lua_setfield(L, -2, "relZ");
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
    {"stop", lua_server_stop},
    {"getMsptAverage", lua_server_getMsptAverage},
    {"getTpsAverage", lua_server_getTpsAverage},
    {"getVersion", lua_server_getVersion},
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
    {"toPlayer", lua_entity_toPlayer},
    {nullptr, nullptr}
};
const luaL_Reg lua_world_fns[] = {
    {"getBlock", lua_world_getBlock},
    {"setBlock", lua_world_setBlock},
    {"sendBlockUpdate", lua_world_sendBlockUpdate},
    {"getCurrent", lua_world_getCurrent},
    {nullptr, nullptr}
};
const luaL_Reg lua_data_fns[] = {
    {"setPlayer", lua_data_setPlayer},
    {"getPlayer", lua_data_getPlayer},
    {nullptr, nullptr}
};
const luaL_Reg lua_commands_fns[] = {
    {"execute", lua_commands_execute},
    {"unregister", lua_commands_unregister},
    {"literal", lua_commands_literal},
    {"string", lua_commands_string},
    {"greedyString", lua_commands_greedyString},
    {"integer", lua_commands_integer},
    {"float", lua_commands_float},
    {"boolean", lua_commands_boolean},
    {"enum", lua_commands_enum},
    {"vec3", lua_commands_vec3},
    {"addChild", lua_commands_addChild},
    {"setAction", lua_commands_setAction},
    {"setDescription", lua_commands_setDescription},
    {"requireOperator", lua_commands_requireOperator},
    {"register", lua_commands_register},
    {"sendFeedback", lua_commands_sendFeedback},
    {"getExecutor", lua_commands_getExecutor},
    {"getStringArg", lua_commands_getStringArg},
    {"getIntegerArg", lua_commands_getIntegerArg},
    {"getFloatArg", lua_commands_getFloatArg},
    {"getBooleanArg", lua_commands_getBooleanArg},
    {"getEnumArg", lua_commands_getEnumArg},
    {"getVec3Arg", lua_commands_getVec3Arg},
    {nullptr, nullptr}
};

void registerLib(lua_State* L, const char* name, const luaL_Reg* fns) {
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setglobal(L, name);
}

// Exposes bp_damage_type as the `damageType` table (damageType.FALL, ...).
void registerDamageTypes(lua_State* L) {
    static const struct { const char* name; bp_damage_type value; } types[] = {
        {"NONE", BP_DAMAGE_NONE},           {"FALL", BP_DAMAGE_FALL},
        {"DROWN", BP_DAMAGE_DROWN},         {"BURN", BP_DAMAGE_BURN},
        {"LAVA", BP_DAMAGE_LAVA},           {"PUNCH", BP_DAMAGE_PUNCH},
        {"ARROW", BP_DAMAGE_ARROW},         {"EXPLOSION", BP_DAMAGE_EXPLOSION},
        {"LIGHTNING", BP_DAMAGE_LIGHTNING}, {"SUFFOCATION", BP_DAMAGE_SUFFOCATION},
        {"COMMAND", BP_DAMAGE_COMMAND},     {"VOID", BP_DAMAGE_VOID},
        {"CACTUS", BP_DAMAGE_CACTUS},       {"ITEM", BP_DAMAGE_ITEM},
    };
    lua_newtable(L);
    for (const auto& t : types) {
        lua_pushinteger(L, t.value);
        lua_setfield(L, -2, t.name);
    }
    lua_setglobal(L, "damageType");
}

void registerApi(lua_State* L) {
    registerLib(L, "log",    lua_log_fns);
    registerLib(L, "server", lua_server_fns);
    registerLib(L, "player", lua_player_fns);
    registerLib(L, "entity", lua_entity_fns);
    registerLib(L, "world",  lua_world_fns);
    registerLib(L, "data",   lua_data_fns);
    registerLib(L, "commands", lua_commands_fns);
    registerDamageTypes(L);
}

bool readFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool loadAndRun(lua_State* L, std::string source, const std::string& chunkName) {
    // Editors on Windows like to prepend a UTF-8 BOM, which the Lua lexer rejects.
    if (source.compare(0, 3, "\xEF\xBB\xBF") == 0) source.erase(0, 3);

    // Text chunks only; precompiled bytecode isn't accepted from script files.
    if (luaL_loadbufferx(L, source.data(), source.size(), chunkName.c_str(), "t") != LUA_OK) {
        return false; // syntax error message is on the stack
    }
    return lua_pcall(L, 0, 0, 0) == LUA_OK;
}

void loadPluginFile(const std::filesystem::path& path) {
    lua_State* raw = luaL_newstate();
    if (!raw) {
        g_api->log.error(g_api, "Failed to create Lua state");
        return;
    }
    LuaState state(raw);

    // Full standard library (io, os, package, ...); scripts are not sandboxed.
    luaL_openlibs(raw);
    registerApi(raw);

    std::string source;
    if (!readFile(path, source)) {
        std::string msg = "Failed to read '" + path.string() + "'";
        g_api->log.error(g_api, msg.c_str());
        return;
    }

    std::string name = path.filename().string();
    if (!loadAndRun(raw, std::move(source), "@" + name)) {
        g_api->log.error(g_api, errorMessage(raw));
        // The script may have registered commands before failing; the host keeps
        // those, so make their actions no-ops before the VM is closed below.
        detachActions(raw);
        return; // `state` destructor closes the Lua VM
    }

    std::string msg = "Loaded plugin '" + name + "'";
    g_api->log.info(g_api, msg.c_str());

    Plugin plugin;
    plugin.state = std::move(state);
    plugin.name = std::move(name);
    g_plugins.push_back(std::move(plugin));
}

void loadAllPlugins() {
    std::error_code ec;
    if (!std::filesystem::is_directory(scriptsDir, ec)) {
        g_api->log.warning(g_api, "No 'lua' directory found, no plugins loaded");
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
        lua_State* L = plugin.state.get();
        if (getGlobalFunction(L, "OnUnload")) {
            callWithReport(L, 0, 0);
        }
    }
    // Detach command actions so commands the host still holds become no-ops.
    // The Action records themselves stay allocated: the host may still hold
    // them as userData.
    for (Action& a : g_actions) {
        a.L = nullptr;
        a.ref = LUA_NOREF;
    }
    g_currentAction = nullptr;
    g_nodes.clear();
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

void OnPlayerLeave(const bp_api* /*api*/, const bp_player_leave_event* ev) {
    for (auto& plugin : g_plugins) {
        lua_State* L = plugin.state.get();
        if (getGlobalFunction(L, "OnPlayerLeave")) {
            lua_pushlightuserdata(L, ev->player);
            callWithReport(L, 1, 0);
        }
        clearPlayerData(L, ev->player);
    }
}

void OnPlayerMove(const bp_api* /*api*/, bp_player_move_event* ev) {
    for (auto& plugin : g_plugins) {
        lua_State* L = plugin.state.get();
        if (!getGlobalFunction(L, "OnPlayerMove")) continue;

        lua_pushlightuserdata(L, ev->player);
        pushVec3(L, ev->from);
        pushVec3(L, ev->to);

        // A plugin can cancel the move by returning `false`.
        if (callWithReport(L, 3, 1)) applyCancelReturn(L, ev->cancel);
    }
}

void OnItemUse(const bp_api* /*api*/, bp_item_use_event* ev) {
    for (auto& plugin : g_plugins) {
        lua_State* L = plugin.state.get();
        if (!getGlobalFunction(L, "OnItemUse")) continue;

        lua_pushlightuserdata(L, ev->player);
        pushItemStack(L, ev->item);

        // A plugin can cancel the item use by returning `false`.
        if (callWithReport(L, 2, 1)) applyCancelReturn(L, ev->cancel);
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
        // Appended after the original arguments so existing scripts keep working.
        pushItemStack(L, ev->heldItem);
        pushBlock(L, ev->block);

        // A plugin can cancel the block-use by returning `false`.
        if (callWithReport(L, 7, 1)) applyCancelReturn(L, ev->cancel);
    }
}

void OnBlockBreak(const bp_api* /*api*/, bp_block_break_event* ev) {
    g_world = ev->world; // fallback world context, kept fresh here too

    for (auto& plugin : g_plugins) {
        lua_State* L = plugin.state.get();
        if (!getGlobalFunction(L, "OnBlockBreak")) continue;

        lua_pushlightuserdata(L, ev->player);
        lua_pushlightuserdata(L, ev->world);
        pushItemStack(L, ev->tool);
        lua_pushinteger(L, ev->blockPos.x);
        lua_pushinteger(L, ev->blockPos.y);
        lua_pushinteger(L, ev->blockPos.z);
        pushBlock(L, ev->block);

        // A plugin can cancel the break by returning `false`.
        if (callWithReport(L, 7, 1)) applyCancelReturn(L, ev->cancel);
    }
}

void OnBlockPlace(const bp_api* /*api*/, bp_block_place_event* ev) {
    g_world = ev->world; // fallback world context, kept fresh here too

    for (auto& plugin : g_plugins) {
        lua_State* L = plugin.state.get();
        if (!getGlobalFunction(L, "OnBlockPlace")) continue;

        lua_pushlightuserdata(L, ev->player);
        lua_pushlightuserdata(L, ev->world);
        lua_pushinteger(L, ev->blockPos.x);
        lua_pushinteger(L, ev->blockPos.y);
        lua_pushinteger(L, ev->blockPos.z);
        lua_pushinteger(L, ev->blockId);

        // A plugin can cancel the placement by returning `false`.
        if (callWithReport(L, 6, 1)) applyCancelReturn(L, ev->cancel);
    }
}

void OnEntityDamage(const bp_api* /*api*/, bp_entity_damage_event* ev) {
    for (auto& plugin : g_plugins) {
        lua_State* L = plugin.state.get();
        if (!getGlobalFunction(L, "OnEntityDamage")) continue;

        lua_pushlightuserdata(L, ev->entity);
        lua_pushinteger(L, ev->amount);
        if (ev->attacker) lua_pushlightuserdata(L, ev->attacker); else lua_pushnil(L);
        lua_pushinteger(L, ev->type);

        // OnEntityDamage(entity, amount, attacker|nil, damageType)
        // Return `false` to cancel the damage, or a number to change the amount.
        if (callWithReport(L, 4, 1)) applyDamageReturn(L, ev);
    }
}

void OnServerTick(const bp_api* /*api*/, const bp_server_tick_event* /*ev*/) {
    for (auto& plugin : g_plugins) {
        lua_State* L = plugin.state.get();
        if (!getGlobalFunction(L, "OnServerTick")) continue;
        callWithReport(L, 0, 0);
    }
}

void OnPlayerChat(const bp_api* /*api*/, bp_player_chat_event* ev) {
    for (auto& plugin : g_plugins) {
        lua_State* L = plugin.state.get();
        if (!getGlobalFunction(L, "OnPlayerChat")) continue;

        lua_pushlightuserdata(L, ev->player);
        lua_pushstring(L, ev->message);

        // A plugin can cancel the chat message by returning `false`.
        if (callWithReport(L, 2, 1)) applyCancelReturn(L, ev->cancel);
    }
}

// Fired when the server crashes. Scripts get (crashLog, crashLogPath, version);
// any field the server doesn't provide arrives as nil. Nothing can be cancelled.
void OnCrash(const bp_api* /*api*/, const bp_crash_event* ev) {
    for (auto& plugin : g_plugins) {
        lua_State* L = plugin.state.get();
        if (!getGlobalFunction(L, "OnCrash")) continue;

        lua_pushstring(L, ev->crash_log);
        lua_pushstring(L, ev->crash_log_path);
        lua_pushstring(L, ev->version_string);
        callWithReport(L, 3, 0);
    }
}

void OnLoad(const bp_api* api) {
    // The struct layout changes between API versions, so refuse hosts older than what we were built against.
    if (api->version < ADDON_API_VERSION) {
        std::string err = "Host addon API v" + std::to_string(api->version) +
            " is older than required v" + std::to_string(ADDON_API_VERSION) + "; Lua not loaded";
        api->log.error(api, err.c_str());
        return;
    }
    g_api = api;

    loadAllPlugins();

    std::string msg = "Lua initialized! (" +
        std::to_string(g_plugins.size()) + " plugin(s) loaded)";
    api->log.info(api, msg.c_str());
}

void OnUnload(const bp_api* api) {
    unloadAllPlugins();
    api->log.info(api, "Lua uninitialized!");
}

} // extern "C"

} // namespace
} // namespace bpa

// The host looks this symbol up by name, so it has to be exported explicitly on Windows.
extern "C"
#ifdef _WIN32
    __declspec(dllexport)
#else
    __attribute__((visibility("default")))
#endif
bp_addon_info bp_addon(const bp_api* /*api*/) {
    return bp_addon_info{
        bpa::ADDON_ID,
        bpa::ADDON_NAME,
        bpa::ADDON_VERSION,
        ADDON_API_VERSION,
        0,
        bp_addon_events{
            /* playerJoin   */ bpa::OnPlayerJoin,
            /* playerLeave  */ bpa::OnPlayerLeave,
            /* playerChat   */ bpa::OnPlayerChat,
            /* playerMove   */ bpa::OnPlayerMove,
            /* itemUse      */ bpa::OnItemUse,
            /* blockBreak   */ bpa::OnBlockBreak,
            /* blockPlace   */ bpa::OnBlockPlace,
            /* blockUse     */ bpa::OnBlockUse,
            /* entityDamage */ bpa::OnEntityDamage,
            /* serverTick   */ bpa::OnServerTick,
            /* crash        */ bpa::OnCrash,
            /* addonLoad    */ bpa::OnLoad,
            /* addonUnload  */ bpa::OnUnload,
        },
    };
}