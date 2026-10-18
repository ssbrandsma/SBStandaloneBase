-- Optional firmware-fixture test. Run against copies patched by the installer,
-- never against active firmware. No source fixture or private device data ships.
local root, slimPath, viewerPath = arg[1], arg[2], arg[3]
local function read(path)
    local f = assert(io.open(path))
    local text = f:read("*a")
    f:close()
    return text
end
local compile = loadstring or load
package.preload["applets.StandaloneBase.ImageRender"] = function()
    return dofile(root .. "/applet/ImageRender.lua")
end
local artwork = dofile(root .. "/applet/Artwork.lua")
package.loaded["applets.StandaloneBase.Artwork"] = artwork
logcache = {debug=function() end}
log = logcache
jnt = {getSNHostname=function() error("retired proxy must never be used") end}
Framework = {getScreenSize=function() return 320, 240 end}
SlimServer = {getCurrentServer=function() return nil end}
string.urlEncode = function() error("legacy encoding branch reached") end
local slim = read(slimPath):match("(function fetchArtwork%b().-)function getAppParameters")
assert(compile(assert(slim)))()
local viewer = read(viewerPath):match("(function requestImage%b().-)self.currentImageFile = urlString")
assert(compile(assert(viewer) .. "return urlString\nend"))()
for _, source in ipairs({"http://example.com/a.png", "https://example.com/a.jpg?q=a%2Fb&x=2",
    "http://example.com/a.svg?x=1", "https://example.com/a.svg",
    "http://127.0.0.1:8765/https/example.com/a.jpg", "http://localhost/a.png",
    "http://192.168.1.2/a.jpg", "http://172.31.1.1/a.jpg"}) do
    local self = {imageCache={}, artworkThumbIcons={}, artworkFetchQueue={},
        artworkCache={get=function() return nil end, set=function() end},
        artworkFetchTask={addTask=function() end}}
    fetchArtwork(self, source, nil, "320x240")
    assert(self.artworkFetchQueue[1].url == artwork.route(source, 320, 240), source)
    local result = requestImage({}, {image=source})
    assert(result == artwork.route(source, 320, 240), source)
end
for source, expected in pairs({abc123="/music/abc123/cover_320x240_m",
    ["plugins/icon.png"]="/plugins/icon_320x240_m.png"}) do
    local self = {imageCache={}, artworkThumbIcons={}, artworkFetchQueue={},
        artworkCache={get=function() end, set=function() end}, artworkFetchTask={addTask=function() end}}
    fetchArtwork(self, source, nil, "320x240")
    assert(self.artworkFetchQueue[1].url == expected)
end
print("both-firmware-artwork-paths-tests-ok")
