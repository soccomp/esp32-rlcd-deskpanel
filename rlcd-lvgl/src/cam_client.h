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

/* RLCD-004：取走一条待处理的页面切换命令（USB-CDC 通道收到的 "PAGE:xxx"）。
 * 返回 0=首页 1=吉他页 2=摄像头页，无待处理命令返回 -1；取走即清空。
 * 由 loop() 轮询调用，并在 Lvgl_lock 保护下执行 ui_goto_page()——
 * 解析发生在 cam_task，绝不在该任务内直接操作 LVGL 对象。 */
int8_t cam_client_take_page_cmd(void);

/* Phase 2 P1 —— AI 健康指示（M1 finger_page_control 心跳）。
 * 数据来源：M1 手势识别程序每帧处理完经 hub/WiFi 发 "AI:ALIVE"，
 * bridge 封装 CMD 帧或 WiFi TCP 文本行送入本机；此处记录最近心跳时刻。
 * 三态（与 CAM 一致，不显示假 FPS）：
 *   ai_client_is_fresh(): 心跳在 10s 内 -> true（进程活着且在处理）
 *   ai_client_has_beat(): 曾收到过至少一次心跳（stale 判定用）
 * 线程安全：volatile 时间戳，任意任务可读。 */
void     ai_heartbeat_now(void);          /* 收到 AI:ALIVE 时调用（cam_task/rx 解析内） */
bool     ai_client_is_fresh(void);        /* 心跳 10s 内 -> true */
bool     ai_client_has_beat(void);        /* 曾收到过心跳 */

/* RLCD-004.2（审核修正）：ACK 只在**实际页面状态已确认**后发送。
 * 语义 = "页面已真正切换/已在该页"，而非"命令已收到"。
 * 调用方（main.cpp）必须在 ui_goto_page 成功且 ui_get_current_page()==目标页
 * （或已在目标页）时才调用；Lvgl_lock 失败/切页未生效时不得调用。 */
void cam_client_send_ack(int8_t page);

/* 带锁 printf（8-12）：与 rx_task 的 Serial 读互斥，防 TinyUSB CDC 跨核并发崩溃。
 * 各任务（cam_task/main）输出日志统一走这里。 */
void cam_client_log(const char *fmt, ...);