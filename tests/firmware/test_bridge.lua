-- Exercise the real Lua 5.1 module sandbox: require is NOT a module global.
local framework = {constants = function() end}
package.preload['loop.simple'] = function() return {class = function() end} end
package.preload['jive.ui.Framework'] = function() return framework end
for _, name in ipairs({'Event', 'Timer', 'Surface'}) do
    package.preload['jive.ui.' .. name] = function() return {} end
end
local requests, sockets = {}, {}
package.preload['jive.net.SocketHttp'] = function()
    return function()
        local socket = {fetch = function(self, request) requests[#requests + 1] = request end,
                        close = function(self) self.closed = true end}
        sockets[#sockets + 1] = socket
        return socket
    end
end
package.preload['jive.net.RequestHttp'] = function()
    return function(callback, method, path, options)
        return {callback = callback, method = method, path = path, options = options}
    end
end
package.preload.json = function() return {encode = function() return '{}' end} end
local real_io = io
io = {open = function(path)
    if path:find('remote.capability', 1, true) then
        return {read = function() return string.rep('a', 64) end, close = function() end}
    end
    return nil
end}
local chunk = assert(loadfile(arg[1] .. '/applet/RemoteBridge.lua'))
local bridge = chunk('firmware_bridge_test')
io = real_io
assert(bridge.require == nil, 'test must preserve the restricted module environment')
local object, delivered = {}, 0
bridge.firmware(object, 'permission', {}, function() delivered = delivered + 1 end)
assert(#requests == 1 and requests[1].method == 'POST')
assert(requests[1].path == '/api/internal/firmware/permission')
assert(requests[1].options.t_bodySource() == '{}')
assert(requests[1].options.t_bodySource() == nil)
requests[1].callback('{}')
requests[1].callback(nil, 'closed')
assert(delivered == 1)
bridge.firmware(object, 'permission', {}, function() end)
assert(sockets[1].closed and #requests == 2)
print('Firmware bridge restricted-module and bounded request tests passed')
