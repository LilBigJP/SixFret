#!/usr/bin/env python3
"""Draws the project's icon: six frets in two rows of three on a guitar neck (launcher/sixfret.png, .ico).

    python tools/make_icon.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SIZE = 1024  # drawn large and reduced: smooth edges at every icon size


def main():
    image = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((32, 32, SIZE - 32, SIZE - 32), radius=200, fill=(10, 14, 20, 255),
                           outline=(58, 160, 255, 255), width=28)
    # The neck: three columns, the strings across them.
    for column in range(4):
        x = 190 + column * 215
        draw.line((x, 170, x, SIZE - 170), fill=(40, 54, 74, 255), width=14)
    for row in range(2):
        for column in range(3):
            cx, cy = 297 + column * 215, 370 + row * 285
            black = row == 0
            draw.ellipse((cx - 82, cy - 82, cx + 82, cy + 82),
                         fill=(22, 28, 38, 255) if black else (236, 242, 250, 255),
                         outline=(58, 160, 255, 255) if black else (150, 170, 196, 255), width=16)
    image = image.resize((256, 256), Image.LANCZOS)
    image.save(ROOT / 'launcher' / 'sixfret.png')
    image.save(ROOT / 'launcher' / 'sixfret.ico', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print('icon written')


if __name__ == '__main__':
    main()
