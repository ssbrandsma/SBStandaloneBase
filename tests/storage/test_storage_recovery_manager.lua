-- Execute the actual manager using fake asynchronous Jive primitives.
local command, sink, timers, completed = nil, nil, {}, nil
package.preload["jive.net.Process"] = function()
    return function(_, cmd)
        command = cmd
        return {read=function(_, callback) sink=callback end}
    end
end
package.preload["jive.ui.Timer"] = function()
    return function(_, callback)
        local timer = {callback=callback, start=function() end,
            stop=function(self) self.stopped=true end}
        timers[#timers+1]=timer
        return timer
    end
end
local root = arg and arg[1] or "."
local Manager = assert(loadfile(root .. "/applet/StorageManager.lua"))()
local manager = Manager:new("/usr/share/jive/applets/StandaloneBase")
assert(manager:startRecovery(nil, function(code) completed=code end))
assert(command:find("storage%-setup.sh recover"))
assert(not command:find("initialize"))
assert(not manager:startRecovery(), "Concurrent operations must be refused")
sink("PROCESS_EXIT_CODE=0\n")
sink(nil)
assert(completed==0 and manager.process==nil and timers[1].stopped)
assert(manager:startInitialization())
assert(command:find("storage%-setup.sh initialize"))
assert(command:find("sbubifs%-authorized.ko"))
print("storage-recovery-manager-tests-ok")
