#!/usr/bin/env python3
import http.client
import json
import shlex
import socket
import subprocess
import sys
import time


PLAYER = "02:00:00:00:00:01"


def verify_discovery_and_slimproto():
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.settimeout(2)
    udp.sendto(b"eVERS\0", ("127.0.0.1", 3483))
    response, _ = udp.recvfrom(1024)
    udp.close()
    assert response.startswith(b"EVERS\x097.999.999")

    slim = socket.create_connection(("127.0.0.1", 3483), timeout=2)
    payload = bytes([0, 0, 2, 0, 0, 0, 0, 1])
    slim.sendall(b"HELO" + len(payload).to_bytes(4, "big") + payload)
    reply = slim.recv(64)
    slim.close()
    assert len(reply) >= 6 and reply[2:6] == b"strm"


def post(payload, path="/cometd"):
    connection = http.client.HTTPConnection("127.0.0.1", 9000, timeout=2)
    body = json.dumps(payload if isinstance(payload, list) else [payload])
    connection.request("POST", path, body, {"Content-Type": "application/json"})
    response = connection.getresponse()
    raw = response.read()
    status = response.status
    connection.close()
    return status, json.loads(raw)


def api(method, path, payload=None):
    connection = http.client.HTTPConnection("127.0.0.1", 9000, timeout=2)
    body = b"" if payload is None else json.dumps(payload, ensure_ascii=False).encode("utf-8")
    connection.request(method, path, body, {"Content-Type": "application/json"})
    response = connection.getresponse()
    result = json.loads(response.read())
    status = response.status
    connection.close()
    return status, result


def handshake():
    status, body = post({"channel": "/meta/handshake", "mac": PLAYER})
    assert status == 200 and body[0]["successful"]
    return body[0]["clientId"]


class EventStream:
    def __init__(self, client_id):
        self.client_id = client_id
        self.socket = socket.create_connection(("127.0.0.1", 9000), timeout=2)
        body = json.dumps([
            {"channel": "/meta/connect", "clientId": client_id},
            {"channel": "/meta/subscribe", "clientId": client_id,
             "subscription": f"/{client_id}/**"},
        ]).encode()
        request = (
            b"POST /cometd HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            b"Content-Type: application/json\r\nContent-Length: "
            + str(len(body)).encode()
            + b"\r\nConnection: keep-alive\r\n\r\n"
            + body
        )
        self.socket.sendall(request)
        self.buffer = b""
        while b"\r\n\r\n" not in self.buffer:
            self.buffer += self.socket.recv(4096)
        header, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
        assert b"200 OK" in header and b"Transfer-Encoding: chunked" in header
        initial = self.read_chunk(2)
        assert initial[0]["channel"] == "/meta/connect"

    def read_chunk(self, timeout):
        self.socket.settimeout(timeout)
        while b"\r\n" not in self.buffer:
            self.buffer += self.socket.recv(4096)
        line, self.buffer = self.buffer.split(b"\r\n", 1)
        size = int(line, 16)
        while len(self.buffer) < size + 2:
            self.buffer += self.socket.recv(4096)
        payload, self.buffer = self.buffer[:size], self.buffer[size + 2:]
        return json.loads(payload)

    def next_playerstatus(self, timeout=3, forbid_serverstatus=False):
        deadline = time.time() + timeout
        while time.time() < deadline:
            events = self.read_chunk(max(0.1, deadline - time.time()))
            for event in events:
                if forbid_serverstatus:
                    assert "/slim/serverstatus" not in event.get("channel", "")
                if "/slim/playerstatus/" in event.get("channel", ""):
                    return event
        raise AssertionError("playerstatus event not received")

    def assert_no_playerstatus(self, timeout=0.5):
        deadline = time.time() + timeout
        try:
            while time.time() < deadline:
                events = self.read_chunk(max(0.05, deadline - time.time()))
                assert all("/slim/playerstatus/" not in event.get("channel", "") for event in events)
        except socket.timeout:
            pass

    def close(self):
        self.socket.close()


def subscribe(client_id):
    channel = f"/{client_id}/slim/playerstatus/{PLAYER}"
    status, body = post({
        "channel": "/slim/subscribe",
        "clientId": client_id,
        "id": 7,
        "data": {
            "request": [PLAYER, ["status", "-", 10, "menu:menu", "useContextMenu:1", "subscribe:600"]],
            "response": channel,
        },
    })
    assert status == 200
    event = next(item for item in body if item.get("channel") == channel)
    return event["data"]


def request_serverstatus(client_id):
    status, body = post({
        "channel": "/slim/request", "clientId": client_id, "id": 8,
        "data": {"request": ["", ["serverstatus", 0, 999]],
                 "response": f"/{client_id}/slim/request"},
    })
    assert status == 200
    data = next(item["data"] for item in body if "data" in item)
    assert data["player count"] == 1 and data["players_loop"][0]["playerid"] == PLAYER


def wait_ready():
    for _ in range(60):
        try:
            connection = http.client.HTTPConnection("127.0.0.1", 9000, timeout=0.1)
            connection.request("GET", "/health")
            response = connection.getresponse()
            response.read()
            connection.close()
            if response.status == 200:
                return
        except OSError:
            time.sleep(0.05)
    raise AssertionError("sbbase did not start")


def run_server(binary, demo):
    command = shlex.split(binary) + (["--test-nowplaying"] if demo else [])
    process = subprocess.Popen(command, stderr=subprocess.PIPE)
    wait_ready()
    return process


def stop_server(process):
    process.terminate()
    _, errors = process.communicate(timeout=3)
    assert b"ACCEPT REJECT" not in errors
    return errors


binary = sys.argv[1]

# Production mode remains stopped and exposes no test endpoint.
process = run_server(binary, False)
try:
    verify_discovery_and_slimproto()
    cid = handshake()
    stream = EventStream(cid)
    data = subscribe(cid)
    assert data["mode"] == "stop"
    assert data["playlist_tracks"] == 0 and data.get("item_loop", []) == []
    status, _ = post({}, "/test/next-track")
    assert status == 404
    request_serverstatus(cid)

    # Real API updates drive the native subscription. Identity changes advance
    # the playlist timestamp; metadata and playback changes for one identity do
    # not. Exact duplicates are suppressed.
    status, claim = api("POST", "/api/nowplaying/claim",
                        {"source": "radio", "name": "Standalone Radio"})
    assert status == 201
    token = claim["session_id"]
    stream.next_playerstatus()

    def update(**values):
        payload = {"session_id": token}
        payload.update(values)
        assert api("POST", "/api/nowplaying/update", payload)[0] == 200

    update(state="buffering", live=True,
           track={"id": "station:a", "station": "Station A",
                  "title": "Old title", "artist": "Old artist"})
    a = stream.next_playerstatus(forbid_serverstatus=True)["data"]
    assert a["mode"] == "stop" and a["current_title"] == "Station A"
    rev_a = a["playlist_timestamp"]

    update(state="playing", live=True,
           track={"id": "station:a", "station": "Station A",
                  "title": "New title", "artist": "New artist"})
    meta = stream.next_playerstatus(forbid_serverstatus=True)["data"]
    assert meta["mode"] == "play" and meta["playlist_timestamp"] == rev_a
    assert meta["item_loop"][0]["track"] == "New title"

    # A different identity with omitted metadata must clear the previous
    # station's title/artist instead of leaking it into the new station.
    update(state="buffering", live=True,
           track={"id": "station:b", "station": "Station B"})
    b = stream.next_playerstatus(forbid_serverstatus=True)["data"]
    assert b["playlist_timestamp"] > rev_a
    assert b["item_loop"][0]["track"] == ""
    assert b["item_loop"][0]["artist"] == ""
    assert b["item_loop"][0]["album"] == "Station B"
    rev_b = b["playlist_timestamp"]
    update(state="playing", live=True,
           track={"id": "station:b", "station": "Station B"})
    playing = stream.next_playerstatus(forbid_serverstatus=True)["data"]
    assert playing["playlist_timestamp"] == rev_b

    update(state="playing", live=True,
           track={"id": "station:b", "station": "Station B"})
    stream.assert_no_playerstatus()

    update(track={"id": "station:b", "title": "Björk — Jóga",
                  "artist": "Björk"})
    unicode_status = stream.next_playerstatus()["data"]
    assert unicode_status["playlist_timestamp"] == rev_b
    assert unicode_status["item_loop"][0]["track"] == "Björk — Jóga"
    assert unicode_status["item_loop"][0]["artist"] == "Björk"
    assert unicode_status["item_loop"][0]["album"] == "Station B"
    assert unicode_status["item_loop"][0]["text"] == "Björk — Jóga\nBjörk - Station B"

    update(track={"id": "station:b", "artist": "Björk & Strings"})
    artist_only = stream.next_playerstatus(forbid_serverstatus=True)["data"]
    assert artist_only["playlist_timestamp"] == rev_b
    assert artist_only["item_loop"][0]["artist"] == "Björk & Strings"
    assert artist_only["item_loop"][0]["track"] == "Björk — Jóga"

    # Remote URLs are carried verbatim in the LMS-standard item icon field.
    # Artwork-only changes publish playerstatus without changing identity.
    public_art = "http://covers.example/cover%20one.jpg?size=300&name=\"radio\""
    update(track={"id": "station:b", "artwork_url": public_art})
    art = stream.next_playerstatus(forbid_serverstatus=True)["data"]
    assert art["playlist_timestamp"] == rev_b
    assert art["item_loop"][0]["icon"] == public_art

    update(track={"id": "station:b", "artwork_url": public_art})
    stream.assert_no_playerstatus()

    proxy_art = "http://127.0.0.1:8765/https/example.com/album%20cover.jpg?q=a%26b"
    update(track={"id": "station:b", "artwork_url": proxy_art})
    album_art = stream.next_playerstatus(forbid_serverstatus=True)["data"]
    assert album_art["playlist_timestamp"] == rev_b
    assert album_art["item_loop"][0]["icon"] == proxy_art

    station_logo = "http://logos.example/station-b.png"
    update(track={"id": "station:b", "artwork_url": station_logo})
    logo = stream.next_playerstatus(forbid_serverstatus=True)["data"]
    assert logo["item_loop"][0]["icon"] == station_logo

    update(track={"id": "station:b", "artwork_url": ""})
    removed = stream.next_playerstatus(forbid_serverstatus=True)["data"]
    assert removed["item_loop"][0]["icon"] == ""
    assert removed["playlist_timestamp"] == rev_b

    # Re-add artwork so the next real station switch proves that identity
    # replacement clears stale art even when the new update omits it.
    update(track={"id": "station:b", "artwork_url": station_logo})
    stream.next_playerstatus(forbid_serverstatus=True)

    last_revision = rev_b
    for number in range(12):
        update(state="playing", live=True,
               track={"id": f"station:rapid:{number}",
                      "station": f"Rapid {number}"})
        rapid = stream.next_playerstatus()["data"]
        assert rapid["playlist_timestamp"] > last_revision
        assert rapid["item_loop"][0]["icon"] == ""
        assert rapid["item_loop"][0]["album"] == f"Rapid {number}"
        last_revision = rapid["playlist_timestamp"]

    # Reconnect with the same Bayeux id inherits the subscription and receives
    # the latest complete snapshot without requiring another producer update.
    replacement = EventStream(cid)
    replay = replacement.next_playerstatus()["data"]
    assert replay["item_loop"][0]["params"]["track_id"] == "station:rapid:11"

    assert api("POST", "/api/nowplaying/release", {"session_id": token})[0] == 200
    stopped = replacement.next_playerstatus()["data"]
    assert stopped["playlist_tracks"] == 0
    replacement.close()
    stream.close()
finally:
    errors = stop_server(process)
    assert b"NOWPLAYING" not in errors

# Demonstration mode publishes only to explicitly subscribed stream clients.
process = run_server(binary, True)
try:
    first_id, unrelated_id = handshake(), handshake()
    first_stream, unrelated_stream = EventStream(first_id), EventStream(unrelated_id)
    first = subscribe(first_id)
    assert first["mode"] == "play" and first["remote"] == 0
    assert first["playlist_tracks"] == 1 and first["playlist_cur_index"] == 0
    assert len(first["item_loop"]) == 1
    assert first["item_loop"][0]["params"]["track_id"] == "900001"
    assert first["item_loop"][0]["track"] == "Sultans of Swing"
    assert first["item_loop"][0]["artist"] == "Dire Straits"
    assert first["item_loop"][0]["album"] == "Dire Straits"
    request_serverstatus(first_id)

    status, result = post({}, "/test/next-track")
    assert status == 200 and result["status"] == "ok"
    changed = first_stream.next_playerstatus()["data"]
    assert changed["item_loop"][0]["params"]["track_id"] == "900002"
    assert changed["item_loop"][0]["track"] == "Money for Nothing"
    assert changed["playlist_timestamp"] > first["playlist_timestamp"]
    unrelated_stream.assert_no_playerstatus()

    # Replacing the active stream and re-subscribing returns current state.
    replacement = EventStream(first_id)
    inherited = replacement.next_playerstatus()["data"]
    assert inherited["item_loop"][0]["params"]["track_id"] == "900002"
    current = subscribe(first_id)
    assert current["item_loop"][0]["params"]["track_id"] == "900002"
    status, _ = post({
        "channel": "/slim/unsubscribe", "clientId": first_id, "id": 9,
        "data": {"unsubscribe": f"/{first_id}/slim/playerstatus/{PLAYER}"},
    })
    assert status == 200
    status, _ = post({}, "/test/next-track")
    assert status == 200
    replacement.assert_no_playerstatus()
    replacement.close()
    first_stream.close()
    unrelated_stream.close()
finally:
    errors = stop_server(process)
    assert b"NOWPLAYING TEST enabled" in errors
    assert b"NOWPLAYING TRACK changed" in errors
    assert b"NOWPLAYING PUSH" in errors

print("nowplaying-poc-tests-ok")
