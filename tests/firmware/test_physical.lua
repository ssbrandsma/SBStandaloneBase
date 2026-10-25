-- Host-only trusted-input tests. No native updater or device is involved.
local framework = {windowStack = {}, getTicks = function() return 100 end}
package.preload['jive.ui.Framework'] = function() return framework end
for _, name in ipairs({'Window', 'SimpleMenu', 'Textarea', 'Checkbox', 'Timer'}) do
    package.preload['jive.ui.' .. name] = function() return {} end
end
package.preload['jive.System'] = function()
    return {getMachine = function() return 'baby', 5 end}
end
package.preload.json = function() return {} end
jive = {JIVE_VERSION = '9.0.2 r17146', ui = {
    KEY_GO = 1, EVENT_UNUSED = 0, EVENT_CONSUME = 1, EVENT_KEY_PRESS = 2}}
local Controller = dofile(arg[1] .. '/applet/FirmwareUpdates.lua')
local settings, calls = {customFirmwareUpdates = false}, {}
local bridge = {isInjecting = function(self) return self.injecting end,
    firmware = function(self, command, payload, callback)
        calls[#calls + 1] = {command = command, payload = payload}
    end}
local applet = {getSettings = function() return settings end}
local controller = Controller.new(applet, bridge)
controller:sync()
assert(calls[1].payload.enabled == false)
assert(calls[1].payload.physicalSupported == true)
settings.customFirmwareUpdates = true
controller:sync(true)
assert(bridge.firmwareLock and calls[2].payload.enabled)
assert(calls[2].payload.sequence > calls[1].payload.sequence)
local count, window = 0, {}
local menu = {getSelectedItem = function() return {} end,
getSelectedIndex = function() return 1 end,
getItem = function(self, index)
    assert(index == 1)
    return {physicalCallback = function() count = count + 1 end}
end}
local event = {getKeycode = function() return jive.ui.KEY_GO end}
assert(controller:physicalKey(event, window, menu) == jive.ui.EVENT_UNUSED)
framework.windowStack = {window}
bridge.injecting = true
controller:physicalKey(event, window, menu)
assert(count == 0)
bridge.injecting = false
controller:physicalKey(event, window, menu)
assert(count == 1)
event.getKeycode = function() return 99 end
controller:physicalKey(event, window, menu)
assert(count == 1)
settings.customFirmwareUpdates = false
controller:sync(true)
assert(not bridge.firmwareLock and not calls[3].payload.enabled)
assert(calls[3].payload.sequence > calls[2].payload.sequence)
-- Jive skins show the check widget only for an item_choice row.
local rows, checkbox = {}, nil
local uiWindow = {addWidget = function() end, addListener = function() end,
    show = function(self) framework.windowStack = {self} end,
    hide = function(self) self.hidden = true end}
setmetatable(package.loaded['jive.ui.Window'], {__call = function() return uiWindow end})
setmetatable(package.loaded['jive.ui.SimpleMenu'], {__call = function()
    return {addItem = function(self, item) rows[#rows + 1] = item end,
            getIndex = function(self, item)
                for i, row in ipairs(rows) do if row == item then return i end end
            end,
            insertItem = function(self, item, index) table.insert(rows, index, item) end,
            removeItem = function(self, item)
                for i, row in ipairs(rows) do
                    if row == item then table.remove(rows, i); return end
                end
            end,
            setHeaderWidget = function() end}
end})
setmetatable(package.loaded['jive.ui.Textarea'], {__call = function()
    return {setValue = function(self, value) self.value = value end}
end})
setmetatable(package.loaded['jive.ui.Checkbox'], {__call = function(_, style, callback, selected)
    checkbox = {selected = selected, setSelected = function(self, value) self.selected = value end}
    return checkbox
end})
framework.addListener = function() return {} end
applet.storeSettings = function() end
controller:menu()
assert(rows[1].style == 'item_choice' and rows[1].check == checkbox)
assert(not checkbox.selected)
rows[1].callback()
assert(not settings.customFirmwareUpdates)
rows[1].physicalCallback()
assert(settings.customFirmwareUpdates and checkbox.selected)
rows[1].physicalCallback()
assert(not settings.customFirmwareUpdates and not checkbox.selected)
-- Eligibility arriving after the window opens must add/remove Review live.
controller.status = {ready = true, available = '9.0.2 r17157'}
controller:refreshMenu()
assert(#rows == 3 and rows[2].text == 'Review Update')
assert(controller.header.value:find('17157', 1, true))
local review = rows[2]
controller:refreshMenu()
assert(#rows == 3 and rows[2] == review)
local reviews = 0
controller.review = function() reviews = reviews + 1 end
review.callback()
assert(reviews == 0)
review.physicalCallback()
assert(reviews == 1)
controller.status = nil
controller:refreshMenu()
assert(#rows == 2 and rows[2].text == 'Later')
rows[2].physicalCallback()
assert(uiWindow.hidden)
print('Firmware physical input and permission sequencing tests passed')
