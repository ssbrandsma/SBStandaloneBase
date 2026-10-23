-- Keep the stock installer on the applet tree actually used by SqueezePlay.
-- Stream ZIP members to disk, accepting both Unix and older Windows separators.
local lfs = require("lfs")
local log = require("jive.utils.log").logger("StandaloneBase")
local M = {}
local ACTIVE = "/usr/share/jive/applets"

function M.apply(installer, options)
    if not installer then
        -- AppletManager uses loadfile even if require has already loaded the
        -- module. Patch its managed instance after that load, or a later menu
        -- open replaces the patched methods with the stock implementation.
        local manager = options and options.appletManager or appletManager
        local ok, result = pcall(function()
            assert(manager, "AppletManager is unavailable")
            -- Closing the menu frees the installer and clears package.loaded.
            -- Every newly managed instance must receive the adapter, too.
            if manager._standaloneInstallerLoader ~= manager.loadApplet then
                local load = manager.loadApplet
                local wrapped = function(self, name, ...)
                    local instance = load(self, name, ...)
                    if name == "SetupAppletInstaller" and type(instance) == "table" then
                        M.apply(instance, options)
                    end
                    return instance
                end
                manager.loadApplet = wrapped
                manager._standaloneInstallerLoader = wrapped
            end
            return assert(manager:loadApplet("SetupAppletInstaller"), "Applet Installer did not load")
        end)
        if not ok then log:warn("StandaloneBase: installer compatibility unavailable: ", tostring(result)); return false end
        installer = result
    end
    if installer._standaloneInstallerCompat == installer._zipSink then return true end
    if type(installer.appletInstallerMenu) ~= "function" or type(installer._download) ~= "function"
        or type(installer._remove) ~= "function" or type(installer._zipSink) ~= "function" then
        log:warn("StandaloneBase: unsupported Applet Installer interface"); return false
    end
    local fs = options and options.fs or lfs
    local files = options and options.io or io
    local function route(self)
        local mounts = files.open("/proc/mounts", "r")
        if not mounts then return end
        local bound = false
        for line in mounts:lines() do
            local device, path, kind = line:match("^(%S+)%s+(%S+)%s+(%S+)")
            if path == ACTIVE and kind == "sbubifs" then bound = true end
        end
        mounts:close()
        if bound and fs.attributes(ACTIVE .. "/SetupAppletInstaller", "mode") == "directory" then
            if self.appletdir ~= ACTIVE then
                self._standaloneInstallerOriginalDir = self.appletdir
                log:info("StandaloneBase: Applet Installer uses active Extended Storage tree")
            end
            self.appletdir = ACTIVE
        elseif self.appletdir == ACTIVE and self._standaloneInstallerOriginalDir then
            self.appletdir = self._standaloneInstallerOriginalDir
        end
    end
    local menu, download, remove = installer.appletInstallerMenu, installer._download, installer._remove
    installer.appletInstallerMenu = function(self, ...)
        local result = menu(self, ...)
        route(self) -- Stock menu chooses the first physical package.path entry.
        return result
    end
    installer._download = function(self, ...)
        route(self)
        self._standaloneInstallerError = nil
        self._standaloneInstallerCompleted = {}
        local result = download(self, ...)
        for name in pairs(self.todownload or {}) do
            if not self._standaloneInstallerCompleted[name] then
                self._standaloneInstallerError = self._standaloneInstallerError or ("Download or verification did not complete: " .. name)
            end
        end
        return result
    end
    installer._remove = function(self, ...) route(self); return remove(self, ...) end
    if type(installer._sha1Sink) == "function" then
        local shaSink=installer._sha1Sink
        installer._sha1Sink=function(self,...)
            local sink=shaSink(self,...)
            return function(chunk,err)
                if err then
                    self._standaloneInstallerError=tostring(err)
                    self.fetchedsha=nil;self.fetched=true
                    return nil,err
                end
                return sink(chunk)
            end
        end
    end
    if type(installer._finished) == "function" then
        local finished=installer._finished
        installer._finished=function(self,...)
            if not self._standaloneInstallerError then return finished(self,...) end
            local message=self._standaloneInstallerError
            log:warn("StandaloneBase: Applet Installer stopped: ",message)
            if self.animatewindow then self.animatewindow:hide() end
            if options and options.showError then options.showError(message);return end
            local Window=require("jive.ui.Window")
            local Textarea=require("jive.ui.Textarea")
            local window=Window("text_list","Installation failed")
            window:addWidget(Textarea("help_text",message))
            self._standaloneInstallerErrorWindow=window
            window:show()
        end
    end
    installer._zipSink = function(self, directory)
        local handle, failed
        local reject
        local function close()
            if handle then
                local file=handle; handle=nil
                if not file:close() then reject("cannot close member") end
            end
        end
        reject = function(reason)
            failed=true
            self._standaloneInstallerError="Applet ZIP extraction failed: "..reason
            self.fetched=true -- Wake the stock download task even on sink failure.
            close()
            error("StandaloneBase: applet ZIP extraction failed: " .. reason)
        end
        local root = directory:gsub("/+$", "")
        local function directoryExists(path)
            local attr = fs.symlinkattributes and fs.symlinkattributes(path) or fs.attributes(path)
            if attr and attr.mode ~= "directory" then reject("non-directory path component") end
            if not attr and not fs.mkdir(path) then reject("cannot create directory") end
        end
        return function(chunk, err)
            if failed then return nil,self._standaloneInstallerError end
            if err then reject(tostring(err)) end
            if chunk == nil then
                close(); self.fetched = true
                if self._standaloneInstallerCompleted then
                    self._standaloneInstallerCompleted[root:match("/([^/]+)$")]=true
                end
                return nil
            elseif type(chunk) == "table" then
                close()
                local name = type(chunk.filename) == "string" and chunk.filename:gsub("\\", "/") or ""
                if name == "" or name:find("[%z%c:]") or name:sub(1,1) == "/" or name:find("//",1,true) then reject("invalid member path") end
                for part in name:gmatch("[^/]+") do
                    if part == "." or part == ".." then reject("unsafe member path") end
                end
                directoryExists(root)
                local path = root
                local parts = {}
                for part in name:gmatch("[^/]+") do parts[#parts+1] = part end
                for i = 1, #parts-1 do path = path .. "/" .. parts[i]; directoryExists(path) end
                path = path .. "/" .. parts[#parts]
                if name:sub(-1) == "/" then directoryExists(path)
                else
                    local attr = fs.symlinkattributes and fs.symlinkattributes(path) or fs.attributes(path)
                    if attr and attr.mode ~= "file" then reject("non-file member destination") end
                    handle = files.open(path, "wb")
                    if not handle then reject("cannot open member") end
                end
            elseif type(chunk) == "string" then
                if handle and not handle:write(chunk) then reject("cannot write member") end
            else reject("invalid ZIP chunk") end
            return 1
        end
    end
    installer._standaloneInstallerCompat = installer._zipSink
    log:info("StandaloneBase: Applet Installer path and ZIP compatibility enabled")
    return true
end
return M
