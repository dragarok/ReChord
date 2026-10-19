#!/usr/bin/env python3
"""Core A's overlays out of the full ELF, as the modules the image carries (Makefile, at every link).

    ovl_pack.py ELF --ovl usbdac:ov_dac [--ovl ...] --out DIR

For each overlay (the output section .ovl_<name>, firmware_ap.ld): its bytes, with the header's
len and crc set (firmware/services/ovl.h: crc = the device's CRC-32, tools/make_fw1.py
rknano_crc32, of all len bytes with the crc field as 0), written to DIR/<module>.bin for
make_fw1.py --module <module>=DIR/<module>.bin. Checks what services/ovl.c checks at load: the
magic, the id at the header's place, the ABI of this link (the __ovl_abi symbol), and that the
section sits at the window's start.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_fw1 import rknano_crc32  # noqa: E402

OVL_MAGIC = 0x314C564F            # "OVL1"
HDR = 24                          # magic, abi, id, len, crc, api
IDS = {"usbdac": 1, "usbout": 2, "dev": 3, "fsw": 4, "lib": 5, "libtag": 6}   # services/ovl.h OVL_*
WINDOW = {"lib": "__ovl_lib_start", "fsw": "__ovl_fs_start"}   # their own windows; the others: __ovl_win_start


def sections(d):
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    hdrs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    names = hdrs[shstrndx][4]
    out = {}
    for h in hdrs:
        name = d[names + h[0]:d.index(b"\0", names + h[0])].decode()
        out[name] = h
    return out, hdrs


def symbols(d, hdrs):
    syms = {}
    for s in hdrs:
        if s[1] != 2:
            continue
        strtab = hdrs[s[6]]
        for off in range(s[4], s[4] + s[5], 16):
            name, value = struct.unpack_from("<II", d, off)
            if name:
                syms[d[strtab[4] + name:d.index(b"\0", strtab[4] + name)].decode()] = value
    return syms


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("--ovl", action="append", default=[], metavar="NAME:MODULE")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    d = open(a.elf, "rb").read()
    secs, hdrs = sections(d)
    sym = symbols(d, hdrs)
    abi = sym["__ovl_abi"]
    for spec in a.ovl:
        name, module = spec.split(":")
        win = sym[WINDOW.get(name, "__ovl_win_start")]
        s = secs.get(".ovl_" + name)
        if not s:
            raise SystemExit(f"ovl_pack: no section .ovl_{name} in {a.elf}")
        addr, off, size = s[3], s[4], s[5]
        blob = bytearray(d[off:off + size])
        magic, h_abi, h_id = struct.unpack_from("<III", blob, 0)
        if addr != win or magic != OVL_MAGIC or h_id != IDS[name] or h_abi != abi or size < HDR:
            raise SystemExit(f"ovl_pack: .ovl_{name}: bad header (at 0x{addr:08X}, magic {magic:08X}, "
                             f"id {h_id}, abi {h_abi:08X} vs {abi:08X})")
        blob += bytes(-len(blob) % 4)
        struct.pack_into("<II", blob, 12, len(blob), 0)
        struct.pack_into("<I", blob, 16, rknano_crc32(bytes(blob)))
        path = os.path.join(a.out, module + ".bin")
        open(path, "wb").write(blob)
        print(f"  overlay {name}: {len(blob)} B -> {path} (crc {struct.unpack_from('<I', blob, 16)[0]:08X})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
