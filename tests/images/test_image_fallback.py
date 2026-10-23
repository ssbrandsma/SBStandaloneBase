#!/usr/bin/env python3
"""Deterministic local-first fallback tests using real worker/renderer and mock origins/TLS proxy."""
import concurrent.futures
import http.server
import io
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse as U
import urllib.request as R
from test_raster import png
from test_renderer import decode

FIX=Path(__file__).parent/'fixtures'
PNG=png(30,20,lambda y: bytes([30,50,70,128])*30)
SVG=b'<svg viewBox="0 0 20 20"><rect width="20" height="20" fill="red"/></svg>'
class Origin(http.server.BaseHTTPRequestHandler):
    fallback=[]
    slow=threading.Event()
    def log_message(self,*args):pass
    def send(self,status,data=b'',headers=None):
        self.send_response(status)
        for k,v in (headers or {}).items():self.send_header(k,v)
        self.send_header('Content-Length',str(len(data)))
        self.end_headers()
        try:self.wfile.write(data)
        except (BrokenPipeError,ConnectionResetError):pass
    def do_GET(self):
        if self.path=='/health':return self.send(200,b'ok',{'X-SB-Public-Fetch':'1'})
        if self.path.startswith('/https/fallback.test/'):
            route=self.path[len('/https/fallback.test/'):]
            mode,encoded=route.split('/',1)
            source=U.unquote(encoded.split('?w=')[0]);self.fallback.append((mode,source))
            if mode=='slow':
                self.slow.set();time.sleep(3);return self.send(200,PNG)
            if mode=='oversize':return self.send(200,b'',{'Content-Length':'9999999'})
            if mode=='chunk-error':
                self.send_response(404);self.send_header('Connection','close');self.end_headers()
                try:self.wfile.write(b'x'*600000)
                except (BrokenPipeError,ConnectionResetError):pass
                return
            if mode=='missing':return self.send(404)
            if mode=='invalid':return self.send(200,b'<html>not artwork</html>')
            if mode=='redirect':return self.send(302,b'',{'Location':'http://image.test/final'})
            if mode=='unsafe':return self.send(302,b'',{'Location':'http://127.0.0.1/private'})
            if mode=='loop':return self.send(302,b'',{'Location':'https://fallback.test/loop/'+encoded})
            return self.send(200,(FIX/'baseline.jpg').read_bytes() if mode=='jpeg' else PNG,{'Content-Type':'application/octet-stream'})
        path=U.urlsplit(self.path).path
        if path=='/final':return self.send(200,PNG)
        if path=='/large':
            self.send_response(200);self.send_header('Content-Length',str(9*1024*1024));self.end_headers();return
        if path=='/upstream413':return self.send(413)
        if path in ['/404','/401','/403','/502']:return self.send(int(path[1:]))
        if path=='/jpeg':return self.send(200,(FIX/'baseline.jpg').read_bytes())
        if path=='/progressive':return self.send(200,(FIX/'progressive.jpg').read_bytes())
        if path=='/resource':return self.send(200,(FIX/'jpeg-coefficient-limit.jpg').read_bytes())
        if path=='/png':return self.send(200,PNG)
        if path=='/svg':return self.send(200,SVG)
        if path=='/blocked-svg':return self.send(200,b'<svg><script>bad</script></svg>')
        return self.send(200,(FIX/'unsupported.webp').read_bytes())

def main():
    server,renderer,web=sys.argv[1:4]
    origin=http.server.ThreadingHTTPServer(('127.0.0.1',0),Origin)
    threading.Thread(target=origin.serve_forever,daemon=True).start()
    with tempfile.TemporaryDirectory() as temp:
        root=Path(temp);config=root/'config';config.mkdir()
        with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
        def settings(mode='png',enabled=True,timeout=1,maxbytes=524288):
            (config/'settings.conf').write_text(f'artwork_external_fallback_enabled={int(enabled)}\nartwork_external_fallback_base_url=https://fallback.test/{mode}/\nartwork_external_fallback_timeout={timeout}\nartwork_external_fallback_max_bytes={maxbytes}\n')
        settings()
        env=os.environ|{'SB_IMAGE_RENDERER':str(Path(renderer).resolve()),'SB_IMAGE_CACHE_ROOT':str(root/'cache'),
                        'SB_IMAGE_TEST_IP':'127.0.0.1','SB_IMAGE_TEST_PORT':str(origin.server_port),
                        'SB_IMAGE_PROXY_URL':f'http://127.0.0.1:{origin.server_port}'}
        with (root/'server.log').open('wb') as log:
            process=subprocess.Popen([server,'--port',str(port),'--web-root',web,'--config-dir',str(config)],env=env,stdout=log,stderr=log)
            def path(source):return '/api/image/render?url='+U.quote(source,safe='')+'&width=320&height=240'
            def request(source):
                try:
                    with R.urlopen(f'http://127.0.0.1:{port}'+path(source),timeout=24) as r:return r.status,r.read(),r.headers
                except urllib.error.HTTPError as e:return e.code,e.read(),e.headers
            try:
                for _ in range(100):
                    try:
                        with socket.create_connection(('127.0.0.1',port),.1):break
                    except OSError:time.sleep(.05)
                for name in ['jpeg','progressive','png','svg']:
                    before=len(Origin.fallback);assert request('http://image.test/'+name)[0]==200;assert len(Origin.fallback)==before
                for name in ['unsupported','large','upstream413','resource']:
                    status,data,_=request('http://image.test/'+name)
                    assert status==200 and decode(data)[:2]==(30,20),(name,status,data)
                    assert decode(data)[2](0,0)[3]==128,'fallback transparency'
                settings(maxbytes=32);assert request('http://image.test/unsupported?v=tiny-limit')[0]==415
                settings('jpeg')
                status,data,_=request('http://image.test/unsupported?v=jpeg')
                assert status==200 and decode(data)[:2]==(320,240)
                settings()
                source='http://image.test/unsupported?size=large&v=1%202'
                assert request(source)[0]==200
                assert Origin.fallback[-1][1]==source,Origin.fallback[-1]
                before=len(Origin.fallback);assert request(source)[2]['X-SB-Image-Cache']=='hit';assert len(Origin.fallback)==before
                for mode in ['invalid','oversize','missing','slow','unsafe','loop','chunk-error']:
                    settings(mode)
                    source='http://image.test/unsupported?v='+mode
                    assert request(source)[0]==415,mode
                    before=len(Origin.fallback);assert request(source)[0]==415;assert len(Origin.fallback)==before,'backoff'
                settings('redirect');assert request('http://image.test/unsupported?v=redirect')[0]==200
                settings()
                for name in ['404','401','403','502','blocked-svg']:
                    before=len(Origin.fallback);assert request('http://image.test/'+name)[0]!=200;assert len(Origin.fallback)==before
                for source in ['http://127.0.0.1/private','http://10.0.0.1/x','ftp://image.test/x','http://user:pass@image.test/x',
                               'http://image.test/unsupported?token=secret','http://image.test/unsupported?%74oken=secret','http://image.test/unsupported?opaque=secret',
                               'https://fallback.test/invalid/unsupported']:
                    before=len(Origin.fallback);assert request(source)[0]!=200;assert len(Origin.fallback)-before<=int(source.startswith('https://fallback.test'))
                settings(enabled=False);before=len(Origin.fallback)
                assert request('http://image.test/unsupported?v=disabled')[0]==415;assert len(Origin.fallback)==before
                settings('slow',timeout=5);Origin.slow.clear()
                source='http://image.test/unsupported?v=coalesce'
                with concurrent.futures.ThreadPoolExecutor(2) as pool:
                    a=pool.submit(request,source);assert Origin.slow.wait(4)
                    b=pool.submit(request,source)
                    assert request('http://image.test/unsupported?v=busy')[0]==503
                    assert a.result()[0]==200 and b.result()[0]==200
                settings('slow',timeout=5);Origin.slow.clear()
                # Disconnect the only waiter while fallback is active.
                sock=socket.create_connection(('127.0.0.1',port));sock.sendall(('GET '+path('http://image.test/unsupported?v=cancel')+' HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n').encode())
                assert Origin.slow.wait(4);sock.close();time.sleep(.5)
                settings();assert request('http://image.test/png?v=aftercancel')[0]==200
                assert 'reason=backoff' in (root/'server.log').read_text()
                assert 'source=external_fallback' in (root/'server.log').read_text()
                print('artwork fallback: local codecs, eligibility, JPEG/PNG signatures, bounds, errors, privacy, encoding, cache, backoff, concurrency and cancellation passed')
            except Exception:
                print((root/'server.log').read_text(),file=sys.stderr);raise
            finally:process.terminate();process.wait(timeout=5)
    origin.shutdown()
if __name__=='__main__':main()
