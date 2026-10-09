#!/usr/bin/env python3
"""Generates meta/icon.png (48x48), meta/banner.png (256x128) and meta/banner.wav.

Run from the repo root: /usr/bin/python3 tools/make_art.py
Drawn at 8x and downsampled for anti-aliasing.
"""
import math
import struct
import wave

from PIL import Image, ImageDraw, ImageFilter, ImageFont

BG_TOP = (28, 33, 44)
BG_BOT = (12, 14, 19)
AMBER = (247, 168, 55)
CORAL = (240, 98, 92)
CREAM = (250, 242, 228)


def gradient(w, h, top, bot):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return img


def diag_gradient(w, h, a, b):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            t = (x / w + y / h) / 2
            px[x, y] = tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))
    return img


def draw_mark(size):
    """The ABS3DS mark: an open book whose pages turn into sound waves, on a warm disc."""
    s = size
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    disc = diag_gradient(s, s, AMBER, CORAL).convert("RGBA")
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, s - 1, s - 1), fill=255)
    img.paste(disc, (0, 0), mask)
    d = ImageDraw.Draw(img)
    cx, cy = s / 2, s * 0.56
    pw, ph = s * 0.26, s * 0.30
    lw = max(2, int(s * 0.045))
    # two pages
    for side in (-1, 1):
        x0 = cx
        x1 = cx + side * pw
        top_in, top_out = cy - ph * 0.55, cy - ph * 0.62
        bot_in, bot_out = cy + ph * 0.45, cy + ph * 0.38
        poly = [(x0, top_in), (x1, top_out), (x1, bot_out), (x0, bot_in)]
        d.polygon(poly, fill=CREAM)
    d.line((cx, cy - ph * 0.55, cx, cy + ph * 0.45), fill=(200, 120, 60), width=lw // 2 or 1)
    # sound waves rising from the book
    for i, r in enumerate((0.16, 0.26, 0.36)):
        rr = s * r
        box = (cx - rr, cy - ph * 0.55 - rr, cx + rr, cy - ph * 0.55 + rr)
        d.arc(box, 235, 305, fill=CREAM + (255 - i * 60,), width=lw)
    return img


def make_icon():
    S = 48 * 8
    bg = gradient(S, S, BG_TOP, BG_BOT).convert("RGBA")
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, S - 1, S - 1), radius=S * 0.22, fill=255)
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(bg, (0, 0), mask)
    mark = draw_mark(int(S * 0.78))
    out.alpha_composite(mark, (int(S * 0.11), int(S * 0.11)))
    out.resize((48, 48), Image.LANCZOS).save("meta/icon.png")


def make_banner():
    W, H = 256 * 4, 128 * 4
    bg = gradient(W, H, BG_TOP, BG_BOT).convert("RGBA")
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((W * 0.02, H * 0.05, W * 0.48, H * 0.95), fill=AMBER + (90,))
    glow = glow.filter(ImageFilter.GaussianBlur(60))
    bg.alpha_composite(glow)
    mark = draw_mark(int(H * 0.62))
    bg.alpha_composite(mark, (int(W * 0.08), int(H * 0.19)))
    d = ImageDraw.Draw(bg)
    big = ImageFont.truetype("assets/fonts/Manrope-700.ttf", int(H * 0.27))
    small = ImageFont.truetype("assets/fonts/Manrope-500.ttf", int(H * 0.10))
    tx = int(W * 0.44)
    d.text((tx, int(H * 0.22)), "ABS3DS", font=big, fill=CREAM)
    d.text((tx + 4, int(H * 0.56)), "Audiobookshelf player", font=small, fill=(190, 190, 200))
    d.rounded_rectangle((tx + 4, int(H * 0.74), tx + 4 + int(W * 0.18), int(H * 0.755)), radius=4, fill=AMBER)
    bg.resize((256, 128), Image.LANCZOS).save("meta/banner.png")


def make_wav():
    """A short soft two-note chime (C6 -> G6), 16-bit mono 32728 Hz, under 3 s as the HOME Menu expects."""
    rate = 32728
    n = int(rate * 1.6)
    frames = bytearray()
    for i in range(n):
        t = i / rate
        v = 0.0
        for start, f in ((0.0, 1046.5), (0.18, 1568.0)):
            if t >= start:
                tt = t - start
                v += math.sin(2 * math.pi * f * tt) * math.exp(-tt * 3.2) * 0.32
                v += math.sin(2 * math.pi * f * 2 * tt) * math.exp(-tt * 6) * 0.06
        frames += struct.pack("<h", int(max(-1, min(1, v)) * 32767))
    with wave.open("meta/banner.wav", "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))


if __name__ == "__main__":
    make_icon()
    make_banner()
    make_wav()
    print("wrote meta/icon.png meta/banner.png meta/banner.wav")
