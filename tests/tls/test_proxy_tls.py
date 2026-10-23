#!/usr/bin/env python3
"""Local-only proxy TLS/redirect/timeout regression with an RSA-4096 server."""
import concurrent.futures
import http.client
import http.server
import os
import pathlib
import socket
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading
import time


class Handler(http.server.BaseHTTPRequestHandler):
    def do_HEAD(self):
        self.reply(False)

    def do_GET(self):
        self.reply(True)

    def reply(self, body):
        if self.path == "/redirect-http":
            destination = f"http://127.0.0.1:{plain.server_port}/ok"
        elif self.path == "/redirect-tls":
            destination = f"https://127.0.0.1:{secure.server_port}/ok"
        else:
            destination = None
        self.send_response(302 if destination else 200)
        self.send_header("Content-Type", "audio/mpeg" if self.path == "/stream" else "image/jpeg")
        if destination:
            self.send_header("Location", destination)
            self.send_header("Content-Length", "0")
        elif self.path != "/stream":
            self.send_header("Content-Length", "8192")
        self.end_headers()
        if body and not destination:
            try:
                if self.path == "/stream":
                    for _ in range(1000):
                        self.wfile.write(b"x" * 1024)
                        self.wfile.flush()
                        time.sleep(.01)
                else:
                    self.wfile.write(b"x" * 8192)
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                pass

    def log_message(self, *_):
        pass


class SilentHandler(socketserver.BaseRequestHandler):
    def handle(self):
        time.sleep(20)  # TCP works, but no TLS ServerHello is ever returned.


class SilentServer(socketserver.ThreadingTCPServer):
    daemon_threads = True


def request(path, method="GET", cancel=False, timeout=20):
    connection = http.client.HTTPConnection("127.0.0.1", proxy_port, timeout=timeout)
    try:
        connection.request(method, path)
        response = connection.getresponse()
        status = response.status
        data = response.read(1024) if cancel else response.read()
        return status, data
    finally:
        connection.close()


with tempfile.TemporaryDirectory(prefix="sb-proxy-tls-") as folder:
    folder = pathlib.Path(folder)
    key, cert = folder / "key.pem", folder / "cert.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:4096", "-nodes",
                    "-subj", "/CN=localhost", "-days", "1", "-keyout", str(key),
                    "-out", str(cert)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    plain = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    secure = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    secure.socket = context.wrap_socket(secure.socket, server_side=True)
    silent = SilentServer(("127.0.0.1", 0), SilentHandler)
    servers = [plain, secure, silent]
    for server in servers:
        threading.Thread(target=server.serve_forever, daemon=True).start()
    with socket.socket() as temporary:
        temporary.bind(("127.0.0.1", 0))
        proxy_port = temporary.getsockname()[1]
    environment = dict(os.environ, SB_PROXY_TEST_PORTS=f"{plain.server_port},{secure.server_port}")
    command = sys.argv[1:] + ["--listen", f"127.0.0.1:{proxy_port}"]
    assert len(sys.argv) > 1, "Supply the proxy command (e.g. qemu-arm sbproxy_test)"
    log = (folder / "proxy.log").open("wb")
    proxy = subprocess.Popen(command, env=environment, stdout=log, stderr=log)
    try:
        for _ in range(100):
            try:
                if request("/health")[0] == 200:
                    break
            except OSError:
                time.sleep(.05)
        else:
            raise AssertionError("proxy startup failed")
        https = f"/https/127.0.0.1:{secure.server_port}"
        plain_path = f"/http/127.0.0.1:{plain.server_port}"
        assert request(https + "/ok", "HEAD")[0] == 200
        # Self-signed and wrong-host: accepted according to existing policy.
        assert request(https + "/ok") == (200, b"x" * 8192)
        assert request(https + "/redirect-tls")[0] == 200
        assert request(plain_path + "/redirect-tls")[0] == 200
        assert request(https + "/redirect-http")[0] == 200
        for _ in range(6):
            assert request(https + "/ok", "HEAD")[0] == 200
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
            results = list(executor.map(request, [https + "/ok"] * 3))
            assert all(status == 200 for status, _ in results)
        assert request(https + "/stream", cancel=True)[0] == 200
        assert request(https + "/ok")[0] == 200
        started = time.monotonic()
        status, _ = request(f"/https/127.0.0.1:{silent.server_address[1]}/", timeout=20)
        elapsed = time.monotonic() - started
        assert status == 502 and 13 <= elapsed <= 19, (status, elapsed)
        assert request(https + "/ok")[0] == 200
        assert request("/health")[0] == 200
        print("Proxy RSA-4096, self-signed policy, redirects, sequential/concurrent TLS, "
              "cancellation, handshake timeout and recovery passed")
    finally:
        proxy.terminate()
        proxy.wait(timeout=8)
        log.close()
        for server in servers:
            server.shutdown()
            server.server_close()
