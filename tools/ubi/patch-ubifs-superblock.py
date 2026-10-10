#!/usr/bin/env python3
"""Patch max_leb_cnt on a disposable UBIFS image and repair its node CRC.

This research utility refuses device files and in-place modification.  A valid
result only proves superblock syntax; it does not prove mount/LPT compatibility.
"""
import argparse
import os
import stat
import struct
import zlib

MAGIC = 0x06101831
NODE_SIZE = 4096


def ubifs_crc(data: bytes) -> int:
    # Equivalent to crc32(UBIFS_CRC32_INIT, node + 8, len - 8) in Linux 2.6.27.
    return zlib.crc32(data, 0) ^ 0xFFFFFFFF


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("max_leb_cnt", type=int)
    args = parser.parse_args()
    src, dst = os.path.abspath(args.input), os.path.abspath(args.output)
    if src == dst:
        raise SystemExit("refusing in-place modification")
    mode = os.stat(src).st_mode
    if not stat.S_ISREG(mode):
        raise SystemExit("input must be a regular file")
    if args.max_leb_cnt <= 0:
        raise SystemExit("invalid max_leb_cnt")
    image = bytearray(open(src, "rb").read())
    if len(image) < NODE_SIZE or struct.unpack_from("<I", image, 0)[0] != MAGIC:
        raise SystemExit("not a UBIFS image")
    node_len = struct.unpack_from("<I", image, 16)[0]
    if node_len != NODE_SIZE:
        raise SystemExit("unexpected superblock node length")
    stored = struct.unpack_from("<I", image, 4)[0]
    if stored != ubifs_crc(image[8:node_len]):
        raise SystemExit("input superblock CRC mismatch")
    leb_cnt = struct.unpack_from("<I", image, 40)[0]
    if args.max_leb_cnt < leb_cnt:
        raise SystemExit("max_leb_cnt cannot be below leb_cnt")
    struct.pack_into("<I", image, 44, args.max_leb_cnt)
    struct.pack_into("<I", image, 4, ubifs_crc(image[8:node_len]))
    with open(dst, "xb") as stream:
        stream.write(image)
        stream.flush()
        os.fsync(stream.fileno())
    print(f"input_leb_cnt={leb_cnt}")
    print(f"output_max_leb_cnt={args.max_leb_cnt}")
    print(f"output_crc=0x{struct.unpack_from('<I', image, 4)[0]:08x}")
    print("mount_compatibility_proven=false")


if __name__ == "__main__":
    main()
