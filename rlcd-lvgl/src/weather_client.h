#pragma once

#include "lvgl.h"

/* ============================================================
 *  ESP32 直连和风天气（QWeather）获取天气（不再依赖 Mac 后端）
 *  - 和风 API：每项目专属 Host + API Key + Location ID（默认海淀 101010200）
 *  - 默认配置在 weather_client.cpp；可用 platformio.ini 的
 *      build_flags = -DWEATHER_API_HOST='"..."' -DWEATHER_API_KEY='"..."'
 *                    -DWEATHER_LOC_ID='"..."' -DWEATHER_CITY='"海淀"'
 *    覆盖
 *  - 解析 v7/weather/3d 的 daily[0/1].textDay / tempMax / tempMin
 *    （textDay 直接是中文天气描述，无需 WMO 映射）
 *  - 成功后写缓存（weather_qh）并刷新主页气象卡；失败从缓存恢复
 * ============================================================ */

/* 拉取结果分类：
 *   OK          成功
 *   RETRYABLE   可恢复失败（网络/429 超限）→ 调用方指数退避重试
 *   PERMANENT   永久失败（401 key错/403 额度冻结/404 位置错）→ 调用方停止重试
 */
typedef enum {
    WEATHER_OK = 0,
    WEATHER_RETRYABLE,
    WEATHER_PERMANENT,
} weather_result_t;

/* 返回拉取结果；失败时 UI 保留上一次有效数据。 */
weather_result_t fetch_weather_data(void);
