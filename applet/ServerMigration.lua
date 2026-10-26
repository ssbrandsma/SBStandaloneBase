local M = {}

M.BOOTSTRAP_UUID = "11111111-2222-4333-8444-555555555555"
M.BOOTSTRAP_NAME = "Squeezebox Bootstrap"
M.LOCAL_IP = "127.0.0.1"
M.LOCAL_UUID = "9d989f40-499a-4b85-b92f-8dc415af2a04"
M.LOCAL_NAME = "StandaloneBase"

function M.isBootstrapSettings(settings)
    if not settings then return false end
    return settings.serverUuid == M.BOOTSTRAP_UUID
        or settings.serverName == M.BOOTSTRAP_NAME
end

function M.migrateSettingsText(name, original)
    local updated = original
    if name == "SlimDiscovery.lua" then
        local bootstrap = original:find(M.BOOTSTRAP_UUID, 1, true)
            or original:find('serverName="' .. M.BOOTSTRAP_NAME .. '"', 1, true)
        if bootstrap then
            updated = updated:gsub('(serverUuid%s*=%s*)"[^"]*"', '%1"' .. M.LOCAL_UUID .. '"')
            updated = updated:gsub('(serverName%s*=%s*)"[^"]*"', '%1"' .. M.LOCAL_NAME .. '"')
            updated = updated:gsub('(serverInit%s*=%s*{%s*ip%s*=%s*)"[^"]+"', '%1"' .. M.LOCAL_IP .. '"')
        end
    end
    return updated, updated ~= original
end

function M.migrateLiveSettings(chooseSettings, discoverySettings)
    local bootstrapIp
    if M.isBootstrapSettings(discoverySettings) and discoverySettings.serverInit then
        bootstrapIp = discoverySettings.serverInit.ip
    end
    local changedChoose = false
    if chooseSettings then
        chooseSettings.poll = chooseSettings.poll or {}
        if bootstrapIp and bootstrapIp ~= M.LOCAL_IP and chooseSettings.poll[bootstrapIp] then
            chooseSettings.poll[bootstrapIp] = nil
            changedChoose = true
        end
        if chooseSettings.poll[M.LOCAL_IP] ~= M.LOCAL_IP then
            chooseSettings.poll[M.LOCAL_IP] = M.LOCAL_IP
            changedChoose = true
        end
    end

    local changedDiscovery = false
    if M.isBootstrapSettings(discoverySettings) then
        discoverySettings.serverUuid = M.LOCAL_UUID
        discoverySettings.serverName = M.LOCAL_NAME
        discoverySettings.serverInit = discoverySettings.serverInit or {}
        discoverySettings.serverInit.ip = M.LOCAL_IP
        discoverySettings.squeezeNetwork = false
        changedDiscovery = true
    end
    return changedChoose, changedDiscovery
end

-- Recover only a disconnected remembered player attached to the local Base
-- identity. Never replace a real remote server/player or an unconfigured device.
function M.localPlayerForOrphanSelection(settings, current, localPlayer)
    if not settings or settings.serverUuid ~= M.LOCAL_UUID
        or not settings.serverInit or settings.serverInit.ip ~= M.LOCAL_IP
        or not settings.playerInit or settings.playerInit.model ~= "baby"
        or not current or not localPlayer or current == localPlayer then return nil end
    if not localPlayer:isLocal() or current:isLocal() or current:getSlimServer()
        or current.config == "needsServer" then return nil end
    local id = current:getId()
    if settings.playerId ~= id or (settings.currentPlayer and settings.currentPlayer ~= id) then return nil end
    local server = localPlayer:getSlimServer()
    if not server or server.id ~= M.LOCAL_UUID then return nil end
    return localPlayer
end

return M
