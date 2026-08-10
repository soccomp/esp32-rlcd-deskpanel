#pragma once

/* OTA 固件备份：OTA 升级开始前，把当前运行固件（app 分区）备份到 SD 卡
 *   /sdcard/backup/fw_YYYYMMDD_HHMMSS.bin
 * 无 SD 卡/读取失败时静默跳过（不阻断 OTA）。
 */
#include <stdbool.h>

/* 在 ArduinoOTA.onStart() 中调用（此时旧固件仍可读） */
void ota_backup_current_fw(void);
