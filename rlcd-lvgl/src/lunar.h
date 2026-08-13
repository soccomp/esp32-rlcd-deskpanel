#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ============================================================
 *  公历 -> 农历（查表法，lunar_table.h 由 scripts/gen_lunar_table.py 生成）
 *  支持公历 1900-01-31（农历 1900 正月初一）至农历 2099 年腊月
 *  （约公历 2100 年初）。范围外返回 false。
 * ============================================================ */

/* 公历转农历：填充 lunar_y / lunar_m(1-12) / lunar_d(1-30) / leap(是否闰月)。
 * 超出支持范围返回 false。 */
bool lunar_from_solar(int y, int m, int d,
                      int *lunar_y, int *lunar_m, int *lunar_d, bool *leap);

/* 农历月中文名：1=正月 ... 11=十一月 12=腊月；leap=true 时带"闰"前缀（如"闰四月"）。
 * 返回静态字符串，勿修改。 */
const char *lunar_month_cn(int m, bool leap);

/* 农历日中文名：1=初一 ... 10=初十 20=二十 30=三十。返回静态字符串，勿修改。 */
const char *lunar_day_cn(int d);

/* 便捷格式：buf 填 "七月廿一" / "闰四月廿三"（不含年份）。 */
void lunar_date_cn(int lunar_m, int lunar_d, bool leap, char *buf, size_t sz);
