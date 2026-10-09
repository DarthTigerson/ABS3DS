#!/usr/bin/env python3
"""Generates assets/qr-fbi.png: a QR code FBI can scan (Remote Install) to
download the latest shelf3ds.cia release. Run: /usr/bin/python3 tools/make_qr.py"""
import qrcode
from PIL import Image, ImageDraw, ImageFont

URL = "https://github.com/DarthTigerson/Shelf3DS/releases/latest/download/shelf3ds.cia"

qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_M, box_size=8, border=3)
qr.add_data(URL)
qr.make(fit=True)
code = qr.make_image(fill_color=(14, 17, 22), back_color=(255, 255, 255)).convert("RGB")
w, h = code.size
out = Image.new("RGB", (w, h + 56), (255, 255, 255))
out.paste(code, (0, 0))
d = ImageDraw.Draw(out)
f = ImageFont.truetype("assets/fonts/Manrope-700.ttf", 22)
s = ImageFont.truetype("assets/fonts/Manrope-500.ttf", 14)
d.text((w // 2, h + 6), "Shelf3DS", font=f, fill=(14, 17, 22), anchor="mt")
d.text((w // 2, h + 34), "FBI → Remote Install → Scan QR Code", font=s, fill=(92, 99, 112), anchor="mt")
out.save("assets/qr-fbi.png")
print("wrote assets/qr-fbi.png for", URL)
