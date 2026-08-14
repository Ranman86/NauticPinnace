#!/usr/bin/env python3
"""
logo_convert.py – Convert any image to logo.bin for the NauticPinnace boot screen.

Usage:
    python logo_convert.py <input_image> <output.bin> [--size N]

Examples:
    python logo_convert.py logo.png  logo.bin             # default 160x160
    python logo_convert.py logo.jpg  logo.bin --size 200  # max 200x200
    python logo_convert.py logo.png  logo.bin --size 120  # smaller, faster boot

Output format (raw RGB565, little-endian):
    Offset 0: uint16 width
    Offset 2: uint16 height
    Offset 4: width * height * 2 bytes of RGB565 pixel data

Install dependency:
    pip install Pillow
"""

import sys
import struct
import argparse
from pathlib import Path

def convert(src: Path, dst: Path, size: int) -> None:
    try:
        from PIL import Image
    except ImportError:
        sys.exit("Pillow not found – run:  pip install Pillow")

    img = Image.open(src).convert("RGBA")

    # Composite onto black background (handles transparent PNGs)
    bg = Image.new("RGB", img.size, (0, 0, 0))
    bg.paste(img, mask=img.split()[3])
    img = bg

    # Resize with high-quality Lanczos filter, preserving aspect ratio
    img.thumbnail((size, size), Image.LANCZOS)
    w, h = img.size

    print(f"  Input : {src}  ({img.size[0]}×{img.size[1]} before resize)")
    print(f"  Output: {dst}  ({w}×{h} px, {w*h*2 + 4} bytes)")

    with open(dst, "wb") as f:
        f.write(struct.pack("<HH", w, h))          # 4-byte header
        for y in range(h):
            for x in range(w):
                r, g, b = img.getpixel((x, y))
                # Pack to RGB565: RRRRRGGGGGGBBBBB
                rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                f.write(struct.pack("<H", rgb565))

    print(f"  Done. Upload logo.bin via web UI → Boot-Logo tab.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Convert image to NauticPinnace logo.bin")
    parser.add_argument("input",  help="Input image (PNG, JPG, BMP, …)")
    parser.add_argument("output", help="Output .bin file (default: logo.bin)",
                        nargs="?", default="logo.bin")
    parser.add_argument("--size", type=int, default=160,
                        help="Max width/height in pixels (default: 160, max: 200)")
    args = parser.parse_args()

    if args.size > 200:
        sys.exit("Error: maximum size is 200 px (display limit)")

    convert(Path(args.input), Path(args.output), args.size)
