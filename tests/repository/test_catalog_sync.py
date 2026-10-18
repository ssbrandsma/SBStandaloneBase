#!/usr/bin/env python3
"""Exercise the real async refresh through an isolated fake loopback proxy."""
import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'monitor'))
from test_monitor_http import request, pair

PATH='/https/raw.githubusercontent.com/ssbrandsma/SBStandaloneBase/master/config.json'


def main():
    executable,web=sys.argv[1:3]
    control={'mode':'valid','guard':True,'requests':[],'hold':threading.Event()}
    release=dict(name='StandaloneBase',title='Standalone Base',version='0.4.4',
                 target='baby',min_target_version='7.7.3',
                 url='http://example.com/StandaloneBase-0.4.4.zip',sha='a'*40)

    class Proxy(http.server.BaseHTTPRequestHandler):
        def log_message(self,*args): pass

        def do_GET(self):
            control['requests'].append(self.path)
            if self.path=='/health':
                self.send_response(200)
                if control['guard']: self.send_header('X-SB-Public-Fetch','1')
                body=b'healthy'
            else:
                assert self.path==PATH
                assert self.headers['X-SB-Public-Fetch']=='1'
                mode=control['mode']
                if mode=='hold': control['hold'].wait(5)
                if mode=='missing':
                    self.send_response(404);body=b'not found'
                else:
                    self.send_response(200)
                    body=json.dumps({'server':{'name':'Do not import'},'applets':[release]}).encode()
                    if mode=='invalid': body=b'{not json}'
                    if mode=='oversized': body=b'x'*20000
                self.send_header('Content-Type','text/plain')
            self.send_header('Content-Length',str(len(body)))
            self.send_header('Connection','close')
            self.end_headers()
            try:self.wfile.write(body)
            except (BrokenPipeError,ConnectionResetError):pass

    proxy=http.server.ThreadingHTTPServer(('127.0.0.1',0),Proxy)
    proxy.daemon_threads=True
    thread=threading.Thread(target=proxy.serve_forever,daemon=True);thread.start()
    with tempfile.TemporaryDirectory() as folder:
        root=Path(folder);config=root/'config.json'
        config.write_text(json.dumps({'server':{'name':'Local name'},'applets':[]}))
        listener=socket.socket();listener.bind(('127.0.0.1',0));port=listener.getsockname()[1];listener.close()
        base=f'http://127.0.0.1:{port}'
        env=os.environ|{'SB_CATALOG_TEST_PROXY':f'http://127.0.0.1:{proxy.server_port}',
                        'SB_CATALOG_TEST_START_DELAY_MS':'600000',
                        'SB_CATALOG_TEST_TIMEOUT_MS':'1500',
                        'SBBASE_PID_FILE':str(root/'absent.pid')}
        env.pop('SB_CATALOG_SYNC_DISABLED',None)
        process=subprocess.Popen([executable,'--port',str(port),'--web-root',web,'--config-dir',str(root)],env=env)
        try:
            for _ in range(100):
                try:socket.create_connection(('127.0.0.1',port),.1).close();break
                except OSError:time.sleep(.05)
            assert request(base,'/api/catalog/status')[0]==401
            assert request(base,'/api/catalog/refresh',method='POST',body={})[0]==401
            cookie=pair(base,root)

            def status():
                code,body,_=request(base,'/api/catalog/status',cookie)
                assert code==200
                return json.loads(body)

            def begin():
                return request(base,'/api/catalog/refresh',cookie,method='POST',body={},headers={'Origin':base})[0]

            def wait(expected):
                deadline=time.monotonic()+5
                while time.monotonic()<deadline:
                    value=status()
                    if not value['busy'] and value['state']==expected:return value
                    time.sleep(.03)
                raise AssertionError(status())

            assert request(base,'/api/catalog/refresh',cookie,method='POST',body={},headers={'Origin':'https://evil.example'})[0]==403
            assert not control['requests']
            assert begin()==202
            value=wait('ok');assert value['changed']==1 and value['last_success']>0
            actual=json.loads(config.read_text())
            assert actual['server']=={'name':'Local name'} and actual['applets']==[release]
            assert control['requests']==['/health',PATH]
            before=config.stat(),config.read_bytes()
            assert begin()==202
            assert wait('ok')['changed']==0
            assert config.stat().st_ino==before[0].st_ino
            assert config.stat().st_mtime_ns==before[0].st_mtime_ns

            for mode in ['invalid','oversized','missing']:
                control['mode']=mode
                assert begin()==202
                wait('error')
                assert config.read_bytes()==before[1]

            control['guard']=False;control['mode']='valid'
            previous=len(control['requests'])
            assert begin()==202
            wait('error')
            assert control['requests'][previous:]==['/health']
            assert config.read_bytes()==before[1]

            control['guard']=True;control['mode']='hold'
            assert begin()==202
            time.sleep(.2)
            started=time.monotonic()
            assert request(base,'/api/auth/status',cookie)[0]==200
            assert time.monotonic()-started<1
            assert begin()==409
            wait('error')
            assert config.read_bytes()==before[1]
            control['hold'].set();control['mode']='valid'
            assert begin()==202
            wait('ok')

            # Becoming read-only while a request is in flight must also prevent
            # the eventual successful response from writing the catalog.
            control['hold'].clear();control['mode']='hold'
            assert begin()==202
            time.sleep(.2)
            (root/'settings.conf').write_text('readonly=1\n')
            control['hold'].set()
            wait('error')
            assert config.read_bytes()==before[1]
            assert begin()==403
            (root/'settings.conf').write_text('readonly=0\n')
            control['mode']='valid'
        finally:
            control['hold'].set()
            process.terminate();process.wait(timeout=3)

        # A fresh service automatically checks after its startup delay, without
        # a browser, pairing session, or manual-refresh request.
        config.write_text(json.dumps({'server':{'name':'Local name'},'applets':[]}))
        control['mode']='valid';control['requests'].clear()
        env['SB_CATALOG_TEST_START_DELAY_MS']='20'
        process=subprocess.Popen([executable,'--port',str(port),'--web-root',web,'--config-dir',str(root)],env=env)
        try:
            deadline=time.monotonic()+5
            while time.monotonic()<deadline:
                if json.loads(config.read_text())['applets']:break
                time.sleep(.03)
            assert json.loads(config.read_text())['applets']==[release]
            assert control['requests']==['/health',PATH]
        finally:
            process.terminate();process.wait(timeout=3)
        for disabled in ('readonly','environment'):
            config.write_text(json.dumps({'applets':[]}))
            control['requests'].clear()
            (root/'settings.conf').write_text('readonly='+('1' if disabled=='readonly' else '0')+'\n')
            if disabled=='environment':env['SB_CATALOG_SYNC_DISABLED']='1'
            process=subprocess.Popen([executable,'--port',str(port),'--web-root',web,'--config-dir',str(root)],env=env)
            try:
                time.sleep(.4)
                assert process.poll() is None
                assert not control['requests']
                assert json.loads(config.read_text())=={'applets':[]}
            finally:
                process.terminate();process.wait(timeout=3)
    proxy.shutdown();proxy.server_close()
    print('catalog-sync-tests-ok')


if __name__=='__main__':main()
