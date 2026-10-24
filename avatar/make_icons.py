#!/usr/bin/env python3
"""Makes the small "who's answering" logos drawn by the watch's battery ring.

    python3 avatar/make_icons.py [--size PX] [--out DIR]

From avatar/icons/muse-source.png (Muse's blue mark), grok-source.png (xAI's
Grok mark), both on white, and gemini-source.png (Gemini's star, already
transparent), writes for each NAME:
  DIR/NAME.rgb565a8   for the firmware: u16 width, u16 height, then LVGL's
                      RGB565A8 (the RGB565 plane, then the alpha plane)
  avatar/icons/NAME.png   the same icon, transparent, for previews and the
                      live-voice simulator
White becomes transparent; Muse keeps its colours, and Grok's black mark is
drawn white, for the black screen. Needs Pillow.
"""
import argparse, os, struct
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ICONS = os.path.join(HERE, "icons")
ap = argparse.ArgumentParser(description="the responder logos")
ap.add_argument("--size", type=int, default=20)   # inside the 32 px waiting spinner
ap.add_argument("--out", default=ICONS)
args = ap.parse_args()


def unwhite(im, recolour=None):
    """Alpha from how far each pixel is from white; colours un-blended from the white."""
    out = Image.new("RGBA", im.size)
    px = []
    for r, g, b in im.convert("RGB").getdata():
        a = 255 - min(r, g, b)
        if a == 0:
            px.append((0, 0, 0, 0))
        elif recolour:
            px.append(recolour + (a,))
        else:
            f = 255 / a   # the colour that, a/255 over white, gives this pixel
            px.append(tuple(max(0, min(255, round(255 - (255 - c) * f))) for c in (r, g, b)) + (a,))
    out.putdata(px)
    return out


def square(im, size):
    """Crops to the mark, centres it on a square with a little margin, and scales it down."""
    box = im.split()[3].point(lambda a: 255 if a > 8 else 0).getbbox()
    im = im.crop(box)
    side = int(max(im.size) * 1.08)
    sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    sq.alpha_composite(im, ((side - im.width) // 2, (side - im.height) // 2))
    return sq.resize((size, size), Image.LANCZOS)


def write(name, im):
    im.save(os.path.join(ICONS, name + ".png"))
    rgb, alpha = bytearray(), bytearray()
    for r, g, b, a in im.getdata():
        rgb += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
        alpha.append(a)
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, name + ".rgb565a8"), "wb") as f:
        f.write(struct.pack("<HH", im.width, im.height) + rgb + alpha)
    print(f"{name}: {im.width}x{im.height}")


write("muse", square(unwhite(Image.open(os.path.join(ICONS, "muse-source.png"))), args.size))
write("gemini", square(Image.open(os.path.join(ICONS, "gemini-source.png")).convert("RGBA"), args.size))   # already transparent
write("grok", square(unwhite(Image.open(os.path.join(ICONS, "grok-source.png")), recolour=(255, 255, 255)), args.size))
