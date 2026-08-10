"""Generate TRUE_COLOR (RGB565) image - no palette, proven format."""

from PIL import Image

SRC = "/Users/m1work/Projects/ESP32S3-RLCD/generated-images/fender_reference.jpg"
OUT_H = "/Users/m1work/Projects/ESP32S3-RLCD/rlcd-lvgl/src/fender_img.h"
OUT_C = "/Users/m1work/Projects/ESP32S3-RLCD/rlcd-lvgl/src/fender_img.c"
W, H = 160, 240

img = Image.open(SRC).convert("L")
img = img.crop((0, 0, 480, img.height))
img = img.resize((W, H), Image.LANCZOS)
img_bw = img.point(lambda p: 0 if p < 128 else 255, mode='1')

# TRUE_COLOR: each pixel = 2 bytes RGB565 (little-endian)
# White (0xFFFF) = {0xFF, 0xFF}, Black (0x0000) = {0x00, 0x00}
data = bytearray()
for y in range(H):
    for x in range(W):
        black = (img_bw.getpixel((x, y)) == 0)
        if black:
            data.extend([0x00, 0x00])  # black RGB565
        else:
            data.extend([0xFF, 0xFF])  # white RGB565
print(f"TRUE_COLOR data: {len(data)} bytes ({W}x{H}x2)")

with open(OUT_H, "w") as f:
    f.write("#pragma once\n#include \"lvgl.h\"\n\nextern const lv_img_dsc_t fender_strat_map;\n")

with open(OUT_C, "w") as f:
    f.write('#include "fender_img.h"\n\n')
    f.write(f"/* Fender Stratocaster {W}x{H} TRUE_COLOR RGB565 ({len(data)} bytes) */\n\n")
    f.write("static const uint8_t fender_data_arr[] = {\n")
    for i in range(0, len(data), 16):
        f.write("    " + ", ".join(f"0x{b:02x}" for b in data[i:i+16]) + ",\n")
    f.write("};\n\n")
    f.write(f"""const lv_img_dsc_t fender_strat_map = {{
    .header = {{
        .cf = LV_IMG_CF_TRUE_COLOR,
        .w = {W},
        .h = {H},
    }},
    .data_size = sizeof(fender_data_arr),
    .data = fender_data_arr,
}};
""")
print("Done")
