#!/usr/bin/env python3
import os
import shutil
import subprocess
import time

ROUTE = "/sbstandalonebase/"
ROOT = "/var/www/bytestack_nl_usr/data/www/bytestack.nl"
FILES = (
    "/etc/nginx/conf.d/parking.conf",
    "/etc/nginx/conf.d/sbstandalone-ip.conf",
    "/etc/nginx/fastpanel2-sites/bytestack_nl_usr/bytestack.nl.includes",
)


def route_block(with_root):
    root = f"        root {ROOT};\n" if with_root else ""
    return (
        f"    location ^~ {ROUTE} {{\n"
        f"{root}"
        "        keepalive_timeout 0;\n"
        "        add_header Connection close always;\n"
        "        try_files $uri =404;\n"
        "    }\n\n"
    )


stamp = time.strftime("%Y%m%d-%H%M%S")
backups = []
try:
    for path in FILES:
        with open(path, "r", encoding="utf-8") as handle:
            content = handle.read()
        if ROUTE in content:
            continue
        backup = f"{path}.bak.{stamp}"
        shutil.copy2(path, backup)
        backups.append((path, backup))
        block = route_block(path.endswith(".conf"))
        if path.endswith(".includes"):
            updated = content.rstrip() + "\n\n" + block.lstrip()
        else:
            marker = "    location / {"
            if marker not in content:
                raise RuntimeError(f"location marker not found in {path}")
            updated = content.replace(marker, block + marker, 1)
        temporary = f"{path}.standalonebase.tmp"
        with open(temporary, "w", encoding="utf-8") as handle:
            handle.write(updated)
        shutil.copystat(path, temporary)
        stat = os.stat(path)
        os.chown(temporary, stat.st_uid, stat.st_gid)
        os.replace(temporary, path)
    subprocess.run(["nginx", "-t"], check=True)
except Exception:
    for path, backup in reversed(backups):
        shutil.copy2(backup, path)
    raise

subprocess.run(["systemctl", "reload", "nginx"], check=True)
print(f"Nginx route active; backup timestamp: {stamp}")
