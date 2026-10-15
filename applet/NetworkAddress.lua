local M = {}

local function usable(ip)
    local a,b,c,d = ip:match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")
    if not a then return false end
    a,b,c,d = tonumber(a),tonumber(b),tonumber(c),tonumber(d)
    return a <= 255 and b <= 255 and c <= 255 and d <= 255
        and a ~= 0 and a ~= 127
end

function M.parse(output)
    for ip in output:gmatch("inet%s+(%d+%.%d+%.%d+%.%d+)/") do
        if usable(ip) then return ip end
    end
    for ip in output:gmatch("inet%s+addr:(%d+%.%d+%.%d+%.%d+)") do
        if usable(ip) then return ip end
    end
    return nil
end

function M.current()
    local pipe = io.popen("/bin/ip -4 addr show 2>/dev/null")
    if not pipe then return nil end
    local output = pipe:read("*a") or ""
    pipe:close()
    return M.parse(output)
end

return M
