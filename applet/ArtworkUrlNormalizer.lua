-- Pure URL rules shared by the artwork services. No network, codecs or Jive UI.
local M = {}
local function wikimediaThumbnail(url)
    local scheme, authority, path, suffix = url:match("^([%a][%w+.-]*://)([^/?#]+)([^?#]*)(.*)$")
    if not scheme then return end
    local protocol = scheme:lower()
    if protocol ~= "https://" and protocol ~= "http://" then return end
    local host, port = authority:match("^([^:]+):(%d+)$")
    host = host or authority
    if host:lower() ~= "upload.wikimedia.org" then return end
    if port and port ~= (protocol == "https://" and "443" or "80") then return end
    if not path:find("/wikipedia/[^/]+/thumb/") then return end
    local directory, width, filename = path:match("^(.*)/(%d+)px%-([^/]+)$")
    if not directory or width == "250" then return end
    return scheme .. authority .. directory .. "/250px-" .. filename .. suffix
end

-- Ordered pure rules; add future provider rules here, not in applets.
local rules = { wikimediaThumbnail }
function M.normalize(url)
    if type(url) ~= "string" or #url > 4095 or url:find("[%c%s]") then return url end
    for i = 1, #rules do url = rules[i](url) or url end
    return url
end
return M
