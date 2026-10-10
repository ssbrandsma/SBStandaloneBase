#!/usr/bin/env python3
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).parents[2]
patcher = root / "tools" / "ubi" / "patch-ubifs-superblock.py"
inspector = root / "build-host" / "sb-storage-helper"
source = root / "artifacts" / "ubi" / "sbdata-w4-r0.img"

with tempfile.TemporaryDirectory() as directory:
    output = Path(directory) / "patched.img"
    result = subprocess.run([sys.executable, str(patcher), str(source), str(output), "600"],
                            check=True, text=True, capture_output=True)
    assert "mount_compatibility_proven=false" in result.stdout
    data = output.read_bytes()[:4096]
    assert struct.unpack_from("<I", data, 44)[0] == 600
    if inspector.exists() and os.name != "nt":
        inspected = subprocess.run([str(inspector), "inspect-superblock", str(output)],
                                   check=True, text=True, capture_output=True).stdout
        assert "valid=true" in inspected
        assert "max_leb_cnt=600" in inspected
    repeat = subprocess.run([sys.executable, str(patcher), str(source), str(output), "600"],
                            capture_output=True)
    assert repeat.returncode != 0

print("superblock-patch-tests-ok")
