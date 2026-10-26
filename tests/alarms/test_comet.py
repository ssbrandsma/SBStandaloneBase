#!/usr/bin/env python3
"""Exercise alarm menus/scheduler through sbbase with a fake local Radio only."""
import http.client
import json
from pathlib import Path
import shlex
import socket
import struct
import subprocess
import sys
import tempfile
import time

PLAYER = "00:04:20:00:00:01"


def command(*arguments, player=PLAYER):
    connection = http.client.HTTPConnection("127.0.0.1", 9000, timeout=3)
    body = [{"id": 1, "channel": "/slim/request", "clientId": "alarm-test",
             "data": {"response": "/alarm-test/slim/request", "request": [player, list(arguments)]}}]
    connection.request("POST", "/cometd", json.dumps(body), {"Content-Type": "application/json"})
    response = connection.getresponse()
    data = json.loads(response.read())
    connection.close()
    assert response.status == 200 and data[0]["successful"], data
    return data[1]["data"]


def validate_menu(menu):
    assert menu["offset"] == 0
    assert menu["count"] == len(menu["item_loop"]), menu
    for item in menu["item_loop"]:
        if "item_loop" in item:
            validate_menu(item)


with tempfile.TemporaryDirectory(prefix="sb-alarm-comet-") as temporary:
    config = Path(temporary) / "config.json"
    config.write_text('{"applets":[]}', encoding="utf-8")
    process = subprocess.Popen(shlex.split(sys.argv[1]) + ["--config", str(config)],
                               stderr=subprocess.DEVNULL)
    slim = None
    try:
        for _ in range(60):
            try:
                slim = socket.create_connection(("127.0.0.1", 3483), timeout=1)
                break
            except OSError:
                time.sleep(0.05)
        assert slim is not None
        helo = bytes([7, 0]) + bytes.fromhex(PLAYER.replace(":", ""))
        slim.sendall(b"HELO" + struct.pack("!I", len(helo)) + helo)
        assert b"strm" in slim.recv(100)
        validate_menu(command("alarmsettings", 0, 100))
        root = command("menu", 0, 100)
        assert root["item_loop"][0]["actions"]["go"]["cmd"] == ["alarmsettings"]
        # Epoch support allows a deterministic short test without waiting for a minute.
        due = int(time.time()) + 3
        alarm = command("alarm", "add", f"time:{due}", "playlisturl:sbpreset://2")
        alarm_id = alarm["id"]
        for cmd in ("jiveupdatealarm", "jiveupdatealarmdays"):
            validate_menu(command(cmd, 0, 100, f"id:{alarm_id}"))
        validate_menu(command("alarm", "playlists", f"id:{alarm_id}", "menu:1"))
        assert "error" in command("alarm", "update", f"id:{alarm_id}", "time:-1")
        assert "error" in command("alarm", "update", f"id:{alarm_id}", "playlisturl:http://example.com")
        assert "error" in command("alarm", "disableall", player="00:04:20:00:00:02")
        while time.time() < due + 2:
            state = command("status", "-", 1)
            if state["alarm_state"] == "active":
                break
            time.sleep(0.1)
        assert state["alarm_state"] == "active", state
        assert state["sb_alarm_source"] == "sbpreset://2"
        assert state["sb_alarm_serial"] == 1
        command("jivealarm", "snooze:1")
        assert command("status", "-", 1)["alarm_state"] == "snooze"
        command("jivealarm", "stop:1")
        assert command("status", "-", 1)["alarm_state"] == "none"
        saved = Path(temporary) / "alarms.conf"
        assert saved.is_file() and saved.stat().st_mode & 0o777 == 0o600
        assert command("alarms", 0, 100, "filter:all")["count"] == 1
        command("alarm", "delete", f"id:{alarm_id}")
        assert command("alarms", 0, 100, "filter:all")["count"] == 0
        print("Comet alarm menus, scheduling, source validation and persistence passed")
    finally:
        if slim:
            slim.close()
        process.terminate()
        process.wait(timeout=5)
