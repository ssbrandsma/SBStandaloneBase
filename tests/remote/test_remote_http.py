#!/usr/bin/env python3
import json, os, pathlib, struct, subprocess, sys, tempfile, time, urllib.error, urllib.request

binary, webroot = sys.argv[1:3]
cookie = None
port = 19081

def call(path, method="GET", body=None, auth=True, headers=None):
    data = None if body is None else json.dumps(body).encode()
    h = dict(headers or {})
    if auth and cookie: h["Cookie"] = cookie
    if data is not None: h["Content-Type"] = "application/json"
    request = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data, method=method, headers=h)
    try:
        with urllib.request.urlopen(request, timeout=3) as response:
            return response.status, response.read(), response.headers
    except urllib.error.HTTPError as error:
        return error.code, error.read(), error.headers

with tempfile.TemporaryDirectory() as directory:
    env = dict(os.environ)
    process = subprocess.Popen([binary, "--port", str(port), "--web-root", webroot, "--config-dir", directory], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        for _ in range(50):
            try:
                if call("/remote", auth=False)[0] == 200: break
            except OSError: pass
            time.sleep(.05)
        else: raise AssertionError("webserver did not start")
        client = "browser-client-0001"
        assert call("/api/remote/session", "POST", {"client":client,"source":"auto"}, auth=False)[0] == 401
        capability = pathlib.Path(directory,"remote.capability").read_text().strip()
        status,raw,_=call("/api/internal/web-access/start","POST",{},auth=False,headers={"X-Remote-Capability":capability});pin=json.loads(raw)["pin"]
        status,raw,pair_headers=call("/api/auth/pair","POST",{"pin":pin},auth=False)
        cookie=pair_headers["Set-Cookie"].split(";",1)[0]
        assert call("/api/remote/session", "POST", {"client":client,"padding":"x"*600})[0] == 413
        assert call("/api/remote/session", "POST", {"client":client,"source":"auto"}, headers={"Origin":"http://evil.invalid","Host":f"127.0.0.1:{port}"})[0] == 403
        assert call("/api/remote/session", "POST", {"client":client,"source":"auto"})[0] == 200
        for n in range(2,5): assert call("/api/remote/session", "POST", {"client":f"browser-client-000{n}","source":"auto"})[0] == 200
        assert call("/api/remote/session", "POST", {"client":"browser-client-0005","source":"auto"})[0] == 429
        assert call(f"/api/remote/frame?client={client}", auth=False)[0] == 401
        assert call(f"/api/remote/frame?client={client}&source=invalid")[0] == 400
        assert call(f"/api/remote/frame?client={client}")[0] == 503
        bmp_path=pathlib.Path("/tmp/standalonebase/remote-screen.bmp")
        bmp_path.parent.mkdir(parents=True,exist_ok=True)
        pixels=b"\x00\x00\xff\x00"*2
        bmp_path.write_bytes(b"BM"+struct.pack("<IHHI",54+len(pixels),0,0,54)+struct.pack("<IiiHHIIiiII",40,1,2,1,32,0,len(pixels),0,0,0,0)+pixels)
        status,body,headers=call(f"/api/remote/frame?client={client}&source=bmp")
        assert status == 200 and body.startswith(b"BM") and any("image/bmp" in value for value in headers.get_all("Content-Type",[]))
        bmp_sequence=headers["X-Frame-Sequence"]
        assert call(f"/api/remote/frame?client={client}&source=bmp&after={bmp_sequence}")[0] == 204
        assert call("/api/remote/input", "POST", {"client":client,"control":"shell","phase":"short","sequence":1})[0] == 400
        assert call("/api/remote/input", "POST", {"client":client,"control":"home","phase":"repeat","sequence":1})[0] == 400
        event={"client":client,"control":"home","phase":"short","sequence":1}
        assert call("/api/remote/input", "POST", event)[0] == 202
        assert json.loads(call("/api/remote/input", "POST", event)[1])["duplicate"] is True
        extras=[("encoder_ccw","short"),("encoder_cw","short"),("preset_6","long"),("volume_up","repeat")]
        for seq,(control,phase) in enumerate(extras,2):
            assert call("/api/remote/input", "POST", {"client":client,"control":control,"phase":phase,"sequence":seq})[0] == 202
        for seq in range(6,31):
            assert call("/api/remote/input", "POST", {"client":client,"control":"encoder_cw","phase":"short","sequence":seq})[0] == 202
        assert call("/api/remote/input", "POST", {"client":"browser-client-0002","control":"back","phase":"long","sequence":1})[0] == 202
        assert call("/api/remote/input", "POST", {"client":"browser-client-0002","control":"back","phase":"short","sequence":2})[0] == 503
        status, actions, _ = call("/api/remote/actions", headers={"X-Remote-Capability":capability}, auth=False)
        assert status == 200 and b"home\tshort\t1" in actions and b"back\tlong\t1" in actions
        assert b"encoder_ccw\tshort\t2" in actions and b"preset_6\tlong\t4" in actions and b"volume_up\trepeat\t5" in actions
        assert call("/api/remote/actions", auth=False)[0] == 403
        assert call("/webserver-settings", "PUT", {"readonly":True})[0] == 200
        assert call("/api/remote/input", "POST", {"client":client,"control":"home","phase":"short","sequence":31})[0] == 403
        time.sleep(16)
        status_data=json.loads(call("/api/remote/status")[1])
        assert status_data["clients"]==0 and not pathlib.Path("/tmp/standalonebase/remote-active").exists()
        print("remote-http-tests-ok")
    finally:
        process.terminate()
        try: process.wait(timeout=3)
        except subprocess.TimeoutExpired: process.kill()
