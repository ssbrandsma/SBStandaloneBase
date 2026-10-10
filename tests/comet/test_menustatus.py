#!/usr/bin/env python3
import http.client
import json
import shlex
import subprocess
import sys
import time

PLAYER = "00:04:20:29:16:7f"

def post(payload):
    connection = http.client.HTTPConnection("127.0.0.1", 9000, timeout=2)
    body = json.dumps([payload])
    connection.request("POST", "/cometd", body,
                       {"Content-Type": "application/json"})
    response = connection.getresponse()
    data = json.loads(response.read())
    connection.close()
    assert response.status == 200
    return data

process = subprocess.Popen(shlex.split(sys.argv[1]), stderr=subprocess.PIPE)
try:
    for _ in range(40):
        try:
            health = http.client.HTTPConnection("127.0.0.1", 9000,
                                                timeout=0.2)
            health.request("GET", "/health")
            assert health.getresponse().status == 200
            health.close()
            break
        except OSError:
            time.sleep(0.05)
    else:
        raise AssertionError("sbbase did not start")

    client_id = post({"channel": "/meta/handshake",
                      "ext": {"mac": PLAYER}})[0]["clientId"]
    response_path = f"/{client_id}/slim/menustatus/{PLAYER}"
    events = post({
        "id": 7, "channel": "/slim/subscribe", "clientId": client_id,
        "data": {"response": response_path,
                 "request": [PLAYER, ["menustatus"]]},
    })
    assert events[0] == {"id": 7, "channel": "/slim/request",
                         "successful": True}
    assert events[1]["channel"] == response_path
    assert events[1]["data"] == ["menustatus", [], "add", PLAYER]

    events = post({
        "id": 8, "channel": "/slim/request", "clientId": client_id,
        "data": {"response": f"/{client_id}/slim/request",
                 "request": [PLAYER, ["menu", 0, 100, "direct:1"]]},
    })
    assert events[1]["data"] == {"count": 0, "offset": 0,
                                  "item_loop": []}
finally:
    process.terminate()
    _, errors = process.communicate(timeout=3)
    if process.returncode not in (0, -15):
        sys.stderr.buffer.write(errors)
