local oo = require("loop.simple")
local Applet = require("jive.Applet")
local Framework = require("jive.ui.Framework")
local SimpleMenu = require("jive.ui.SimpleMenu")
local Window = require("jive.ui.Window")
local Timer = require("jive.ui.Timer")
local io, os, tonumber, tostring = io, os, tonumber, tostring
local ipairs, pairs, math = ipairs, pairs, math
module(..., Framework.constants)
oo.class(_M, Applet)

local ROOT = "/usr/share/jive/applets/StandaloneBase"
local RUN = "/tmp/standalonebase"
local SERVICES = {
    { name="Bootstrap service", exe="sbbase", args="--config /mnt/storage/standalonebase/config.json" },
    { name="Webserver", exe="sbwebserver", args="--web-root "..ROOT.."/web --config-dir /mnt/storage/standalonebase" },
    { name="HTTPS Proxy", exe="sbproxy", args="--listen 127.0.0.1:8765" },
}
local started = false
local BOOTSTRAP_IP_PATTERN = "49%.12%.198%.91"
local LOCAL_IP = "127.0.0.1"
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
    local exe=ROOT.."/bin/"..s.exe
    return os.execute("test -r /proc/"..tostring(pid).."/cmdline && grep -q '^"..exe.."' /proc/"..tostring(pid).."/cmdline 2>/dev/null") == 0
end
local function launch(s)
    if alive(s) then return true end
    os.remove(pidPath(s))
    os.execute("chmod 755 "..ROOT.."/bin/"..s.exe.." >/dev/null 2>&1")
    local cmd="( "..ROOT.."/bin/"..s.exe.." "..s.args.." >>"..RUN.."/"..s.exe..".log 2>&1 & pid=$!; "..
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
function init(self)
    if started then return end; started=true
    os.execute("mkdir -p "..RUN.." /mnt/storage/standalonebase")
    redirectBootstrapServer()
    self.failures={}; self.backoff={}; self:startServices()
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
    menu:addItem({text="LMS Server: 127.0.0.1"})
    for _,s in ipairs(SERVICES) do menu:addItem({text=s.name..": "..(alive(s) and "Running" or "Stopped")}) end
    menu:addItem({text="Version: 0.1.0"})
    window:addWidget(menu); window:show()
end
function free(self)
    if self.monitor then self.monitor:stop() end
    for _,t in pairs(self.backoff or {}) do t:stop() end
    for i=#SERVICES,1,-1 do stop(SERVICES[i]) end
    started=false; return true
end

return _M
