#pragma once

/* 法语短句词库：SD 卡 /sdcard/french.txt 优先（每行格式：法文<分隔>中文）
 * 无卡/文件缺失 → 使用内置 100 句（french_quotes.h）。
 * SD 词库加载到 PSRAM，支持几千句。
 */
#include <stdbool.h>
#include <stdint.h>

/* 初始化：尝试加载 SD 词库（失败回退内置）。返回实际总句数。 */
uint16_t french_lib_init(void);

/* 总句数 */
uint16_t french_lib_count(void);

/* 取第 idx 句（0-based）的法文和中文。返回 true 成功。 */
bool french_lib_get(uint16_t idx, const char **fr, const char **cn);
