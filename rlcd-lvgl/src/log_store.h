#pragma once

/* 本地日志落盘：日志追加写 /sdcard/log/YYYYMMDD.txt（按天分文件）
 * 滚动保留最近 7 天；无 SD 卡时静默跳过（不影响主流程）。
 */
#include <stdbool.h>

/* 初始化：确保 log 目录存在（无卡时返回 false，后续 log_store 静默跳过） */
bool log_store_init(void);

/* 追加一条日志（自动加时间戳前缀 + 换行）。任意线程可调。 */
void log_store_append(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* 清理超过 7 天的日志文件（每次 init 时调用一次即可） */
void log_store_cleanup(void);
