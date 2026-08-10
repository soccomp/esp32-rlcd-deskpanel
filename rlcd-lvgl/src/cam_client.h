/*
 * cam_client.h —— RLCD 侧摄像头客户端
 *
 * 职责：通过 mDNS 发现 ESP32-CAM（esp32cam.local），定时拉取 /capture
 *       JPEG 帧，解码成 320x240 单色(1bit/px)缓冲，供 LVGL 侧贴屏。
 *
 * 线程模型（与项目现有约束一致）：
 *   - cam_client 是独立 FreeRTOS 任务，全程不碰 LVGL 对象。
 *   - 解码结果写入 PSRAM 双缓冲，LVGL 侧通过 cam_client_get_frame()
 *     拷贝到自己的 PSRAM 缓冲（调用方提供，至少 CAM_BYTES 字节）。
 *   - 灰度阈值与 main.cpp 的 Lvgl_FlushCallback 完全一致：<0x7fff 为黑。
 *
 * 依赖：bodmer/TJpg_Decoder@1.1.0（JPEG 解码）
 *
 * ⚠️ 接口设计：bitmap_out 必须指向 ≥CAM_BYTES 字节的 PSRAM/全局缓冲。
 *   不要传栈上的大数组，否则会撑爆 LVGL 任务栈（实测：cam_frame_t
 *   结构含 9600B bitmap，LVGL 任务默认栈 8KB 直接 overflow）。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define CAM_W       320
#define CAM_H       240
#define CAM_BYTES   ((CAM_W * CAM_H) / 8)   /* 1bit/px */

/* 创建 cam_fetch 任务（setup() 中 Wi-Fi 连接后调用一次） */
void cam_client_init(void);

/* 取当前帧解码缓冲到 bitmap_out（≥CAM_BYTES 字节）；
 * 返回 true 表示有有效帧。无新帧返回 false。线程安全。 */
bool cam_client_get_frame(uint8_t *bitmap_out, uint32_t *seq_out);

/* 是否已成功拉到过至少一帧 */
bool cam_client_has_frame(void);