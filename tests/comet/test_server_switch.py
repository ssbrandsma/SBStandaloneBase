#!/usr/bin/env python3
"""Exercise Jive's old-server connect command against a fake SlimProto player."""
import http.client
import json
import shlex
import socket
import struct
import subprocess
import sys
import time

PLAYER = "00:04:20:29:16:7f"


def post(command, player=PLAYER, background=False):
    connection = http.client.HTTPConnection("127.0.0.1", 9000, timeout=2)
    payload = {
        "id": 9, "channel": "/slim/request", "clientId": "test",
        "data": {"response": "/test/slim/request",
                 "request": [player, command]},
    }
    if background:
        del payload["id"]
    connection.request("POST", "/cometd", json.dumps([payload]),
                       {"Content-Type": "application/json"})
    response = connection.getresponse()
    assert response.status == 200
    result = json.loads(response.read())
    connection.close()
    return result


def exact(sock, length):
    data = b""
    while len(data) < length:
        part = sock.recv(length - len(data))
        assert part, "Unexpected SlimProto disconnect"
        data += part
    return data


def frame(sock):
    size = struct.unpack("!H", exact(sock, 2))[0]
    return exact(sock, size)


process = subprocess.Popen(shlex.split(sys.argv[1]), stderr=subprocess.DEVNULL)
slim = None
try:
    for _ in range(60):
        try:
            slim = socket.create_connection(("127.0.0.1", 3483), timeout=1)
            break
        except OSError:
            time.sleep(0.05)
    assert slim is not None, "sbbase did not start"
    slim.settimeout(2)
    helo = bytes([7, 0]) + bytes.fromhex(PLAYER.replace(":", ""))
    slim.sendall(b"HELO" + struct.pack("!I", len(helo)) + helo)
    assert frame(slim)[:4] == b"strm"

    # Actual Jive background requests omit id. This must still execute.
    response = post(["connect", "203.0.113.42"], background=True)
    assert response[0]["successful"] is True and "id" not in response[0]
    assert frame(slim) == b"serv" + socket.inet_aton("203.0.113.42")

    # Real Jive command shape; network byte order, no unwanted sync group.
    for destination in ("203.0.113.42", "192.168.1.141", "127.0.0.1"):
        response = post(["connect", destination])
        assert response[0]["successful"] is True
        assert frame(slim) == b"serv" + socket.inet_aton(destination)

    # Reject unsafe/special/malformed destinations and mismatched players.
    for destination in ("0.0.0.0", "0.0.0.1", "224.0.0.1", "255.255.255.255",
                        "localhost", "203.0.113.42:3483", "1.2.3", "1.2.3.999",
                        "1.2.3.4;reboot", "1" * 100, "1.2.3.4\n"):
        assert post(["connect", destination])[0]["successful"] is False
    assert post(["connect"])[0]["successful"] is False
    assert post(["connect", "192.168.1.141", "extra"])[0]["successful"] is False
    assert post(["connect", "192.168.1.141"],
                "00:04:20:00:00:01")[0]["successful"] is False

    # No rejected request should leave a serv frame queued.
    assert post(["status", "-", 1])[0]["successful"] is True
    post(["connect", "192.168.1.142"])
    assert frame(slim) == b"serv" + socket.inet_aton("192.168.1.142")
    slim.close()
    slim = None
    time.sleep(0.1)
    assert post(["connect", "192.168.1.141"])[0]["successful"] is False
    print("server-switch-tests-ok")
finally:
    if slim is not None:
        slim.close()
    process.terminate()
    process.wait(timeout=5)
