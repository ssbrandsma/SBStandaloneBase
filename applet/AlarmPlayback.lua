-- Audio half of sbbase's LMS alarm protocol. AlarmSnooze remains responsible
-- for the stock window, hardware wake-up, buttons and decoder-failure fallback.
-- Notifications, not polling, drive this bridge. Never act on another LMS.
local Timer = require("jive.ui.Timer")
local migration = require("applets.StandaloneBase.ServerMigration")
local log = require("jive.utils.log").logger("StandaloneBase")
local manager = appletManager
local powerManager = jiveMain
local M = {}
M.__index = M

function M.new(dependencies)
    dependencies = dependencies or {}
    return setmetatable({manager = dependencies.manager or manager,
        power = dependencies.power or powerManager,
        timer = dependencies.timer or Timer, log = dependencies.log or log}, M)
end

function M:owns(player)
    if not player or not player:isLocal() then return false end
    local server = player:getSlimServer()
    return server and server.id == migration.LOCAL_UUID
end

-- Loading the Radio is only allowed for an explicit configured preset alarm.
function M:radio(load)
    local radio = self.manager:getAppletInstance("StandaloneRadio")
    if not radio and load then radio = self.manager:loadApplet("StandaloneRadio") end
    if radio and radio._ensureComponents then radio:_ensureComponents() end
    return radio
end

-- Cancel unresolved station playlists too: their callback must not restart a
-- station after snooze/off, even when stop precedes the network response.
function M:stopRadio(radio)
    if not radio then return end
    radio.playlistRequestGeneration = (radio.playlistRequestGeneration or 0) + 1
    if radio.playlistClient and radio.playlistClient.cancelPlaylist then
        radio.playlistClient:cancelPlaylist()
    end
    if radio.streamPlayer and radio.streamPlayer.ownsPlayback then radio.streamPlayer:stop() end
end

function M:cancelFade()
    if self.fade then self.fade:stop(); self.fade = nil end
end

-- Keep the firmware's fallback UI/audio, but release Radio's reconnect watchdog
-- before its tone takes the decoder. The guard is scoped to this Base alarm;
-- alarms belonging to a real LMS continue through the original methods.
function M:bindFallback(alarmUI)
    if not alarmUI then return end
    alarmUI._standaloneAlarmBridge = self
    if alarmUI._standaloneAlarmFallbackBound then return end
    if type(alarmUI.soundFallbackAlarm) ~= "function" or type(alarmUI._alarmSnooze) ~= "function" then return end
    local originalFallback, originalSnooze = alarmUI.soundFallbackAlarm, alarmUI._alarmSnooze
    alarmUI.soundFallbackAlarm = function(instance, ...)
        local bridge = instance._standaloneAlarmBridge
        if bridge.player and bridge:owns(bridge.player) then
            bridge:cancelFade()
            bridge:stopRadio(bridge.radioAlarm)
        end
        return originalFallback(instance, ...)
    end
    alarmUI._alarmSnooze = function(instance, ...)
        local bridge = instance._standaloneAlarmBridge
        local player = bridge.player
        -- Stock Player:snooze only sends to the server for state 'active'.
        -- Its own fallback sound changes that to 'active_fallback'. Base is
        -- still alive here, so synchronize the native snooze as well.
        if bridge:owns(player) and player:getAlarmState() == "active_fallback"
            and (player:getPlayerStatus() or {}).alarm_state == "active" then
            player:setAlarmState("active")
        end
        return originalSnooze(instance, ...)
    end
    alarmUI._standaloneAlarmFallbackBound = true
end

function M:setVolume(player, target, fade)
    self:cancelFade()
    target = math.max(0, math.min(100, tonumber(target) or 50))
    player:volumeLocal(fade and target > 1 and 1 or target, true)
    if not fade or target <= 1 then return end
    local step, expected = 0, 1
    self.fade = self.timer(1000, function()
        -- An explicit volume change or server handoff wins over the fade.
        if not self:owns(player) or player:getVolume() ~= expected then
            self:cancelFade(); return
        end
        step = step + 1
        expected = math.floor(1 + (target - 1) * math.min(step, 20) / 20)
        player:volumeLocal(expected, true)
        if step >= 20 then self:cancelFade() end
    end)
    self.fade:start()
end

function M:start(player, status)
    self.player = player
    if self.previousVolume == nil then self.previousVolume = player:getVolume() end
    -- Normally the stock RTC timer opens the window. After a Jive restart
    -- during an already active alarm that timer can be in the past. Recover
    -- only that case, without racing/restarting a still-running RTC timer.
    local alarmUI = self.manager:getAppletInstance("AlarmSnooze")
    self:bindFallback(alarmUI)
    if alarmUI and not alarmUI.alarmInProgress then
        local rtc = alarmUI.RTCAlarmTimer
        if rtc and not rtc:isRunning() then
            alarmUI.localPlayer = player
            alarmUI.alarmInProgress = "rtc"
            alarmUI:openAlarmWindow()
        end
    end
    local source = status.sb_alarm_source
    local radio = self:radio(source and source:match("^sbpreset://[1-6]$") ~= nil)
    local stream = radio and radio.streamPlayer
    local preset = source and tonumber(source:match("^sbpreset://([1-6])$"))
    self:cancelFade()
    if preset then
        self:stopRadio(radio)
        local station = radio and radio.presetStore and radio.presetStore:getPreset(preset)
        if station then
            self.radioAlarm = radio
            radio:_playStation(station)
        else
            self.log:warn("Alarm preset unavailable; using built-in alarm sound")
            player:playFileInLoop("applets/AlarmSnooze/alarm.mp3")
        end
    elseif source == "CURRENT_PLAYLIST" and stream and stream:getCurrentStation()
        and (stream.ownsPlayback or player:getPlayMode() ~= "play") then
        self.radioAlarm = radio
        if not stream:isPlaying() then radio:_playStation(stream:getCurrentStation()) end
    else
        -- No remote library is available while Base is the LMS. A local tone
        -- is reliable even with no Radio applet, network or extended storage.
        self:stopRadio(radio)
        player:playFileInLoop("applets/AlarmSnooze/alarm.mp3")
    end
    self:setVolume(player, status.sb_alarm_volume, tonumber(status.sb_alarm_fade) == 1)
end

function M:finish(player, keepAudio, snoozing)
    self:cancelFade()
    if not keepAudio then
        self:stopRadio(self.radioAlarm)
        player:stop(true)
        if self.previousVolume ~= nil then player:volumeLocal(self.previousVolume, true) end
    end
    if not snoozing then
        local alarmUI = self.manager:getAppletInstance("AlarmSnooze")
        if alarmUI and alarmUI.alarmInProgress then
            -- The stock button handler may already have hidden the window.
            -- Remote disable/delete and native timeout must close it too.
            alarmUI.alarmInProgress = nil
            alarmUI:_stopDecodeStatePoller()
            alarmUI:_stopSnoozeTimer()
            if alarmUI.fallbackAlarmTimeout then alarmUI.fallbackAlarmTimeout:stop() end
            alarmUI:_hideAlarmWindow()
        end
        self.player, self.previousVolume, self.radioAlarm = nil, nil, nil
    end
end

function M:notify_playerAlarmState(player)
    if not self:owns(player) then return end
    local status = player:getPlayerStatus() or {}
    local serial = tonumber(status.sb_alarm_serial)
    if not serial or serial == 0 or self.serial == serial then return end
    self.serial = serial
    local event = status.sb_alarm_event
    self.log:info("Standalone Base alarm event=", tostring(event), " serial=", serial,
        " source=", tostring(status.sb_alarm_source))
    local ok, err = pcall(function()
        if event == "start" and status.alarm_state == "active" then
            self:start(player, status)
        elseif self.player == player then
            self:finish(player, event == "dismiss", event == "snooze")
        end
    end)
    if not ok then
        self:cancelFade()
        self.log:warn("Alarm audio bridge failed; stock fallback remains available: ", tostring(err))
    end
end

-- A handoff relinquishes ownership without issuing stop/volume commands to the
-- newly selected server. Reconnection must not replay the last alarm event.
function M:notify_playerConnected(player)
    if self.player == player and not self:owns(player) then
        self:cancelFade()
        self.player, self.previousVolume, self.radioAlarm = nil, nil, nil
    end
end

function M:notify_playerDisconnected(player)
    -- Stop a pending fade immediately. Do not stop audio or clear the saved
    -- volume: the stock RTC fallback and an ordinary reconnect still need it.
    if self.player == player then self:cancelFade() end
end

-- Stock Player emits this notification when the LMS sleep field changes.
-- Establish a baseline first: an old expiry must not stop fresh playback after
-- Jive is restarted or the user returns from a different music library.
function M:notify_playerSleepChange(player)
    if not self:owns(player) then return end
    local status = player:getPlayerStatus() or {}
    local serial = tonumber(status.sb_sleep_serial)
    if not serial then return end
    local previous = self.sleepSerial
    self.sleepSerial = serial
    if previous == nil or previous == serial then return end
    self:cancelFade()
    self:stopRadio(self:radio(false))
    player:stop(true)
    if self.power then self.power:setSoftPowerState("off") end
    self.log:info("Standalone Base sleep timer expired")
end

return M
