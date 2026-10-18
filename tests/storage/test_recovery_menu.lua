-- The live storage status must override a cached process failure flag.
local root = arg and arg[1] or "."
local f = assert(io.open(root .. "/applet/StandaloneBaseApplet.lua"))
local text = f:read("*a"); f:close()
local source = assert(text:match("(function storageMenu%b().-)function confirmStorageInitialization"))
ExtendedStorageState = assert(loadfile(root .. "/applet/ExtendedStorageState.lua"))()
local status, header, items
storage = {check=function() return {STATUS=status} end}
Window = function() return {addWidget=function() end,show=function() end} end
SimpleMenu = function() return {addItem=function(_, item) items[#items+1]=item end,
    setHeaderWidget=function(_, value) header=value end} end
Textarea = function(_, text) return text end
appletManager = {callService=function() end}
assert((loadstring or load)(source))()
local self = {storageRecoveryFailed=true}
status="REBOOT_REQUIRED"; items={}; storageMenu(self)
assert(header:find("Restart the Radio"), "Stale failure must not hide reboot instruction")
assert(items[2].text=="Restart Now")
status="ACTIVE"; items={}; header=nil; storageMenu(self)
assert(header==nil and items[2].text:find("Capacity"), "Active status must not show failure")
status="ERROR"; items={}; storageMenu(self)
assert(header:find("recovery stopped safely"), "Real errors must remain visible")
print("recovery-menu-tests-ok")
