local M = {}

M.BOOTSTRAP_IP = "203.0.113.10"
M.BOOTSTRAP_UUID = "11111111-2222-4333-8444-555555555555"
M.BOOTSTRAP_NAME = "Squeezebox Bootstrap"
M.LOCAL_IP = "127.0.0.1"
M.LOCAL_UUID = "9d989f40-499a-4b85-b92f-8dc415af2a04"
M.LOCAL_NAME = "StandaloneBase"

local function escapePattern(value)
    return (value:gsub("(%W)", "%%%1"))
end

function M.isBootstrapSettings(settings)
    if not settings then return false end
    local initIp = settings.serverInit and settings.serverInit.ip
    return settings.serverUuid == M.BOOTSTRAP_UUID
        or settings.serverName == M.BOOTSTRAP_NAME
        or initIp == M.BOOTSTRAP_IP
end

function M.migrateSettingsText(name, original)
    local updated = original:gsub(escapePattern(M.BOOTSTRAP_IP), M.LOCAL_IP)
    if name == "SlimDiscovery.lua" then
        local bootstrap = original:find(M.BOOTSTRAP_UUID, 1, true)
            or original:find('serverName="' .. M.BOOTSTRAP_NAME .. '"', 1, true)
            or original:find(M.BOOTSTRAP_IP, 1, true)
        if bootstrap then
            updated = updated:gsub('(serverUuid%s*=%s*)"[^"]*"', '%1"' .. M.LOCAL_UUID .. '"')
            updated = updated:gsub('(serverName%s*=%s*)"[^"]*"', '%1"' .. M.LOCAL_NAME .. '"')
            updated = updated:gsub('(serverInit%s*=%s*{%s*ip%s*=%s*)"[^"]+"', '%1"' .. M.LOCAL_IP .. '"')
        end
    end
    return updated, updated ~= original
end

function M.migrateLiveSettings(chooseSettings, discoverySettings)
    local changedChoose = false
    if chooseSettings then
        chooseSettings.poll = chooseSettings.poll or {}
        if chooseSettings.poll[M.BOOTSTRAP_IP] then
            chooseSettings.poll[M.BOOTSTRAP_IP] = nil
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

return M
