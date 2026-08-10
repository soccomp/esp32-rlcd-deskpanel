#!/usr/bin/env python3
"""生成 ESP32 RLCD 单色天气图标 (24x24, LV_IMG_CF_INDEXED_1BIT)
晴=带射线圆圈 / 多云=云朵 / 雨=云+下斜虚线 / 雪=云+雪花
"""
import math

W = H = 24

def blank():
    return [[0]*W for _ in range(H)]

def dist(x, y, cx, cy):
    return math.hypot(x-cx, y-cy)

def fill_circle(g, cx, cy, r):
    for y in range(H):
        for x in range(W):
            if dist(x+0.5, y+0.5, cx, cy) <= r:
                g[y][x] = 1

def ring(g, cx, cy, r_in, r_out):
    for y in range(H):
        for x in range(W):
            if r_in <= dist(x+0.5, y+0.5, cx, cy) <= r_out:
                g[y][x] = 1

def thick_line(g, x0, y0, x1, y1, t=1.0):
    """粗线段：到线段距离 <= t/2 的像素置 1"""
    for y in range(H):
        for x in range(W):
            px, py = x+0.5, y+0.5
            dx, dy = x1-x0, y1-y0
            L2 = dx*dx + dy*dy
            if L2 == 0:
                d = dist(px, py, x0, y0)
            else:
                s = max(0.0, min(1.0, ((px-x0)*dx + (py-y0)*dy) / L2))
                d = dist(px, py, x0 + s*dx, y0 + s*dy)
            if d <= t/2:
                g[y][x] = 1

def cloud_silhouette(g, oy=0, scale=1.0):
    """实心云朵剪影（单色下辨识度最高）：三圆 + 底部平底矩形"""
    s = scale
    fill_circle(g,  8.0*s, (13.0+oy)*s, 4.2*s)
    fill_circle(g, 13.5*s, (10.5+oy)*s, 5.2*s)
    fill_circle(g, 18.0*s, (13.5+oy)*s, 3.8*s)
    y0, y1 = int((12+oy)*s), int((17+oy)*s)
    for y in range(max(0, y0), min(H, y1+1)):
        for x in range(int(5*s), min(W, int(21*s))):
            g[y][x] = 1

# ---- 晴：圆圈(空心环) + 8 方向射线 ----
sun = blank()
ring(sun, 12, 12, 3.4, 5.4)
for i in range(8):
    a = math.pi/4 * i
    x0 = 12 + 7.0*math.cos(a); y0 = 12 + 7.0*math.sin(a)
    x1 = 12 + 10.3*math.cos(a); y1 = 12 + 10.3*math.sin(a)
    thick_line(sun, x0, y0, x1, y1, 1.8)

# ---- 多云：云朵剪影（居中） ----
cloud = blank()
cloud_silhouette(cloud, oy=-1.0)

# ---- 雨：云(上移) + 3 条下斜虚线 ----
rain = blank()
rain_c = blank()
cloud_silhouette(rain_c, oy=-4.5)
for y in range(H):
    for x in range(W):
        rain[y][x] = rain_c[y][x]
for bx in (7, 12, 17):
    # 每条雨线拆两段 → 虚线感
    thick_line(rain, bx,     15.5, bx-1.2, 18.0, 1.6)
    thick_line(rain, bx-2.0, 20.0, bx-3.2, 22.5, 1.6)

# ---- 雪：云(上移) + 雪花(米字星) ----
snow = blank()
snow_c = blank()
cloud_silhouette(snow_c, oy=-4.5)
for y in range(H):
    for x in range(W):
        snow[y][x] = snow_c[y][x]
cxs, cys, rr = 12, 19.5, 4.0
for i in range(3):
    a = math.pi/3 * i + math.pi/2
    x0 = cxs - rr*math.cos(a); y0 = cys - rr*math.sin(a)
    x1 = cxs + rr*math.cos(a); y1 = cys + rr*math.sin(a)
    thick_line(snow, x0, y0, x1, y1, 1.4)

# ---- 温度计：环形 bulb + 竖杆 + 顶部圆帽 + 两侧刻度 ----
thermo = blank()
ring(thermo, 12, 18.5, 3.0, 5.2)
thick_line(thermo, 12, 5.5, 12, 14.5, 2.4)
fill_circle(thermo, 12, 4.5, 2.0)
thick_line(thermo, 8.6, 8.0, 10.6, 8.0, 1.4)
thick_line(thermo, 13.4, 11.0, 15.4, 11.0, 1.4)

# ---- 水滴：底部圆 + 顶部尖角 ----
drop = blank()
fill_circle(drop, 12, 16.0, 5.6)
thick_line(drop, 12, 3.5, 7.2, 12.0, 2.6)
thick_line(drop, 12, 3.5, 16.8, 12.0, 2.6)

icons = {"sun": sun, "cloud": cloud, "rain": rain, "snow": snow,
         "thermo": thermo, "drop": drop}

# ---- 预览 ----
for name, g in icons.items():
    print(f"--- {name} ---")
    for row in g:
        print("".join("##" if v else ".." for v in row))
    print()

# ---- 输出 C 文件（LV_IMG_CF_INDEXED_1BIT，调色板: idx0=透明 idx1=黑） ----
def to_c_array(g):
    out = []
    for y in range(H):
        for bx in range(W // 8):
            b = 0
            for bit in range(8):
                if g[y][bx*8 + bit]:
                    b |= 0x80 >> bit
            out.append(b)
    return out

c_lines = [
    "/* 自动生成：单色天气图标 24x24 (LV_IMG_CF_INDEXED_1BIT)",
    " * 由 /tmp/gen_weather_icons.py 生成，勿手改。",
    " * 调色板: index0 = 透明, index1 = 黑色（浅底上直接可见）*/",
    '#include "lvgl.h"',
    "",
]
for name, g in icons.items():
    data = to_c_array(g)
    c_lines.append(f"static const uint8_t weather_{name}_map[] = {{")
    c_lines.append("    /* 调色板 (lv_color32_t: B,G,R,A) */")
    c_lines.append("    0x00, 0x00, 0x00, 0x00,   /* idx0 透明 */")
    c_lines.append("    0x00, 0x00, 0x00, 0xff,   /* idx1 黑   */")
    for y in range(H):
        row = data[y*3:(y+1)*3]
        c_lines.append("    " + ", ".join(f"0x{b:02x}" for b in row) + ",")
    c_lines.append("};")
    c_lines.append("")
    c_lines.append(f"const lv_img_dsc_t weather_icon_{name} = {{")
    c_lines.append("    .header = {")
    c_lines.append("        .cf = LV_IMG_CF_INDEXED_1BIT,")
    c_lines.append("        .always_zero = 0,")
    c_lines.append("        .reserved = 0,")
    c_lines.append(f"        .w = {W},")
    c_lines.append(f"        .h = {H},")
    c_lines.append("    },")
    c_lines.append(f"    .data_size = sizeof(weather_{name}_map),")
    c_lines.append(f"    .data = weather_{name}_map,")
    c_lines.append("};")
    c_lines.append("")

# ---- 追加 code -> 图标 映射函数（保持与手写版一致，避免重生成时被覆盖丢失） ----
c_lines.append('#include <string.h>')
c_lines.append('#include "weather_icons.h"')
c_lines.append('')
c_lines.append('/* 后端 weather code -> 图标（未知 code 一律回退云朵，绝不返回 NULL） */')
c_lines.append('const lv_img_dsc_t *weather_icon_by_code(const char *code)')
c_lines.append('{')
c_lines.append('    if (!code) return &weather_icon_cloud;')
c_lines.append('    if (strcmp(code, "sun")  == 0) return &weather_icon_sun;')
c_lines.append('    if (strcmp(code, "rain") == 0) return &weather_icon_rain;')
c_lines.append('    if (strcmp(code, "snow") == 0) return &weather_icon_snow;')
c_lines.append('    return &weather_icon_cloud;    /* cloud / 未知 */')
c_lines.append('}')
c_lines.append('')

with open("/Users/m1work/Projects/ESP32S3-RLCD/rlcd-lvgl/src/weather_icons.c", "w") as f:
    f.write("\n".join(c_lines))

hdr = """#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* 单色天气图标 24x24（LV_IMG_CF_INDEXED_1BIT，idx0 透明 / idx1 黑）
 * 晴=带射线圆圈  多云=云朵剪影  雨=云+下斜虚线  雪=云+雪花星
 * 室内指标：thermo=温度计  drop=水滴 */
extern const lv_img_dsc_t weather_icon_sun;
extern const lv_img_dsc_t weather_icon_cloud;
extern const lv_img_dsc_t weather_icon_rain;
extern const lv_img_dsc_t weather_icon_snow;
extern const lv_img_dsc_t weather_icon_thermo;
extern const lv_img_dsc_t weather_icon_drop;

/* 后端 weather code 字符串 -> 图标（未知 code 回退云朵） */
const lv_img_dsc_t *weather_icon_by_code(const char *code);

#ifdef __cplusplus
}
#endif
"""
with open("/Users/m1work/Projects/ESP32S3-RLCD/rlcd-lvgl/src/weather_icons.h", "w") as f:
    f.write(hdr)
print("written: src/weather_icons.c / .h")
