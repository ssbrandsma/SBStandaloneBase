-- The live storage status must override a cached process failure flag.
local root = arg and arg[1] or "."
local f = assert(io.open(root .. "/applet/StandaloneBaseApplet.lua"))
local text = f:read("*a"); f:close()
local source = assert(text:match("(function storageMenu%b().-)function confirmStorageInitialization"))
ExtendedStorageState = assert(loadfile(root .. "/applet/ExtendedStorageState.lua"))()
local status, header, items, reinitialization, repair
storage = {check=function() return {STATUS=status, REINITIALIZATION_SUPPORTED=reinitialization, REPAIR_SUPPORTED=repair} end}
Window = function() return {addWidget=function() end,show=function() end,hide=function() end} end
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
status="AVAILABLE"; reinitialization=1; items={}; storageMenu(self)
assert(header:find("recovery stopped safely"))
assert(items[#items].text=="Erase and reinitialize Extended Storage")
local selected=false
self.confirmStorageErasure=function() selected=true end
items[#items].callback(); assert(selected)
storage.process=true; items={}; storageMenu(self)
assert(#items==1, "Do not offer erasure during recovery")
storage.process=nil; reinitialization=0; items={}; storageMenu(self)
assert(#items==1, "Do not offer erasure without server-side eligibility")

self.storageRecoveryFailed=false; repair=1; items={}
storageMenu(self)
assert(header:find("data is intact"))
assert(items[#items].text=="Repair startup integration (preserve data)")
local repaired=false
storage.startRepair=function(_,_,done) repaired=true; done(0,{STAGE="COMPLETE"}); return true end
self.storageMenu=function() end
self.remoteBridge={isInjecting=function() return true end}
items[#items].callback(); assert(not repaired)
self.remoteBridge={isInjecting=function() return false end}
items[#items].callback(); assert(repaired and not self.storageRecoveryFailed)
repair=0

-- Exercise the actual confirmation UI, including remote-input rejection.
local confirmation=assert(text:match("(function confirmStorageErasure%b().-)function startStorageInitialization"))
assert((loadstring or load)(confirmation))()
local hidden, started, injecting=false, false, false
Window=function() return {addWidget=function() end,show=function() end,
    hide=function() hidden=true end} end
self.remoteBridge={isInjecting=function() return injecting end}
self.startStorageInitialization=function(_, erase) started=erase end
reinitialization=1; items={}; confirmStorageErasure(self)
assert(header:find("Permanently erase ALL data"))
assert(items[1].text=="Cancel")
items[1].callback(); assert(hidden and not started)
injecting=true; items[2].callback(); assert(not started)
injecting=false; items[2].callback(); assert(started)
items={}; injecting=true; confirmStorageErasure(self)
assert(#items==0, "Remote input must not open the destructive confirmation")
injecting=false; reinitialization=0; confirmStorageErasure(self)
assert(#items==0, "Recheck eligibility before showing confirmation")
print("recovery-menu-tests-ok")
