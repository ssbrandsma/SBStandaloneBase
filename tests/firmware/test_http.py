#!/usr/bin/env python3
"""Host fixtures only: prepare and serve a synthetic image, never install it."""
import hashlib
import http.client
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import zipfile

binary, webroot = sys.argv[1:]
port, cookie = 19089, ''


def call(path, method='GET', payload=None, headers=None, authenticated=True):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=4)
    supplied = dict(headers or {})
    if authenticated and cookie:
        supplied['Cookie'] = cookie
    if payload is not None:
        supplied['Content-Type'] = 'application/json'
    connection.request(method, path, None if payload is None else json.dumps(payload), supplied)
    response = connection.getresponse()
    result = response.status, response.read(), dict(response.getheaders())
    connection.close()
    return result


with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    stage = root / 'stage'
    stage.mkdir()
    run = root / 'run'
    environment = dict(os.environ, SB_FIRMWARE_TEST_STAGE=str(stage),
                       SB_FIRMWARE_TEST_RUN=str(run), SB_CATALOG_SYNC_DISABLED='1',
                       SB_FIRMWARE_TEST_FREE_BYTES=str(5 * 1024 * 1024))
    process = subprocess.Popen([binary, '--port', str(port), '--web-root', webroot,
                                '--config-dir', directory], env=environment,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(80):
            try:
                if call('/api/auth/status')[0] == 200:
                    break
            except OSError:
                time.sleep(.05)
        assert call('/api/firmware/status', authenticated=False)[0] == 401
        capability = (root / 'remote.capability').read_text().strip()
        internal = {'X-Remote-Capability': capability}
        status, body, _ = call('/api/internal/web-access/start', 'POST', {}, internal)
        assert status == 200
        pin = json.loads(body)['pin']
        status, body, headers = call('/api/auth/pair', 'POST', {'pin': pin})
        assert status == 200
        cookie = headers['Set-Cookie'].split(';')[0]
        assert not json.loads(call('/api/firmware/status')[1])['radioPermission']
        permission = dict(installed='9.0.2 r17146', machine='baby', board=5,
                          enabled=True, physicalSupported=True, sequence=1)
        endpoint = '/api/internal/firmware/permission'
        assert call(endpoint, 'POST', permission)[0] == 403
        assert call(endpoint, 'POST', permission, internal)[0] == 200
        assert call(endpoint, 'POST', permission, internal)[0] == 409
        assert call(endpoint, 'POST', permission, dict(internal, Origin='http://evil.invalid'))[0] == 403
        assert call('/api/firmware/config', 'PUT', {'url': 'ftp://example.com/f.bin', 'sha256': 'a'*64})[0] == 400
        assert call('/api/firmware/config', 'PUT', {'url': 'https://example.com/f.bin'})[0] == 400
        assert call('/api/firmware/config', 'PUT', {'padding': 'x'*5000})[0] == 413
        image = io.BytesIO()
        kernel, filesystem = b'kernel' * 100, b'root' * 100
        md5 = (hashlib.md5(kernel).hexdigest() + '  zImage\n' +
               hashlib.md5(filesystem).hexdigest() + '  root.cramfs\n').encode()
        with zipfile.ZipFile(image, 'w', zipfile.ZIP_DEFLATED) as archive:
            for name, content in [('jive.version', b'9.0.2 r17157\nfixture'),
                                  ('board.version', b'baby:0005\n'),
                                  ('upgrade.md5', md5), ('zImage', kernel),
                                  ('root.cramfs', filesystem)]:
                archive.writestr(name, content)
        image = image.getvalue()
        digest = hashlib.sha256(image).hexdigest()
        config = {'url': 'https://example.com/firmware.bin', 'sha256': digest}
        assert call('/api/firmware/config', 'PUT', config)[0] == 200
        # 5 MiB cannot reserve an unknown-size download, but can validate a
        # pre-staged image in place without allocating a second image copy.
        assert call('/api/firmware/check', 'POST', {})[0] == 507
        candidate = stage / 'firmware'
        candidate.mkdir(exist_ok=True)
        candidate.chmod(0o700)
        (candidate / (digest + '.bin')).write_bytes(image)
        assert call('/api/firmware/check', 'POST', {})[0] == 202
        for _ in range(100):
            status = json.loads(call('/api/firmware/status')[1])
            if status['state'] != 'preparing':
                break
            time.sleep(.05)
        assert status['ready'], status
        path = '/firmware/' + digest + '/custom.baby.bin'
        code, content, headers = call(path, authenticated=False)
        assert code == 200 and content == image
        assert int(headers['Content-Length']) == len(image)
        assert call(path, headers={'Origin': 'http://evil.invalid'})[0] == 403
        assert call(path, headers={'Host': 'evil.invalid'})[0] == 403
        assert call(path, headers={'Range': 'bytes=0-1'})[0] == 403
        assert call('/firmware/' + 'b'*64 + '/custom.baby.bin')[0] == 403
        time.sleep(46)
        assert call(path)[0] == 403
        assert not json.loads(call('/api/firmware/status')[1])['radioPermission']
        permission.update(sequence=2, enabled=False)
        assert call(endpoint, 'POST', permission, internal)[0] == 200
        assert call(path)[0] == 403
        assert not json.loads(call('/api/firmware/status')[1])['ready']
        assert call('/webserver-settings', 'PUT', {'readonly': True})[0] == 200
        assert call('/api/firmware/config', 'PUT', config)[0] == 403
        assert json.loads((root / 'firmware.json').read_text())['sha256'] == digest
        process.terminate()
        process.wait(timeout=5)
        # An installation predating checksum selection has only the SHA-256
        # URL pin. Loading it must retain the candidate/config without trust.
        (root / 'firmware.json').write_text(json.dumps(config))
        process = subprocess.Popen([binary, '--port', str(port), '--web-root', webroot,
                                    '--config-dir', directory], env=environment,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(80):
            try:
                if call('/api/auth/status')[0] == 200:
                    break
            except OSError:
                time.sleep(.05)
        # Neither a previous session nor the staged file restores authority.
        assert call('/api/firmware/status')[0] == 401
        assert call(path, authenticated=False)[0] == 403
        capability = (root / 'remote.capability').read_text().strip()
        internal = {'X-Remote-Capability': capability}
        code, raw, _ = call('/api/internal/web-access/start', 'POST', {}, internal)
        assert code == 200
        code, raw, headers = call('/api/auth/pair', 'POST', {'pin': json.loads(raw)['pin']})
        assert code == 200
        cookie = headers['Set-Cookie'].split(';')[0]
        restarted = json.loads(call('/api/firmware/status')[1])
        assert restarted['url'] == config['url'] and restarted['sha256'] == digest
        assert restarted['state'] == 'unchecked' and not restarted['ready']
        assert not restarted['radioPermission'] and not restarted['available']
        print('Firmware HTTP authentication, configuration, validation and delivery tests passed')
    finally:
        process.terminate()
        process.wait(timeout=5)
