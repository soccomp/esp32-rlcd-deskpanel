#pragma once

/* 法语对话词库：内置 200 组（数据见 french_dialogues.h，仅 french_lib.cpp 包含）。
 * 每组含 A/B 法语 + A/B 中文，供首页法语学习卡同屏双语展示。
 * （8-14 起由"100 句单句 + SD french.txt"改为"内置 200 组对话"，不再读 SD。）
 */
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const char *a_fr;
    const char *b_fr;
    const char *a_cn;
    const char *b_cn;
} french_dialogue_t;

/* 总对话组数 */
uint16_t french_lib_count(void);

/* 取第 idx 组（0-based）对话，填入 *out。返回 true 成功。 */
bool french_lib_get(uint16_t idx, french_dialogue_t *out);
