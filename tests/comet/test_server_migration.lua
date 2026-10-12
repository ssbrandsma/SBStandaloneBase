package.path = arg[1] .. "/applet/?.lua;" .. package.path

local migration = require("ServerMigration")

local old = 'settings = {serverUuid="11111111-2222-4333-8444-555555555555",serverInit={ip="203.0.113.10",},serverName="Squeezebox Bootstrap",}'
local updated, changed = migration.migrateSettingsText("SlimDiscovery.lua", old)
assert(changed)
assert(updated:find('serverUuid="' .. migration.LOCAL_UUID .. '"', 1, true))
assert(updated:find('serverInit={ip="127.0.0.1"', 1, true))
assert(updated:find('serverName="StandaloneBase"', 1, true))
assert(not updated:find("203.0.113.10", 1, true))

local unrelated = 'settings = {serverUuid="other",serverInit={ip="192.0.2.10",},serverName="Living Room LMS",}'
local same, unrelatedChanged = migration.migrateSettingsText("SlimDiscovery.lua", unrelated)
assert(not unrelatedChanged and same == unrelated)

local choose = {poll={ ["255.255.255.255"]="255.255.255.255", [migration.BOOTSTRAP_IP]=migration.BOOTSTRAP_IP, ["192.0.2.10"]="192.0.2.10" }}
local discovery = {serverUuid=migration.BOOTSTRAP_UUID,serverName=migration.BOOTSTRAP_NAME,serverInit={ip=migration.LOCAL_IP}}
local chooseChanged, discoveryChanged = migration.migrateLiveSettings(choose, discovery)
assert(chooseChanged and discoveryChanged)
assert(choose.poll[migration.BOOTSTRAP_IP] == nil)
assert(choose.poll[migration.LOCAL_IP] == migration.LOCAL_IP)
assert(choose.poll["192.0.2.10"] == "192.0.2.10")
assert(discovery.serverUuid == migration.LOCAL_UUID)
assert(discovery.serverName == migration.LOCAL_NAME)

local alreadyChoose = {poll={ [migration.LOCAL_IP]=migration.LOCAL_IP }}
local alreadyDiscovery = {serverUuid=migration.LOCAL_UUID,serverName=migration.LOCAL_NAME,serverInit={ip=migration.LOCAL_IP}}
local chooseAgain, discoveryAgain = migration.migrateLiveSettings(alreadyChoose, alreadyDiscovery)
assert(not chooseAgain and not discoveryAgain)

print("server migration tests passed")
