#!/usr/bin/env python3
"""Makes the MOBOT tape label drawn in the top-left of the watch's screen.

    python3 avatar/make_tape.py FONTS_DIR

FONTS_DIR holds PermanentMarker.ttf (Apache 2.0, Google Fonts). Writes
avatar/tape.png (a preview) and avatar/tape.rgb565a8, the image the firmware
draws at its offset on the 466 px round screen. Needs Pillow.
"""
import os, struct, sys
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
FONTS = sys.argv[1]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tape.rgb565a8")
os.environ["FONTS"] = FONTS
from tape_art import worn_tape

R = 466
CX, CY, ANGLE = 96, 102, 40          # tape centre on screen and its tilt

tape = worn_tape(420, 64, "MOBOT", 38, 7, NOTE="A7·22·10·K")
rot = tape.rotate(ANGLE, resample=Image.BICUBIC, expand=True)
screen = Image.new("RGBA", (R, R), (0, 0, 0, 0))
shadow = Image.new("RGBA", rot.size, (0, 0, 0, 0))
shadow.putalpha(rot.split()[3].point(lambda a: int(a * 0.5)))
screen.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(4)), (CX - rot.width // 2 + 3, CY - rot.height // 2 + 5))
screen.alpha_composite(rot, (CX - rot.width // 2, CY - rot.height // 2))
circle = Image.new("L", (R, R), 0)
ImageDraw.Draw(circle).ellipse([0, 0, R - 1, R - 1], fill=255)
screen.putalpha(Image.composite(screen.split()[3], Image.new("L", (R, R), 0), circle))
x0, y0, x1, y1 = screen.split()[3].point(lambda a: 255 if a > 2 else 0).getbbox()
img = screen.crop((x0, y0, x1, y1))
img.save(os.path.join(os.path.dirname(os.path.abspath(__file__)), "tape.png"))

w, h = img.size
rgb, alpha = bytearray(), bytearray()
for r, g, b, a in img.get_flattened_data():
    rgb += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    alpha.append(a)
# tape.rgb565a8: u16 width, u16 height, i16 x, i16 y on the 466 px screen, then
# LVGL's RGB565A8 (the RGB565 plane, then the alpha plane). build.sh embeds it.
with open(OUT, "wb") as f:
    f.write(struct.pack("<HHhh", w, h, x0, y0) + rgb + alpha)
print(f"tape: {w}x{h} at ({x0},{y0}), {(8 + len(rgb) + len(alpha)) // 1024} KB")
