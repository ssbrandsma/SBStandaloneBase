#!/usr/bin/env python3
"""Host integration coverage for the native radio-station management API."""
import json, os, pathlib, socket, subprocess, sys, tempfile, time, urllib.error, urllib.request, uuid

COOKIE = None

def call(base, path, method="GET", data=None, token=True, content_type="application/json", headers=None):
    body = None if data is None else (data if isinstance(data, bytes) else json.dumps(data, ensure_ascii=False).encode())
    h = dict(headers or {})
    if token and COOKIE: h["Cookie"] = COOKIE
    if body is not None: h["Content-Type"] = content_type
    req = urllib.request.Request(base + path, body, h, method=method)
    try:
        with urllib.request.urlopen(req, timeout=3) as r: return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e: return e.code, e.read(), dict(e.headers)

def main():
    exe, web = sys.argv[1:3]
    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td); (root / "applets" / "StandaloneRadio").mkdir(parents=True); (root/"web").mkdir()
        port = 19000 + os.getpid() % 10000; base = f"http://127.0.0.1:{port}"
        env = os.environ | {"SB_RADIO_INTERNAL_ROOT": str(root / "radio"), "SB_APPLET_ROOTS": str(root / "applets")}
        p = subprocess.Popen([exe, "--port", str(port), "--web-root", web, "--config-dir", str(root / "web")], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            for _ in range(50):
                try: socket.create_connection(("127.0.0.1", port), .1).close(); break
                except OSError: time.sleep(.05)
            global COOKIE
            cap=(root/"web"/"remote.capability").read_text().strip()
            status,raw,_=call(base,"/api/internal/web-access/start","POST",b"",token=False,headers={"X-Remote-Capability":cap});pin=json.loads(raw)["pin"]
            status,raw,h=call(base,"/api/auth/pair","POST",{"pin":pin},token=False);COOKIE=h["Set-Cookie"].split(";",1)[0]
            assert call(base, "/api/radio/stations", token=False)[0] == 401
            status, raw, _ = call(base, "/api/radio/stations"); empty=json.loads(raw)
            assert status == 200 and empty == {"schemaVersion":1,"revision":0,"stations":[],"storageAvailable":True}
            station={"expectedRevision":0,"name":"Rádio Paradise","description":"Main, Mix","streamUrl":"https://example.org/live.mp3?a=1&token=x","logoUrl":"","logoFile":"","codec":"mp3","enabled":True,"order":10}
            status,raw,_=call(base,"/api/radio/stations","POST",station); cfg=json.loads(raw)
            assert status==200 and cfg["revision"]==1 and cfg["stations"][0]["id"]=="r-dio-paradise", (status, raw)
            stale=station|{"name":"Stale"}; assert call(base,"/api/radio/stations","POST",stale)[0]==409
            x=cfg["stations"][0]|{"expectedRevision":1,"name":"Edited","enabled":False}
            status,raw,_=call(base,"/api/radio/stations/r-dio-paradise","PUT",x); cfg=json.loads(raw)
            assert status==200 and cfg["revision"]==2 and not cfg["stations"][0]["enabled"]
            assert call(base,"/api/radio/export?format=csv")[1].startswith(b"id,name,description")
            imported={"schemaVersion":1,"revision":0,"stations":[{"id":"second","name":"Second","description":"","streamUrl":"http://example.net/radio","logoUrl":"","logoFile":"","codec":"auto","enabled":True,"order":20}]}
            status,raw,_=call(base,"/api/radio/import","POST",{"expectedRevision":2,"mode":"merge","config":imported}); cfg=json.loads(raw)
            assert status==200 and len(cfg["stations"])==2 and cfg["revision"]==3
            boundary="----sb"+uuid.uuid4().hex; png=b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01"+b"x"*20
            mp=(f"--{boundary}\r\nContent-Disposition: form-data; name=\"logo\"; filename=\"x.png\"\r\nContent-Type: image/png\r\n\r\n".encode()+png+f"\r\n--{boundary}--\r\n".encode())
            status,raw,_=call(base,"/api/radio/logos","POST",mp,content_type=f"multipart/form-data; boundary={boundary}"); logo=json.loads(raw)
            assert status==201 and logo["logoFile"].endswith(".png")
            assert call(base,"/api/radio/logos/../stations.json")[0] in (400,404)
            assert call(base,"/api/radio/stream-test","POST",{"url":"http://example.org"})[0]==501
            status,raw,_=call(base,"/api/radio/stations/second","DELETE",{"expectedRevision":3}); assert status==200 and json.loads(raw)["revision"]==4
            saved=json.loads((root/"radio"/"stations.json").read_text()); assert "storageAvailable" not in saved and saved["revision"]==4
            assert saved['stations'][0]['type']=='station'  # Legacy/default remains playable.
            unknown={**station,'expectedRevision':4,'name':'Unknown','type':'invalid','order':30}
            assert call(base,'/api/radio/stations','POST',unknown)[0]==400
            directory={**station,'expectedRevision':4,'name':'My Directory','type':'opml','streamUrl':'https://example.org/directory.opml?query=one&v=2','codec':'bogus','order':30,'logoFile':logo['logoFile']}
            for bad in ['ftp://example.org/test.opml','file:///etc/passwd','http://','http://?x=1','http://example.org:bad/a','http://example.org/a b']:
                assert call(base,'/api/radio/stations','POST',directory|{'streamUrl':bad})[0]==400,bad
            status,raw,_=call(base,'/api/radio/stations','POST',directory);cfg=json.loads(raw)
            assert status==200 and cfg['revision']==5,(status,raw)
            item=next(x for x in cfg['stations'] if x['type']=='opml');directory_id=item['id']
            assert item['codec']=='auto' and item['logoFile']==logo['logoFile'] and item['streamUrl']==directory['streamUrl']
            status,raw,_=call(base,'/api/radio/stations/'+directory_id,'PUT',item|{'expectedRevision':5,'name':'Edited Directory','enabled':False})
            cfg=json.loads(raw);assert status==200 and cfg['revision']==6
            ids=[directory_id,'r-dio-paradise']
            status,raw,_=call(base,'/api/radio/stations/reorder','POST',{'expectedRevision':6,'ids':ids});cfg=json.loads(raw)
            cfg['stations'].sort(key=lambda x:x['order'])
            assert status==200 and [x['id'] for x in cfg['stations']]==ids
            assert cfg['stations'][0]['type']=='opml' and not cfg['stations'][0]['enabled']
            assert b'order,type' in call(base,'/api/radio/export?format=csv')[1]
            # Restart the actual service against the saved station collection.
            p.terminate();p.wait(timeout=3)
            p=subprocess.Popen([exe,'--port',str(port),'--web-root',web,'--config-dir',str(root/'web')],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            for _ in range(50):
                try: socket.create_connection(('127.0.0.1',port),.1).close();break
                except OSError:time.sleep(.05)
            cap=(root/'web'/'remote.capability').read_text().strip()
            status,raw,_=call(base,'/api/internal/web-access/start','POST',b'',token=False,headers={'X-Remote-Capability':cap});pin=json.loads(raw)['pin']
            status,raw,h=call(base,'/api/auth/pair','POST',{'pin':pin},token=False);COOKIE=h['Set-Cookie'].split(';',1)[0]
            status,raw,_=call(base,'/api/radio/stations');cfg=json.loads(raw)
            cfg['stations'].sort(key=lambda x:x['order'])
            assert status==200 and cfg['stations'][0]['type']=='opml' and cfg['stations'][0]['name']=='Edited Directory'
            item=cfg['stations'][0]
            status,raw,_=call(base,'/api/radio/stations/'+directory_id,'PUT',item|{'expectedRevision':cfg['revision'],'enabled':True})
            cfg=json.loads(raw);assert status==200 and next(x for x in cfg['stations'] if x['id']==directory_id)['enabled']
            status,raw,_=call(base,'/api/radio/stations/'+directory_id,'DELETE',{'expectedRevision':cfg['revision']})
            assert status==200 and len(json.loads(raw)['stations'])==1

        finally:
            p.terminate(); p.wait(timeout=3)
    print("radio management integration tests passed")

if __name__ == "__main__": main()
