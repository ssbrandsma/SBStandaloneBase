-- Pure rule and shared-pipeline regression tests. Lua 5.1 compatible.
local root = assert(arg[1], "project root required")
package.path = root .. "/applet/?.lua;" .. package.path
local normalizer = require("ArtworkUrlNormalizer")
local base = "https://upload.wikimedia.org/wikipedia/commons/thumb/a/ab/example.svg/"
local cases = {
    {base .. "640px-example.svg.png", base .. "250px-example.svg.png"},
    {base .. "1024px-example.svg.png", base .. "250px-example.svg.png"},
    {base .. "250px-example.svg.png", base .. "250px-example.svg.png"},
    {base .. "640px-example.svg.png?v=640px&x=a%2Fb#1024px-fragment", base .. "250px-example.svg.png?v=640px&x=a%2Fb#1024px-fragment"},
    {"http://upload.wikimedia.org/wikipedia/en/thumb/640px/a/file.jpg/500px-file.jpg", "http://upload.wikimedia.org/wikipedia/en/thumb/640px/a/file.jpg/250px-file.jpg"},
    {"HTTPS://UPLOAD.WIKIMEDIA.ORG:443/wikipedia/commons/thumb/a/ab/test.png/500px-test.png", "HTTPS://UPLOAD.WIKIMEDIA.ORG:443/wikipedia/commons/thumb/a/ab/test.png/250px-test.png"},
    {"https://upload.wikimedia.org/wikipedia/commons/thumb/a/ab/test.gif/500px-test.gif", "https://upload.wikimedia.org/wikipedia/commons/thumb/a/ab/test.gif/250px-test.gif"},
    {"https://upload.wikimedia.org/wikipedia/commons/thumb/f/f4/NPO_Radio_2_logo.svg/640px-NPO_Radio_2_logo.svg.png", "https://upload.wikimedia.org/wikipedia/commons/thumb/f/f4/NPO_Radio_2_logo.svg/250px-NPO_Radio_2_logo.svg.png"},
}
for _, test in ipairs(cases) do
    assert(normalizer.normalize(test[1]) == test[2], test[1])
    assert(normalizer.normalize(test[2]) == test[2], "idempotence")
end
for _, url in ipairs({
    "https://upload.wikimedia.org/wikipedia/commons/a/ab/640px-example.svg.png",
    "https://example.com/wikipedia/commons/thumb/a/ab/a.png/640px-a.png",
    "https://upload.wikimedia.org.evil.test/wikipedia/commons/thumb/a/ab/a.png/640px-a.png",
    "https://evil-upload.wikimedia.org/wikipedia/commons/thumb/a/ab/a.png/640px-a.png",
    "https://upload.wikimedia.org@evil.test/wikipedia/commons/thumb/a/ab/a.png/640px-a.png",
    "https://evil.test@upload.wikimedia.org/wikipedia/commons/thumb/a/ab/a.png/640px-a.png",
    "https://upload.wikimedia.org:bad/wikipedia/commons/thumb/a/ab/a.png/640px-a.png",
    "https://upload.wikimedia.org:444/wikipedia/commons/thumb/a/ab/a.png/640px-a.png",
    base .. "640px-", base .. "x640px-file.png", base .. "640px-file.png/other.png",
    base .. "640px-file.png/", base .. "file.png?name=640px-file.png",
    "ftp://upload.wikimedia.org/wikipedia/commons/thumb/a/ab/a.png/640px-a.png",
    "https://upload.wikimedia.org/wikipedia//thumb/a/ab/a.png/640px-a.png",
    "https://upload.wikimedia.org/wikipedia/commons/thumb/a/ab/a.png/640px-a b.png",
    "https://upload.wikimedia.org/wikipedia/commons/thumb/a/ab/a.png/640px-a\0b.png",
    "", "not a URL", "https://", "file:///tmp/640px-image.png", string.rep("a", 4096),
}) do assert(normalizer.normalize(url) == url, url) end
for _, value in ipairs({false, 1, {}}) do assert(normalizer.normalize(value) == value) end
assert(normalizer.normalize(nil) == nil)

local diagnostics = {}
package.preload["jive.utils.log"] = function() return {logger=function() return {
    debug=function(_, ...) diagnostics[#diagnostics+1]=table.concat({...}) end,
    warn=function() end,
} end} end
package.preload["applets.StandaloneBase.ArtworkUrlNormalizer"] = function() return normalizer end
local helper = require("ImageRender")
package.preload["applets.StandaloneBase.ImageRender"] = function() return helper end
local artwork = require("Artwork")
local function decode(url)
    return (url:gsub("%%(%x%x)", function(hex) return string.char(tonumber(hex,16)) end))
end
local rendered
for _, width in ipairs({250,640,1024}) do
    local url = base .. width .. "px-example.svg.png"
    local result = helper.artworkUrl(url,320,240)
    assert(result:find("/api/image/render?url=",1,true))
    if rendered then assert(result == rendered, "normalized renderer URL/cache identity differs") end
    rendered = result
    assert(decode(result:match("[?&]url=([^&]+)")) == base .. "250px-example.svg.png")
    assert(artwork.route(url,320,240) == rendered)
    assert(artwork.route("http://127.0.0.1:8765/https/" .. url:sub(9),320,240) == rendered)
end
assert(helper.artworkUrl(base.."640px-image.gif",320,240,"image/*"):find("250px",1,true))
assert(helper.artworkUrl(base.."640px-example.svg.png",64,32) ~= rendered, "requested dimensions must remain distinct")
local query = helper.artworkUrl(base.."640px-example.svg.png?v=a%2Fb&quality=80")
assert(decode(query:match("[?&]url=([^&]+)")) == base.."250px-example.svg.png?v=a%2Fb&quality=80")
assert(#diagnostics > 0 and diagnostics[1]:find("640px",1,true) and diagnostics[1]:find("250px",1,true))
assert(not diagnostics[#diagnostics]:find("a%2Fb",1,true), "diagnostics must redact query values")
print("artwork URL normalizer: Wikimedia formats, exact host/path matching, malformed inputs, idempotence, preservation, shared routing/cache identity and diagnostics passed")
