#!/usr/bin/env python3
"""Fixture-driven integration coverage for the process monitor."""
import json, os, pathlib, socket, subprocess, sys, tempfile, time, urllib.error, urllib.request

def request(base,path,cookie=None,method="GET",body=None,headers=None):
    data=None if body is None else (body if isinstance(body,bytes) else json.dumps(body).encode())
    h=dict(headers or {}); h.update({"Cookie":cookie} if cookie else {})
    if data is not None:h.setdefault("Content-Type","application/json")
    try:
        with urllib.request.urlopen(urllib.request.Request(base+path,data=data,method=method,headers=h),timeout=3) as r:return r.status,r.read(),dict(r.headers)
    except urllib.error.HTTPError as e:return e.code,e.read(),dict(e.headers)

def pair(base,config):
    cap=(config/"remote.capability").read_text().strip()
    status,body,_=request(base,"/api/internal/web-access/start",method="POST",body=b"",headers={"X-Remote-Capability":cap}); assert status==200
    pin=json.loads(body)["pin"]
    status,_,headers=request(base,"/api/auth/pair",method="POST",body={"pin":pin}); assert status==200
    return headers["Set-Cookie"].split(";",1)[0]

def stat(pid,name,ppid=1,utime=10,stime=4,start=100,vsize=409600,rss=20):
    fields={3:"S",4:ppid,14:utime,15:stime,22:start,23:vsize,24:rss}
    return f"{pid} ({name}) "+" ".join(str(fields.get(i,0)) for i in range(3,25))+"\n"

def main():
    executable,web=sys.argv[1:3]
    with tempfile.TemporaryDirectory() as d:
        root=pathlib.Path(d); proc=root/"proc"; config=root/"config"; proc.mkdir(); config.mkdir()
        (proc/"stat").write_text("cpu 100 2 30 800 4 1 3 0\nbtime 1700000000\n")
        (proc/"uptime").write_text("123.45 100.00\n")
        (proc/"meminfo").write_text("MemTotal: 60924 kB\nMemFree: 8000 kB\nBuffers: 1000 kB\nCached: 12000 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\nSlab: 500 kB\n")
        (proc/"net").mkdir(); (proc/"net/dev").write_text("Inter-| Receive | Transmit\n face |bytes packets errs drop fifo frame compressed multicast|bytes packets errs drop fifo colls carrier compressed\n wlan0: 1000 10 1 2 0 0 0 0 2000 20 3 4 0 0 0 0\n")
        (proc/"diskstats").write_text("31 0 mtdblock0 1 0 2 3 4 0 5 6 0 7 8\n")
        (proc/"sys/kernel/random").mkdir(parents=True); (proc/"sys/kernel/random/boot_id").write_text("boot-test\n")
        for pid,name,cmd in [("7","odd (worker) name",b"odd\x00arg\"x\x00"),("8","kernel thread",b"")]:
            p=proc/pid;p.mkdir();(p/"stat").write_text(stat(pid,name));(p/"cmdline").write_bytes(cmd)
        malformed=proc/"9";malformed.mkdir();(malformed/"stat").write_text("truncated\n")
        for pid in range(1000,1260):
            p=proc/str(pid);p.mkdir();(p/"stat").write_text(stat(pid,"load"));(p/"cmdline").write_bytes(b"load\x00")
        port=21500+os.getpid()%1000;base=f"http://127.0.0.1:{port}"
        env=os.environ|{"SB_MONITOR_PROC_ROOT":str(proc)}
        process=subprocess.Popen([executable,"--port",str(port),"--web-root",web,"--config-dir",str(config)],env=env)
        try:
            for _ in range(80):
                try:socket.create_connection(("127.0.0.1",port),.1).close();break
                except OSError:time.sleep(.05)
            assert request(base,"/api/monitor/snapshot")[0]==401
            cookie=pair(base,config);status,body,headers=request(base,"/api/monitor/snapshot",cookie);assert status==200
            data=json.loads(body);assert data["schema"]==1 and data["boot_id"]=="boot-test" and data["cpu"]["idle"]==800
            assert data["memory_kib"]["total"]==60924 and data["network"][0]["rx_dropped"]==2
            assert data["disk"]["available"] is False and len(data["processes"])==256 and data["skipped_processes"]>=7
            odd=next(p for p in data["processes"] if p["pid"]==7)
            assert odd["comm"]=="odd (worker) name" and odd["cmd"]=='odd arg"x'
            assert odd["ppid"]==1 and odd["user_ticks"]==10 and odd["system_ticks"]==4
            assert odd["start_ticks"]==100 and odd["vsize_bytes"]==409600 and odd["rss_pages"]==20
            assert headers["Cache-Control"]=="no-store"
            script=pathlib.Path(web,"js","performance.js").read_text();compact="".join(script.split());assert "INTERVALS=[1,2,5,10,30,60]" in compact and "WINDOW_SECONDS=600" in compact and "document.hidden" in script
        finally:
            process.terminate();process.wait(timeout=3)
    print("process-monitor-tests-ok")
if __name__=="__main__":main()
