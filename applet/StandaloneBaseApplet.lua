local oo = require("loop.simple")
local Applet = require("jive.Applet")
local Framework = require("jive.ui.Framework")
local SimpleMenu = require("jive.ui.SimpleMenu")
local Textarea = require("jive.ui.Textarea")
local Window = require("jive.ui.Window")
local Timer = require("jive.ui.Timer")
local StorageManager = require("applets.StandaloneBase.StorageManager")
local ExtendedStorageState = require("applets.StandaloneBase.ExtendedStorageState")
local TimeSync = require("applets.StandaloneBase.TimeSync")
local io, os, tonumber, tostring = io, os, tonumber, tostring
local ipairs, pairs, math = ipairs, pairs, math
local appletManager = appletManager
module(..., Framework.constants)
oo.class(_M, Applet)

local ROOT = "/usr/share/jive/applets/StandaloneBase"
local RUN = "/tmp/standalonebase"
local CONFIG = "/mnt/storage/standalonebase/config.json"
local REMOTE_CONFIG = "http://127.0.0.1:8765/https/raw.githubusercontent.com/ssbrandsma/SBStandaloneBase/master/config.json"
local SERVICES = {
    { name="HTTPS Proxy", exe="sbproxy", args="--listen 127.0.0.1:8765" },
    { name="Bootstrap service", exe="sbbase", args="--config "..CONFIG },
    { name="Webserver", exe="sbwebserver", args="--web-root "..ROOT.." --config-dir /mnt/storage/standalonebase" },
}
local started = false
local storage = StorageManager:new(ROOT)
local BOOTSTRAP_IP_PATTERN = "49%.12%.198%.91"
local LOCAL_IP = "127.0.0.1"
local SERVER_UUID = "9d989f40-499a-4b85-b92f-8dc415af2a04"
local SETTINGS_DIR = "/etc/squeezeplay/userpath/settings"
local SERVER_SETTINGS = {
    "ChooseMusicSource.lua",
    "Playback.lua",
    "SlimDiscovery.lua",
}

local function redirectBootstrapServer()
    for _,name in ipairs(SERVER_SETTINGS) do
        local path=SETTINGS_DIR.."/"..name
        local f=io.open(path,"r")
        if f then
            local original=f:read("*a"); f:close()
            local updated,n=original:gsub(BOOTSTRAP_IP_PATTERN,LOCAL_IP)
            if name == "ChooseMusicSource.lua" then
                local normalized,m=updated:gsub('poll%s*=%s*%b{}','poll={["127.0.0.1"]="127.0.0.1",}')
                updated,n=normalized,n+m
            end
            if original:find(SERVER_UUID,1,true) or original:find('serverName="StandaloneBase"',1,true) then
                local normalized,m=updated:gsub('(serverInit%s*=%s*{%s*ip%s*=%s*)"[^"]+"','%1"'..LOCAL_IP..'"')
                updated,n=normalized,n+m
            end
            if n > 0 then
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
    local tmp=RUN.."/config.json.download"
    os.remove(tmp)
    local ok=os.execute("wget -q -T 20 -O "..tmp.." "..REMOTE_CONFIG)
    if ok == 0 then
        local c=io.open(tmp,"r")
        local body=c and c:read("*a") or ""
        if c then c:close() end
        if body:find('"StandaloneRadio"',1,true) and body:find('"SpotifyConnect"',1,true) then
            os.rename(tmp,CONFIG)
            return
        end
    end
    os.remove(tmp)
end
function init(self)
    if started then return end; started=true
    os.execute("mkdir -p "..RUN.." /mnt/storage/standalonebase")
    os.execute("chmod 755 "..ROOT.."/sb-storage-helper "..ROOT.."/storage-setup.sh "..ROOT.."/storage-boot.sh >/dev/null 2>&1")
    redirectBootstrapServer()
    self.failures={}; self.backoff={}
    launch(SERVICES[1])
    refreshConfig()
    for i=2,#SERVICES do if not launch(SERVICES[i]) then self.failures[SERVICES[i].exe]=1 end end
    self.timeSync=TimeSync.new(); self.timeSync:start()
    self.monitor=Timer(15000,function() self:supervise() end); self.monitor:start()
end
function startServices(self)
    for _,s in ipairs(SERVICES) do if not launch(s) then self.failures[s.exe]=1 end end
end
function supervise(self)
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
    menu:addItem({text="Version: 0.2.4"})
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
        menu:addItem({text="Initialize Storage",callback=function() self:confirmStorageInitialization() end})
        if tonumber(s.INITIALIZATION_SUPPORTED)~=1 then
            menu:addItem({text="Physical validation gate active",style="item_info"})
        end
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
    menu:setHeaderWidget(Textarea("help_text","This erases only the dedicated sbdata StandaloneBase storage volume. Current applets will be copied. The normal firmware and settings storage will not be erased. A restart will be required."))
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
    storage:stop()
    for _,t in pairs(self.backoff or {}) do t:stop() end
    for i=#SERVICES,1,-1 do stop(SERVICES[i]) end
    started=false; return true
end

return _M
