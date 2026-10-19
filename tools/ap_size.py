#!/usr/bin/env python3
"""Core A's memory budget, at every link (Makefile, the rechord_ap.elf rule).

    ap_size.py ELF --map MAP [--min-free BYTES] [--fonts BIN] [--out FILE] [--detail]

Prints what the build log keeps:

    core A: code 139420/161824 B (22404 free, keeps 4096), RAM 148720/180160 B (31376 free)
    core A overlays (window 32768 B at 0x03040000): usbdac 6940, usbout 7128, dev 8016 (largest 24752 free)

and writes the whole breakdown (--out; --detail prints it too): by subsystem (firmware/<dir>) and
by object, code / rodata / data / bss, resident and per overlay. "code" in the headline is what
the ROM loads and runs in place: .vectors + .text (with .rodata) + the .data load image, against
FW_CODE (firmware_ap.ld). Strings the linker merges (.rodata.*.str1.*) are given to each object in
proportion to its own strings before the merge (the map's "size before relaxing").

Exit 1 when FW_CODE has less than --min-free bytes free (ap.mk AP_MIN_FREE: why that number).
"""
import argparse
import collections
import os
import re
import struct
import sys


def elf_symbols(path):
    """{name: value} of an ELF32 little-endian file's symbol table."""
    d = open(path, "rb").read()
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    syms = {}
    for s in secs:
        if s[1] != 2:                                  # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for off in range(s[4], s[4] + s[5], 16):
            name, value = struct.unpack_from("<II", d, off)
            if name:
                end = d.index(b"\0", strtab[4] + name)
                syms[d[strtab[4] + name:end].decode()] = value
    return syms


def kind_of(sec):
    if sec.startswith((".bss", "COMMON")):
        return "bss"
    if sec.startswith(".data"):
        return "data"
    if sec.startswith(".rodata"):
        return "rodata"
    if sec.startswith((".text", ".vectors", ".ovl")):
        return "rodata" if ".api" in sec or sec.startswith(".ovl_hdr") else "code"
    return None


def parse_map(path):
    """[(output section, input section, object, size, size before merge or None)]"""
    text = open(path).read()
    text = text[text.index("Linker script and memory map"):]
    lines = text.split("\n")
    out, rows = None, []
    head = re.compile(r"^(\.\S+)\s*(0x[0-9a-f]+)?")
    one = re.compile(r"^ (\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o\S*|\S+\.a\(\S+\))$")
    name_only = re.compile(r"^ (\S+)$")
    cont = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o\S*|\S+\.a\(\S+\))$")
    relax = re.compile(r"^\s+0x([0-9a-f]+) \(size before relaxing\)")
    i = 0
    while i < len(lines):
        ln = lines[i]
        m = head.match(ln)
        if m and not ln.startswith(" "):
            out = m.group(1)
            i += 1
            continue
        sec = obj = None
        m = one.match(ln)
        if m:
            sec, size, obj = m.group(1), int(m.group(3), 16), m.group(4)
        else:
            m = name_only.match(ln)
            if m and i + 1 < len(lines) and cont.match(lines[i + 1]):
                c = cont.match(lines[i + 1])
                sec, size, obj = m.group(1), int(c.group(2), 16), c.group(3)
                i += 1
        if sec:
            before = None
            if i + 1 < len(lines) and relax.match(lines[i + 1]):
                before = int(relax.match(lines[i + 1]).group(1), 16)
                i += 1
            rows.append((out, sec, obj, size, before))
        i += 1
    return rows


def short(obj):
    obj = re.sub(r".*/libgcc\.a\((.*)\)", r"libgcc/\1", obj)
    obj = re.sub(r"^.*?objs/firmware/", "", obj)
    return obj


def account(rows):
    """{region: {object: Counter(kind)}}; region = 'resident' or an overlay's name."""
    acc = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    merged = collections.defaultdict(lambda: [0, collections.Counter()])   # region -> [output size, before]
    for out, sec, obj, size, before in rows:
        if out in (".vectors", ".text", ".data", ".bss"):
            region = "resident"
        elif out and out.startswith(".ovl_"):
            region = out[5:]
        else:
            continue
        k = kind_of(sec)
        if not k:
            continue
        o = short(obj)
        if before is not None and ".str" in sec:   # merged strings: shared out below
            merged[region][0] += size
            merged[region][1][o] += before
            continue
        acc[region][o][k] += size
    for region, (total, before) in merged.items():
        whole = sum(before.values()) or 1
        given = 0
        for o, b in sorted(before.items()):
            n = total * b // whole
            acc[region][o]["rodata"] += n
            given += n
        if before:                                       # the rounding, to the biggest
            acc[region][max(before, key=before.get)]["rodata"] += total - given
    return acc


def table(acc, title, by_dir):
    lines = [title, f"  {'':38s} {'code':>7s} {'rodata':>7s} {'data':>6s} {'bss':>7s} {'total':>7s}"]
    groups = collections.defaultdict(collections.Counter)
    for o, c in acc.items():
        groups[o.split("/")[0] if by_dir else o].update(c)
    tot = collections.Counter()
    for g, c in sorted(groups.items(), key=lambda kv: -(kv[1]["code"] + kv[1]["rodata"] + kv[1]["data"])):
        tot.update(c)
        lines.append(f"  {g:38s} {c['code']:7d} {c['rodata']:7d} {c['data']:6d} {c['bss']:7d} "
                     f"{c['code'] + c['rodata'] + c['data']:7d}")
    lines.append(f"  {'(all)':38s} {tot['code']:7d} {tot['rodata']:7d} {tot['data']:6d} {tot['bss']:7d} "
                 f"{tot['code'] + tot['rodata'] + tot['data']:7d}")
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("--map", required=True)
    ap.add_argument("--min-free", type=int, default=0)
    ap.add_argument("--fonts", help="the fonts module, against its window")
    ap.add_argument("--out", help="write the breakdown here")
    ap.add_argument("--detail", action="store_true", help="print the breakdown too")
    a = ap.parse_args()

    sym = elf_symbols(a.elf)
    rows = parse_map(a.map)
    acc = account(rows)
    sizes = {}
    for ln in open(a.map).read().split("\n"):
        m = re.match(r"^(\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)", ln)
        if m:
            sizes[m.group(1)] = int(m.group(3), 16)

    code_lo, code_hi = sym["ap_vectors"], sym["__fw_code_end"]
    used = sym["_data_load_start"] + sizes.get(".data", 0) - code_lo
    free = code_hi - code_lo - used
    ram_lo, ram_top = sym["_data_start"], sym["_noinit_start"]
    ram_used = sym["_bss_end"] - ram_lo
    ram_free = ram_top - 0x100 - sizes.get(".data", 0) - sym["_bss_end"]    # firmware_ap.ld's margin
    head = [f"core A: code {used}/{code_hi - code_lo} B ({free} free, keeps {a.min_free}), "
            f"RAM {ram_used}/{ram_top - ram_lo} B ({ram_free} free)"]
    win = sym.get("__ovl_win_end", 0) - sym.get("__ovl_win_start", 0)
    ovls = [(s[5:], n) for s, n in sizes.items() if s.startswith(".ovl_")]
    win2 = [(n, s) for n, s in ovls if n == "fsw"]          # the file system's window (firmware_ap.ld OVL_FS)
    lib = [(n, s) for n, s in ovls if n == "lib"]           # the library's (OVL_LIB)
    w1 = [(n, s) for n, s in ovls if n not in ("fsw", "lib")]
    if w1:
        head.append(f"core A overlays (window {win} B at 0x{sym['__ovl_win_start']:08X}): "
                    + ", ".join(f"{n} {s}" for n, s in w1)
                    + f" (largest {win - max(s for _, s in w1)} free)")
    if win2:
        fw2 = sym.get("__ovl_fs_end", 0) - sym.get("__ovl_fs_start", 0)
        head.append(f"core A file-system window ({fw2} B at 0x{sym['__ovl_fs_start']:08X}): "
                    + ", ".join(f"{n} {s}" for n, s in win2) + f" ({fw2 - win2[0][1]} free)")
    if lib:
        lw = sym.get("__ovl_lib_end", 0) - sym.get("__ovl_lib_start", 0)
        head.append(f"core A library window ({lw} B at 0x{sym['__ovl_lib_start']:08X}): lib {lib[0][1]} "
                    f"({lw - lib[0][1]} free)")
    if a.fonts and os.path.exists(a.fonts):
        fw = sym["__fonts_ram_end"] - sym["__fonts_ram_start"]
        fs = os.path.getsize(a.fonts)
        head.append(f"core A fonts window: {fs}/{fw} B ({fw - fs} free)")

    fill = used - sum(c["code"] + c["rodata"] + c["data"] for c in acc["resident"].values())
    detail = head + [""] + table(acc["resident"], "resident, by subsystem:", True) \
        + [f"  {'(alignment fill)':38s} {fill:39d}", ""] + table(acc["resident"], "resident, by object:", False)
    for n, _ in ovls:
        detail += [""] + table(acc[n], f"overlay {n}, by object:", False)
    if a.out:
        open(a.out, "w").write("\n".join(detail) + "\n")
    print("\n".join(detail if a.detail else head))
    if free < a.min_free:
        print(f"core A: {free} B of code space free, the build keeps {a.min_free} (ap.mk AP_MIN_FREE): "
              "put the feature in an overlay or free code first (docs/MEMORY_MAP.md)", file=sys.stderr)
        return 1
    if free < 2 * a.min_free:
        print(f"core A: warning: {free} B of code space free (the build stops below {a.min_free})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
