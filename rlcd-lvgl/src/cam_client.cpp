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
#include <esp_system.h>
#include <stdarg.h>
#include <string.h>

namespace {

/* ---- 参数 ---- */
constexpr int   JPEG_BUF_SIZE = 96 * 1024;   /* VGA 640x480 quality18 单帧 <60KB，留余量 */

/* ★ VGA 适配：摄像头出 640x480，RLCD 裁切中央 320x240 显示（等效 2x 变焦） */
constexpr int   CAM_SRC_W = 640;
constexpr int   CAM_SRC_H = 480;
constexpr int   CROP_X0   = (CAM_SRC_W - CAM_W) / 2;
constexpr int   CROP_Y0   = (CAM_SRC_H - CAM_H) / 2;
constexpr uint32_t FRAME_INTERVAL_MS = 1500;  /* ~0.7fps；BTWIFI6 对 ESP32 高频短连接 RST，降频提高通过率 */
constexpr uint32_t RETRY_BACKOFF_MS  = 2000;  /* 连续失败后的退避间隔（失败多为瞬时 RST，短退避快速重试） */

/* ---- 灰度双缓冲（PSRAM，init 时分配一次，任务运行期零分配） ----
 * RLCD-003：解码只输出 8bit 灰度强度（不在此处抖动），1-bit 转换
 * 由 UI 侧在最终显示尺寸上完成（先缩放后抖动）。 */
uint8_t  *g_gray[2] = { nullptr, nullptr };
volatile uint8_t  g_read_idx  = 0;   /* LVGL 侧可读 */
volatile uint8_t  g_write_idx = 1;
volatile uint32_t g_seq       = 0;
volatile uint32_t g_pub_ms    = 0;   /* 最近成功发布时刻（新鲜度） */
bool      g_ready = false;

#ifdef CAM_USB_INPUT
/* ---- RLCD-004.2：RX 消费与解码解耦（S3 双核） ----
 * 根因：单线程里"解码(1~1.5s)期间不读 USB RX"会让 bridge 写入的帧+命令积压，
 * 32KB CDC 队列在解码窗口内被灌满溢出 -> 命令乱序/迟到、帧 CRC 错（bad++）。
 * 修复：新增 rx_task（core0 高优先）持续 pump USB-CDC：帧间隙命令即时喂
 * cmd_feed，完整帧写入 PSRAM 帧槽；cam_task 只做解码发布。
 * 命令处理与解码彻底解耦 -> 任意时刻命令 ≤50ms 被处理，RX 队列永不积压。
 * 8-12 审核修正：帧槽用 free_q / ready_q 双队列实现真正的 producer/consumer
 * slot 所有权——cam_task 解码期间该 slot 不在任何队列中，rx_task 不可能写入，
 * 彻底消除"解码中被覆盖"的竞争（不再依赖 counting semaphore 计数判断可重用）。
 * 8-12 崩溃根治：TinyUSB CDC 的 read(RX) 与 printf(TX) 跨核并发访问会卡死
 * rx_task -> 占满 core0 -> TWDT 重启（命令到达时崩，周期性黑洞）。用全局
 * g_serial_lock 让所有 Serial 读/写互斥（rx_task 读与各任务 printf 串行化）。 */
#define FRAME_SLOTS 4
uint8_t *g_jpeg[FRAME_SLOTS] = { nullptr };
uint8_t *g_rx_buf = nullptr;                        /* rx_task 独立接收缓冲（不属于 slot） */
volatile size_t g_ring_len[FRAME_SLOTS] = { 0 };    /* 每 slot 帧长（仅持有者读写） */
QueueHandle_t g_free_q  = nullptr;                  /* 空闲 slot 索引（rx_task 领取） */
QueueHandle_t g_ready_q = nullptr;                  /* 已就绪待解码 slot 索引（cam_task 领取） */
SemaphoreHandle_t g_serial_lock = nullptr;          /* Serial 读/写全局互斥（8-12） */
#else
uint8_t *g_jpeg[1] = { nullptr };                    /* WiFi 模式单槽 */
#endif

/* 串口批量读（一次锁内最多读 cap 字节）。锁超时 5ms：拿不到锁返回 0，
 * rx_task 绝不因锁阻塞（防"printf 持锁 -> read 等待 -> RX 积压 -> 背压死锁"链）。
 * 返回实际读到的字节数。 */
static size_t serial_read_batch(uint8_t *dst, size_t cap)
{
#ifdef CAM_USB_INPUT
    if (cap == 0) return 0;
    if (g_serial_lock && !xSemaphoreTake(g_serial_lock, pdMS_TO_TICKS(5))) return 0;
    size_t n = 0;
    while (n < cap && Serial.available()) dst[n++] = (uint8_t)Serial.read();
    if (g_serial_lock) xSemaphoreGive(g_serial_lock);
    return n;
#else
    size_t n = 0;
    while (n < cap && Serial.available()) dst[n++] = (uint8_t)Serial.read();
    return n;
#endif
}

/* ---- 诊断（限速打印用） ---- */
volatile uint32_t g_last_sum = 0;    /* 最近发布帧的简单 checksum（字节和） */
volatile uint32_t g_diag_cnt = 0;    /* 发布计数（每 N 帧打印一次） */

#ifdef CAM_USB_INPUT
/* ---- RLCD-004：USB-CDC 文本命令（与视频帧复用同一条 CDC 通道） ----
 * M1 手势识别程序经桥接下发 ASCII 行："PAGE:HOME\n" / "PAGE:GUITAR\n" / "PAGE:CAMERA\n"。
 * 采集时机：只在帧头搜索态（S_H0）喂字节，JPEG 载荷在 S_BODY 消费，不会被误解析。
 * 抗噪：只接受 [A-Z:_] 字符，遇到任何其它字节立即清空累积，随机二进制拼不出完整命令。
 * 线程：本解析跑在 cam_task（非 LVGL 任务），只登记请求；实际切页由 loop()
 *       在 Lvgl_lock 保护下执行（项目约束：非 LVGL 任务不得直接动 LVGL 对象）。 */
constexpr uint8_t CMD_MAX = 16;
char     g_cmd_buf[CMD_MAX + 1];
uint8_t  g_cmd_len = 0;
volatile int8_t g_page_req = -1;      /* -1 = 无待处理请求 */

void cmd_feed(uint8_t c)
{
    if (c == '\n' || c == '\r') {
        if (g_cmd_len > 0) {
            g_cmd_buf[g_cmd_len] = '\0';
            int8_t page = -1;
            if      (strcmp(g_cmd_buf, "PAGE:HOME")   == 0) page = 0;
            else if (strcmp(g_cmd_buf, "PAGE:GUITAR") == 0) page = 1;
            else if (strcmp(g_cmd_buf, "PAGE:CAMERA") == 0) page = 2;
            if (page >= 0) {
                g_page_req = page;
                /* 8-12 审核修正：此处只登记请求，**不 printf、不 ACK**。
                 * (1) ACK 语义必须是"实际页面状态已确认"，由 main.cpp 在切页确认后回；
                 * (2) rx_task 内 printf 与 cam_task/main 的 printf 三方并发访问
                 *     TinyUSB CDC 会卡死 rx_task -> 占满 core0 -> TWDT 重启（实测）。
                 *     日志统一由 main/cam_task 输出，rx_task 纯读 RX。 */
            }
            g_cmd_len = 0;
        }
        return;
    }
    if ((c >= 'A' && c <= 'Z') || c == ':' || c == '_') {
        if (g_cmd_len < CMD_MAX) g_cmd_buf[g_cmd_len++] = (char)c;
        else                     g_cmd_len = 0;   /* 超长 = 噪声，丢弃重来 */
    } else {
        g_cmd_len = 0;
    }
}
#endif  /* CAM_USB_INPUT */

/* ---- mDNS 解析缓存（照 stocks_client.cpp 模式） ---- */
String    g_url;
bool      g_mdns_ok = false;
uint32_t  g_mdns_ms = 0;
bool      g_mdns_init_done = false;

/* 解码回调：RGB565 -> BT.601 亮度(0..255) -> 写入灰度缓冲（中央 320x240 裁剪）。
 * RLCD-003 Defect D 修复：此处**只出灰度强度**，不做任何 1-bit 抖动——
 * 抖动/阈值化必须由 UI 侧在最终显示尺寸上完成（先缩放后抖动）。 */
bool tjpg_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
    uint8_t *dst = g_gray[g_write_idx];
    for (uint16_t yy = 0; yy < h; yy++) {
        int16_t gy = y + yy;
        if (gy < CROP_Y0 || gy >= CROP_Y0 + CAM_H) continue;
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
            if (lum < 0)   lum = 0;
            if (lum > 255) lum = 255;
            dst[(size_t)(gy - CROP_Y0) * CAM_W + (gx - CROP_X0)] = (uint8_t)lum;
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
        cam_client_log("[cam] mDNS resolving \"%s\" ...\n", g_cam_proxy_host);
        IPAddress ip = MDNS.queryHost(g_cam_proxy_host, 3000);
        if (ip != INADDR_NONE && ip != IPAddress(0, 0, 0, 0)) {
            g_url = "http://" + ip.toString() + ":" + String(CAM_PROXY_PORT) + CAM_PROXY_PATH;
            g_mdns_ok = true;
            g_mdns_ms = now;
            cam_client_log("[cam] proxy resolved: %s -> %s\n",
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
    cam_client_log("[cam] proxy fallback to %s\n", g_cam_proxy_fallback);
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
        cam_client_log("[cam] TCP connect %s:%u failed\n", host.c_str(), port);
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
                    cam_client_log("[cam] HTTP status: %s", line.c_str());
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
        cam_client_log("[cam] bad Content-Length: %d\n", content_length);
        if (g_mdns_ok) g_mdns_ms = 0;
        return 0;
    }

    /* 精确读取 body（Content-Length 字节） */
    size_t total = 0;
    t0 = millis();
    while (total < (size_t)content_length && millis() - t0 < 4000) {
        size_t avail = conn.available();
        if (avail > 0) {
            size_t n = conn.read(g_jpeg[0] + total,
                                 min(avail, (size_t)content_length - total));
            total += n;
        } else {
            if (!conn.connected()) break;
            delay(2);
        }
    }

    if (total != (size_t)content_length) {
        cam_client_log("[cam] short read %u/%d\n", (unsigned)total, content_length);
        if (g_mdns_ok) g_mdns_ms = 0;
        return 0;
    }

    if (total < 4) return 0;
    if (g_jpeg[0][0] != 0xFF || g_jpeg[0][1] != 0xD8) {
        Serial.println("[cam] not a JPEG stream");
        if (g_mdns_ok) g_mdns_ms = 0;
        return 0;
    }
    /* RLCD-003：校验 EOI（FFD9）——只有 SOI 的半帧/损坏帧不得当作有效 JPEG */
    if (g_jpeg[0][total - 2] != 0xFF || g_jpeg[0][total - 1] != 0xD9) {
        cam_client_log("[cam] missing EOI (tail %02X %02X)\n", g_jpeg[0][total-2], g_jpeg[0][total-1]);
        if (g_mdns_ok) g_mdns_ms = 0;
        return 0;
    }
    return total;
}

/* ★ USB-CDC pump：收帧 + 帧间隙命令解析一体化（RLCD-004.2 重构）。
 * 帧协议与摄像头串口直传一致：AA 55 5A A5 | len(2B BE) | JPEG | crc16(2B BE, len+data 累加)
 * 与旧 fetch_usb_frame 的差异：不再"只在读帧阶段跑"，而是可随时以时间分片被
 * cam_task 调用（收帧/等待阶段都调）。命令字节到达后最多等一个 ~50ms 分片即被
 * cmd_feed 处理并回 ACK，消除"解码/等待期间命令积压 -> 背压 -> 延迟放大"的根因。
 * 状态机 static 保持跨调用（帧中途截断也能续传）；成功返回 JPEG 长度，超时返回 0。
 * USB-CDC 全双工：RX 收帧与 Serial.printf 日志(TX)互不干扰。 */
static size_t usb_pump_frame(uint32_t ms)
{
    /* 8-13 修复：st 必须是 static！rx_task 以 100ms 分片调用本函数，
     * 15KB 大帧跨多个窗口——若 st 为局部变量，每次调用从 S_H0 重置，
     * 帧剩余字节被当命令丢弃（1KB 小帧单窗口读完所以一直没暴露；
     * 大帧必丢 -> RLCD 收不到帧 -> cam offline）。frame_len/got/crc_calc
     * 已是 static，st 保持后分片续接即可完整收帧。 */
    static enum { S_H0, S_H1, S_H2, S_H3, S_LENH, S_LENL, S_BODY, S_CRCH, S_CRCL, S_DONE } st = S_H0;
    static size_t frame_len = 0, got = 0;
    static uint16_t crc_calc = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < ms) {
        uint8_t tmp[64];
        size_t n = serial_read_batch(tmp, sizeof(tmp));
        for (size_t i = 0; i < n; i++) {
            uint8_t c = tmp[i];
            switch (st) {
                case S_H0:
                    if (c == 0xAA) st = S_H1;
                    else           cmd_feed(c);   /* RLCD-004：帧间隙的 ASCII 命令 */
                    break;
                case S_H1: st = (c == 0x55) ? S_H2 : S_H0; break;
                case S_H2: st = (c == 0x5A) ? S_H3 : S_H0; break;
                case S_H3: st = (c == 0xA5) ? S_LENH : S_H0; break;
                case S_LENH: frame_len = ((size_t)c) << 8; st = S_LENL; break;
                case S_LENL:
                    frame_len |= c;
                    if (frame_len == 0 || frame_len > JPEG_BUF_SIZE) { st = S_H0; break; }
                    crc_calc = (uint16_t)(frame_len & 0xFFFF);
                    got = 0;
                    st = S_BODY;
                    break;
                case S_BODY:
                    if (got < frame_len) g_rx_buf[got++] = c;
                    crc_calc = (uint16_t)(crc_calc + c);
                    if (got >= frame_len) st = S_CRCH;
                    break;
                case S_CRCH:
                    st = (c == (uint8_t)(crc_calc >> 8)) ? S_CRCL : S_H0;
                    break;
                case S_CRCL:
                    st = (c == (uint8_t)(crc_calc & 0xFF)) ? S_DONE : S_H0;
                    break;
                default: st = S_H0; break;
            }
            if (st == S_DONE) { st = S_H0; return frame_len; }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return 0;   /* 分片内无完整帧 */
}

#ifdef CAM_USB_INPUT
/* ★ RLCD-004.2：USB RX 消费任务（core0 高优先）。
 * 持续 pump USB-CDC：帧间隙命令即时喂 cmd_feed（回 ACK）；完整帧存入 PSRAM
 * 环形帧槽并 give 信号量给解码任务。与解码任务解耦 -> 命令处理不等待解码，
 * RX 队列不再因解码窗口积压溢出。 */
void rx_task(void *arg)
{
    for (;;) {
        size_t len = usb_pump_frame(100);
        if (len > 0) {
            /* 8-13：命令帧（bridge 封装：HEAD+len+"CMD:PAGE:X"+crc，len<=64）。
             * 与视频帧同协议 -> 状态机按 len 精确消费，命令永不与帧尾竞争。
             * 命令帧不走视频解码，直接登记页面请求（main 任务切页确认后 ACK）。 */
            if (len <= 64 && memcmp(g_rx_buf, "CMD:", 4) == 0) {
                /* 8-13 修复：g_rx_buf 是二进制帧缓冲，命令帧体后无 '\0'——
                 * strcmp 会读越界，残留字节不为 0 时匹配失败（命令不处理、
                 * 无 ACK，实测仅偶发成功）。先补终止符再解析。 */
                g_rx_buf[len] = '\0';
                const char *body = (const char *)g_rx_buf + 4;
                int8_t page = -1;
                if      (strcmp(body, "PAGE:HOME")   == 0) page = 0;
                else if (strcmp(body, "PAGE:GUITAR") == 0) page = 1;
                else if (strcmp(body, "PAGE:CAMERA") == 0) page = 2;
                if (page >= 0) g_page_req = page;
                continue;   /* 命令帧不进入视频帧槽 */
            }
            /* 从 free_q 领取一个空闲 slot；无空闲 = 所有 slot 正被 cam_task
             * 占用（解码中/待解码）-> 丢弃本帧（绝不覆盖在用 slot）。
             * slot 所有权：rx 从 free_q 取 -> memcpy -> 放 ready_q 交还给 cam。 */
            int slot = -1;
            if (xQueueReceive(g_free_q, &slot, 0) == pdTRUE) {
                memcpy(g_jpeg[slot], g_rx_buf, len);
                g_ring_len[slot] = len;
                xQueueSend(g_ready_q, &slot, 0);
            }
        }
        /* 8-12：显式让出，防 core0 的 idle 任务被饿死触发任务看门狗(TWDT)重启。
         * 降频（pump 100ms + 让出 5ms）：降低双核高负载下的峰值功耗/CPU 占用，
         * 缓解 USB hub 供电不足导致的芯片复位（reset reason 0）。命令延迟仍 ≤150ms。 */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
#endif  /* CAM_USB_INPUT */

void cam_task(void *arg)
{
    uint32_t fail_ms = 0;
    uint32_t fail_total = 0;   /* 连续失败总次数：用于网络自愈（不重置，两阶段各触发一次） */
    for (;;) {
        uint32_t start = millis();

#ifdef CAM_USB_INPUT
        /* USB 全链路视频模式：帧由 rx_task 收进 PSRAM 帧槽（free/ready 队列），
         * 这里从 ready_q 领取 slot 解码。解码期间该 slot 不在任何队列中，
         * rx_task 无法写入 —— 真正的 producer/consumer 所有权，无覆盖竞争。 */
        int slot = -1;
        if (xQueueReceive(g_ready_q, &slot, pdMS_TO_TICKS(500)) == pdTRUE) {
            size_t len = g_ring_len[slot];
            uint8_t *buf = g_jpeg[slot];
            /* 解码 -> 灰度 -> 发布（仅解码成功且完整） */
            memset(g_gray[g_write_idx], 0, CAM_GRAY_BYTES);
            TJpgDec.setJpgScale(1);
            TJpgDec.setSwapBytes(true);
            TJpgDec.setCallback(tjpg_output);
            JRESULT jr = TJpgDec.drawJpg(0, 0, buf, len);
            /* 解码结束（无论成败）立即归还 slot 给 rx_task */
            xQueueSend(g_free_q, &slot, 0);
            if (jr == JDR_OK) {
                g_read_idx  = g_write_idx;
                g_write_idx = 1 - g_read_idx;
                g_seq++;
                g_pub_ms = millis();
                g_ready = true;
                fail_ms = 0;
                fail_total = 0;
                uint32_t sum = 0;
                const uint8_t *rp = g_gray[g_read_idx];
                for (uint32_t i = 0; i < CAM_GRAY_BYTES; i += 97) sum += rp[i];
                bool changed = (sum != g_last_sum);
                g_last_sum = sum;
                if (++g_diag_cnt >= 48) {
                    g_diag_cnt = 0;
                    cam_client_log("[cam] USB pub seq=%u len=%u sum=%u %s\n",
                                  (unsigned)g_seq, (unsigned)len,
                                  (unsigned)sum, changed ? "CHANGED" : "same");
                }
            } else {
                cam_client_log("[cam] USB decode failed: %d\n", (int)jr);
                fail_ms = (fail_ms == 0) ? FRAME_INTERVAL_MS : RETRY_BACKOFF_MS;
                fail_total++;
            }
        } else {
            fail_ms = (fail_ms == 0) ? FRAME_INTERVAL_MS : RETRY_BACKOFF_MS;
            fail_total++;
        }
#else
        if (WiFi.status() == WL_CONNECTED) {
            String url = resolve_cam_url();
            size_t len = fetch_jpeg(url);
            if (len > 0) {
                /* 解码 -> 灰度 -> 发布（仅在解码成功且校验通过后） */
                memset(g_gray[g_write_idx], 0, CAM_GRAY_BYTES);
                TJpgDec.setJpgScale(1);
                TJpgDec.setSwapBytes(true);
                TJpgDec.setCallback(tjpg_output);
                JRESULT jr = TJpgDec.drawJpg(0, 0, g_jpeg[0], len);

                if (jr == JDR_OK) {
                    g_read_idx  = g_write_idx;
                    g_write_idx = 1 - g_read_idx;
                    g_seq++;
                    g_pub_ms = millis();
                    g_ready = true;
                    fail_ms = 0;
                    fail_total = 0;

                    /* 诊断（限速）：字节和 checksum 判断帧是否真的在变 */
                    uint32_t sum = 0;
                    const uint8_t *rp = g_gray[g_read_idx];
                    for (uint32_t i = 0; i < CAM_GRAY_BYTES; i += 97) sum += rp[i];  /* 抽样省 CPU */
                    bool changed = (sum != g_last_sum);
                    g_last_sum = sum;
                    if (++g_diag_cnt >= 48) {   /* 每 8 帧打印一次 */
                        g_diag_cnt = 0;
                        cam_client_log("[cam] pub seq=%u len=%u sum=%u %s\n",
                                      (unsigned)g_seq, (unsigned)len,
                                      (unsigned)sum, changed ? "CHANGED" : "same");
                    }
                } else {
                    cam_client_log("[cam] decode failed: %d\n", (int)jr);
                    fail_ms = (fail_ms == 0) ? FRAME_INTERVAL_MS : RETRY_BACKOFF_MS;
                    fail_total++;
                }
            } else {
                fail_ms = (fail_ms == 0) ? FRAME_INTERVAL_MS : RETRY_BACKOFF_MS;
                fail_total++;
            }
        }
#endif  /* CAM_USB_INPUT */

#ifdef CAM_USB_INPUT
        /* USB 全链路视频模式不依赖 WiFi：跳过 WiFi 自愈/重启逻辑 */
        (void)fail_total;
#else
        /* ★ 网络自愈（RLCD-003 Defect B 修复）：BTWIFI6 对 ESP32 出站 TCP 做间歇性
         * RST 会把 lwIP TCP 栈打坏（errno 113 死循环）。两阶段计数不重置：
         *  - 第 24 次失败：重启 WiFi 关联（disconnect(false) 不关 radio，再 reconnect）
         *  - 第 48 次失败（WiFi 重启仍无效）：整机重启兜底 */
        if (fail_total == 24) {
            Serial.println("[cam] self-heal-1: WiFi reconnect (RST-stuck stack)");
            WiFi.disconnect(false);   /* false：不关闭 radio，reconnect 才能生效 */
            delay(200);
            WiFi.reconnect();
            g_mdns_ms = 0;            /* 强制重新解析后端 */
        } else if (fail_total == 48) {
            Serial.println("[cam] self-heal-2: WiFi restart failed, rebooting...");
            delay(200);
            esp_restart();
        }
#endif  /* CAM_USB_INPUT */

        uint32_t elapsed = millis() - start;
        uint32_t wait = (fail_ms > 0) ? fail_ms : FRAME_INTERVAL_MS;
        if (elapsed < wait) vTaskDelay(pdMS_TO_TICKS(wait - elapsed));
    }
}

} // namespace

/* 带锁 printf（8-12）：rx_task 的 Serial 读与所有任务的 printf 串行化，
 * 防 TinyUSB CDC 跨核并发访问导致 rx_task 卡死（TWDT 重启）。main.cpp 的
 * [cmd] 日志与 cam_client 内部日志统一走这里。置于 namespace 外供 main 调用。 */
void cam_client_log(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len <= 0) return;
#ifdef CAM_USB_INPUT
    /* 锁超时 5ms：拿不到（rx_task 正持锁读 RX）就丢这条日志，绝不阻塞。
     * 非阻塞写：TinyUSB TX 缓冲不足时直接丢弃——printf 内部阻塞是崩溃根因
     * （TX 满持锁 -> rx_task 读被拖 -> RX 积压 -> bridge 写阻塞 -> 死锁链 TWDT）。 */
    if (g_serial_lock && !xSemaphoreTake(g_serial_lock, pdMS_TO_TICKS(5))) return;
    if (Serial.availableForWrite() >= (size_t)len) Serial.print(buf);
    if (g_serial_lock) xSemaphoreGive(g_serial_lock);
#else
    Serial.print(buf);
#endif
}

void cam_client_init(void)
{
    if (g_jpeg[0]) return;   /* 防重复初始化 */

    g_gray[0] = static_cast<uint8_t *>(
        heap_caps_malloc(CAM_GRAY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_gray[1] = static_cast<uint8_t *>(
        heap_caps_malloc(CAM_GRAY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    bool alloc_ok = (g_gray[0] && g_gray[1]);
#ifdef CAM_USB_INPUT
    for (int i = 0; i < FRAME_SLOTS && alloc_ok; i++) {
        g_jpeg[i] = static_cast<uint8_t *>(
            heap_caps_malloc(JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        alloc_ok = alloc_ok && (g_jpeg[i] != nullptr);
    }
    g_rx_buf = static_cast<uint8_t *>(
        heap_caps_malloc(JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    alloc_ok = alloc_ok && (g_rx_buf != nullptr);
#else
    g_jpeg[0] = static_cast<uint8_t *>(
        heap_caps_malloc(JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    alloc_ok = alloc_ok && (g_jpeg[0] != nullptr);
#endif

    if (!alloc_ok) {
        Serial.println("[cam] PSRAM alloc failed, cam disabled");
        return;
    }
    memset(g_gray[0], 0, CAM_GRAY_BYTES);
    memset(g_gray[1], 0, CAM_GRAY_BYTES);

#ifdef CAM_USB_INPUT
    /* ★ USB 全链路模式关键：Arduino 层 USBCDC rx_queue 默认仅 256 字节，
     * 15KB 的 JPEG 帧到达即溢出丢弃（"CDC RX Overflow"，_onRX 里 xQueueSend
     * 失败即丢）。必须调大——setRxBufferSize 运行中调用会重建队列并保留
     * 旧数据。TinyUSB 驱动层 64B 缓冲不用动（满了会 NAK 主机暂停传输，不丢）。 */
    Serial.setRxBufferSize(32768);
    Serial.println("[cam] USB input mode, CDC rx queue = 32KB");

    /* RLCD-004.2：rx_task(core0, 优先6) 持续消费 RX + 解析命令；
     * cam_task(core1 由调度器安排, 优先4) 只做解码发布。命令与解码解耦。
     * 8-12 审核修正：free_q/ready_q 双队列实现 slot 所有权（见 rx_task/cam_task）。 */
    g_free_q  = xQueueCreate(FRAME_SLOTS, sizeof(int));
    g_ready_q = xQueueCreate(FRAME_SLOTS, sizeof(int));
    g_serial_lock = xSemaphoreCreateMutex();   /* 8-12：Serial 读/写全局互斥 */
    for (int i = 0; i < FRAME_SLOTS; i++) {
        int idx = i;
        xQueueSend(g_free_q, &idx, 0);
    }
    xTaskCreatePinnedToCore(rx_task, "camrx", 4096, nullptr, 6, nullptr, 0);
    xTaskCreatePinnedToCore(cam_task, "camfetch", 8192, nullptr, 4, nullptr, 1);
#else
    /* 栈 8192：VGA 640x480 全尺寸解码（TJpgDec 内部 MCU 缓冲 + 裁切）需要大栈 */
    xTaskCreate(cam_task, "camfetch", 8192, nullptr, 4, nullptr);
#endif
    Serial.println("[cam] cam_client started (2fps, QVGA grayscale)");
    /* 8-12 诊断：复位原因（1=POWERON 3=SW 4=PANIC 5=INT_WDT 6=TASK_WDT 7=WDT
     * 9=BROWNOUT），区分崩溃类型（PANIC=程序异常/断言；WDT=任务饿死） */
    cam_client_log("[cam] reset reason: %d\n", (int)esp_reset_reason());
    cam_client_log("[cam] heap free: %u\n", (unsigned)ESP.getFreeHeap());
}

bool cam_client_get_frame(uint8_t *gray_out, uint32_t *seq_out)
{
    if (!g_ready || !g_gray[g_read_idx] || !gray_out) return false;
    memcpy(gray_out, g_gray[g_read_idx], CAM_GRAY_BYTES);
    if (seq_out) *seq_out = g_seq;
    return true;
}

bool cam_client_has_frame(void)
{
    return g_ready;
}

bool cam_client_is_fresh(void)
{
    if (!g_ready) return false;
    uint32_t now = millis();
    uint32_t age = (now >= g_pub_ms) ? (now - g_pub_ms) : 0;
    return (age < 4000UL);   /* 4s 内未发布新帧 = 陈旧/离线 */
}

int8_t cam_client_take_page_cmd(void)
{
#ifdef CAM_USB_INPUT
    int8_t p = g_page_req;
    if (p >= 0) g_page_req = -1;   /* 取走即清空，同一命令只执行一次 */
    return p;
#else
    return -1;                     /* WiFi 模式无 USB 命令通道 */
#endif
}

/* RLCD-004.2 审核修正：ACK 只在**实际页面状态已确认**后发送（main.cpp 调用）。
 * 调用时机必须是：ui_goto_page 成功且 ui_get_current_page()==目标页，或已在目标页。
 * Lvgl_lock 失败/切页未生效时不得调用，让 M1 超时重发。 */
void cam_client_send_ack(int8_t page)
{
#ifdef CAM_USB_INPUT
    const char *name = (page == 0) ? "PAGE:HOME" :
                       (page == 1) ? "PAGE:GUITAR" :
                       (page == 2) ? "PAGE:CAMERA" : nullptr;
    if (name) {
        /* 8-13 修复：ACK 必须可靠送达——用阻塞写，绝不用非阻塞丢弃。
         * 根因：RLCD 日志多（[cam]48帧/次 + [cam-ui]6帧/次 + 天气 + [cmd]）
         * 使 TX 持续满，非阻塞 printf 全部丢弃（含 ACK）-> M1 收不到 ACK
         * 超时重发（30 条注入实测仅 46% ACK；手动测试无 TX 竞争则 4/4）。
         * ACK 为短行（14B），TinyUSB TX 缓冲常有空位，阻塞时间极短。 */
        if (g_serial_lock && !xSemaphoreTake(g_serial_lock, pdMS_TO_TICKS(100))) return;
        Serial.print("ACK:");
        Serial.print(name);
        Serial.print("\n");
        if (g_serial_lock) xSemaphoreGive(g_serial_lock);
    }
#else
    (void)page;
#endif
}
