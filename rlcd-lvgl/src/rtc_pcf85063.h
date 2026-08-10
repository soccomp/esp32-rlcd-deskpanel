#pragma once

#include <Wire.h>
#include <time.h>

/* ============================================================
 *  PCF85063A 实时时钟驱动（板载 I²C 0x51）
 *  - 复用 SHTC3 同一条 Arduino Wire 总线（SDA=13 / SCL=14）
 *  - 时间寄存器从 0x04 起共 7 字节：SEC/MIN/HR/DAY/WEEKDAY/MONTH/YEAR
 *  - 仅支持 24 小时制；年份按 2000+YY 解释
 *  - 写入请求由主循环通过 rtc_request_set() 延迟到 LVGL 任务内执行，
 *    以避免两个 FreeRTOS 任务争用同一根 I²C 总线
 * ============================================================ */

/* 初始化：探测芯片、强制 24h 模式、启动振荡器。返回 true 表示 RTC 在线 */
bool rtc_init(TwoWire &wire);

/* RTC 是否在线（初始化成功） */
bool rtc_is_available(void);

/* 用系统 epoch（已在 TZ=CST-8 下 localtime）写入 RTC */
bool rtc_set_time(time_t epoch);

/* 读出并填充 struct tm（本地时区）。失败（掉电/未设置）返回 false */
bool rtc_read_time(struct tm *out);

/* 由主循环在 NTP 同步后调用，记录待写入的 epoch；
 * 实际写入延迟到 LVGL 任务（rtc_process_pending）内执行 */
void rtc_request_set(time_t epoch);

/* 在 LVGL 定时器中调用：若有待写入则执行 rtc_set_time */
void rtc_process_pending(void);
