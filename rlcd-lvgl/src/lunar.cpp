#include "lunar.h"
#include "lunar_table.h"

#include <string.h>
#include <stdio.h>

/* 公历日期 -> 自 1970-01-01 的天数（Howard Hinnant 标准算法） */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);              // [0, 399]
    const unsigned doy = (153 * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  // [0, 146096]
    return era * 146097 + (int64_t)doe - 719468;
}

/* 查表：农历 y 年（相对表基年）的月份序列与每月天数。
 * 编码 bit0-12 对应 days 顺序（正月..腊月，闰月插在 lm 后）：
 *   非闰 m 月位 = m-1（m<=lm）或 m（m>lm）；闰 lm 月位 = lm。
 * 返回月份数（无闰月 12，闰月 13）；*out_leap_m 返回闰月月份（0=无）。 */
static int year_months(int idx, int out_leap_m[1], int out_days[13])
{
    uint32_t enc = LUNAR_YEAR_TABLE[idx];
    int lm = (enc >> 16) & 0xF;
    *out_leap_m = lm;
    int k = 0;
    for (int m = 1; m <= 12; m++) {
        /* 无闰月(lm=0)或 m<=lm 时位 = m-1；有闰且 m>lm 时位 = m（闰月占位右移） */
        int bit = (lm == 0 || m <= lm) ? (m - 1) : m;
        out_days[k++] = ((enc >> bit) & 1) ? 30 : 29;   /* m 月 */
        if (m == lm) {
            out_days[k++] = ((enc >> m) & 1) ? 30 : 29; /* 闰 m 月 = 位 m */
        }
    }
    return k;
}

bool lunar_from_solar(int y, int m, int d,
                      int *lunar_y, int *lunar_m, int *lunar_d, bool *leap)
{
    if (!lunar_y || !lunar_m || !lunar_d || !leap) return false;

    /* offset = 目标公历日距 1900-01-31（农历 1900 正月初一）的天数 */
    int64_t offset = days_from_civil(y, m, d) - days_from_civil(LUNAR_EPOCH_Y, LUNAR_EPOCH_M, LUNAR_EPOCH_D);
    if (offset < 0) return false;

    /* 定位农历年 */
    int y_idx = 0;
    for (;;) {
        if (y_idx >= LUNAR_TABLE_LEN) return false;          /* 超出 2099 年 */
        int lm = 0;
        int days[13];
        int n = year_months(y_idx, &lm, days);
        int64_t yd = 0;
        for (int i = 0; i < n; i++) yd += days[i];
        if (offset >= yd) {
            offset -= yd;
            y_idx++;
        } else {
            break;
        }
    }

    /* 定位农历月 */
    int lm = 0;
    int days[13];
    int n = year_months(y_idx, &lm, days);
    int mi = 0;
    while (mi < n && offset >= days[mi]) {
        offset -= days[mi];
        mi++;
    }
    if (mi >= n) return false;                                /* 不应发生 */

    /* 月份序号 -> 农历月/闰月标志：顺序 1..lm(非闰), 闰lm, lm+1..12 */
    int lunar_m_val = 0;
    bool is_leap = false;
    {
        int seq = 0;
        for (int mm = 1; mm <= 12; mm++) {
            if (seq == mi) { lunar_m_val = mm; is_leap = false; break; }
            seq++;
            if (mm == lm) {
                if (seq == mi) { lunar_m_val = mm; is_leap = true; break; }
                seq++;
            }
        }
    }

    *lunar_y = LUNAR_TABLE_Y0 + y_idx;
    *lunar_m = lunar_m_val;
    *lunar_d = (int)offset + 1;
    *leap = is_leap;
    return true;
}

static const char *const LUNAR_MONTH_CN[] = {
    "正月", "二月", "三月", "四月", "五月", "六月",
    "七月", "八月", "九月", "十月", "十一月", "腊月",
};

static const char *const LUNAR_DAY_CN[] = {
    "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
    "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
    "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十",
};

const char *lunar_month_cn(int m, bool leap)
{
    if (m < 1 || m > 12) return "";
    if (leap) {
        static char buf[8];
        /* "闰" + 月份名去"月"（UTF-8 每汉字 3 字节）+ "月" */
        const char *base = LUNAR_MONTH_CN[m - 1];   /* 如 "四月" */
        size_t base_len = strlen(base);
        size_t keep = (base_len >= 3) ? (base_len - 3) : 0;
        size_t i = 0;
        memcpy(buf, "闰", 3); i += 3;
        memcpy(buf + i, base, keep); i += keep;
        memcpy(buf + i, "月", 3); i += 3;
        buf[i] = '\0';
        return buf;
    }
    return LUNAR_MONTH_CN[m - 1];
}

const char *lunar_day_cn(int d)
{
    if (d < 1 || d > 30) return "";
    return LUNAR_DAY_CN[d - 1];
}

void lunar_date_cn(int lunar_m, int lunar_d, bool leap, char *buf, size_t sz)
{
    if (!buf || sz == 0) return;
    if (lunar_m < 1 || lunar_m > 12 || lunar_d < 1 || lunar_d > 30) {
        buf[0] = '\0';
        return;
    }
    const char *mo = lunar_month_cn(lunar_m, leap);
    const char *da = lunar_day_cn(lunar_d);
    int n = snprintf(buf, sz, "%s%s", mo, da);
    if (n < 0) buf[0] = '\0';
}
