#!/usr/bin/env python3
"""Image endpoint integration: real workers, renderer, cache and HTTP client."""
import concurrent.futures
import http.server
import http.client
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

from test_renderer import decode
from test_raster import png as raster_png

SVG = b'<svg viewBox="0 0 100 50"><rect width="100" height="50" fill="red"/></svg>'


class Source(http.server.BaseHTTPRequestHandler):
    count = 0

    def do_GET(self):
        Source.count += 1
        if self.path.startswith('/raster'):
            data = raster_png(640, 480, lambda y: bytes([240, 20, 30, 255]) * 640)
            if self.path == '/raster-large':
                # Poorly compressing input above the former 1 MiB limit.
                import random
                generator = random.Random(42)
                data = raster_png(640, 480, lambda y: generator.randbytes(640 * 4))
            self.send_response(200)
            if self.path == '/raster-chunked':
                self.send_header('Transfer-Encoding', 'chunked')
            elif self.path != '/raster-close':
                self.send_header('Content-Length', str(len(data) + (10 if self.path == '/raster-truncated' else 0)))
            self.send_header('Connection', 'close')
            self.end_headers()
            if self.path == '/raster-chunked':
                for offset in range(0, len(data), 73):
                    part = data[offset:offset+73]
                    self.wfile.write(f'{len(part):x};test=1\r\n'.encode() + part + b'\r\n')
                self.wfile.write(b'0\r\n\r\n')
            else:
                self.wfile.write(data)
            return
        if self.path.startswith('/slow'):
            time.sleep(0.5)
        if self.path == '/timeout':
            time.sleep(13)
        if self.path == '/oversize':
            self.send_response(200)
            self.send_header('Content-Length', str(9 * 1024 * 1024))
            self.end_headers()
            return
        redirects = {'/redirect': '/logo', '/unsafe': 'http://127.0.0.1/private',
                     '/loop': '/loop', '/scheme': 'file:///etc/passwd'}
        self.send_response(302 if self.path in redirects else 404 if self.path == '/missing' else 200)
        if self.path in redirects:
            self.send_header('Location', redirects[self.path])
        self.send_header('Content-Length', '0' if self.path in redirects else str(len(SVG)))
        self.end_headers()
        if self.path not in redirects:
            try:
                self.wfile.write(SVG)
            except (BrokenPipeError, ConnectionResetError):
                pass  # Expected when the client enforces its deadline.

    def log_message(self, *args):
        pass


def main():
    executable, renderer, web = sys.argv[1:4]
    network = '--test-network' in sys.argv
    origin = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Source)
    threading.Thread(target=origin.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        config = root / 'config'
        config.mkdir()
        port_socket = socket.socket()
        port_socket.bind(('127.0.0.1', 0))
        port = port_socket.getsockname()[1]
        port_socket.close()
        base = f'http://127.0.0.1:{port}'
        wrapper = root / 'renderer-wrapper'
        marker = root / 'hang-renderer'
        wrapper.write_text('#!' + sys.executable + '\nimport os, sys, time\n'
                           + 'if os.path.exists(' + repr(str(marker)) + '): time.sleep(30)\n'
                           + 'os.execv(' + repr(str(pathlib.Path(renderer).resolve())) + ', ['
                           + repr(str(pathlib.Path(renderer).resolve())) + '] + sys.argv[1:])\n')
        wrapper.chmod(0o700)
        env = os.environ | {'SB_IMAGE_RENDERER': str(wrapper), 'SB_IMAGE_CACHE_ROOT': str(root / 'cache'),
                            'SB_IMAGE_TEST_IP': '127.0.0.1', 'SB_IMAGE_TEST_PORT': str(origin.server_port),
                            'SB_IMAGE_PROXY_URL': 'http://127.0.0.1:' + str(origin.server_port)}
        with (root / 'server.log').open('wb') as log:
            process = subprocess.Popen([executable, '--port', str(port), '--web-root', web,
                                        '--config-dir', str(config)], env=env, stderr=log, stdout=log)

            def request(path, data=None, headers=None, method=None):
                req = urllib.request.Request(base + path, data=data, headers=headers or {}, method=method)
                try:
                    with urllib.request.urlopen(req, timeout=25) as r:
                        return r.status, r.read(), dict(r.headers)
                except urllib.error.HTTPError as e:
                    return e.code, e.read(), dict(e.headers)

            def image(url='http://image.test/logo', suffix=''):
                return '/api/image/render?url=' + urllib.parse.quote(url, safe='') + suffix

            try:
                for _ in range(100):
                    try:
                        with socket.create_connection(('127.0.0.1', port), 0.1):
                            break
                    except OSError:
                        time.sleep(.05)
                cap = (config / 'remote.capability').read_text().strip()
                status, body, _ = request('/api/internal/web-access/start', b'', {'X-Remote-Capability': cap})
                assert status == 200
                pin = json.loads(body)['pin']
                status, _, headers = request('/api/auth/pair', json.dumps({'pin': pin}).encode(), {'Content-Type': 'application/json'})
                cookie = headers['Set-Cookie'].split(';')[0]
                upload = {'Content-Type': 'image/svg+xml', 'Cookie': cookie}
                status, png, headers = request('/api/image/render?width=64&height=64', SVG, upload)
                assert status == 200, (status, png, (root / 'server.log').read_text())
                assert decode(png)[:2] == (64, 64) and headers['X-SB-Image-Cache'] == 'miss'
                status, repeat, headers = request('/api/image/render?width=64&height=64', SVG, upload)
                assert status == 200 and repeat == png and headers['X-SB-Image-Cache'] == 'hit'
                cache_file = next((root / 'cache').glob('*.png'))
                os.utime(cache_file, (time.time() - 8 * 86400,) * 2)
                assert request('/api/image/render?width=64&height=64', SVG, upload)[2]['X-SB-Image-Cache'] == 'miss'
                assert request('/api/image/render', SVG, {'Content-Type': 'image/svg+xml'})[0] == 401
                assert request('/api/image/render', b'<svg><text>x</text></svg>', upload)[0] == 415
                connection = http.client.HTTPConnection('127.0.0.1', port, timeout=3)
                connection.putrequest('POST', '/api/image/render')
                connection.putheader('Content-Length', str(1024 * 1024 + 1))
                connection.endheaders()
                assert connection.getresponse().status == 413
                connection.close()
                assert request('/api/image/render?width=999', SVG, upload)[0] == 400
                assert request('/api/image/render', SVG, upload | {'Origin': 'http://evil.invalid'})[0] == 403
                assert request(image(), headers={'Host': 'evil.invalid'})[0] == 401
                assert request(image(), headers={'Origin': 'http://evil.invalid'})[0] == 401
                for url in ['http://127.0.0.1/a', 'http://10.0.0.1/a', 'http://169.254.169.254/a',
                            'http://2130706433/a', 'http://localhost/a']:
                    assert request(image(url))[0] == 403, url
                for url in ['file:///etc/passwd', 'ftp://example.com/a', 'http://user:pass@example.com/a',
                            'http://example.com:8080/a', 'http://[::1]/a']:
                    assert request(image(url))[0] == 400, url
                assert request(image(), method='DELETE')[0] == 405
                if network:
                    for path in ['/raster', '/raster-chunked', '/raster-close', '/raster-large']:
                        status, png, _ = request(image('http://image.test' + path))
                        assert status == 200 and decode(png)[:2] == (320, 240), (path, status, png)
                    assert request(image('http://image.test/raster-truncated'))[0] == 502
                    before = Source.count
                    assert request(image('https://image.test/logo'))[0] == 503
                    assert Source.count == before + 1  # Only /health; no unsafe fallback against an old proxy.
                    before = Source.count
                    status, png, headers = request(image())
                    assert status == 200, (status, png, (root / 'server.log').read_text())
                    assert Source.count == before + 1 and decode(png)[:2] == (320, 320)
                    assert request(image())[2]['X-SB-Image-Cache'] == 'hit'
                    assert Source.count == before + 1
                    status, body, headers = request(image(), method='HEAD')
                    assert status == 200 and not body and int(headers['Content-Length']) > 0
                    assert request(image('http://image.test/redirect'))[0] == 200
                    assert request(image('http://image.test/unsafe'))[0] == 403
                    assert request(image('http://image.test/scheme'))[0] == 403
                    assert request(image('http://image.test/loop'))[0] == 502
                    assert request(image('http://image.test/oversize'))[0] == 413
                    assert request(image('http://image.test/missing'))[0] == 502
                    assert request(image('http://image.test/timeout'))[0] == 504
                    with concurrent.futures.ThreadPoolExecutor(3) as pool:
                        before = Source.count
                        a = pool.submit(request, image('http://image.test/slow'))
                        time.sleep(.1)
                        b = pool.submit(request, image('http://image.test/slow'))
                        assert request(image('http://image.test/other'))[0] == 503
                        started = time.monotonic()
                        assert request('/api/auth/status')[0] == 200
                        assert time.monotonic() - started < .4
                        assert a.result()[0] == 200 and b.result()[0] == 200
                        assert Source.count == before + 1
                # Force eviction with well-named, old regular cache entries.
                for index in range(21):
                    entry = root / 'cache' / (f'{index:064x}' + '.png')
                    entry.write_bytes(b'X' * (1024 * 1024))
                    os.utime(entry, (time.time() - 1000 - index,) * 2)
                assert request('/api/image/render?width=65', SVG, upload)[0] == 200
                assert sum(p.stat().st_size for p in (root / 'cache').glob('*.png')) <= 20 * 1024 * 1024
                # Unavailable cache falls back to rendering, without an internal-storage cache.
                (root / 'cache').rename(root / 'saved-cache')
                (root / 'cache').write_text('not a directory')
                for _ in range(2):
                    result = request('/api/image/render?width=66', SVG, upload)
                    assert result[0] == 200 and result[2]['X-SB-Image-Cache'] == 'miss'
                (root / 'cache').unlink()
                (root / 'saved-cache').rename(root / 'cache')
                assert not list((root / 'cache').glob('.pending'))
                marker.touch()
                started = time.monotonic()
                assert request('/api/image/render?width=67', SVG, upload)[0] == 504
                assert 19 <= time.monotonic() - started < 24
                assert request('/api/auth/status')[0] == 200
                marker.unlink()
                assert request('/api/image/render?width=68', SVG, upload)[0] == 200
            finally:
                process.terminate()
                process.wait(timeout=3)
    origin.shutdown()
    print('svg-image-http-tests-ok')


if __name__ == '__main__':
    main()
