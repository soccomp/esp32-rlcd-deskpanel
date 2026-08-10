/*
 * ui_camera.h —— 摄像头预览页（第 4 页，全屏 320x240 预览）
 *
 * 显示机制：cam_client 解码出的 1bit 缓冲（bit=1 黑）与 LVGL 的
 * LV_IMG_CF_ALPHA_1BIT 同构，零转换直接作为图片贴到 lv_img 上，
 * 每帧仅拷贝 9.6KB（PSRAM），2fps 无压力。
 */

#pragma once

#include "lvgl.h"

void ui_camera_init(lv_obj_t *parent);

/* 缩略预览（首页 X 卡片位置复用）：在 parent 上 (x,y) 处创建 w*h 的小预览窗。
 * 内部 1bit 最近邻缩放（320x240 -> 预览尺寸），不依赖 LVGL zoom（ALPHA_1BIT
 * transform 渲染有风险）。 */
void ui_camera_thumb_init(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                          lv_coord_t w, lv_coord_t h);
