/*
 * ui_camera.cpp —— 摄像头预览页实现
 *
 * 布局（页面区 400x280）：
 *   - 中间 320x240 预览图（LV_IMG_CF_ALPHA_1BIT，白底黑点）
 *   - 底部状态文字：CAM online / stale / offline
 *
 * 渲染管线（RLCD-003 Defect D 修复）：
 *   cam_client 只出 320x240 8bit 灰度 → 这里「先缩放、后 1-bit 抖动」：
 *   - 全页：灰度 320x240 直接 1-bit 转换（修正后的 Bayer 阈值，0..255 域）
 *   - 缩略图：灰度 box 平均缩放到 w×h → 1-bit 转换
 *   不再对已抖动的 1-bit 图做最近邻缩放。
 *
 * 刷新：lv_timer 每 500ms 取灰度帧，seq 变化才转换+invalidate；
 *       状态用 cam_client_is_fresh()（4s 无新帧 = stale/offline）。
 *       定时器运行在 LVGL 任务内，只碰 LVGL 对象，无锁竞争。
 */

#include "ui_camera.h"
#include "cam_client.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace {

constexpr lv_coord_t CAM_DISP_W = 320;
constexpr lv_coord_t CAM_DISP_H = 240;

/* ---- 1-bit 转换（Bayer 8x8，阈值映射到 0..255 域） ----
 * RLCD-003 Defect C 修复：旧代码 thr = bayer*4 - 128（-128..124）与
 * 0..255 亮度不在同一域，负阈值永不命中 → 半数点位永不黑（灰点矩阵）。
 * 修正：thr = bayer*4 + 2（0..63 → 2..254），与亮度同域直接比较。
 * 黑 = 亮度低于阈值；无硬编码 200% 对比度（先线性，实测再看）。 */
static const uint8_t BAYER8[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 },
};

static void bit_set(uint8_t *buf, int x, int y, int w)
{
    size_t idx = (size_t)y * w + x;
    buf[idx >> 3] |= (uint8_t)(0x80 >> (idx & 7));
}

/* 灰度(0..255) -> 1bit 黑点（Bayer 有序抖动，阈值同域） */
static void gray_to_1bit_bayer(const uint8_t *gray, int w, int h, uint8_t *dst)
{
    memset(dst, 0, ((size_t)w * h + 7) / 8);
    for (int y = 0; y < h; y++) {
        const uint8_t *bayer_row = BAYER8[y & 7];
        for (int x = 0; x < w; x++) {
            uint8_t lum = gray[(size_t)y * w + x];
            int thr = (int)bayer_row[x & 7] * 4 + 2;   /* 0..63 -> 2..254 */
            if (lum < thr) bit_set(dst, x, y, w);
        }
    }
}

/* 灰度 box 平均缩放：src(sw x sh) -> dst(dw x dh)。每目标像素取源区域均值，
 * 保留强度信息供后续抖动（先缩放后抖动）。 */
static void gray_downsample_box(const uint8_t *src, int sw, int sh,
                                uint8_t *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        int y0 = (y * sh) / dh;
        int y1 = ((y + 1) * sh) / dh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (x * sw) / dw;
            int x1 = ((x + 1) * sw) / dw;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t sum = 0, cnt = 0;
            for (int sy = y0; sy < y1; sy++) {
                const uint8_t *row = src + (size_t)sy * sw;
                for (int sx = x0; sx < x1; sx++) { sum += row[sx]; cnt++; }
            }
            dst[(size_t)y * dw + x] = (uint8_t)(sum / cnt);
        }
    }
}

lv_obj_t      *g_img    = nullptr;
lv_obj_t      *g_status = nullptr;
lv_img_dsc_t   g_img_dsc = {};
uint8_t       *g_gray    = nullptr;   /* PSRAM: CAM_GRAY_BYTES（从 cam_client 拷贝） */
uint8_t       *g_1bit    = nullptr;   /* PSRAM: CAM_BYTES（LVGL 显示缓冲） */
uint32_t       g_last_seq = 0;

void ui_camera_timer(lv_timer_t *timer)
{
    (void)timer;

    uint32_t seq;
    static uint32_t ui_diag = 0;
    if (cam_client_get_frame(g_gray, &seq)) {
        if (seq != g_last_seq) {
            g_last_seq = seq;
            gray_to_1bit_bayer(g_gray, CAM_DISP_W, CAM_DISP_H, g_1bit);
            if (g_img) {
                lv_obj_invalidate(g_img);
                lv_refr_now(NULL);   /* 实验：强制立即刷新，验证 invalidate->重绘 链路 */
                /* 诊断（限速）：UI 观察到新 seq 并 invalidate */
                if (++ui_diag >= 8) { ui_diag = 0;
                    Serial.printf("[cam-ui] new seq=%u invalidate+refr\n", (unsigned)seq);
                }
            }
        }
    }

    if (g_status) {
        const char *st = "CAM offline";
        if (cam_client_is_fresh())      st = "CAM online \xc2\xb7 2fps";
        else if (cam_client_has_frame()) st = "CAM stale \xc2\xb7 retrying";
        lv_label_set_text(g_status, st);
    }
}

/* ---- 首页缩略预览（X 卡片位置复用） ---- */
lv_obj_t      *g_th_img    = nullptr;
lv_obj_t      *g_th_status = nullptr;
lv_img_dsc_t   g_th_dsc = {};
uint8_t       *g_th_gray    = nullptr;  /* PSRAM: CAM_GRAY_BYTES（源灰度） */
uint8_t       *g_th_gray_s  = nullptr;  /* PSRAM: w*h 缩放灰度 */
uint8_t       *g_th_1bit    = nullptr;  /* PSRAM: w*h/8（LVGL 显示） */
lv_coord_t     g_th_w = 0, g_th_h = 0;
uint32_t       g_th_seq = 0;

void ui_camera_thumb_timer(lv_timer_t *timer)
{
    (void)timer;

    uint32_t seq;
    if (cam_client_get_frame(g_th_gray, &seq)) {
        if (seq != g_th_seq) {
            g_th_seq = seq;
            /* 先灰度缩放到缩略图最终尺寸，再做 1-bit 转换（先缩放后抖动） */
            gray_downsample_box(g_th_gray, CAM_DISP_W, CAM_DISP_H,
                                g_th_gray_s, g_th_w, g_th_h);
            gray_to_1bit_bayer(g_th_gray_s, g_th_w, g_th_h, g_th_1bit);
            if (g_th_img) lv_obj_invalidate(g_th_img);
        }
    }

    if (g_th_status) {
        const char *st = "CAM offline";
        if (cam_client_is_fresh())      st = "CAM online";
        else if (cam_client_has_frame()) st = "CAM stale";
        lv_label_set_text(g_th_status, st);
    }
}

} // namespace

void ui_camera_init(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);

    if (!g_gray) {
        g_gray = static_cast<uint8_t *>(
            heap_caps_malloc(CAM_GRAY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        g_1bit = static_cast<uint8_t *>(
            heap_caps_malloc(CAM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!g_gray || !g_1bit) {
        lv_obj_t *err = lv_label_create(parent);
        lv_label_set_text(err, "camera page alloc failed");
        lv_obj_set_pos(err, 150, 120);
        return;
    }
    memset(g_gray, 0, CAM_GRAY_BYTES);
    memset(g_1bit, 0, CAM_BYTES);

    /* LV_IMG_CF_ALPHA_1BIT: bit=1 -> 黑点（alpha=不透明，颜色固定黑），
     * bit=0 -> 透明（透出白底）。 */
    g_img_dsc.header.cf       = LV_IMG_CF_ALPHA_1BIT;
    g_img_dsc.header.w        = CAM_DISP_W;
    g_img_dsc.header.h        = CAM_DISP_H;
    g_img_dsc.data_size       = CAM_BYTES;
    g_img_dsc.data            = g_1bit;

    g_img = lv_img_create(parent);
    lv_img_set_src(g_img, &g_img_dsc);
    lv_obj_set_pos(g_img, (400 - CAM_DISP_W) / 2, (280 - CAM_DISP_H) / 2);

    g_status = lv_label_create(parent);
    lv_obj_set_style_text_font(g_status, &lv_font_montserrat_14, 0);
    lv_label_set_text(g_status, "CAM searching...");
    lv_obj_align(g_status, LV_ALIGN_BOTTOM_MID, 0, -6);

    lv_timer_create(ui_camera_timer, 500, nullptr);
}

/* 首页缩略预览：从 cam_client 取灰度，先 box 缩放再 1-bit 转换。
 * 需要三块 PSRAM：源灰度(76.8KB) + 缩放灰度(w*h) + 1bit(w*h/8)。 */
void ui_camera_thumb_init(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                          lv_coord_t w, lv_coord_t h)
{
    if (g_th_1bit) return;   /* 防重复初始化 */

    g_th_w = w;
    g_th_h = h;

    g_th_gray = static_cast<uint8_t *>(
        heap_caps_malloc(CAM_GRAY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_th_gray_s = static_cast<uint8_t *>(
        heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_th_1bit = static_cast<uint8_t *>(
        heap_caps_malloc(((size_t)w * h + 7) / 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!g_th_gray || !g_th_gray_s || !g_th_1bit) {
        Serial.println("[cam-thumb] PSRAM alloc failed, thumb disabled");
        return;
    }
    memset(g_th_gray, 0, CAM_GRAY_BYTES);
    memset(g_th_gray_s, 0, (size_t)w * h);
    memset(g_th_1bit, 0, ((size_t)w * h + 7) / 8);

    g_th_dsc.header.cf       = LV_IMG_CF_ALPHA_1BIT;
    g_th_dsc.header.w        = w;
    g_th_dsc.header.h        = h;
    g_th_dsc.data_size       = ((size_t)w * h + 7) / 8;
    g_th_dsc.data            = g_th_1bit;

    g_th_img = lv_img_create(parent);
    lv_img_set_src(g_th_img, &g_th_dsc);
    lv_obj_set_pos(g_th_img, x, y);

    g_th_status = lv_label_create(parent);
    lv_obj_set_style_text_font(g_th_status, &lv_font_montserrat_12, 0);
    lv_label_set_text(g_th_status, "CAM...");
    lv_obj_align(g_th_status, LV_ALIGN_BOTTOM_RIGHT, -6, -2);

    lv_timer_create(ui_camera_thumb_timer, 500, nullptr);
}
