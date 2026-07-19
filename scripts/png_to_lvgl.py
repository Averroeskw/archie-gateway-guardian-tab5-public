#!/usr/bin/env python3
"""Convert a PNG to an LVGL v9 ARGB8888 C array (in-memory byte order B,G,R,A)."""
import sys
from PIL import Image

def convert(src, dst, symbol, target_w, key_dark=False):
    im = Image.open(src).convert("RGBA")
    w, h = im.size
    new_h = round(h * target_w / w)
    im = im.resize((target_w, new_h), Image.LANCZOS)
    px = im.load()
    if key_dark:
        # Key out the near-black banner background to transparent; keep the
        # amber letters and their trace outlines.
        for y in range(new_h):
            for x in range(target_w):
                r, g, b, a = px[x, y]
                if max(r, g, b) < 40:
                    px[x, y] = (r, g, b, 0)
    data = bytearray()
    for y in range(new_h):
        for x in range(target_w):
            r, g, b, a = px[x, y]
            data += bytes((b, g, r, a))  # LVGL ARGB8888 memory order
    lines = []
    lines.append('#include "lvgl.h"\n')
    lines.append(f"// Auto-generated from the open-source Hermes-Agent asset (MIT).")
    lines.append(f"// Source resized to {target_w}x{new_h}, ARGB8888.\n")
    lines.append(f"static const uint8_t {symbol}_map[] = {{")
    row = []
    for i, byte in enumerate(data):
        row.append(f"0x{byte:02x},")
        if len(row) == 16:
            lines.append("    " + "".join(row))
            row = []
    if row:
        lines.append("    " + "".join(row))
    lines.append("};\n")
    lines.append(f"const lv_image_dsc_t {symbol} = {{")
    lines.append("    .header = {")
    lines.append("        .magic = LV_IMAGE_HEADER_MAGIC,")
    lines.append("        .cf = LV_COLOR_FORMAT_ARGB8888,")
    lines.append("        .flags = 0,")
    lines.append(f"        .w = {target_w},")
    lines.append(f"        .h = {new_h},")
    lines.append(f"        .stride = {target_w * 4},")
    lines.append("    },")
    lines.append(f"    .data_size = sizeof({symbol}_map),")
    lines.append(f"    .data = {symbol}_map,")
    lines.append("};")
    with open(dst, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{dst}: {target_w}x{new_h}  ({len(data)} bytes)")

convert("/tmp/hermes_banner.png", sys.argv[1], "hermes_wordmark", 420, key_dark=True)
convert("/tmp/hermes_icon.png", sys.argv[2], "hermes_mascot", 180)
