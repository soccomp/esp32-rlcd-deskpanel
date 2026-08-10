/*
 * ESP32-CAM (AI-Thinker) —— RLCD 配套摄像头固件
 *
 * 功能：
 *   1. 多 WiFi 自动切换（家里 Hi12 / 办公室 BTWIFI6 / 手机热点 Gnos）
 *   2. mDNS 注册为 esp32cam.local —— RLCD 无需关心 IP 变化
 *   3. HTTP 服务：GET /capture（单帧 JPEG）、GET /status（JSON）
 *
 * 与 RLCD 侧约定：
 *   RLCD 通过 mDNS 解析 "esp32cam" 得到 IP，然后 GET /capture 拿 JPEG。
 *
 * 硬件：AI-Thinker ESP32-CAM，OV2640，4MB PSRAM，2.4G 单频 WiFi
 * 烧录：IO0-GND 短接 + USB-TTL，烧完去线按 RESET
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include "esp_camera.h"
#include "SD_MMC.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

/* ================== 摄像头引脚（CAMERA_MODEL_AI_THINKER） ================== */
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27

#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

#define LED_GPIO_NUM     4   /* 板载闪光灯（GPIO4，低电平亮） */

/* ================== 多 WiFi 自动切换（与 RLCD main.cpp 同款模式） ==================
 * 真实凭据在本地 src/wifi_config.h（已被 .gitignore 忽略，不提交 GitHub）；
 * 公开仓库无该文件时用下方占位网络编译（复制 wifi_config.example.h 为 wifi_config.h 并填入自己的网络）。 */
struct WifiCredential { const char *ssid; const char *password; };
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#endif
#ifndef WIFI_CONFIG_PRESENT
static const WifiCredential wifi_list[] = {
    { "YOUR_WIFI_SSID", "YOUR_WIFI_PASSWORD" },
};
static const int wifi_count = sizeof(wifi_list) / sizeof(wifi_list[0]);
#endif

static bool g_wifi_up = false;
static uint32_t g_frames = 0;          /* /status 用：/capture 被请求次数 */

#define JPEG_BUF   (96 * 1024)         /* VGA 640x480 quality18 单帧 <60KB，留余量 */

/* ================== SD 卡（板载 TF 槽，SDMMC 接口） ==================
 * 官方标注 ≤4GB（FAT16），实际 SDHC/FAT32 也可挂载；exFAT 视固件配置。
 * 失败不致命，仅上报状态。 */
static const char *g_sd_status = "none";
static uint64_t   g_sd_bytes   = 0;

static const char *sd_type_str(uint8_t t)
{
    switch (t) {
        case CARD_MMC:  return "MMC";
        case CARD_SD:   return "SDSC";
        case CARD_SDHC: return "SDHC";
        case CARD_NONE: return "NONE";
        default:        return "?";
    }
}

static void setup_sdcard(void)
{
    if (!SD_MMC.begin()) {
        Serial.println("[SD] card mount FAILED (32G 卡注意：需 FAT32 格式，exFAT 不支持)");
        g_sd_status = "mount_failed";
        return;
    }
    uint8_t t = SD_MMC.cardType();
    g_sd_bytes = SD_MMC.totalBytes();
    g_sd_status = sd_type_str(t);
    Serial.printf("[SD] ok type=%s size=%lluMB\n", sd_type_str(t),
                  (unsigned long long)(g_sd_bytes >> 20));
    SD_MMC.end();
}

/* ================== 摄像头初始化 ==================
 * 2026-08-06 下午：QVGA → VGA 640x480（RLCD 侧裁切中央 400x300 显示，细节翻倍），
 * quality 15 → 18。注意：多分辨率扫描会触发 POWERON_RESET，保持单分辨率。 */
static framesize_t g_active_fs = FRAMESIZE_VGA;

static void setup_camera(void)
{
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;

    config.xclk_freq_hz = 20000000;      /* 标准配置 */
    config.frame_size   = FRAMESIZE_VGA;       /* 640x480：RLCD 裁切中央 400x300 */
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY; /* 官方验证配置 */
    config.fb_location  = CAMERA_FB_IN_PSRAM;    /* 生产配置 */
    config.jpeg_quality = 12;                  /* VGA 下降质量：单帧 ~20KB，发送快 3 倍，
                                                * 避免大帧 send_P 阻塞 WebServer（半死根因之一） */
    config.fb_count     = 1;                   /* 官方验证配置 */

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAM] esp_camera_init failed: 0x%x\n", err);
        Serial.println("[CAM] 检查摄像头排线是否插好（注意事项第 3 条）");
        return;
    }
    Serial.println("[CAM] camera ready (VGA 640x480, JPEG)");
}

/* 帧缓存任务：持续抓帧，把最新一帧 JPEG 缓存到 PSRAM。
 * 原因1：esp32-camera 无人取帧时帧缓冲占满 -> 事件队列满 -> EV-EOF-OVF -> 摄像头自停。
 * 原因2：按需 fb_get 会阻塞（等新帧），HTTP handler 卡死。
 * 架构：后台常驻抓帧 -> /capture 秒回缓存帧，与 RLCD 的"拉最新帧"模式天然匹配。
 * fb_get/fb_return 不涉及 JPEG 解码（传感器硬件编码），CPU 开销极低。 */
/* ★ 双缓冲 JPEG：grab 写 buf[1-idx]，HTTP 发 buf[idx]——消除 send_P 与 memcpy 的
 * 无锁竞态（VGA 大帧 + 高频拉流下旧版单缓冲会卡死 WebServer，即"半死"症状）。 */
static uint8_t       *g_last_jpeg[2] = { nullptr, nullptr };
static volatile size_t g_last_len = 0;
static volatile uint8_t  g_jpeg_idx = 0;    /* 原子交换：读侧取 idx，写侧取 1-idx */
static volatile uint32_t g_last_seq = 0;    /* 递增帧号：>0 即证明有帧流 */
static volatile uint32_t g_last_handle_ms = 0;  /* loop 里 handleClient 心跳 */
static volatile uint32_t g_last_serve_ms  = 0;  /* 最近一次成功响应（/capture 或 /status） */

static void cam_grab_task(void *arg)
{
    (void)arg;
    for (;;) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            uint8_t w = 1 - g_jpeg_idx;
            if (fb->len <= JPEG_BUF && g_last_jpeg[w]) {
                memcpy(g_last_jpeg[w], fb->buf, fb->len);
                g_last_len = fb->len;
                g_jpeg_idx = w;      /* 写完原子交换，读侧可见 */
                g_last_seq++;
            }
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(120));   /* VGA 下降低抓帧负载（~8fps 够用） */
    }
}

/* ★ 看门狗：双条件自愈。
 * 条件1：loop 心跳卡死（handleClient 阻塞）超 15s -> 重启。
 * 条件2：WebServer 半死（30s 内没有任何成功响应——连接资源耗尽、
 *        新连接全失败的典型症状，此时 loop 仍正常转）-> 重启。
 * 优先级 5：必须高于抓帧任务(2)与 WebServer 处理，否则卡死时它也被饿死。 */
static void watchdog_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        uint32_t now = millis();
        if (g_last_handle_ms != 0 && (now - g_last_handle_ms) > 15000UL) {
            Serial.println("[WDT] HTTP handler stuck, restarting...");
            esp_restart();
        }
        if (g_last_serve_ms != 0 && (now - g_last_serve_ms) > 30000UL) {
            Serial.println("[WDT] no successful serve for 30s, restarting...");
            esp_restart();
        }
    }
}

/* ================== WiFi 连接 ================== */
static void connect_wifi(void)
{
    for (int i = 0; i < wifi_count; i++) {
        Serial.printf("[WiFi] trying \"%s\" ...\n", wifi_list[i].ssid);

        /* ★ IP 绑定：BTWIFI6（办公室网）用静态 IP，不依赖 DHCP，永不乱跳。
         * 网关 192.168.100.5 = 办公室 AP 网关（Mac netstat 实测）。
         * 其余网络显式恢复 DHCP，防止静态配置串网。 */
        if (strcmp(wifi_list[i].ssid, "BTWIFI6-169148") == 0) {
            IPAddress local_ip(192, 168, 100, 199);
            IPAddress gateway(192, 168, 100, 5);
            IPAddress subnet(255, 255, 255, 0);
            IPAddress dns(192, 168, 100, 5);
            WiFi.config(local_ip, gateway, subnet, dns);
            Serial.println("[WiFi] static IP 192.168.100.199 for BTWIFI6");
        } else {
            WiFi.config((uint32_t)0, (uint32_t)0, (uint32_t)0);  /* DHCP */
        }

        WiFi.begin(wifi_list[i].ssid, wifi_list[i].password);
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
            delay(200);
        }
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.setSleep(false);            /* 关闭 modem sleep：
             * BTWIFI6 等企业 AP 对省电设备有空闲踢除策略，ESP32 周期休眠
             * 会被判定离线踢出 → WiFi 反复掉线 → 画面定格（卡死根因之一）。 */
            Serial.printf("[WiFi] connected to \"%s\", IP=%s\n",
                          wifi_list[i].ssid, WiFi.localIP().toString().c_str());
            g_wifi_up = true;

            /* ★ mDNS：RLCD 用 esp32cam.local 找到本机，IP 变化无感 */
            if (MDNS.begin("esp32cam")) {
                MDNS.addService("http", "tcp", 80);
                Serial.println("[mDNS] esp32cam.local registered");
            } else {
                Serial.println("[mDNS] MDNS.begin failed");
            }
            return;
        }
        Serial.printf("[WiFi] \"%s\" failed, try next\n", wifi_list[i].ssid);
        WiFi.disconnect(false);
    }
    g_wifi_up = false;
    Serial.println("[WiFi] no network reachable");
}

/* ================== HTTP 服务（原生 socket 服务器） ==================
 * ⚠️ 2026-08-07 重写：Arduino WebServer 库是单客户端模型——每个连接
 * 处理完请求后 keep-alive 死等 5s（HTTP_MAX_DATA_WAIT），期间不响应
 * 任何新连接，导致高频拉帧"每 5-10 秒一帧"（火星探测器传照片）。
 * 原生 socket：每连接即连即断（Connection: close），响应完立即关闭，
 * 多客户端轮流服务，绝无等待阻塞。 */
static WiFiServer http_server(80);

/* 读取请求行（GET /capture HTTP/1.1），超时保护防慢客户端占住 */
static bool read_request_line(WiFiClient &client, String &path)
{
    uint32_t t0 = millis();
    String line;
    while (millis() - t0 < 2000) {
        if (client.available()) {
            char c = client.read();
            if (c == '\n') break;
            if (c != '\r') line += c;
        } else {
            delay(1);
        }
    }
    if (line.startsWith("GET ")) {
        int sp = line.indexOf(' ', 4);
        if (sp > 4) {
            path = line.substring(4, sp);
            return true;
        }
    }
    return false;
}

/* 丢弃剩余请求头（读到空行），避免影响下一个连接 */
static void drain_headers(WiFiClient &client)
{
    uint32_t t0 = millis();
    bool blank = false;
    while (millis() - t0 < 500) {
        if (client.available()) {
            char c = client.read();
            if (c == '\n') {
                if (blank) break;   /* 空行 = 头结束 */
                blank = true;
            } else if (c != '\r') {
                blank = false;
            }
        } else {
            delay(1);
        }
    }
}

static void handle_capture(WiFiClient &client)
{
    uint8_t idx = g_jpeg_idx;   /* 读侧：取当前完整帧，与 grab 写侧无竞态 */
    if (g_last_len == 0 || !g_last_jpeg[idx]) {
        client.print("HTTP/1.1 503 Service Unavailable\r\n"
                     "Content-Length: 8\r\nConnection: keep-alive\r\n\r\nno frame");
        return;
    }
    client.print("HTTP/1.1 200 OK\r\n");
    client.print("Content-Type: image/jpeg\r\n");
    client.print("Content-Length: ");
    client.print(g_last_len);
    client.print("\r\nConnection: close\r\n\r\n");
    client.write(g_last_jpeg[idx], g_last_len);
    client.flush();
    g_frames++;
    g_last_serve_ms = millis();
}

static void handle_status(WiFiClient &client)
{
    sensor_t *s = esp_camera_sensor_get();
    String mac = WiFi.macAddress();
    char json[320];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"model\":\"esp32cam\",\"mac\":\"%s\",\"frames\":%lu,"
             "\"seq\":%lu,\"last_len\":%lu,"
             "\"sensor\":%s,\"pid\":0x%04x,\"mid\":0x%04x,"
             "\"pixfmt\":%u,\"framesize\":%u,"
             "\"sd\":\"%s\",\"sd_bytes\":%llu}",
             mac.c_str(),
             (unsigned long)g_frames,
             (unsigned long)g_last_seq,
             (unsigned long)g_last_len,
             s ? "present" : "absent",
             s ? (unsigned)((s->id.PID << 8) | s->id.VER) : 0,
             s ? (unsigned)((s->id.MIDH << 8) | s->id.MIDL) : 0,
             s ? (unsigned)s->pixformat : 0,
             s ? (unsigned)s->status.framesize : 0,
             g_sd_status,
             (unsigned long long)g_sd_bytes);
    client.print("HTTP/1.1 200 OK\r\n");
    client.print("Content-Type: application/json\r\n");
    client.print("Content-Length: ");
    client.print(strlen(json));
    client.print("\r\nConnection: close\r\n\r\n");
    client.print(json);
    client.flush();
    g_last_serve_ms = millis();
}

/* MJPEG 流：一条连接持续推送 multipart/x-mixed-replace 帧，直到客户端断开。
 * —— 视频流标准架构：零请求开销、零时序竞争、永不新建连接，
 *    彻底取代"每帧一次 GET"的请求-响应模型（那是所有卡死的根源）。
 * ⚠️ ESP32 Arduino WiFiClient.connected() 在客户端 FIN 后的 CLOSE_WAIT
 * 状态返回 true（已知坑），必须周期 read() 触发 lwIP 状态刷新才能
 * 检测到断开——否则流死循环占死 loop()，其他请求全部超时。 */
static void handle_stream(WiFiClient &client)
{
    client.setNoDelay(true);
    /* 先发送完整响应头 + flush：响应头必须立即发出（实测：若只 print 不 flush，
     * 且循环因 connected() 初次 false 立即退出 -> client.stop() 丢缓冲 + RST，
     * 客户端连响应头都收不到，errno 54 的根因） */
    client.print("HTTP/1.1 200 OK\r\n");
    client.print("Content-Type: multipart/x-mixed-replace; boundary=camframe\r\n");
    client.print("Cache-Control: no-cache\r\n");
    client.print("Connection: close\r\n\r\n");
    client.flush();

    uint32_t last_seq = 0;
    uint32_t last_probe = 0;
    uint32_t last_tx_ok = millis();
    /* for(;;) 而不是 while(client.connected())：ESP32 Arduino 的 connected()
     * 在客户端等待响应期间可能短暂返回 false（已知坑），初次检查就 false 会
     * 导致流立即退出 + stop() 发 RST。循环退出只靠 write 探测 + 超时兜底。 */
    for (;;) {
        uint32_t seq = g_last_seq;
        if (seq != last_seq && g_last_len > 0) {
            uint8_t idx = g_jpeg_idx;
            client.printf("--camframe\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                          (unsigned)g_last_len);
            size_t n = client.write(g_last_jpeg[idx], g_last_len);
            client.print("\r\n");
            if (n > 0) last_tx_ok = millis();
            last_seq = seq;
            g_last_serve_ms = millis();
        }
        g_last_handle_ms = millis();

        /* 断开探测：每 500ms read() 刷新 lwIP 状态；
         * 退出条件：connected() false 且（write 探测失败 或 5s 无成功发送） */
        if (millis() - last_probe > 500) {
            last_probe = millis();
            while (client.available()) client.read();
            if (!client.connected()) {
                if (client.write("") == 0) break;          /* write 探测：对端已断 */
                if (millis() - last_tx_ok > 5000UL) break;  /* 5s 无发送兜底 */
            }
        }
        delay(10);
    }
}

/* 处理一个客户端连接：读请求行 -> 路由 -> 响应。
 * keep-alive 长连接：同一连接循环处理多个请求（响应头带
 * Connection: keep-alive），连接断开或客户端请求关闭才退出。
 * —— 消灭"每请求新建连接 -> TIME_WAIT 堆满 lwIP socket 表 -> 假死"的老坑 */
static void handle_client(WiFiClient &client)
{
    client.setNoDelay(true);
    String path;
    if (!read_request_line(client, path)) {
        client.stop();
        return;
    }
    drain_headers(client);

    if (path == "/stream") {
        handle_stream(client);   /* 流模式：独占连接直到断开 */
        client.stop();
        return;
    }

    /* 单请求即连即断：处理完立即关闭，不 keep-alive 等待。
     * ⚠️ keep-alive 循环（等 500ms 下一个请求）会让单个连接占住 loop，
     * 高频新建连接的客户端全部排队超时（实测 /capture 45% 成功）。
     * 单线程服务器 = 每次响应完立即释放，多客户端轮流服务。 */
    if (path == "/capture") {
        handle_capture(client);
    } else if (path == "/status") {
        handle_status(client);
    } else {
        client.print("HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: 34\r\nConnection: close\r\n\r\n"
                     "ESP32-CAM for RLCD. GET /capture or /status");
    }
    g_last_serve_ms = millis();
    client.stop();
}

static void start_http_server(void)
{
    http_server.begin();
    /* ★ WiFi OTA：烧入后从此免按键（deep sleep 自举硬件不可行——strapping
     * 引脚只在上电复位采样，deep sleep 唤醒不复位 ROM bootloader）。
     * OTA 是入站连接（Mac→摄像头），不受 BTWIFI6 出站 RST 影响。 */
    ArduinoOTA.setHostname("esp32cam");
    ArduinoOTA.onStart([]() {
        Serial.println("[OTA] start");
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("[OTA] end");
    });
    ArduinoOTA.onError([](ota_error_t err) {
        Serial.printf("[OTA] error %u\n", (unsigned)err);
    });
    ArduinoOTA.begin();
    Serial.println("[OTA] ArduinoOTA ready (:3232)");
    Serial.println("[HTTP] native server on :80 (/capture /status /stream)");
}

/* ================== 固件自举进下载模式（免按键烧录） ==================
 * ★ 2026-08-07 核心改进：ESP32 复位瞬间 GPIO0=0 即进 ROM 下载模式。
 * 传统方案靠人手按 IO0 键/短接 IO0-GND；本函数在收到串口命令 'D'
 * 后，先把 GPIO0 拉低并 gpio_hold_en 锁存电平，再**深度睡眠**——
 * ⚠️ 关键：gpio_hold 只在 deep sleep（RTC 域）保留，esp_restart（SW_RESET）
 * 会清除 hold（8-07 实测失败）。deep sleep 定时器 1s 唤醒后，复位采样
 * 瞬间 GPIO0 仍保持低 → ROM 自动进下载模式，esptool no_reset 直连烧录。
 * 触发：Mac 侧向串口发送单个字符 'D'（0x44）。 */
static void enter_download_mode(void)
{
    Serial.println("[UART] download cmd, GPIO0 LOW + hold + deep-sleep wake -> download mode");
    delay(50);
    pinMode(0, OUTPUT);
    digitalWrite(0, LOW);      /* 拉低 GPIO0 */
    gpio_hold_en(GPIO_NUM_0);  /* 锁存电平：deep sleep 唤醒复位后仍保持低 */
    delay(20);
    esp_sleep_enable_timer_wakeup(1000000);  /* 1 秒后自动唤醒触发复位采样 */
    esp_deep_sleep_start();    /* 深度睡眠（永不返回）——唤醒后进下载模式 */
}

/* 监听串口命令（独立任务，不依赖 loop——setup 里 connect_wifi 阻塞期间
 * 也必须响应 'D'）：'D' 进下载模式（免按键烧录） */
static void uart_cmd_task(void *arg)
{
    (void)arg;
    for (;;) {
        while (Serial.available()) {
            char c = Serial.read();
            if (c == 'D') {
                enter_download_mode();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ================== 串口直传（WiFi 模式已禁用） ==================
 * ⚠️ 2026-08-07 16:30：串口直传任务（uart_frame_task）在 WiFi 模式下**禁用**。
 * 原因：1M 满载 UART 发送 + Serial.flush() 阻塞严重干扰 WiFi 协议栈
 * （实测 /status 时通时断、/stream 超时）。串口直传与 WiFi 服务资源冲突，
 * 二者不能同时满载运行。需要串口直传时单独烧"串口版"固件。
 * 帧协议（供串口版参考）：AA55 5AA5 | len(2B LE) | JPEG | crc16(2B 累加)
 * 波特率必须 1000000（整数分频零误差；921600 错位）。 */

/* ================== 主流程 ================== */
void setup(void)
{
    /* ESP32-CAM 用 USB-TTL 供电容易掉压触发 brownout 重启：
     * 这是规避手段，正式部署仍建议 5V/2A 独立供电 */
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(1000000);
    Serial.setDebugOutput(true);
    delay(100);
    Serial.println("\n=== ESP32-CAM RLCD companion ===");

    setup_camera();
    setup_sdcard();

    /* 帧缓存缓冲（PSRAM 双缓冲） + 抓帧任务 + 看门狗任务 */
    g_last_jpeg[0] = static_cast<uint8_t *>(heap_caps_malloc(JPEG_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_last_jpeg[1] = static_cast<uint8_t *>(heap_caps_malloc(JPEG_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_last_jpeg[0] && g_last_jpeg[1]) {
        memset(g_last_jpeg[0], 0, JPEG_BUF);
        memset(g_last_jpeg[1], 0, JPEG_BUF);
        xTaskCreatePinnedToCore(cam_grab_task, "camgrab", 2048, nullptr, 2, nullptr, 0);
        xTaskCreate(watchdog_task, "camwdt", 2048, nullptr, 5, nullptr);  /* 最高优先：不被饿死 */
        /* ⚠️ 串口直传任务已禁用（8-07 16:30）：1M 满载 UART 发送 + Serial.flush()
         * 阻塞会严重干扰 WiFi 协议栈（/status 时通时断、/stream 超时）。
         * 串口直传与 WiFi 服务资源冲突，WiFi 模式下载口任务不启动。
         * 需要串口直传时再单独烧"串口版"固件（git 历史里有 uart_frame_task）。 */
        // xTaskCreate(uart_frame_task, "uartfrm", 3072, nullptr, 1, nullptr);
        xTaskCreate(uart_cmd_task, "uartcmd", 2048, nullptr, 4, nullptr);    /* 串口命令监听（免按键烧录） */
        Serial.println("[CAM] frame grabber (dual-buf) + WDT started (WiFi mode)");
    } else {
        Serial.println("[CAM] WARN: PSRAM alloc failed, /capture will 503");
    }

    connect_wifi();
    start_http_server();
    g_last_handle_ms = millis();
}

void loop(void)
{
    /* WiFi 掉线自动重连 */
    if (WiFi.status() != WL_CONNECTED && g_wifi_up) {
        Serial.println("[WiFi] lost, reconnecting...");
        g_wifi_up = false;
        connect_wifi();
        if (g_wifi_up) start_http_server();
    }
    /* 原生 socket：接受并处理新连接（每次一个，响应完即关） */
    WiFiClient client = http_server.available();
    if (client) {
        handle_client(client);
    }
    ArduinoOTA.handle();       /* WiFi OTA 轮询（免按键烧录通道） */
    g_last_handle_ms = millis();   /* WDT 心跳 */
    delay(5);
}
