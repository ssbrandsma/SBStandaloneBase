-- Deterministic alarm audio tests; no Jive, network, hardware or real timers.
package.path = arg[1] .. "/applet/?.lua;" .. package.path
local migration = require("ServerMigration")
package.loaded["applets.StandaloneBase.ServerMigration"] = migration
package.preload["jive.ui.Timer"] = function() return function() error("inject timer") end end
package.preload["jive.utils.log"] = function() return {logger=function() return {} end} end
local Bridge = require("AlarmPlayback")
local ticks = {}
local function timer(_, fn)
    local t = {fn=fn, start=function(self) self.running=true end,
        stop=function(self) self.running=false end}
    ticks[#ticks+1]=t; return t
end
local server = {id=migration.LOCAL_UUID}
local status = {}
local player = {volume=24, stops=0, tones=0,
    isLocal=function() return true end,
    getSlimServer=function() return server end,
    getPlayerStatus=function() return status end,
    getPlayMode=function() return "stop" end,
    getAlarmState=function(self) return self.alarmState end,
    setAlarmState=function(self,state) self.alarmState=state end,
    getVolume=function(self) return self.volume end,
    volumeLocal=function(self,v) self.volume=v end,
    stop=function(self) self.stops=self.stops+1 end,
    playFileInLoop=function(self,file)
        assert(file=="applets/AlarmSnooze/alarm.mp3"); self.tones=self.tones+1
    end}
local radio, alarmUI
local manager = {getAppletInstance=function(_,name)
        return name=="AlarmSnooze" and alarmUI or name=="StandaloneRadio" and radio or nil
    end,
    loadApplet=function() return radio end}
local poweredOff=0
local bridge = Bridge.new({manager=manager,timer=timer,log={warn=function() end,info=function() end},
    power={setSoftPowerState=function(_,state) assert(state=="off");poweredOff=poweredOff+1 end}})
bridge:setVolume(player,0,true); assert(player.volume==0)
player.volume=24
local function event(serial, name, source, fade)
    status={sb_alarm_serial=serial,sb_alarm_event=name,sb_alarm_source=source or "sb-tone://default",
        sb_alarm_volume=60,sb_alarm_fade=fade or 0,
        alarm_state=name=="start" and "active" or name=="snooze" and "snooze" or "set"}
    bridge:notify_playerAlarmState(player)
end
event(1,"start"); assert(player.tones==1 and player.volume==60)
event(1,"start"); assert(player.tones==1) -- duplicate notification is harmless
event(2,"snooze"); assert(player.stops==1 and player.volume==24)
event(3,"start"); assert(player.tones==2 and player.volume==60)
event(4,"stop"); assert(player.stops==2 and player.volume==24)
event(5,"start",nil,1); assert(player.volume==1)
for i=1,20 do ticks[#ticks].fn() end
assert(player.volume==60 and not ticks[#ticks].running)
event(6,"dismiss"); assert(player.volume==60 and player.stops==2)
event(7,"start",nil,1); local fade=ticks[#ticks]
player.volume=31; fade.fn(); assert(not fade.running and player.volume==31)
event(8,"stop"); assert(player.volume==60)
-- Handoff cannot stop playback or restore volume on the newly selected LMS.
event(9,"start",nil,1); local stops=player.stops
server={id="external-lms"}; bridge:notify_playerConnected(player)
assert(not ticks[#ticks].running and player.stops==stops)
event(10,"start"); assert(player.stops==stops)
server={id=migration.LOCAL_UUID}
local stream={ownsPlayback=true, playing=true, stops=0,
    isPlaying=function(self) return self.playing end,
    getCurrentStation=function() return {name="saved station"} end,
    stop=function(self) self.stops=self.stops+1; self.ownsPlayback=false end}
radio={streamPlayer=stream,plays=0,
    presetStore={getPreset=function(_,n) return n==2 and {name="Preset 2"} end},
    _playStation=function(self,station) self.plays=self.plays+1; self.station=station; stream.ownsPlayback=true end}
event(11,"start","CURRENT_PLAYLIST"); assert(radio.plays==0 and stream.stops==0)
event(12,"snooze"); assert(stream.stops==1)
stream.playing=false
event(13,"start","CURRENT_PLAYLIST"); assert(radio.plays==1)
event(14,"stop"); assert(stream.stops==2)
event(15,"start","sbpreset://2"); assert(radio.station.name=="Preset 2")
event(16,"stop")
local tones=player.tones
event(17,"start","sbpreset://3"); assert(player.tones==tones+1)
event(18,"stop")
-- A stale persisted start received after timeout may not replay audio.
status={sb_alarm_serial=99,sb_alarm_event="start",alarm_state="set"}
tones=player.tones; bridge:notify_playerAlarmState(player); assert(player.tones==tones)
-- A Jive restart during a native active alarm recovers the stock popup only
-- when its RTC timer is no longer running (no second window at normal due time).
local opened, hidden, rtcRunning = 0, 0, true
alarmUI={RTCAlarmTimer={isRunning=function() return rtcRunning end},
    openAlarmWindow=function() opened=opened+1 end,
    _stopDecodeStatePoller=function() end, _stopSnoozeTimer=function() end,
    _hideAlarmWindow=function() hidden=hidden+1 end}
event(100,"start"); assert(opened==0)
event(101,"stop")
rtcRunning=false
event(102,"start"); assert(opened==1 and alarmUI.alarmInProgress=="rtc")
event(103,"stop"); assert(hidden==1 and alarmUI.alarmInProgress==nil)
local nativeSnoozes, fallbacks = 0, 0
alarmUI.soundFallbackAlarm=function()
    fallbacks=fallbacks+1; player.alarmState="active_fallback"
end
alarmUI._alarmSnooze=function()
    if player.alarmState=="active" then nativeSnoozes=nativeSnoozes+1 end
end
event(104,"start","sbpreset://2",1)
assert(stream.ownsPlayback)
alarmUI:soundFallbackAlarm()
assert(not stream.ownsPlayback and not ticks[#ticks].running and fallbacks==1)
alarmUI:_alarmSnooze(); assert(nativeSnoozes==1)
-- The instance hooks are inert for alarms from a different server.
server={id="external-lms"}; stream.ownsPlayback=true
player.alarmState="active_fallback"
alarmUI:soundFallbackAlarm(); alarmUI:_alarmSnooze()
assert(stream.ownsPlayback and fallbacks==2 and nativeSnoozes==1)
server={id=migration.LOCAL_UUID}
status={sleep=30,sb_sleep_serial=0};bridge:notify_playerSleepChange(player)
local beforeSleep=player.stops
status={sleep=0,sb_sleep_serial=1};bridge:notify_playerSleepChange(player)
assert(player.stops==beforeSleep+1)
assert(poweredOff==1)
bridge:notify_playerSleepChange(player);assert(player.stops==beforeSleep+1)
local fresh=Bridge.new({manager=manager,timer=timer,log={warn=function() end,info=function() end}})
fresh:notify_playerSleepChange(player);assert(player.stops==beforeSleep+1)
server={id="external-lms"};status.sb_sleep_serial=2
bridge:notify_playerSleepChange(player);assert(player.stops==beforeSleep+1)
print("alarm playback and sleep expiry/handoff tests passed")
