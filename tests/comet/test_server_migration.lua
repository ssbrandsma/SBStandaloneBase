package.path = arg[1] .. "/applet/?.lua;" .. package.path

local migration = require("ServerMigration")

local localServer = {id=migration.LOCAL_UUID}
local localPlayer = {isLocal=function() return true end,
    getSlimServer=function() return localServer end, getId=function() return "00:04:20:29:16:7f" end}
local orphan = {isLocal=function() return false end,
    getSlimServer=function() return nil end, getId=function() return "00:04:20:2a:56:81" end}
local saved = {serverUuid=migration.LOCAL_UUID,serverInit={ip="127.0.0.1"},
    playerId=orphan:getId(),currentPlayer=orphan:getId(),playerInit={model="baby"}}
assert(migration.localPlayerForOrphanSelection(saved,orphan,localPlayer)==localPlayer)
assert(migration.localPlayerForOrphanSelection(saved,localPlayer,localPlayer)==nil)
orphan.getSlimServer=function() return {id="external-lms"} end
assert(migration.localPlayerForOrphanSelection(saved,orphan,localPlayer)==nil)
orphan.getSlimServer=function() return nil end
orphan.config="needsServer"
assert(migration.localPlayerForOrphanSelection(saved,orphan,localPlayer)==nil)
orphan.config=nil
saved.serverUuid="external-lms"
assert(migration.localPlayerForOrphanSelection(saved,orphan,localPlayer)==nil)
saved.serverUuid=migration.LOCAL_UUID
saved.serverInit.ip="192.168.1.100"
assert(migration.localPlayerForOrphanSelection(saved,orphan,localPlayer)==nil)
saved.serverInit.ip="127.0.0.1"
saved.playerId="another-player"
assert(migration.localPlayerForOrphanSelection(saved,orphan,localPlayer)==nil)
saved.playerId=orphan:getId()
localServer.id="external-lms"
assert(migration.localPlayerForOrphanSelection(saved,orphan,localPlayer)==nil)
localServer.id=migration.LOCAL_UUID
assert(migration.localPlayerForOrphanSelection(saved,orphan,nil)==nil)
assert(migration.localPlayerForOrphanSelection(saved,nil,localPlayer)==nil)
saved.currentPlayer="a-different-selection"
assert(migration.localPlayerForOrphanSelection(saved,orphan,localPlayer)==nil)
saved.currentPlayer=orphan:getId()
saved.playerInit.model="touch"
assert(migration.localPlayerForOrphanSelection(saved,orphan,localPlayer)==nil)
saved.playerInit.model="baby"

-- Exercise the actual startup integration, including its one-time guard.
local file=assert(io.open(arg[1].."/applet/StandaloneBaseApplet.lua"))
local appletSource=file:read("*a");file:close()
local connectSource=assert(appletSource:match("(local function connectToLocalServer%b().-)local function pidPath"))
local current,recovered=orphan,0
localServer.connect=function() end
localPlayer.connectToServer=function() error("Unexpected forced local reconnect") end
local manager={callService=function() return current end,
    loadApplet=function() return {getSettings=function() return saved end} end}
local players={getLocalPlayer=function() return localPlayer end,
    setCurrentPlayer=function(_,player) current=player;recovered=recovered+1 end}
local servers={iterate=function()
    local done=false
    return function() if not done then done=true;return 1,localServer end end
end}
local connect=assert(loadstring("local appletManager,Player,SlimServer,ServerMigration,migrationLog=...; local checkedInitialPlayerSelection=false; "
    ..connectSource.." return connectToLocalServer"))(manager,players,servers,migration,{info=function() end})
assert(connect() and current==localPlayer and recovered==1)
assert(connect() and recovered==1)
-- A later genuine user selection must not be pulled back to Base by supervise.
local external={id="external-lms",getName=function() return "Desktop LMS" end}
current={getSlimServer=function() return external end}
assert(not connect() and recovered==1)

local bootstrapIp = "203.0.113.10"
local old = 'settings = {serverUuid="11111111-2222-4333-8444-555555555555",serverInit={ip="' .. bootstrapIp .. '",},serverName="Squeezebox Bootstrap",}'
local updated, changed = migration.migrateSettingsText("SlimDiscovery.lua", old)
assert(changed)
assert(updated:find('serverUuid="' .. migration.LOCAL_UUID .. '"', 1, true))
assert(updated:find('serverInit={ip="127.0.0.1"', 1, true))
assert(updated:find('serverName="StandaloneBase"', 1, true))
assert(not updated:find(bootstrapIp, 1, true))

local unrelatedIp = "192.0.2.20"
local unrelated = 'settings = {serverUuid="other",serverInit={ip="' .. unrelatedIp .. '",},serverName="Living Room LMS",}'
local same, unrelatedChanged = migration.migrateSettingsText("SlimDiscovery.lua", unrelated)
assert(not unrelatedChanged and same == unrelated)

local choose = {poll={ ["255.255.255.255"]="255.255.255.255", [bootstrapIp]=bootstrapIp, [unrelatedIp]=unrelatedIp }}
local discovery = {serverUuid=migration.BOOTSTRAP_UUID,serverName=migration.BOOTSTRAP_NAME,serverInit={ip=bootstrapIp}}
local chooseChanged, discoveryChanged = migration.migrateLiveSettings(choose, discovery)
assert(chooseChanged and discoveryChanged)
assert(choose.poll[bootstrapIp] == nil)
assert(choose.poll[migration.LOCAL_IP] == migration.LOCAL_IP)
assert(choose.poll[unrelatedIp] == unrelatedIp)
assert(discovery.serverUuid == migration.LOCAL_UUID)
assert(discovery.serverName == migration.LOCAL_NAME)

local alreadyChoose = {poll={ [migration.LOCAL_IP]=migration.LOCAL_IP }}
local alreadyDiscovery = {serverUuid=migration.LOCAL_UUID,serverName=migration.LOCAL_NAME,serverInit={ip=migration.LOCAL_IP}}
local chooseAgain, discoveryAgain = migration.migrateLiveSettings(alreadyChoose, alreadyDiscovery)
assert(not chooseAgain and not discoveryAgain)

print("server migration tests passed")
