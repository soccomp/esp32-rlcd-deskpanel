#!/usr/bin/env python3
"""
生成项目中文字体（lv_font_conv）。
用法: python3 gen_chinese_font.py [size]
默认生成 14px -> src/lv_font_chinese_14.c
字表来源: schedule.json(动态会议数据) + src 下全部 .cpp/.h(静态 UI 文案)
这样无论后端推送什么会议内容，字库都已覆盖，不会再出方块。

RLCD 最终只有黑白两级，因此使用 1bpp + 强提示黑体。
不要改回灰度抗锯齿字体；驱动二值化后会产生断笔和毛边。
"""
import os, json, subprocess, glob, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
SIZE = int(sys.argv[1]) if len(sys.argv) > 1 else 14
NO_CJK = len(sys.argv) > 2 and sys.argv[2] == "--no-cjk"
TTF_CANDIDATES = glob.glob(
    "/System/Library/AssetsV2/**/STHEITI.ttf", recursive=True
)
TTF = TTF_CANDIDATES[0] if TTF_CANDIDATES else "/Library/Fonts/Arial Unicode.ttf"
LVFC_CANDIDATES = [
    "/Users/m1work/.workbuddy/binaries/node/workspace/node_modules/lv_font_conv/lv_font_conv.js",
    "/tmp/lvfc/node_modules/lv_font_conv/lv_font_conv.js",
]
LVFC = next((path for path in LVFC_CANDIDATES if os.path.exists(path)), None)
if not LVFC:
    raise SystemExit("lv_font_conv not found")

# 0) 天气词汇兜底字表：weather 文案由后端动态下发（parse_schedule.py 的
#    WMO_CODE_MAP），这些字不一定出现在源码/日程数据里，必须显式并入。
WEATHER_CHARS = (
    "晴多云阴雾凇毛冻小中大雪霰阵暴雷雹雨天气获取内外室今明"
    "北京上海广州深圳杭州成都西安武汉南廊坊城市~°"
)

# 1) 收集字表
texts = [WEATHER_CHARS]
with open(os.path.join(ROOT, "schedule.json"), encoding="utf-8") as f:
    texts.append(f.read())
for ext in ("*.cpp", "*.h"):
    for p in glob.glob(os.path.join(ROOT, "src", ext)):
        with open(p, encoding="utf-8") as f:
            texts.append(f.read())

chars = set()
for t in texts:
    for ch in t:
        chars.add(ch)

# ASCII 可打印 + 全部非 ASCII（含 CJK 标点/全角符号）
syms = sorted({ch for ch in chars if 0x20 <= ord(ch) <= 0x7e or ord(ch) > 0x7e})
syms = "".join(syms)
print("symbol count:", len(syms))

# X 帖文是任意文本，光靠 schedule.json/src 字表必然缺字出方框。
# 追加 Unicode 范围兜底：
#   Latin-1 补充(重音字母/符号) + Latin Extended-A + 常用标点 + 全部常用汉字(GB2312 区)
FALLBACK_RANGES = [
    "0xA0-0xFF",        # Latin-1 Supplement: à é ü ç ¡ ¿ ° ± 等
    "0x100-0x17F",      # Latin Extended-A: ą ć ę ł ś ź 等
    "0x2000-0x206F",    # General Punctuation: – — … “ ” ‘ ’ • 等
    "0x4E00-0x9FA5",    # CJK 常用汉字（GB2312 全量 ≈6763 字）
]
if NO_CJK:
    # 18px 只渲染固定 UI 文案 + schedule.json 会议标题（已由符号字表覆盖），
    # 跳过全量汉字可省 ~1MB Flash。
    FALLBACK_RANGES = [r for r in FALLBACK_RANGES if not r.startswith("0x4E00")]

# 2) 调用 lv_font_conv 生成 .c
out = os.path.join(ROOT, "src", f"lv_font_chinese_{SIZE}.c")
cmd = ["node", LVFC,
       "--font", TTF,
       "--autohint-strong",
       "--lv-font-name", f"lv_font_chinese_{SIZE}",
       "--size", str(SIZE),
       "--bpp", "1",
       "--format", "lvgl",
       "--no-compress",
       "--symbols", syms]
for rng in FALLBACK_RANGES:
    cmd += ["--range", rng]
cmd += ["--output", out]
r = subprocess.run(cmd, capture_output=True, text=True)
print("rc", r.returncode)
if r.stdout: print("STDOUT:", r.stdout[-400:])
if r.stderr: print("STDERR:", r.stderr[-400:])
if r.returncode != 0:
    sys.exit(1)

# 3) 修正 include：本工程 LV_LVGL_H_INCLUDE_SIMPLE 未定义，必须是 "lvgl.h"
with open(out, encoding="utf-8") as f:
    src = f.read()
if '#include "lvgl/lvgl.h"' in src:
    src = src.replace('#include "lvgl/lvgl.h"', '#include "lvgl.h"')
    with open(out, "w", encoding="utf-8") as f:
        f.write(src)
    print("fixed include -> lvgl.h")

# 4) 生成头文件
hdr = os.path.join(ROOT, "src", f"lv_font_chinese_{SIZE}.h")
with open(hdr, "w", encoding="utf-8") as f:
    f.write(
        "#ifndef LV_FONT_CHINESE_%d_H\n"
        "#define LV_FONT_CHINESE_%d_H\n\n"
        "#ifdef __cplusplus\n"
        "extern \"C\" {\n"
        "#endif\n\n"
        "#include \"lvgl.h\"\n\n"
        "/* 单色屏中文字体：%dpx / 1bpp，基于 STHeiti */\n"
        "extern const lv_font_t lv_font_chinese_%d;\n\n"
        "#ifdef __cplusplus\n"
        "}\n"
        "#endif\n\n"
        "#endif /* LV_FONT_CHINESE_%d_H */\n"
        % (SIZE, SIZE, SIZE, SIZE, SIZE)
    )
print("generated:", out, "and", hdr)
