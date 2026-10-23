#!/usr/bin/env python3
import http.client
import json
import shlex
import subprocess
import sys
import time


def request(method, path, value=None):
    connection = http.client.HTTPConnection("127.0.0.1", 9000, timeout=3)
    body = b"" if value is None else json.dumps(value, ensure_ascii=False).encode("utf-8")
    connection.request(method, path, body, {"Content-Type": "application/json"})
    response = connection.getresponse()
    result = json.loads(response.read())
    status = response.status
    connection.close()
    return status, result


command = shlex.split(sys.argv[1])
process = subprocess.Popen(command, stderr=subprocess.PIPE)
try:
    for _ in range(60):
        try:
            if request("GET", "/api/nowplaying")[0] == 200:
                break
        except OSError:
            time.sleep(.05)
    else:
        raise AssertionError("server did not start: " + process.stderr.read().decode(errors="replace"))

    status, first = request("POST", "/api/nowplaying/claim", {
        "source": "spotify", "name": "Spotify Connect",
        "capabilities": {"pause": True, "next": True, "seek": True},
    })
    assert status == 201 and first["active"] and len(first["session_id"]) >= 32
    token = first["session_id"]

    status, _ = request("POST", "/api/nowplaying/update", {
        "session_id": token, "state": "playing", "position_ms": 1250,
        "track": {"id": "spotify:track:1", "title": "Beyoncé – Déjà Vu",
                  "artist": "Beyoncé", "album": "B'Day", "duration_ms": 240000},
    })
    assert status == 200
    time.sleep(.08)
    status, current = request("GET", "/api/nowplaying")
    assert current["state"] == "playing" and current["position_ms"] >= 1300
    assert current["track"]["title"] == "Beyoncé – Déjà Vu"

    # The real Comet dispatcher must reach the metadata commands before the
    # generic menu fallback (contextmenu requests contain menu:track).
    assert request("POST", "/api/nowplaying/update", {
        "session_id": token,
        "track": {"url": "http://example.org/serverstatus/jiveapplets",
                  "audio": {"codec": "flc", "sample_rate_hz": 96000,
                             "bit_depth": 24, "lossless": True}},
    })[0] == 200

    def query(command):
        status, messages = request("POST", "/cometd", {
            "channel": "/slim/request", "clientId": "metadata-test", "id": 90,
            "data": {"request": ["00:00:00:00:00:01", command],
                     "response": "/metadata-test/reply"},
        })
        assert status == 200
        return next(message["data"] for message in messages
                    if message.get("channel") == "/metadata-test/reply")

    selected = query(["status", "-", 1, "tags:oTIQ"])["playlist_loop"][0]
    assert selected["type"] == "flc" and selected["samplerate"] == 96000
    assert selected["samplesize"] == 24 and selected["lossless"] == 1
    assert "artist" not in selected
    info = query(["songinfo", 0, 100, "track_id:-1", "tags:I"])["songinfo_loop"]
    assert {"samplesize": 24} in info
    selected_by_url = query(["songinfo", 0, 100,
                             "url:http://example.org/serverstatus/jiveapplets", "tags:I"])
    assert {"samplesize": 24} in selected_by_url["songinfo_loop"]
    menu = query(["contextmenu", 0, 100, "menu:track", "context:playlist", "playlist_index:0"])
    assert menu["count"] == len(menu["item_loop"])
    assert "Sample size (bits): 24" in [row["text"] for row in menu["item_loop"]]

    status, second = request("POST", "/api/nowplaying/claim",
                             {"source": "radio", "name": "Standalone Radio"})
    assert status == 201 and second["generation"] > first["generation"]
    status, _ = request("POST", "/api/nowplaying/update",
                        {"session_id": token, "state": "stopped"})
    assert status == 409
    radio = second["session_id"]
    status, _ = request("POST", "/api/nowplaying/update", {
        "session_id": radio, "state": "playing", "live": True,
        "track": {"id": "radio:station:123", "station": "Radio Paradise",
                  "title": "Sultans of Swing", "artist": "Dire Straits"},
    })
    assert status == 200
    _, current = request("GET", "/api/nowplaying")
    assert current["live"] and not current["duration_known"]

    assert request("POST", "/api/nowplaying/heartbeat", {"session_id": radio})[0] == 200
    assert request("POST", "/api/nowplaying/release", {"session_id": radio})[0] == 200
    _, current = request("GET", "/api/nowplaying")
    assert not current["active"] and current["state"] == "stopped"

    assert request("POST", "/api/nowplaying/claim", {"source": 3, "name": "bad"})[0] == 400
    connection = http.client.HTTPConnection("127.0.0.1", 9000, timeout=3)
    connection.request("POST", "/api/nowplaying/claim", b"{broken", {"Content-Type": "application/json"})
    response = connection.getresponse()
    assert response.status == 400
    response.read()
    connection.close()
finally:
    process.terminate()
    _, errors = process.communicate(timeout=5)
    assert b"ACCEPT REJECT" not in errors

print("nowplaying-api-tests-ok")
