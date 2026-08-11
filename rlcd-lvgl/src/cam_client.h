/*
 * cam_client.h —— RLCD 侧摄像头客户端
 *
 * 职责：通过 mDNS 发现 Mac 后端，定时拉取 /api/camframe JPEG 帧，
 *       解码成 320x240 8bit 灰度中间帧（RLCD-003：抖动必须最后做，
 *       解码只出强度数据），供 LVGL 侧做最终 1-bit 转换 + 缩放。
 *
 * 线程模型（与项目现有约束一致）：
 *   - cam_client 是独立 FreeRTOS 任务，全程不碰 LVGL 对象。
 *   - 解码灰度写入 PSRAM 双缓冲，LVGL 侧通过 cam_client_get_frame()
 *     拷贝到自己的 PSRAM 缓冲（调用方提供，至少 CAM_GRAY_BYTES 字节）。
 *   - 新鲜度用 seq + 时间戳：cam_client_is_fresh() 报告最近发布是否
 *     仍在阈值窗口内（陈旧帧可继续显示，但状态必须如实报告 stale）。
 *
 * 依赖：bodmer/TJpg_Decoder@1.1.0（JPEG 解码）
 *
 * ⚠️ 接口设计：gray_out 必须指向 ≥CAM_GRAY_BYTES 字节的 PSRAM/全局缓冲。
 *   不要传栈上的大数组，否则会撑爆 LVGL 任务栈。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define CAM_W       320
#define CAM_H       240
#define CAM_GRAY_BYTES (CAM_W * CAM_H)      /* 8bit 灰度中间帧 */
#define CAM_BYTES      (CAM_W * CAM_H / 8)  /* 1bit 显示缓冲（全页） */

/* 创建 cam_fetch 任务（setup() 中 Wi-Fi 连接后调用一次） */
void cam_client_init(void);

/* 取当前灰度帧到 gray_out（≥CAM_GRAY_BYTES 字节）；
 * 有有效帧返回 true 并带出最新 seq；无有效帧返回 false。线程安全。
 * 不消费"新帧"标记：调用方各自记录上次 seq 判断是否有新帧。 */
bool cam_client_get_frame(uint8_t *gray_out, uint32_t *seq_out);

/* 是否已成功拉到过至少一帧 */
bool cam_client_has_frame(void);

/* 新鲜度：最近一次成功解码发布距今 < FRESH_MS 且曾成功过。
 * 返回 false = 帧已陈旧/离线（画面可保留，但状态必须显示 stale/offline）。 */
bool cam_client_is_fresh(void);