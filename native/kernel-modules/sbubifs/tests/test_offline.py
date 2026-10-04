#!/usr/bin/env python3
"""Offline SBUBIFS artifact, ABI and source-policy checks."""
import os
import pathlib
import struct
import subprocess
import tempfile


MODULE = pathlib.Path(os.environ["SBUBIFS_MODULE"])
VENDOR_MODULE = pathlib.Path(os.environ["VENDOR_MODULE"])
KERNEL = pathlib.Path(os.environ["KERNEL_SRC"])
BUILD_SRC = pathlib.Path(os.environ["SBUBIFS_BUILD_SRC"])
PREFIX = os.environ.get("CROSS_COMPILE", "arm-none-linux-gnueabi-")
OBJCOPY = os.environ.get("ANALYSIS_OBJCOPY", PREFIX + "objcopy")


def output(*args):
    return subprocess.check_output(args, text=True)


def versions(module):
    with tempfile.TemporaryDirectory() as tmp:
        section = pathlib.Path(tmp) / "versions.bin"
        subprocess.check_call(
            [OBJCOPY, "--dump-section", f"__versions={section}", module]
        )
        data = section.read_bytes()
    assert len(data) % 64 == 0
    result = {}
    for offset in range(0, len(data), 64):
        entry = data[offset : offset + 64]
        name = entry[4:].split(b"\0", 1)[0].decode("ascii")
        result[name] = struct.unpack("<I", entry[:4])[0]
    return result


assert MODULE.is_file() and MODULE.stat().st_size > 100_000
metadata = output(PREFIX + "strings", MODULE)
for required in (
    "vermagic=2.6.26.8-rt16 preempt mod_unload modversions ARMv5 ",
    "license=GPL",
    "depends=",
    "sbubifs",
    "sbubifs_%d_%d",
    "sbubifs_inode_slab",
    "sbubifs_bgt%d_%d",
):
    assert required in metadata, required

sections = output(PREFIX + "readelf", "-S", MODULE)
assert "__ksymtab" not in sections, "prototype must not export clone symbols"

module_versions = versions(MODULE)
vendor_versions = versions(VENDOR_MODULE)
overlap = set(module_versions) & set(vendor_versions)
assert len(overlap) >= 2
assert all(module_versions[name] == vendor_versions[name] for name in overlap)

symvers = {}
for line in (KERNEL / "Module.symvers").read_text().splitlines():
    crc, name, *_ = line.split()
    symvers[name] = int(crc, 16)
missing = sorted(set(module_versions) - set(symvers))
mismatch = sorted(
    name for name, crc in module_versions.items() if symvers.get(name) != crc
)
assert not missing, missing
assert not mismatch, mismatch

super_source = (BUILD_SRC / "super.c").read_text()
header = (BUILD_SRC / "ubifs.h").read_text()
for policy in (
    'vi->ubi_num != 0',
    'vi->vol_id != 5',
    'allowed_name[] = "sbdata"',
    'vi->vol_type != UBI_DYNAMIC_VOLUME',
    'vi->corrupted',
    'vi->upd_marker',
):
    assert policy in super_source, policy
assert super_source.count("sbubifs_volume_allowed(") == 3
assert '.name    = "sbubifs"' in super_source
assert 'bdi_register(&c->bdi, NULL, "sbubifs_%d_%d"' in super_source
assert 'kmem_cache_create("sbubifs_inode_slab"' in super_source
assert '#define BGT_NAME_PATTERN "sbubifs_bgt%d_%d"' in header

# The cloned source differs from the vendor tree only in identity/safety files.
vendor_files = sorted((KERNEL / "fs/ubifs").glob("*.[ch]"))
for vendor in vendor_files:
    clone = BUILD_SRC / vendor.name
    assert clone.is_file()
    if vendor.name not in {"super.c", "ubifs.h"}:
        assert vendor.read_bytes() == clone.read_bytes(), vendor.name

print(
    "PASS: offline SBUBIFS checks; "
    f"size={MODULE.stat().st_size}, imports={len(module_versions)}, "
    f"vendor_crc_overlap={len(overlap)}"
)
