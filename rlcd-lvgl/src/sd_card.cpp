/* SD 卡驱动：SDMMC 1 线（CLK=38, CMD=21, D0=39），挂载点 /sdcard（FAT32）
 * 复用已验证的官方引脚配置；无卡/失败优雅降级。
 */
#include "sd_card.h"
#include <Arduino.h>
#include <driver/sdmmc_host.h>
#include <driver/sdmmc_types.h>
#include <sdmmc_cmd.h>
#include <esp_vfs_fat.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#define SD_CLK      38
#define SD_CMD      21
#define SD_D0       39
#define SD_MOUNT    "/sdcard"

static sdmmc_card_t *s_sd_card = NULL;
static bool s_ok = false;

bool sd_card_init(void)
{
    if (s_ok) return true;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 8;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = (gpio_num_t)SD_CLK;
    slot.cmd = (gpio_num_t)SD_CMD;
    slot.d0  = (gpio_num_t)SD_D0;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot,
                                            &mount_config, &s_sd_card);
    if (err != ESP_OK) {
        Serial.printf("[sd] mount failed: %s (0x%x)\n", esp_err_to_name(err), err);
        s_sd_card = NULL;
        return false;
    }
    s_ok = true;
    sdmmc_card_print_info(stdout, s_sd_card);
    Serial.printf("[sd] mounted OK (%lluMB)\n",
                  (unsigned long long)(s_sd_card->csd.capacity *
                                       s_sd_card->csd.sector_size / (1024*1024)));
    return true;
}

bool sd_card_ok(void) { return s_ok && s_sd_card != NULL; }
const char *sd_card_root(void) { return s_ok ? SD_MOUNT : NULL; }

static void full_path(char *out, size_t sz, const char *rel)
{
    snprintf(out, sz, "%s/%s", SD_MOUNT, rel ? rel : "");
}

bool sd_file_write(const char *rel, const void *data, size_t len)
{
    if (!sd_card_ok() || !rel || !data) return false;
    char path[128];
    full_path(path, sizeof(path), rel);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len;
}

bool sd_file_append(const char *rel, const void *data, size_t len)
{
    if (!sd_card_ok() || !rel || !data) return false;
    char path[128];
    full_path(path, sizeof(path), rel);
    FILE *f = fopen(path, "ab");
    if (!f) return false;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len;
}

bool sd_file_read(const char *rel, void *buf, size_t buf_size, size_t *out_len)
{
    if (!sd_card_ok() || !rel || !buf || buf_size == 0) return false;
    char path[128];
    full_path(path, sizeof(path), rel);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t r = fread(buf, 1, buf_size, f);
    fclose(f);
    if (out_len) *out_len = r;
    return r > 0;
}

void *sd_file_read_alloc(const char *rel, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!sd_card_ok() || !rel) return NULL;
    char path[128];
    full_path(path, sizeof(path), rel);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }

    /* 多分配 1 字节放 '\0'，方便文本文件直接当 C 字符串用 */
    void *buf = heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz) { heap_caps_free(buf); return NULL; }
    ((char *)buf)[sz] = '\0';
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

bool sd_file_remove(const char *rel)
{
    if (!sd_card_ok() || !rel) return false;
    char path[128];
    full_path(path, sizeof(path), rel);
    return remove(path) == 0;
}

bool sd_file_exists(const char *rel)
{
    if (!sd_card_ok() || !rel) return false;
    char path[128];
    full_path(path, sizeof(path), rel);
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool sd_make_dir(const char *rel)
{
    if (!sd_card_ok() || !rel) return false;
    char path[128];
    full_path(path, sizeof(path), rel);
    /* 递归创建各级目录（支持 a/b/c）：已存在则视为成功 */
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            struct stat st;
            if (stat(tmp, &st) != 0) mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    struct stat st;
    if (stat(tmp, &st) == 0) return true;   /* 已存在（目录或文件）→ 成功 */
    return mkdir(tmp, 0755) == 0;
}
