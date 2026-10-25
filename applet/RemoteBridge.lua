local oo = require("loop.simple")
local Framework = require("jive.ui.Framework")
local Event = require("jive.ui.Event")
local Timer = require("jive.ui.Timer")
local SocketHttp = require("jive.net.SocketHttp")
local RequestHttp = require("jive.net.RequestHttp")
local Surface = require("jive.ui.Surface")
local json = require("json")
local io, os, tonumber, tostring, pcall, pairs = io, os, tonumber, tostring, pcall, pairs
local jnt = jnt
module(..., Framework.constants)
oo.class(_M)

local RUN = "/tmp/standalonebase"
local ACTIVE = RUN.."/remote-active"
local BMP = RUN.."/remote-screen.bmp"
local HEARTBEAT = RUN.."/remote-bridge-heartbeat"
local CAPABILITY = "/mnt/storage/standalonebase/remote.capability"
local SETTINGS = "/mnt/storage/standalonebase/settings.conf"
local injecting = false
local untrustedUntil = 0

local short = {
    home="go_home_or_now_playing", back="back", encoder_cw="down",
    encoder_ccw="up", encoder_press="go", play="play", pause="pause",
    volume_up="volume_up", volume_down="volume_down", power="power",
}
local hold = {
    home="go_home", back="go_home", encoder_press="add", play="create_mix",
    pause="stop", volume_up="volume_up", volume_down="volume_down", power="shutdown",
}
for i=1,6 do short["preset_"..i]="play_preset_"..i; hold["preset_"..i]="set_preset_"..i end

local function readOne(path)
    local f=io.open(path,"r"); if not f then return nil end
    local value=f:read("*l"); f:close(); return value
end
local function webPort()
    local f=io.open(SETTINGS,"r"); if not f then return 80 end
    for line in f:lines() do local p=line:match("^port=(%d+)$"); if p then f:close(); return tonumber(p) or 80 end end
    f:close(); return 80
end
local function dispatch(control,phase)
    if control=="encoder_cw" then Framework:pushEvent(Event:new(EVENT_SCROLL,1)); return end
    if control=="encoder_ccw" then Framework:pushEvent(Event:new(EVENT_SCROLL,-1)); return end
    local action=(phase=="long" and hold[control]) or short[control]
    if action then Framework:pushAction(action) end
end

function init(self)
    self.busy=false; self.bmpBusy=false; self.lastBmp=0
    -- SocketHttp is asynchronous. Keep it reachable for the lifetime of the
    -- bridge; a request-local socket may be collected before jnt services it.
    self.http=SocketHttp(jnt,"127.0.0.1",webPort(),"StandaloneBaseRemote")
    self.internalHttp=SocketHttp(jnt,"127.0.0.1",webPort(),"StandaloneBaseWebAccess")
    self.timer=Timer(250,function() self:tick() end); self.timer:start()
    return self
end
function webAccess(self,command,callback)
    local capability=readOne(CAPABILITY)
    if not capability or #capability<32 then callback(nil,"Internal capability unavailable"); return end
    -- This firmware's RequestHttp expects an LTN12 source for every POST.
    -- A `body` string is ignored and leaves the source nil, causing
    -- ltn12.pump.all() to assert in the network thread.
    local delivered=false
    local request=RequestHttp(function(data,err)
        -- RequestHttp follows LTN12 convention and calls its sink again with
        -- (nil, nil) after delivering the complete response body.
        if delivered or (not data and not err) then return end
        delivered=true; callback(data,err)
    end,"POST","/api/internal/web-access/"..command,{headers={Connection="close",["Content-Length"]="0",["X-Remote-Capability"]=capability},t_bodySource=function() return nil end})
    local ok=pcall(function() self.internalHttp:fetch(request) end)
    if not ok then callback(nil,"Internal request failed") end
end
function capture(self)
    if self.bmpBusy then return end
    self.bmpBusy=true
    local tmp=BMP..".tmp"
    local ok=pcall(function()
        local sw,sh=Framework:getScreenSize()
        local window=Framework.windowStack[1]
        local bg=Framework.getBackground()
        if not window or not bg then error("Jive screen is unavailable") end
        local srf=Surface:newRGB(sw,sh)
        bg:blit(srf,0,0,sw,sh)
        window:draw(srf,JIVE_LAYER_ALL)
        srf:saveBMP(tmp)
        if not os.rename(tmp,BMP) then error("cannot publish BMP") end
    end)
    if not ok then os.remove(tmp) end
    self.bmpBusy=false; self.lastBmp=Framework:getTicks()
end
function response(self,data,err)
    self.busy=false
    if err or not data then return end
    local heartbeat=io.open(HEARTBEAT,"w"); if heartbeat then heartbeat:write("ok\n"); heartbeat:close() end
    local wantBmp=false
    local protected=false
    for line in data:gmatch("[^\r\n]+") do
        local control,phase=line:match("^([@%w_]+)\t([%w_]+)")
        if control=="@bmp" then wantBmp=phase=="1"
        elseif control=="@protected" then protected=phase=="1"
        elseif control and phase and not self.firmwareLock then injecting=true; untrustedUntil=Framework:getTicks()+1000; dispatch(control,phase); injecting=false end
    end
    if protected then os.remove(BMP); os.remove(BMP..".tmp")
    elseif wantBmp and Framework:getTicks()-self.lastBmp>=750 then self:capture() end
end
function isInjecting(self) return injecting or Framework:getTicks()<untrustedUntil end
function firmware(self,command,payload,callback)
    local capability=readOne(CAPABILITY)
    if not capability then callback(nil,"Internal capability unavailable"); return end
    self.firmwareHttp=self.firmwareHttp or {}
    if self.firmwareHttp[command] then self.firmwareHttp[command]:close("Replaced firmware request") end
    local http=SocketHttp(jnt,"127.0.0.1",webPort(),"StandaloneBaseFirmware")
    self.firmwareHttp[command]=http
    local body=json.encode(payload)
    local sent,delivered=false,false
    local request=RequestHttp(function(data,err)
        if delivered or (not data and not err) then return end
        delivered=true; callback(data,err)
    end,"POST","/api/internal/firmware/"..command,{
        headers={Connection="close",["Content-Type"]="application/json",["Content-Length"]=tostring(#body),["X-Remote-Capability"]=capability},
        t_bodySource=function() if not sent then sent=true; return body end end})
    local ok=pcall(function() http:fetch(request) end)
    if not ok then callback(nil,"Internal firmware request failed") end
end
function tick(self)
    local active=io.open(ACTIVE,"r")
    if not active then return end
    active:close()
    if self.busy then return end
    local capability=readOne(CAPABILITY); if not capability or #capability<32 then return end
    local heartbeat=io.open(HEARTBEAT,"w"); if heartbeat then heartbeat:write("polling\n"); heartbeat:close() end
    self.busy=true
    local request=RequestHttp(function(data,err) self:response(data,err) end,"GET","/api/remote/actions",{headers={Connection="close",["X-Remote-Capability"]=capability}})
    local ok=pcall(function() self.http:fetch(request) end)
    if not ok then self.busy=false end
end
function stop(self)
    if self.timer then self.timer:stop(); self.timer=nil end
    for _, http in pairs(self.firmwareHttp or {}) do
        pcall(function() http:close("Standalone Base stopped") end)
    end
    self.firmwareHttp = nil
    self.http=nil; self.busy=false
    self.internalHttp=nil
    os.remove(BMP); os.remove(BMP..".tmp")
    os.remove(HEARTBEAT)
end
return _M
