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
#include <lwip/sockets.h>       /* socket()/close()：net_diag 探测剩余 fd 配额 */
/* main.cpp 提供：彻底关 radio 再重连（数据面死而关联在时唯一有效的自愈手段） */
extern void wifi_hard_restart(void);
#include <lwip/priv/tcp_priv.h> /* tcp_active_pcbs / tcp_tw_pcbs 等全局链表：net_diag 统计 PCB 占用 */

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

/* 方案B：页面切换请求/ACK 状态（移出 CAM_USB_INPUT 守卫，WiFi 直连模式也用）。
 * g_page_req：命令任务登记、main loop 取走执行（取走即清 -1）。
 * g_ack_pending：main loop 切页确认后置位，cmd_server_task 在同一 TCP 连接上回 ACK。 */
volatile int8_t g_page_req   = -1;    /* 待处理页面请求（-1 无） */
volatile int8_t g_ack_pending = -1;   /* 待回 ACK 的页（-1 无） */
/* 9-4：M1 键盘吉他控制（GTR:STRUM/CHORD/GROUP）。请求与 ACK 各一条通道，
 * 与页面通道相互独立；USB(rx_task) 与 WiFi(cmd_server_task) 双入口登记，
 * main loop 统一取走执行（取走即清 -1，同一命令只执行一次）。 */
volatile int8_t g_gtr_req        = -1;  /* 0=STRUM 1=CHORD 2=GROUP（-1 无） */
volatile int8_t g_gtr_ack_pending = -1; /* WiFi 客户端待回 GTR ACK（-1 无） */
WiFiServer g_cmd_server(8771);       /* M1 手势程序经 WiFi 下发 PAGE 命令 */
WiFiClient g_cmd_client;              /* 当前连上的手势客户端（用于回 ACK） */

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
            } else if (strcmp(g_cmd_buf, "AI:ALIVE") == 0) {
                /* Phase 2 P1：M1 手势进程心跳，仅记录时刻（不输出避免刷日志） */
                ai_heartbeat_now();
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

/* ★ 方案B：摄像头帧直连 camera（不再经 Mac 后端代理）
 * 背景：原担心 RLCD→camera 直连会被 BTWIFI6 的 per-device RST 搞死才绕 Mac；
 * 但 RST 是**出网**方向，RLCD→camera 是同 AP 下的 LAN 本地 TCP，不受影响；
 * 且 802.11ax→n 切换已根治 RST。camera 稳定服务 /capture（mDNS "esp32cam"，端口
 * 80），AP 侧 MAC 绑定给 camera 保留 192.168.100.199。RLCD 直接拉帧即可。 */
static const char g_cam_host[]     = "esp32cam";          /* camera mDNS（无 .local，回退用） */
static const char g_cam_proxy[]    = "192.168.100.198";   /* M1 Mac：finger_page_control 本地帧代理(:8780) */
static const int  CAM_PROXY_PORT   = 8780;
static const char g_cam_fallback[] = "192.168.100.199";   /* 摄像头（M1 宕机时 RLCD 直连，独占不挤） */
static const int  CAM_PORT         = 80;
static const char CAM_PATH[]       = "/capture";

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

    /* ★ 方案B+：优先从 M1 本地代理(:8780)取帧。M1 是摄像头唯一外部消费者
     * （ESP32-CAM 单线程 HTTP server 只扛得住一个），拉到帧后在本地代理喂 RLCD，
     * 摄像头不再被 RLCD+M1 双拉挤爆（实测双消费者 60% 超时）。 */
    WiFiClient t;
    if (t.connect(g_cam_proxy, CAM_PROXY_PORT)) {
        t.stop();
        g_url = String("http://") + g_cam_proxy + ":" + String(CAM_PROXY_PORT) + CAM_PATH;
        g_mdns_ok = true;
        g_mdns_ms = now;
        cam_client_log("[cam] use M1 proxy %s:%d\n", g_cam_proxy, CAM_PROXY_PORT);
        return g_url;
    }

    /* 回退：摄像头直连（mDNS 优先，失败用 AP 保留 IP）。
     * M1 宕机时走这里——此时无手势，RLCD 独占摄像头也能显示。 */
    if (g_mdns_init_done) {
        cam_client_log("[cam] mDNS resolving \"%s\" ...\n", g_cam_host);
        IPAddress ip = MDNS.queryHost(g_cam_host, 3000);
        if (ip != INADDR_NONE && ip != IPAddress(0, 0, 0, 0)) {
            g_url = "http://" + ip.toString() + ":" + String(CAM_PORT) + CAM_PATH;
            g_mdns_ok = true;
            g_mdns_ms = now;
            cam_client_log("[cam] camera resolved: %s -> %s\n",
                          g_cam_host, ip.toString().c_str());
            return g_url;
        }
        g_mdns_ok = false;
        Serial.println("[cam] camera mDNS resolution failed");
    }

    /* mDNS 失败 -> 固定 IP 兜底（AP 保留地址，稳定） */
    g_url = String("http://") + g_cam_fallback + ":" + String(CAM_PORT) + CAM_PATH;
    g_mdns_ok = true;
    g_mdns_ms = now;
    cam_client_log("[cam] camera fallback to %s\n", g_cam_fallback);
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

/* fetch_jpeg 连接失败标志：true=出站 TCP 连接失败（本地 lwIP 栈被 RST 夯死），
 * false=连接成功但取不到帧（远端无帧/503）。供 cam_task 区分自愈触发条件，
 * 避免把"远端无帧"误判为"本地栈故障"导致 WiFi 震荡。 */
static bool g_cam_conn_fail = false;

/* ★ 短连接拉取一帧 JPEG 到 g_jpeg，返回字节数；失败返回 0。
 * 用短连接（每次新建、用完即关）而不是 keep-alive 长连接——
 * 原因：BTWIFI6-169148 会对 ESP32 的长连接 TCP 会话做定期清理
 * （几十秒 ≈ NAT 会话超时），长连接跑到点就被 RST（errno 113）死循环。
 * 短连接每次都是新会话，AP 清不掉；目标是 Mac 后端（uvicorn 线程池
 * 并发强，每 500ms 一个短连接毫无压力，不像摄像头 WebServer 单槽位）。 */
size_t fetch_jpeg(const String &url)
{
    g_cam_conn_fail = false;
    if (url.length() == 0) return 0;

    String host, path;
    uint16_t port;
    url_split(url, host, port, path);
    if (path.length() == 0) path = "/";

    /* 每次新建连接（短连接） */
    WiFiClient conn;
    if (!conn.connect(host.c_str(), port)) {
        cam_client_log("[cam] TCP connect %s:%u failed\n", host.c_str(), port);
        g_cam_conn_fail = true;        /* 本地出站连接失败：PCB 池可能已被 TIME_WAIT 榨干 */
        if (g_mdns_ok) g_mdns_ms = 0;   /* 可能后端 IP 变了，强制重解析 */
        return 0;
    }
    conn.setTimeout(3000);

    /* ★ 8-18 修正：这里曾试图用 SO_LINGER(l_linger=0) 让 close() 发 RST 跳过
     * TIME_WAIT，但 ESP-IDF 的 lwIP 默认 LWIP_SO_LINGER=0，setsockopt 直接返回
     * errno 109 (ENOPROTOOPT)，只在日志刷 "setSocketOption(): fail on 50"，
     * 毫无效果 —— 已移除。且 TIME_WAIT 本身并非楔死主因：lwIP 的 tcp_alloc()
     * 在 PCB 池耗尽时会调 tcp_kill_timewait() 自动回收最老的 TIME_WAIT PCB。
     * 真凶改由 net_diag() 打印 PCB/socket 实测占用来定位，不再靠猜。 */

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

#ifdef CAM_USB_INPUT
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

#endif  /* CAM_USB_INPUT */

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
                if (page >= 0) {
                    g_page_req = page;
                } else if (strcmp(body, "GTR:STRUM") == 0) {
                    g_gtr_req = 0;       /* 9-4：M1 键盘拨弦 */
                } else if (strcmp(body, "GTR:CHORD") == 0) {
                    g_gtr_req = 1;       /* 下一个和弦 */
                } else if (strcmp(body, "GTR:GROUP") == 0) {
                    g_gtr_req = 2;       /* 切练习组 OPEN/7TH */
                } else if (strcmp(body, "AI:ALIVE") == 0) {
                    /* Phase 2 P1：M1 手势进程心跳（bridge 封装 CMD 帧透传） */
                    ai_heartbeat_now();
                }
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

/* 方案B：WiFi 命令服务（端口 8771）。M1 finger_page_control 经 TCP 下发
 * "PAGE:HOME|GUITAR|CAMERA\n"，本任务解析并登记 g_page_req（与 USB 版 cmd_feed
 * 同语义）；cam_client_send_ack() 置 g_ack_pending 后，本任务在同一 TCP 连接上
 * 写回 "ACK:PAGE:X\n"。所有 socket I/O 集中在本任务，避免跨任务写同一 socket。 */
static void cmd_server_task(void *arg)
{
    (void)arg;
    char line[24];
    int  li = 0;
    for (;;) {
        if (!g_cmd_client || !g_cmd_client.connected()) {
            WiFiClient c = g_cmd_server.available();
            if (c) { g_cmd_client = c; li = 0; cam_client_log("[cmd] client connected\n"); }
        }
        if (g_cmd_client && g_cmd_client.connected()) {
            while (g_cmd_client.available()) {
                char ch = (char)g_cmd_client.read();
                if (ch == '\n' || ch == '\r') {
                    if (li > 0) {
                        line[li] = '\0';
                        int8_t page = -1;
                        if      (strcmp(line, "PAGE:HOME")   == 0) page = 0;
                        else if (strcmp(line, "PAGE:GUITAR") == 0) page = 1;
                        else if (strcmp(line, "PAGE:CAMERA") == 0) page = 2;
                        if (page >= 0) {
                            g_page_req = page;
                        } else if (strcmp(line, "GTR:STRUM") == 0) {
                            g_gtr_req = 0;   /* 9-4：WiFi 通道同样支持吉他控制 */
                        } else if (strcmp(line, "GTR:CHORD") == 0) {
                            g_gtr_req = 1;
                        } else if (strcmp(line, "GTR:GROUP") == 0) {
                            g_gtr_req = 2;
                        } else if (strcmp(line, "AI:ALIVE") == 0) {
                            ai_heartbeat_now();   /* Phase 2 P1：WiFi 模式心跳 */
                        }
                    }
                    li = 0;
                } else if (li < (int)sizeof(line) - 1) {
                    line[li++] = ch;
                }
            }
        }
        /* 回 ACK（main loop 在切页确认后置 g_ack_pending） */
        if (g_ack_pending >= 0 && g_cmd_client && g_cmd_client.connected()) {
            const char *name = (g_ack_pending == 0) ? "PAGE:HOME" :
                               (g_ack_pending == 1) ? "PAGE:GUITAR" :
                               (g_ack_pending == 2) ? "PAGE:CAMERA" : nullptr;
            if (name) {
                g_cmd_client.print("ACK:");
                g_cmd_client.print(name);
                g_cmd_client.print("\n");
                cam_client_log("[cmd] ACK sent: %s\n", name);
            }
            g_ack_pending = -1;
        }
        /* 9-4：吉他命令的 WiFi ACK（执行成功后由 main loop 置位） */
        if (g_gtr_ack_pending >= 0 && g_cmd_client && g_cmd_client.connected()) {
            const char *gname = (g_gtr_ack_pending == 0) ? "GTR:STRUM" :
                                (g_gtr_ack_pending == 1) ? "GTR:CHORD" :
                                (g_gtr_ack_pending == 2) ? "GTR:GROUP" : nullptr;
            if (gname) {
                g_cmd_client.print("ACK:");
                g_cmd_client.print(gname);
                g_cmd_client.print("\n");
                cam_client_log("[cmd] ACK sent: %s\n", gname);
            }
            g_gtr_ack_pending = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

#ifndef CAM_USB_INPUT
/* ★ 8-18 网络资源诊断 —— 定位"ping 0% 丢包却什么都连不上"的真凶。
 * 之前靠推测（先怀疑供电/RF/路由器，再怀疑 TIME_WAIT 耗尽 PCB 池）全部落空，
 * 改为直接读 lwIP 内部计数，用数字说话：
 *   free_fd 掉到 0            → socket fd 耗尽（CONFIG_LWIP_MAX_SOCKETS 上限，泄漏不会自动回收）
 *   active 持续涨不回落       → 连接对象没被正常关闭（真泄漏）
 *   tw 很大但 active/fd 正常  → 只是 TIME_WAIT 堆积；lwIP tcp_alloc() 会 tcp_kill_timewait()
 *                               自动回收最老的，通常无害，可排除
 *   四项全正常却连不上        → 本地栈资源无关，问题在 RF/AP/对端
 * 遍历链表带 64 上限：lwIP 线程可能并发改链表，防万一读到环形结构死循环。
 * fd 探测：连续 socket() 到失败为止即剩余配额，用完立刻全部 close 归还。 */
static void net_diag(const char *tag)
{
    int n_active = 0, n_tw = 0, n_bound = 0, n_listen = 0;
    for (struct tcp_pcb *p = tcp_active_pcbs; p && n_active < 64; p = p->next) n_active++;
    for (struct tcp_pcb *p = tcp_tw_pcbs;     p && n_tw     < 64; p = p->next) n_tw++;
    for (struct tcp_pcb *p = tcp_bound_pcbs;  p && n_bound  < 64; p = p->next) n_bound++;
    for (struct tcp_pcb_listen *p = tcp_listen_pcbs.listen_pcbs;
         p && n_listen < 64; p = p->next) n_listen++;

    int fds[24];
    int free_fd = 0;
    while (free_fd < 24) {
        int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) break;
        fds[free_fd++] = fd;
    }
    for (int i = 0; i < free_fd; i++) lwip_close(fds[i]);

    /* 带上 RSSI/状态：资源走势与信号走势必须对齐看，才能分清
     * "资源耗尽导致连不上" 与 "信号掉了导致连不上" —— 前者 free_fd 会跌，
     * 后者 free_fd 不动而 rssi 掉/status 变。 */
    cam_client_log("[net] %s active=%d tw=%d bound=%d listen=%d free_fd=%d heap=%u wl=%d rssi=%d ch=%d\n",
                   tag, n_active, n_tw, n_bound, n_listen, free_fd,
                   (unsigned)ESP.getFreeHeap(),
                   (int)WiFi.status(), (int)WiFi.RSSI(), (int)WiFi.channel());
}

/* ★ 8-18：裸 socket 探针 —— 必须拿到 connect 的 errno 才能定位故障层次。
 * Arduino 的 WiFiClient::connect() 超时只走 log_i（默认不输出），失败原因被吞掉，
 * 所以改用 lwip socket 自己做非阻塞 connect + select，把 errno 打出来。判据：
 *   ETIMEDOUT(116)                → SYN 发出去无人应答（AP 不转发/对端不在/信道问题）
 *   ECONNREFUSED(111)             → 收到 RST（对端在，但端口没开 → 网络通！）
 *   EHOSTUNREACH(118)/ENETUNREACH(114) → 路由表/ARP 解析不出下一跳
 *   ENOBUFS(105)/ENOMEM(12)       → lwIP 内存或 PCB 真的不够
 * 返回 0=成功，>0=errno，-1=socket 创建失败。 */
static int probe_connect(IPAddress ip, uint16_t port, uint32_t timeout_ms)
{
    int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(port);
    sa.sin_addr.s_addr = (uint32_t)ip;

    int rc = lwip_connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc == 0) { lwip_close(fd); return 0; }         /* 立刻连上（同网段罕见但可能） */
    if (errno != EINPROGRESS) { int e = errno; lwip_close(fd); return e; }

    fd_set wr;
    FD_ZERO(&wr);
    FD_SET(fd, &wr);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    rc = lwip_select(fd + 1, nullptr, &wr, nullptr, &tv);
    if (rc == 0) { lwip_close(fd); return ETIMEDOUT; } /* select 超时：SYN 无响应 */
    if (rc < 0)  { int e = errno; lwip_close(fd); return e ? e : -1; }

    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
    lwip_close(fd);
    return soerr;                                      /* 0=连接建立成功 */
}

/* ★ 8-18：用网关当"试纸"，区分【本地 TCP 栈楔死】与【远端不可达】。
 * 注意 ECONNREFUSED 也算"网络健康"——收到 RST 说明包一来一回都通了，
 * 只是网关没开那个端口。之前用 WiFiClient 时把 RST 也当失败，会误判。
 * 附带 loopback(127.0.0.1) 探测：loopback 走不出协议栈，
 * 若 loopback 也失败 → 是 socket/lwIP 层坏了；loopback 成功而网关失败
 * → 协议栈健康，问题出在 WiFi 数据面（发得出但收不回）。 */
static bool local_tcp_stack_alive(void)
{
    IPAddress gw = WiFi.gatewayIP();
    if ((uint32_t)gw == 0) return false;

    /* 超时 800ms：同网段网关正常 RTT <50ms（实测 4~20ms），800ms 已是 16 倍余量。
     * 原先 1500ms 让每次判定多等 0.7s，4 次就白等近 3 秒，直接拖慢自愈。 */
    int e_gw = probe_connect(gw, 80, 800);
    if (e_gw == 0 || e_gw == ECONNREFUSED) return true;

    /* 网关不通：再测 loopback 与 M1，把故障层次一次问清楚 */
    IPAddress lo(127, 0, 0, 1);
    int e_lo = probe_connect(lo, 8771, 500);      /* 本机 cmd_server 监听端口 */
    cam_client_log("[net] probe gw(%s):80 errno=%d  loopback:8771 errno=%d\n",
                   gw.toString().c_str(), e_gw, e_lo);
    return false;
}
#endif

void cam_task(void *arg)
{
    uint32_t fail_ms = 0;
    uint32_t fail_total = 0;   /* 远端摄像头取流失败次数（仅退避重试，不触发自愈重启） */
    uint32_t local_fail = 0;   /* 本地 WiFi 掉线次数（驱动自愈：重连/整机重启） */
#ifndef CAM_USB_INPUT
    uint32_t diag_ms  = 0;     /* net_diag 基线节流：20s 一次，用于看资源占用走势 */
    uint32_t heal_cnt = 0;     /* 连续 hard restart 次数；取帧成功即清零，达 6 次才整机重启 */
#endif
    for (;;) {
        uint32_t start = millis();

#ifndef CAM_USB_INPUT
        /* 每 20s 打一次基线：只看楔死瞬间的快照无法区分"缓慢泄漏"与"瞬间耗尽"，
         * 有了时间序列才能算出斜率（例如 fd 每分钟少 2 个 = 每帧漏 1 个）。 */
        if (diag_ms == 0 || millis() - diag_ms > 20000) {
            diag_ms = millis();
            net_diag("base");
        }
#endif

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
                    local_fail = 0;
                    heal_cnt = 0;   /* 取到帧＝数据面已恢复，重置自愈升级计数 */

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
                g_mdns_ms = 0;   /* 取流失败：清 5 分钟缓存，下次循环重新探测 M1 代理(可能已恢复) */
                if (g_cam_conn_fail) {
                    /* 出站 TCP 连接失败：可能是【本地栈楔死】也可能只是【远端挂了】，
                     * 用网关探测区分，避免把远端故障当本地故障而无谓重启面板。 */
                    if (WiFi.status() == WL_CONNECTED) {
                        if (local_tcp_stack_alive()) {
                            fail_ms = (fail_ms == 0) ? FRAME_INTERVAL_MS : RETRY_BACKOFF_MS;
                            fail_total++;
                            if ((fail_total % 10) == 1)
                                cam_client_log("[cam] remote unreachable, local stack OK (fail_total=%u)\n",
                                               (unsigned)fail_total);
                        } else {
                            local_fail++;
                            cam_client_log("[cam] local TCP stack wedged (gw unreachable, local_fail=%u)\n",
                                           (unsigned)local_fail);
                            net_diag("WEDGED");   /* 楔死现场取证：PCB/fd 到底是哪一项见底 */
                        }
                    }
                } else {
                    fail_ms = (fail_ms == 0) ? FRAME_INTERVAL_MS : RETRY_BACKOFF_MS;
                    fail_total++;   /* 远端取流失败：仅退避重试 */
                }
            }
        } else {
            /* 本地 WiFi 掉线：计入 local_fail 驱动自愈；远端不可达不算 */
            g_mdns_ms = 0;   /* 清缓存，重连后重新探测 M1 代理 */
            local_fail++;
            cam_client_log("[cam] local WiFi down (local_fail=%u)\n", (unsigned)local_fail);
        }
#endif  /* CAM_USB_INPUT */

#ifdef CAM_USB_INPUT
        /* USB 全链路视频模式不依赖 WiFi：跳过 WiFi 自愈/重启逻辑 */
        (void)fail_total;
        (void)local_fail;
#else
        /* ★ 8-18 网络自愈（已由实测数据重写，之前几版的根因判断全部作废）：
         * 真凶＝**WiFi 单播数据面周期性双向死亡，而关联层仍活着**。取证：
         *   [net] WEDGED active=0 tw=0 listen=1 free_fd=14 wl=3 rssi=-38
         *   [net] probe gw:80 errno=116(ETIMEDOUT)  loopback:8771 errno=0
         *   同时刻 M1→RLCD ping 100% 丢包，M1→网关 0% 丢包
         * 即 lwIP 资源全空闲、协议栈（loopback）完全正常、信号极好、关联未断，
         * 但对外 SYN 收不到任何回应。故与 socket 泄漏/TIME_WAIT/供电/RF 全都无关
         * （那几版假说均已被上面的数字推翻）。AP 侧元凶候选：MiFi 的
         * <ssv_wifi6>1 独立 WiFi6 开关仍开着 + 允许 40MHz，ax 调度与 ESP32
         * 老 WiFi 栈不兼容 —— beacon 照收所以不掉线，单播帧却收发失效。
         *
         * 自愈策略（实测有效性排序）：
         *   WiFi.reconnect()   ❌ 无效：同秒 CONNECTED+GOT_IP，紧接着仍 ETIMEDOUT
         *   wifi_hard_restart() ✅ 有效：radio OFF→STA→重新 begin，走完整
         *                          scan/auth/assoc/4-way，实测恢复后 60s 零丢包
         *   esp_restart()       ✅ 有效但代价大（丢 UI 状态），仅作最终兜底
         * 阈值演进（都有实测数据，血泪）：
         *   local_fail==12 → 中断约 90 秒（太慢）
         *   local_fail>=4  → 中断约 36 秒（ping 丢包 39%→19.2%）
         *   local_fail>=2  → ❌ 反而恶化到 76% 丢包！原因不在阈值，而在"自愈本身太贵"：
         *                    当时 wifi_hard_restart 走 start_wifi 的冷启动列表轮询，
         *                    每次在无关 SSID 上白等 8 秒 → 单次恢复 16~24 秒，
         *                    触发越频繁 → 设备越多时间耗在重新关联上。
         *   local_fail>=3  → 当前值。前提是 wifi_hard_restart 已改为
         *                    "定 BSSID+定信道快速重连"（2~4 秒回来）。
         *                    预期最坏中断 ≈ 3 次判定(~10s) + 重连(~3s) ≈ 13 秒。
         * 判定本身很可靠（只有"网关 TCP 也连不上"才计数，远端挂掉不会误触发）。
         * 每次 hard restart 后归零重新计数，连续 8 次都救不回来才整机重启；
         * 取帧一旦成功 heal_cnt 清零。 */
        if (local_fail >= 3) {
            heal_cnt++;
            if (heal_cnt >= 8) {
                Serial.println("[cam] self-heal-2: hard restart x8 ineffective, rebooting...");
                delay(200);
                esp_restart();
            }
            Serial.printf("[cam] self-heal-1: hard restart WiFi (attempt %u)\n",
                          (unsigned)heal_cnt);
            wifi_hard_restart();
            g_mdns_ms   = 0;   /* 强制重新解析后端 */
            local_fail  = 0;   /* 归零：给重连后的恢复留出观察窗口，避免立刻再触发 */
        }
#endif  /* CAM_USB_INPUT */

        uint32_t elapsed = millis() - start;
        uint32_t wait = (fail_ms > 0) ? fail_ms : FRAME_INTERVAL_MS;
        if (elapsed < wait) vTaskDelay(pdMS_TO_TICKS(wait - elapsed));
    }
}

} // namespace

/* Phase 2 P1 —— AI 健康心跳（M1 finger_page_control 上报）。
 * 置于匿名 namespace 外：头文件在 public 位置声明，需 external linkage。
 * g_ai_beat_ms：最近一次收到 "AI:ALIVE" 心跳的时刻（0=从未）。
 * 线程安全：volatile uint32，单次写原子。 */
volatile uint32_t g_ai_beat_ms = 0;
#define AI_FRESH_MS 10000UL   /* 10s 内心跳 = 进程活着且在处理 */

void ai_heartbeat_now(void) { g_ai_beat_ms = millis(); }
bool ai_client_has_beat(void) { return g_ai_beat_ms != 0; }
bool ai_client_is_fresh(void)
{
    if (g_ai_beat_ms == 0) return false;
    uint32_t now = millis();
    uint32_t age = (now >= g_ai_beat_ms) ? (now - g_ai_beat_ms) : 0;
    return (age < AI_FRESH_MS);
}

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
    /* 方案B：WiFi 模式下摄像头走 WiFi 取帧，无 rx_task */
    /* 栈 8192：VGA 640x480 全尺寸解码（TJpgDec 内部 MCU 缓冲 + 裁切）需要大栈 */
    xTaskCreate(cam_task, "camfetch", 8192, nullptr, 4, nullptr);
#endif
    /* ★ 8-31：WiFi 命令服务（8771）改为两种模式都常开——Mac 端键盘/手势切页
     * 不再依赖摄像头链路，无线通道独立可用。cmd_server_task 无 CAM_USB_INPUT 依赖
     * （仅用 g_cmd_client/g_page_req/ai_heartbeat_now/cam_client_log），安全。
     * 与 USB-CDC 命令通道（rx_task/cmd_feed）互不冲突：都只登记 g_page_req，
     * 由 main loop 统一取走执行；ACK 由 cam_client_send_ack 分发（见下）。 */
    g_cmd_server.begin();
    xTaskCreate(cmd_server_task, "camcmd", 4096, nullptr, 4, nullptr);
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
    /* 方案B：WiFi 命令服务登记的请求同样从这里取走（取走即清 -1） */
    int8_t p = g_page_req;
    if (p >= 0) g_page_req = -1;
    return p;
#endif
}

int8_t cam_client_take_gtr_cmd(void)
{
    int8_t g = g_gtr_req;
    if (g >= 0) g_gtr_req = -1;    /* 取走即清空（USB/WiFi 双入口共用） */
    return g;
}

/* RLCD-004.2 审核修正：ACK 只在**实际页面状态已确认**后发送（main.cpp 调用）。
 * 调用时机必须是：ui_goto_page 成功且 ui_get_current_page()==目标页，或已在目标页。
 * Lvgl_lock 失败/切页未生效时不得调用，让 M1 超时重发。 */
void cam_client_send_ack(int8_t page)
{
    const char *name = (page == 0) ? "PAGE:HOME" :
                       (page == 1) ? "PAGE:GUITAR" :
                       (page == 2) ? "PAGE:CAMERA" : nullptr;
    if (!name) return;
#ifdef CAM_USB_INPUT
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
#endif
    /* ★ 8-31：WiFi 命令服务（8771）两种模式都常开——这里同时置 g_ack_pending，
     * cmd_server_task 会顺带往当前连接的 WiFi 客户端回 ACK（无客户端时该标志
     * 保留，下次连接消费；可能被下一条命令覆盖，无害）。USB 命令的 ACK 走
     * Serial（上），WiFi 命令的 ACK 走 TCP（cmd_server_task），互不冲突。 */
    if (page >= 0 && page <= 2) g_ack_pending = page;
}

/* 9-4：吉他命令 ACK（GTR:STRUM/CHORD/GROUP 执行成功后由 main.cpp 调用）。
 * 语义与 cam_client_send_ack 相同：只在动作实际生效后回执。
 * USB 通道走 Serial（bridge 整行搜 "ACK:" 转发 hub），WiFi 通道走 TCP。 */
void cam_client_send_gtr_ack(int8_t kind)
{
    const char *name = (kind == 0) ? "GTR:STRUM" :
                       (kind == 1) ? "GTR:CHORD" :
                       (kind == 2) ? "GTR:GROUP" : nullptr;
    if (!name) return;
#ifdef CAM_USB_INPUT
    if (g_serial_lock && !xSemaphoreTake(g_serial_lock, pdMS_TO_TICKS(100))) return;
    Serial.print("ACK:");
    Serial.print(name);
    Serial.print("\n");
    if (g_serial_lock) xSemaphoreGive(g_serial_lock);
#endif
    if (kind >= 0 && kind <= 2) g_gtr_ack_pending = kind;
}
