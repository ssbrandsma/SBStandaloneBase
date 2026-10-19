-- Shared Jive artwork adapter. Raster bytes stay in the streaming proxy;
-- only SVG uses ImageRender's existing bounded native conversion/cache.
local ImageRender = require("applets.StandaloneBase.ImageRender")
local M = {}
local log = require("jive.utils.log").logger("StandaloneBase")

local function localHost(host)
    if host == "localhost" or host:match("%.local$") or host:match("%.lan$") or
        not host:find("%.") then return true end
    local a, b = host:match("^(%d+)%.(%d+)%.%d+%.%d+$")
    a, b = tonumber(a), tonumber(b)
    return a and (a == 127 or a == 10 or a == 0 or
        (a == 192 and b == 168) or (a == 172 and b >= 16 and b <= 31) or
        (a == 169 and b == 254))
end

-- Preserve non-URL/LMS identifiers and local resources. Never decode/re-encode
-- source paths: signed queries and percent escapes must survive unchanged.
function M.route(url, width, height)
    if type(url) ~= "string" then return url end
    local original = url
    -- Existing integrations already wrap HTTPS. Upgrade only this exact known
    -- transport, once, so MIME-only SVG also reaches the conversion path.
    local wrappedScheme, wrappedSource = url:match("^http://127%.0%.0%.1:8765/(https?)/(.+)$")
    if wrappedScheme then url = wrappedScheme .. "://" .. wrappedSource end
    local scheme, authority, tail = url:match("^(https?)://([^/?#]+)(.*)$")
    if not scheme then return url end
    if tail:sub(1, 1) ~= "/" then tail = "/" .. tail end
    if #url > 4095 or url:find("[%c%s]") or authority:find("[@%%\\]") then
        return "http://127.0.0.1:8765/invalid-artwork"
    end
    local host = authority:match("^([^:]+)") or ""
    if localHost(host:lower()) or host:sub(1, 1) == "[" then return original end
    width, height = tonumber(width) or 320, tonumber(height) or 320
    width = math.max(16, math.min(512, math.floor(width)))
    height = math.max(16, math.min(512, math.floor(height)))
    local converted = ImageRender.artworkUrl(url, width, height, "image/*")
    if converted ~= url then return converted end
    -- Dimensions and web port let the proxy redirect MIME-identified SVG to
    -- the existing local renderer, without receiving SVG bytes in Lua.
    return "http://127.0.0.1:8765/artwork/" .. width .. "/" .. height .. "/" ..
        ImageRender.webPort() .. "/" .. scheme .. "/" .. authority .. tail
end

-- One terminal callback owns each slot. Late/duplicate callbacks cannot
-- decrement the count twice or overwrite a newer request for the same key.
function M.sink(self, key, size, url, decode)
    self._sbArtworkRequests = self._sbArtworkRequests or {}
    local token, finished = {}, false
    self._sbArtworkRequests[key] = token
    return function(chunk, err)
        if finished or (not chunk and not err) then return end
        finished = true
        self.artworkFetchCount = math.max(0, self.artworkFetchCount - 1)
        local current = self._sbArtworkRequests[key] == token
        if current then
            self._sbArtworkRequests[key] = nil
            local image
            if chunk and not err then
                local ok, result = pcall(decode, self, key, chunk, size)
                if ok then image = result end
            end
            if image then
                self.artworkCache:set(key, chunk)
            else
                log:warn("StandaloneBase: artwork failed bytes=", tostring(type(chunk) == "string" and #chunk or 0),
                    " reason=", tostring(err or "image decode failed"))
                self.artworkCache:set(key, nil)
                self.imageCache[key] = nil
            end
            for icon, waiting in pairs(self.artworkThumbIcons) do
                if waiting == key then
                    icon:setValue(image)
                    self.artworkThumbIcons[icon] = nil
                end
            end
        end
        self.artworkFetchTask:addTask()
    end
end

-- Retire active generations without pretending their sockets have finished.
-- Their terminal callbacks still release slots, but cannot update old icons.
function M.cancel(self)
    for key in pairs(self._sbArtworkRequests or {}) do
        self.artworkCache:set(key, nil)
        self._sbArtworkRequests[key] = nil
    end
    for icon in pairs(self.artworkThumbIcons) do
        self.artworkThumbIcons[icon] = nil
    end
end

return M
