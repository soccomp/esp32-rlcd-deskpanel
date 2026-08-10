#pragma once

/* 离线数据缓存：网络数据（天气/会议/X帖文/行情）JSON 快照存 SD 卡。
 *  - 每次 fetch 成功后 save_* 写入 /sdcard/cache/xxx.json
 *  - 开机/数据不可用时 load_* 读取缓存（电脑关机时屏幕显示最后数据）
 *  - 无 SD 卡时静默（缓存功能自动禁用，不影响主流程）
 */
#include <stdbool.h>

/* 初始化：确保 cache 目录存在。无卡返回 false（后续 save/load 静默）。 */
bool data_cache_init(void);

/* 保存原始 JSON payload 到缓存文件 */
bool data_cache_save(const char *key, const char *json_payload);

/* 读取缓存 JSON（自动分配 PSRAM 缓冲，调用方 free）。无缓存返回 NULL。 */
char *data_cache_load(const char *key);
