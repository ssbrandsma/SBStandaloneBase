#!/usr/bin/env python3
"""Deterministic native-image fixtures; no device, network or flashing."""
import hashlib
import io
import pathlib
import random
import struct
import subprocess
import sys
import tempfile
import zipfile

COMMAND = sys.argv[1:]
assert COMMAND


def image(version=b"9.0.2 r17157\nfixture", board=b"baby:0001\nbaby:25000\n",
          compression=zipfile.ZIP_DEFLATED, extras=False,
          kernel=b"kernel" * 1000, root=b"root" * 3000, **changes):
    checksums = (hashlib.md5(kernel).hexdigest() + "  zImage\n" +
                 hashlib.md5(root).hexdigest() + "  root.cramfs\n").encode()
    entries = dict(zip(["jive.version", "board.version", "upgrade.md5", "zImage", "root.cramfs"],
                       [version, board, checksums, kernel, root]))
    entries.update(changes)
    result = io.BytesIO()
    with zipfile.ZipFile(result, "w", compression=compression) as archive:
        for name, payload in entries.items():
            info = zipfile.ZipInfo(name)
            info.compress_type = compression
            if extras:
                info.extra = struct.pack("<HHBI", 0x5455, 5, 1, 0)
            archive.writestr(info, payload)
    return result.getvalue()


with tempfile.TemporaryDirectory(prefix="sb-firmware-test-") as folder:
    path = pathlib.Path(folder) / "candidate.bin"

    def check(data, accepted=False, board=1, digest=None):
        path.write_bytes(data)
        before = hashlib.sha256(data).hexdigest()
        result = subprocess.run(COMMAND + [str(path), before if digest is None else digest, str(board)],
                                capture_output=True, text=True, timeout=20)
        assert (result.returncode == 0) == accepted, (result.stdout, result.stderr)
        assert hashlib.sha256(path.read_bytes()).hexdigest() == before

    check(image(), True)
    data = image()
    for algorithm in ('sha1', 'sha256'):
        checksum = hashlib.new(algorithm, data).hexdigest()
        check(data, True, digest=checksum)
        check(data, True, digest=checksum.upper())
        check(data, digest='0' * len(checksum))
        check(data[:-10], digest=checksum)
    for checksum in ['a' * 39, 'a' * 41, 'a' * 63, 'a' * 65, 'g' * 40, '!' * 64]:
        check(data, digest=checksum)
    check(image(compression=zipfile.ZIP_STORED), True)
    check(image(extras=True), True)
    # ZIP data crosses input/output boundaries with little compression, as in
    # a real already-compressed cramfs. A zero-input Z_BUF_ERROR is a refill.
    noise = random.Random(17157).randbytes(1024 * 1024)
    check(image(kernel=noise, root=noise + b'end'), True)
    check(image(), True, board=25000)
    check(image(), board=42)
    check(image(board=b"fab4:0001\n"))
    check(image(version=b"r17157"))
    check(image(version=b"9.0.2 r17157 suffix"))
    check(image(version=b"9.0.2 r9999999999999999999999"))
    check(image(), digest="0" * 64)
    check(image(), digest="")
    check(image(**{"upgrade.md5": b"0" * 32 + b"  zImage\n"}))
    check(image(**{"unexpected": b"extra"}))
    check(image(**{"../path": b"not extracted"}))
    data = image()
    check(data[:-10])
    check(data + b"trailing")
    corrupted = bytearray(data)
    corrupted[14] ^= 1
    check(corrupted)
    inflated = bytearray(data)
    struct.pack_into("<I", inflated, 22, 8193)
    check(inflated)
    path.write_bytes(b"x")
    with path.open("r+b") as stream:
        stream.truncate(24 * 1024 * 1024 + 1)
    result = subprocess.run(COMMAND + [str(path), "0" * 64, "1"], capture_output=True)
    assert result.returncode != 0

    for newer, installed, expected in [
        ("9.0.2 r17157", "9.0.1 r17084", 1),
        ("9.0.2 r100000", "9.0.2 r99999", 1),
        ("8.9.9 r99999", "9.0.0 r1", -1),
        ("9.0.2 r17157", "9.0.2 r17157", 0),
        ("9.0.2 r17156", "9.0.2 r17157", -1),
    ]:
        result = subprocess.run(COMMAND + ["compare", newer, installed],
                                capture_output=True, text=True, check=True)
        assert int(result.stdout) == expected
    for bad in ["9 r1", "9.0 r1", "9.0.2 r", "9.0.2 r-1", "9.0.2 r1 extra"]:
        result = subprocess.run(COMMAND + ["compare", bad, "9.0.2 r1"], capture_output=True)
        assert result.returncode != 0
print("Native firmware structure, bounds, checksums, boards and numeric versions passed")
