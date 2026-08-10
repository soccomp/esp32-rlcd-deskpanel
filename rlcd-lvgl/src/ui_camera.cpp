/*
 * ui_camera.cpp —— 摄像头预览页实现
 *
 * 布局（页面区 400x280）：
 *   - 中间 320x240 预览图（LV_IMG_CF_ALPHA_1BIT，白底黑点）
 *   - 底部状态文字：CAM 在线 / 离线
 *
 * 刷新：lv_timer 每 500ms 调用 cam_client_get_frame() 取新帧，
 *       有新帧则拷入 PSRAM 缓冲并 invalidate 图片。定时器运行在
 *       LVGL 任务内，全程只碰 LVGL 对象，与 cam_client 的独立
 *       任务通过"取帧即拷贝"接口解耦，无锁竞争。
 */

#include "ui_camera.h"
#include "cam_client.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace {

constexpr lv_coord_t CAM_DISP_W = 320;
constexpr lv_coord_t CAM_DISP_H = 240;

lv_obj_t      *g_img    = nullptr;
lv_obj_t      *g_status = nullptr;
lv_img_dsc_t   g_img_dsc = {};
uint8_t       *g_buf    = nullptr;   /* PSRAM: CAM_BYTES */
uint32_t       g_last_seq = 0;
uint32_t       g_fail_cnt = 0;

void ui_camera_timer(lv_timer_t *timer)
{
    (void)timer;

    uint32_t seq;
    if (cam_client_get_frame(g_buf, &seq)) {
        if (seq != g_last_seq) {
            g_last_seq = seq;
            if (g_img) lv_obj_invalidate(g_img);
        }
        g_fail_cnt = 0;
    } else {
        if (g_fail_cnt < 0xFFFFFFFFUL) g_fail_cnt++;
    }

    if (g_status) {
        char s[40];
        if (cam_client_has_frame() && g_fail_cnt < 8) {
            snprintf(s, sizeof(s), "CAM online \xc2\xb7 2fps");
        } else {
            snprintf(s, sizeof(s), "CAM offline \xc2\xb7 retrying");
        }
        lv_label_set_text(g_status, s);
    }
}

/* ---- 首页缩略预览（X 卡片位置复用） ---- */
lv_obj_t      *g_th_img    = nullptr;
lv_obj_t      *g_th_status = nullptr;
lv_img_dsc_t   g_th_dsc = {};
uint8_t       *g_th_src    = nullptr;   /* PSRAM: CAM_BYTES 全帧（从 cam_client 取） */
uint8_t       *g_th_buf    = nullptr;   /* PSRAM: 缩放后 w*h/8（LVGL 显示） */
lv_coord_t     g_th_w = 0, g_th_h = 0;
uint32_t       g_th_seq = 0;
uint32_t       g_th_fail = 0;

/* 1bit 最近邻缩放：src(sw x sh, bit=1 黑) -> dst(dw x dh) */
static void scale_1bit(const uint8_t *src, int sw, int sh,
                       uint8_t *dst, int dw, int dh)
{
    memset(dst, 0, ((size_t)dw * dh + 7) / 8);
    for (int y = 0; y < dh; y++) {
        int sy = (y * sh) / dh;
        for (int x = 0; x < dw; x++) {
            int sx = (x * sw) / dw;
            size_t si = (size_t)sy * sw + sx;
            if (src[si >> 3] & (0x80 >> (si & 7))) {
                size_t di = (size_t)y * dw + x;
                dst[di >> 3] |= (0x80 >> (di & 7));
            }
        }
    }
}

void ui_camera_thumb_timer(lv_timer_t *timer)
{
    (void)timer;

    uint32_t seq;
    if (cam_client_get_frame(g_th_src, &seq)) {
        if (seq != g_th_seq) {
            g_th_seq = seq;
            scale_1bit(g_th_src, CAM_DISP_W, CAM_DISP_H,
                       g_th_buf, g_th_w, g_th_h);
            if (g_th_img) lv_obj_invalidate(g_th_img);
        }
        g_th_fail = 0;
    } else {
        if (g_th_fail < 0xFFFFFFFFUL) g_th_fail++;
    }

    if (g_th_status) {
        char s[28];
        if (cam_client_has_frame() && g_th_fail < 8) {
            snprintf(s, sizeof(s), "CAM online");
        } else {
            snprintf(s, sizeof(s), "CAM offline");
        }
        lv_label_set_text(g_th_status, s);
    }
}

} // namespace

void ui_camera_init(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);

    if (!g_buf) {
        g_buf = static_cast<uint8_t *>(
            heap_caps_malloc(CAM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!g_buf) {
        lv_obj_t *err = lv_label_create(parent);
        lv_label_set_text(err, "camera page alloc failed");
        lv_obj_set_pos(err, 150, 120);
        return;
    }
    memset(g_buf, 0, CAM_BYTES);

    /* LV_IMG_CF_ALPHA_1BIT: bit=1 -> 黑点（alpha=不透明，颜色固定黑），
     * bit=0 -> 透明（透出白底）。与 cam_client 的 1bit 格式完全同构。 */
    g_img_dsc.header.cf       = LV_IMG_CF_ALPHA_1BIT;
    g_img_dsc.header.w        = CAM_DISP_W;
    g_img_dsc.header.h        = CAM_DISP_H;
    g_img_dsc.data_size       = CAM_BYTES;
    g_img_dsc.data            = g_buf;

    g_img = lv_img_create(parent);
    lv_img_set_src(g_img, &g_img_dsc);
    lv_obj_set_pos(g_img, (400 - CAM_DISP_W) / 2, (280 - CAM_DISP_H) / 2);

    g_status = lv_label_create(parent);
    lv_obj_set_style_text_font(g_status, &lv_font_montserrat_14, 0);
    lv_label_set_text(g_status, "CAM searching...");
    lv_obj_align(g_status, LV_ALIGN_BOTTOM_MID, 0, -6);

    lv_timer_create(ui_camera_timer, 500, nullptr);
}

/* 首页缩略预览：复用 cam_client 的帧流，缩放到 w x h 显示。
 * 需要两块 PSRAM：源（全帧 9.6KB）+ 缩放缓冲（w*h/8）。 */
void ui_camera_thumb_init(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                          lv_coord_t w, lv_coord_t h)
{
    if (g_th_buf) return;   /* 防重复初始化 */

    g_th_w = w;
    g_th_h = h;

    g_th_src = static_cast<uint8_t *>(
        heap_caps_malloc(CAM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_th_buf = static_cast<uint8_t *>(
        heap_caps_malloc(((size_t)w * h + 7) / 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!g_th_src || !g_th_buf) {
        Serial.println("[cam-thumb] PSRAM alloc failed, thumb disabled");
        return;
    }
    memset(g_th_src, 0, CAM_BYTES);
    memset(g_th_buf, 0, ((size_t)w * h + 7) / 8);

    g_th_dsc.header.cf       = LV_IMG_CF_ALPHA_1BIT;
    g_th_dsc.header.w        = w;
    g_th_dsc.header.h        = h;
    g_th_dsc.data_size       = ((size_t)w * h + 7) / 8;
    g_th_dsc.data            = g_th_buf;

    g_th_img = lv_img_create(parent);
    lv_img_set_src(g_th_img, &g_th_dsc);
    lv_obj_set_pos(g_th_img, x, y);

    g_th_status = lv_label_create(parent);
    lv_obj_set_style_text_font(g_th_status, &lv_font_montserrat_12, 0);
    lv_label_set_text(g_th_status, "CAM...");
    lv_obj_align(g_th_status, LV_ALIGN_BOTTOM_RIGHT, -6, -2);

    lv_timer_create(ui_camera_thumb_timer, 500, nullptr);
}
