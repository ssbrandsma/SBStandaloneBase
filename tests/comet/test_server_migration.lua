package.path = arg[1] .. "/applet/?.lua;" .. package.path

local migration = require("ServerMigration")

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
