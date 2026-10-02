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

local function pidPath(s) return RUN.."/"..s.exe..".pid" end
local function alive(s)
    local f=io.open(pidPath(s),"r"); if not f then return false end
    local pid=tonumber(f:read("*l")); f:close()
    if not pid then return false end
    return os.execute("kill -0 "..tostring(pid).." >/dev/null 2>&1") == 0
end
local function launch(s)
    if alive(s) then return true end
    os.remove(pidPath(s))
    os.execute("chmod 755 "..ROOT.."/bin/"..s.exe.." >/dev/null 2>&1")
    local cmd="( "..ROOT.."/bin/"..s.exe.." "..s.args.." >>"..RUN.."/"..s.exe..".log 2>&1 & echo $! >"..pidPath(s).." )"
    return os.execute(cmd) == 0
end
local function stop(s)
    local f=io.open(pidPath(s),"r"); if not f then return end
    local pid=tonumber(f:read("*l")); f:close()
    if pid then os.execute("kill "..tostring(pid).." >/dev/null 2>&1") end
    os.remove(pidPath(s))
end
function init(self)
    if started then return end; started=true
    os.execute("mkdir -p "..RUN.." /mnt/storage/standalonebase")
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
    local window=Window("text_list","STANDALONE_BASE")
    local menu=SimpleMenu("menu")
    menu:addItem({text="LMS Server: 127.0.0.1"})
    for _,s in ipairs(SERVICES) do menu:addItem({text=s.name..": "..(alive(s) and "Running" or "Stopped")}) end
    menu:addItem({text="Local activation: Pending hardware validation"})
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
