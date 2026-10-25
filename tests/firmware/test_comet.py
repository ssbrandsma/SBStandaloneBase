#!/usr/bin/env python3
"""Exercise read-only firmware advertisements; never invoke an installer."""
import http.client
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time


def request(command):
    connection = http.client.HTTPConnection('127.0.0.1', 9000, timeout=2)
    payload = [{'channel': '/slim/request', 'clientId': 'firmware-test',
                'id': 1, 'data': {'response': '/firmware-test/slim/firmwarestatus',
                                 'request': ['', command]}}]
    connection.request('POST', '/cometd', json.dumps(payload),
                       {'Content-Type': 'application/json'})
    response = connection.getresponse()
    assert response.status == 200
    messages = json.loads(response.read())
    connection.close()
    return next(message['data'] for message in messages if 'data' in message)


with tempfile.TemporaryDirectory() as directory:
    descriptor = pathlib.Path(directory) / 'firmware.state'
    environment = dict(os.environ, SB_FIRMWARE_STATE=str(descriptor))
    process = subprocess.Popen([sys.argv[1]], env=environment,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        command = ['firmwareupgrade', 'firmwareVersion:9.0.2 r17146',
                   'machine:baby', 'inSetup:0', 'subscribe:0']
        for attempt in range(60):
            try:
                result = request(command)
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise AssertionError('sbbase did not start')
        assert result == {'firmwareUpgrade': 0}, result
        url = 'http://127.0.0.1:9080/firmware/' + 'a' * 64 + '/custom.baby.bin'

        def publish(expiry):
            descriptor.write_text(f'{expiry}\n{url}\n9.0.2 r17146\n9.0.2 r17157\n')
            descriptor.chmod(0o600)

        clock = getattr(time, 'CLOCK_MONOTONIC_RAW', time.CLOCK_MONOTONIC)
        publish(int(time.clock_gettime(clock) * 1000) + 30000)
        assert request(command) == {'firmwareUrl': url, 'firmwareUpgrade': 0}
        wrong_machine = [item.replace('machine:baby', 'machine:fab4') for item in command]
        assert request(wrong_machine) == {'firmwareUpgrade': 0}
        wrong_version = [item.replace('r17146', 'r17145') for item in command]
        assert request(wrong_version) == {'firmwareUpgrade': 0}
        descriptor.chmod(0o666)
        assert request(command) == {'firmwareUpgrade': 0}
        publish(1)
        assert request(command) == {'firmwareUpgrade': 0}
        descriptor.unlink()
        assert request(command) == {'firmwareUpgrade': 0}

        def connect_stream():
            payload = json.dumps([{'channel': '/meta/connect', 'clientId': 'firmware-test'}]).encode()
            stream = socket.create_connection(('127.0.0.1', 9000), timeout=2)
            stream.sendall(b'POST /cometd HTTP/1.1\r\nHost: localhost\r\n'
                           b'Content-Type: application/json\r\nContent-Length: ' +
                           str(len(payload)).encode() + b'\r\n\r\n' + payload)
            received = b''
            while b'firmwarestatus' not in received:
                received += stream.recv(8192)
            return stream, received

        def receive_until(stream, marker):
            received = b''
            stream.settimeout(2)
            while marker not in received:
                received += stream.recv(8192)
            return received

        request(command)
        stream, initial = connect_stream()
        assert b'firmwareUpgrade' in initial
        publish(int(time.clock_gettime(clock) * 1000) + 30000)
        receive_until(stream, url.encode())
        stream.settimeout(.4)
        try:
            unexpected = stream.recv(8192)
            assert not unexpected, unexpected
        except socket.timeout:
            pass
        stream.close()
        stream, replay = connect_stream()
        assert url.encode() in replay, replay
        descriptor.unlink()
        revoked = receive_until(stream, b'firmwareUpgrade')
        assert b'firmwareUrl' not in revoked, revoked
        connection = http.client.HTTPConnection('127.0.0.1', 9000, timeout=2)
        connection.request('POST', '/cometd', json.dumps([{
            'channel': '/meta/unsubscribe', 'clientId': 'firmware-test',
            'unsubscribe': '/firmware-test/slim/firmwarestatus'}]),
            {'Content-Type': 'application/json'})
        assert connection.getresponse().read()
        connection.close()
        publish(int(time.clock_gettime(clock) * 1000) + 30000)
        stream.settimeout(.4)
        try:
            unexpected = stream.recv(8192)
            assert not unexpected, unexpected
        except socket.timeout:
            pass
        stream.close()
        print('Firmware Comet identity, expiry and permission tests passed')
    finally:
        process.terminate()
        process.wait(timeout=5)
