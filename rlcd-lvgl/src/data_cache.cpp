/* 离线数据缓存：JSON 快照存 /sdcard/cache/*.json，无卡静默降级。 */
#include "data_cache.h"
#include "sd_card.h"
#include "log_store.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CACHE_DIR "cache"

static bool s_ready = false;

bool data_cache_init(void)
{
    bool card = sd_card_init();
    bool dir = card && sd_make_dir(CACHE_DIR);
    s_ready = dir;
    if (!s_ready) {
        Serial.println("[cache] SD unavailable, cache disabled");
    }
    return s_ready;
}

static void cache_path(char *out, size_t sz, const char *key)
{
    snprintf(out, sz, "%s/%s.json", CACHE_DIR, key);
}

bool data_cache_save(const char *key, const char *json_payload)
{
    if (!s_ready || !key || !json_payload) return false;
    char path[80];
    cache_path(path, sizeof(path), key);
    bool ok = sd_file_write(path, json_payload, strlen(json_payload));
    if (ok) {
        log_store_append("cache save %s (%u B)", key, (unsigned)strlen(json_payload));
    }
    return ok;
}

char *data_cache_load(const char *key)
{
    if (!s_ready || !key) return NULL;
    char path[80];
    cache_path(path, sizeof(path), key);
    if (!sd_file_exists(path)) return NULL;
    size_t len = 0;
    char *buf = (char *)sd_file_read_alloc(path, &len);
    if (!buf) return NULL;
    return buf;    /* sd_file_read_alloc 已补 '\0' */
}
