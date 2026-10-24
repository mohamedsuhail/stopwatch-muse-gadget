#!/usr/bin/env python3
"""Packs the character's clips into the watch's animation partition.

    python3 avatar/character/make_anims.py [CLIPS_DIR] [OUT]

CLIPS_DIR (default avatar/character/clips) holds the .mp4 clips named in CLIPS
below (16:9, character centred on black; ANIMATION_PROMPTS.md says how they're
made). Each is cropped to its centre square, scaled to SIZE px at its own frame
rate, its near-black background made true black, sharpened a little, and scaled
so its first (neutral) frame's figure matches the reference clip's. One-shots
lose the still frames at their ends. OUT (default avatar/character/anims.pack)
is what scripts/flash.sh writes to the "anims" partition; it must fit in
PARTITION bytes. Needs ffmpeg and Pillow.

anims.pack, little-endian (version 2):
    "MANI", u16 version (2), u16 width, u16 height, u16 fps (the default),
    u16 clip count, u16 0
    clip count x (u16 first frame, u16 frame count, u16 flags (1 = plays once),
        u16 fps), in the order of CLIPS; clips may share frames
    u32 frame count, then that many (u32 offset, u32 size) from the file start
    the frames: baseline JPEG
Version 1 had 8 clips, no count, and one fps for all; the firmware reads both.
"""
import io, os, struct, subprocess, sys, tempfile
from PIL import Image, ImageChops, ImageFilter, ImageStat

HERE = os.path.dirname(os.path.abspath(__file__))
CLIPS_DIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "clips")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "anims.pack")
SIZE, FPS, QUALITY = 256, 24, 85   # 1.5x to the watch's 384 px; q85; speaking at 24 fps, joy/sad/thinking 15, the rest 8-12
PARTITION = 0x9D0000                # partitions_stopwatch.csv's "anims" (3 MB app slots)
SHARPEN = ImageFilter.UnsharpMask(radius=1.0, percent=80, threshold=2)   # offsets the watch's blow-up
REFERENCE = "listening.mp4"         # the neutral pose every clip starts and ends on

# In the firmware's order (muse_pixel.c's CLIP_*). Each: the pack's name, its file
# (clips with the same file and settings share frames), fps, plays once?, and how
# to trim: "loop" (drop the last frame when it repeats the first), "still" (cut
# the still frames at both ends), "loop-best" (old clips: the best-matching start
# and end), or a number of seconds to end at.
CLIPS = [
    ("wake",          "wake.mp4",          12, True,  None),
    ("idle",          "listening.mp4",     10, False, "loop"),        # calm: the listening loop
    ("listening",     "listening.mp4",     10, False, "loop"),
    ("thinking",      "thinking.mp4",      15, False, "loop"),
    ("speaking",      "speaking.mp4",      24, False, "loop"),
    ("happy",         "happy.mp4",         15, False, "still"),
    ("error",         "error.mp4",         10, False, "loop-best"),
    ("goodbye",       "goodbye.mp4",       12, True,  3.8),           # holds asleep, not the neutral end
    ("sad",           "sad.mp4",           15, False, "still"),
    ("nod",           "nod.mp4",           12, True,  "still"),
    ("checking",      "checking.mp4",      10, False, "loop"),
    ("idle_bubbles",  "idle_bubbles.mp4",  10, True,  "loop"),
    ("idle_juggling", "idle_juggling.mp4", 8, True,  "loop"),
]


def frames(file, fps):
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["ffmpeg", "-v", "error", "-i", os.path.join(CLIPS_DIR, file),
                        "-vf", f"fps={fps},crop=ih:ih:(iw-ih)/2:0,scale={SIZE}:{SIZE}:flags=lanczos",
                        os.path.join(tmp, "%03d.png")], check=True)
        out = []
        for f in sorted(os.listdir(tmp)):
            im = Image.open(os.path.join(tmp, f)).convert("RGB")
            mask = im.convert("L").point(lambda v: 255 if v > 22 else 0)   # noise -> true black
            clean = Image.new("RGB", im.size); clean.paste(im.filter(SHARPEN), (0, 0), mask)
            out.append(clean)
        return out


def figure(im):
    """The character's box: what isn't background."""
    return im.convert("L").point(lambda v: 255 if v > 30 else 0).getbbox()


def match_size(ims, ref):
    """Scales a clip so its first (neutral) frame's figure is as tall as the
    reference's, its feet where the reference's are: clips made apart come out
    a little closer or further, and switching between them showed the character
    grow and shrink."""
    b = figure(ims[0])
    f = (ref[3] - ref[1]) / (b[3] - b[1])
    if abs(f - 1) < 0.01 and abs(b[3] - ref[3]) < 2:
        return ims, 1.0
    out = []
    for im in ims:
        big = im.resize((round(SIZE * f), round(SIZE * f)), Image.LANCZOS)
        dx = round((ref[0] + ref[2]) / 2 - (b[0] + b[2]) / 2 * f)
        dy = round(ref[3] - b[3] * f)
        frame = Image.new("RGB", (SIZE, SIZE))
        frame.paste(big, (dx, dy))
        out.append(frame)
    return out, f


def diff(a, b):
    return sum(ImageStat.Stat(ImageChops.difference(a, b).convert("L")).mean)


def best_loop(ims, fps):
    best = None
    for s in range(0, fps):
        for e in range(s + 2 * fps, len(ims) + 1):
            d = diff(ims[s], ims[e - 1])
            if best is None or d < best[0]:
                best = (d, s, e)
    return best[1], best[2]


def trim(ims, how, fps):
    """The frames to keep, as (start, end)."""
    if how == "loop":
        return 0, len(ims) - 1 if diff(ims[0], ims[-1]) < 2 else len(ims)   # the last repeats the first
    if how == "loop-best":
        return best_loop(ims, fps)
    if how == "still":
        moving = [i for i in range(len(ims)) if diff(ims[i], ims[0]) > 1.5]
        if not moving:
            return 0, len(ims)
        return max(0, moving[0] - 1), min(len(ims), moving[-1] + 2)
    if isinstance(how, float):
        return 0, min(len(ims), round(how * fps))
    return 0, len(ims)


ref = figure(frames(REFERENCE, FPS)[0])
table, jpegs, shared = [], [], {}
for name, file, fps, once, how in CLIPS:
    key = (file, fps, how)
    if key in shared:
        first, count = shared[key]
        table.append((first, count, 1 if once else 0, fps))
        print(f"{name:14s} shares {file}'s frames")
        continue
    ims = frames(file, fps)
    if name != "wake":   # wake starts curled up: nothing neutral to match
        ims, f = match_size(ims, ref)
        if f != 1.0:
            print(f"{name:14s} scaled {f:.3f} to {REFERENCE}'s size")
    s, e = trim(ims, how, fps)
    shared[key] = (len(jpegs), e - s)
    table.append((len(jpegs), e - s, 1 if once else 0, fps))
    for im in ims[s:e]:
        buf = io.BytesIO()
        im.save(buf, "JPEG", quality=QUALITY, subsampling=2, progressive=False, optimize=False)
        jpegs.append(buf.getvalue())
    print(f"{name:14s} {e - s:3d} frames at {fps} fps ({(e - s) / fps:.1f} s){' once' if once else ' loop'}")

head = b"MANI" + struct.pack("<HHHHHH", 2, SIZE, SIZE, FPS, len(CLIPS), 0)
for first, count, flags, fps in table:
    head += struct.pack("<HHHH", first, count, flags, fps)
head += struct.pack("<I", len(jpegs))
offset = len(head) + 8 * len(jpegs)
index = b""
for j in jpegs:
    index += struct.pack("<II", offset, len(j))
    offset += len(j)
print(f"{len(jpegs)} frames, {offset / 1024 / 1024:.2f} MB of {PARTITION / 1024 / 1024:.2f} MB")
if offset > PARTITION:
    sys.exit(f"too big for the anims partition by {(offset - PARTITION) / 1024:.0f} KB: lower some fps or trim")
with open(OUT, "wb") as f:
    f.write(head + index + b"".join(jpegs))
print(f"-> {OUT}")
