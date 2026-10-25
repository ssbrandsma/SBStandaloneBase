local root = arg[1]
local nodes, bytes, closed = {}, {}, 0
package.preload.lfs = function() return {} end
package.preload["jive.utils.log"] = function()
    return {logger=function() return {info=function() end,warn=function() end} end}
end
local compat = dofile(root .. "/applet/InstallerCompat.lua")
local active = "/usr/share/jive/applets"
local mounted = true
local mountKind, mountDevice = "sbubifs", "ubi0:sbdata"
local fs = {
    attributes=function(path, key)
        local mode=nodes[path]
        if key then return mode end
        return mode and {mode=mode} or nil
    end,
    mkdir=function(path) nodes[path]="directory"; return true end,
}
fs.symlinkattributes=fs.attributes
local files = {open=function(path, mode)
    if path=="/proc/mounts" then
        return {close=function() end,lines=function()
            local used=false
            return function()
                if used then return nil end
                used=true
                return mounted and (mountDevice.." /usr/share/jive/applets "..mountKind.." rw 0 0") or "none / unionfs rw 0 0"
            end
        end}
    end
    assert(mode=="wb")
    bytes[path]=""; nodes[path]="file"
    return {write=function(self,data) bytes[path]=bytes[path]..data; return self end,close=function() closed=closed+1; return true end}
end}
local seen={}
local installer={
    appletInstallerMenu=function(self) self.appletdir="/mnt/storage/usr/share/jive/applets"; return "window" end,
    _download=function(self) seen.download=self.appletdir; return "download" end,
    _remove=function(self) seen.remove=self.appletdir; return "remove" end,
    _zipSink=function() error("stock sink must be replaced") end,
}
nodes[active.."/SetupAppletInstaller"]="directory"
assert(compat.apply(installer,{fs=fs,io=files}))
local wrapper=installer._download
assert(compat.apply(installer,{fs=fs,io=files}) and installer._download==wrapper)
local instance=setmetatable({}, {__index=installer})
assert(instance:appletInstallerMenu()=="window" and instance.appletdir==active)
instance.appletdir="/mnt/storage/usr/share/jive/applets"
assert(instance:_download()=="download" and seen.download==active)
instance.appletdir="/mnt/storage/usr/share/jive/applets"
assert(instance:_remove()=="remove" and seen.remove==active)
mountKind="ubifs"
assert(instance:appletInstallerMenu()=="window" and instance.appletdir==active)
mountDevice="ubi0:ubifs"
instance:appletInstallerMenu()
assert(instance.appletdir~="/usr/share/jive/applets", "Do not route to the wrong UBIFS volume")
mountDevice="ubi0:sbdata"; mountKind="sbubifs"
mounted=false
instance:_download(); assert(seen.download=="/mnt/storage/usr/share/jive/applets")
instance:appletInstallerMenu();instance:_download();instance:_remove()
assert(seen.download=="/mnt/storage/usr/share/jive/applets" and seen.remove==seen.download)
mounted=true
local function extract(entries)
    instance.fetched=false
    local sink=instance:_zipSink(active.."/StandaloneRadio/")
    for _,entry in ipairs(entries) do sink({filename=entry[1]}); if entry[2] then sink(entry[2]) end end
    sink(nil)
    assert(instance.fetched)
end
extract({{"images/"},{"images/radio.png","PNG\0data"},{"StandaloneRadioMeta.lua","lua"}})
assert(bytes[active.."/StandaloneRadio/images/radio.png"]=="PNG\0data")
assert(bytes[active.."/StandaloneRadio/StandaloneRadioMeta.lua"]=="lua")
extract({{"images\\radio.png","legacy PNG"},{"nested/deep/file.txt","nested"}})
assert(bytes[active.."/StandaloneRadio/images/radio.png"]=="legacy PNG")
assert(bytes[active.."/StandaloneRadio/nested/deep/file.txt"]=="nested")
extract({{"empty/"}}) -- Directory-only EOF must complete, without closing a string sentinel.
for _,bad in ipairs({"../outside", "images/../../outside", "/absolute", "C:\\outside", "images//file", "images/./file", "a\0b"}) do
    local sink=instance:_zipSink(active.."/StandaloneRadio/")
    assert(not pcall(sink,{filename=bad}),bad)
end
nodes[active.."/StandaloneRadio/link"]="link"
assert(not pcall(instance:_zipSink(active.."/StandaloneRadio/"),{filename="link/escape"}))
assert(closed>=4)
-- The release check can replay real ZIP members, including legacy archives,
-- through the same streaming sink without depending on a host zipfilter DLL.
for _,entries in ipairs(ZIP_FIXTURE_ARCHIVES or {}) do
    extract(entries)
    for _,entry in ipairs(entries) do
        if entry[2] then
            local name=entry[1]:gsub("\\", "/")
            assert(bytes[active.."/StandaloneRadio/"..name]==entry[2],name)
        end
    end
end
print("installer active/fallback paths, updates/removal, streamed ZIP, legacy separators and directory EOF tests passed")

-- Match Jive's lifecycle: loadApplet executes the stock module again, even
-- after require has loaded it. Patch the managed object, not that early module.
local lifecycleClass = {
    appletInstallerMenu=function() end,
    _download=function() end,
    _remove=function() end,
    _zipSink=function() error("early module sink") end,
}
local managed = setmetatable({}, {__index=lifecycleClass})
local loads=0
local manager={loadApplet=function(_, name)
    assert(name=="SetupAppletInstaller")
    loads=loads+1
    if loads==1 then lifecycleClass._zipSink=function() error("reloaded stock sink") end end
    return managed
end}
assert(compat.apply(nil,{appletManager=manager,fs=fs,io=files}))
local managedSink=managed._zipSink
assert(rawget(managed,"_zipSink")==managedSink)
assert(compat.apply(nil,{appletManager=manager,fs=fs,io=files}))
assert(managed._zipSink==managedSink)
local sink=managed:_zipSink(active.."/StandaloneRadio/")
sink({filename="images/"});sink({filename="images/radio.png"});sink("image");sink(nil)
assert(managed.fetched and bytes[active.."/StandaloneRadio/images/radio.png"]=="image")
print("managed AppletManager load and instance patch lifecycle tests passed")

-- Jive frees an applet when its menu closes, then reloads its module and
-- constructs another object next time. Verify the permanent manager hook.
local current
local other={}
local reloads=0
local reloader={loadApplet=function(_,name)
    if name~="SetupAppletInstaller" then return other end
    if not current then
        reloads=reloads+1
        local class={appletInstallerMenu=function(self) self.appletdir="/mnt/storage/usr/share/jive/applets" end,
            _download=function() end,_remove=function() end,
            _zipSink=function() error("unpatched stock sink after reload") end}
        current=setmetatable({}, {__index=class})
    end
    return current
end}
assert(compat.apply(nil,{appletManager=reloader,fs=fs,io=files}))
local loader=reloader.loadApplet
for i=1,3 do
    current=nil -- AppletManager.freeApplet clears both the class and instance.
    local nextInstance=reloader:loadApplet("SetupAppletInstaller")
    nextInstance:appletInstallerMenu()
    assert(nextInstance.appletdir==active)
    local nextSink=nextInstance:_zipSink(active.."/StandaloneRadio/")
    nextSink({filename="images/"});nextSink({filename="images/radio.png"});nextSink("reopened");nextSink(nil)
    assert(nextInstance.fetched)
end
assert(reloads==4 and reloader:loadApplet("OtherApplet")==other)
assert(compat.apply(nil,{appletManager=reloader,fs=fs,io=files}) and reloader.loadApplet==loader)
print("installer close/free/reload/reopen lifecycle tests passed")

local shown,finished,stored=false,false,false
local broken={appletInstallerMenu=function() end,_remove=function() end,
    _zipSink=function() end,_finished=function() finished=true;stored=true end,
    _download=function(self)
        local sink=self:_zipSink(active.."/Broken/")
        assert(not pcall(sink,{filename="../escape"}))
        assert(self.fetched,"failed extraction must wake the download task")
        assert(sink("late chunk")==nil)
    end}
assert(compat.apply(broken,{fs=fs,io=files,showError=function(message) shown=message end}))
broken.todownload={Broken={}}
broken:_download();broken:_finished()
assert(shown and not finished and not stored)
local checksum={appletInstallerMenu=function() end,_remove=function() end,_zipSink=function() end,
    _finished=function() error("incomplete download must not record success") end,
    _download=function() end}
assert(compat.apply(checksum,{fs=fs,io=files,showError=function(message) shown=message end}))
checksum.todownload={Broken={}};checksum:_download();checksum:_finished()
assert(shown:find("verification did not complete",1,true))
print("extraction errors wake the task, stop the spinner and prevent false installation bookkeeping")
