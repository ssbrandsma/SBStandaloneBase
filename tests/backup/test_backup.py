#!/usr/bin/env python3
"""Paired-browser backup/restore tests, confined to temporary host directories."""
import base64
import hashlib
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"auth"))
from test_web_pairing import request

binary,web=sys.argv[1:3]
with tempfile.TemporaryDirectory(prefix="sb-backup-test-") as temporary:
    root=Path(temporary); config=root/"base"; radio=root/"radio"; prefs=root/"prefs"
    for directory in (config,radio,prefs):directory.mkdir()
    port=20500+os.getpid()%1000;url=f"http://127.0.0.1:{port}"
    env=os.environ|{"SBWEBSERVER_CONFIG_DIR":str(config),"SB_RADIO_INTERNAL_ROOT":str(radio),"SB_BACKUP_JIVE_SETTINGS":str(prefs)}
    (config/"settings.conf").write_text(f"name=Original\nport={port}\nreadonly=0\n")
    (config/"management.token").write_text("must-never-be-exported")
    (config/"firmware.json").write_text('{"enabled":true,"private":"firmware-permission-must-not-migrate"}')
    (config/"alarms.conf").write_text("SBALARMS 1\noptions 1 50 0 540 3600 2 0 0 0 0 idle\nalarm 1 1 1 127 0 25200 -1 0 sb-tone://default\n")
    (radio/"stations.json").write_text('{"schemaVersion":1,"revision":0,"stations":[]}')
    (prefs/"StandaloneRadio.lua").write_text('settings = {presets={[2]={name="Station",url="https://example.org/audio",},},favorites={},lastPreset=2,}\n')
    (prefs/"AlarmSnooze.lua").write_text('settings = {alarmNext=1900000000,alarmRepeat=1,alarmDays="1111111"}\n')
    process=subprocess.Popen([binary,"--port",str(port),"--web-root",web,"--config-dir",str(config)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    try:
        for _ in range(80):
            try:socket.create_connection(("127.0.0.1",port),.1).close();break
            except OSError:time.sleep(.05)
        assert request(url,"/api/backup")[0]==401
        cap=(config/"remote.capability").read_text().strip()
        _,raw,_=request(url,"/api/internal/web-access/start","POST",b"",{"X-Remote-Capability":cap})
        _,_,headers=request(url,"/api/auth/pair","POST",{"pin":json.loads(raw)["pin"]})
        cookie=headers["Set-Cookie"].split(";",1)[0]
        status,raw,_=request(url,"/api/backup",cookie=cookie);assert status==200,(status,raw)
        backup=json.loads(raw);assert set(backup["files"])=={"settings","alarms","stations","radioPreferences"}
        assert "remote.capability" not in raw.decode() and "management" not in raw.decode()
        for entry in backup["files"].values():
            decoded=base64.b64decode(entry["data"])
            assert hashlib.sha256(decoded).hexdigest()==entry["sha256"]
            assert cap.encode() not in decoded and b"must-never-be-exported" not in decoded and b"firmware-permission-must-not-migrate" not in decoded
        def validate(document,headers=None):return request(url,"/api/backup/validate","POST",document,headers,cookie)[0]
        assert validate(backup)==200
        assert validate(backup|{"version":1.5})==400
        duplicate=raw.decode().replace('"version":1','"version":1,"version":1')
        assert validate(duplicate.encode())==400
        assert not list(config.glob("restore.*")),"Preview must clean staging"
        assert validate(backup,{"Origin":"http://evil.invalid"})==403
        oversized=http.client.HTTPConnection("127.0.0.1",port,timeout=3)
        oversized.putrequest("POST","/api/backup/validate")
        oversized.putheader("Cookie",cookie)
        oversized.putheader("Content-Type","application/json")
        oversized.putheader("Content-Length",str(768*1024+1));oversized.endheaders()
        assert oversized.getresponse().status==413
        oversized.close()
        corrupt=json.loads(raw);corrupt["files"]["alarms"]["sha256"]="0"*64;assert validate(corrupt)==400
        corrupt=json.loads(raw);corrupt["files"]["../settings"]=corrupt["files"]["settings"];assert validate(corrupt)==400
        def replace(name,data):
            document=json.loads(raw);document["files"][name]={"data":base64.b64encode(data).decode(),"sha256":hashlib.sha256(data).hexdigest()};return document
        for script in [b'settings = os.execute("reboot")',b'settings = {}; os.execute("reboot")',b'settings={x=(function() return 1 end)()}',b'settings={x=1+2}',b'settings={x=setmetatable({},{})}',b'settings={[nil]=1}',b'settings={function=1}']:
            assert validate(replace("radioPreferences",script))==400,script
        assert validate(replace("alarms",b"broken\n"))==400
        assert validate(replace("stations",b'{"schemaVersion":900}'))==400
        assert request(url,"/api/backup/restore","POST",backup,cookie=cookie)[0]==503,"Host must never restart services"
        # A trusted local helper fixture exercises actual atomic replacement.
        stage=config/"restore.host-test";stage.mkdir()
        for name,entry in backup["files"].items():(stage/name).write_bytes(base64.b64decode(entry["data"]))
        # The HTTP staging normalization is separately tested above. Simulate it here.
        normalized=(stage/"alarms").read_text().replace("options 1 ","options 0 ")
        (stage/"alarms").write_text(normalized)
        (config/"settings.conf").write_text(f"name=Changed\nport={port}\nreadonly=1\n")
        auth_before=(config/"remote.capability").read_bytes()
        result=subprocess.run([binary,"--apply-backup",str(stage)],env=env,capture_output=True)
        assert result.returncode==0,result.stderr
        settings=(config/"settings.conf").read_text();assert "name=Original" in settings and "readonly=1" in settings and f"port={port}" in settings
        assert (config/"remote.capability").read_bytes()==auth_before
        assert "options 0 " in (config/"alarms.conf").read_text()
        assert "alarmNext=false" in (prefs/"AlarmSnooze.lua").read_text()
        assert (prefs/"StandaloneRadio.lua").read_text().startswith("settings = ")
        assert validate(backup)==403,"Read-only policy survives restore"
        # Inject an I/O error on the final live-file rename. Earlier commits
        # must be rolled back, not left as a partially restored configuration.
        if len(sys.argv)>3:
            stage=config/"restore.commit-failure";stage.mkdir()
            for name,entry in backup["files"].items():(stage/name).write_bytes(base64.b64decode(entry["data"]))
            (stage/"settings").write_text("name=Must roll back\n")
            targets=[config/"settings.conf",config/"alarms.conf",radio/"stations.json",prefs/"StandaloneRadio.lua",prefs/"AlarmSnooze.lua"]
            previous={path:path.read_bytes() for path in targets}
            fault_env=env|{"LD_PRELOAD":sys.argv[3],"SB_BACKUP_TEST_RENAME_FAILURE":"1"}
            result=subprocess.run([binary,"--apply-backup",str(stage)],env=fault_env,capture_output=True)
            assert result.returncode!=0,result.stderr
            assert all(path.read_bytes()==data for path,data in previous.items()),"Partial commit was not rolled back"
        # Fail during staging: none of the other live files may change.
        prior=(config/"settings.conf").read_bytes();stage=config/"restore.failure-test";stage.mkdir()
        (stage/"settings").write_text("name=Must not apply\n")
        (stage/"radioPreferences").write_text("settings = {}")
        (prefs/"StandaloneRadio.lua.restore-new").write_text("occupied")
        result=subprocess.run([binary,"--apply-backup",str(stage)],env=env,capture_output=True)
        assert result.returncode!=0 and (config/"settings.conf").read_bytes()==prior
        print("backup export, validation, credential isolation, policy preservation and rollback passed")
    finally:
        process.terminate();process.wait(timeout=5)
