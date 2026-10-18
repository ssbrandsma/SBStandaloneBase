local oo = require("loop.simple")
local Applet = require("jive.Applet")
local Framework = require("jive.ui.Framework")
local SimpleMenu = require("jive.ui.SimpleMenu")
local Textarea = require("jive.ui.Textarea")
local Window = require("jive.ui.Window")
local Timer = require("jive.ui.Timer")
local SlimServer = require("jive.slim.SlimServer")
local StorageManager = require("applets.StandaloneBase.StorageManager")
local ExtendedStorageState = require("applets.StandaloneBase.ExtendedStorageState")
local TimeSync = require("applets.StandaloneBase.TimeSync")
local RemoteBridge = require("applets.StandaloneBase.RemoteBridge")
local ServerMigration = require("applets.StandaloneBase.ServerMigration")
local NetworkAddress = require("applets.StandaloneBase.NetworkAddress")
local ImageRender = require("applets.StandaloneBase.ImageRender")
local Artwork = require("applets.StandaloneBase.Artwork")
local io, os, tonumber, tostring = io, os, tonumber, tostring
local ipairs, pairs, math, pcall, string = ipairs, pairs, math, pcall, string
local appletManager = appletManager
module(..., Framework.constants)
oo.class(_M, Applet)

local ROOT = "/usr/share/jive/applets/StandaloneBase"
local RUN = "/tmp/standalonebase"
local CONFIG = "/mnt/storage/standalonebase/config.json"
local SERVICES = {
    { name="HTTPS Proxy", exe="sbproxy", args="--listen 127.0.0.1:8765" },
    { name="Bootstrap service", exe="sbbase", args="--config "..CONFIG },
    { name="Webserver", exe="sbwebserver", args="--web-root "..ROOT.." --config-dir /mnt/storage/standalonebase" },
}
local started = false

function getStandaloneImageURL(self, url, width, height, mime)
    return ImageRender.artworkUrl(url, width, height, mime)
end

function getStandaloneArtworkURL(self, url, width, height)
    return Artwork.route(url, width, height)
end

local storage = StorageManager:new(ROOT)
local SETTINGS_DIR = "/etc/squeezeplay/userpath/settings"
local SERVER_SETTINGS = {
    "ChooseMusicSource.lua",
    "Playback.lua",
    "SlimDiscovery.lua",
}
local ARTWORK_PATCH = ROOT.."/patch-squeezeplay-artwork.sh"

local function patchArtworkRouting()
    -- On an Applet Installer reboot this prepares the active firmware file for
    -- the next SqueezePlay process. Package deployment runs the same helper
    -- before restarting SqueezePlay, so the change takes effect immediately.
    os.execute("chmod 755 "..ARTWORK_PATCH.." >/dev/null 2>&1")
    return os.execute(ARTWORK_PATCH.." >>"..RUN.."/artwork-patch.log 2>&1") == 0
end

-- mysqueezebox.com has been retired, but older SqueezePlay builds can retain
-- its SlimServer object and continuously retry its Comet connection.  Besides
-- wasting resources, those retries make the Radio show a misleading
-- "Connecting to Standalone Base" overlay while the loopback server is fine.
-- Disconnect only the retired cloud object; never touch the local LMS object.
local function suppressRetiredCloudServer()
    for _,server in SlimServer:iterate() do
        local ok,name=pcall(function() return server:getName() end)
        if ok and name == "mysqueezebox.com" then
            -- UI code can call connect() again after a normal disconnect.
            -- Shadow the instance methods so the retired service stays inert.
            if not server._standaloneBaseCloudBlocked then
                server._standaloneBaseCloudBlocked=true
                server.connect=function() return nil end
                server.reconnect=function() return nil end
            end
            pcall(function() server:disconnect() end)
        end
    end
end

local function redirectBootstrapServer()
    for _,name in ipairs(SERVER_SETTINGS) do
        local path=SETTINGS_DIR.."/"..name
        local f=io.open(path,"r")
        if f then
            local original=f:read("*a"); f:close()
            local updated,changed=ServerMigration.migrateSettingsText(name,original)
            if changed then
                local backup=path..".pre-standalonebase"
                local b=io.open(backup,"r")
                if b then b:close() else
                    b=io.open(backup,"w")
                    if b then b:write(original); b:close() end
                end
                local tmp=path..".standalonebase.tmp"
                local out=io.open(tmp,"w")
                if out then
                    out:write(updated); out:close()
                    os.rename(tmp,path)
                end
            end
        end
    end
end

local function redirectLiveBootstrapServer()
    local ok,choose=pcall(function() return appletManager:loadApplet("ChooseMusicSource") end)
    local chooseSettings=ok and choose and choose:getSettings() or nil
    local okDiscovery,discovery=pcall(function() return appletManager:loadApplet("SlimDiscovery") end)
    local discoverySettings=okDiscovery and discovery and discovery:getSettings() or nil
    local changedChoose,changedDiscovery=ServerMigration.migrateLiveSettings(chooseSettings,discoverySettings)
    if changedChoose then
        pcall(function()
            appletManager:callService("setPollList",chooseSettings.poll)
            choose:storeSettings()
        end)
    end
    if changedDiscovery then pcall(function() discovery:storeSettings() end) end
    return changedChoose or changedDiscovery
end

local function connectToLocalServer()
    local localServer
    for _,server in SlimServer:iterate() do
        if server.id == ServerMigration.LOCAL_UUID then localServer=server; break end
    end
    if not localServer then return false end
    pcall(function() localServer:connect() end)
    local ok,player=pcall(function() return appletManager:callService("getCurrentPlayer") end)
    if not ok or not player then return false end
    local current=player:getSlimServer()
    if current ~= localServer and current
        and (current.id == ServerMigration.BOOTSTRAP_UUID or current:getName() == ServerMigration.BOOTSTRAP_NAME) then
        return pcall(function() player:connectToServer(localServer) end)
    end
    return current == localServer
end

local function pidPath(s) return RUN.."/"..s.exe..".pid" end
local function alive(s)
    local f=io.open(pidPath(s),"r"); if not f then return false end
    local pid=tonumber(f:read("*l")); f:close()
    if not pid then return false end
    local exe=ROOT.."/"..s.exe
    return os.execute("test -r /proc/"..tostring(pid).."/cmdline && grep -q '^"..exe.."' /proc/"..tostring(pid).."/cmdline 2>/dev/null") == 0
end
local function launch(s)
    if alive(s) then return true end
    os.remove(pidPath(s))
    os.execute("chmod 755 "..ROOT.."/"..s.exe.." >/dev/null 2>&1")
    local cmd="( "..ROOT.."/"..s.exe.." "..s.args.." >>"..RUN.."/"..s.exe..".log 2>&1 & pid=$!; "..
        "sleep 1; if kill -0 $pid >/dev/null 2>&1; then echo $pid >"..pidPath(s).."; else rm -f "..pidPath(s).."; fi )"
    os.execute(cmd)
    return alive(s)
end
local function stop(s)
    local f=io.open(pidPath(s),"r"); if not f then return end
    local pid=tonumber(f:read("*l")); f:close()
    if pid and alive(s) then os.execute("kill "..tostring(pid).." >/dev/null 2>&1") end
    os.remove(pidPath(s))
end
local function refreshConfig()
    local f=io.open(CONFIG,"r")
    if f then f:close() else os.execute("cp "..ROOT.."/config.json "..CONFIG) end
end
function init(self)
    if started then return end; started=true
    os.execute("mkdir -p "..RUN.." /mnt/storage/standalonebase")
    os.execute("chmod 755 "..ROOT.."/svgpngrenderer "..ROOT.."/sb-storage-helper "..ROOT.."/storage-setup.sh "..ROOT.."/storage-boot.sh >/dev/null 2>&1")
    redirectBootstrapServer()
    patchArtworkRouting()
    suppressRetiredCloudServer()
    self.failures={}; self.backoff={}
    launch(SERVICES[1])
    refreshConfig()
    for i=2,#SERVICES do if not launch(SERVICES[i]) then self.failures[SERVICES[i].exe]=1 end end
    self.timeSync=TimeSync.new(); self.timeSync:start()
    self.remoteBridge=RemoteBridge(); self.remoteBridge:init()
    redirectLiveBootstrapServer()
    self.monitor=Timer(15000,function() self:supervise() end); self.monitor:start()
end
function startServices(self)
    for _,s in ipairs(SERVICES) do if not launch(s) then self.failures[s.exe]=1 end end
end
function supervise(self)
    suppressRetiredCloudServer()
    redirectLiveBootstrapServer()
    connectToLocalServer()
    for _,s in ipairs(SERVICES) do
        if not alive(s) then
            local n=(self.failures[s.exe] or 0)+1; self.failures[s.exe]=n
            if n <= 5 and not self.backoff[s.exe] then
                self.backoff[s.exe]=Timer(math.min(60000,n*5000),function() self.backoff[s.exe]=nil; launch(s) end,true)
                self.backoff[s.exe]:start()
            end
        else self.failures[s.exe]=0 end
    end
end
function menu(self)
    local window=Window("text_list","Standalone Base")
    local menu=SimpleMenu("menu")
    for _,s in ipairs(SERVICES) do menu:addItem({text=s.name..": "..(alive(s) and "Running" or "Stopped")}) end
    menu:addItem({text="Extended Storage",callback=function() self:storageMenu() end})
    menu:addItem({text="Web Access",callback=function() if not self.remoteBridge:isInjecting() then self:webAccessMenu() end end})
	menu:addItem({text="Version: 0.4.4"})
    window:addWidget(menu); window:show()
end
function webAccessMenu(self)
    local window=Window("text_list","Web Access")
    local menu=SimpleMenu("menu")
    menu:setHeaderWidget(Textarea("help_text","Pair a browser without SSH. Pairing actions must be selected on the physical Radio."))
    menu:addItem({text="Show Pairing Code",callback=function() if not self.remoteBridge:isInjecting() then self:showPairingCode(window) end end})
    menu:addItem({text="Revoke All Web Sessions",callback=function() if not self.remoteBridge:isInjecting() then self:confirmWebRevoke(window) end end})
    window:addWidget(menu); window:show()
end
function showPairingCode(self,previous)
    self.remoteBridge:webAccess("start",function(data,err)
        if err or not data then self:webAccessError(previous,"Could not create a pairing code."); return end
        local pin=data:match('"pin"%s*:%s*"(%d%d%d%d%d%d)"')
        local seconds=tonumber(data:match('"expiresIn"%s*:%s*(%d+)')) or 120
        if not pin then self:webAccessError(previous,"The pairing service returned an invalid response."); return end
        if previous then previous:hide() end
        local window=Window("text_list","Web Access")
        window:setAllowScreensaver(false)
        local menu=SimpleMenu("menu")
        menu:setHeaderWidget(Textarea("help_text","Enter this code in the Standalone Base web page. Remote screen capture is disabled while the code is active."))
        local pinItem={text="Pairing code: "..pin:sub(1,3).." "..pin:sub(4,6),style="item_info"}
        local address=NetworkAddress.current()
        local addressItem={text=address and ("Web access: http://"..address) or "Web access: IP address unavailable",style="item_info"}
        local expiresItem={text="Expires in 02:00",style="item_info"}
        menu:addItem(pinItem); menu:addItem(addressItem); menu:addItem(expiresItem)
        local remaining=seconds
        local timer
        timer=Timer(1000,function()
            remaining=remaining-1
            if remaining<=0 then timer:stop(); menu:setText(expiresItem,"Code expired")
            else menu:setText(expiresItem,string.format("Expires in %02d:%02d",math.floor(remaining/60),remaining%60)) end
        end); timer:start()
        menu:addItem({text="Cancel Pairing",callback=function() timer:stop(); self.remoteBridge:webAccess("cancel",function() end); window:hide() end})
        window:addWidget(menu); window:show()
    end)
end
function confirmWebRevoke(self,previous)
    local window=Window("text_list","Revoke Web Sessions?")
    local menu=SimpleMenu("menu")
    menu:setHeaderWidget(Textarea("help_text","Every paired browser will be logged out immediately."))
    menu:addItem({text="Cancel",callback=function() window:hide() end})
    menu:addItem({text="Revoke All",callback=function()
        if self.remoteBridge:isInjecting() then return end
        self.remoteBridge:webAccess("revoke-all",function(data,err)
            window:hide(); if err or not data then self:webAccessError(previous,"Session revocation failed.") else self:webAccessError(previous,"All web sessions were revoked.") end
        end)
    end})
    window:addWidget(menu); window:show()
end
function webAccessError(self,previous,message)
    if previous then previous:hide() end
    local window=Window("text_list","Web Access")
    local menu=SimpleMenu("menu"); menu:setHeaderWidget(Textarea("help_text",message))
    menu:addItem({text="Close",callback=function() window:hide() end})
    window:addWidget(menu); window:show()
end
function storageMenu(self)
    local s=storage:check()
    local window=Window("text_list","Extended Storage")
    local menu=SimpleMenu("menu")
    menu:addItem({text="Status: "..ExtendedStorageState.statusLabel(s.STATUS),style="item_info"})
    if s.STATUS=="ACTIVE" then
        menu:addItem({text="Capacity: "..ExtendedStorageState.mibFromKb(s.TOTAL_KB),style="item_info"})
        menu:addItem({text="Used: "..ExtendedStorageState.mibFromKb(s.USED_KB),style="item_info"})
        menu:addItem({text="Free: "..ExtendedStorageState.mibFromKb(s.FREE_KB),style="item_info"})
        menu:addItem({text="Applet storage: Extended Storage",style="item_info"})
    elseif s.STATUS=="AVAILABLE" then
        menu:setHeaderWidget(Textarea("help_text","Additional internal storage is available on this Radio."))
        if tonumber(s.INITIALIZATION_SUPPORTED)==1 then
            menu:addItem({text="Initialize Storage",callback=function() self:confirmStorageInitialization() end})
        else menu:addItem({text="Initialization prerequisites unavailable",style="item_info"}) end
    elseif s.STATUS=="REBOOT_REQUIRED" then
        menu:setHeaderWidget(Textarea("help_text","Extended Storage is initialized. Restart the Radio to activate it."))
        menu:addItem({text="Restart Now",callback=function() appletManager:callService("reboot") end})
    else
        menu:setHeaderWidget(Textarea("help_text","Extended Storage cannot be initialized on the detected storage layout."))
    end
    window:addWidget(menu); window:show()
end

function confirmStorageInitialization(self)
    local window=Window("text_list","Initialize Extended Storage?")
    local menu=SimpleMenu("menu")
    menu:setHeaderWidget(Textarea("help_text","This creates or erases a dedicated approximately 64 MB storage volume in internal flash and migrates current applets to it. The normal firmware and settings storage will not be erased. A restart will be required."))
    menu:addItem({text="Cancel",callback=function() window:hide() end})
    menu:addItem({text="Initialize",callback=function() window:hide(); self:startStorageInitialization() end})
    window:addWidget(menu); window:show()
end

function startStorageInitialization(self)
    local window=Window("text_list","Extended Storage")
    window:setAllowScreensaver(false)
    local menu=SimpleMenu("menu")
    local stageItem={text="Checking system...",style="item_info"}
    menu:addItem(stageItem); window:addWidget(menu); window:show()
    local function progress(status)
        local text=ExtendedStorageState.stageText(status.STAGE)
        if text then menu:setText(stageItem,text) end
    end
    local function done(exitCode,status)
        if exitCode==0 then self:storageSuccess(window)
        else self:storageError(window,tonumber(status.EXIT_CODE) or exitCode,status.ERROR) end
    end
    local ok=storage:startInitialization(progress,done)
    if not ok then self:storageError(window,1) end
end

function storageSuccess(self,previous)
    if previous then previous:hide() end
    local window=Window("text_list","Extended Storage")
    local menu=SimpleMenu("menu")
    menu:setHeaderWidget(Textarea("help_text","Extended Storage initialized successfully. Your applets have been migrated. Restart the Radio to activate Extended Storage."))
    menu:addItem({text="Restart Now",callback=function() appletManager:callService("reboot") end})
    menu:addItem({text="Later",callback=function() window:hide() end})
    window:addWidget(menu); window:show()
end

function storageError(self,previous,exitCode,reason)
    if previous then previous:hide() end
    local window=Window("text_list","Extended Storage")
    local menu=SimpleMenu("menu")
    menu:setHeaderWidget(Textarea("help_text",ExtendedStorageState.errorText(exitCode,reason)))
    menu:addItem({text="Error code: "..tostring(exitCode),style="item_info"})
    menu:addItem({text="Close",callback=function() window:hide() end})
    window:addWidget(menu); window:show()
end
function getStandaloneStorageInfo(self) return storage:getStorageInfo() end
function getStandaloneStorageCompatibility(self) return storage:getCompatibility() end
function getStandaloneStorageStatus(self) return storage:getStorageStatus() end
function getStandaloneStoragePreparation(self) return storage:getStoragePreparation() end
function verifyStandaloneStorage(self) return storage:verifyStorage() end
function free(self)
    if self.monitor then self.monitor:stop() end
    if self.timeSync then self.timeSync:stop(); self.timeSync=nil end
    if self.remoteBridge then self.remoteBridge:stop(); self.remoteBridge=nil end
    storage:stop()
    for _,t in pairs(self.backoff or {}) do t:stop() end
    for i=#SERVICES,1,-1 do stop(SERVICES[i]) end
    started=false; return true
end

return _M
