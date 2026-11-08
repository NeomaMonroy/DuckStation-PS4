#!/usr/bin/env python3
import os
import shutil
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

out = sys.argv[1]
duck_path = sys.argv[2] if len(sys.argv) > 2 else ""
here = os.path.dirname(os.path.abspath(__file__))
custom = os.path.join(here, "..", "assets", "art")

def font(size):
    for path in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                 "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                 "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"):
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()

def backdrop(w, h):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        r = int(14 + 26 * t)
        g = int(18 + 30 * t)
        b = int(40 + 70 * t)
        for x in range(w):
            px[x, y] = (r, g, b)
    glow = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(glow)
    d.ellipse([w * 0.25, h * 0.1, w * 0.75, h * 0.9], fill=90)
    glow = glow.filter(ImageFilter.GaussianBlur(min(w, h) * 0.12))
    return Image.composite(Image.new("RGB", (w, h), (70, 110, 200)), img, glow)

def duck(size):
    if duck_path and os.path.exists(duck_path):
        img = Image.open(duck_path).convert("RGBA")
        img.thumbnail((size, size), Image.LANCZOS)
        return img
    return None

def centered(draw, w, y, text, size, fill):
    f = font(size)
    box = draw.textbbox((0, 0), text, font=f)
    x = (w - (box[2] - box[0])) // 2
    draw.text((x + 4, y + 4), text, font=f, fill=(0, 0, 0))
    draw.text((x, y), text, font=f, fill=fill)

if os.path.exists(os.path.join(custom, "icon0.png")):
    shutil.copy(os.path.join(custom, "icon0.png"), os.path.join(out, "icon0.png"))
else:
    icon = backdrop(512, 512)
    d = duck(340)
    if d:
        icon.paste(d, ((512 - d.width) // 2, 40), d)
    centered(ImageDraw.Draw(icon), 512, 400, "DuckStation", 62, (255, 255, 255))
    icon.save(os.path.join(out, "icon0.png"))

if os.path.exists(os.path.join(custom, "pic1.png")):
    shutil.copy(os.path.join(custom, "pic1.png"), os.path.join(out, "pic1.png"))
else:
    pic = backdrop(1920, 1080)
    d = duck(520)
    if d:
        pic.paste(d, ((1920 - d.width) // 2, 170), d)
    centered(ImageDraw.Draw(pic), 1920, 740, "DuckStation", 130, (255, 255, 255))
    centered(ImageDraw.Draw(pic), 1920, 900, "PS1 emulator for PS4", 54, (200, 215, 255))
    pic.save(os.path.join(out, "pic1.png"))
