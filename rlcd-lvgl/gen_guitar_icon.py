#!/usr/bin/env python3
"""生成 Fender Stratocaster 单色轮廓点阵图 (LV_IMG_CF_INDEXED_1BIT)

竖向摆放（琴头在上、琴体在下），尺寸 104x240，适配 400x300 反射屏左半区。
内容：6 连排琴头 + 弦钮 / 品丝 + 指板点 / 双切角琴体 / 护板 / 三单拾音器 /
      琴桥摇座 + 弦马 / 三旋钮 + 五档档位开关 / 输出插孔。
注意：**不画琴弦**——琴弦由 ui_ambient.cpp 用 lv_line 覆盖绘制，以便做振动动画。

输出: src/guitar_icon.c / src/guitar_icon.h
用法: python3 gen_guitar_icon.py [--preview]
勿手改生成的 .c，改图形请改本脚本后重跑。
"""
import math
import os
import sys

W, H = 104, 240
CX = 52.0

# ---------------- 基础光栅 ----------------
g = [[0] * W for _ in range(H)]


def px(x, y, v=1):
    xi, yi = int(x), int(y)
    if 0 <= xi < W and 0 <= yi < H:
        g[yi][xi] = v


def seg_dist(pxx, pyy, x0, y0, x1, y1):
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    if L2 == 0:
        return math.hypot(pxx - x0, pyy - y0)
    s = max(0.0, min(1.0, ((pxx - x0) * dx + (pyy - y0) * dy) / L2))
    return math.hypot(pxx - (x0 + s * dx), pyy - (y0 + s * dy))


def line(x0, y0, x1, y1, t=1.6):
    """粗线段（只扫描包围盒，避免整屏遍历）"""
    r = t / 2 + 1
    xa, xb = int(min(x0, x1) - r), int(max(x0, x1) + r) + 1
    ya, yb = int(min(y0, y1) - r), int(max(y0, y1) + r) + 1
    for y in range(max(0, ya), min(H, yb)):
        for x in range(max(0, xa), min(W, xb)):
            if seg_dist(x + 0.5, y + 0.5, x0, y0, x1, y1) <= t / 2:
                g[y][x] = 1


def polyline(pts, t=1.6, closed=False):
    n = len(pts)
    last = n if closed else n - 1
    for i in range(last):
        a, b = pts[i], pts[(i + 1) % n]
        line(a[0], a[1], b[0], b[1], t)


def disc(cx, cy, r):
    for y in range(max(0, int(cy - r - 1)), min(H, int(cy + r + 2))):
        for x in range(max(0, int(cx - r - 1)), min(W, int(cx + r + 2))):
            if math.hypot(x + 0.5 - cx, y + 0.5 - cy) <= r:
                g[y][x] = 1


def ring(cx, cy, r, t=1.6):
    for y in range(max(0, int(cy - r - 2)), min(H, int(cy + r + 3))):
        for x in range(max(0, int(cx - r - 2)), min(W, int(cx + r + 3))):
            if abs(math.hypot(x + 0.5 - cx, y + 0.5 - cy) - r) <= t / 2:
                g[y][x] = 1


def fill_poly(pts):
    """扫描线填充（用于护板/拾音器等实心块）"""
    ys = [p[1] for p in pts]
    for y in range(max(0, int(min(ys))), min(H, int(max(ys)) + 1)):
        yc = y + 0.5
        xs = []
        n = len(pts)
        for i in range(n):
            x0, y0 = pts[i]
            x1, y1 = pts[(i + 1) % n]
            if (y0 <= yc < y1) or (y1 <= yc < y0):
                xs.append(x0 + (yc - y0) * (x1 - x0) / (y1 - y0))
        xs.sort()
        for k in range(0, len(xs) - 1, 2):
            for x in range(max(0, int(xs[k])), min(W, int(xs[k + 1]) + 1)):
                g[y][x] = 1


def catmull(pts, closed=True, steps=14):
    """Catmull-Rom 采样成密集折线，让琴体轮廓平滑"""
    out = []
    n = len(pts)
    rng = range(n) if closed else range(n - 1)
    for i in rng:
        p0 = pts[(i - 1) % n] if closed else pts[max(i - 1, 0)]
        p1 = pts[i]
        p2 = pts[(i + 1) % n] if closed else pts[min(i + 1, n - 1)]
        p3 = pts[(i + 2) % n] if closed else pts[min(i + 2, n - 1)]
        for s in range(steps):
            t = s / steps
            t2, t3 = t * t, t * t * t
            x = 0.5 * ((2 * p1[0]) + (-p0[0] + p2[0]) * t +
                       (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2 +
                       (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3)
            y = 0.5 * ((2 * p1[1]) + (-p0[1] + p2[1]) * t +
                       (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2 +
                       (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3)
            out.append((x, y))
    return out


# ============================================================
#  1) 琴头（Fender 6 连排，向左侧张开的经典"逗号"形）
# ============================================================
NUT_Y = 44.0
HEAD = [
    (60.5, NUT_Y),          # 右下（接琴颈右侧）
    (62.5, 34.0),
    (62.0, 22.0),
    (59.0, 11.0),
    (53.0, 4.0),            # 顶部圆角
    (44.0, 3.0),
    (34.0, 7.0),            # 左上外张
    (26.5, 16.0),
    (24.0, 27.0),
    (27.5, 36.5),
    (35.0, 42.5),
    (43.5, NUT_Y),          # 左下（接琴颈左侧）
]
polyline(catmull(HEAD, closed=True, steps=12), t=2.0, closed=True)

# 弦钮：沿琴头左/上缘 6 连排（实心钮 + 指向中轴的短弦柱）
TUNERS = [(36.0, 38.0), (31.0, 30.0), (30.0, 21.5), (33.0, 14.0),
          (39.0, 9.0), (46.5, 6.5)]
for (tx, ty) in TUNERS:
    disc(tx, ty, 2.4)

# ============================================================
#  2) 琴颈 / 指板（上窄下宽）+ 品丝 + 指板点
# ============================================================
JOINT_Y = 134.0          # 琴颈与琴体交接
NUT_HALF, JOINT_HALF = 8.5, 11.5


def neck_half(y):
    k = (y - NUT_Y) / (JOINT_Y - NUT_Y)
    return NUT_HALF + (JOINT_HALF - NUT_HALF) * k


line(CX - NUT_HALF, NUT_Y, CX - JOINT_HALF, JOINT_Y, 2.0)
line(CX + NUT_HALF, NUT_Y, CX + JOINT_HALF, JOINT_Y, 2.0)
# 上弦枕
line(CX - NUT_HALF - 1, NUT_Y, CX + NUT_HALF + 1, NUT_Y, 3.0)

SCALE_PX = 168.0         # 等效弦长（px），用 2^(-n/12) 定品位
fret_ys = []
for n in range(1, 20):
    d = SCALE_PX * (1 - 2 ** (-n / 12.0))
    y = NUT_Y + d
    if y > JOINT_Y - 4:
        break
    fret_ys.append(y)
    hw = neck_half(y) - 1.0
    line(CX - hw, y, CX + hw, y, 1.0)   # 品丝细线，单色屏下避免"梯子"噪点

# 指板点：3/5/7/9 单点，12 双点（位于相邻品之间）
def mid_of_fret(n):
    """第 n 品与第 n-1 品之间的中点 y（fret_ys[k] 对应第 k+1 品）"""
    if n > len(fret_ys):
        return None
    a = fret_ys[n - 2] if n >= 2 else NUT_Y
    b = fret_ys[n - 1]
    return (a + b) / 2.0


for n in (3, 5, 7, 9):
    y = mid_of_fret(n)
    if y:
        disc(CX, y, 2.0)
y12 = mid_of_fret(12)
if y12:
    disc(CX - 4.5, y12, 1.8)
    disc(CX + 4.5, y12, 1.8)

# ============================================================
#  3) 琴体（双切角 Strat 轮廓）
# ============================================================
BODY = [
    (13.0, 131.0),   # 低音侧长号角尖
    (4.0, 145.0),
    (2.5, 163.0),
    (6.0, 186.0),
    (16.0, 213.0),
    (32.0, 231.0),
    (52.0, 236.0),   # 底部
    (72.0, 230.0),
    (88.0, 214.0),
    (97.0, 194.0),
    (100.0, 174.0),
    (98.0, 155.0),
    (90.0, 140.0),
    (85.0, 133.0),   # 高音侧短号角尖
    (76.0, 145.0),   # 高音侧切角内凹
    (69.0, 141.0),
    (CX + JOINT_HALF, JOINT_Y),      # 琴颈根部右
    (CX - JOINT_HALF, JOINT_Y),      # 琴颈根部左
    (34.0, 143.0),   # 低音侧切角内凹
    (23.0, 148.0),
]
polyline(catmull(BODY, closed=True, steps=16), t=2.2, closed=True)

# ============================================================
#  4) 护板（Strat 招牌大护板：包住三拾音器 + 三旋钮 + 档位开关）
#     单色屏上与琴体轮廓形成双线，注意留够间距避免糊成一团
# ============================================================
#     ⚠ 上缘/两侧必须让开琴体切角内凹曲线，否则两条线并成一条粗黑带
PG = [
    (34.0, 148.0),   # 左上：让开左切角内凹 (34,143)-(23,148)
    (22.0, 157.0),
    (16.0, 171.0),
    (17.0, 188.0),
    (26.0, 204.0),
    (40.0, 215.0),
    (54.0, 219.0),
    (68.0, 214.0),
    (79.0, 205.0),
    (86.0, 193.0),
    (86.0, 179.0),
    (79.0, 168.0),
    (80.0, 157.0),   # 靠近颈拾音器的小台阶
    (72.0, 149.0),   # 右上：让开右切角内凹 (76,145)-(69,141)
    (58.0, 145.0),
    (46.0, 145.0),
]
polyline(catmull(PG, closed=True, steps=14), t=1.6, closed=True)

# ============================================================
#  5) 三只单线圈拾音器（桥拾音器倾斜，Strat 标志性）
# ============================================================
def pickup(cy, halfw, halfh, slant=0.0):
    """实心拾音器块，slant = 右端相对左端的 y 偏移（正值为右端下沉）"""
    x0, x1 = CX - halfw, CX + halfw
    y0, y1 = cy - slant / 2, cy + slant / 2
    fill_poly([(x0, y0 - halfh), (x1, y1 - halfh),
               (x1, y1 + halfh), (x0, y0 + halfh)])


pickup(160.0, 10.5, 3.0, 0.0)     # 颈拾音器
pickup(178.0, 10.5, 3.0, 0.0)     # 中拾音器
pickup(197.0, 11.0, 3.2, 6.0)     # 桥拾音器（倾斜）

# ============================================================
#  6) 琴桥 / 摇座（空心框 + 6 个弦马，避免大黑块糊掉）
# ============================================================
BR_Y = 212.0
polyline([(CX - 12, BR_Y - 4), (CX + 12, BR_Y - 4),
          (CX + 12, BR_Y + 3), (CX - 12, BR_Y + 3)], t=1.6, closed=True)
for i in range(6):
    disc(CX - 10 + i * 4.0, BR_Y - 0.5, 1.3)

# ============================================================
#  7) 旋钮（1 音量 + 2 音色，沿护板右下对角排布）
#     半径与位置须与护板右缘留 ≥4px 间隙，否则单色下糊在一起
# ============================================================
for (kx, ky, kr) in ((69.0, 180.0, 4.0), (75.0, 191.0, 3.8), (80.0, 202.0, 3.8)):
    ring(kx, ky, kr, 1.6)
    disc(kx, ky, 1.2)

# 注：五档开关与输出插孔（各仅 ~4px）在此尺寸下只会变成噪点，已刻意省略。

# ============================================================
#  预览 / 导出
# ============================================================
if "--preview" in sys.argv:
    for y in range(H):
        print("".join("#" if g[y][x] else "." for x in range(W)))
    sys.exit(0)

ROOT = os.path.dirname(os.path.abspath(__file__))
stride = (W + 7) // 8
rows = []
for y in range(H):
    row = bytearray(stride)
    for x in range(W):
        if g[y][x]:
            row[x >> 3] |= 0x80 >> (x & 7)
    rows.append(row)

lines = []
lines.append("/* 自动生成：Fender Stratocaster 单色轮廓图 %dx%d" % (W, H))
lines.append(" * (LV_IMG_CF_INDEXED_1BIT，idx0 透明 / idx1 黑)")
lines.append(" * 由 gen_guitar_icon.py 生成，勿手改；改图形请改脚本后重跑。")
lines.append(" * 琴弦不在图中——由 ui_ambient.cpp 的 lv_line 覆盖绘制以支持振动动画。*/")
lines.append('#include "lvgl.h"')
lines.append("")
lines.append("static const uint8_t guitar_strat_map[] = {")
lines.append("    /* 调色板 (lv_color32_t: B,G,R,A) */")
lines.append("    0x00, 0x00, 0x00, 0x00,   /* idx0 透明 */")
lines.append("    0x00, 0x00, 0x00, 0xff,   /* idx1 黑   */")
for row in rows:
    lines.append("    " + " ".join("0x%02x," % b for b in row))
lines.append("};")
lines.append("")
lines.append("const lv_img_dsc_t guitar_strat = {")
lines.append("    .header = {")
lines.append("        .cf = LV_IMG_CF_INDEXED_1BIT,")
lines.append("        .always_zero = 0,")
lines.append("        .reserved = 0,")
lines.append("        .w = %d," % W)
lines.append("        .h = %d," % H)
lines.append("    },")
lines.append("    .data_size = sizeof(guitar_strat_map),")
lines.append("    .data = guitar_strat_map,")
lines.append("};")
lines.append("")

with open(os.path.join(ROOT, "src", "guitar_icon.c"), "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

hdr = """#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* Fender Stratocaster 单色轮廓图 %dx%d（INDEXED_1BIT，idx0 透明 / idx1 黑）
 * 竖向：琴头在上、琴体在下。琴弦另由 lv_line 覆盖绘制（可振动）。
 * 关键几何（图内局部坐标，供 ui_ambient.cpp 对齐琴弦用）： */
#define GUITAR_IMG_W        %d
#define GUITAR_IMG_H        %d
#define GUITAR_NUT_Y        %d    /* 上弦枕 y */
#define GUITAR_NUT_HALF     %d    /* 上弦枕半宽 */
#define GUITAR_BRIDGE_Y     %d    /* 琴桥弦马 y */
#define GUITAR_BRIDGE_HALF  %d    /* 琴桥弦距半宽 */
#define GUITAR_CX           %d    /* 中轴 x */

extern const lv_img_dsc_t guitar_strat;

#ifdef __cplusplus
}
#endif
""" % (W, H, W, H, int(NUT_Y), 8, int(BR_Y + 7), 11, int(CX))

with open(os.path.join(ROOT, "src", "guitar_icon.h"), "w", encoding="utf-8") as f:
    f.write(hdr)

on = sum(sum(r) for r in g)
print("generated src/guitar_icon.c  %dx%d  黑像素 %d (%.1f%%)  data=%d B"
      % (W, H, on, on * 100.0 / (W * H), 8 + stride * H))
