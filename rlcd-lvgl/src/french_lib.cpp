/* 法语词库：SD 优先（/sdcard/french.txt，每行 "法文\t中文"），内置 100 句兜底。
 * SD 词库整体读入 PSRAM，按行解析为 {fr, cn} 指针数组。
 */
#include "french_lib.h"
#include "french_quotes.h"      // 内置 kFrenchQuotes / kFrenchTranslations
#include "sd_card.h"
#include "log_store.h"
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#define SD_FRENCH_FILE "french.txt"

typedef struct {
    const char *fr;
    const char *cn;
} french_pair_t;

static french_pair_t *s_pairs = NULL;   /* PSRAM 动态数组 */
static uint16_t s_count = 0;
static bool s_from_sd = false;

/* 内置兜底：直接索引静态数组 */
static bool use_builtin_get(uint16_t idx, const char **fr, const char **cn)
{
    if (idx >= FRENCH_QUOTES_COUNT) return false;
    if (fr) *fr = kFrenchQuotes[idx];
    if (cn) *cn = kFrenchTranslations[idx];
    return true;
}

uint16_t french_lib_init(void)
{
    /* 尝试 SD 词库 */
    if (sd_card_ok() && sd_file_exists(SD_FRENCH_FILE)) {
        size_t len = 0;
        char *raw = (char *)sd_file_read_alloc(SD_FRENCH_FILE, &len);
        if (raw && len > 0) {
            /* 统计行数（\n 分隔） */
            uint32_t lines = 0;
            for (size_t i = 0; i < len; ++i) if (raw[i] == '\n') lines++;
            if (lines > 0 && lines <= 10000) {
                s_pairs = (french_pair_t *)heap_caps_malloc(
                    lines * sizeof(french_pair_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (s_pairs) {
                    /* 解析每行：法文<tab>中文（\r 容忍） */
                    uint16_t n = 0;
                    char *line = raw;
                    for (uint32_t i = 0; i < lines && n < lines; ++i) {
                        char *nl = strchr(line, '\n');
                        if (!nl) break;
                        *nl = '\0';
                        /* 去行尾 \r */
                        size_t ll = strlen(line);
                        if (ll > 0 && line[ll-1] == '\r') line[ll-1] = '\0';
                        /* 找分隔符 \t */
                        char *tab = strchr(line, '\t');
                        if (tab) {
                            *tab = '\0';
                            s_pairs[n].fr = line;
                            s_pairs[n].cn = tab + 1;
                            n++;
                        } else if (line[0]) {
                            /* 无中文，法文即显示内容 */
                            s_pairs[n].fr = line;
                            s_pairs[n].cn = line;
                            n++;
                        }
                        line = nl + 1;
                    }
                    if (n > 0) {
                        s_count = n;
                        s_from_sd = true;
                        Serial.printf("[french] SD 词库加载 %u 句\n", s_count);
                        log_store_append("french SD library %u quotes", s_count);
                        return s_count;
                    }
                    heap_caps_free(s_pairs);
                    s_pairs = NULL;
                }
            }
            heap_caps_free(raw);
        }
    }
    /* 回退内置 */
    s_count = FRENCH_QUOTES_COUNT;
    s_from_sd = false;
    Serial.printf("[french] 使用内置词库 %u 句\n", s_count);
    return s_count;
}

uint16_t french_lib_count(void)
{
    return s_count ? s_count : FRENCH_QUOTES_COUNT;
}

bool french_lib_get(uint16_t idx, const char **fr, const char **cn)
{
    if (s_from_sd && s_pairs && idx < s_count) {
        if (fr) *fr = s_pairs[idx].fr;
        if (cn) *cn = s_pairs[idx].cn;
        return true;
    }
    return use_builtin_get(idx, fr, cn);
}
