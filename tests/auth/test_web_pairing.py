#!/usr/bin/env python3
import json, os, pathlib, socket, subprocess, sys, tempfile, time, urllib.error, urllib.request

def request(base,path,method="GET",body=None,headers=None,cookie=None):
    data=None if body is None else (body if isinstance(body,bytes) else json.dumps(body).encode())
    h=dict(headers or {})
    if data is not None:h.setdefault("Content-Type","application/json")
    if cookie:h["Cookie"]=cookie
    req=urllib.request.Request(base+path,data=data,method=method,headers=h)
    try:
        with urllib.request.urlopen(req,timeout=3) as r:return r.status,r.read(),dict(r.headers)
    except urllib.error.HTTPError as e:return e.code,e.read(),dict(e.headers)

def main():
    binary,web=sys.argv[1:3]
    with tempfile.TemporaryDirectory() as td:
        port=19500+os.getpid()%1000;base=f"http://127.0.0.1:{port}"
        p=subprocess.Popen([binary,"--port",str(port),"--web-root",web,"--config-dir",td],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            for _ in range(60):
                try:socket.create_connection(("127.0.0.1",port),.1).close();break
                except OSError:time.sleep(.05)
            cap=pathlib.Path(td,"remote.capability").read_text().strip()
            internal={"X-Remote-Capability":cap}
            assert request(base,"/system-information")[0]==401
            assert request(base,"/system-information",headers={"X-Management-Token":"legacy-token"})[0]==401
            assert request(base,"/api/remote/frame?client=browser-client-0001")[0]==401
            assert request(base,"/api/internal/web-access/start","POST",b"")[0]==403
            status,raw,_=request(base,"/api/internal/web-access/start","POST",b"",internal);pin1=json.loads(raw)["pin"]
            assert status==200 and len(pin1)==6 and pin1.isdigit()
            status,raw,_=request(base,"/api/internal/web-access/start","POST",b"",internal);pin2=json.loads(raw)["pin"]
            assert request(base,"/api/auth/pair","POST",{"pin":pin1})[0]==401
            status,raw,h=request(base,"/api/auth/pair","POST",{"pin":pin2});cookie=h["Set-Cookie"].split(";",1)[0]
            assert status==200 and "HttpOnly" in h["Set-Cookie"] and "SameSite=Strict" in h["Set-Cookie"] and "Path=/" in h["Set-Cookie"] and "Max-Age=86400" in h["Set-Cookie"]
            assert request(base,"/api/auth/pair","POST",{"pin":pin2})[0]==401
            assert json.loads(request(base,"/api/auth/status",cookie=cookie)[1])["authenticated"]
            assert request(base,"/system-information",cookie=cookie)[0]==200
            assert request(base,"/webserver-settings","PUT",{"readonly":False},headers={"Origin":"http://evil.invalid","Host":f"127.0.0.1:{port}"},cookie=cookie)[0]==403
            client="browser-client-0001"
            assert request(base,"/api/remote/session","POST",{"client":client,"source":"bmp"},cookie=cookie)[0]==200
            request(base,"/api/internal/web-access/start","POST",b"",internal)
            assert request(base,f"/api/remote/frame?client={client}&source=bmp",cookie=cookie)[0]==423
            assert not pathlib.Path("/tmp/standalonebase/remote-screen.bmp").exists()
            request(base,"/api/internal/web-access/cancel","POST",b"",internal)
            assert request(base,"/api/auth/logout","POST",b"",cookie=cookie)[0]==200
            assert request(base,"/system-information",cookie=cookie)[0]==401
            status,raw,_=request(base,"/api/internal/web-access/start","POST",b"",internal);pin=json.loads(raw)["pin"]
            status,raw,h=request(base,"/api/auth/pair","POST",{"pin":pin});cookie2=h["Set-Cookie"].split(";",1)[0]
            assert request(base,"/api/internal/web-access/revoke-all","POST",b"",internal)[0]==200
            assert request(base,"/system-information",cookie=cookie2)[0]==401
            status,raw,_=request(base,"/api/internal/web-access/start","POST",b"",internal);pin=json.loads(raw)["pin"]
            for _ in range(5):assert request(base,"/api/auth/pair","POST",{"pin":"999999"})[0] in (401,429)
            assert request(base,"/api/auth/pair","POST",{"pin":pin})[0] in (401,429)
            assert request(base,"/api/auth/pair","POST",b"x"*129)[0]==413
            print("web-pairing-tests-ok")
        finally:
            p.terminate();p.wait(timeout=3)

if __name__=="__main__":main()
