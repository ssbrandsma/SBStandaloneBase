local oo = require("loop.simple")
local AppletMeta = require("jive.AppletMeta")
local _ = require("jive.i18n")
module(..., oo.class(AppletMeta))

function jiveVersion(meta) return 1, 1 end
function defaultSettings(meta) return { enabled = true } end
function registerApplet(meta)
    jiveMain:addItem(meta:menuItem("standaloneBase", "home", "STANDALONE_BASE",
        function(applet) applet:menu() end, 90))
end
function configureApplet(meta)
    local applet = meta:loadApplet()
    applet:init()
end
