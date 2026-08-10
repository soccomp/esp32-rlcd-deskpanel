"""Convert the supplied Shenzhou Media logo into a monochrome LVGL image."""

from pathlib import Path

from PIL import Image, ImageEnhance, ImageFilter, ImageOps


ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(
    "/Users/m1work/Downloads/"
    "u=1958481956,382570546&fm=217&app=126&size=re3,2&q=75&n=0&g=4n&f=JPEG&fmt=auto&maxorilen2heic=2000000.webp"
)
OUT_H = ROOT / "src" / "brand_logo.h"
OUT_C = ROOT / "src" / "brand_logo.c"
PREVIEW = ROOT / "brand_logo_preview.png"
WIDTH, HEIGHT = 160, 48


def build_logo() -> Image.Image:
    source = Image.open(SOURCE).convert("RGB")
    gray = ImageOps.grayscale(source)

    # Remove the large empty margin in the downloaded thumbnail, retaining a
    # small white safety edge so the lettering never touches its LVGL bounds.
    content_mask = gray.point(lambda pixel: 255 if pixel < 230 else 0)
    bounds = content_mask.getbbox()
    if bounds is None:
        raise RuntimeError("No visible logo content found")
    left, top, right, bottom = bounds
    cropped = gray.crop(
        (max(0, left - 3), max(0, top - 3), min(gray.width, right + 3), min(gray.height, bottom + 3))
    )

    cropped.thumbnail((WIDTH - 4, HEIGHT - 4), Image.Resampling.LANCZOS)
    canvas = Image.new("L", (WIDTH, HEIGHT), 255)
    canvas.paste(cropped, ((WIDTH - cropped.width) // 2, (HEIGHT - cropped.height) // 2))

    # Blue artwork becomes solid black on the 1-bit reflective display.
    canvas = ImageEnhance.Contrast(canvas).enhance(1.7)
    canvas = canvas.filter(ImageFilter.UnsharpMask(radius=0.8, percent=160, threshold=3))
    return canvas.point(lambda pixel: 0 if pixel < 205 else 255, mode="1")


def write_lvgl_image(image: Image.Image) -> None:
    data = bytearray()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if image.getpixel((x, y)) == 0:
                data.extend((0x00, 0x00))
            else:
                data.extend((0xFF, 0xFF))

    OUT_H.write_text(
        '#pragma once\n#include "lvgl.h"\n\nextern const lv_img_dsc_t shenzhou_media_logo;\n',
        encoding="utf-8",
    )

    lines = [
        '#include "brand_logo.h"',
        "",
        f"/* Shenzhou Media logo {WIDTH}x{HEIGHT}, monochrome RGB565. */",
        "static const uint8_t shenzhou_media_logo_data[] = {",
    ]
    for offset in range(0, len(data), 16):
        chunk = ", ".join(f"0x{byte:02x}" for byte in data[offset : offset + 16])
        lines.append(f"    {chunk},")
    lines.extend(
        [
            "};",
            "",
            "const lv_img_dsc_t shenzhou_media_logo = {",
            "    .header = {",
            "        .cf = LV_IMG_CF_TRUE_COLOR,",
            f"        .w = {WIDTH},",
            f"        .h = {HEIGHT},",
            "    },",
            "    .data_size = sizeof(shenzhou_media_logo_data),",
            "    .data = shenzhou_media_logo_data,",
            "};",
            "",
        ]
    )
    OUT_C.write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    logo = build_logo()
    logo.convert("L").save(PREVIEW)
    write_lvgl_image(logo)
    print(f"generated {OUT_C.relative_to(ROOT)} and {PREVIEW.relative_to(ROOT)}")
