local root = arg[1]
package.preload["jive.utils.log"] = function()
    return {logger=function() return {warn=function() end} end}
end
package.preload["applets.StandaloneBase.ArtworkUrlNormalizer"] = function()
    return dofile(root .. "/applet/ArtworkUrlNormalizer.lua")
end
package.preload["applets.StandaloneBase.ImageRender"] = function()
    return dofile(root .. "/applet/ImageRender.lua")
end
local artwork = dofile(root .. "/applet/Artwork.lua")
for _, scheme in ipairs({"http", "https"}) do
    for _, format in ipairs({"png", "jpg", "jpeg"}) do
        local source = scheme .. "://example.com/logo." .. format .. "?x=a%2Fb&token=q@r"
        local result = artwork.route(source, 320, 240)
        assert(result:find("http://127.0.0.1:80/api/image/render?", 1, true) == 1)
        assert(result:find("width=320&height=240", 1, true))
        assert(result:find("%252F", 1, true)) -- source escapes survive query encoding
        assert(artwork.route(result) == result)
    end
    assert(artwork.route(scheme .. "://example.com/logo.svg?q=1", 64, 32):find("/api/image/render?", 1, true))
end
for _, url in ipairs({"abc123", "/music/foo/cover.png", "plugins/icon.png",
    "http://127.0.0.1:80/file", "http://localhost:80/file", "http://10.0.0.1/file",
    "http://172.31.1.1/file", "http://192.168.1.2/file", "http://radio.local/file"}) do
    assert(artwork.route(url) == url, url)
end
assert(artwork.route("http://127.0.0.1:8765/https/example.com/art.jpg") ==
    artwork.route("https://example.com/art.jpg"))
assert(artwork.route("http://example.com?q=a%2Fb"):find("/api/image/render?", 1, true))

local function state()
    local cache, wakes = {}, 0
    local obj = {artworkFetchCount=1, imageCache={}, artworkThumbIcons={},
        artworkCache={set=function(_, k, v) cache[k]=v end},
        artworkFetchTask={addTask=function() wakes=wakes+1 end}}
    return obj, cache, function() return wakes end
end
local obj, cache, wakes = state()
cache.k = true
local fail = artwork.sink(obj, "k", 320, "url", function() error("bad image") end)
fail(nil, "timeout")
assert(cache.k == nil and obj.artworkFetchCount == 0 and wakes() == 1)
fail("late", nil)
assert(wakes() == 1 and obj.artworkFetchCount == 0)
obj.artworkFetchCount = 1
local success = artwork.sink(obj, "k", 320, "url", function() return "surface" end)
success("png", nil)
success(nil, nil)
assert(cache.k == "png" and obj.artworkFetchCount == 0)
obj.artworkFetchCount = 2
local stale = artwork.sink(obj, "k", 320, "url", function() return "old" end)
local latest = artwork.sink(obj, "k", 320, "url", function() return "new" end)
stale("old-bytes")
assert(cache.k == "png")
latest("new-bytes")
assert(cache.k == "new-bytes" and obj.artworkFetchCount == 0)
obj.artworkFetchCount = 1
local invalid = artwork.sink(obj, "k", 320, "url", function() error("decode failure") end)
invalid("bad-bytes")
assert(cache.k == nil and obj.imageCache.k == nil)
obj.artworkFetchCount = 1
cache.k = true
local cancelled = artwork.sink(obj, "k", 320, "url", function() return "late-surface" end)
artwork.cancel(obj)
assert(cache.k == nil and obj.artworkFetchCount == 1)
cancelled("late")
assert(cache.k == nil and obj.artworkFetchCount == 0)
obj.artworkFetchCount = 1
artwork.sink(obj, "k", 320, "url", function() return "retry" end)("retry-bytes")
assert(cache.k == "retry-bytes")
print("artwork-routing-and-lifecycle-tests-ok")
