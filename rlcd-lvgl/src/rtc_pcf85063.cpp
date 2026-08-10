#include "rtc_pcf85063.h"
#include <Arduino.h>

#define PCF_ADDR   0x51
#define REG_CTRL1  0x00
#define REG_CTRL2  0x01
#define REG_SEC    0x04   /* 时间寄存器起始：SEC/MIN/HR/DAY/WEEK/MONTH/YEAR */

static TwoWire *g_wire = nullptr;
static bool     g_ok   = false;
static time_t   g_set_pending = 0;   /* 0 = 无待写入 */

static inline uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static inline uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

static bool i2c_write_regs(uint8_t reg, const uint8_t *buf, uint8_t len)
{
    if (!g_wire) return false;
    g_wire->beginTransmission(PCF_ADDR);
    g_wire->write(reg);
    g_wire->write(buf, len);
    return g_wire->endTransmission() == 0;
}

static bool i2c_read_regs(uint8_t reg, uint8_t *buf, uint8_t len)
{
    if (!g_wire) return false;
    g_wire->beginTransmission(PCF_ADDR);
    g_wire->write(reg);
    if (g_wire->endTransmission(false) != 0) return false;   /* repeated-start，保持总线 */
    if (g_wire->requestFrom((int)PCF_ADDR, (int)len) != len) return false;
    for (uint8_t i = 0; i < len; i++) buf[i] = g_wire->read();
    return true;
}

bool rtc_init(TwoWire &wire)
{
    g_wire = &wire;
    uint8_t ctrl1 = 0;
    if (!i2c_read_regs(REG_CTRL1, &ctrl1, 1)) {
        g_ok = false;
        return false;   /* 芯片无响应（不应发生，板载已焊接） */
    }
    /* 强制 24 小时模式（清 bit1），并清除 STOP（清 bit5）以启动振荡器 */
    ctrl1 &= ~((1 << 1) | (1 << 5));
    i2c_write_regs(REG_CTRL1, &ctrl1, 1);
    g_ok = true;
    return true;
}

bool rtc_is_available(void) { return g_ok; }

void rtc_request_set(time_t epoch) { g_set_pending = epoch; }

void rtc_process_pending(void)
{
    if (g_set_pending != 0) {
        rtc_set_time(g_set_pending);
        g_set_pending = 0;
    }
}

bool rtc_set_time(time_t epoch)
{
    if (!g_ok) return false;
    struct tm *t = localtime(&epoch);
    if (!t) return false;

    uint8_t buf[7];
    buf[0] = dec2bcd((uint8_t)t->tm_sec)  & 0x7F;   /* 清 VL（电压低）标志位 */
    buf[1] = dec2bcd((uint8_t)t->tm_min);
    buf[2] = dec2bcd((uint8_t)t->tm_hour) & 0x3F;   /* 24h 模式 */
    buf[3] = dec2bcd((uint8_t)t->tm_mday);
    buf[4] = dec2bcd((uint8_t)(t->tm_wday & 7));
    buf[5] = dec2bcd((uint8_t)(t->tm_mon + 1));
    buf[6] = dec2bcd((uint8_t)(t->tm_year % 100));

    bool ok = i2c_write_regs(REG_SEC, buf, 7);

    /* 写完后再次确认振荡器已启动（清 STOP） */
    uint8_t c = 0;
    if (i2c_read_regs(REG_CTRL1, &c, 1)) {
        c &= ~(1 << 5);
        i2c_write_regs(REG_CTRL1, &c, 1);
    }
    return ok;
}

bool rtc_read_time(struct tm *out)
{
    if (!g_ok) return false;
    uint8_t buf[7];
    if (!i2c_read_regs(REG_SEC, buf, 7)) return false;

    int sec  = bcd2dec(buf[0] & 0x7F);
    int min  = bcd2dec(buf[1] & 0x7F);
    int hour = bcd2dec(buf[2] & 0x3F);
    int day  = bcd2dec(buf[3] & 0x3F);
    int mon  = bcd2dec(buf[5] & 0x1F);
    int year = bcd2dec(buf[6]) + 2000;

    /* 有效性校验：掉电后时间常为 00:00 或 2099 等非法值 */
    if (year < 2020 || year > 2099) return false;
    if (mon < 1 || mon > 12) return false;
    if (day < 1 || day > 31) return false;
    if (hour > 23 || min > 59 || sec > 59) return false;

    memset(out, 0, sizeof(*out));
    out->tm_year = year - 1900;
    out->tm_mon  = mon - 1;
    out->tm_mday = day;
    out->tm_hour = hour;
    out->tm_min  = min;
    out->tm_sec  = sec;
    out->tm_isdst = -1;
    mktime(out);   /* 规范化并填 tm_wday */
    return true;
}
