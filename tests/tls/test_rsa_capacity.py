#!/usr/bin/env python3
"""Isolate TLS RSA key-size support from DNS, WAN, certificate trust and Jive."""
import http.server
import pathlib
import ssl
import subprocess
import sys
import tempfile
import threading


class Handler(http.server.BaseHTTPRequestHandler):
    def do_HEAD(self):
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *_):
        pass


if len(sys.argv) not in (2, 3):
    raise SystemExit("Usage: test_rsa_capacity.py [old_probe] fixed_probe")

with tempfile.TemporaryDirectory(prefix="sb-tls-rsa-") as work:
    work = pathlib.Path(work)
    for bits in (2048, 4096):
        cert, key = work / f"{bits}.crt", work / f"{bits}.key"
        subprocess.run(["openssl", "req", "-x509", "-newkey", f"rsa:{bits}",
                        "-nodes", "-subj", "/CN=localhost", "-days", "1",
                        "-keyout", str(key), "-out", str(cert)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for tls in ("--tls12", "--tls13"):
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(cert, key)
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            server.socket = context.wrap_socket(server.socket, server_side=True)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                for index, binary in enumerate(sys.argv[1:]):
                    result = subprocess.run(["qemu-arm", "-cpu", "arm926", binary,
                                             f"https://127.0.0.1:{server.server_port}/", "--head", tls],
                                            capture_output=True, text=True, timeout=15)
                    summary = result.stdout.strip().splitlines()[-1]
                    print(f"rsa={bits} tls={tls} binary={binary}: {summary}", flush=True)
                    success = result.returncode == 0 and "status=200" in summary
                    expected = bits == 2048 or len(sys.argv) == 2 or index > 0
                    assert success == expected, result.stdout + result.stderr
            finally:
                server.shutdown()
                server.server_close()
                thread.join()
print("RSA-2048/4096 TLS capacity regression passed")
