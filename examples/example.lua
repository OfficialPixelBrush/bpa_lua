-- example.lua

-- Handles are opaque values owned by the server; just pass them back to the API.
-- Position tables look like { x, y, z }, blocks like { id, meta },
-- and item stacks like { id, count, data }.

log.info("Loaded!")
log.info("Running on server version " .. tostring(server.getVersion()))

function OnPlayerJoin(playerHandle)
    player.sendMessage(playerHandle, "Welcome to the server!")
end

function OnPlayerLeave(playerHandle)
    log.info(player.getUsername(playerHandle) .. " left the server")
end

function OnPlayerChat(playerHandle, message)
    log.info(player.getUsername(playerHandle) .. " said: " .. message)

    -- Returning false cancels the chat message.
    if message == "banned word" then
        player.sendMessage(playerHandle, "Watch your language!")
        return false
    end

    -- Run a server command on the player's behalf. Pass nil as the sender
    -- to run it without a player.
    if message == "!tps" then
        player.sendMessage(playerHandle, string.format("TPS: %.1f, MSPT: %.2f",
            server.getTpsAverage(), server.getMsptAverage()))
        return false
    end

    if message == "!stop" then
        local ok = commands.execute(playerHandle, "stop")
        player.sendMessage(playerHandle, ok and "Command executed" or "Command failed")
        return false
    end

    if message == "kick me" then
        player.kick(playerHandle, "Asked for it")
    end

    -- The standard io library works, e.g. keep a chat log next to the server.
    if message == "!log" then
        local f, err = io.open("chat_log.txt", "a")
        if f then
            f:write(player.getUsername(playerHandle), ": ", message, "\n")
            f:close()
        else
            log.warning("Could not open chat_log.txt: " .. tostring(err))
        end
        return false
    end

    return nil
end

function OnPlayerMove(playerHandle, from, to)
    -- Simple example: stop anyone from going below bedrock.
    if to.y < 0 then
        return false
    end

    return nil
end

function OnItemUse(playerHandle, itemStack)
    log.info(player.getUsername(playerHandle) .. " used item id=" .. itemStack.id)
end

function OnBlockUse(playerHandle, worldHandle, x, y, z)
    local block = world.getBlock(worldHandle, x, y, z)
    log.info(string.format("Block used at (%d, %d, %d): id=%d meta=%d", x, y, z, block.id, block.meta))

    -- Remember the last block each player used, using the data.* API.
    data.setPlayer(playerHandle, { lastBlockId = block.id, x = x, y = y, z = z })
end

function OnBlockBreak(playerHandle, worldHandle, tool, x, y, z, block)
    log.info(string.format("%s broke block id=%d at (%d, %d, %d) with item id=%d",
        player.getUsername(playerHandle), block.id, x, y, z, tool.id))
end

function OnBlockPlace(playerHandle, worldHandle, x, y, z, blockId)
    log.info(string.format("%s placed block id=%d at (%d, %d, %d)",
        player.getUsername(playerHandle), blockId, x, y, z))

    -- Protect id 7 (e.g. bedrock) from being placed by players.
    if blockId == 7 then
        return false
    end

    return nil
end

function OnEntityDamage(entityHandle, amount)
    local pos = entity.getPosition(entityHandle)
    log.info(string.format("Entity at (%.1f, %.1f, %.1f) took %d damage", pos.x, pos.y, pos.z, amount))
end

-- Careful with this one: it fires every server tick, so keep it cheap.
local tickCount = 0
function OnServerTick()
    tickCount = tickCount + 1
end

-- Fired when the server crashes; fields the server doesn't provide are nil.
function OnCrash(crashLog, crashLogPath, version)
    log.error("Server crashed on version " .. (version or "?") .. ", log at " .. (crashLogPath or "?"))
end

function OnUnload()
    log.info("Unloading, bye! Saw " .. tickCount .. " ticks.")
end
