#include "stocks_client.h"
#include "ui_clock.h"       // ui_clock_set_stocks / ui_clock_update_stocks
#include "lvgl_bsp.h"       // Lvgl_lock / Lvgl_unlock
#include "data_cache.h"     // 离线缓存：fetch 成功保存 JSON 快照到 SD
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

/* 本地后端地址覆盖（被 gitignore，Mac 局域网 IP；见 local_config.example.h） */
#if __has_include("local_config.h")
#include "local_config.h"
#endif

/* ============================================================
 *  股票指数行情：设备直连公开免费行情源（脱离 Mac 后端）
 *  - 主源：腾讯 qt.gtimg.cn（GBK 文本，只解析 ASCII 数字字段）
 *  - 备用：新浪 hq.sinajs.cn（需 Referer，涨跌幅自行计算）——BTWIFI6 老坑
 *    导致腾讯 TLS 间歇失败时自动切换，双源互备提升成功率
 *  - AI 行 = 159819 人工智能ETF易方达（用户 8-17 定，替代原东财 930713 指数）
 *  返回与旧后端 /api/stocks 完全一致：{"stocks":[{name,value,pct,up}...]}
 *  交易时段每 10 分钟由 main.cpp 调用；双源均失败从 SD 缓存恢复。
 * ============================================================ */

/* 腾讯行情：代码 + 硬编码缩写名（名称不解析 GBK 中文，直接映射） */
typedef struct { const char *code; const char *name; } tx_idx_t;
static const tx_idx_t TX_IDX[] = {
    {"sh000001", "上证"},
    {"sh000300", "300"},
    {"sz399006", "创板"},
    {"sz159819", "AI"},
};
#define TX_IDX_N 4

/* 抓到的行情（最多 4 个） */
typedef struct { const char *name; float value; float pct; bool up; } quote_t;
static quote_t g_quotes[4];
static int     g_quote_n = 0;

/* ★ 8-18 socket 泄漏根治（与 weather_client.cpp 同款）：
 * 旧实现 TLS 连接失败时 `client = nullptr` 泄漏对象以躲避析构 PANIC，
 * 代价被严重低估——泄漏的是 socket fd / lwIP TCP PCB，不是几 KB 内存。
 * CONFIG_LWIP_MAX_SOCKETS = 16，攒满即本机所有 TCP connect/accept 全挂，
 * 但 ping 仍通（ICMP 不占 socket）；WiFi.reconnect() 无法恢复，
 * 只有 esp_restart 能清空 → 面板 30~60 秒楔死自重启一轮。
 * 正解：静态单例永不析构（规避 PANIC）+ 用前/失败后 stop() 归还 socket。
 * 注意：本函数在股票刷新任务中串行调用，单例无并发问题。 */
static WiFiClientSecure &stocks_tls_client(void)
{
    static WiFiClientSecure inst;
    return inst;
}

/* 一次性 HTTPS GET：成功返回 body（非空），失败返回空 String。 */
static String https_get(const char *host, const String &path,
                        const char *hdr[][2], int nhdr)
{
    String body;
    WiFiClientSecure &client = stocks_tls_client();
    client.stop();           /* 归还上一轮残留 socket */
    client.setInsecure();    /* 桌面板场景跳过证书校验 */

    if (!client.connect(host, 443, 8000)) {
        Serial.printf("[stocks] TLS connect %s failed\n", host);
        client.stop();       /* ★ 必须 stop：释放握手失败残留的 socket / TCP PCB */
        return body;
    }

    HTTPClient http;
    http.begin(client, String("https://") + host + path);
    http.setTimeout(8000);
    for (int i = 0; i < nhdr; i++) http.addHeader(hdr[i][0], hdr[i][1]);
    int code = http.GET();
    if (code == HTTP_CODE_OK) body = http.getString();
    else Serial.printf("[stocks] %s HTTP %d\n", host, code);
    http.end();
    client.stop();           /* 关 socket 但不析构 */
    return body;
}

/* 提取以 '~' 分隔的第 idx 个字段（0-based），不存在返回空串 */
static String field_at(const String &s, int idx)
{
    int pos = 0;
    for (int i = 0; i < idx; i++) {
        pos = s.indexOf('~', pos);
        if (pos < 0) return "";
        pos++;
    }
    int end = s.indexOf('~', pos);
    if (end < 0) end = s.length();
    return s.substring(pos, end);
}

/* 腾讯：上证/沪深300/创业板/AI(159819 人工智能ETF)。
 * 返回形如 v_sh000001="1~上证指数~000001~3822.28~...~12.62~0.33~...";
 * 字段（~ 分隔）：[3]=点位  [32]=涨跌幅%。中文名不用，按代码映射缩写。 */
static void fetch_tencent(void)
{
    String codes = TX_IDX[0].code;
    for (int i = 1; i < TX_IDX_N; i++) { codes += ','; codes += TX_IDX[i].code; }
    const char *hdr[1][2] = {{"User-Agent", "rlcd-desk-panel/1.0"}};
    String body = https_get("qt.gtimg.cn", String("/q=") + codes, hdr, 1);
    if (body.length() == 0) return;

    int pos = 0;
    while (pos < (int)body.length() && g_quote_n < 4) {
        int eq = body.indexOf('=', pos);
        if (eq < 0) break;
        int q1 = body.indexOf('"', eq);
        int q2 = body.indexOf('"', q1 + 1);
        if (q1 < 0 || q2 < 0) break;

        String key     = body.substring(pos, eq);      /* "v_sh000001" */
        String payload = body.substring(q1 + 1, q2);
        String vs = field_at(payload, 3);              /* 点位 */
        String ps = field_at(payload, 32);             /* 涨跌幅% */

        if (vs.length() > 0 && ps.length() > 0) {
            const char *nm = NULL;
            for (int i = 0; i < TX_IDX_N; i++) {
                if (key.indexOf(TX_IDX[i].code) >= 0) { nm = TX_IDX[i].name; break; }
            }
            if (nm && g_quote_n < 4) {
                float pct = ps.toFloat();
                g_quotes[g_quote_n].name  = nm;
                g_quotes[g_quote_n].value = vs.toFloat();
                g_quotes[g_quote_n].pct   = pct;
                g_quotes[g_quote_n].up    = pct >= 0.0f;
                g_quote_n++;
            }
        }
        pos = q2 + 1;
    }
}

/* 新浪备用源：上证/沪深300/创业板/AI(159819)。
 * 返回形如 var hq_str_sh000001="上证指数,今开,昨收,现价,...";
 * 字段（, 分隔）：[2]=昨收  [3]=现价；涨跌幅 = (现价-昨收)/昨收×100。
 * 需 Referer（新浪校验反爬），名称仍按代码硬编码映射。 */
static void fetch_sina(void)
{
    String codes = TX_IDX[0].code;
    for (int i = 1; i < TX_IDX_N; i++) { codes += ','; codes += TX_IDX[i].code; }
    const char *hdr[2][2] = {
        {"Referer", "https://finance.sina.com.cn"},
        {"User-Agent", "rlcd-desk-panel/1.0"},
    };
    String body = https_get("hq.sinajs.cn", String("/list=") + codes, hdr, 2);
    if (body.length() == 0) return;

    int pos = 0;
    while (pos < (int)body.length() && g_quote_n < 4) {
        int eq = body.indexOf('=', pos);
        if (eq < 0) break;
        int q1 = body.indexOf('"', eq);
        int q2 = body.indexOf('"', q1 + 1);
        if (q1 < 0 || q2 < 0) break;

        String key     = body.substring(pos, eq);      /* "var hq_str_sh000001" */
        String payload = body.substring(q1 + 1, q2);
        String cur  = field_at(payload, 3);            /* 现价 */
        String prev = field_at(payload, 2);            /* 昨收 */

        float p = prev.toFloat();
        if (cur.length() > 0 && p != 0.0f) {
            const char *nm = NULL;
            for (int i = 0; i < TX_IDX_N; i++) {
                if (key.indexOf(TX_IDX[i].code) >= 0) { nm = TX_IDX[i].name; break; }
            }
            if (nm && g_quote_n < 4) {
                float c = cur.toFloat();
                float pct = (c - p) / p * 100.0f;
                g_quotes[g_quote_n].name  = nm;
                g_quotes[g_quote_n].value = c;
                g_quotes[g_quote_n].pct   = pct;
                g_quotes[g_quote_n].up    = pct >= 0.0f;
                g_quote_n++;
            }
        }
        pos = q2 + 1;
    }
}

/* 兜底：请求 Mac 后端 /api/stocks（HTTP 明文局域网，Mac 出口可正常出公网抓数）
 * 后端地址复用 local_config.h（与天气代理同机）。注意：后端旧代码的 AI 行仍是
 * 930713 指数（点位 6xxx），设备端已改 159819（价格 1.8xx）——回退时跳过该行，
 * AI 行保留设备内存旧值，避免数值体系跳变。 */
#ifndef STOCKS_BACKEND_URL
#define STOCKS_BACKEND_URL "http://YOUR_MAC_IP:8100/api/stocks"
#endif
static char g_backend_names[4][16] = {{0}};   /* 后端名字拷贝缓冲（JSON doc 生命周期短） */

static bool fetch_backend_stocks(void)
{
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    http.begin(STOCKS_BACKEND_URL);
    http.setTimeout(6000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[stocks] backend proxy failed: HTTP %d\n", code);
        http.end();
        return false;
    }
    String payload = http.getString();
    http.end();

    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, payload) != DeserializationError::Ok) return false;
    JsonArray arr = doc["stocks"].as<JsonArray>();
    g_quote_n = 0;
    for (JsonObject s : arr) {
        if (g_quote_n >= TX_IDX_N) break;
        const char *nm = s["name"] | "";
        if (!nm[0]) continue;
        const char *cd = s["code"] | "";
        if (cd[0] && strstr(cd, "930713")) continue;   /* 跳过旧 AI 指数行 */
        strncpy(g_backend_names[g_quote_n], nm, sizeof(g_backend_names[0]) - 1);
        g_backend_names[g_quote_n][sizeof(g_backend_names[0]) - 1] = '\0';
        g_quotes[g_quote_n].name  = g_backend_names[g_quote_n];
        g_quotes[g_quote_n].value = s["value"] | 0.0f;
        g_quotes[g_quote_n].pct   = s["pct"]   | 0.0f;
        g_quotes[g_quote_n].up    = s["up"]    | (g_quotes[g_quote_n].pct >= 0);
        g_quote_n++;
    }
    return g_quote_n > 0;
}

void fetch_stocks_data(void)
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Stocks fetch skipped: WiFi not connected");
        return;
    }

    g_quote_n = 0;
    fetch_tencent();
    if (g_quote_n != TX_IDX_N) {
        /* 腾讯未全量成功（TLS 间歇失败）：整体改用新浪备用源重拉，保证四行同源一致 */
        if (g_quote_n > 0)
            Serial.printf("[stocks] 腾讯源仅 %d/%d，切换新浪备用源\n", g_quote_n, TX_IDX_N);
        g_quote_n = 0;
        fetch_sina();
    }

    if (g_quote_n == 0) {
        /* 直连双源均失败（BTWIFI6 整机 RST）→ Mac 后端代理兜底，与天气回退一致 */
        Serial.println("[stocks] 直连双源失败，尝试 Mac 后端代理");
        if (fetch_backend_stocks()) {
            Serial.printf("[stocks] backend proxy OK: %d quotes\n", g_quote_n);
        } else {
            Serial.println("[stocks] 后端代理也失败，恢复 SD 缓存");
            load_cached_stocks();
            return;
        }
    }

    /* 记录最近一次成功刷新时刻（NTP 未同步时 time() 为 1970，UI 侧会过滤） */
    ui_clock_set_stocks_time((uint32_t)time(NULL));

    /* 构造与旧后端一致的 JSON 快照保存 SD，供离线恢复 */
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc.createNestedArray("stocks");
    for (int i = 0; i < g_quote_n; i++) {
        JsonObject o = arr.createNestedObject();
        o["name"]  = g_quotes[i].name;
        o["value"] = g_quotes[i].value;
        o["pct"]   = g_quotes[i].pct;
        o["up"]    = g_quotes[i].up;
    }
    String json;
    serializeJson(doc, json);
    data_cache_save("stocks", json.c_str());

    const char *names[4] = {0, 0, 0, 0};
    float values[4] = {0, 0, 0, 0};
    float pcts[4]   = {0, 0, 0, 0};
    bool  ups[4]    = {false, false, false, false};
    for (int i = 0; i < g_quote_n; i++) {
        names[i]  = g_quotes[i].name;
        values[i] = g_quotes[i].value;
        pcts[i]   = g_quotes[i].pct;
        ups[i]    = g_quotes[i].up;
    }

    ui_clock_set_stocks(names, values, pcts, ups);
    if (Lvgl_lock(2000)) {
        ui_clock_update_stocks();
        Lvgl_unlock();
    } else {
        Serial.println("[stocks] Lvgl_lock 超时，行情 UI 未刷新");
    }
    Serial.printf("[stocks] OK: %d quotes\n", g_quote_n);
}

/* 从 SD 缓存恢复行情（直连失败时调用；无缓存则保持现状） */
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
