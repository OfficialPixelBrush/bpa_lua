-- example.lua
-- Drop this file in the addon's `lua_scripts/` directory and it'll be picked
-- up automatically on OnLoad, alongside any other .lua plugins there.

log.info("Loaded!")

function OnPlayerJoin(player)
    player.sendMessage(player, "Welcome to the server!")
end

function OnPlayerChat(player, message)
    log.info(player.getUsername(player) .. " said: " .. message)

    -- Returning false cancels the chat message.
    if message == "banned word" then
        player.sendMessage(player, "Watch your language!")
        return false
    end
end

function OnBlockUse(playerHandle, worldHandle, x, y, z)
    local block = world.getBlock(worldHandle, x, y, z)
    log.info(string.format("Block used at (%d, %d, %d): id=%d meta=%d", x, y, z, block.id, block.meta))

    -- Remember the last block each player used, using the data.* API.
    data.setPlayer(playerHandle, { lastBlockId = block.id, x = x, y = y, z = z })
end

function OnUnload()
    log.info("Unloading, bye!")
end