#include "weather_client.h"
#include "ui_clock.h"          // ui_clock_set_weather / ui_clock_update_weather
#include "lvgl_bsp.h"         // Lvgl_lock / Lvgl_unlock
#include "lv_font_chinese_18.h"
#include "lv_font_chinese_14.h"
#include "data_cache.h"       // 离线缓存：天气 JSON 快照到 SD
#include <time.h>             // time() / localtime_r / strftime（记录天气获取时间）
#include <string.h>           // strstr（天气文字→图标映射）
#include <stdlib.h>           // malloc / free
#include <esp_heap_caps.h>    // heap_caps_malloc（PSRAM 大缓冲）
#include <rom/miniz.h>        // tinfl_decompress（gzip 解压，ROM 内置）
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

/* 和风天气 API：每项目专属 Host + API Key + Location ID（海淀 101010200）。
 * 可在 platformio.ini 用 -DWEATHER_API_HOST / -DWEATHER_API_KEY / -DWEATHER_LOC_ID / -DWEATHER_CITY 覆盖 */
#ifndef WEATHER_API_HOST
#define WEATHER_API_HOST "ku3h2tg63t.re.qweatherapi.com"
#endif
#ifndef WEATHER_API_KEY
#define WEATHER_API_KEY  "6e111479f6fd439684e205e40d36ebe4"
#endif
#ifndef WEATHER_LOC_ID
#define WEATHER_LOC_ID   "101010200"   /* 北京海淀（GeoAPI 查得） */
#endif
#ifndef WEATHER_CITY
#define WEATHER_CITY     "海淀"
#endif

/* 和风天气 textDay → 单色图标映射（与 weather_icons.h 的图标集对应） */
static const char *qh_icon(const char *text)
{
    if (!text) return "cloud";
    if (strstr(text, "晴"))  return "sun";
    if (strstr(text, "雪") || strstr(text, "霰")) return "snow";
    if (strstr(text, "雨") || strstr(text, "雷")) return "rain";
    if (strstr(text, "雾") || strstr(text, "霾")) return "cloud";
    return "cloud";   /* 多云/阴/未知 */
}

/* gzip 解压（和风 Web API 强制返回 gzip 体，ESP32 HTTPClient 不解压）。
 * ROM miniz 的 tinfl_decompress_mem_to_heap 因 MINIZ_NO_MALLOC 不可用，
 * 改用 tinfl_decompress + 固定输出缓冲（和风 3d 明文 < 4KB）。
 * 输入 payload 若以 1f 8b 开头则解压（跳过 gzip 头 → 原始 DEFLATE → tinfl）。
 * 成功返回新 malloc 缓冲（调用方 free）；失败返回 NULL。 */
static char *maybe_gunzip(const String &payload)
{
    const uint8_t *p = (const uint8_t *)payload.c_str();
    size_t len = payload.length();
    if (len < 18 || p[0] != 0x1f || p[1] != 0x8b) {
        return NULL;   /* 非 gzip */
    }
    if (p[2] != 8) {
        Serial.println("[weather] gzip: CM!=8, not deflate");
        return NULL;
    }
    /* 解析 gzip 头（RFC1952）：0-1 magic, 2 CM, 3 FLG, 4-7 MTIME, 8 XFL, 9 OS, 可选字段 */
    uint8_t flg = p[3];
    size_t off = 10;
    if (flg & 0x04) {                 /* FEXTRA：2 字节长度 + 内容 */
        if (off + 2 > len) return NULL;
        size_t xlen = p[off] | (p[off+1] << 8);
        off += 2 + xlen;
    }
    if (flg & 0x08) {                 /* FNAME：null 结尾 */
        while (off < len && p[off] != 0) off++;
        off++;
    }
    if (flg & 0x10) {                 /* FCOMMENT：null 结尾 */
        while (off < len && p[off] != 0) off++;
        off++;
    }
    if (flg & 0x02) off += 2;         /* FHCRC：2 字节 */
    if (off >= len) {
        Serial.println("[weather] gzip: header parse failed");
        return NULL;
    }

    /* 原始 DEFLATE 流解压。tinfl 要求：
     *   - 不带 NON_WRAPPING flag → 输出缓冲必须 >= 32KB 字典窗口
     *   - 带 NON_WRAPPING flag → 输出缓冲须能容纳全部解压数据
     * 和风 3d 明文 < 2KB，PSRAM 分配 32KB 缓冲双满足。 */
    const size_t OUT_CAP = 32 * 1024;
    uint8_t *out_buf = (uint8_t *)heap_caps_malloc(OUT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out_buf) {
        Serial.println("[weather] gzip: OOM");
        return NULL;
    }
    tinfl_decompressor decomp;
    tinfl_init(&decomp);
    size_t in_avail = len - off;
    size_t out_avail = OUT_CAP;
    tinfl_status st = tinfl_decompress(&decomp, p + off, &in_avail,
                                       out_buf, out_buf, &out_avail,
                                       TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (st != TINFL_STATUS_DONE) {
        Serial.printf("[weather] gzip inflate failed: st=%d\n", (int)st);
        heap_caps_free(out_buf);
        return NULL;
    }
    size_t out_len = OUT_CAP - out_avail;
    char *res = (char *)malloc(out_len + 1);
    if (!res) { heap_caps_free(out_buf); return NULL; }
    memcpy(res, out_buf, out_len);
    res[out_len] = '\0';
    heap_caps_free(out_buf);
    Serial.printf("[weather] gzip %u -> %u bytes\n", (unsigned)len, (unsigned)out_len);
    return res;
}

/* 解析和风 JSON（daily 数组或后端代理的扁平字段）→ 更新天气卡。成功返回 true。 */
static bool apply_qh_payload(const char *json_str)
{
    /* ★ 8-18：直连和风返回的 7 天预报解压后实测 31040 字节（日志
     * "[weather] gzip 586 -> 31040 bytes"），远超原先 DynamicJsonDocument(3072)
     * → 直连路径永远 NoMemory 解析失败、只能回退 M1 后端代理（功能看似正常，
     * 实则白连一次且仍依赖 M1）。不能简单把 doc 开到 40KB：堆只剩 ~104KB，
     * 一次性大块分配容易碰上碎片。改用 ArduinoJson 的 Filter：解析时就丢掉
     * 不需要的字段，只保留 daily[].textDay/tempMax/tempMin 与后端代理的扁平
     * 字段，3072 字节足够，且堆占用与响应大小解耦。
     * 注意 filter 里数组用 [0] 作为"所有元素"的模板（ArduinoJson 6 语义）。 */
    StaticJsonDocument<384> filter;
    filter["daily"][0]["textDay"] = true;
    filter["daily"][0]["tempMax"] = true;
    filter["daily"][0]["tempMin"] = true;
    /* 后端代理 /api/weather_qh 的扁平字段（走回退路径时用） */
    filter["today_text"]    = true;
    filter["tomorrow_text"] = true;
    filter["today_high"]    = true;
    filter["today_low"]     = true;
    filter["tomorrow_high"] = true;
    filter["tomorrow_low"]  = true;

    DynamicJsonDocument doc(3072);
    DeserializationError werr = deserializeJson(doc, json_str,
                                    DeserializationOption::Filter(filter));
    if (werr != DeserializationError::Ok) {
        Serial.printf("Weather JSON parse error: %s\n", werr.c_str());
        return false;
    }
    const char *tc = NULL, *tm = NULL;
    int thi = 0, tlo = 0, mhi = 0, mlo = 0;
    JsonArray daily = doc["daily"].as<JsonArray>();
    if (!daily.isNull() && daily.size() >= 2) {
        /* 直连和风：daily[0/1] */
        tc  = daily[0]["textDay"] | "多云";
        tm  = daily[1]["textDay"] | "多云";
        thi = daily[0]["tempMax"] | 0;
        tlo = daily[0]["tempMin"] | 0;
        mhi = daily[1]["tempMax"] | 0;
        mlo = daily[1]["tempMin"] | 0;
    } else {
        /* 后端代理 /api/weather_qh：扁平字段 */
        tc  = doc["today_text"] | "多云";
        tm  = doc["tomorrow_text"] | "多云";
        thi = doc["today_high"] | 0;
        tlo = doc["today_low"]  | 0;
        mhi = doc["tomorrow_high"] | 0;
        mlo = doc["tomorrow_low"]  | 0;
    }
    if (!tc || !tm) return false;

    ui_clock_set_weather(WEATHER_CITY, tc, qh_icon(tc), thi, tlo,
                         tm, qh_icon(tm), mhi, mlo);
    /* 记录获取时间（MM-DD HH:MM）到天气卡顶部 */
    {
        time_t wt = time(nullptr);
        if (wt > 1700000000UL) {
            struct tm *wti = localtime(&wt);
            char ts[16];
            strftime(ts, sizeof(ts), "%m-%d %H:%M", wti);
            ui_clock_set_weather_time(ts);
        }
    }
    if (Lvgl_lock(2000)) { ui_clock_update_weather(); Lvgl_unlock(); }
    else Serial.println("[weather] Lvgl_lock 超时，天气 UI 未刷新");
    Serial.printf("Weather OK(%s): 今 %s %d~%d / 明 %s %d~%d\n",
                  WEATHER_CITY, tc, tlo, thi, tm, mlo, mhi);
    return true;
}

/* 回退：请求 Mac 后端 /api/weather_qh（HTTP 明文，Mac 出口正常可抓到和风）
 * 后端地址默认占位，真实值在本地 src/local_config.h（被 .gitignore 忽略）。 */
#if __has_include("local_config.h")
#include "local_config.h"
#endif
#ifndef WEATHER_QH_BACKEND
#define WEATHER_QH_BACKEND "http://YOUR_MAC_IP:8100/api/weather_qh"
#endif
static bool fetch_backend_qh(void)
{
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    http.begin(WEATHER_QH_BACKEND);
    http.setTimeout(6000);
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
        String payload = http.getString();
        data_cache_save("weather_qh", payload.c_str());
        bool ok = apply_qh_payload(payload.c_str());
        Serial.printf("[weather] backend proxy: HTTP %d, ok=%d\n", code, ok);
        http.end();
        return ok;
    }
    Serial.printf("[weather] backend proxy failed: HTTP %d\n", code);
    http.end();
    return false;
}

/* ★ 8-18 socket 泄漏根治：TLS 客户端改为函数内静态单例。
 * 旧实现在 TLS 连接失败时用 `client = nullptr` 泄漏对象来躲避析构 PANIC
 * （start_ssl_client:-1 后析构偶发崩溃）。但注释低估了代价——泄漏的不只是
 * ~KB 级内存，更致命的是那个 socket fd / lwIP TCP PCB 永不归还。
 * ESP32 CONFIG_LWIP_MAX_SOCKETS = 16：攒满 16 个泄漏 socket 后，本机所有
 * TCP 出站 connect 与入站 accept 全部失败，而 ICMP 不占 socket 所以 ping
 * 依旧完美 → 表现为"WiFi 显示已连接、ping 0 丢包，但取流全失败、:8771 也死"，
 * WiFi.reconnect() 救不回来（重连不释放 PCB），只有 esp_restart 能清空
 * → 面板每 30~60 秒楔死自重启一轮。
 * 正解：静态单例永不析构（规避 PANIC），每次使用前与失败后显式 stop()
 * 关闭 socket，PCB 立即归还 → 零泄漏且不崩。 */
static WiFiClientSecure &weather_tls_client(void)
{
    static WiFiClientSecure inst;
    return inst;
}

/* 直连和风天气，解析后写入天气卡缓存（本函数内部自管 Lvgl_lock 用于刷新） */
weather_result_t fetch_weather_data(void)
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Weather fetch skipped: WiFi not connected");
        return WEATHER_RETRYABLE;
    }

    WiFiClientSecure &client = weather_tls_client();
    client.stop();           /* 归还上一轮可能残留的 socket */
    client.setInsecure();    /* ★ 补齐：原代码只有注释没调用，导致直连 TLS 必失败 */
    bool success = false;
    weather_result_t result = WEATHER_RETRYABLE;

    HTTPClient http;
    String url = String("https://") + WEATHER_API_HOST
               + "/v7/weather/3d?location=" + WEATHER_LOC_ID
               + "&key=" + WEATHER_API_KEY
               + "&lang=zh&unit=m";
    Serial.printf("[weather] Fetching: %s\n", url.c_str());

    /* 先显式建立 TLS 连接（带超时）：BTWIFI6 网络出站 HTTPS 可能被 RST，
     * 若交给 HTTPClient 内部连接，无超时会无限阻塞。 */
    bool conn_ok = client.connect(WEATHER_API_HOST, 443, 8000);
    if (conn_ok) {
        http.begin(client, url);   /* 复用已连接 socket */
    } else {
        Serial.printf("[weather] connect fail, fallback backend\n");
        client.stop();   /* ★ 必须 stop：释放握手失败残留的 socket / TCP PCB */
    }
    http.setTimeout(8000);
    http.setUserAgent("ESP32-RLCD-DeskPanel");

    if (conn_ok) {
        int code = http.GET();
        if (code == HTTP_CODE_OK) {
            String payload = http.getString();
            char *plain = maybe_gunzip(payload);
            const char *json_str = plain ? plain : payload.c_str();
            /* 和风 v1 错误码：code 字段 200=成功，401/403/404 为永久性错误 */
            DynamicJsonDocument doc(2048);
            if (deserializeJson(doc, json_str) == DeserializationError::Ok) {
                const char *rc = doc["code"] | "200";
                if (strcmp(rc, "200") != 0) {
                    if (strcmp(rc, "401") == 0 || strcmp(rc, "403") == 0 ||
                        strcmp(rc, "404") == 0) {
                        Serial.printf("[weather] QWeather permanent err code=%s\n", rc);
                        result = WEATHER_PERMANENT;   /* key/额度/位置错：不重试 */
                    } else {
                        Serial.printf("[weather] QWeather retryable err code=%s\n", rc);
                    }
                } else {
                    data_cache_save("weather_qh", json_str);
                    success = apply_qh_payload(json_str);
                }
            }
            if (plain) free(plain);
        } else {
            Serial.printf("Weather fetch failed: HTTP %d\n", code);
        }
        http.end();
    }

    /* 直连失败（连接失败/HTTP 失败/解析失败）→ 回退 Mac 后端代理（和风数据） */
    if (!success && result != WEATHER_PERMANENT) {
        Serial.println("[weather] direct fail, try backend proxy");
        success = fetch_backend_qh();
    }

    /* 仍失败 → 从 SD 缓存恢复 */
    if (!success) {
        char *cached = data_cache_load("weather_qh");
        if (cached) {
            success = apply_qh_payload(cached);
            if (success) Serial.println("Weather restored from SD cache");
            free(cached);
        }
    }

    client.stop();   /* 静态单例：只关 socket 不析构，PCB 归还且不触发 PANIC */
    if (success) return WEATHER_OK;
    return result;   /* 保留永久/可恢复分类 */
}
