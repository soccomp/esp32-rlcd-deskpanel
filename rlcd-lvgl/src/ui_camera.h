/*
 * ui_camera.h —— 摄像头预览页（第 4 页，全屏 320x240 预览）
 *
 * 渲染管线（RLCD-003）：cam_client 解码出 320x240 8bit 灰度 →
 * 此处「先缩放（缩略图 box 平均）、后 1-bit Bayer 转换（阈值同域）」，
 * 输出 LV_IMG_CF_ALPHA_1BIT（bit=1 黑）直接贴 lv_img。
 */

#pragma once

#include "lvgl.h"

void ui_camera_init(lv_obj_t *parent);

/* 缩略预览（首页 X 卡片位置复用）：在 parent 上 (x,y) 处创建 w*h 的小预览窗。
 * 灰度 box 平均缩放到 w×h 后再做 1-bit 转换（先缩放后抖动）。 */
void ui_camera_thumb_init(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                          lv_coord_t w, lv_coord_t h);
