#!/usr/bin/env python3
"""Reproduce Linux 2.6.27 UBIFS small-LPT geometry calculations.

This is an offline calculator.  It never opens a device and intentionally does
not contain superblock-writing functionality.
"""
import argparse
import json

FANOUT = 4
CRC_BITS = 16
TYPE_BITS = 4


def fls(value: int) -> int:
    return value.bit_length()


def div_round_up(value: int, divisor: int) -> int:
    return (value + divisor - 1) // divisor


def geometry(max_lebs: int, leb_cnt: int, *, leb_size: int = 129024,
             min_io: int = 2048, log_lebs: int = 3, lpt_lebs: int = 2,
             orph_lebs: int = 1, lsave_cnt: int = 256,
             big_lpt: bool = False) -> dict:
    fixed = 1 + 2 + log_lebs + lpt_lebs + orph_lebs
    main_lebs = leb_cnt - fixed
    if main_lebs <= 0 or max_lebs < leb_cnt:
        raise ValueError("invalid LEB counts")
    capacity_main = main_lebs + max_lebs - leb_cnt
    max_pnodes = div_round_up(capacity_main, FANOUT)
    height, slots = 1, FANOUT
    while slots < max_pnodes:
        height += 1
        slots *= FANOUT
    pnodes = div_round_up(main_lebs, FANOUT)
    n = div_round_up(pnodes, FANOUT)
    nnodes = n
    for _ in range(1, height):
        n = div_round_up(n, FANOUT)
        nnodes += n
    space_bits = fls(leb_size) - 3
    lpt_lnum_bits = fls(lpt_lebs)
    lpt_offs_bits = fls(leb_size - 1)
    lpt_spc_bits = fls(leb_size)
    pcnt_bits = fls(div_round_up(max_lebs, FANOUT) - 1)
    lnum_bits = fls(max_lebs - 1)
    pbits = CRC_BITS + TYPE_BITS + (pcnt_bits if big_lpt else 0)
    pbits += (space_bits * 2 + 1) * FANOUT
    pnode_sz = div_round_up(pbits, 8)
    nbits = CRC_BITS + TYPE_BITS + (pcnt_bits if big_lpt else 0)
    nbits += (lpt_lnum_bits + lpt_offs_bits) * FANOUT
    nnode_sz = div_round_up(nbits, 8)
    ltab_sz = div_round_up(CRC_BITS + TYPE_BITS + lpt_lebs * lpt_spc_bits * 2, 8)
    lsave_sz = div_round_up(CRC_BITS + TYPE_BITS + lnum_bits * lsave_cnt, 8)
    raw_lpt = pnodes * pnode_sz + nnodes * nnode_sz + ltab_sz + lsave_sz
    size = raw_lpt
    wastage = max(pnode_sz, nnode_sz)
    size += wastage
    total_wastage = wastage
    while size > leb_size:
        size += wastage - leb_size
        total_wastage += wastage
    total_wastage += div_round_up(size, min_io) * min_io - size
    lpt_size = raw_lpt + total_wastage
    required_lebs = div_round_up(2 * lpt_size, leb_size)
    return {
        "max_leb_cnt": max_lebs, "leb_cnt": leb_cnt, "main_lebs": main_lebs,
        "lnum_bits": lnum_bits, "pcnt_bits": pcnt_bits,
        "pnode_sz": pnode_sz, "nnode_sz": nnode_sz, "lpt_height": height,
        "pnode_cnt": pnodes, "nnode_cnt": nnodes, "lpt_size": lpt_size,
        "required_lpt_lebs": required_lebs, "reserved_lpt_lebs": lpt_lebs,
        "reservation_ok": required_lebs <= lpt_lebs,
        "small_lpt": not big_lpt and lpt_size <= leb_size,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("max_lebs", nargs="+", type=int)
    parser.add_argument("--current-lebs", type=int,
                        help="keep leb_cnt at this value; default grows to max")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    rows = [geometry(value, args.current_lebs or value) for value in args.max_lebs]
    if args.json:
        print(json.dumps(rows, indent=2, sort_keys=True))
        return
    print("max leb main lnum pcnt psz nsz h pnodes nnodes lpt_bytes need reserved small")
    for r in rows:
        print("{max_leb_cnt:3} {leb_cnt:3} {main_lebs:4} {lnum_bits:4} "
              "{pcnt_bits:4} {pnode_sz:3} {nnode_sz:3} {lpt_height:1} "
              "{pnode_cnt:6} {nnode_cnt:6} {lpt_size:9} "
              "{required_lpt_lebs:4} {reserved_lpt_lebs:8} {small_lpt}".format(**r))


if __name__ == "__main__":
    main()
