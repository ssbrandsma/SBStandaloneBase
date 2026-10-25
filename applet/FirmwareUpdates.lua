-- Physical opt-in and optional native UI handoff. No flashing implementation.
local Framework = require("jive.ui.Framework")
local Window = require("jive.ui.Window")
local SimpleMenu = require("jive.ui.SimpleMenu")
local Textarea = require("jive.ui.Textarea")
local Checkbox = require("jive.ui.Checkbox")
local System = require("jive.System")
local Timer = require("jive.ui.Timer")
local json = require("json")
local appletManager = appletManager
local pcall, tostring, tonumber, ipairs, type, setmetatable, math = pcall, tostring, tonumber, ipairs, type, setmetatable, math
local M = {}
M.__index = M

-- Only the hardware KEY_GO event can mutate permission or review firmware.
-- Virtual Radio injects EVENT_ACTION or EVENT_SCROLL, never EVENT_KEY_PRESS.
function M:physicalKey(event, window, menu)
    if Framework.windowStack[1] ~= window or event:getKeycode() ~= jive.ui.KEY_GO then
        return jive.ui.EVENT_UNUSED
    end
    if self.bridge:isInjecting() then return jive.ui.EVENT_CONSUME end
    -- Menu:getSelectedItem() returns a rendered widget, not SimpleMenu data.
    local index = menu:getSelectedIndex()
    local item = index and menu:getItem(index)
    if item and item.physicalCallback then item.physicalCallback() end
    return jive.ui.EVENT_CONSUME
end

function M:lock()
    self.bridge.firmwareLock = self.applet:getSettings().customFirmwareUpdates == true
        or self.reviewing == true or self.window ~= nil
end

function M:sync(force)
    self:lock()
    if self.pending and not force and Framework:getTicks() < self.pendingDeadline then return end
    self.generation = (self.generation or 0) + 1
    local generation = self.generation
    self.sequence = math.max((self.sequence or 0) + 1, Framework:getTicks() * 1000)
    local machine, board = System:getMachine()
    local payload = {
        machine = machine or "unknown", board = tonumber(board) or 0,
        installed = jive.JIVE_VERSION or "unknown",
        enabled = self.applet:getSettings().customFirmwareUpdates == true,
        physicalSupported = jive.ui.EVENT_KEY_PRESS ~= nil and jive.ui.KEY_GO ~= nil,
        sequence = self.sequence,
    }
    self.pending = true
    self.pendingDeadline = Framework:getTicks() + 15000
    self.bridge:firmware("permission", payload, function(data, err)
        if generation ~= self.generation then return end
        self.pending = false
        if err or not data then
            self.status = nil
            self.error = "Firmware service unavailable"
            self:refreshMenu()
            if self.reviewWindow and not self:installing() then self.reviewWindow:hide() end
            return
        end
        local ok, status = pcall(json.decode, data)
        if ok and type(status) == "table" then self.status = status; self.error = status.error
        else self.status = nil; self.error = "Invalid firmware status" end
        self:refreshMenu()
        if self.reviewWindow and (not self.status or not self.status.ready
            or self.status.sha256 ~= self.reviewDigest) and not self:installing() then
            self.reviewWindow:hide()
        end
    end)
end

-- Update eligibility in the open menu after asynchronous permission replies.
-- Keep the same row objects so polling does not reset selection or scrolling.
function M:refreshMenu()
    local menu = self.menuWidget
    if not menu then return end
    local status = self.status or {}
    self.header:setValue("Use the physical knob to enable or review. " ..
        "Virtual Radio controls are disabled while this option is enabled.\nInstalled: " ..
        tostring(status.installed or jive.JIVE_VERSION) .. "\nAvailable: " ..
        tostring(status.available or "Not checked") .. "\n" .. tostring(self.error or status.state or ""))
    local ready = status.ready == true
    if ready and not self.reviewItem then
        self.reviewItem = {text = "Review Update", callback = function() end,
            physicalCallback = function() self:review() end}
        menu:insertItem(self.reviewItem, menu:getIndex(self.laterItem))
    elseif not ready and self.reviewItem then
        menu:removeItem(self.reviewItem)
        self.reviewItem = nil
    end
end

function M:installing()
    if not self.reviewing then return false end
    local ok, updater = pcall(function() return appletManager:loadApplet("SetupFirmwareUpgrade") end)
    return ok and updater and updater.updating == true
end

function M:busy()
    return self.reviewing == true
end

function M:review()
    -- This method is reached only from the raw-key listener below.
    if self.stopped or self.reviewing or not self.status or not self.status.ready
        or self.bridge:isInjecting() then return end
    self.reviewing = true
    self.reviewDigest = self.status.sha256
    self:lock()
    self.bridge:firmware("review", {}, function(data, err)
        if self.stopped then return end
        local ok, result = pcall(json.decode, data or "")
        if err or not ok or not result.firmwareUrl then
            self.reviewing = false; self.error = "Firmware eligibility changed; check again"; self:lock(); return
        end
        local url = result.firmwareUrl
        -- Public service, false force flag, versionless alias. No private updater
        -- method is invoked and no synthetic Begin Update action is generated.
        local server = {
            getUpgradeUrl = function() return url, false end,
            getName = function() return "Standalone Base" end,
        }
        local called, window = pcall(function()
            return appletManager:callService("firmwareUpgrade", server)
        end)
        -- Stock _upgradeWindow shows its window but returns nil. Observe the
        -- newly shown public window stack rather than calling private methods.
        if called and not window and Framework.windowStack[1] ~= self.window then
            window = Framework.windowStack[1]
        end
        if called and window then self.reviewWindow = window
        else self.reviewing = false; self.error = "Native optional firmware UI unavailable"; self:lock() end
    end)
end

function M:menu()
    if self.window then self.window:show(); return end
    local window = Window("text_list", "Custom Firmware Updates")
    local menu = SimpleMenu("menu")
    self.window = window
    self:lock()
    local checkbox
    local function repaintCheckbox() checkbox:setSelected(self.applet:getSettings().customFirmwareUpdates == true) end
    checkbox = Checkbox("checkbox", repaintCheckbox, self.applet:getSettings().customFirmwareUpdates == true)
    menu:addItem({text = "Enable custom firmware updates", style = "item_choice", check = checkbox,
        callback = repaintCheckbox,
        physicalCallback = function()
            local settings = self.applet:getSettings()
            settings.customFirmwareUpdates = not (settings.customFirmwareUpdates == true)
            self.applet:storeSettings()
            repaintCheckbox()
            self:sync(true)
        end})
    self.menuWidget = menu
    self.header = Textarea("help_text", "")
    menu:setHeaderWidget(self.header)
    self.laterItem = {text = "Later", callback = function() window:hide() end,
        physicalCallback = function() window:hide() end}
    menu:addItem(self.laterItem)
    self:refreshMenu()
    window:addWidget(menu)
    -- Framework's stock input mapper is priority -1. A smaller absolute
    -- priority intercepts a real KEY_GO before it becomes an anonymous action.
    local listener = Framework:addListener(jive.ui.EVENT_KEY_PRESS, function(event)
        return self:physicalKey(event, window, menu)
    end, -0.01)
    window:addListener(jive.ui.EVENT_WINDOW_POP, function()
        Framework:removeListener(listener)
        self.window = nil
        self.menuWidget = nil
        self.header = nil
        self.reviewItem = nil
        self.laterItem = nil
        self:lock()
        return jive.ui.EVENT_UNUSED
    end)
    window:show()
    self:sync(true)
end

function M:start()
    self:sync()
    self.timer = Timer(10000, function()
        if self.reviewWindow and not self:installing() then
            local found = false
            for _, window in ipairs(Framework.windowStack) do if window == self.reviewWindow then found = true end end
            if not found then self.reviewWindow = nil; self.reviewing = false end
        end
        self:sync()
    end)
    self.timer:start()
end

function M:stop()
    self.stopped = true
    self.generation = (self.generation or 0) + 1
    if self.timer then self.timer:stop() end
    -- No new permission heartbeat: native authorization expires fail-closed.
end

function M.new(applet, bridge)
    return setmetatable({applet = applet, bridge = bridge}, M)
end

return M
