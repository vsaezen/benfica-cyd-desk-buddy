#!/usr/bin/env python3
"""Prepare compact RGB565 assets for the 320x240 Freenove display."""

from pathlib import Path

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
ASSETS.mkdir(exist_ok=True)

KEY_RGB = (255, 0, 255)
RED = (214, 0, 28)
RED_DARK = (94, 0, 13)
WHITE = (250, 250, 248)
BLACK = (8, 8, 11)
PANEL = (24, 22, 25)
MUTED = (206, 198, 200)


def cover(image: Image.Image, size: tuple[int, int], center=(0.5, 0.5)) -> Image.Image:
    """Resize and crop an image so it completely covers size."""
    image = image.convert("RGB")
    target_w, target_h = size
    scale = max(target_w / image.width, target_h / image.height)
    resized = image.resize(
        (round(image.width * scale), round(image.height * scale)), Image.Resampling.LANCZOS
    )
    left = round((resized.width - target_w) * center[0])
    top = round((resized.height - target_h) * center[1])
    left = max(0, min(left, resized.width - target_w))
    top = max(0, min(top, resized.height - target_h))
    return resized.crop((left, top, left + target_w, top + target_h))


def rounded_photo(
    image: Image.Image,
    size: tuple[int, int],
    radius: int,
    center=(0.5, 0.5),
) -> Image.Image:
    photo = cover(image, size, center)
    photo = ImageEnhance.Color(photo).enhance(1.10)
    photo = ImageEnhance.Contrast(photo).enhance(1.08)
    photo = photo.filter(ImageFilter.UnsharpMask(radius=1.0, percent=85, threshold=3))

    # A subtle Benfica-red cinematic grade keeps the photographs coherent with the UI.
    grade = Image.new("RGB", size, RED_DARK)
    photo = Image.blend(photo, grade, 0.08)
    rgba = photo.convert("RGBA")
    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size[0] - 1, size[1] - 1), radius, fill=255)
    rgba.putalpha(mask)
    return rgba


def prepare_eagle() -> Image.Image:
    source = Image.open(ROOT / "aguia_vitoria_source.jpg")
    # Crop around the open wings and face; the handler is deliberately kept mostly out.
    crop = source.crop((15, 5, 553, 470))
    result = rounded_photo(crop, (190, 132), 15, center=(0.50, 0.40))
    result.save(ASSETS / "aguia_vitoria_190x132.png")
    return result


def prepare_crest() -> Image.Image:
    source = Image.open(ROOT / "benfica_crest_source.png").convert("RGBA")
    bbox = source.getbbox()
    if bbox:
        source = source.crop(bbox)
    source.thumbnail((94, 94), Image.Resampling.LANCZOS)
    result = Image.new("RGBA", (96, 96), (0, 0, 0, 0))
    result.alpha_composite(source, ((96 - source.width) // 2, (96 - source.height) // 2))
    result.save(ASSETS / "benfica_crest_96.png")
    return result


def prepare_poppy() -> Image.Image:
    source = Image.open(ROOT / "papoila_source.jpg")
    # The source flower is in the left half; this crop preserves petals and black centre.
    crop = source.crop((115, 25, 1255, 870))
    result = rounded_photo(crop, (132, 96), 15, center=(0.43, 0.44))
    result.save(ASSETS / "papoila_real_132x96.png")
    return result


def prepare_eagle_flight() -> list[Image.Image]:
    """Split the generated 2x2 flight sheet into four compact RGBA frames."""
    source = Image.open(ASSETS / "eagle_flight_sheet_source.png").convert("RGBA")
    half_w = source.width // 2
    half_h = source.height // 2
    frames = []

    for index, (column, row) in enumerate(((0, 0), (1, 0), (0, 1), (1, 1))):
        quadrant = source.crop(
            (column * half_w, row * half_h, (column + 1) * half_w, (row + 1) * half_h)
        )
        alpha_box = quadrant.getchannel("A").getbbox()
        if alpha_box:
            quadrant = quadrant.crop(alpha_box)
        quadrant.thumbnail((108, 68), Image.Resampling.LANCZOS)
        frame = Image.new("RGBA", (112, 72), (0, 0, 0, 0))
        frame.alpha_composite(
            quadrant,
            ((frame.width - quadrant.width) // 2, (frame.height - quadrant.height) // 2),
        )
        frame.save(ASSETS / f"eagle_flight_{index}_112x72.png")
        frames.append(frame)

    return frames


def rgb565(red: int, green: int, blue: int) -> int:
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


def write_array(handle, name: str, image: Image.Image, matte=RED_DARK) -> None:
    rgba = image.convert("RGBA")
    values = []
    for red, green, blue, alpha in rgba.getdata():
        if alpha < 96:
            value = rgb565(*KEY_RGB)
        else:
            # Blend partially transparent antialiasing against the nearby dark-red UI.
            if alpha < 255:
                factor = alpha / 255.0
                red = round(red * factor + matte[0] * (1.0 - factor))
                green = round(green * factor + matte[1] * (1.0 - factor))
                blue = round(blue * factor + matte[2] * (1.0 - factor))
            value = rgb565(red, green, blue)
        values.append(value)

    handle.write(f"static constexpr uint16_t {name}_WIDTH = {rgba.width};\n")
    handle.write(f"static constexpr uint16_t {name}_HEIGHT = {rgba.height};\n")
    handle.write(f"static const uint16_t PROGMEM {name}[] = {{\n")
    for index in range(0, len(values), 16):
        row = ", ".join(f"0x{value:04X}" for value in values[index : index + 16])
        handle.write(f"  {row},\n")
    handle.write("};\n\n")


def font(size: int, bold: bool = False):
    candidates = [
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf" if bold else "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
    ]
    for candidate in candidates:
        try:
            return ImageFont.truetype(candidate, size)
        except OSError:
            pass
    return ImageFont.load_default()


def make_preview(eagle: Image.Image, crest: Image.Image) -> None:
    canvas = Image.new("RGB", (320, 240), BLACK)
    draw = ImageDraw.Draw(canvas)

    # Hero region: fixed imagery, white accent rail, strong red field.
    draw.rounded_rectangle((8, 7, 312, 148), 18, fill=RED_DARK, outline=(238, 238, 235), width=1)
    draw.rounded_rectangle((10, 9, 310, 146), 16, fill=RED)
    canvas.paste(eagle, (12, 12), eagle)
    draw.rectangle((202, 16, 205, 140), fill=WHITE)
    draw.ellipse((209, 22, 307, 120), fill=(255, 255, 255))
    canvas.paste(crest, (210, 23), crest)
    draw.text((258, 135), "1904", fill=WHITE, font=font(12, True), anchor="mm")

    button_specs = [
        (9, 160, 101, 226, "SLB", "GLORIOSO", False),
        (110, 160, 210, 226, "GOLO", "TESTE", True),
        (219, 160, 311, 226, "SOM", "PAPOILAS", False),
    ]
    for left, top, right, bottom, title, subtitle, primary in button_specs:
        fill = RED if primary else PANEL
        outline = WHITE if primary else (96, 88, 91)
        draw.rounded_rectangle((left, top, right, bottom), 13, fill=(0, 0, 0))
        draw.rounded_rectangle((left, top - 2, right, bottom - 2), 13, fill=fill, outline=outline, width=1)
        draw.text(((left + right) // 2, top + 22), title, fill=WHITE, font=font(17, True), anchor="mm")
        draw.text(((left + right) // 2, top + 44), subtitle, fill=MUTED, font=font(10, True), anchor="mm")

    # A small white/red/white lower stripe replaces debug/status text.
    draw.rectangle((10, 233, 310, 234), fill=WHITE)
    draw.rectangle((10, 235, 310, 237), fill=RED)
    canvas.save(ASSETS / "ui_home_preview.png")


def main() -> None:
    eagle = prepare_eagle()
    crest = prepare_crest()
    poppy = prepare_poppy()
    flight_frames = prepare_eagle_flight()

    header = ROOT / "BenficaAssets.h"
    with header.open("w", encoding="utf-8") as handle:
        handle.write("#pragma once\n#include <Arduino.h>\n\n")
        handle.write("static constexpr uint16_t BENFICA_IMAGE_KEY = 0xF81F;\n\n")
        write_array(handle, "BENFICA_CREST", crest, matte=WHITE)
        write_array(handle, "AGUIA_VITORIA", eagle)
        write_array(handle, "PAPOILA_REAL", poppy)
        for index, frame in enumerate(flight_frames):
            write_array(handle, f"EAGLE_FLIGHT_{index}", frame)

    make_preview(eagle, crest)
    print(f"Generated {header}")


if __name__ == "__main__":
    main()
