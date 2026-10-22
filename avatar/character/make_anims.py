#!/usr/bin/env python3
"""Packs the character's state clips into the watch's animation partition.

    python3 avatar/character/make_anims.py [CLIPS_DIR] [OUT]

CLIPS_DIR (default avatar/character/clips) holds wake, idle, listening,
thinking, speaking, happy, error and goodbye .mp4 (16:9, character centred on
black). Each is cropped to its centre square, scaled to SIZE px at FPS, its
near-black background made true black, and the loop states trimmed to their
best-matching start and end frames. OUT (default avatar/character/anims.pack)
is what scripts/flash.sh writes to the "anims" partition. Needs ffmpeg and
Pillow.

anims.pack, little-endian:
    "MANI", u16 version (1), u16 width, u16 height, u16 fps
    8 x (u16 first frame, u16 frame count, u16 flags (1 = plays once), u16 0),
        in the order of STATES
    u32 frame count, then that many (u32 offset, u32 size) from the file start
    the frames: baseline JPEG
"""
import io, os, struct, subprocess, sys, tempfile
from PIL import Image, ImageChops, ImageStat

HERE = os.path.dirname(os.path.abspath(__file__))
CLIPS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "clips")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "anims.pack")
STATES = ["wake", "idle", "listening", "thinking", "speaking", "happy", "error", "goodbye"]
ONCE = {"wake", "goodbye"}
SIZE, FPS, QUALITY = 192, 12, 85

def frames(name):
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["ffmpeg", "-v", "error", "-i", os.path.join(CLIPS, name + ".mp4"),
                        "-vf", f"fps={FPS},crop=ih:ih:(iw-ih)/2:0,scale={SIZE}:{SIZE}:flags=lanczos",
                        os.path.join(tmp, "%03d.png")], check=True)
        out = []
        for f in sorted(os.listdir(tmp)):
            im = Image.open(os.path.join(tmp, f)).convert("RGB")
            mask = im.convert("L").point(lambda v: 255 if v > 22 else 0)   # noise -> true black
            clean = Image.new("RGB", im.size); clean.paste(im, (0, 0), mask)
            out.append(clean)
        return out

def diff(a, b):
    return sum(ImageStat.Stat(ImageChops.difference(a, b).convert("L")).mean)

def best_loop(ims):
    best = None
    for s in range(0, FPS):
        for e in range(s + 2 * FPS, len(ims) + 1):
            d = diff(ims[s], ims[e - 1])
            if best is None or d < best[0]:
                best = (d, s, e)
    return best[1], best[2]

table, jpegs = [], []
for name in STATES:
    ims = frames(name)
    s, e = (0, len(ims)) if name in ONCE else best_loop(ims)
    table.append((len(jpegs), e - s, 1 if name in ONCE else 0))
    for im in ims[s:e]:
        buf = io.BytesIO()
        im.save(buf, "JPEG", quality=QUALITY, subsampling=2, progressive=False, optimize=False)
        jpegs.append(buf.getvalue())
    print(f"{name:10s} {e - s:3d} frames{' (once)' if name in ONCE else f' loop {s}..{e - 1}'}")

head = b"MANI" + struct.pack("<HHHH", 1, SIZE, SIZE, FPS)
for first, count, flags in table:
    head += struct.pack("<HHHH", first, count, flags, 0)
head += struct.pack("<I", len(jpegs))
offset = len(head) + 8 * len(jpegs)
index = b""
for j in jpegs:
    index += struct.pack("<II", offset, len(j))
    offset += len(j)
with open(OUT, "wb") as f:
    f.write(head + index + b"".join(jpegs))
print(f"{len(jpegs)} frames, {offset / 1024 / 1024:.2f} MB -> {OUT}")
