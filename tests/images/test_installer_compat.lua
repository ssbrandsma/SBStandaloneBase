local root = arg[1]
local nodes, bytes, closed = {}, {}, 0
package.preload.lfs = function() return {} end
package.preload["jive.utils.log"] = function()
    return {logger=function() return {info=function() end,warn=function() end} end}
end
local compat = dofile(root .. "/applet/InstallerCompat.lua")
local active = "/usr/share/jive/applets"
local mounted = true
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
                return mounted and "ubi0:sbdata /usr/share/jive/applets sbubifs rw 0 0" or "none / unionfs rw 0 0"
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
