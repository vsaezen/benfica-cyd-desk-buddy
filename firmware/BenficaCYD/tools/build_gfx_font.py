#!/usr/bin/env python3
"""Convert the OFL Graduate typeface into compact Adafruit GFX fonts."""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
FONT_FILE = ROOT / "assets" / "Graduate-Regular.ttf"
OUTPUT = ROOT / "BenficaFont.h"
FIRST = 0x20
# Latin-1 keeps the Portuguese accents supplied for the anthem while remaining
# compatible with TFT_eSPI's UTF-8 decoder and compact GFX font format.
LAST = 0xFF


def packed_bitmap(mask: Image.Image) -> list[int]:
    bits = []
    byte = 0
    used = 0
    pixels = mask.load()
    for y in range(mask.height):
        for x in range(mask.width):
            byte = (byte << 1) | (1 if pixels[x, y] >= 96 else 0)
            used += 1
            if used == 8:
                bits.append(byte)
                byte = 0
                used = 0
    if used:
        bits.append(byte << (8 - used))
    return bits


def build_font(pixel_size: int):
    font = ImageFont.truetype(str(FONT_FILE), pixel_size)
    ascent, descent = font.getmetrics()
    bitmap = []
    glyphs = []

    for codepoint in range(FIRST, LAST + 1):
        character = chr(codepoint)
        x0, y0, x1, y1 = font.getbbox(character, anchor="ls")
        width = max(0, x1 - x0)
        height = max(0, y1 - y0)
        advance = max(1, round(font.getlength(character)))
        offset = len(bitmap)

        if width and height:
            mask = Image.new("L", (width, height), 0)
            ImageDraw.Draw(mask).text(
                (-x0, -y0), character, font=font, fill=255, anchor="ls"
            )
            bitmap.extend(packed_bitmap(mask))
        else:
            width = 1
            height = 1
            bitmap.append(0)
            x0 = 0
            y0 = 0

        glyphs.append((offset, width, height, advance, x0, y0, character))

    return bitmap, glyphs, ascent + descent


def write_font(handle, pixel_size: int) -> None:
    name = f"Graduate{pixel_size}"
    bitmap, glyphs, advance = build_font(pixel_size)
    handle.write(f"static const uint8_t PROGMEM {name}Bitmaps[] = {{\n")
    for index in range(0, len(bitmap), 16):
        handle.write("  " + ", ".join(f"0x{value:02X}" for value in bitmap[index:index + 16]) + ",\n")
    handle.write("};\n\n")
    handle.write(f"static const GFXglyph PROGMEM {name}Glyphs[] = {{\n")
    for offset, width, height, x_advance, x_offset, y_offset, character in glyphs:
        label = character.replace("\\", "backslash").replace("'", "apostrophe")
        handle.write(
            f"  {{{offset:5d}, {width:2d}, {height:2d}, {x_advance:2d}, "
            f"{x_offset:3d}, {y_offset:3d}}}, // {label}\n"
        )
    handle.write("};\n\n")
    handle.write(
        f"static const GFXfont PROGMEM {name} = {{\n"
        f"  (uint8_t *){name}Bitmaps,\n"
        f"  (GFXglyph *){name}Glyphs,\n"
        f"  0x{FIRST:02X}, 0x{LAST:02X}, {advance}\n"
        "};\n\n"
    )


def main() -> None:
    with OUTPUT.open("w", encoding="utf-8") as handle:
        handle.write("#pragma once\n#include <Arduino.h>\n\n")
        for size in (14, 18, 26, 42):
            write_font(handle, size)
    print(f"Generated {OUTPUT}")


if __name__ == "__main__":
    main()
