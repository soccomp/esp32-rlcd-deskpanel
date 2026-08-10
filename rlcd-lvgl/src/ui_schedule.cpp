#include "ui_schedule.h"
#include "ui.h"              // cn_label()
#include "ui_clock.h"       // ui_clock_sync_meeting()：fetch/解析后刷新主页"我的下一场会议"
#include "lvgl_bsp.h"       // Lvgl_lock / Lvgl_unlock
#include "lv_font_chinese_18.h"
#include "lv_font_chinese_14.h"   // 14px 紧凑副文本（标题仍用 18px）
#include "schedule_data.h" // SCHEDULE_BUNDLED_JSON 离线兜底数据
#include <Arduino.h>
#include <WiFi.h>           // WiFi.status()
#include <WiFiUdp.h>
#include <ESPmDNS.h>        // mDNS 解析 Mac 主机名
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include "data_cache.h"      // 离线缓存：fetch 成功保存 JSON 快照到 SD

/* ============================================================
 *  全局状态
 * ============================================================ */
static Meeting *   g_meetings = nullptr;   // 数组本体驻留 PSRAM
static uint8_t     g_meeting_count = 0;
static bool        g_schedule_loaded = false;
static uint8_t     g_filter_mode = 0;      // 0=全部, 1=我的会议

static lv_obj_t *  g_sched_indicator = nullptr;  // 顶部指示栏文本
static lv_obj_t *  g_sched_scroll    = nullptr;  // 卡片容器

/* —— 分页状态：反射屏翻页比滚动更干脆，每屏 3 张，KEY 下页 / BOOT 上页 —— */
static const uint8_t PER_PAGE = 3;
static lv_obj_t *  g_card_list[MAX_MEETINGS];     // 当前筛选下渲染出的卡片（按序）
static uint8_t     g_card_count = 0;              // 当前筛选下卡片总数
static uint8_t     g_page_idx   = 0;              // 当前页（0 基）
static uint8_t     g_page_total = 1;              // 总页数

/* —— mDNS 解析缓存 —— */
static String g_resolved_url = "";       // mDNS 解析成功后拼出的 URL
static String g_resolved_base = "";      // mDNS 解析成功后拼出的 base（http://ip:port）
static bool   g_mdns_ok = false;          // 上次 mDNS 解析是否成功
static uint32_t g_mdns_resolve_ms = 0;    // 上次 mDNS 解析时间戳

/* ============================================================
 *  PSRAM 字符串辅助
 * ============================================================ */
static char *ps_strdup(const char *s)
{
    if (!s) return nullptr;
    size_t n = strlen(s) + 1;
    char *p = (char *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p) memcpy(p, s, n);
    return p;
}

static void free_meetings(void)
{
    if (!g_meetings) return;
    for (uint8_t i = 0; i < g_meeting_count; i++) {
        Meeting *m = &g_meetings[i];
        if (m->date)      { heap_caps_free(m->date);      m->date = nullptr; }
        if (m->weekday)   { heap_caps_free(m->weekday);   m->weekday = nullptr; }
        if (m->time)      { heap_caps_free(m->time);      m->time = nullptr; }
        if (m->location)  { heap_caps_free(m->location);  m->location = nullptr; }
        if (m->title)     { heap_caps_free(m->title);     m->title = nullptr; }
        if (m->host)      { heap_caps_free(m->host);      m->host = nullptr; }
        if (m->attendees) { heap_caps_free(m->attendees); m->attendees = nullptr; }
        if (m->organizer) { heap_caps_free(m->organizer); m->organizer = nullptr; }
    }
    g_meeting_count = 0;
}

/* ============================================================
 *  JSON 解析（ArduinoJson v6）
 * ============================================================ */
static void parse_schedule_json(const char *json)
{
    free_meetings();

    // 16KB 文档缓冲（驻留内部 DRAM，解析后即释放；字符串已拷入 PSRAM）
    DynamicJsonDocument doc(16384);
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("Schedule JSON parse error: %s\n", err.c_str());
        g_schedule_loaded = false;
        return;
    }

    JsonArray arr = doc["meetings"];
    uint8_t n = 0;
    for (JsonObject m : arr) {
        if (n >= MAX_MEETINGS) break;
        Meeting *mt = &g_meetings[n];
        mt->date          = ps_strdup(m["date"]          | "");
        mt->weekday       = ps_strdup(m["weekday"]       | "");
        mt->time          = ps_strdup(m["time"]          | "");
        mt->location      = ps_strdup(m["location"]      | "");
        mt->title         = ps_strdup(m["title"]         | "");
        mt->host          = ps_strdup(m["host"]          | "");
        mt->attendees     = ps_strdup(m["attendees"]     | "");
        mt->organizer     = ps_strdup(m["organizer"]     | "");
        mt->is_video_conf = m["is_video_conf"]  | false;
        mt->is_secret_conf= m["is_secret_conf"] | false;
        mt->is_my_meeting = m["is_my_meeting"]  | false;
        n++;
    }
    g_meeting_count = n;
    g_schedule_loaded = (n > 0);
    Serial.printf("Schedule loaded: %d meetings (mine filter ready)\n", n);

    /* 注：天气现由 ESP32 直连 Open-Meteo（weather_client.cpp）负责，
     * 不再依赖本后端的 weather 字段，因此这里不再解析 weather。
     * 会议数据仍走本后端（或离线兜底）。 */
}

/* ============================================================
 *  网络拉取（mDNS 动态发现后端）
 * ============================================================ */

/* 通过 mDNS 解析 Mac 主机名，返回 base（http://ip:port）。
 * 解析成功缓存 5 分钟，失败时立即重试。schedule / xfeed 共用。 */
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
        int p = fb.indexOf(SCHEDULE_API_PATH);
        if (p > 0) fb = fb.substring(0, p);
        return fb;
    }
}

/* 拼出完整 schedule API URL（base + 路径） */
static String resolve_api_url(void)
{
    return resolve_api_base() + String(SCHEDULE_API_PATH);
}

void fetch_schedule_data(void)
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Schedule fetch skipped: WiFi not connected");
        return;
    }

    String url = resolve_api_url();
    Serial.printf("[schedule] Fetching: %s\n", url.c_str());

    HTTPClient http;
    http.begin(url);
    http.setTimeout(8000);
    http.setUserAgent("ESP32-RLCD-DeskPanel");

    int code = http.GET();
    if (code == HTTP_CODE_OK) {
        String payload = http.getString();
        data_cache_save("schedule", payload.c_str());   // 离线快照
        parse_schedule_json(payload.c_str());
        // 重建 UI（fetch 内部自管锁，调用方请勿外持锁）
        if (Lvgl_lock(2000)) {
            ui_schedule_rebuild();
            ui_clock_sync_meeting();     // 同步主页"我的下一场会议"
            Lvgl_unlock();
        } else {
            Serial.println("[schedule] Lvgl_lock 超时，会议 UI 未刷新");
        }
    } else {
        Serial.printf("Schedule fetch failed: HTTP %d\n", code);
        // mDNS 解析成功但请求失败时，标记需要重新解析（Mac 可能换了 IP）
        if (g_mdns_ok) {
            g_mdns_resolve_ms = 0;  // 下次 fetch 时强制重新解析
        }
    }
    http.end();
}

/* 轻量探测后端可达性：GET /api/schedule，只看状态码不解析。
 * 成功返回 true（电脑开机/后端恢复）；失败返回 false 且静默（不刷日志）。 */
bool probe_backend(void)
{
    if (WiFi.status() != WL_CONNECTED) return false;

    String url = resolve_api_url();
    HTTPClient http;
    http.begin(url);
    http.setTimeout(3000);        /* 探测：3 秒快速超时，失败静默 */
    http.setUserAgent("ESP32-RLCD-DeskPanel");
    int code = http.GET();
    http.end();
    return (code == HTTP_CODE_OK);
}

/* ============================================================
 *  股票指数行情：mDNS 找本机后端 GET /api/stocks，解析后刷新主页右上卡
 *  返回 {"stocks":[{name,value,pct,up}x3], "updated"}。
 *  交易时段每 20 分钟由 main.cpp 调用。
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

void ui_schedule_set_api_url(const char *url)
{    // 兼容旧接口：直接覆盖 resolved_url
    if (url && strlen(url) > 0) {
        g_resolved_url = String(url);
        g_mdns_ok = true;
        g_mdns_resolve_ms = millis();
    }
}

/* ============================================================
 *  LVGL 卡片渲染
 *
 *  反射屏上看日程的第一诉求是"几点、在哪、是什么会"。参会名单
 *  在纸质表中很重要，但在 400px 宽屏幕上会挤掉这三个信息，因此
 *  主界面只保留主持/承办；"我的会议"、视频、涉密改为醒目标记。
 * ============================================================ */

/* 构建单张会议卡片：时间左栏 + 标题/地点右栏，固定高度以保证一屏三场。 */
static lv_obj_t *build_card(lv_obj_t *parent, Meeting *m)
{
    bool mine = m->is_my_meeting;

    /* 卡片改为横向两栏，让时间成为最先被扫到的信息。 */
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, 78);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(card, 5, 0);
    lv_obj_set_style_pad_column(card, 7, 0);
    lv_obj_set_style_radius(card, 3, 0);

    if (mine) {
        /* 个人会议使用唯一的反色，避免与普通会议混淆。 */
        lv_obj_set_style_bg_color(card, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_white(), 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_text_color(card, lv_color_white(), 0);
    } else {
        /* 普通会议：浅底深字 + 细黑线框 */
        lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(card, lv_color_black(), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_text_color(card, lv_color_black(), 0);
    }

    /* 左侧时间栏 */
    lv_obj_t *when = lv_obj_create(card);
    lv_obj_remove_style_all(when);
    lv_obj_set_size(when, 58, 66);
    lv_obj_clear_flag(when, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(when, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(when, 1, 0);
    lv_obj_set_style_border_color(when, mine ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_pad_right(when, 6, 0);

    /* 只显示开始时间；日程的结束时间留给完整表，不占用主界面。 */
    char start_time[8] = "待定";
    if (m->time && m->time[0]) {
        size_t n = 0;
        while (m->time[n] && n < sizeof(start_time) - 1 &&
               ((m->time[n] >= '0' && m->time[n] <= '9') || m->time[n] == ':')) {
            start_time[n] = m->time[n];
            n++;
        }
        if (n > 0) start_time[n] = '\0';
    }
    lv_obj_t *time = cn_label(when, start_time);
    lv_obj_set_style_text_font(time, &lv_font_chinese_18, 0);
    lv_obj_set_width(time, lv_pct(100));
    lv_obj_set_style_text_align(time, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(time, LV_LABEL_LONG_DOT);
    lv_obj_align(time, LV_ALIGN_TOP_MID, 0, 0);

    char date[8] = "";
    int year = 0, month = 0, day_num = 0;
    if (m->date && sscanf(m->date, "%d-%d-%d", &year, &month, &day_num) == 3)
        snprintf(date, sizeof(date), "%02d-%02d", month, day_num);
    else if (m->date)
        snprintf(date, sizeof(date), "%s", m->date);
    lv_obj_t *date_label = cn_label(when, date);
    lv_obj_set_style_text_font(date_label, &lv_font_chinese_14, 0);
    lv_obj_set_width(date_label, lv_pct(100));
    lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_CENTER, 0);
    /* 反射屏上的浅灰副文本在实机上几乎不可见，日期和星期必须全黑。 */
    lv_obj_set_style_text_opa(date_label, LV_OPA_COVER, 0);
    lv_obj_align(date_label, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *day = cn_label(when, m->weekday ? m->weekday : "");
    lv_obj_set_style_text_font(day, &lv_font_chinese_14, 0);
    lv_obj_set_width(day, lv_pct(100));
    lv_obj_set_style_text_align(day, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(day, LV_OPA_COVER, 0);
    lv_obj_align(day, LV_ALIGN_TOP_MID, 0, 42);

    /* 右侧信息栏：标题可占两行，地点独立显示，避免被长标题吞没。 */
    lv_obj_t *content = lv_obj_create(card);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_height(content, 66);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, 1, 0);

    lv_obj_t *title = cn_label(content, m->title ? m->title : "(无标题)");
    lv_obj_set_style_text_font(title, &lv_font_chinese_18, 0);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_height(title, 37);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);

    char place[150];
    snprintf(place, sizeof(place), "地点  %s", (m->location && m->location[0]) ? m->location : "待定");
    lv_obj_t *location = cn_label(content, place);
    lv_obj_set_style_text_font(location, &lv_font_chinese_14, 0);
    lv_obj_set_width(location, lv_pct(100));
    lv_label_set_long_mode(location, LV_LABEL_LONG_DOT);

    char note[150];
    const char *tag = mine ? "我的会议" : (m->is_video_conf ? "视频会议" : (m->is_secret_conf ? "涉密会议" : ""));
    if (tag[0]) {
        snprintf(note, sizeof(note), "%s%s%s", tag,
                 (m->host && m->host[0]) ? " · 主持 " : "",
                 (m->host && m->host[0]) ? m->host : "");
    } else {
        snprintf(note, sizeof(note), "主持 %s · 承办 %s",
                 (m->host && m->host[0]) ? m->host : "—",
                 (m->organizer && m->organizer[0]) ? m->organizer : "—");
    }
    lv_obj_t *meta = cn_label(content, note);
    lv_obj_set_style_text_font(meta, &lv_font_chinese_14, 0);
    lv_obj_set_width(meta, lv_pct(100));
    lv_label_set_long_mode(meta, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_opa(meta, mine ? LV_OPA_80 : LV_OPA_60, 0);

    return card;
}

/* ============================================================
 *  公开接口
 * ============================================================ */

void ui_schedule_init(lv_obj_t *parent)
{
    /* 数组本体分配在 PSRAM（只需一次） */
    if (!g_meetings) {
        g_meetings = (Meeting *)heap_caps_malloc(MAX_MEETINGS * sizeof(Meeting),
                                                 MALLOC_CAP_SPIRAM);
        if (g_meetings) memset(g_meetings, 0, MAX_MEETINGS * sizeof(Meeting));
    }

    /* 离线兜底：上电先用内置 schedule 数据渲染，连上后端后再用实时数据覆盖。
     * 这样即便当前网络无法访问后端（如 AP 隔离），会议页也能直接显示真实日程。 */
    parse_schedule_json(SCHEDULE_BUNDLED_JSON);

    /* 顶部指示栏（固定，不随卡片滚动） */
    lv_obj_t *ind = lv_obj_create(parent);
    lv_obj_remove_style_all(ind);
    lv_obj_set_size(ind, SCREEN_W, 26);
    lv_obj_align(ind, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_flex_flow(ind, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ind, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_set_style_pad_hor(ind, 6, 0);

    g_sched_indicator = cn_label(ind, "全部会议 0");
    lv_obj_set_style_text_font(g_sched_indicator, &lv_font_chinese_14, 0);
    lv_obj_t *hint = cn_label(ind, "左:下页/筛选  右:上页/退出");
    lv_obj_set_style_text_font(hint, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_opa(hint, LV_OPA_50, 0);

    /* 可滚动卡片容器 */
    g_sched_scroll = lv_obj_create(parent);
    lv_obj_remove_style_all(g_sched_scroll);
    lv_obj_set_size(g_sched_scroll, SCREEN_W, SCREEN_H - STATUS_BAR_H - 26);
    lv_obj_align(g_sched_scroll, LV_ALIGN_TOP_LEFT, 0, 26);
    lv_obj_set_flex_flow(g_sched_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_sched_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(g_sched_scroll, 4, 0);
    lv_obj_set_style_pad_row(g_sched_scroll, 4, 0);   // 卡片之间的纵向间隔（本 LVGL 构建不支持 margin，用容器 pad_row 实现）
    lv_obj_add_flag(g_sched_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_sched_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_sched_scroll, LV_SCROLLBAR_MODE_AUTO);

    ui_schedule_rebuild();

    /* 同步主页"我的下一场会议"预告区 */
    ui_clock_sync_meeting();
}

/* 前向声明：翻页窗口应用（定义见下方） */
static void ui_schedule_apply_page(void);

void ui_schedule_rebuild(void)
{
    if (!g_sched_scroll) return;

    lv_obj_clean(g_sched_scroll);   // 清空旧卡片
    g_card_count = 0;

    if (g_meeting_count == 0) {
        if (g_sched_indicator)
            lv_label_set_text_fmt(g_sched_indicator, "会议数据加载中…");
        cn_label(g_sched_scroll, g_schedule_loaded ? "无匹配会议" : "会议数据加载中…");
        return;
    }

    /* 按当前筛选构建全部卡片，存入 g_card_list（顺序即显示顺序） */
    for (uint8_t i = 0; i < g_meeting_count && g_card_count < MAX_MEETINGS; i++) {
        Meeting *m = &g_meetings[i];
        if (g_filter_mode == 1 && !m->is_my_meeting) continue;
        g_card_list[g_card_count++] = build_card(g_sched_scroll, m);
    }

    if (g_card_count == 0) {
        if (g_sched_indicator)
            lv_label_set_text_fmt(g_sched_indicator, "我的会议 0");
        cn_label(g_sched_scroll, "无我的会议");
        return;
    }

    /* 计算总页数并把当前页收敛到合法范围 */
    g_page_total = (g_card_count + PER_PAGE - 1) / PER_PAGE;
    if (g_page_idx >= g_page_total) g_page_idx = g_page_total - 1;
    ui_schedule_apply_page();
}

/* 显示当前页窗口内的卡片，其余隐藏；并刷新指示栏页码 */
static void ui_schedule_apply_page(void)
{
    for (uint8_t i = 0; i < g_card_count; i++) {
        bool vis = (i >= g_page_idx * PER_PAGE) && (i < (g_page_idx + 1) * PER_PAGE);
        if (vis) lv_obj_clear_flag(g_card_list[i], LV_OBJ_FLAG_HIDDEN);
        else     lv_obj_add_flag(g_card_list[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (g_sched_indicator) {
        const char *mode = (g_filter_mode == 1) ? "我的会议" : "全部会议";
        lv_label_set_text_fmt(g_sched_indicator, "%s %d · 第%d/%d页",
                              mode, g_card_count, g_page_idx + 1, g_page_total);
    }
}

void ui_schedule_next_page(void)
{
    if (g_page_total <= 1 || g_card_count == 0) return;
    g_page_idx = (g_page_idx + 1) % g_page_total;
    ui_schedule_apply_page();
}

void ui_schedule_prev_page(void)
{
    if (g_page_total <= 1 || g_card_count == 0) return;
    g_page_idx = (g_page_idx + g_page_total - 1) % g_page_total;
    ui_schedule_apply_page();
}

void ui_schedule_toggle_filter(void)
{
    g_filter_mode = (g_filter_mode == 0) ? 1 : 0;
    g_page_idx = 0;               // 切换筛选后回到首页
    ui_schedule_rebuild();
}

uint8_t ui_schedule_get_filter(void)
{
    return g_filter_mode;
}

bool ui_schedule_has_data(void)
{
    return g_schedule_loaded;
}

/* 取"我的下一场会议"：未来最近的一场（is_my_meeting==true 且开始时间 >= 当前） */
bool ui_schedule_get_next_my_meeting(char *out_time, size_t tsz,
                                     char *out_loc,  size_t lsz,
                                     char *out_title, size_t ttl_sz)
{
    if (out_time)   out_time[0]   = '\0';
    if (out_loc)    out_loc[0]    = '\0';
    if (out_title)  out_title[0]  = '\0';
    if (!g_meetings || g_meeting_count == 0) return false;

    time_t now = time(nullptr);
    time_t best = 0;
    int    best_idx = -1;

    for (uint8_t i = 0; i < g_meeting_count; i++) {
        Meeting *m = &g_meetings[i];
        if (!m->is_my_meeting) continue;

        int y = 0, mo = 0, d = 0;
        if (sscanf(m->date ? m->date : "", "%d-%d-%d", &y, &mo, &d) != 3) continue;

        int hh = 0, mm = 0;
        if (sscanf(m->time ? m->time : "", "%d:%d", &hh, &mm) != 2) continue;

        struct tm ct;
        memset(&ct, 0, sizeof(ct));
        ct.tm_year = y - 1900;
        ct.tm_mon  = mo - 1;
        ct.tm_mday = d;
        ct.tm_hour = hh;
        ct.tm_min  = mm;
        ct.tm_sec  = 0;
        ct.tm_isdst = -1;
        time_t c = mktime(&ct);
        if (c == (time_t)-1) continue;
        if (c < now) continue;                 // 仅取未来

        if (best_idx < 0 || c < best) { best = c; best_idx = (int)i; }
    }

    if (best_idx < 0) return false;

    Meeting *m = &g_meetings[best_idx];
    if (out_time)  snprintf(out_time,  tsz, "%s", m->time     ? m->time     : "");
    if (out_loc)   snprintf(out_loc,   lsz, "%s", m->location ? m->location : "");
    if (out_title) snprintf(out_title, ttl_sz, "%s", m->title ? m->title : "");
    return true;
}

/* 取"我的下一场会议"的开始 epoch（复用上面的过滤逻辑，仅返回 epoch） */
bool ui_schedule_get_next_my_meeting_epoch(time_t *out_epoch)
{
    if (out_epoch) *out_epoch = 0;
    if (!g_meetings || g_meeting_count == 0) return false;

    time_t now = time(nullptr);
    time_t best = 0;
    int    best_idx = -1;

    for (uint8_t i = 0; i < g_meeting_count; i++) {
        Meeting *m = &g_meetings[i];
        if (!m->is_my_meeting) continue;

        int y = 0, mo = 0, d = 0;
        if (sscanf(m->date ? m->date : "", "%d-%d-%d", &y, &mo, &d) != 3) continue;

        int hh = 0, mm = 0;
        if (sscanf(m->time ? m->time : "", "%d:%d", &hh, &mm) != 2) continue;

        struct tm ct;
        memset(&ct, 0, sizeof(ct));
        ct.tm_year = y - 1900;
        ct.tm_mon  = mo - 1;
        ct.tm_mday = d;
        ct.tm_hour = hh;
        ct.tm_min  = mm;
        ct.tm_sec  = 0;
        ct.tm_isdst = -1;
        time_t c = mktime(&ct);
        if (c == (time_t)-1) continue;
        if (c < now) continue;                 // 仅取未来

        if (best_idx < 0 || c < best) { best = c; best_idx = (int)i; }
    }

    if (best_idx < 0) return false;
    if (out_epoch) *out_epoch = best;
    return true;
}
