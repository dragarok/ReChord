#!/usr/bin/env python3
"""Synthetic tag fixtures for test_tags.c, written to the directory given.

ID3/APE/RIFF/FLAC structures are built here byte by byte, so the test knows
exactly what each file holds; ffmpeg, lame, flac and metaflac make the audio
and a few files the way real tools tag them. The expected values live in
test_tags.c (k_cases), keyed by file name.
"""
import os
import struct
import subprocess
import sys

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)


def run(*cmd):
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode:
        sys.exit("%s failed:\n%s" % (cmd[0], r.stderr.decode(errors="replace")[-2000:]))


def path(name):
    return os.path.join(OUT, name)


def write(name, data):
    with open(path(name), "wb") as f:
        f.write(data)


# ---- audio ----------------------------------------------------------------
run("ffmpeg", "-y", "-f", "lavfi", "-i", "sine=f=440:d=0.3", "-ac", "1", "-ar", "44100",
    "-c:a", "pcm_s16le", path("_base.wav"))
run("lame", "--quiet", "-V2", path("_base.wav"), path("_base.mp3"))
run("flac", "--silent", "-f", "-o", path("_base.flac"), path("_base.wav"))
MP3 = open(path("_base.mp3"), "rb").read()
assert MP3[:3] != b"ID3" and MP3[-128:-125] != b"TAG"


# ---- ID3v2 ----------------------------------------------------------------
def ss(n):
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])


def txt(enc, s):
    """Text frame body: encoding byte + string (UTF-16 with BOM is little-endian)."""
    if enc == 0:
        return b"\0" + s.encode("latin-1")
    if enc == 1:
        return b"\1\xff\xfe" + s.encode("utf-16-le")
    if enc == 2:
        return b"\2" + s.encode("utf-16-be")
    return b"\3" + s.encode("utf-8")


def unsync(b):
    out = bytearray()
    for i, x in enumerate(b):
        out.append(x)
        if x == 0xFF and (i + 1 == len(b) or b[i + 1] >= 0xE0 or b[i + 1] == 0):
            out.append(0)
    return bytes(out)


def frame(ver, fid, data, flags=0):
    if ver == 2:
        return fid.encode() + len(data).to_bytes(3, "big") + data
    size = struct.pack(">I", len(data)) if ver == 3 else ss(len(data))
    return fid.encode() + size + struct.pack(">H", flags) + data


def id3(ver, frames, flags=0, pad=0, ext=b"", footer=False):
    body = ext + b"".join(frames) + bytes(pad)
    hdr = bytes([ver, 0, flags]) + ss(len(body))
    return b"ID3" + hdr + body + (b"3DI" + hdr if footer else b"")


def pic(n):
    return b"\0image/jpeg\0\3\0" + bytes((i * 7 + 1) & 0xFF for i in range(n))


def v1(title=b"", artist=b"", album=b"", track=0):
    pad = lambda s, n: s[:n].ljust(n, b"\0")
    return (b"TAG" + pad(title, 30) + pad(artist, 30) + pad(album, 30) + b"2026" +
            bytes(28) + (b"\0" + bytes([track]) if track else bytes(2)) + b"\xff")


def ape(items, header=True):
    """items: (key, value bytes, flags); flags 2 = binary."""
    body = b"".join(struct.pack("<II", len(v), fl) + k.encode() + b"\0" + v for k, v, fl in items)
    size = len(body) + 32
    tail = lambda fl: b"APETAGEX" + struct.pack("<IIII", 2000, size, len(items), fl) + bytes(8)
    return (tail(0xA0000000) if header else b"") + body + tail(0x80000000 if header else 0)


# ID3v2.2: Latin-1 accents, UTF-16, a PIC before the text frames
write("v22.mp3", id3(2, [
    frame(2, "PIC", b"\0JPG\3\0" + bytes(3000)),
    frame(2, "TT2", txt(0, "Café del Mar")),
    frame(2, "TP1", txt(1, "Ólafur Arnalds")),
    frame(2, "TAL", txt(0, "Album 22")),
    frame(2, "TRK", txt(0, "7/9")),
], pad=200) + MP3)

# ID3v2.3: UTF-16 with BOM (CJK, a surrogate pair), a 1 MB APIC first, lyrics,
# a compressed frame that must be skipped, a grouped frame
write("v23_utf16.mp3", id3(3, [
    frame(3, "APIC", pic(1 << 20)),
    frame(3, "USLT", b"\1eng\xff\xfe\0\0" + "la la la\n".encode("utf-16-le") * 2000),
    frame(3, "TPE1", txt(3, "compressed, must be skipped"), flags=0x0080),
    frame(3, "TIT2", txt(1, "東京事変 – 群青日和 𝄞")),
    frame(3, "TPE1", b"\x55" + txt(1, "椎名林檎"), flags=0x0020),
    frame(3, "TALB", b"\1\xfe\xff" + "教育".encode("utf-16-be")),
    frame(3, "TRCK", txt(0, "03/12")),
], pad=4096) + MP3)

# ID3v2.4: UTF-8, extended header, a multi-string frame, UTF-16BE album
write("v24_utf8.mp3", id3(4, [
    frame(4, "TIT2", txt(3, "Ünïcødé Title") + b"\0second string"),
    frame(4, "TPE1", txt(3, "Sigur Rós")),
    frame(4, "TALB", txt(2, "Ágætis byrjun")),
    frame(4, "TRCK", txt(3, "12")),
], flags=0x40, ext=ss(6) + b"\1\0", pad=100) + MP3)

# ID3v2.4: unsynchronised frames (per frame, and one with a data length indicator)
t = txt(1, "ÿÿ Unsync ÿ")                                  # BOM FF FE and FF FF in the text
a = txt(0, "Artist ÿ")
write("v24_unsync.mp3", id3(4, [
    frame(4, "TIT2", unsync(t), flags=0x0002),
    frame(4, "TPE1", ss(len(a)) + unsync(a), flags=0x0003),
    frame(4, "TALB", txt(3, "Plain")),
]) + MP3)

# ID3v2.4 with the tag-level unsync flag only (frames unflagged, as some writers do)
write("v24_tag_unsync.mp3", id3(4, [frame(4, "TIT2", unsync(txt(1, "ÿ Tag ÿ")))], flags=0x80) + MP3)

# ID3v2.3 whole-tag unsynchronisation: not supported, the tag is skipped
write("v23_tag_unsync.mp3", id3(3, [frame(3, "TIT2", txt(0, "skipped"))], flags=0x80) +
      MP3 + v1(b"From v1", b"V1 Artist", b"V1 Album", 9))

# ID3v1(.1) only, Latin-1, space padded
write("v1_only.mp3", MP3 + v1(b"Beyonc\xe9 Song   ", b"Beyonc\xe9", b"Caf\xe9 Album", 4))

# APEv2 only, with header, a 500 KB binary cover first
write("ape_hdr.mp3", MP3 + ape([
    ("Cover Art (Front)", b"cover.jpg\0" + bytes(500000), 2),
    ("Title", "日本語のタイトル".encode(), 0),
    ("ARTIST", b"Ape Artist", 0),
    ("album", b"Ape Album", 0),
    ("Track", b"2/10", 0),
]))

# APEv2 without header, then Lyrics3v2, then ID3v1: APE wins, ID3v1 fills the rest
ly = b"LYRICSBEGIN" + b"IND00003110" + b"LYR00010la la la!"
write("ape_nohdr_lyrics_v1.mp3", MP3 + ape([("Title", b"Ape Title", 0)], header=False) +
      ly + b"%06dLYRICS200" % len(ly) + v1(b"V1 Title", b"V1 Artist", b"V1 Album", 5))

# appended ID3v2.4 (footer "3DI") then ID3v1
write("appended.mp3", MP3 + id3(4, [
    frame(4, "APIC", pic(70000)),
    frame(4, "TIT2", txt(3, "Appended Title")),
    frame(4, "TPE1", txt(3, "Appended Artist")),
], footer=True) + v1(b"V1 Title", b"V1 Artist", b"V1 Album", 6))

# every source, each the first to have one field:
# front ID3v2 (title), APE (artist), appended ID3v2 (album), ID3v1 (track)
write("precedence.mp3",
      id3(3, [frame(3, "TIT2", txt(0, "Front Title"))]) + MP3 +
      id3(4, [frame(4, "TIT2", txt(3, "App Title")), frame(4, "TPE1", txt(3, "App Artist")),
              frame(4, "TALB", txt(3, "App Album"))], footer=True) +
      ape([("Title", b"Ape Title", 0), ("Artist", b"Ape Artist", 0)]) +
      v1(b"V1 Title", b"V1 Artist", b"V1 Album", 8))

# two ID3v2 tags in front: the first wins, the second fills
write("two_front.mp3", id3(3, [frame(3, "TIT2", txt(0, "First")), frame(3, "APIC", pic(5000))]) +
      id3(4, [frame(4, "TIT2", txt(3, "Second")), frame(4, "TPE1", txt(3, "Second Artist"))]) + MP3)

# truncation at a character boundary
write("trunc.mp3", id3(4, [
    frame(4, "TIT2", txt(3, "漢" * 200)),      # 3-byte chars: 31 fit in title[96]
    frame(4, "TPE1", txt(1, "é" * 100)),       # 2-byte chars: 31 fit in artist[64]
    frame(4, "TALB", txt(3, "a" + "🎵" * 40)), # 4-byte chars: 1 + 15 * 4 = 61 bytes
    frame(4, "TRCK", txt(3, " 0042 of 99")),
]) + MP3)

# ffmpeg's own ID3v2.3 (UTF-16 for non-ASCII) and ID3v2.4 (UTF-8)
for ver, name in (("3", "ffmpeg_v23.mp3"), ("4", "ffmpeg_v24.mp3")):
    run("ffmpeg", "-y", "-i", path("_base.mp3"), "-c", "copy", "-id3v2_version", ver,
        "-metadata", "title=Ffmpeg 東京 v" + ver, "-metadata", "artist=Björk",
        "-metadata", "album=Homogenic", "-metadata", "track=5/10", path(name))


# ---- WAV ------------------------------------------------------------------
def chunk(cid, data):
    return cid + struct.pack("<I", len(data)) + data + (b"\0" if len(data) & 1 else b"")


def wav(*chunks):
    body = b"WAVE" + b"".join(chunks)
    return b"RIFF" + struct.pack("<I", len(body)) + body


FMT = chunk(b"fmt ", struct.pack("<HHIIHH", 1, 1, 8000, 8000, 1, 8))
DATA = chunk(b"data", bytes(range(256)) * 300 + b"\x80")   # odd length: padded

info = lambda *kv: chunk(b"LIST", b"INFO" + b"".join(chunk(k, v + b"\0") for k, v in kv))
write("info.wav", wav(FMT, DATA, info((b"ISFT", b"Lavf"), (b"INAM", b"Mot\xf6rhead Live"),
                                      (b"IART", "張學友".encode()), (b"IPRD", b"Odd"),
                                      (b"IPRT", b"11/12"))))
write("id3chunk.wav", wav(FMT, DATA, chunk(b"id3 ", id3(3, [
    frame(3, "APIC", pic(20000)),
    frame(3, "TIT2", txt(1, "WAV ID3 Title")),
    frame(3, "TPE1", txt(0, "WAV ID3 Artist")),
    frame(3, "TRCK", txt(0, "3")),
]))))
# both: the first in the file wins
write("info_then_id3.wav", wav(FMT, info((b"INAM", b"Info Title")), DATA,
                               chunk(b"ID3 ", id3(4, [frame(4, "TIT2", txt(3, "Id3 Title")),
                                                      frame(4, "TALB", txt(3, "Id3 Album"))]))))
run("ffmpeg", "-y", "-i", path("_base.wav"), "-c", "copy", "-metadata", "title=Ffmpeg Wav",
    "-metadata", "artist=Ffmpeg Artist", "-metadata", "album=Ffmpeg Album",
    "-metadata", "track=9", path("ffmpeg.wav"))


# ---- FLAC -----------------------------------------------------------------
def flac_blocks(data):
    p, blocks = 4, []
    while True:
        h = data[p]
        n = int.from_bytes(data[p + 1:p + 4], "big")
        blocks.append((h & 0x7F, data[p + 4:p + 4 + n]))
        p += 4 + n
        if h & 0x80:
            return blocks, data[p:]


def flac_join(blocks, frames):
    out = b"fLaC"
    for i, (t, b) in enumerate(blocks):
        out += bytes([t | (0x80 if i == len(blocks) - 1 else 0)]) + len(b).to_bytes(3, "big") + b
    return out + frames


write("_cover.jpg", b"\xff\xd8\xff\xe0" + bytes((i * 13) & 0xFF for i in range(1500000)))
run("metaflac", "--remove-all-tags", "--set-tag=LYRICS=" + "lyrics " * 2000, "--set-tag=title=Flac Títle", "--set-tag=ARTIST=First Artist",
    "--set-tag=ARTIST=Second Artist", "--set-tag=Album=Flac Album", "--set-tag=TRACKNUMBER=4/12",
    "--import-picture-from=3|image/jpeg|cover|1600x1600x24|" + path("_cover.jpg"),
    path("_base.flac"))
blocks, frames = flac_blocks(open(path("_base.flac"), "rb").read())
order = {0: 0, 6: 1, 1: 2, 4: 3}                           # STREAMINFO, PICTURE, PADDING, VORBIS
blocks.sort(key=lambda b: order.get(b[0], 2))
assert [b[0] for b in blocks][:2] == [0, 6] and blocks[-1][0] == 4
write("vorbis_pic.flac", flac_join(blocks, frames))

# an ID3v2 in front of the FLAC (some taggers): it is read first
write("id3_flac.flac", id3(3, [frame(3, "TIT2", txt(0, "Id3 On Flac"))]) + flac_join(blocks, frames))

# ---- malformed ----------------------------------------------------------
write("bad_id3_size.mp3", b"ID3\4\0\0\x7f\x7f\x7f\x7f" + frame(4, "TIT2", txt(3, "Big")) + b"\xff\xfb")
write("bad_frame_size.mp3", id3(3, [b"TIT2\xff\xff\xff\xf0\0\0" + b"\0abc", frame(3, "TPE1", txt(0, "x"))]) + MP3)
write("bad_ape.mp3", MP3 + b"APETAGEX" + struct.pack("<IIII", 2000, 0xFFFFFFF0, 0xFFFFFFFF, 0) + bytes(8))
write("bad_vorbis.flac", b"fLaC" + bytes([0x84]) + (100).to_bytes(3, "big") +
      struct.pack("<I", 0xFFFFFFF0) + bytes(96))
write("tiny.mp3", b"ID3")
write("empty.mp3", b"")
write("bad_riff.wav", b"RIFF\xff\xff\xff\xffWAVELIST\xff\xff\xff\xffINFOINAM\xff\xff\xff\x7f")

# ---- MP4 (iTunes ilst) and Ogg (Vorbis comment header), as ffmpeg writes them --
run("ffmpeg", "-y", "-i", path("_base.wav"), "-c:a", "aac", "-b:a", "64k",          # moov after mdat
    "-metadata", "title=M4A Títle 東京", "-metadata", "artist=M4A Artist", "-metadata", "album=M4A Album",
    "-metadata", "track=3/12", path("aac_end.m4a"))
run("ffmpeg", "-y", "-i", path("_base.wav"), "-f", "lavfi", "-i", "color=c=red:s=64x64:d=0.1",
    "-map", "0:a", "-map", "1:v", "-frames:v", "1", "-c:v", "png", "-disposition:v", "attached_pic",
    "-c:a", "alac", "-movflags", "+faststart",                                      # moov first, a cover
    "-metadata", "title=Alac Title", "-metadata", "artist=Alac Artist", "-metadata", "track=7",
    path("alac_fast.m4a"))
run("ffmpeg", "-y", "-i", path("_base.wav"), "-ac", "2", "-c:a", "vorbis", "-strict", "-2",
    "-metadata", "title=Ogg Títle", "-metadata", "artist=Ogg Artist", "-metadata", "album=Ogg Album",
    "-metadata", "track=5", path("vorbis.ogg"))
run("ffmpeg", "-y", "-i", path("_base.wav"), "-c:a", "libopus",                      # the packet spans pages
    "-metadata", "LYRICS=" + "lyrics " * 20000, "-metadata", "title=Opus Títle",
    "-metadata", "artist=Opus Artist", "-metadata", "track=2", path("opus_long.opus"))
run("ffmpeg", "-y", "-i", path("_base.wav"), "-c:a", "libopus", "-map_metadata", "-1",
    "-fflags", "+bitexact", path("ogg_none.ogg"))
write("bad_mp4.m4a", struct.pack(">I", 0xFFFFFFF0) + b"moov" + bytes(64))
write("bad_ogg.ogg", b"OggS\0\2" + bytes(20) + b"\xff" + b"\xff" * 255 + b"\x03vorbis" + struct.pack("<I", 0xFFFFFF00))

for f in ("_base.wav", "_base.mp3", "_base.flac", "_cover.jpg"):
    os.remove(path(f))
