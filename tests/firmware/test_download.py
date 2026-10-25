"""Both checksum formats through the real streaming transport, fixture network only."""
import hashlib
import http.client
import http.server
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import time
import zipfile

binary, webroot = sys.argv[1:]
image = io.BytesIO()
kernel, filesystem = b'kernel' * 1000, b'root' * 2000
md5 = (hashlib.md5(kernel).hexdigest() + '  zImage\n' +
       hashlib.md5(filesystem).hexdigest() + '  root.cramfs\n').encode()
with zipfile.ZipFile(image, 'w', zipfile.ZIP_DEFLATED) as archive:
    for name, data in [('jive.version', b'9.0.2 r17157'), ('board.version', b'baby:0005\n'),
                       ('upgrade.md5', md5), ('zImage', kernel), ('root.cramfs', filesystem)]:
        archive.writestr(name, data)
image = image.getvalue()
sha1, sha256 = hashlib.sha1(image).hexdigest(), hashlib.sha256(image).hexdigest()


class Origin(http.server.BaseHTTPRequestHandler):
    count = 0

    def do_GET(self):
        Origin.count += 1
        if self.path == '/redirect':
            self.send_response(302)
            self.send_header('Location', '/firmware.bin')
            self.end_headers()
            return
        self.send_response(200)
        if self.path == '/chunked':
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            for offset in range(0, len(image), 97):
                block = image[offset:offset + 97]
                self.wfile.write(('%x\r\n' % len(block)).encode() + block + b'\r\n')
            self.wfile.write(b'0\r\n\r\n')
            return
        self.send_header('Content-Length', str(len(image)))
        self.end_headers()
        self.wfile.write(image)

    def log_message(self, *args):
        pass


origin = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Origin)
threading.Thread(target=origin.serve_forever, daemon=True).start()
port, cookie = 19091, ''


def call(path, method='GET', payload=None, headers=None):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
    headers = dict(headers or {}, Cookie=cookie)
    if payload is not None:
        payload = json.dumps(payload).encode()
        headers['Content-Type'] = 'application/json'
    connection.request(method, path, payload, headers)
    response = connection.getresponse()
    result = response.status, response.read(), dict(response.getheaders())
    connection.close()
    return result


with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    stage = root / 'stage'
    stage.mkdir()
    env = dict(os.environ, SB_FIRMWARE_TEST_STAGE=str(stage), SB_FIRMWARE_TEST_RUN=str(root / 'run'),
               SB_CATALOG_SYNC_DISABLED='1', SB_FIRMWARE_TEST_FREE_BYTES=str(32 * 1024 * 1024),
               SB_IMAGE_TEST_IP='127.0.0.1', SB_IMAGE_TEST_PORT=str(origin.server_port))
    process = subprocess.Popen([binary, '--port', str(port), '--web-root', webroot,
                               '--config-dir', directory], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                if call('/api/auth/status')[0] == 200:
                    break
            except OSError:
                time.sleep(.05)
        internal = {'X-Remote-Capability': (root / 'remote.capability').read_text().strip()}
        pin = json.loads(call('/api/internal/web-access/start', 'POST', {}, internal)[1])['pin']
        code, _, headers = call('/api/auth/pair', 'POST', {'pin': pin})
        assert code == 200
        cookie = headers['Set-Cookie'].split(';')[0]
        identity = dict(installed='9.0.2 r17146', machine='baby', board=5,
                        enabled=False, physicalSupported=True, sequence=1)
        assert call('/api/internal/firmware/permission', 'POST', identity, internal)[0] == 200

        def prepare(checksum, accepted=True, url='http://image.test/firmware.bin'):
            assert call('/api/firmware/config', 'PUT', dict(url=url, checksum=checksum))[0] == 200
            assert call('/api/firmware/check', 'POST', {})[0] == 202
            for _ in range(100):
                state = json.loads(call('/api/firmware/status')[1])
                if state['state'] != 'preparing':
                    break
                time.sleep(.05)
            assert state['candidateReady'] == accepted, state
            assert not state['radioPermission'] and not state['ready']
            return state

        for checksum in (sha1, sha1.upper(), sha256, sha256.upper()):
            # Force a cache miss so both algorithms traverse the network.
            for path in (stage / 'firmware').glob('*.bin'):
                path.unlink()
            before = Origin.count
            state = prepare(checksum)
            assert Origin.count == before + 1
            assert state['sha256'] == sha256 and state['calculatedSha256'] == sha256
            assert (stage / 'firmware' / (sha256 + '.bin')).read_bytes() == image
            assert not (stage / 'firmware' / (sha1 + '.bin')).exists()
            saved = json.loads((root / 'firmware.json').read_text())
            assert saved['sha256'] == sha256 and saved['checksum'] == checksum.lower()
        before = Origin.count
        assert prepare(sha1)['checksumAlgorithm'] == 'SHA-1'
        assert Origin.count == before  # Switching expected algorithm reuses the image.
        for path in (stage / 'firmware').glob('*.bin'):
            path.unlink()
        prepare(sha1, url='http://image.test/redirect')
        for path in (stage / 'firmware').glob('*.bin'):
            path.unlink()
        assert prepare(sha1.upper(), url='http://image.test/chunked')['sha256'] == sha256
        # Even a matching SHA-1 cannot change an existing cache key's SHA-256.
        changed = io.BytesIO()
        with zipfile.ZipFile(io.BytesIO(image)) as old, zipfile.ZipFile(changed, 'w', zipfile.ZIP_DEFLATED) as new:
            for name in old.namelist():
                new.writestr(name, b'9.0.2 r17158' if name == 'jive.version' else old.read(name))
        changed = changed.getvalue()
        (stage / 'firmware' / (sha256 + '.bin')).write_bytes(changed)
        assert call('/api/firmware/check', 'POST', {'checksum': hashlib.sha1(changed).hexdigest()})[0] == 202
        for _ in range(100):
            state = json.loads(call('/api/firmware/status')[1])
            if state['state'] != 'preparing':
                break
            time.sleep(.05)
        assert not state['candidateReady'] and 'SHA-256 identity changed' in state['error'], state
        prepare('0' * 40, accepted=False)
        for checksum in ('a'*39, 'a'*41, 'g'*40, 'a'*63, 'a'*65):
            assert call('/api/firmware/config', 'PUT', dict(url='http://image.test/f.bin', checksum=checksum))[0] == 400
        assert call('/firmware/' + sha256 + '/custom.baby.bin')[0] == 403
    finally:
        process.terminate()
        process.wait(timeout=5)
        origin.shutdown()
print('SHA-1/SHA-256 URL streaming, cache, canonical naming, rejection and revocation passed')
