#!/usr/bin/env python3
"""Streaming/redirect tests; only the dedicated test binary permits these origins."""
import hashlib
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import parse_qs, urlsplit

BLOCK = b"jpeg-stream-test" * 4096


class Origin(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/slow":
            time.sleep(9)
        if path in ("/to-http", "/to-https", "/loop", "/relative", "/unsafe", "/ftp"):
            self.send_response(302)
            self.send_header("Location", {
                "/to-http": f"http://127.0.0.1:{http_origin.server_port}/ok",
                "/to-https": f"https://127.0.0.1:{https_origin.server_port}/to-http",
                "/loop": "/loop", "/relative": "ok",
                "/unsafe": "http://169.254.169.254/latest/meta-data/",
                "/ftp": "ftp://example.com/image.jpg"}[path])
            self.end_headers()
            return
        status = int(path[1:]) if path in ("/403", "/404") else 200
        if path == "/range":
            assert self.headers.get("Range") == "bytes=0-4"
            assert self.headers.get("Authorization") is None
            assert self.headers.get("Cookie") is None
            status = 206
        body = b"<svg xmlns='http://www.w3.org/2000/svg'/>" if path == "/mime" else b"unchanged-png"
        self.send_response(status)
        self.send_header("Content-Type", "image/svg+xml; charset=utf-8" if path == "/mime" else "image/png")
        if path == "/range":
            self.send_header("Content-Range", "bytes 0-4/13")
        self.send_header("Content-Length", str(len(BLOCK) * 80 if path == "/large" else len(body)))
        self.end_headers()
        if self.command == "HEAD":
            return
        try:
            if path == "/large":
                for _ in range(80):
                    self.wfile.write(BLOCK)
            else:
                self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass


with tempfile.TemporaryDirectory() as temporary:
    cert, key = Path(temporary) / "cert.pem", Path(temporary) / "key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj", "/CN=localhost"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    http_origin = ThreadingHTTPServer(("127.0.0.1", 0), Origin)
    https_origin = ThreadingHTTPServer(("127.0.0.1", 0), Origin)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    https_origin.socket = context.wrap_socket(https_origin.socket, server_side=True)
    for server in (http_origin, https_origin):
        threading.Thread(target=server.serve_forever, daemon=True).start()
    with socket.socket() as holder:
        holder.bind(("127.0.0.1", 0))
        port = holder.getsockname()[1]
    env = dict(os.environ, SB_PROXY_TEST_PORTS=f"{http_origin.server_port},{https_origin.server_port}")
    process = subprocess.Popen(sys.argv[1:] + ["--listen", f"127.0.0.1:{port}"], env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                with socket.create_connection(("127.0.0.1", port), .1):
                    break
            except OSError:
                time.sleep(.05)

        def request(scheme, path, method="GET", headers=None):
            upstream = http_origin.server_port if scheme == "http" else https_origin.server_port
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
            connection.request(method, f"/artwork/320/240/80/{scheme}/127.0.0.1:{upstream}{path}", headers=headers or {})
            return connection, connection.getresponse()

        for scheme, path in (("http", "/ok"), ("https", "/ok"), ("http", "/relative"),
                             ("http", "/to-https"), ("https", "/to-http")):
            connection, response = request(scheme, path)
            assert response.status == 200, (scheme, path, response.status)
            assert response.getheader("Content-Type") == "image/png"
            assert response.read() == b"unchanged-png"
            connection.close()
        for code in (403, 404):
            connection, response = request("http", f"/{code}")
            assert response.status == code
            response.read()
            connection.close()
        connection, response = request("http", "/loop")
        assert response.status == 502
        response.read()
        connection.close()
        for path in ("/unsafe", "/ftp"):
            connection, response = request("http", path)
            assert response.status == 502
            response.read()
            connection.close()
        connection, response = request("http", "/range", headers={"Range": "bytes=0-4",
            "Authorization": "Bearer private", "Cookie": "secret=private"})
        assert response.status == 206 and response.getheader("Content-Range") == "bytes 0-4/13"
        response.read()
        connection.close()
        connection, response = request("http", "/slow")
        assert response.status == 502
        response.read()
        connection.close()
        connection, response = request("http", "/ok", "HEAD")
        assert response.status == 200 and response.read() == b""
        connection.close()
        connection, response = request("https", "/mime?x=a%2Fb&token=q@r")
        assert response.status == 302
        location = urlsplit(response.getheader("Location"))
        query = parse_qs(location.query)
        assert location.path == "/api/image/render"
        assert query["url"] == [f"https://127.0.0.1:{https_origin.server_port}/mime?x=a%2Fb&token=q@r"]
        assert query["width"] == ["320"] and query["height"] == ["240"]
        assert response.read() == b""
        connection.close()
        connection, response = request("http", "/large")
        assert response.status == 200
        digest, count = hashlib.sha256(), 0
        while True:
            chunk = response.read(16384)
            if not chunk:
                break
            digest.update(chunk)
            count += len(chunk)
        assert count == len(BLOCK) * 80 and digest.digest() == hashlib.sha256(BLOCK * 80).digest()
        connection.close()
        for target in ("/artwork/999/1/80/http/example.com/a", "/http/user@example.com/a",
                       "/artwork/32/32/80/file/example.com/a"):
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
            connection.request("GET", target)
            response = connection.getresponse()
            assert response.status in (400, 404)
            response.read()
            connection.close()
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        connection.request("GET", "/artwork/320/320/80/http/no-artwork.invalid/image.jpg")
        response = connection.getresponse()
        assert response.status == 502
        response.read()
        connection.close()
    finally:
        process.terminate()
        process.wait(timeout=5)
        for server in (http_origin, https_origin):
            server.shutdown()
print("artwork-proxy-streaming-tests-ok")
