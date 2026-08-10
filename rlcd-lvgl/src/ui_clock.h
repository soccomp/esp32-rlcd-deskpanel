#pragma once

#include "lvgl.h"

/* ============================================================
 *  桌面主页（Clock Page / Page 0）模块
 *  - 四张独立机械页片式翻页钟（上下页缝 + 双侧铰链）+ 日期星期
 *  - 左：室内外气象卡（左半 SHTC3 室内大字温湿度 | 右半 今日/明日天气）
 *  - 右：反色卡 = 上半截 X(@thsottiaux) 最新帖文 + 下半截迷你会议提示
 *  - 1s 定时器从 PCF85063A RTC 读时；5s 定时器读 SHTC3
 *  - shtc3_read() 由 main.cpp 提供（仅在 LVGL 任务内被调用，避免跨任务争用 I²C）
 *  - 天气由 ESP32 通过 HTTPS 直连 Open-Meteo，联网后立即取一次；
 *    成功后每小时刷新，失败时每 5 分钟重试并保留上一次有效数据；
 *    未获取到时天气区显示“天气获取中...”，不影响室内温湿度 5s 刷新
 * ============================================================ */

/* 构建 Page 0 内容（在 Lvgl_lock 保护下由 ui_init 调用） */
void ui_clock_init(lv_obj_t *parent, lv_obj_t *status_bar);

/* fetch_schedule_data() 更新会议列表后调用，刷新右下角迷你会议提示 */
void ui_clock_sync_meeting(void);

/* ---- 股票指数行情（右上卡） ----
 * ui_clock_set_stocks(): 仅缓存数据（线程安全、不触碰 LVGL 对象），
 *   由 fetch_stocks_data() 在解析后端 /api/stocks 后调用。
 * ui_clock_update_stocks(): 把缓存应用到行情卡（必须持有 Lvgl_lock 或在
 *   LVGL 任务内调用）。三指数：上证/沪深300/创业板指。 */
void ui_clock_set_stocks(const char *names[3], const float values[3],
                         const float pcts[3], const bool ups[3]);
void ui_clock_update_stocks(void);

/* ---- 天气 ----
 * ui_clock_set_weather(): 仅缓存数据（线程安全、不触碰 LVGL 对象），
 *   由 weather_client.cpp 在解析 Open-Meteo 响应后调用。
 * ui_clock_update_weather(): 把缓存应用到天气卡（必须持有 Lvgl_lock 或在
 *   LVGL 任务内调用），fetch_schedule_data() 成功后同步调用。 */
void ui_clock_set_weather(const char *city,
                          const char *today_text, const char *today_code,
                          int today_high, int today_low,
                          const char *tomorrow_text, const char *tomorrow_code,
                          int tomorrow_high, int tomorrow_low);
void ui_clock_update_weather(void);

/* 记录天气预报获取时间（MM-DD HH:MM），显示在天气卡顶部两铆钉之间、右对齐贴右侧铆钉。
 * 由 weather_client.cpp 在 Open-Meteo 获取成功后调用（线程安全，仅缓存字符串）。 */
void ui_clock_set_weather_time(const char *time_str);

/* 由 main.cpp 实现：读取 SHTC3 温湿度（内部用 g_shtc3，仅在 LVGL 任务调用安全） */
bool shtc3_read(float *temp, float *humi);
