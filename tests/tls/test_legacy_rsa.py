#!/usr/bin/env python3
"""Validate bounded RSA/AES-GCM compatibility without enabling RSA/CBC."""
import http.server
import pathlib
import ssl
import subprocess
import sys
import tempfile
import threading


class Handler(http.server.BaseHTTPRequestHandler):
    def do_HEAD(self):
        self.server.negotiated.append(self.connection.cipher()[0])
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *_):
        pass


with tempfile.TemporaryDirectory(prefix="sb-tls-legacy-") as directory:
    root = pathlib.Path(directory)
    cert, key = root / "cert.pem", root / "key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048",
                    "-nodes", "-subj", "/CN=localhost", "-days", "1",
                    "-keyout", str(key), "-out", str(cert)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for cipher, allowed in [("AES128-GCM-SHA256", True),
                            ("AES256-GCM-SHA384", True),
                            ("AES256-SHA256", False),
                            ("ECDHE-RSA-AES128-GCM-SHA256", True)]:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.maximum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(cert, key)
        context.set_ciphers(cipher)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.negotiated = []
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            result = subprocess.run(["qemu-arm", "-cpu", "arm926", sys.argv[1],
                                     f"https://127.0.0.1:{server.server_port}/",
                                     "--head"], capture_output=True, text=True,
                                    timeout=15)
            success = result.returncode == 0 and "status=200" in result.stdout
            assert success == allowed, (cipher, result.stdout, result.stderr)
            assert server.negotiated == ([cipher] if allowed else [])
            print(f"{cipher}: {'accepted' if allowed else 'rejected'}", flush=True)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
print("Legacy RSA/AES-GCM-only compatibility regression passed")
