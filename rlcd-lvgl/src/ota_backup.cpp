/* OTA 固件备份：读当前 app 分区 → /sdcard/backup/fw_*.bin
 * 用 esp_partition 只读接口读整个 app 分区（含引导头），存到 SD。
 * 无 SD / 读失败 → 静默跳过。
 */
#include "ota_backup.h"
#include "sd_card.h"
#include "log_store.h"
#include <Arduino.h>
#include <esp_partition.h>
#include <stdio.h>
#include <time.h>

void ota_backup_current_fw(void)
{
    if (!sd_card_ok()) {
        Serial.println("[ota] SD not ready, skip backup");
        return;
    }

    const esp_partition_t *app = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    if (!app) {
        Serial.println("[ota] app partition not found, skip backup");
        return;
    }

    /* 目录 */
    if (!sd_make_dir("backup")) {
        Serial.println("[ota] cannot create backup dir");
        return;
    }

    /* 文件名带时间 */
    time_t now = time(nullptr);
    char fname[64];
    if (now > 1700000000UL) {
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(fname, sizeof(fname), "backup/fw_%Y%m%d_%H%M%S.bin", &tm);
    } else {
        snprintf(fname, sizeof(fname), "backup/fw_%ld.bin", (long)now);
    }

    /* 分块读取写 SD */
    const size_t CHUNK = 16 * 1024;
    static uint8_t buf[CHUNK];
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", sd_card_root(), fname);

    FILE *f = fopen(path, "wb");
    if (!f) {
        Serial.println("[ota] cannot open backup file");
        return;
    }

    esp_err_t err;
    size_t total = 0;
    for (size_t off = 0; off < app->size; off += CHUNK) {
        size_t n = (app->size - off < CHUNK) ? (app->size - off) : CHUNK;
        err = esp_partition_read(app, off, buf, n);
        if (err != ESP_OK) {
            Serial.printf("[ota] read fail @%u: %s\n", (unsigned)off, esp_err_to_name(err));
            fclose(f);
            remove(path);
            return;
        }
        if (fwrite(buf, 1, n, f) != n) {
            Serial.println("[ota] write fail");
            fclose(f);
            remove(path);
            return;
        }
        total += n;
    }
    fclose(f);
    Serial.printf("[ota] firmware backed up: %s (%u KB)\n", fname, (unsigned)(total / 1024));
    log_store_append("OTA backup %s (%u KB)", fname, (unsigned)(total / 1024));
}
