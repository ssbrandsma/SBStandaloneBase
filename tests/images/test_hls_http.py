#!/usr/bin/env python3
"""Deterministic HLS relay tests, no audio decoder or external network required.
Uses a dedicated test proxy permitting only the two explicit fixture ports.
Synthetic AAC/MP3 frames test byte preservation, not acoustic correctness.
"""
import http.client
import base64
import concurrent.futures
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time


def aac(value=0, rate=4):
    length = 39
    return bytes([255, 241, 64 | (rate << 2), 128 | (length >> 11),
                  (length >> 3) & 255, ((length & 7) << 5) | 31, 252]) + bytes([value]) * (length - 7)


AAC = aac(1) * 20
MP3 = bytes.fromhex('fffb9000') + bytes(413)
ID3 = b'ID3\x04\0\0\0\0\0\x04test'
PNG = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a3ioAAAAASUVORK5CYII=')
def sync(n):
    return bytes([(n >> 21) & 127, (n >> 14) & 127, (n >> 7) & 127, n & 127])
def metadata(title, artwork=False):
    frames = []
    for key, value in ((b'TIT2', b'\x03' + title.encode()), (b'TPE1', b'\x03Artist'), (b'TALB', b'\x03Album')):
        frames.append(key + sync(len(value)) + b'\0\0' + value)
    if artwork:
        value = b'\0image/png\0\x03\0' + PNG
        frames.append(b'APIC' + sync(len(value)) + b'\0\0' + value)
    body = b''.join(frames)
    return b'ID3\x04\0\0' + sync(len(body)) + body


def crc(section):
    value = 0xffffffff
    for byte in section:
        value ^= byte << 24
        for _ in range(8):
            value = ((value << 1) ^ (0x04c11db7 if value & 0x80000000 else 0)) & 0xffffffff
    return section + value.to_bytes(4, 'big')


def packet(pid, body, start, cc):
    header = bytes([71, (pid >> 8) | (64 if start else 0), pid & 255])
    if len(body) == 184:
        return header + bytes([16 | cc]) + body
    padding = 183 - len(body)
    return header + bytes([48 | cc, padding]) + (bytes(padding) if padding else b'') + body


def ts(audio):
    pat = crc(bytes.fromhex('00b00d0001c100000001e064'))
    pmt = crc(bytes.fromhex('02b0120001c10000e100f0000fe100f000'))
    result = packet(0, b'\0' + pat, True, 0) + packet(100, b'\0' + pmt, True, 0)
    length = len(audio) + 3
    pes = b'\0\0\1\xc0' + length.to_bytes(2, 'big') + b'\x80\0\0' + audio
    # Force a PES header split across packets, then regular packet-sized chunks.
    result += packet(256, pes[:5], True, 0)
    pes = pes[5:]
    cc = 1
    while pes:
        result += packet(256, pes[:184], False, cc)
        pes = pes[184:]
        cc = (cc + 1) & 15
    return result


def timed_ts():
    pat = crc(bytes.fromhex('00b00d0001c100000001e064'))
    pmt = crc(bytes.fromhex('02b0170001c10000e100f0000fe100f00015e101f000'))
    result = packet(0, b'\0'+pat, True, 0) + packet(100, b'\0'+pmt, True, 0)
    def pes(pid, stream, data, pts, cc=0):
        stamp=bytes([0x21|((pts>>29)&14),(pts>>22)&255,1|((pts>>14)&254),(pts>>7)&255,1|((pts<<1)&254)])
        body=b'\0\0\1'+bytes([stream])+(len(data)+8).to_bytes(2,'big')+b'\x80\x80\x05'+stamp+data
        output=b'';first=True
        while body:
            output+=packet(pid,body[:184],first,cc);body=body[184:];first=False;cc=(cc+1)&15
        return output,cc
    first,cc=pes(256,0xc0,AAC,0)
    tags,_=pes(257,0xbd,metadata('TS track',True),90000)
    second,_=pes(256,0xc0,AAC,41796,cc)
    return result+first+tags+second


def media(names, sequence=0, live=False):
    text = '#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:%d\n' % sequence
    text += ''.join('#EXTINF:1.0,\n%s\n' % name for name in names)
    return (text + ('' if live else '#EXT-X-ENDLIST\n')).encode()


class Origin(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    counts = {}
    ranges = []

    def log_message(self, *args):
        pass

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass

    def do_GET(self):
        path = self.path.split('?')[0]
        Origin.counts[path] = Origin.counts.get(path, 0) + 1
        n = Origin.counts[path]
        code, body = 200, AAC
        if path == '/ending.m3u8':
            body=media(['first.aac'],0,n==1)
        elif path == '/metadata-wait.m3u8':
            sequence=(n-1)//3
            body=media(['wait%d.aac' % sequence],sequence,True)
        elif path.startswith('/wait') and path.endswith('.aac'):
            body=metadata('Wake '+path[5:-4])+AAC
        elif path == '/metadata-vod.m3u8':
            body = media(['metadata.aac'])
        elif path == '/metadata-overflow.m3u8':
            body = media(['metadata-overflow.aac'], live=True)
        elif path == '/metadata-overflow.aac':
            body = b''.join(metadata('Track '+str(i)) + aac(i) for i in range(12))
        elif path == '/metadata-ts.m3u8':
            body = media(['metadata.ts'], live=True)
        elif path == '/metadata.ts':
            body = timed_ts()
        elif path == '/metadata.m3u8':
            body = media(['metadata.aac'], live=True)
        elif path == '/metadata.aac':
            body = metadata('First', True) + AAC + metadata('Second') + AAC
        elif path == '/redirect':
            self.send_response(302)
            self.send_header('Location', f'https://127.0.0.1:{https_origin.server_port}/folder/master.m3u8')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if path == '/folder/master.m3u8':
            body = b'#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=128000,CODECS="mp4a.40.2"\n../high.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=64000,CODECS="mp4a.40.2"\n../low.m3u8\n'
        elif path == '/renditions.m3u8':
            body = b'#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="a",NAME="Radio",DEFAULT=YES,URI="low.m3u8"\n#EXT-X-STREAM-INF:BANDWIDTH=100000,CODECS="avc1.42e01e,mp4a.40.2",AUDIO="a"\nvideo.m3u8\n'
        elif path == '/low.m3u8':
            body = media(['first.aac', 'second.aac'])
        elif path == '/first.aac':
            body = ID3 + AAC
        elif path == '/second.aac':
            body = aac(2) * 20
        elif path == '/mp3.m3u8':
            body = media(['one.mp3', 'one.mp3'])
        elif path == '/one.mp3':
            body = ID3 + MP3 * 4
        elif path == '/ts.m3u8':
            body = media(['one.ts', 'one.ts'])
        elif path == '/one.ts':
            body = ts(AAC)
        elif path == '/bad-ts.m3u8':
            body = media(['bad.ts'])
        elif path == '/bad.ts':
            damaged = bytearray(ts(AAC))
            damaged[4] = 255
            body = bytes(damaged)
        elif path == '/bad-crc.m3u8':
            body = media(['crc.ts'])
        elif path == '/crc.ts':
            damaged = bytearray(ts(AAC))
            damaged[187] ^= 1
            body = bytes(damaged)
        elif path == '/split.m3u8':
            body = media(['split-one', 'split-two'])
        elif path == '/split-one':
            body = AAC[:101]
        elif path == '/split-two':
            body = AAC[101:]
        elif path in ('/resume.m3u8', '/ignored-range.m3u8'):
            body = media(['interrupted' if path == '/resume.m3u8' else 'ignored-range'])
        elif path in ('/interrupted', '/ignored-range'):
            full = AAC * 5
            range_header = self.headers.get('Range')
            if range_header:
                Origin.ranges.append(range_header)
                begin = int(range_header[6:-1])
                if path == '/ignored-range':
                    body = full
                else:
                    self.send_response(206)
                    self.send_header('Content-Range', f'bytes {begin}-{len(full)-1}/{len(full)}')
                    self.send_header('Content-Length', str(len(full) - begin))
                    self.end_headers()
                    self.wfile.write(full[begin:])
                    return
            else:
                self.send_response(200)
                self.send_header('Content-Length', str(len(full)))
                self.end_headers()
                self.wfile.write(full[:101])
                self.wfile.flush()
                self.close_connection = True
                return
        elif path == '/live.m3u8':
            body = media([f'seq{i}.aac' for i in range(n, n + 3)], n, n < 3)
        elif path.startswith('/seq') and path.endswith('.aac'):
            body = aac(int(path[4:-4]))
        elif path == '/expired.m3u8':
            body = media(['gone', 'second.aac'], 10, n == 1)
        elif path == '/gone':
            code, body = 404, b'gone'
        elif path == '/sequence-reset.m3u8':
            body = media(['second.aac'], 100 if n == 1 else 0, n == 1)
        elif path == '/never.m3u8':
            body = media([f'seq{i}.aac' for i in range(n, n+3)], n, True)
        elif path == '/stalled.m3u8':
            body = media(['first.aac'], 0, True)
        elif path == '/encrypted.m3u8':
            body = b'#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI="key"\n'
        elif path == '/fmp4.m3u8':
            body = b'#EXTM3U\n#EXT-X-MAP:URI="init.mp4"\n'
        elif path == '/ordinary.m3u8':
            body = b'#EXTM3U\nhttp://example.com/live.mp3\n'
        elif path == '/large.m3u8':
            body = b'#' * 65537
        elif path == '/unknown.m3u8':
            body = media(['unknown'])
        elif path == '/unknown':
            body = b'\0\0\0\x18ftyp' + bytes(20)
        elif path == '/config.m3u8':
            body = media(['first.aac', 'config-change'])
        elif path == '/config-change':
            body = aac(3, 3)
        elif path == '/nested.m3u8':
            body = b'#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nnested.m3u8\n'
        elif path == '/slow.m3u8':
            time.sleep(2)
            body = media(['first.aac'])
        elif path == '/timeout.m3u8':
            time.sleep(9)
            body = media(['first.aac'])
        elif path in ('/missing.m3u8', '/forbidden.m3u8'):
            code, body = (404 if path == '/missing.m3u8' else 403), b'no'
        elif path == '/transient.m3u8':
            code, body = (503, b'retry') if n == 1 else (200, media(['first.aac']))
        elif path == '/cross-segment.m3u8':
            body = media([f'https://127.0.0.1:{https_origin.server_port}/one.ts'])
        self.send_response(code)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        try:
            # Irregular small chunks exercise packet/header/frame boundaries.
            for pos in range(0, len(body), 113):
                self.wfile.write(body[pos:pos+113])
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass


with tempfile.TemporaryDirectory() as temporary:
    cert, key = Path(temporary) / 'cert.pem', Path(temporary) / 'key.pem'
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(key),
                    '-out', str(cert), '-days', '1', '-subj', '/CN=localhost'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    http_origin = ThreadingHTTPServer(('127.0.0.1', 0), Origin)
    https_origin = ThreadingHTTPServer(('127.0.0.1', 0), Origin)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    https_origin.socket = context.wrap_socket(https_origin.socket, server_side=True)
    for server in (http_origin, https_origin):
        threading.Thread(target=server.serve_forever, daemon=True).start()
    with socket.socket() as holder:
        holder.bind(('127.0.0.1', 0))
        port = holder.getsockname()[1]
    env = dict(os.environ, SB_PROXY_TEST_PORTS=f'{http_origin.server_port},{https_origin.server_port}')
    log = open(Path(temporary) / 'proxy.log', 'w+')
    process = subprocess.Popen(sys.argv[1:] + ['--listen', f'127.0.0.1:{port}'], env=env,
                               stdout=subprocess.DEVNULL, stderr=log)
    try:
        for _ in range(100):
            try:
                with socket.create_connection(('127.0.0.1', port), .1):
                    break
            except OSError:
                time.sleep(.05)

        def get(path, method='GET', headers=None):
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=30)
            connection.request(method, f'/hls/http/127.0.0.1:{http_origin.server_port}{path}', headers=headers or {})
            response = connection.getresponse()
            result = response.status, response.getheader('Content-Type'), response.read()
            connection.close()
            return result

        def local_get(path):
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
            connection.request('GET', path)
            result = connection.getresponse()
            status, body = result.status, result.read()
            connection.close()
            return status, body

        # HEAD never creates a metadata session. GET does, before the first tag.
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
        source = f'/hls/http/127.0.0.1:{http_origin.server_port}/metadata.m3u8'
        connection.request('HEAD', source)
        response = connection.getresponse()
        assert response.status == 200 and response.getheader('X-SB-HLS-Session') is None
        response.read(); connection.close()
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
        connection.request('GET', source)
        response = connection.getresponse()
        identity = response.getheader('X-SB-HLS-Session')
        assert identity and len(identity) == 32
        assert response.read(len(AAC)*2) == AAC*2, 'ID3 must never reach the decoder'
        status, body = local_get(f'/hls-meta/{identity}?after=0')
        first = json.loads(body)
        assert status == 200 and first['title'] == 'First' and first['position_ms'] == 0
        assert first['artist'] == 'Artist' and first['album'] == 'Album'
        artwork = first['artwork_url'].split(':8765', 1)[1]
        assert local_get(artwork) == (200, PNG)
        second = json.loads(local_get(f'/hls-meta/{identity}?after={first["revision"]}')[1])
        assert second['title'] == 'Second' and 460 <= second['position_ms'] <= 465
        assert 'artwork_url' not in second
        assert json.loads(local_get(f'/hls-meta/{identity}?after={second["revision"]}')[1])['pending']
        assert local_get('/hls-meta/' + 'f'*32)[0] == 404
        assert local_get(f'/hls-meta/{identity}?after=oops')[0] == 400
        response.close(); connection.close()
        for _ in range(20):
            if local_get(artwork)[0] == 404: break
            time.sleep(.1)
        else: raise AssertionError('cancelled stream retained artwork identity')

        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
        connection.request('GET', f'/hls/http/127.0.0.1:{http_origin.server_port}/metadata-ts.m3u8')
        response = connection.getresponse()
        identity = response.getheader('X-SB-HLS-Session')
        assert response.status == 200 and identity
        assert response.read(len(AAC)*2) == AAC*2
        event = json.loads(local_get(f'/hls-meta/{identity}?after=0')[1])
        assert event['title'] == 'TS track' and event['timestamp_90khz'] == 90000
        assert event['position_ms'] == 1000, event
        response.close(); connection.close(); time.sleep(.6)

        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
        connection.request('GET', f'/hls/http/127.0.0.1:{http_origin.server_port}/metadata-overflow.m3u8')
        response = connection.getresponse(); identity = response.getheader('X-SB-HLS-Session')
        assert len(response.read(39*12)) == 39*12
        event = json.loads(local_get(f'/hls-meta/{identity}?after=0')[1])
        assert event['revision'] == 5 and event['dropped'] == 4 and event['title'] == 'Track 4'
        response.close(); connection.close(); time.sleep(.6)

        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
        connection.request('GET', f'/hls/http/127.0.0.1:{http_origin.server_port}/metadata-vod.m3u8')
        response = connection.getresponse(); identity = response.getheader('X-SB-HLS-Session')
        assert response.read() == AAC*2
        response.close(); connection.close()
        event = json.loads(local_get(f'/hls-meta/{identity}?after=0')[1])
        assert event['title'] == 'First', 'natural EOF must retain buffered metadata'
        event = json.loads(local_get(f'/hls-meta/{identity}?after=2')[1])
        assert event['pending'] and event['finished']

        # Long polls wake on actual metadata, not periodic decoder checks.
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
        connection.request('GET', f'/hls/http/127.0.0.1:{http_origin.server_port}/metadata-wait.m3u8')
        response=connection.getresponse(); identity=response.getheader('X-SB-HLS-Session')
        response.read(len(AAC))
        started=time.monotonic()
        event=json.loads(local_get(f'/hls-meta/{identity}?after=0&wait=25')[1])
        assert event['revision']==1 and time.monotonic()-started<1
        started=time.monotonic()
        event=json.loads(local_get(f'/hls-meta/{identity}?after=1&wait=25')[1])
        assert event['revision']==2 and .3<time.monotonic()-started<3
        response.close();connection.close();time.sleep(.6)

        connection=http.client.HTTPConnection('127.0.0.1',port,timeout=5)
        connection.request('GET',f'/hls/http/127.0.0.1:{http_origin.server_port}/ending.m3u8')
        response=connection.getresponse();identity=response.getheader('X-SB-HLS-Session')
        response.read(39)
        started=time.monotonic()
        status,body=local_get(f'/hls-meta/{identity}?after=0&wait=25')
        assert status==200 and json.loads(body)['finished']
        assert time.monotonic()-started<2
        response.close();connection.close()

        # One wait per stream; two waits total leave four workers for other
        # traffic even while both HLS audio slots are active.
        audio=[]
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            try:
                for _ in range(2):
                    c=http.client.HTTPConnection('127.0.0.1',port,timeout=5)
                    c.request('GET',f'/hls/http/127.0.0.1:{http_origin.server_port}/never.m3u8')
                    res=c.getresponse();res.read(39)
                    audio.append((c,res,res.getheader('X-SB-HLS-Session')))
                identity=audio[0][2]
                started=time.monotonic()
                assert json.loads(local_get(f'/hls-meta/{identity}?after=0&wait=1')[1])['pending']
                assert .8<time.monotonic()-started<2
                # Abandoned wait connections must release their cap slots
                # without needing a metadata event or an audio disconnect.
                for _ in range(3):
                    abandoned=socket.create_connection(('127.0.0.1',port),timeout=5)
                    abandoned.sendall((f'GET /hls-meta/{identity}?after=0&wait=25 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n').encode())
                    time.sleep(.08)
                    assert local_get(f'/hls-meta/{identity}?after=0&wait=1')[0]==429
                    abandoned.close()
                    time.sleep(.08)
                    started=time.monotonic()
                    status,body=local_get(f'/hls-meta/{identity}?after=0&wait=1')
                    assert status==200 and json.loads(body)['pending']
                    assert time.monotonic()-started<2
                wait1=executor.submit(local_get,f'/hls-meta/{identity}?after=0&wait=25')
                wait2=executor.submit(local_get,f'/hls-meta/{audio[1][2]}?after=0&wait=25')
                time.sleep(.2)
                assert local_get(f'/hls-meta/{identity}?after=0&wait=25')[0]==429
                assert local_get('/health')[0]==200
                assert local_get(f'/hls-meta/{identity}?after=0')[0]==200
                blocked=[]
                try:
                    # Existing pool exhaustion still rejects excess work;
                    # releasing unrelated readers restores service while the
                    # two bounded metadata waits remain alive.
                    for _ in range(4):
                        peer=socket.create_connection(('127.0.0.1',port),timeout=5)
                        peer.sendall(b'GET /health HTTP/1.1\r\nHost: localhost\r\n')
                        blocked.append(peer)
                    time.sleep(.2)
                    assert local_get('/health')[0]==503
                finally:
                    for peer in blocked:peer.close()
                time.sleep(.2)
                assert local_get('/health')[0]==200
                # Disconnecting the audio stream must wake its blocked reader.
                started=time.monotonic()
                audio[0][1].close();audio[0][0].close()
                assert wait1.result(timeout=2)[0]==404
                assert time.monotonic()-started<2
                audio[1][1].close();audio[1][0].close()
                assert wait2.result(timeout=2)[0]==404
            finally:
                for c,res,_ in audio:res.close();c.close()
        time.sleep(.6)
        assert local_get('/hls-meta/'+'f'*32+'?after=0&wait=25')[0]==404
        assert local_get('/hls-meta/'+'f'*32+'?after=0&wait=31')[0]==400

        for path, content_type, wanted in (
            ('/low.m3u8', 'audio/aac', AAC + aac(2) * 20),
            ('/redirect', 'audio/aac', AAC + aac(2) * 20),
            ('/renditions.m3u8', 'audio/aac', AAC + aac(2) * 20),
            ('/ts.m3u8', 'audio/aac', AAC * 2),
            ('/cross-segment.m3u8', 'audio/aac', AAC),
            ('/mp3.m3u8', 'audio/mpeg', MP3 * 8),
            ('/split.m3u8', 'audio/aac', AAC),
            ('/resume.m3u8', 'audio/aac', AAC * 5),
            ('/transient.m3u8', 'audio/aac', AAC),
            ('/slow.m3u8', 'audio/aac', AAC),
            ('/live.m3u8', 'audio/aac', b''.join(aac(i) for i in range(1, 6))),
            ('/expired.m3u8', 'audio/aac', aac(2) * 20),
        ):
            status, mime, body = get(path)
            if (status, mime, body) != (200, content_type, wanted):
                log.flush(); log.seek(0)
                raise AssertionError((path, status, mime, len(body), len(wanted), log.read()))
        assert 'bytes=101-' in Origin.ranges
        assert get('/low.m3u8', 'HEAD') == (200, 'audio/aac', b'')
        assert get('/mp3.m3u8', 'HEAD') == (200, 'audio/mpeg', b'')
        assert get('/low.m3u8', headers={'X-SB-HLS-Codec': 'aac'})[0] == 200
        assert get('/low.m3u8', headers={'X-SB-HLS-Codec': 'mp3'})[0] == 415
        assert get('/mp3.m3u8', headers={'x-sb-hls-codec': 'aac'})[0] == 415
        assert get('/low.m3u8', headers={'X-SB-HLS-Codec': 'invalid'})[0] == 400
        for path in ('/encrypted.m3u8', '/fmp4.m3u8', '/ordinary.m3u8', '/large.m3u8',
                     '/unknown.m3u8', '/nested.m3u8', '/missing.m3u8', '/forbidden.m3u8',
                     '/bad-ts.m3u8', '/bad-crc.m3u8'):
            assert get(path)[0] >= 400, path
        assert get('/config.m3u8')[2] == AAC
        assert get('/sequence-reset.m3u8')[2] == aac(2) * 20
        assert get('/stalled.m3u8')[2] == AAC
        # Interrupted retry must not duplicate emitted frames if Range is ignored.
        assert get('/ignored-range.m3u8')[2] == AAC[:78]
        # Repeated probes and cancellations must release the bounded slots.
        for _ in range(5):
            assert get('/low.m3u8', 'HEAD')[0] == 200
        c = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
        c.request('GET', f'/hls/http/127.0.0.1:{http_origin.server_port}/never.m3u8')
        response = c.getresponse()
        response.read(39)
        response.close()
        c.close()
        time.sleep(.3)
        assert get('/low.m3u8', 'HEAD')[0] == 200
        if os.environ.get('SB_HLS_TEST_TIMEOUT'):
            started = time.monotonic()
            assert get('/timeout.m3u8', 'HEAD')[0] == 504
            assert 12 < time.monotonic() - started < 25
        streams = []
        try:
            for _ in range(2):
                c = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
                c.request('GET', f'/hls/http/127.0.0.1:{http_origin.server_port}/never.m3u8')
                response = c.getresponse()
                assert response.status == 200 and len(response.read(39)) == 39
                identity = response.getheader('X-SB-HLS-Session')
                assert identity and local_get(f'/hls-meta/{identity}')[0] == 200
                streams.append((c, response))
            assert streams[0][1].getheader('X-SB-HLS-Session') != streams[1][1].getheader('X-SB-HLS-Session')
            assert get('/low.m3u8', 'HEAD')[0] == 503
        finally:
            for c, response in streams:
                response.close()
                c.close()
        time.sleep(.3)
        assert get('/low.m3u8', 'HEAD')[0] == 200
    finally:
        process.terminate()
        process.wait(timeout=5)
        for server in (http_origin, https_origin):
            server.shutdown()
        log.close()
print('hls-http-tests-ok')
