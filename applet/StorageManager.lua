local Process = require("jive.net.Process")
local Timer = require("jive.ui.Timer")
local io, tonumber, tostring = io, tonumber, tostring
local setmetatable = setmetatable
local jnt = jnt

local Manager = {}
Manager.__index = Manager

local function parseStream(stream)
    local result = {}
    for line in stream:lines() do
        local key, value = line:match("^([%w_]+)=(.*)$")
        if key then result[key] = tonumber(value) or value end
    end
    return result
end

local function parseFile(path)
    local stream = io.open(path, "r")
    if not stream then return {} end
    local result = parseStream(stream)
    stream:close()
    return result
end

function Manager:new(root)
    return setmetatable({ root = root, statusPath = "/tmp/sbstorage.status",
        logPath = "/tmp/sbstorage.log" }, self)
end

function Manager:_runHelper(command)
    local p = io.popen(self.root .. "/sb-storage-helper " .. command .. " 2>/dev/null", "r")
    if not p then return { compatible = "false", reason = "helper_unavailable" } end
    local result = parseStream(p)
    p:close()
    return result
end

function Manager:check()
    local p = io.popen("/bin/sh " .. self.root .. "/storage-setup.sh check 2>/dev/null", "r")
    if not p then return { STATUS = "ERROR", ERROR = "setup_helper_unavailable" } end
    local result = parseStream(p)
    p:close()
    if not result.STATUS then result.STATUS = "ERROR"; result.ERROR = "invalid_status_output" end
    return result
end

function Manager:startInitialization(progressCallback, doneCallback)
    if self.process then return false, "already_running" end
    local command = "/bin/sh " .. self.root .. "/storage-setup.sh initialize " ..
        self.root .. "/sbubifs-authorized.ko 2>&1; rc=$?; echo PROCESS_EXIT_CODE=$rc"
    local output, finished = "", false
    self.process = Process(jnt, command)
    self.pollTimer = Timer(500, function()
        if progressCallback then progressCallback(parseFile(self.statusPath)) end
    end)
    self.pollTimer:start()
    self.process:read(function(chunk, err)
        if finished then return end
        if err then output = output .. tostring(err)
        elseif chunk then output = output .. tostring(chunk)
        else
            finished = true
            if self.pollTimer then self.pollTimer:stop(); self.pollTimer = nil end
            self.process = nil
            local exitCode = tonumber(output:match("PROCESS_EXIT_CODE=(%d+)")) or 1
            local status = parseFile(self.statusPath)
            status.EXIT_CODE = tonumber(status.EXIT_CODE) or exitCode
            if progressCallback then progressCallback(status) end
            if doneCallback then doneCallback(exitCode, status, output) end
        end
    end)
    return true
end

function Manager:stop()
    if self.pollTimer then self.pollTimer:stop(); self.pollTimer = nil end
    self.process = nil
end

function Manager:getStorageInfo() return self:_runHelper("info") end
function Manager:getStorageStatus() return self:_runHelper("storage-status") end
function Manager:getStoragePreparation() return self:_runHelper("prepare") end
function Manager:verifyStorage() return self:_runHelper("verify") end
function Manager:getCompatibility()
    local i = self:getStorageInfo()
    return { supported = i.compatible == "true", reason = i.reason or "unknown" }
end
function Manager:getExpansionOptions()
    return { enabled = false, targetBytes = 0, targetLebs = 0,
        state = "replaced_by_extended_storage", reason = "Use Extended Storage" }
end
function Manager:getExpansionStatus() return self:getExpansionOptions() end

return Manager
