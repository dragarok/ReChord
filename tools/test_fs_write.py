#!/usr/bin/env python3
"""Host check for the FAT32 write path of firmware/services/fs_target.c.

Builds FAT32 images itself (long names, subdirectories, a full directory
cluster, a long name crossing a sector, orphan long-name entries, gaps
between files), runs firmware/services/test_fs_write.c on them, then checks
every image with an independent FAT32 reader written here (both FATs equal,
chains vs sizes, no cross-links, no lost clusters, FSInfo, long-name runs
and checksums, contents against a model).

Power-cut test: the same scenario is re-run with the block device failing
every write after the first N, for every N; each image must still hold
every file either in its old or its new state, with at worst lost clusters
(or, mid-append, a chain longer than the size).

No mkfs / mtools / fsck.fat needed.   Usage: python3 tools/test_fs_write.py"""
import os, re, shutil, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = tempfile.mkdtemp(prefix='fsw_')
EXE = os.path.join(T, 'test_fs_write')
SEC = 512
EOC = 0x0FFFFFF8


def pat(seed, n):
    return bytes((seed * 31 + i * 7 + (i >> 9)) & 0xFF for i in range(n))


# ---------------------------------------------------------------- builder

def sfn_sum(raw):
    s = 0
    for c in raw:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def lfn_run(name, raw):
    units = [ord(c) for c in name] + [0]
    units += [0xFFFF] * (-len(units) % 13)
    parts = [units[i:i + 13] for i in range(0, len(units), 13)]
    out = []
    for i, part in enumerate(parts):
        e = bytearray(32)
        e[0] = (i + 1) | (0x40 if i == len(parts) - 1 else 0)
        e[11], e[13] = 0x0F, sfn_sum(raw)
        for j, u in enumerate(part):
            off = 1 + 2 * j if j < 5 else 14 + 2 * (j - 5) if j < 11 else 28 + 2 * (j - 11)
            struct.pack_into('<H', e, off, u)
        out.append(bytes(e))
    return out[::-1]


def sfn_entry(raw, attr, clus, size):
    e = bytearray(32)
    e[0:11] = raw
    e[11] = attr
    struct.pack_into('<HHHHHHHI', e, 14, 0x6000, 0x5C21, 0x5C21, clus >> 16, 0x6000, 0x5C21,
                     clus & 0xFFFF, size)
    return bytes(e)


class Dir:
    def __init__(self):
        self.items = []          # ('file', name, data) | ('dir', name, Dir) | ('raw', bytes)
        self.n = 0

    def file(self, name, data):
        self.items.append(('file', name, data))
        return self

    def dir(self, name):
        d = Dir()
        self.items.append(('dir', name, d))
        return d

    def raw(self, e):
        self.items.append(('raw', e))


def short_of(name, used):
    base, _, ext = name.upper().rpartition('.') if '.' in name else (name.upper(), '', '')
    ok = lambda t: all(c.isalnum() or c in "!#$%&'()-@^_`{}~" for c in t)
    if name == name.upper() and 0 < len(base) <= 8 and len(ext) <= 3 and ok(base) and ok(ext):
        raw = (base.ljust(8) + ext.ljust(3)).encode()
        used.add(raw)
        return raw, False
    clean = ''.join(c for c in base if c.isalnum())
    ext = ''.join(c for c in ext if c.isalnum())[:3]
    for n in range(1, 100):
        raw = (f'{clean[:7 - len(str(n))]}~{n}'.ljust(8) + ext.ljust(3)).encode()
        if raw not in used:
            used.add(raw)
            return raw, True


def nent(d):
    """Entries of directory d: ".", "..", one per raw entry, long name + 8.3 per item."""
    n = 2
    for it in d.items:
        n += 1 if it[0] == 'raw' else 1 + (-(-(len(it[1]) + 1) // 13) if short_of(it[1], set())[1] else 0)
    return n


def build(path, root, total_secs=None, spc=1, mbr=False, fsinfo_known=True, gap=True):
    total_secs = total_secs or 68000 * spc + 4096
    part = 2048 if mbr else 0
    vol = total_secs - part
    rsvd, nf = 32, 2
    fatsz = ((vol - rsvd) // spc + 2) * 4 // SEC + 1
    nclus = (vol - rsvd - nf * fatsz) // spc
    assert nclus >= 65525, nclus
    data0 = rsvd + nf * fatsz
    cl = spc * SEC
    img = bytearray(total_secs * SEC)
    fat = [0x0FFFFFF8, 0x0FFFFFFF] + [0] * nclus
    nxt = [2]

    def alloc(n):
        first = nxt[0]
        for i in range(n):
            fat[first + i] = first + i + 1 if i < n - 1 else 0x0FFFFFFF
        nxt[0] += n + (1 if gap else 0)          # free holes between objects
        return first

    def put(c, blob):
        off = (part + data0 + (c - 2) * spc) * SEC
        img[off:off + len(blob)] = blob

    def lay(d, me, parent):
        used, ents, subs = set(), [], []
        if me is not None:
            ents += [sfn_entry(b'.          ', 0x10, me, 0), sfn_entry(b'..         ', 0x10, parent, 0)]
        else:
            ents.append(sfn_entry(b'ECHO MINI  ', 0x08, 0, 0))
        for it in d.items:
            if it[0] == 'raw':
                ents.append(it[1])
                continue
            kind, name, obj = it
            raw, lfn = short_of(name, used)
            if kind == 'file':
                first = alloc(-(-len(obj) // cl)) if obj else 0
                for i in range(0, len(obj), cl):
                    put(first + i // cl, obj[i:i + cl])
                e = sfn_entry(raw, 0x20, first, len(obj))
            else:
                first = alloc(max(1, -(-(32 * nent(obj)) // cl)))
                subs.append((obj, first))
                e = sfn_entry(raw, 0x10, first, 0)
            ents += (lfn_run(name, raw) if lfn else []) + [e]
        return ents, subs

    def emit(d, first, parent):
        ents, subs = lay(d, first, parent)
        blob = b''.join(ents)
        n = 0
        c = first
        while True:                               # dir chain from alloc()
            put(c, blob[n:n + cl].ljust(cl, b'\0'))
            n += cl
            if fat[c] >= EOC:
                break
            c = fat[c]
        assert n >= len(blob), 'directory larger than allocated'
        for sd, sf in subs:
            emit(sd, sf, 0 if first == root_first else first)

    root_first = alloc(4)
    emit(root, root_first, 0)
    free = sum(1 for x in fat[2:] if x == 0)
    bs = bytearray(SEC)
    bs[0:3] = b'\xEB\x58\x90'
    bs[3:11] = b'MSDOS5.0'
    struct.pack_into('<HBHBHHBHHHII', bs, 11, SEC, spc, rsvd, nf, 0, 0, 0xF8, 0, 63, 255, part, vol)
    struct.pack_into('<IHHIHH', bs, 36, fatsz, 0, 0, root_first, 1, 6)
    struct.pack_into('<BBBI', bs, 64, 0x80, 0, 0x29, 0x20261015)
    bs[71:82] = b'ECHO MINI  '
    bs[82:90] = b'FAT32   '
    bs[510:512] = b'\x55\xAA'
    fsi = bytearray(SEC)
    struct.pack_into('<I', fsi, 0, 0x41615252)
    struct.pack_into('<III', fsi, 484, 0x61417272, free if fsinfo_known else 0xFFFFFFFF,
                     nxt[0] if fsinfo_known else 0xFFFFFFFF)
    fsi[510:512] = b'\x55\xAA'
    for s, blob in ((0, bs), (1, fsi), (6, bs), (7, fsi)):
        img[(part + s) * SEC:(part + s + 1) * SEC] = blob
    fb = b''.join(struct.pack('<I', x) for x in fat)
    for k in range(nf):
        o = (part + rsvd + k * fatsz) * SEC
        img[o:o + len(fb)] = fb
    if mbr:
        struct.pack_into('<B3sB3sII', img, 446, 0, b'\0\2\0', 0x0C, b'\xFE\xFF\xFF', part, vol)
        img[510:512] = b'\x55\xAA'
    open(path, 'wb').write(img)


# ---------------------------------------------------------------- checker

class Bad(Exception):
    pass


def check(path, strict=True):
    """Independent FAT32 reader. Returns ({path: bytes}, info). Raises Bad.
    strict=False (after a power cut) allows lost clusters, a chain longer
    than the size, a stale FSInfo and differing FAT copies -- nothing else."""
    img = open(path, 'rb').read()
    part = 0
    if not (img[0] in (0xEB, 0xE9) and struct.unpack_from('<H', img, 11)[0] == SEC):
        part = struct.unpack_from('<I', img, 0x1C6)[0]
    b = img[part * SEC:(part + 1) * SEC]
    spc, rsvd, nf = b[13], struct.unpack_from('<H', b, 14)[0], b[16]
    vol = struct.unpack_from('<I', b, 32)[0]
    fatsz, = struct.unpack_from('<I', b, 36)
    root, fsi_sec = struct.unpack_from('<IH', b, 44)
    data0 = rsvd + nf * fatsz
    nclus = (vol - data0) // spc
    cl = spc * SEC
    fats = [img[(part + rsvd + k * fatsz) * SEC:(part + rsvd + (k + 1) * fatsz) * SEC] for k in range(nf)]
    if strict and any(f != fats[0] for f in fats[1:]):
        raise Bad('FAT copies differ')
    fat = [x & 0x0FFFFFFF for x in struct.unpack_from(f'<{nclus + 2}I', fats[0])]
    if fat[0] != 0x0FFFFFF8 or fat[1] != 0x0FFFFFFF:
        raise Bad('reserved FAT entries changed')
    owner = {}
    info = {'orphans': 0, 'lost': 0, 'long_chain': 0}

    def chain(first, who):
        out, c = [], first
        while True:
            if not 2 <= c < nclus + 2:
                raise Bad(f'{who}: cluster {c:#x} out of range')
            if c in owner:
                raise Bad(f'{who}: cluster {c:#x} cross-linked with {owner[c]}')
            if fat[c] == 0:
                raise Bad(f'{who}: chain runs into free cluster {c:#x}')
            owner[c] = who
            out.append(c)
            if fat[c] >= EOC:
                return out
            if fat[c] in (1, 0x0FFFFFF7) or fat[c] >= nclus + 2:
                raise Bad(f'{who}: bad FAT value {fat[c]:#x} at {c:#x}')
            c = fat[c]

    def read(cs):
        return b''.join(img[(part + data0 + (c - 2) * spc) * SEC:][:cl] for c in cs)

    files = {}

    def walk(first, dpath):
        ents = read(chain(first, dpath or '/'))
        run, names = None, set()
        for i in range(0, len(ents), 32):
            e = ents[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                if run:
                    info['orphans'] += 1
                run = None
                continue
            if e[11] == 0x0F:
                if e[0] & 0x40:
                    if run:
                        info['orphans'] += 1
                    run = {'n': e[0] & 0x3F, 'next': e[0] & 0x3F, 'sum': e[13], 'u': {}}
                if not run or (e[0] & 0x3F) != run['next'] or e[13] != run['sum']:
                    if run:
                        info['orphans'] += 1
                    run = None
                    continue
                k = e[0] & 0x3F
                run['u'][k] = [struct.unpack_from('<H', e, o)[0] for o in
                               (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)]
                run['next'] -= 1
                continue
            raw = e[:11]
            if e[11] & 0x08 or raw[0] == ord('.'):
                if run:
                    info['orphans'] += 1
                run = None
                continue
            if any(c < 0x20 or c in b'"*+,./:;<=>?[\\]|' for c in raw):
                raise Bad(f'{dpath}: bad 8.3 name {raw!r}')
            name = raw[:8].decode().rstrip() + ('.' + raw[8:].decode().rstrip() if raw[8:] != b'   ' else '')
            if run:
                if run['next'] == 0 and run['sum'] == sfn_sum(raw):
                    u = sum((run['u'][k] for k in range(1, run['n'] + 1)), [])
                    name = ''.join(chr(x) for x in u[:u.index(0)] if True) if 0 in u else ''.join(map(chr, u))
                else:
                    info['orphans'] += 1
            run = None
            if name.upper() in names or raw in names:
                raise Bad(f'{dpath}: duplicate {name}')
            names |= {name.upper(), raw}
            first_c = struct.unpack_from('<H', e, 20)[0] << 16 | struct.unpack_from('<H', e, 26)[0]
            size, = struct.unpack_from('<I', e, 28)
            p = f'{dpath}/{name}'
            if e[11] & 0x10:
                walk(first_c, p)
                continue
            if first_c == 0:
                if size:
                    raise Bad(f'{p}: size {size} without clusters')
                files[p] = b''
                continue
            cs = chain(first_c, p)
            need = -(-size // cl)
            if len(cs) < need:
                raise Bad(f'{p}: {len(cs)} clusters for {size} bytes')
            if len(cs) > max(need, 1):
                if strict:
                    raise Bad(f'{p}: chain of {len(cs)} clusters for {size} bytes')
                info['long_chain'] += 1
            files[p] = read(cs)[:size]
        if run:
            info['orphans'] += 1

    walk(root, '')
    used = sum(1 for x in fat[2:] if x)
    lost = used - len(owner)
    info['lost'] = lost
    if lost and strict:
        raise Bad(f'{lost} lost clusters')
    fsi = img[(part + fsi_sec) * SEC:(part + fsi_sec + 1) * SEC]
    free_cnt, hint = struct.unpack_from('<II', fsi, 488)
    if struct.unpack_from('<I', fsi, 0)[0] != 0x41615252:
        raise Bad('FSInfo signature')
    if strict and free_cnt != 0xFFFFFFFF and free_cnt != nclus - used:
        raise Bad(f'FSInfo free {free_cnt} != {nclus - used}')
    if hint != 0xFFFFFFFF and not 2 <= hint < nclus + 2:
        raise Bad(f'FSInfo hint {hint:#x}')
    info['free'] = free_cnt
    info['real_free'] = nclus - used
    return files, info


# ---------------------------------------------------------------- fixture

def fixture(spc):
    r = Dir()
    r.file('RECHORD.CFG', pat(1, 700))
    r.file('Echo Log File.txt', pat(2, 1500))
    m = r.dir('Music')
    for i in range(12):
        m.file(f'track number {i:02d} with a rather long file name.flac', pat(40 + i, 100 * i + 3))
    full = r.dir('FULL')
    for i in range(spc * 16 - 2):                 # exactly one cluster of entries
        full.file(f'F{i:02d}.TXT', pat(60 + i, i % 3 * 50))
    # orphan long names (as fw_update_retire can leave them) whose checksum
    # matches NEW.TXT: a NEW.TXT entry placed right behind one adopts it
    o = r.dir('ORPH')                             # orphan, then a deleted entry
    o.raw(lfn_run('orphan name', b'NEW     TXT')[0])
    o.raw(b'\xE5' + sfn_entry(b'XDELETEDTXT', 0x20, 0, 0)[1:])
    o2 = r.dir('ORPH2')                           # orphan, then the end marker
    o2.raw(lfn_run('orphan two', b'NEW     TXT')[0])
    for i in range(4):
        r.file(f'P{i}.BIN', pat(80 + i, 10))
    # root so far: label, CFG, 2+1 log, 1+1 Music, FULL, ORPH, ORPH2, P0..P3 = 14 entries
    # -> this name's 3 long-name entries start at entry 14 and cross into sector 2
    r.file('Span long name crossing sector.dat', pat(3, 2000))
    r.file('Keep Me Long Name.txt', pat(4, 3000))
    return r


def model_of(r, base=''):
    out = {}
    for it in r.items:
        if it[0] == 'file':
            out[f'{base}/{it[1]}'] = it[2]
        elif it[0] == 'dir':
            out.update(model_of(it[2], f'{base}/{it[1]}'))
    return out


def run(img, cmds, cut=None):
    args = [EXE, img] + (['--cut', str(cut)] if cut is not None else []) + cmds
    p = subprocess.run(args, capture_output=True, text=True)
    if p.returncode:
        raise SystemExit(f'harness failed ({p.returncode}): {p.stderr}\n{p.stdout}')
    return p.stdout.splitlines()


def fnv(b):
    h = 2166136261
    for x in b:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h


def fail(msg):
    print('FAIL:', msg)
    sys.exit(1)


# ---------------------------------------------------------------- tests

def scenario(spc, mbr, fsinfo_known):
    img = os.path.join(T, f'full_{spc}_{int(mbr)}.img')
    build(img, fixture(spc), spc=spc, mbr=mbr, fsinfo_known=fsinfo_known)
    files0, info0 = check(img)
    model = model_of(fixture(spc))
    assert files0 == model and info0['orphans'] == 2, 'fixture self-check'
    I = '/Internal'
    cmds = [
        'w:/SD/RO.TXT:10:1:10', 'rm:/SD/RECHORD.CFG',                # read-only volume
        'w:/Internal/NoDir/X.TXT:10:1:10', 'w:/Internal/not 8.3 name.txt:10:1:10',   # a long name (fsw)
        'rm:/Internal/Music', 'rm:/Internal/NOPE.TXT', 'busy:/Internal/RECHORD.CFG',
        'w:/Internal/RECHORD.CFG:300:10:300',                         # rewrite shorter
        'w:/Internal/RECHORD.CFG:5000:11:100',                        # rewrite longer
        'w:/Internal/SHOT1.BMP:109312:12:5120',                       # big, aligned
        'w:/Internal/MIS.BIN:20000:13:1000:1',                        # misaligned source
        'a:/Internal/Echo Log File.txt:3000:14:37',                   # append, LFN name
        'a:/Internal/ECHOLO~1.TXT:100:15:100',                        # append by 8.3 alias
        'a:/Internal/NEWLOG.TXT:600:16:50', 'a:/Internal/newlog.txt:600:17:600',
        'w:/Internal/FULL/NEW1.TXT:10:18:10', 'w:/Internal/FULL/NEW2.TXT:700:19:64',
        'w:/Internal/ORPH/NEW.TXT:100:20:100', 'w:/Internal/ORPH2/NEW.TXT:100:21:100',
        'rm:/Internal/Span long name crossing sector.dat',
        'rm:/Internal/Music/track number 03 with a rather long file name.flac',
        'w:/Internal/EMPTY.TXT:0:1:1', 'a:/Internal/EMPTY.TXT:10:22:10',
        'w:/Internal/ZERO.TXT:0:1:1',
        'rm:/Internal/MIS.BIN', 'w:/Internal/AFTER.BIN:3000:23:3000',
    ] + [f'w:/Internal/Music/N{i:02d}.TXT:{i * 97}:{30 + i}:256' for i in range(40)]
    exp = {
        '/RECHORD.CFG': pat(11, 5000), '/SHOT1.BMP': pat(12, 109312),
        '/Echo Log File.txt': pat(2, 1500) + pat(14, 3000) + pat(15, 100),
        '/NEWLOG.TXT': pat(16, 600) + pat(17, 600), '/FULL/NEW1.TXT': pat(18, 10),
        '/FULL/NEW2.TXT': pat(19, 700), '/ORPH/NEW.TXT': pat(20, 100), '/ORPH2/NEW.TXT': pat(21, 100),
        '/EMPTY.TXT': pat(22, 10), '/ZERO.TXT': b'', '/AFTER.BIN': pat(23, 3000),
        '/not 8.3 name.txt': pat(1, 10),
    }
    for i in range(40):
        exp[f'/Music/N{i:02d}.TXT'] = pat(30 + i, i * 97)
    del model['/Span long name crossing sector.dat']
    del model['/Music/track number 03 with a rather long file name.flac']
    model.update(exp)
    cmds += ['cat:' + I + p for p in sorted(model)] + ['ex:/Internal/MIS.BIN', 'ex:/Internal/FULL']
    out = run(img, cmds)
    o = '\n'.join(out)
    want_lines = ['mount 0 0', 'writable 1 0', 'w /SD/RO.TXT open-fail', 'rm /SD/RECHORD.CFG -4',
                  'w /Internal/NoDir/X.TXT open-fail',
                  'rm /Internal/Music -1', 'rm /Internal/NOPE.TXT -2',
                  'busy /Internal/RECHORD.CFG 1 1 -1', 'ex /Internal/MIS.BIN 0', 'ex /Internal/FULL 1']
    for w in want_lines:
        if w not in out:
            fail(f'spc={spc}: missing "{w}"\n{o}')
    for line in out:
        m = re.search(r' wrote (\d+) of (\d+) close (-?\d+) calls (\d+) secs (\d+)$', line)
        if m and (m[1] != m[2] or m[3] != '0'):
            fail(f'spc={spc}: {line}')
        if line.startswith('rm /Internal/') and line.split()[-1] != '0' and 'Music -1' not in line and 'NOPE' not in line:
            fail(f'spc={spc}: {line}')
    for p, data in model.items():
        if f'cat {I}{p} {len(data)} {fnv(data):08x}' not in out:
            fail(f'spc={spc}: read-back of {p} differs: ' + next((l for l in out if l.startswith(f'cat {I}{p} ')), 'none'))
    # the 108 KB screenshot-sized write: multi-sector writes, not per sector
    big = re.search(r'calls (\d+) secs (\d+)$', next(l for l in out if l.startswith('w /Internal/SHOT1.BMP')))
    calls, secs = int(big[1]), int(big[2])
    if secs > 109312 // SEC + 30 or calls > (40 if fsinfo_known else 100):
        fail(f'spc={spc}: SHOT1.BMP took {calls} write calls / {secs} sectors')
    files, info = check(img)
    if files != model:
        diff = sorted(set(files) ^ set(model)) or [p for p in model if files[p] != model[p]]
        fail(f'spc={spc}: image content differs: {diff[:5]}')
    if info['orphans'] != 2:
        fail(f'spc={spc}: orphan long names {info["orphans"]} (fixture has 2)')
    print(f'scenario spc={spc} mbr={mbr} fsinfo={"known" if fsinfo_known else "unknown"}: '
          f'{len(model)} files OK, FSInfo free {info["free"]:#x} (real {info["real_free"]}), '
          f'SHOT1.BMP {calls} write calls')


def crash(spc):
    base = os.path.join(T, f'crash_base_{spc}.img')
    build(base, fixture(spc), spc=spc)
    old = model_of(fixture(spc))
    cmds = ['w:/Internal/RECHORD.CFG:5000:21:512', 'a:/Internal/Echo Log File.txt:2000:22:300',
            'w:/Internal/FULL/NEW1.TXT:1000:23:1000', 'rm:/Internal/Span long name crossing sector.dat',
            'w:/Internal/SHOT1.BMP:30000:24:5120', 'rm:/Internal/Keep Me Long Name.txt']
    img = os.path.join(T, f'crash_{spc}.img')
    shutil.copyfile(base, img)
    total = int(run(img, cmds)[-1].split()[2])
    allowed = {
        '/RECHORD.CFG': [pat(1, 700), pat(21, 5000)],
        '/Echo Log File.txt': [pat(2, 1500), pat(2, 1500) + pat(22, 2000)],
        '/FULL/NEW1.TXT': [None, b'', pat(23, 1000)],
        '/SHOT1.BMP': [None, b'', pat(24, 30000)],
        '/Span long name crossing sector.dat': [None, pat(3, 2000)],
        '/Keep Me Long Name.txt': [None, pat(4, 3000)],
    }
    aliases = {'/SPANLO~1.DAT': '/Span long name crossing sector.dat', '/KEEPME~1.TXT': '/Keep Me Long Name.txt'}
    seen = {k: set() for k in allowed}
    for cut in range(total + 1):
        shutil.copyfile(base, img)
        run(img, cmds, cut)
        try:
            files, info = check(img, strict=False)
        except Bad as e:
            fail(f'power cut after write {cut}/{total} (spc={spc}): {e}')
        for a, real in aliases.items():                      # long name gone first
            if a in files:
                files[real] = files.pop(a)
        for p in set(old) | set(allowed):
            got = files.get(p)
            ok = allowed.get(p, [old.get(p)])
            if got not in ok:
                fail(f'power cut after write {cut}/{total} (spc={spc}): {p} is '
                      f'{None if got is None else len(got)} bytes, not old/new')
            if p in seen:
                seen[p].add(ok.index(got))
        extra = set(files) - set(old) - set(allowed)
        if extra:
            fail(f'power cut after write {cut}/{total}: unexpected {sorted(extra)}')
    # the full run ends in the new state everywhere
    files, info = check(img)
    print(f'power cut spc={spc}: {total + 1} cut points, every file old or new, '
          f'no cross-links; states seen ' + ', '.join(f'{os.path.basename(k)}:{sorted(v)}' for k, v in seen.items()))


def screenshot():
    exe = os.path.join(T, 'test_screenshot')
    d = os.path.join(ROOT, 'firmware/drivers')
    subprocess.check_call([os.environ.get('HOSTCC', 'cc'), '-O2', '-Wall', '-Werror',
                           '-I' + os.path.join(ROOT, 'firmware'), '-I' + d, '-o', exe,
                           os.path.join(ROOT, 'firmware/services/test_screenshot.c'),
                           os.path.join(ROOT, 'firmware/services/screenshot.c'),
                           os.path.join(ROOT, 'firmware/services/fs_target.c'), os.path.join(ROOT, 'firmware/services/fs_write_ext.c'),
                           os.path.join(ROOT, 'firmware/services/fs_write_host.c')] +
                          [os.path.join(d, f) for f in ('display.c', 'text_stub.c', 'font.c', 'font8x8.c',
                                                        'panel_init_rknanoc.c', 'panel_io_sim.c')])
    img = os.path.join(T, 'shot.img')
    build(img, fixture(1))
    out = subprocess.run([exe, img], capture_output=True, text=True, check=True).stdout.split()
    if out[:3] != ['shots', '1', '2']:
        fail(f'screenshot: {out}')
    files, _ = check(img)
    a, b = files['/SHOT1.BMP'], files['/SHOT2.BMP']
    W, H = 320, 170
    if a != b or len(a) != 512 + W * H * 2 or a[:2] != b'BM':
        fail('screenshot: bad file')
    off, = struct.unpack_from('<I', a, 10)
    w, h, planes, bpp, comp = struct.unpack_from('<iiHHI', a, 18)
    masks = struct.unpack_from('<III', a, 54)
    if (off, w, h, bpp, comp, masks) != (512, W, -H, 16, 3, (0xF800, 0x07E0, 0x001F)):
        fail(f'screenshot header {(off, w, h, bpp, comp, masks)}')
    px = lambda x, y: struct.unpack_from('<H', a, off + (y * W + x) * 2)[0]
    want = {(0, 0): 0xF800, (9, 9): 0xF800, (10, 10): 0, (W - 1, H - 1): 0x07E0,
            (150, 85): 0x001F, (150, 95): 0}
    for xy, v in want.items():
        if px(*xy) != v:
            fail(f'screenshot pixel {xy} = {px(*xy):#06x}, want {v:#06x}')
    if not any(px(x, y) == 0xFFFF for x in range(8, 64) for y in range(40, 48)):
        fail('screenshot: text missing')
    bmp = os.path.join(T, 'SHOT1.BMP')
    open(bmp, 'wb').write(a)
    if shutil.which('sips'):                         # an independent BMP decoder
        p = subprocess.run(['sips', '-g', 'pixelWidth', '-g', 'pixelHeight', bmp],
                           capture_output=True, text=True).stdout
        if 'pixelWidth: 320' not in p or 'pixelHeight: 170' not in p:
            fail(f'sips cannot read the BMP: {p}')
    print(f'screenshot: SHOT1/SHOT2.BMP {len(a)} bytes, pixels OK, {out[4]} block writes'
          + (', sips decodes it' if shutil.which('sips') else ''))


def main():
    cc = os.environ.get('HOSTCC', 'cc')
    subprocess.check_call([cc, '-O2', '-Wall', '-Wextra', '-Wno-sign-compare', '-Werror',
                           '-I' + os.path.join(ROOT, 'firmware'), '-o', EXE,
                           os.path.join(ROOT, 'firmware/services/test_fs_write.c'),
                           os.path.join(ROOT, 'firmware/services/fs_target.c'), os.path.join(ROOT, 'firmware/services/fs_write_ext.c'),
                           os.path.join(ROOT, 'firmware/services/fs_write_host.c')])
    scenario(1, False, True)
    scenario(1, True, False)
    scenario(4, True, True)
    screenshot()
    crash(1)
    crash(4)
    shutil.rmtree(T)
    print('fs_target write: OK')


if __name__ == '__main__':
    main()
