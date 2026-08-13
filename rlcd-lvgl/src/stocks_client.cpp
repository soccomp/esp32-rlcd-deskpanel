#include "stocks_client.h"
#include "ui_clock.h"       // ui_clock_set_stocks / ui_clock_update_stocks
#include "lvgl_bsp.h"       // Lvgl_lock / Lvgl_unlock
#include "data_cache.h"     // 离线缓存：fetch 成功保存 JSON 快照到 SD
#include <Arduino.h>
#include <WiFi.h>           // WiFi.status()
#include <WiFiUdp.h>
#include <ESPmDNS.h>        // mDNS 解析 Mac 主机名
#include <HTTPClient.h>
#include <ArduinoJson.h>

/* ============================================================
 *  后端 API 地址：通过 mDNS 解析 Mac 主机名动态获取 IP，无需硬编码 IP。
 *  SCHEDULE_API_HOST 是 Mac 的 mDNS 主机名（不含 .local 后缀）。
 *  如果 mDNS 解析失败，回退到 SCHEDULE_API_FALLBACK_URL。
 *  真实主机名/IP 在本地 src/local_config.h（被 .gitignore 忽略，不提交 GitHub）；
 *  公开仓库无该文件时使用下方占位（复制 local_config.example.h 为 local_config.h 并填入自己的值）。
 * ============================================================ */
#if __has_include("local_config.h")
#include "local_config.h"
#endif
#ifndef SCHEDULE_API_HOST
#define SCHEDULE_API_HOST "YOUR_MAC_HOSTNAME"
#endif
#ifndef SCHEDULE_API_PORT
#define SCHEDULE_API_PORT 8100
#endif
#ifndef SCHEDULE_API_FALLBACK_URL
#define SCHEDULE_API_FALLBACK_URL "http://YOUR_MAC_IP:8100/api/schedule"
#endif

/* —— mDNS 解析缓存 —— */
static String   g_resolved_base = "";    // mDNS 解析成功后拼出的 base（http://ip:port）
static bool     g_mdns_ok = false;        // 上次 mDNS 解析是否成功
static uint32_t g_mdns_resolve_ms = 0;    // 上次 mDNS 解析时间戳

/* 通过 mDNS 解析 Mac 主机名，返回 base（http://ip:port）。
 * 解析成功缓存 5 分钟，失败时立即重试。 */
static String resolve_api_base(void)
{
    static bool mdns_init_done = false;
    if (!mdns_init_done) {
        MDNS.begin("esp32-rlcd");
        mdns_init_done = true;
    }

    uint32_t now = millis();
    // 缓存有效期 5 分钟
    if (g_mdns_ok && (now - g_mdns_resolve_ms) < 300000UL && g_resolved_base.length() > 0) {
        return g_resolved_base;
    }

    Serial.printf("[mDNS] Resolving host \"%s\" ...\n", SCHEDULE_API_HOST);
    IPAddress ip = MDNS.queryHost(SCHEDULE_API_HOST, 3000);
    if (ip != INADDR_NONE && ip != IPAddress(0,0,0,0)) {
        g_resolved_base = "http://" + ip.toString() + ":" + String(SCHEDULE_API_PORT);
        g_mdns_ok = true;
        g_mdns_resolve_ms = now;
        Serial.printf("[mDNS] Resolved base: %s\n", g_resolved_base.c_str());
        return g_resolved_base;
    } else {
        g_mdns_ok = false;
        Serial.println("[mDNS] Resolution failed, using fallback base");
        String fb = String(SCHEDULE_API_FALLBACK_URL);
        int p = fb.indexOf("/api/");
        if (p > 0) fb = fb.substring(0, p);
        return fb;
    }
}

/* ============================================================
 *  股票指数行情：mDNS 找本机后端 GET /api/stocks，解析后刷新主页右上卡
 *  返回 {"stocks":[{name,value,pct,up}x3], "updated"}。
 *  交易时段每 10 分钟由 main.cpp 调用。
 * ============================================================ */
void fetch_stocks_data(void)
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Stocks fetch skipped: WiFi not connected");
        return;
    }

    String url = resolve_api_base() + "/api/stocks";
    Serial.printf("[stocks] Fetching: %s\n", url.c_str());

    HTTPClient http;
    http.begin(url);
    http.setTimeout(8000);
    http.setUserAgent("ESP32-RLCD-DeskPanel");

    int code = http.GET();
    if (code == HTTP_CODE_OK) {
        String payload = http.getString();
        DynamicJsonDocument doc(1024);
        DeserializationError err = deserializeJson(doc, payload);
        if (err) {
            Serial.printf("Stocks JSON parse error: %s\n", err.c_str());
            http.end();
            return;
        }

        const char *names[4] = {nullptr, nullptr, nullptr, nullptr};
        float values[4] = {0, 0, 0, 0};
        float pcts[4]   = {0, 0, 0, 0};
        bool  ups[4]    = {false, false, false, false};

        JsonArray arr = doc["stocks"].as<JsonArray>();
        int n = 0;
        for (JsonObject s : arr) {
            if (n >= 4) break;
            names[n]  = s["name"] | "";
            values[n] = s["value"] | 0.0f;
            pcts[n]   = s["pct"]   | 0.0f;
            ups[n]    = s["up"]    | (pcts[n] >= 0);
            n++;
        }

        /* 空数组 = 后端行情源异常/不可用（历史 bug：会渲染出 "-- 0.00 0.00%" 假数据）。
         * 此时不覆盖已有数据、不写 SD 缓存，保留旧值（无旧值则维持"行情获取中..."），
         * 并强制下次重新 mDNS 解析，等待下个周期重试。 */
        if (n == 0) {
            Serial.println("[stocks] 空数据（后端行情源异常），保留旧值");
            if (g_mdns_ok) g_mdns_resolve_ms = 0;  // 下次强制重解析
            http.end();
            return;
        }

        data_cache_save("stocks", payload.c_str());      // 离线快照（仅有效数据）
        ui_clock_set_stocks(names, values, pcts, ups);
        if (Lvgl_lock(2000)) {
            ui_clock_update_stocks();
            Lvgl_unlock();
        } else {
            Serial.println("[stocks] Lvgl_lock 超时，行情 UI 未刷新");
        }
        Serial.printf("[stocks] OK: %d quotes\n", n);
    } else {
        Serial.printf("Stocks fetch failed: HTTP %d\n", code);
        if (g_mdns_ok) g_mdns_resolve_ms = 0;  // 下次强制重解析
        load_cached_stocks();   /* 后端不可达：从 SD 缓存恢复上次数据 */
    }
    http.end();
}

/* 从 SD 缓存恢复行情（fetch 失败时调用；无缓存则保持现状） */
void load_cached_stocks(void)
{
    char *json = data_cache_load("stocks");
    if (!json) return;
    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, json) == DeserializationError::Ok) {
        const char *names[4] = {0, 0, 0, 0};
        float values[4] = {0, 0, 0, 0};
        float pcts[4]   = {0, 0, 0, 0};
        bool  ups[4]    = {false, false, false, false};
        JsonArray arr = doc["stocks"].as<JsonArray>();
        int n = 0;
        for (JsonObject s : arr) {
            if (n >= 4) break;
            names[n]  = s["name"] | "";
            values[n] = s["value"] | 0.0f;
            pcts[n]   = s["pct"]   | 0.0f;
            ups[n]    = s["up"]    | (pcts[n] >= 0);
            n++;
        }
        if (n > 0) {
            ui_clock_set_stocks(names, values, pcts, ups);
            if (Lvgl_lock(2000)) {
                ui_clock_update_stocks();
                Lvgl_unlock();
            }
            Serial.printf("[stocks] restored %d quotes from SD cache\n", n);
        }
    }
    free(json);
}
