#!/usr/bin/env python3
"""Ensure the opt-in proxy policy rejects private sockets and advertises support."""
import http.client
import os
import socket
import subprocess
import sys
import time


def main():
    holder = socket.socket()
    holder.bind(('127.0.0.1', 0))
    port = holder.getsockname()[1]
    holder.close()
    process = subprocess.Popen(sys.argv[1:] + ['--listen', f'127.0.0.1:{port}'],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                with socket.create_connection(('127.0.0.1', port), .1):
                    break
            except OSError:
                time.sleep(.05)
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
        connection.request('GET', '/health')
        response = connection.getresponse()
        assert response.status == 200 and response.getheader('X-SB-Public-Fetch') == '1'
        response.read()
        connection.close()
        for route, host in [(route, host) for route in ('/https/', '/http/', '/artwork/320/320/80/https/')
                            for host in ('127.0.0.1', 'localhost', '10.0.0.1', '169.254.169.254', '[::1]')]:
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
            connection.request('GET', f'{route}{host}/private', headers={'X-SB-Public-Fetch': '1'})
            response = connection.getresponse()
            assert response.status == 502, (host, response.status)
            response.read()
            connection.close()
        for host in ('127.0.0.1', 'localhost', '10.0.0.1', '169.254.169.254'):
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
            connection.request('HEAD', f'/hls/http/{host}/playlist.m3u8')
            response = connection.getresponse()
            assert response.status >= 400, (host, response.status)
            response.read()
            connection.close()
    finally:
        process.terminate()
        process.wait(timeout=3)
    print('image-proxy-guard-tests-ok')


if __name__ == '__main__':
    main()
