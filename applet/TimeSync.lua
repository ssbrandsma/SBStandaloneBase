local Timer = require("jive.ui.Timer")
local SocketUdp = require("jive.net.SocketUdp")
local Process = require("jive.net.Process")
local Resolver = require("applets.StandaloneBase.TimeResolver")
local string = require("string")
local setmetatable = setmetatable
local tostring = tostring
local math = require("math")
local os = require("os")
local table = require("table")
local log = require("jive.utils.log").logger("StandaloneBase.time")
local jnt = jnt

module(...)

local NTP_PORT = 123
local TIMEOUT_MS = 5000
local START_DELAY_MS = 10000
local RETRY_DELAYS = { 60000, 300000, 900000, 3600000 }
local RESYNC_MS = 86400000
local SERVERS = { "time.google.com", "pool.ntp.org", "time.cloudflare.com" }

-- Keep literals signed 32-bit safe on the Radio's Lua 5.1 runtime.
local NTP_EPOCH_HIGH = 2000000000
local NTP_EPOCH_LOW = 208988800

local TimeSync = {}
TimeSync.__index = TimeSync

local function uint32(data, offset)
    local b1, b2 = string.byte(data, offset), string.byte(data, offset + 1)
    local b3, b4 = string.byte(data, offset + 2), string.byte(data, offset + 3)
    if not b4 then return nil end
    return b1 * 16777216 + b2 * 65536 + b3 * 256 + b4
end

local function printableRefid(data)
    local out, printable = {}, true
    for i = 13, 16 do
        local b = string.byte(data, i)
        if not b then return "" end
        if b < 32 or b > 126 then printable = false else out[#out + 1] = string.char(b) end
    end
    if printable then return table.concat(out) end
    return ""
end

local function utcString(seconds)
    local t = os.date("!*t", seconds)
    if not t then return nil end
    return string.format("%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour, t.min, t.sec)
end

local function closeSocket(self)
    if self.socket then self.socket:close(); self.socket = nil end
end

local function stopTimer(self)
    if self.timer then self.timer:stop(); self.timer = nil end
end

local function schedule(self, delay, callback)
    if not self.active then return end
    if self.nextTimer then self.nextTimer:stop() end
    self.nextTimer = Timer(delay, function()
        self.nextTimer = nil
        if self.active then callback() end
    end, true)
    self.nextTimer:start()
end

local function beginNextServer(self)
    if not self.active then return end
    self.serverIndex = self.serverIndex + 1
    if self.serverIndex > #SERVERS then
        self.retryIndex = math.min(self.retryIndex + 1, #RETRY_DELAYS)
        local delay = RETRY_DELAYS[self.retryIndex]
        log:warn("StandaloneBase time: all servers failed; retry in " .. tostring(delay) .. " ms")
        schedule(self, delay, function() self.serverIndex = 1; self:nextRequest() end)
    else
        schedule(self, 100, function() self:nextRequest() end)
    end
end

local function failAttempt(self, reason)
    if not self.active then return end
    log:error("StandaloneBase time: server " .. tostring(self.server) .. " failed: " .. reason)
    closeSocket(self)
    stopTimer(self)
    beginNextServer(self)
end

local function runCommand(self, command, marker, callback)
    if not self.active then return end
    local output = ""
    local process = Process(jnt, command)
    self.process = process
    local finished = false
    local guard = Timer(10000, function()
        if not finished then
            finished = true
            log:error("StandaloneBase time: command timeout: " .. command)
            self.process = nil
            if self.active then callback(false, output) end
        end
    end, true)
    self.commandTimer = guard
    guard:start()
    process:read(function(chunk, err)
        if finished then return end
        if err then output = output .. tostring(err)
        elseif chunk then output = output .. tostring(chunk)
        else
            finished = true
            guard:stop()
            self.commandTimer = nil
            self.process = nil
            if self.active then callback(string.find(output, marker, 1, true) ~= nil, output) end
        end
    end)
end

local function writeRtc(self, utc)
    local command = "hwclock -w -u >/dev/null 2>&1; rc=$?; echo STANDALONEBASE_HWCLOCK_RC:$rc"
    runCommand(self, command, "STANDALONEBASE_HWCLOCK_RC:0", function(ok, output)
        if not ok then
            log:error("StandaloneBase time: hwclock -w -u failed: " .. output)
            beginNextServer(self)
            return
        end
        log:warn("StandaloneBase time: Linux clock and RTC synchronized from NTP UTC = " .. utc)
        self.retryIndex = 0
        schedule(self, RESYNC_MS, function() self.serverIndex = 1; self:nextRequest() end)
    end)
end

local function writeClock(self, unix)
    local utc = utcString(unix)
    if not utc then return failAttempt(self, "cannot format packet timestamp") end
    log:warn("StandaloneBase time: setting Linux UTC from packet: " .. utc)
    local command = "date -u -s \"" .. utc .. "\" >/dev/null 2>&1; rc=$?; echo STANDALONEBASE_DATE_RC:$rc"
    runCommand(self, command, "STANDALONEBASE_DATE_RC:0", function(ok, output)
        if not ok then
            log:error("StandaloneBase time: date -u -s failed: " .. output)
            beginNextServer(self)
            return
        end
        writeRtc(self, utc)
    end)
end

local function responseSink(self, chunk, err)
    if not self.active then return end
    if err then return failAttempt(self, "UDP receive error: " .. tostring(err)) end
    if not chunk or not chunk.data then return end
    local data, length = chunk.data, string.len(chunk.data)
    if length < 48 then return failAttempt(self, "short packet") end

    local first = string.byte(data, 1)
    local li = math.floor(first / 64) % 4
    local version = math.floor(first / 8) % 8
    local mode = first % 8
    local stratum = string.byte(data, 2)
    local refid = printableRefid(data)
    local txTs = uint32(data, 41)
    if stratum == 0 then log:warn("StandaloneBase time: stratum=0 reference/Kiss code=\"" .. refid .. "\"") end
    if li == 3 or mode ~= 4 or version < 3 or version > 4 or stratum < 1 or stratum > 15 then
        return failAttempt(self, "invalid NTP response")
    end
    if not txTs or txTs == 0 then return failAttempt(self, "zero transmit timestamp") end
    local unix = txTs - NTP_EPOCH_HIGH - NTP_EPOCH_LOW
    if unix < 1500000000 or unix > 2000000000 then return failAttempt(self, "implausible transmit timestamp") end
    log:warn("StandaloneBase time: valid SNTP response from " .. tostring(self.server) .. ", UTC = " .. utcString(unix))
    closeSocket(self)
    stopTimer(self)
    writeClock(self, unix)
end

local function sendRequest(self, ip)
    if not self.active then return end
    self.socket = SocketUdp(jnt, function(chunk, err) responseSink(self, chunk, err) end)
    if not self.socket or not self.socket.t_sock then return failAttempt(self, "unable to create UDP socket") end
    local packet = string.char(0x23) .. string.rep(string.char(0), 47)
    if string.len(packet) ~= 48 or string.byte(packet, 1) ~= 0x23 then
        return failAttempt(self, "internal request validation failed")
    end
    self.socket:send(function() return packet end, ip, NTP_PORT)
    log:warn("StandaloneBase time: SNTP request sent to " .. tostring(self.server) .. " (" .. tostring(ip) .. "):123")
    self.timer = Timer(TIMEOUT_MS, function() failAttempt(self, "timeout") end, true)
    self.timer:start()
end

local function resolveAndSend(self)
    local server = self.server
    self.resolver:resolve(server, function(ip, err)
        if not self.active then return end
        if not ip then return failAttempt(self, "DNS failed: " .. tostring(err)) end
        sendRequest(self, ip)
    end)
end

function TimeSync:nextRequest()
    if not self.active then return end
    self.server = SERVERS[self.serverIndex]
    log:warn("StandaloneBase time: starting request for " .. tostring(self.server))
    resolveAndSend(self)
end

function TimeSync:start()
    if self.active then return end
    self.active = true
    self.serverIndex = 0
    self.retryIndex = 0
    log:warn("StandaloneBase time: background synchronization scheduled")
    self.startTimer = Timer(START_DELAY_MS, function()
        self.startTimer = nil
        if self.active then self.serverIndex = 1; self:nextRequest() end
    end, true)
    self.startTimer:start()
end

function TimeSync:stop()
    self.active = false
    if self.startTimer then self.startTimer:stop(); self.startTimer = nil end
    if self.nextTimer then self.nextTimer:stop(); self.nextTimer = nil end
    if self.commandTimer then self.commandTimer:stop(); self.commandTimer = nil end
    stopTimer(self)
    closeSocket(self)
    self.process = nil
end

function new()
    return setmetatable({ active = false, resolver = Resolver.new({ log = log }) }, TimeSync)
end

return _M
