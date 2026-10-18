#!/usr/bin/env python3
"""Optional online smoke test through the real running proxy and image endpoint."""
import hashlib
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

from test_renderer import decode


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def main():
    webserver, renderer, proxy, web_root = sys.argv[1:5]
    port, proxy_port = free_port(), free_port()
    processes = []
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        config = root / 'config'
        config.mkdir()
        env = os.environ | {'SB_IMAGE_RENDERER': renderer, 'SB_IMAGE_CACHE_ROOT': str(root / 'cache'),
                            'SB_IMAGE_PROXY_URL': f'http://127.0.0.1:{proxy_port}'}
        with (root / 'log').open('wb') as log:
            try:
                processes.append(subprocess.Popen([proxy, '--listen', f'127.0.0.1:{proxy_port}'], stderr=log, stdout=log))
                processes.append(subprocess.Popen([webserver, '--port', str(port), '--web-root', web_root,
                                                   '--config-dir', str(config)], env=env, stderr=log, stdout=log))
                for service_port in [port, proxy_port]:
                    for _ in range(100):
                        try:
                            with socket.create_connection(('127.0.0.1', service_port), .1):
                                break
                        except OSError:
                            time.sleep(.05)
                name = 'BBC_Radio_Logo_2020.svg'
                digest = hashlib.md5(name.encode()).hexdigest()
                source = f'https://upload.wikimedia.org/wikipedia/commons/{digest[0]}/{digest[:2]}/{name}'
                url = f'http://127.0.0.1:{port}/api/image/render?url=' + urllib.parse.quote(source, safe='')
                with urllib.request.urlopen(url, timeout=25) as response:
                    png = response.read()
                    assert response.headers['X-SB-Image-Cache'] == 'miss'
                assert decode(png)[:2] == (320, 320)
                with urllib.request.urlopen(url, timeout=25) as response:
                    assert response.headers['X-SB-Image-Cache'] == 'hit' and response.read() == png
                print('svg-https-proxy-integration-ok')
            except Exception:
                print((root / 'log').read_text(errors='replace'))
                raise
            finally:
                for process in reversed(processes):
                    process.terminate()
                    process.wait(timeout=3)


if __name__ == '__main__':
    main()
