local oo = require("loop.simple")
local AppletMeta = require("jive.AppletMeta")
local appletManager = appletManager
local jiveMain = jiveMain
module(...)
oo.class(_M, AppletMeta)

function jiveVersion(meta) return 1, 1 end
function defaultSettings(meta) return { enabled = true, customFirmwareUpdates = false } end
function registerApplet(meta)
    meta:registerService("getStandaloneStorageInfo")
    meta:registerService("getStandaloneStorageCompatibility")
    meta:registerService("getStandaloneImageURL")
    meta:registerService("getStandaloneArtworkURL")
    local applet = appletManager:loadApplet("StandaloneBase")
    jiveMain:addItem(meta:menuItem("standaloneBase", "home", "STANDALONE_BASE",
        function() applet:menu() end, 90))
end

return _M
