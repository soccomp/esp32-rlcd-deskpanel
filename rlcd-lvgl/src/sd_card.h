#pragma once

/* SD 卡驱动封装：SDMMC 1 线模式
 *   引脚（Waveshare 官方）：CLK=GPIO38, CMD=GPIO21, D0=GPIO39
 *   挂载点 /sdcard（FAT32）
 * 优雅降级：无卡/挂载失败时 sd_card_ok() 返回 false，所有写/读调用静默返回失败，
 * 不影响主功能（时钟/温湿度/网络数据照常）。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 初始化：挂载 SD 卡（幂等，可重复调用）。返回是否挂载成功。 */
bool sd_card_init(void);

/* 是否已挂载可用 */
bool sd_card_ok(void);

/* 挂载点路径（"/sdcard"），未挂载返回 NULL */
const char *sd_card_root(void);

/* 写文件（覆盖）：成功返回 true */
bool sd_file_write(const char *rel_path, const void *data, size_t len);

/* 追加写文件：成功返回 true（文件不存在则创建） */
bool sd_file_append(const char *rel_path, const void *data, size_t len);

/* 读文件：成功返回 true 并填充 *out_len（实际读到的字节数） */
bool sd_file_read(const char *rel_path, void *buf, size_t buf_size, size_t *out_len);

/* 读文件（整体读入，自动分配 PSRAM 缓冲）：返回 malloc 缓冲（调用方 free），
 * 失败返回 NULL 并置 *out_len=0 */
void *sd_file_read_alloc(const char *rel_path, size_t *out_len);

/* 删除文件：成功返回 true */
bool sd_file_remove(const char *rel_path);

/* 文件是否存在 */
bool sd_file_exists(const char *rel_path);

/* 确保目录存在（递归创建） */
bool sd_make_dir(const char *rel_path);
