#!/usr/bin/env python3
"""Streaming upload fixtures; no physical device or installer is involved."""
import hashlib
import http.client
import io
import json
import os
import pathlib
import random
import socket
import subprocess
import sys
import tempfile
import time
import zipfile

binary, webroot = sys.argv[1:]
port, cookie = 19090, ''


def fixture(version='9.0.2 r17157', board=b'baby:0005\n', size=100000):
    kernel = random.Random(1).randbytes(size)
    root = random.Random(2).randbytes(size)
    checksums = (hashlib.md5(kernel).hexdigest() + '  zImage\n' +
                 hashlib.md5(root).hexdigest() + '  root.cramfs\n').encode()
    target = io.BytesIO()
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, content in [('jive.version', version.encode()), ('board.version', board),
                              ('upgrade.md5', checksums), ('zImage', kernel), ('root.cramfs', root)]:
            archive.writestr(name, content)
    return target.getvalue()


def call(path, method='GET', data=None, headers=None, auth=True):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=12)
    supplied = dict(headers or {})
    if auth and cookie:
        supplied['Cookie'] = cookie
    if isinstance(data, dict):
        data = json.dumps(data).encode()
        supplied['Content-Type'] = 'application/json'
    connection.request(method, path, data, supplied)
    response = connection.getresponse()
    result = response.status, response.read(), dict(response.getheaders())
    connection.close()
    return result


def upload(data, expected=None, headers=None, auth=True):
    supplied = {'Content-Type': 'application/octet-stream',
                'X-Firmware-SHA256': expected or hashlib.sha256(data).hexdigest()}
    supplied.update(headers or {})
    return call('/api/firmware/upload', 'POST', data, supplied, auth)


def raw(size, digest, extra='', auth=True):
    client = socket.create_connection(('127.0.0.1', port), timeout=3)
    client.settimeout(3)
    request = (f'POST /api/firmware/upload HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n'
               'Content-Type: application/octet-stream\r\n'
               f'X-Firmware-SHA256: {digest}\r\n' +
               (f'Cookie: {cookie}\r\n' if auth else '') +
               (f'Content-Length: {size}\r\n' if size is not None else '') + extra + '\r\n')
    client.sendall(request.encode())
    return client


def settle():
    for _ in range(80):
        state = json.loads(call('/api/firmware/status')[1])
        if state['state'] not in ('uploading', 'validating', 'preparing'):
            return state
        time.sleep(.05)
    raise AssertionError(state)


with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    stage = root / 'stage'
    stage.mkdir()
    environment = dict(os.environ, SB_FIRMWARE_TEST_STAGE=str(stage),
                       SB_FIRMWARE_TEST_RUN=str(root / 'run'), SB_CATALOG_SYNC_DISABLED='1',
                       SB_FIRMWARE_TEST_FREE_BYTES=str(6 * 1024 * 1024),
                       SB_FIRMWARE_TEST_UPLOAD_IDLE_MS='400')

    def launch():
        process = subprocess.Popen([binary, '--port', str(port), '--web-root', webroot,
                                    '--config-dir', directory], env=environment,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            try:
                if call('/api/auth/status')[0] == 200:
                    return process
            except OSError:
                time.sleep(.05)
        raise AssertionError('No server')

    def pair():
        global cookie
        capability = (root / 'remote.capability').read_text().strip()
        internal = {'X-Remote-Capability': capability}
        code, data, _ = call('/api/internal/web-access/start', 'POST', {}, internal)
        assert code == 200
        code, _, headers = call('/api/auth/pair', 'POST', {'pin': json.loads(data)['pin']})
        assert code == 200
        cookie = headers['Set-Cookie'].split(';')[0]
        return internal

    process = launch()
    try:
        internal = pair()
        permission = dict(installed='9.0.2 r17146', machine='baby', board=5,
                          enabled=False, physicalSupported=True, sequence=1)
        assert call('/api/internal/firmware/permission', 'POST', permission, internal)[0] == 200
        data = fixture()
        digest = hashlib.sha256(data).hexdigest()
        assert upload(data, auth=False)[0] == 401
        assert upload(data, headers={'Origin': 'http://evil.invalid'})[0] == 403
        assert upload(data, headers={'Content-Type': 'text/plain'})[0] == 400
        assert upload(data, headers={'X-Firmware-SHA256': ''})[0] == 400
        assert upload(data, headers={'X-Firmware-SHA256': 'short'})[0] == 400
        assert upload(data, expected='')[0] == 200  # helper computes explicit digest
        state = json.loads(call('/api/firmware/status')[1])
        assert state['candidateReady'] and not state['ready'] and state['source'] == 'upload', state
        path = stage / 'firmware' / (digest + '.bin')
        assert path.read_bytes() == data
        sha1 = hashlib.sha1(data).hexdigest()
        for checksum in (sha1, sha1.upper(), digest.upper()):
            code, content, _ = upload(data, headers={'X-Firmware-SHA256': checksum} if len(checksum) == 64 else
                                     {'X-Firmware-SHA256': '', 'X-Firmware-Checksum': checksum})
            # Never send both header names, including an empty legacy header.
            if len(checksum) == 40:
                assert code == 400
                code, content, _ = call('/api/firmware/upload', 'POST', data,
                    {'Content-Type': 'application/octet-stream', 'X-Firmware-Checksum': checksum})
            assert code == 200, content
            result = json.loads(content)
            assert result['sha256'] == digest and result['calculatedSha256'] == digest
            assert result['checksumAlgorithm'] == ('SHA-1' if len(checksum) == 40 else 'SHA-256')
            assert path.read_bytes() == data
            assert not (stage / 'firmware' / (sha1 + '.bin')).exists()
        for checksum in ('0'*40, 'g'*40, 'a'*39, 'a'*41, 'a'*63, 'a'*65):
            assert call('/api/firmware/upload', 'POST', data,
                {'Content-Type': 'application/octet-stream', 'X-Firmware-Checksum': checksum})[0] == 400
            assert settle()['candidateReady']
        assert call('/api/firmware/check', 'POST', {'checksum': sha1.upper()})[0] == 202
        assert settle()['checksumAlgorithm'] == 'SHA-1'
        assert settle()['sha256'] == digest
        assert call('/api/firmware/upload', 'POST', data[:-100],
            {'Content-Type': 'application/octet-stream', 'X-Firmware-Checksum': sha1})[0] == 400
        assert settle()['candidateReady']
        for size, extra, status in [(None, '', b'411'), (25 * 1024 * 1024, '', b'413'),
                                    (10 * 1024 * 1024, '', b'507'),
                                    (len(data), 'Transfer-Encoding: chunked\r\n', b'400'),
                                    (len(data), f'Content-Length: {len(data)}\r\n', b'400')]:
            client = raw(size, digest, extra)
            assert status in client.recv(2048).split(b'\r\n')[0]
            client.close()
        assert upload(data, expected='0'*64)[0] == 400
        client = raw(100, digest)
        client.sendall(data[:500])
        assert b'400' in client.recv(2048).split(b'\r\n')[0]
        client.close()
        settle()
        assert upload(b'corrupt')[0] == 400
        assert upload(data[:-100])[0] == 400
        assert upload(fixture(board=b'baby:0001\n'))[0] == 400
        assert upload(fixture(board=b'fab4:0005\n'))[0] == 400
        assert upload(fixture(version='9.0.2 r17146'))[0] == 400
        assert upload(fixture(version='9.0.2 r17145'))[0] == 400
        assert settle()['candidateReady'] and path.read_bytes() == data
        assert not (stage / 'firmware/upload.tmp').exists()
        client = raw(len(data), digest)
        client.sendall(data[:1000])
        time.sleep(.1)
        assert upload(data)[0] == 409
        assert call('/api/firmware/check', 'POST', {})[0] == 409
        assert call('/api/firmware/config', 'PUT', {'url': 'https://example.com/f.bin', 'sha256': digest})[0] == 409
        client.close()
        assert settle()['candidateReady']
        assert not (stage / 'firmware/upload.tmp').exists()
        client = raw(len(data), digest)
        client.sendall(data[:100])
        assert b'408' in client.recv(2048).split(b'\r\n')[0]
        client.close()
        assert settle()['candidateReady']
        client = raw(len(data), digest)
        for offset in range(0, len(data), 16384):
            client.sendall(data[offset:offset+16384])
            time.sleep(.02)
        response = b''
        while True:
            chunk = client.recv(8192)
            if not chunk:
                break
            response += chunk
        assert b'200' in response.split(b'\r\n')[0], response
        client.close()
        # Hold a slow local reader open across candidate replacement.
        permission.update(enabled=True, sequence=2)
        assert call('/api/internal/firmware/permission', 'POST', permission, internal)[0] == 200
        reader = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
        reader.request('GET', '/firmware/' + digest + '/custom.baby.bin')
        opened = reader.getresponse()
        assert opened.status == 200 and int(opened.getheader('Content-Length')) == len(data)
        prefix = opened.read(100)
        replacement = fixture(version='9.0.2 r17158', size=120000)
        replacement_digest = hashlib.sha256(replacement).hexdigest()
        assert call('/api/firmware/upload', 'POST', replacement,
            {'Content-Type': 'application/octet-stream',
             'X-Firmware-Checksum': hashlib.sha1(replacement).hexdigest()})[0] == 200
        assert prefix + opened.read() == data
        reader.close()
        assert not path.exists()
        assert call('/firmware/' + digest + '/custom.baby.bin')[0] == 403
        assert call('/firmware/' + replacement_digest + '/custom.baby.bin')[1] == replacement
        interrupted = raw(len(data), digest)
        interrupted.sendall(data[:10000])
        time.sleep(.1)
        process.terminate()
        process.wait(timeout=5)
        interrupted.close()
        process = launch()
        internal = pair()
        state = json.loads(call('/api/firmware/status')[1])
        assert state['source'] == 'upload' and not state['candidateReady'] and not state['ready']
        assert not (stage / 'firmware/upload.tmp').exists()
        permission.update(enabled=False, sequence=1)
        assert call('/api/internal/firmware/permission', 'POST', permission, internal)[0] == 200
        assert call('/api/firmware/check', 'POST', {})[0] == 202
        state = settle()
        assert state['candidateReady'] and not state['ready'], state
        assert call('/webserver-settings', 'PUT', {'readonly': True})[0] == 200
        assert upload(data)[0] == 403
        print('Firmware binary streaming, cancellation, validation, replacement and restart tests passed')
    finally:
        process.terminate()
        process.wait(timeout=5)
