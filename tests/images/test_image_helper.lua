package.path = arg[1] .. "/applet/?.lua;" .. package.path
package.preload["applets.StandaloneBase.ArtworkUrlNormalizer"] = function() return require("ArtworkUrlNormalizer") end
local helper = require("ImageRender")
local png = "https://example.invalid/logo.png"
assert(helper.artworkUrl(png):find("/api/image/render?", 1, true))
assert(helper.artworkUrl(png):find("width=320&height=240", 1, true))
assert(helper.artworkUrl("http://127.0.0.1/api/image/render?url=x") == "http://127.0.0.1/api/image/render?url=x")
for _, url in ipairs({"http://example.com/marcher-vibe", "https://example.com/live.mp3", "https://example.com/live.m3u8", "http://example.com/stations.json", "http://example.com/search.opml"}) do
    assert(helper.artworkUrl(url) == url, url)
end
assert(helper.artworkUrl("http://example.com/cover", 320, 240, "image/*"):find("/api/image/render?", 1, true))
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
    assert(transport.transportUrl(png):find("/api/image/render?", 1, true))
    local converted = transport.forRequest("https://example.invalid/logo.svg?size=small")
    assert(converted.host == "127.0.0.1" and converted.path:find("/api/image/render?", 1, true))
    assert(transport.artworkUrl("https://example.invalid/logo.svg"))
    local stream = transport.forRequest("http://uksoutha.streaming.broadcast.radio:18321/marcher-vibe")
    assert(stream.host == "uksoutha.streaming.broadcast.radio" and stream.port == 18321 and stream.path == "/marcher-vibe")
    assert(transport.transportUrl("https://example.invalid/live.mp3") == "http://127.0.0.1:8765/https/example.invalid/live.mp3")
    appletManager = nil
    assert(transport.transportUrl(png) == "http://127.0.0.1:8765/https/example.invalid/logo.png")
    assert(loadfile(arg[2] .. "/LogoCache.lua"))
end
print("image-helper-tests-ok")
