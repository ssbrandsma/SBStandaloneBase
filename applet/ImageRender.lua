-- Generic PNG/JPEG/SVG artwork helper. Decode and resizing stay in native code.
local M = {}

local function webPort()
    local f = io.open("/mnt/storage/standalonebase/settings.conf", "r")
    if not f then return 80 end
    local port = 80
    for line in f:lines() do
        local value = tonumber(line:match("^port=(%d+)$"))
        if value and value >= 1 and value <= 65535 then port = value end
    end
    f:close()
    return port
end

M.webPort = webPort

function M.artworkUrl(url, width, height, mime)
    if type(url) ~= "string" then return url end
    -- Never wrap the local renderer or proxy again.
    local host = url:lower():match("^https?://([^/:]+)")
    if host == "127.0.0.1" or host == "localhost" then return url end
    -- This service is also consulted by Radio's shared HTTP transport. Only
    -- artwork callers may force conversion of extensionless URLs; audio,
    -- playlists, directory JSON/XML and metadata requests must pass through.
    local path = url:lower():match("^[^?#]+") or ""
    if not path:match("%.png$") and not path:match("%.jpe?g$") and
       not path:match("%.svg$") and mime ~= "image/svg+xml" and mime ~= "image/*" then return url end
    if not url:match("^https?://") or url:find("[%s%z]") or #url > 4095 then return nil end
    width, height = tonumber(width) or 320, tonumber(height) or 240
    if width % 1 ~= 0 or height % 1 ~= 0 or width < 16 or width > 512 or height < 16 or height > 512 then return nil end
    width, height = math.min(width, 320), math.min(height, 240)
    local escaped = url:gsub("([^%w%-_%.~])", function(c) return string.format("%%%02X", string.byte(c)) end)
    return "http://127.0.0.1:" .. webPort() .. "/api/image/render?url=" .. escaped ..
        "&width=" .. width .. "&height=" .. height
end

return M
