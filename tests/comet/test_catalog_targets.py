#!/usr/bin/env python3
"""Exercise model filtering through the real Comet applet installer API."""
import http.client
import json
import shlex
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def post(payload):
    connection = http.client.HTTPConnection("127.0.0.1", 9000, timeout=2)
    connection.request("POST", "/cometd", json.dumps([payload]),
                       {"Content-Type": "application/json"})
    response = connection.getresponse()
    result = json.loads(response.read())
    connection.close()
    assert response.status == 200
    return result


with tempfile.TemporaryDirectory() as directory:
    entries = [
        {"name": "StandaloneRadio", "target": "baby", "version": "0.9.18"},
        {"name": "StandaloneRadio", "target": "fab4", "version": "0.9.18"},
        {"name": "StandaloneBase", "target": "baby"},
        {"name": "Shared", "target": "*", "desc": 'braces { } and "quotes"'},
        {"name": "Untargeted"},
    ]
    config = Path(directory) / "config.json"
    config.write_text(json.dumps({"applets": entries}))
    process = subprocess.Popen(shlex.split(sys.argv[1]) + ["--config", str(config)],
                               stderr=subprocess.PIPE)
    try:
        for _ in range(60):
            try:
                client = post({"channel": "/meta/handshake"})[0]["clientId"]
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise AssertionError("sbbase did not start")
        for target in ["baby", "fab4", "unknown", None, "baby", "fab4"]:
            command = ["jiveapplets", "version:7.7.3"]
            if target is not None:
                command.append("target:" + target)
            events = post({"channel": "/slim/request", "clientId": client,
                           "data": {"response": "/" + client + "/slim/request",
                                    "request": ["", command]}})
            actual = next(event["data"] for event in events if "data" in event)
            model = target or "baby"
            expected = [entry for entry in entries
                        if entry.get("target", "*") in (model, "*")]
            assert actual == {"count": len(expected), "item_loop": expected}, actual
            assert sum(e["name"] == "StandaloneRadio"
                       for e in actual["item_loop"]) <= 1
    finally:
        process.terminate()
        _, errors = process.communicate(timeout=3)
        if process.returncode not in (0, -15):
            raise AssertionError(errors.decode())
print("Applet target filtering and per-request isolation passed")
