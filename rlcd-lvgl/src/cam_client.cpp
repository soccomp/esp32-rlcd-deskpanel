/*
 * cam_client.cpp —— RLCD 侧摄像头客户端实现
 *
 * 工作流：
 *   [WiFi 已连] -> mDNS 解析 "esp32cam"（缓存 5min，失败强制重解析）
 *              -> HTTP GET http://<ip>/capture（单帧 JPEG）
 *              -> TJpg_Decoder 解码 -> 逐像素 <0x7fff 阈值 -> 1bit 双缓冲
 *  帧率锁 ~2fps；连续失败退避到 5s，避免空转刷日志。
 */

#include "cam_client.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace {

/* ---- 参数 ---- */
constexpr int   JPEG_BUF_SIZE = 96 * 1024;   /* VGA 640x480 quality18 单帧 <60KB，留余量 */

/* ★ VGA 适配：摄像头出 640x480，RLCD 裁切中央 320x240 显示（等效 2x 变焦） */
constexpr int   CAM_SRC_W = 640;
constexpr int   CAM_SRC_H = 480;
constexpr int   CROP_X0   = (CAM_SRC_W - CAM_W) / 2;
constexpr int   CROP_Y0   = (CAM_SRC_H - CAM_H) / 2;
constexpr uint32_t FRAME_INTERVAL_MS = 500;  /* ~2fps，反射屏足够 */
constexpr uint32_t RETRY_BACKOFF_MS  = 5000; /* 连续失败后的退避间隔 */

/* ---- 双缓冲（PSRAM，init 时分配一次，任务运行期零分配） ---- */
uint8_t  *g_buf[2] = { nullptr, nullptr };
uint8_t  *g_jpeg    = nullptr;
volatile uint8_t  g_read_idx  = 0;   /* LVGL 侧可读 */
volatile uint8_t  g_write_idx = 1;
volatile bool     g_new_frame = false;
volatile uint32_t g_seq       = 0;
bool      g_ready = false;

/* ---- mDNS 解析缓存（照 ui_schedule.cpp 模式） ---- */
String    g_url;
bool      g_mdns_ok = false;
uint32_t  g_mdns_ms = 0;
bool      g_mdns_init_done = false;

void set_bit(uint8_t *buf, int x, int y, bool black)
{
    if (x < 0 || x >= CAM_W || y < 0 || y >= CAM_H) return;
    size_t idx = (size_t)y * CAM_W + x;
    if (black) {
        buf[idx >> 3] |= (uint8_t)(0x80 >> (idx & 7));
    } else {
        buf[idx >> 3] &= (uint8_t)~(0x80 >> (idx & 7));
    }
}

/* Bayer 8x8 ordered dithering —— 比 4x4 颗粒细腻一倍，灰阶过渡更顺滑。
 * 流程：RGB565→8bit→BT.601 亮度→200% 对比度→与 8x8 Bayer 阈值比较。
 * 参数经 Mac PIL 模拟验证（用户选定：8x8 + 对比 200%）。 */
static const uint8_t BAYER8[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 },
};

bool tjpg_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
    uint8_t *dst = g_buf[g_write_idx];
    for (uint16_t yy = 0; yy < h; yy++) {
        int16_t gy = y + yy;
        if (gy < CROP_Y0 || gy >= CROP_Y0 + CAM_H) continue;
        const uint8_t *bayer_row = BAYER8[(gy - CROP_Y0) & 7];
        for (uint16_t xx = 0; xx < w; xx++) {
            int16_t gx = x + xx;
            if (gx < CROP_X0 || gx >= CROP_X0 + CAM_W) continue;

            uint16_t px = bitmap[(size_t)yy * w + xx];
            /* RGB565 -> 8bit: R5<<3, G6<<2, B5<<3 */
            uint16_t r8 = ((px >> 11) & 0x1F) << 3;
            uint16_t g8 = ((px >>  5) & 0x3F) << 2;
            uint16_t b8 = ( px        & 0x1F) << 3;
            /* BT.601 亮度 0..255 */
            int32_t lum = (int32_t)((r8 * 77u + g8 * 150u + b8 * 29u) >> 8);
            /* 对比度 200%: (lum-128)*2 + 128 = lum*2 - 128 */
            lum = lum * 2 - 128;
            if (lum < 0)   lum = 0;
            if (lum > 255) lum = 255;
            /* 8x8 Bayer 阈值 -128..+124 */
            int16_t thr = (int16_t)(bayer_row[(gx - CROP_X0) & 7] * 4) - 128;
            set_bit(dst, gx - CROP_X0, gy - CROP_Y0, lum < thr);
        }
    }
    return true;   /* 继续解码 */
}

/* ★ 摄像头帧走 Mac 后端代理（/api/camframe）
 * 背景：某些企业 AP（如 BTWIFI6 系列）对 ESP32 出站 TCP 做 per-device RST，
 * RLCD→摄像头直连（esp32cam:80）不稳定；Mac 是电脑不受限。后端后台线程
 * 每 500ms 抓摄像头 /capture 缓存，RLCD 从这里拉帧——RLCD→Mac 与 Mac→
 * 摄像头两条路径都稳定，彻底绕开 RST。
 * 主机解析：mDNS 查 Mac 主机名（缓存 5min），失败回退固定 IP。
 * 真实主机名/IP 在本地 src/local_config.h（被 .gitignore 忽略，不提交 GitHub）。 */
#if __has_include("local_config.h")
#include "local_config.h"
#endif
#ifndef CAM_PROXY_HOST
#define CAM_PROXY_HOST "YOUR_MAC_HOSTNAME"    /* Mac mDNS 主机名（无 .local） */
#endif
#ifndef CAM_PROXY_FALLBACK
#define CAM_PROXY_FALLBACK "YOUR_MAC_IP"       /* Mac 局域网 IP */
#endif
static const char g_cam_proxy_host[]     = CAM_PROXY_HOST;      /* Mac mDNS 主机名（无 .local） */
static const char g_cam_proxy_fallback[] = CAM_PROXY_FALLBACK;  /* Mac 局域网 IP */
static const int  CAM_PROXY_PORT         = 8100;
static const char CAM_PROXY_PATH[]       = "/api/camframe";

String resolve_cam_url(void)
{
    if (!g_mdns_init_done) {
        if (MDNS.begin("esp32-rlcd")) {
            g_mdns_init_done = true;
        } else {
            Serial.println("[cam] MDNS.begin failed, will retry");
        }
    }

    uint32_t now = millis();
    /* 缓存 5 分钟 */
    if (g_mdns_ok && (now - g_mdns_ms) < 300000UL && g_url.length() > 0) {
        return g_url;
    }

    if (g_mdns_init_done) {
        Serial.printf("[cam] mDNS resolving \"%s\" ...\n", g_cam_proxy_host);
        IPAddress ip = MDNS.queryHost(g_cam_proxy_host, 3000);
        if (ip != INADDR_NONE && ip != IPAddress(0, 0, 0, 0)) {
            g_url = "http://" + ip.toString() + ":" + String(CAM_PROXY_PORT) + CAM_PROXY_PATH;
            g_mdns_ok = true;
            g_mdns_ms = now;
            Serial.printf("[cam] proxy resolved: %s -> %s\n",
                          g_cam_proxy_host, ip.toString().c_str());
            return g_url;
        }
        g_mdns_ok = false;
        Serial.println("[cam] proxy mDNS resolution failed");
    }

    /* mDNS 失败 -> 固定 IP 兜底 */
    g_url = String("http://") + g_cam_proxy_fallback + ":" + String(CAM_PROXY_PORT) + CAM_PROXY_PATH;
    g_mdns_ok = true;
    g_mdns_ms = now;
    Serial.printf("[cam] proxy fallback to %s\n", g_cam_proxy_fallback);
    return g_url;
}

/* 从 URL 拆出 host / port / path（http://ip:8100/api/camframe） */
static void url_split(const String &url, String &host, uint16_t &port, String &path)
{
    host = url;
    host.replace("http://", "");
    int slash = host.indexOf('/');
    if (slash >= 0) {
        path = host.substring(slash);
        host = host.substring(0, slash);
    } else {
        path = "/";
    }
    int colon = host.indexOf(':');
    port = 80;
    if (colon >= 0) {
        port = (uint16_t)host.substring(colon + 1).toInt();
        host = host.substring(0, colon);
    }
}

/* ★ 短连接拉取一帧 JPEG 到 g_jpeg，返回字节数；失败返回 0。
 * 用短连接（每次新建、用完即关）而不是 keep-alive 长连接——
 * 原因：BTWIFI6-169148 会对 ESP32 的长连接 TCP 会话做定期清理
 * （几十秒 ≈ NAT 会话超时），长连接跑到点就被 RST（errno 113）死循环。
 * 短连接每次都是新会话，AP 清不掉；目标是 Mac 后端（uvicorn 线程池
 * 并发强，每 500ms 一个短连接毫无压力，不像摄像头 WebServer 单槽位）。 */
size_t fetch_jpeg(const String &url)
{
    if (url.length() == 0) return 0;

    String host, path;
    uint16_t port;
    url_split(url, host, port, path);
    if (path.length() == 0) path = "/";

    /* 每次新建连接（短连接） */
    WiFiClient conn;
    if (!conn.connect(host.c_str(), port)) {
        Serial.printf("[cam] TCP connect %s:%u failed\n", host.c_str(), port);
        if (g_mdns_ok) g_mdns_ms = 0;   /* 可能后端 IP 变了，强制重解析 */
        return 0;
    }
    conn.setTimeout(3000);

    /* 发送请求（HTTP/1.1，Connection: close 让服务端响应后即断） */
    conn.print("GET ");
    conn.print(path);
    conn.print(" HTTP/1.1\r\nHost: ");
    conn.print(host);
    conn.print("\r\nConnection: close\r\n\r\n");

    /* 读响应头，解析 Content-Length */
    int content_length = -1;
    uint32_t t0 = millis();
    String line;
    while (millis() - t0 < 3000) {
        if (conn.available()) {
            line = conn.readStringUntil('\n');
            if (line.startsWith("HTTP/")) {
                /* 状态行，检查 200（uvicorn 返回 "HTTP/1.1 200 OK"） */
                if (line.indexOf(" 200") < 0) {
                    Serial.printf("[cam] HTTP status: %s", line.c_str());
                    if (g_mdns_ok) g_mdns_ms = 0;
                    return 0;
                }
            } else if (line == "\r") {
                break;                       /* 空行 = 头结束 */
            } else {
                /* 大小写不敏感匹配 Content-Length（uvicorn 返回小写） */
                int cl = line.indexOf(':');
                if (cl > 0) {
                    String key = line.substring(0, cl);
                    key.toLowerCase();
                    if (key == "content-length") {
                        content_length = line.substring(cl + 1).toInt();
                    }
                }
            }
        } else {
            if (!conn.connected()) {
                if (g_mdns_ok) g_mdns_ms = 0;
                return 0;
            }
            delay(2);
        }
    }

    if (content_length <= 0 || content_length > JPEG_BUF_SIZE) {
        Serial.printf("[cam] bad Content-Length: %d\n", content_length);
        if (g_mdns_ok) g_mdns_ms = 0;
        return 0;
    }

    /* 精确读取 body（Content-Length 字节） */
    size_t total = 0;
    t0 = millis();
    while (total < (size_t)content_length && millis() - t0 < 4000) {
        size_t avail = conn.available();
        if (avail > 0) {
            size_t n = conn.read(g_jpeg + total,
                                 min(avail, (size_t)content_length - total));
            total += n;
        } else {
            if (!conn.connected()) break;
            delay(2);
        }
    }

    if (total != (size_t)content_length) {
        Serial.printf("[cam] short read %u/%d\n", (unsigned)total, content_length);
        if (g_mdns_ok) g_mdns_ms = 0;
        return 0;
    }

    if (total < 4) return 0;
    if (g_jpeg[0] != 0xFF || g_jpeg[1] != 0xD8) {
        Serial.println("[cam] not a JPEG stream");
        if (g_mdns_ok) g_mdns_ms = 0;
        return 0;
    }
    return total;
}

void cam_task(void *arg)
{
    uint32_t fail_ms = 0;
    uint32_t fail_total = 0;   /* 连续失败总次数：用于网络自愈 */
    for (;;) {
        uint32_t start = millis();

        if (WiFi.status() == WL_CONNECTED) {
            String url = resolve_cam_url();
            size_t len = fetch_jpeg(url);
            if (len > 0) {
                /* 解码 -> 灰度化 -> 双缓冲交换 */
                memset(g_buf[g_write_idx], 0, CAM_BYTES);
                TJpgDec.setJpgScale(1);
                TJpgDec.setSwapBytes(true);
                TJpgDec.setCallback(tjpg_output);
                TJpgDec.drawJpg(0, 0, g_jpeg, len);

                g_read_idx  = g_write_idx;
                g_write_idx = 1 - g_read_idx;
                g_new_frame = true;
                g_seq++;
                g_ready = true;
                fail_ms = 0;
                fail_total = 0;
                Serial.printf("[cam] frame %lu rendered (%u bytes)\n",
                              (unsigned long)g_seq, (unsigned)len);
            } else {
                fail_ms = (fail_ms == 0) ? FRAME_INTERVAL_MS : RETRY_BACKOFF_MS;
                fail_total++;
            }
        }

        /* ★ 网络自愈：BTWIFI6 对 ESP32 出站 TCP 做间歇性 RST，会把 lwIP
         * TCP 栈打坏（errno 113 死循环）。连续失败超阈值后主动重启 WiFi
         * 重新关联 AP 清栈；再不行直接重启系统（无人值守兜底）。 */
        if (fail_total >= 24) {              /* 24 次 ≈ 2 分钟连续失败 */
            Serial.println("[cam] self-heal: restarting WiFi (RST-stuck stack)");
            WiFi.disconnect(true);
            delay(200);
            WiFi.reconnect();
            fail_total = 0;
            g_mdns_ms = 0;                   /* 强制重新解析后端 */
        } else if (fail_total >= 48) {       /* WiFi 重启仍无效 -> 整机重启 */
            Serial.println("[cam] self-heal: WiFi restart failed, rebooting...");
            delay(200);
            esp_restart();
        }

        uint32_t elapsed = millis() - start;
        uint32_t wait = (fail_ms > 0) ? fail_ms : FRAME_INTERVAL_MS;
        if (elapsed < wait) vTaskDelay(pdMS_TO_TICKS(wait - elapsed));
    }
}

} // namespace

void cam_client_init(void)
{
    if (g_jpeg) return;   /* 防重复初始化 */

    g_buf[0] = static_cast<uint8_t *>(
        heap_caps_malloc(CAM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_buf[1] = static_cast<uint8_t *>(
        heap_caps_malloc(CAM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_jpeg = static_cast<uint8_t *>(
        heap_caps_malloc(JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    if (!g_buf[0] || !g_buf[1] || !g_jpeg) {
        Serial.println("[cam] PSRAM alloc failed, cam disabled");
        return;
    }
    memset(g_buf[0], 0, CAM_BYTES);
    memset(g_buf[1], 0, CAM_BYTES);

    /* 栈 8192：VGA 640x480 全尺寸解码（TJpgDec 内部 MCU 缓冲 + 裁切）需要大栈 */
    xTaskCreate(cam_task, "camfetch", 8192, nullptr, 4, nullptr);
    Serial.println("[cam] cam_client started (2fps, QVGA mono)");
}

bool cam_client_get_frame(uint8_t *bitmap_out, uint32_t *seq_out)
{
    if (!g_new_frame || !g_buf[g_read_idx] || !bitmap_out) return false;
    memcpy(bitmap_out, g_buf[g_read_idx], CAM_BYTES);
    if (seq_out) *seq_out = g_seq;
    return true;
}

bool cam_client_has_frame(void)
{
    return g_ready;
}
