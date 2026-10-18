package.path = arg[1] .. "/applet/?.lua;" .. package.path
local helper = require("ImageRender")
local png = "https://example.invalid/logo.png"
assert(helper.artworkUrl(png) == png)
assert(helper.artworkUrl("https://example.invalid/logo.svg?x=1"):find("/api/image/render?url=", 1, true))
assert(helper.artworkUrl("https://example.invalid/logo.svg?x=1"):find("%3Fx%3D1", 1, true))
assert(helper.artworkUrl(png, 64, 32, "image/svg+xml"):find("width=64&height=32", 1, true))
assert(helper.artworkUrl("https://example.invalid/logo.svg", 513) == nil)
if arg[2] then
    appletManager = {callService = function(self, service, ...)
        assert(service == "getStandaloneImageURL")
        return helper.artworkUrl(...)
    end}
    local transport = dofile(arg[2] .. "/UrlTransport.lua")
    assert(transport.transportUrl(png) == "http://127.0.0.1:8765/https/example.invalid/logo.png")
    local converted = transport.forRequest("https://example.invalid/logo.svg?size=small")
    assert(converted.host == "127.0.0.1" and converted.path:find("/api/image/render?", 1, true))
    assert(transport.artworkUrl("https://example.invalid/logo.svg"))
    appletManager = nil
    assert(transport.transportUrl(png) == "http://127.0.0.1:8765/https/example.invalid/logo.png")
    assert(loadfile(arg[2] .. "/LogoCache.lua"))
end
print("image-helper-tests-ok")
