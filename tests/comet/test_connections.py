#!/usr/bin/env python3
import http.client
import json
import socket
import shlex
import subprocess
import sys
import time


def post(payload):
    connection = http.client.HTTPConnection("127.0.0.1", 9000, timeout=2)
    body = json.dumps([payload])
    connection.request("POST", "/cometd", body, {"Content-Type": "application/json"})
    response = connection.getresponse()
    data = json.loads(response.read())
    expected_connection = ("close" if payload["channel"] == "/meta/disconnect"
                           else "keep-alive")
    assert response.getheader("Connection") == expected_connection
    connection.close()
    assert response.status == 200
    return data


def stream(client_id):
    body = json.dumps([{"channel": "/meta/connect", "clientId": client_id}]).encode()
    header = (
        b"POST /cometd HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        b"Content-Type: application/json\r\nTransfer-Encoding: chunked\r\n"
        b"Connection: keep-alive\r\n\r\n"
    )
    chunk = format(len(body), "x").encode() + b"\r\n" + body + b"\r\n"
    sock = socket.create_connection(("127.0.0.1", 9000), timeout=2)
    sock.sendall(header)
    time.sleep(0.05)
    sock.sendall(chunk)
    received = b""
    while b"\r\n\r\n" not in received or client_id.encode() not in received:
        received += sock.recv(8192)
    assert b"200 OK" in received and client_id.encode() in received
    return sock


def reused_handshake_stream():
    sock = socket.create_connection(("127.0.0.1", 9000), timeout=2)
    handshake = json.dumps([{"channel": "/meta/handshake"}]).encode()
    request = (
        b"POST /cometd HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        b"Content-Type: application/json\r\nContent-Length: "
        + str(len(handshake)).encode()
        + b"\r\nConnection: keep-alive\r\n\r\n"
        + handshake
    )
    sock.sendall(request)
    received = b""
    while b"\r\n\r\n" not in received:
        received += sock.recv(4096)
    header, body = received.split(b"\r\n\r\n", 1)
    length = int(next(line.split(b":", 1)[1] for line in header.split(b"\r\n") if line.lower().startswith(b"content-length:")))
    while len(body) < length:
        body += sock.recv(4096)
    client_id = json.loads(body[:length])[0]["clientId"]

    connect = json.dumps([
        {"channel": "/meta/connect", "clientId": client_id},
        {"channel": "/meta/subscribe", "clientId": client_id, "subscription": f"/{client_id}/**"},
    ]).encode()
    sock.sendall(
        b"POST /cometd HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        b"Content-Type: application/json\r\nContent-Length: "
        + str(len(connect)).encode()
        + b"\r\nConnection: keep-alive\r\n\r\n"
        + connect
    )
    response = b""
    while b"/meta/connect" not in response:
        response += sock.recv(8192)
    assert client_id.encode() in response
    return sock


process = subprocess.Popen(shlex.split(sys.argv[1]), stderr=subprocess.PIPE)
try:
    for _ in range(40):
        try:
            health = http.client.HTTPConnection("127.0.0.1", 9000, timeout=0.2)
            health.request("GET", "/health")
            assert health.getresponse().status == 200
            health.close()
            break
        except OSError:
            time.sleep(0.05)
    else:
        raise AssertionError("sbbase did not start")

    reused_stream = reused_handshake_stream()
    reused_stream.close()

    first = post({"channel": "/meta/handshake"})[0]["clientId"]
    second = post({"channel": "/meta/handshake"})[0]["clientId"]
    assert first != second
    first_stream = stream(first)
    second_stream = stream(second)
    replacement = stream(first)
    time.sleep(0.1)
    first_stream.settimeout(1)
    assert first_stream.recv(1) == b"", "replaced stream was not closed"

    replacement.settimeout(5)
    heartbeat = b""
    while b"[]" not in heartbeat:
        heartbeat += replacement.recv(256)
    assert b"[]" in heartbeat, "stream heartbeat was not received"

    for _ in range(48):
        health = http.client.HTTPConnection("127.0.0.1", 9000, timeout=2)
        health.request("GET", "/health")
        response = health.getresponse()
        response.read()
        health.close()
        assert response.status == 200

    second_stream.close()
    replacement.close()
finally:
    process.terminate()
    _, errors = process.communicate(timeout=3)
    assert b"ACCEPT REJECT" not in errors
