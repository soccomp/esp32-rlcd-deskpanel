/* 本地日志落盘：/sdcard/log/YYYYMMDD.txt，按天分文件，滚动保留 7 天。
 * 无卡/失败静默。时间戳用系统时间（NTP 同步后准确；未同步显示 epoch）。 */
#include "log_store.h"
#include "sd_card.h"
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

#define LOG_DIR   "log"
#define KEEP_DAYS 7

static bool s_ready = false;

bool log_store_init(void)
{
    bool card = sd_card_init();
    bool dir = card && sd_make_dir(LOG_DIR);
    s_ready = dir;
    if (s_ready) {
        log_store_cleanup();
        log_store_append("---- log started ----");
    }
    return s_ready;
}

void log_store_append(const char *fmt, ...)
{
    if (!s_ready) return;

    /* 时间戳 */
    time_t now = time(nullptr);
    struct tm tm;
    char ts[32], day[16], fname[64];
    if (now > 1700000000UL) {
        localtime_r(&now, &tm);
        strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", &tm);
        strftime(day, sizeof(day), "%Y%m%d", &tm);
    } else {
        snprintf(ts, sizeof(ts), "epoch=%ld", (long)now);
        snprintf(day, sizeof(day), "%ld", (long)now);
    }
    snprintf(fname, sizeof(fname), "%s/%s.txt", LOG_DIR, day);

    va_list ap;
    va_start(ap, fmt);
    char msg[256];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char line[320];
    snprintf(line, sizeof(line), "[%s] %s\n", ts, msg);
    sd_file_append(fname, line, strlen(line));
}

void log_store_cleanup(void)
{
    if (!s_ready) return;

    time_t now = time(nullptr);
    if (now <= 1700000000UL) return;   /* 时间未同步，不清扫 */

    char logpath[96];
    snprintf(logpath, sizeof(logpath), "%s/%s", sd_card_root(), LOG_DIR);

    DIR *d = opendir(logpath);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_REG) continue;
        /* 文件名 YYYYMMDD.txt，解析日期判断是否超 7 天 */
        if (strlen(e->d_name) != 12) continue;          /* 8 位日期 + .txt */
        if (strncmp(e->d_name + 8, ".txt", 4) != 0) continue;
        char datebuf[9];
        memcpy(datebuf, e->d_name, 8);
        datebuf[8] = '\0';
        char *end = NULL;
        long fileday = strtol(datebuf, &end, 10);
        if (!end || *end != '\0') continue;
        struct tm ftm = {0};
        ftm.tm_year = (int)(fileday / 10000) - 1900;
        ftm.tm_mon  = (int)(fileday / 100 % 100) - 1;
        ftm.tm_mday = (int)(fileday % 100);
        time_t ft = mktime(&ftm);
        if (ft <= 0) continue;
        if ((now - ft) > KEEP_DAYS * 86400L) {
            char full[128];
            snprintf(full, sizeof(full), "%s/%s", logpath, e->d_name);
            remove(full);
            Serial.printf("[log] cleaned old file: %s\n", e->d_name);
        }
    }
    closedir(d);
}
