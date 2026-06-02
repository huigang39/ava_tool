#!/usr/bin/env python3
"""
Generate a multi-resolution Windows .ico from a source PNG.

Resizes the source image to several icon sizes and packs them into an .ico
container (PNG-compressed entries, supported by Windows Vista+).

Usage:
    python tools/make_icon.py
    python tools/make_icon.py --input assets/icon.png --output assets/icon.ico
"""

import argparse
import io
import struct
import sys

from PIL import Image

SIZES = [16, 24, 32, 48, 64, 128, 256]


def make_ico(input_path: str, output_path: str):
    src = Image.open(input_path).convert("RGBA")

    # Resize to each icon size and encode as PNG
    png_data_list: list[bytes] = []
    for s in SIZES:
        resized = src.resize((s, s), Image.LANCZOS)
        buf = io.BytesIO()
        resized.save(buf, format="PNG")
        png_data_list.append(buf.getvalue())

    count = len(SIZES)

    out = io.BytesIO()
    # ICONDIR header
    out.write(struct.pack("<HHH", 0, 1, count))

    # ICONDIRENTRY array
    offset = 6 + 16 * count
    for i, s in enumerate(SIZES):
        data = png_data_list[i]
        dim = 0 if s >= 256 else s  # 0 means 256 in ICO format
        out.write(struct.pack("<BBBBHHII",
                              dim,          # width
                              dim,          # height
                              0,            # palette count
                              0,            # reserved
                              1,            # color planes
                              32,           # bits per pixel
                              len(data),    # data size
                              offset))      # data offset
        offset += len(data)

    # Image data
    for data in png_data_list:
        out.write(data)

    with open(output_path, "wb") as f:
        f.write(out.getvalue())

    print(f"Wrote {output_path} ({count} sizes: {', '.join(str(s) for s in SIZES)})")


def main():
    parser = argparse.ArgumentParser(description="Generate .ico from .png")
    parser.add_argument("-i", "--input", default="assets/icon.png", help="Source PNG")
    parser.add_argument("-o", "--output", default="assets/icon.ico", help="Output ICO")
    args = parser.parse_args()

    try:
        make_ico(args.input, args.output)
    except ImportError:
        print("ERROR: Pillow is required. Install with: pip install Pillow", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
