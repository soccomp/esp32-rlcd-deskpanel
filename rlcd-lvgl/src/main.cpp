#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>       // esp_wifi_set_protocol / set_bandwidth：强制 11n+HT20 规避 AP 的 ax 调度
#include <ArduinoOTA.h>     // WiFi OTA：连网后可免线烧录固件
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <lwip/sockets.h>   // wifi_data_alive：非阻塞 connect 探测网关数据面健康
#include "driver/gpio.h"

#include "display_bsp.h"
#include "lvgl_bsp.h"
#include "ui.h"
#include "ui_clock.h"          // ui_clock_init / shtc3_read
#include "weather_client.h"   // fetch_weather_data：ESP32 直连 Open-Meteo
#include "rtc_pcf85063.h"     // PCF85063A RTC 驱动
#include "ui_ambient.h"       // 环境与趣味页（Page 1）：木鱼 / 雷达 / 和弦
#include "cam_client.h"       // 摄像头客户端：mDNS 发现 esp32cam + 拉帧解码
#include "audio_es8311.h"     // ES8311 CODEC + I2S 播放（非阻塞）
#include "shtc3.h"
#include "sd_card.h"          // SD 卡驱动（SDMMC 1线）
#include "log_store.h"        // 本地日志落盘（/sdcard/log）
#include "data_cache.h"       // 离线数据缓存（JSON 快照）
#include "stocks_client.h"    // 股票指数行情：直连腾讯+东财（脱离 Mac 后端）
#include "ota_backup.h"       // OTA 固件备份（升级前存 SD）

/* ----- 硬件对象与引脚 ----- */
// 显示 SPI: MOSI=12 SCK=11 DC=5 CS=40 RST=41, 横屏 400x300（与厂商验证一致）
// 注意：必须是 setup() 内用 new 分配，不能做全局静态对象——C++ 全局构造在
// PSRAM 初始化之前运行，heap_caps_malloc(MALLOC_CAP_SPIRAM) 会返回 NULL 触发 assert 崩溃
static DisplayPort *RlcdPort = nullptr;
static I2cMasterBus *g_i2c_bus = nullptr;   // SHTC3 I2C: SCL=14, SDA=13
static Shtc3Port    *g_shtc3    = nullptr;

// 物理左键 = KEY GPIO18，右键 = BOOT GPIO0（Waveshare ESP32-S3-RLCD-4.2 实物，面对屏幕方向）
#define BTN_LEFT_GPIO  18    // 左键：短按=下一页 / 吉他页短按=切和弦
#define BTN_RIGHT_GPIO 0     // 右键：短按=上一页 / 吉他页短按=拨弦 / 长按=切和弦组

// Wi-Fi 凭据列表：设备启动时依次尝试，哪个先连上就用哪个
// 真实凭据在本地 src/wifi_config.h（已被 .gitignore 忽略，不提交 GitHub）；
// 公开仓库无该文件时用下方占位网络编译（复制 wifi_config.example.h 为 wifi_config.h 并填入自己的网络）。
struct WifiCredential { const char* ssid; const char* password; };
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#endif
#ifndef WIFI_CONFIG_PRESENT
static const WifiCredential wifi_list[] = {
    { "YOUR_WIFI_SSID", "YOUR_WIFI_PASSWORD" },
    // { "你的热点名",     "密码"       },
};
static const int wifi_count = sizeof(wifi_list) / sizeof(wifi_list[0]);
#endif

// 没有 Wi-Fi 时的基准时间(UTC 秒)，仅让时钟先跑起来；连上 NTP 后自动校准
#define BASE_EPOCH 1753800000UL

/* ----- 状态 ----- */
static volatile uint32_t g_btn_down_ms = 0;   // KEY 按下时刻（用于判断短/长按）
static volatile bool     g_btn_pressed = false;
static volatile uint32_t g_btn_dur_ms  = 0;    // 最近一次按下的持续时间
static volatile bool     g_btn_release = false; // 已松开，待 loop 处理

/* BOOT 键（GPIO0）状态，结构同上 */
static volatile uint32_t g_btn2_down_ms = 0;
static volatile bool     g_btn2_pressed = false;
static volatile uint32_t g_btn2_dur_ms  = 0;
static volatile bool     g_btn2_release = false;
static time_t g_epoch_base = BASE_EPOCH;
static bool g_wifi_prev = false;
static bool g_ntp_done = false;
static bool g_bat_printed = false;   // 一次性打印电池电压用于验证
static uint32_t g_last_weather = 0;   // 上次直连尝试拉取天气的时刻
static uint32_t g_last_stocks = 0;    // 上次拉取股票行情的时刻（兼容旧引用；见下方槽位逻辑）
static int g_last_stocks_slot = -1;   // Phase 2 P2：上次成功刷新的交易所时钟槽位（分钟数，-1=未就绪）
static uint32_t g_last_wifi_retry = 0;  // 上次 WiFi 重连尝试的时刻（掉线后 30s 重试）
static bool g_weather_ready = false;  // 最近一次拉取是否成功（失败时 5 分钟重试）
static uint8_t g_weather_fail_cnt = 0; // 连续失败次数（指数退避：2^n 分钟，上限 1 小时）
static bool g_weather_permanent = false; // 永久失败（401/403/404）→ 24h 后再试

#define LONG_PRESS_MS 600             // 长按阈值：>=600ms 视为长按

/* ----- LVGL flush：把 RGB565 帧缓冲按阈值转成 ST7305 单色 ----- */
static void Lvgl_FlushCallback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    /* 8-13 审核修复：防御性 clamp——LVGL full_refresh 异常时 area 可能越界，
     * 直接按屏尺寸截断，杜绝 SetPixel 越界写（见 display_bsp.cpp RLCD_SetPixel）。 */
    int x1 = area->x1 < 0 ? 0 : area->x1;
    int y1 = area->y1 < 0 ? 0 : area->y1;
    int x2 = area->x2 >= SCREEN_W ? SCREEN_W - 1 : area->x2;
    int y2 = area->y2 >= SCREEN_H ? SCREEN_H - 1 : area->y2;
    uint16_t *buffer = (uint16_t *)color_map;
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
            RlcdPort->RLCD_SetPixel(x, y, color);
            buffer++;
        }
    }
    RlcdPort->RLCD_Display();
    lv_disp_flush_ready(drv);
}

/* ----- 按键 ISR（CHANGE 边沿：下降=按下，上升=松开；低电平有效） ----- */
static void IRAM_ATTR btn_isr(void)
{
    uint32_t now = millis();
    int lvl = gpio_get_level((gpio_num_t)BTN_LEFT_GPIO);
    if (lvl == 0) {
        /* 下降沿：按下 */
        g_btn_down_ms = now;
        g_btn_pressed = true;
    } else {
        /* 上升沿：松开，记录持续时长 */
        if (g_btn_pressed) {
            g_btn_dur_ms  = (now > g_btn_down_ms) ? (now - g_btn_down_ms) : 0;
            g_btn_release = true;
            g_btn_pressed = false;
        }
    }
}

/* 右键（GPIO0/BOOT）不用中断——GPIO0 是 strapping 脚，中断信号不可靠，改用 loop 轮询检测 */

/* ----- Wi-Fi / 时间 ----- */
/* 工作时段判断：工作日(周一~周五) 08:00-18:00 为工作时段。
 * 非工作时段（工作日 18:00-次日 8:00 + 周末全天）：天气/行情不周期刷新。 */
static bool is_work_hours(void)
{
    time_t now = time(nullptr);
    if (now <= 1700000000UL) return false;   // 时间未同步，保守按非工作时段
    struct tm *ti = localtime(&now);
    bool weekday = (ti->tm_wday >= 1 && ti->tm_wday <= 5);
    int h = ti->tm_hour;
    return weekday && (h >= 8 && h < 18);
}

/* ★ 8-18 WiFi 断连取证 —— 这是定位"每 30~60s 网络失效"的最后一块拼图。
 * 已用 net_diag 排除本地 lwIP 资源（free_fd 恒 14 未耗尽、tw 仅 6 远低于池上限
 * 16、listen PCB 健在、heap 稳定），故障不在 TCP 层而在 **WiFi 关联层**
 * （日志出现 local WiFi down → WL_CONNECTED 失效）。断连原因只有 AP/RF 才知道，
 * 而 ESP32 的 STA_DISCONNECTED 事件带 reason code，能一击定性：
 *   2  AUTH_EXPIRE      / 4 ASSOC_EXPIRE  → AP 主动老化踢除（AP 侧超时设置）
 *   8  ASSOC_LEAVE      / 5 ASSOC_TOOMANY → AP 踢人 / 客户端数超限
 *   15 4WAY_HANDSHAKE_TIMEOUT             → WPA2 握手/PMF 问题
 *   200 BEACON_TIMEOUT                    → 收不到 beacon（RF 干扰、信号弱、AP 睡了）
 *   201 NO_AP_FOUND / 202 AUTH_FAIL / 203 ASSOC_FAIL
 * 同时打印 RSSI/channel：RSSI < -75dBm 则属信号余量不足（换位置/加天线），
 * RSSI 良好却 BEACON_TIMEOUT 则是 AP 与 ESP32 的协议栈兼容性问题。 */
static volatile int  g_wifi_disc_reason = 0;
static volatile uint32_t g_wifi_disc_cnt = 0;

static void wifi_event_cb(WiFiEvent_t event, WiFiEventInfo_t info)
{
    switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        Serial.printf("[wifi] CONNECTED ch=%d rssi=%d\n",
                      WiFi.channel(), WiFi.RSSI());
        /* ★ 8-19：强制 11b/g-only（排除 11n）→ 下行不走 A-MPDU 聚合，从根上消除
         * "AP 对 ESP32 聚合下行帧让 RX 解不出"的假在线楔死。A-MPDU 是 11n 特性，
         * 11g 下行必为非聚合 MPDU，ESP32 稳定接收。协议变更需一次重关联才生效，
         * 由预防性重关联(12s)或自愈接管。仅在首次连接设一次。 */
        {
            static bool proto_set = false;
            if (!proto_set) {
                esp_err_t ep = esp_wifi_set_protocol(WIFI_IF_STA,
                                    WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
                Serial.printf("[wifi] force 11b/g (no 11n/A-MPDU): %d\n", (int)ep);
                proto_set = true;
            }
        }
        break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
        int r = info.wifi_sta_disconnected.reason;
        g_wifi_disc_reason = r;
        g_wifi_disc_cnt++;
        Serial.printf("[wifi] DISCONNECTED reason=%d cnt=%u uptime=%us\n",
                      r, (unsigned)g_wifi_disc_cnt, (unsigned)(millis() / 1000));
        break;
    }
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.printf("[wifi] GOT_IP %s rssi=%d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        break;
    default:
        break;
    }
}

/* ★ 8-18：记住"上次真正连通的那个 AP"，供快速重连用。
 * 血泪教训：start_wifi() 是"按列表逐个 SSID 试、每个等 8 秒"的冷启动逻辑，
 * 当它被自愈路径反复调用时，每次都要先在 Hi12/Gnos 上各白等 8 秒才轮到
 * BTWIFI6 —— 一次 hard restart 实测耗 16~24 秒。结果就是"检测越灵敏、
 * 丢包越严重"（local_fail>=2 时 4 分钟丢包率反而从 19% 恶化到 76%）。
 * 记下成功 AP 的下标 + BSSID + 信道后，重连可直接定频定 BSSID，
 * 跳过全信道扫描，实测 2~4 秒即回来。 */
static int      g_wifi_ok_idx = -1;
static uint8_t  g_wifi_ok_bssid[6] = {0};
static int32_t  g_wifi_ok_ch = 0;

/* ★ 8-20（Phase 2 P1）统一 WiFi 恢复入口：**首次连接与断线重连走同一函数**。
 * 旧实现把"刚连上拉数据"逻辑内联在 loop 的 g_wifi_prev 翻转分支里，
 * 且 30s 重连分支成功后又手动置 g_wifi_prev=true 而跳过恢复——隐藏状态 bug：
 *   - 断线后 g_wifi_prev 要等 1s tick 才变 false，若 tick 未跑而重连分支已置 true，
 *     则 connected != g_wifi_prev 恒假，数据恢复/摄像头启动被永久跳过；
 *   - 预防性 hard restart（未真正掉线）成功后同样不恢复。
 * 正解：连接就绪（无论首次/重连/硬重启后）统一走本函数刷新数据面。
 * 幂等：cam_client_init 内部防重；天气/行情由各自定时器节流，重复调用安全。 */
static void wifi_on_connected(void)
{
    /* 刚连上：立即拉取一次行情 + 直连天气 + 启动摄像头客户端 */
    g_last_stocks = millis();
    g_last_stocks_slot = -1;       /* P2: 重连后重新从当前槽位开始 */
    fetch_stocks_data();         /* 行情：连上即拉一次 */
    g_last_weather = millis();
    weather_result_t wr = fetch_weather_data();
    g_weather_ready = (wr == WEATHER_OK);
    g_weather_permanent = (wr == WEATHER_PERMANENT);
    if (g_weather_ready) g_weather_fail_cnt = 0;
    else g_weather_fail_cnt++;
    cam_client_init();   // 幂等：内部防重复初始化，离线时任务自行退避重试
    Serial.printf("WiFi connected, IP=%s (services refreshed)\n",
                  WiFi.localIP().toString().c_str());
}

static void start_wifi(void)
{    if (wifi_count == 0) return;
    setenv("TZ", "CST-8", 1);
    tzset();

    /* 断连 reason code 取证。注意：必须只注册一次！
     * 原先写在每次 start_wifi 里，被自愈路径反复调用后同一回调注册了 5 份，
     * 于是每个事件刷 5 行日志、cnt 一次跳 5 —— 排查时极易误判为"疯狂断连"。 */
    static bool ev_registered = false;
    if (!ev_registered) { WiFi.onEvent(wifi_event_cb); ev_registered = true; }

    /* ★ 8-18 已回退：曾在此强制 esp_wifi_set_protocol(11B|11G|11N)+HT20，
     * 意图是拒绝参与 AP 的 802.11ax 调度。实测证伪且有害：
     *   - 冷启动时调用返回 0x3014 (ESP_ERR_WIFI_STOP_STATE)，其实根本没生效；
     *   - 唯一真正生效的那次（hard restart 后 proto=0），紧接着就是连续
     *     reason=201 NO_AP_FOUND 风暴 + 关联成功 12 秒后数据面再次死亡。
     * 即"强制 legacy" 并不能防楔死，反而让扫描/关联本身变得不可靠。
     * 真正有效的手段只有两条：路由器侧关 <ssv_wifi6>，以及本机侧的
     * 快速 hard restart 自愈（见 wifi_hard_restart）。 */
    WiFi.mode(WIFI_STA);

    /* ★ 关闭 WiFi 省电模式（modem sleep）——关键！
     * ESP32 默认省电模式下周期休眠，BTWIFI6 这类企业级 AP 会把休眠设备
     * 判定为离线踢除（约 1-2 分钟一次，正是"摄像头几十秒卡死"的根因）。
     * 省电对桌面插电面板无意义，关闭后 WiFi 常驻在线。 */
    WiFi.setSleep(false);

    /* ★ 8-18：最大发射功率，提高与 BTWIFI6-MiFi 的链路余量。
     * 排查确认路由器配置干净（net_mode=0 非 ax、WPA2-PSK 无 PMF、
     * 防火墙/Dos 全关、无 AP 隔离、无 NAT 会话上限），供电也正常
     * （reset reason=0、电池 95%、USB 4.12V），但 WiFi 关联后约 30~60s
     * 即退化为"连着但丢包/短读"（short read 7180/16643），属 ESP32 与该
     * AP 的 RF/协议栈兼容性不稳。拉满 TX 功率是低成本稳妥的链路余量手段。 */
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    /* 依次尝试列表中的 Wi-Fi，每个最多等 8 秒。
     * 顺序上把"上次成功过的那个"提到最前，避免每次自愈都在无关 SSID 上白等 8 秒。 */
    for (int k = 0; k < wifi_count; k++) {
        int i = (g_wifi_ok_idx >= 0) ? ((g_wifi_ok_idx + k) % wifi_count) : k;
        Serial.printf("[WiFi] 尝试连接 \"%s\" ...\n", wifi_list[i].ssid);
        WiFi.begin(wifi_list[i].ssid, wifi_list[i].password);
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
            delay(200);
        }
        if (WiFi.status() == WL_CONNECTED) {
            /* 记下 BSSID/信道，后续 wifi_hard_restart 可定频直连 */
            g_wifi_ok_idx = i;
            g_wifi_ok_ch  = WiFi.channel();
            const uint8_t *bs = WiFi.BSSID();
            if (bs) memcpy(g_wifi_ok_bssid, bs, 6);
            Serial.printf("[WiFi] 已连接 \"%s\", IP=%s ch=%d\n",
                          wifi_list[i].ssid, WiFi.localIP().toString().c_str(),
                          (int)g_wifi_ok_ch);
            wifi_on_connected();   /* 首次连接：统一恢复数据面 */
            break;  // 连上了就不再试下一个
        }
        Serial.printf("[WiFi] \"%s\" 连接失败，尝试下一个\n", wifi_list[i].ssid);
        WiFi.disconnect(false);
    }

    configTime(8 * 3600, 0, "pool.ntp.org", "cn.ntp.org.cn");
}

/* ★ 8-18：数据面死而关联在时的唯一有效自愈（供 cam_task 兜底调用）。
 * 实测 WiFi.disconnect(false) + WiFi.reconnect() 无效：日志显示重连在同一秒
 * 就 CONNECTED + GOT_IP，但紧接着 connect 仍是 ETIMEDOUT —— 只重做 association
 * 不会重建 PTK/GTK，也清不掉 AP 侧那条坏掉的会话状态。彻底关 radio 再开，
 * 会走完整的 scan → auth → assoc → 4-way handshake，等价于"重新插网线"，
 * 且比 esp_restart 温和（不丢 UI 状态、不重新加载字体/PSRAM 缓冲）。 */
void wifi_hard_restart(void)
{
    uint32_t t0 = millis();
    Serial.println("[wifi] hard restart: radio OFF -> STA -> re-associate");
    WiFi.disconnect(true);      /* true：连带关闭 radio，彻底断开 */
    WiFi.mode(WIFI_OFF);
    delay(300);

    /* 快路径：定 BSSID + 定信道直连上次成功的 AP，跳过全信道扫描。
     * 自愈的价值完全取决于"多快回来"——冷启动列表轮询要 16~24s，
     * 这条快路径实测 2~4s。只有它失败了才退回完整 start_wifi()。 */
    if (g_wifi_ok_idx >= 0) {
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
        WiFi.begin(wifi_list[g_wifi_ok_idx].ssid,
                   wifi_list[g_wifi_ok_idx].password,
                   g_wifi_ok_ch, g_wifi_ok_bssid);
        uint32_t t1 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t1 < 5000) delay(100);
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[wifi] fast reconnect OK in %ums, IP=%s ch=%d\n",
                          (unsigned)(millis() - t0),
                          WiFi.localIP().toString().c_str(), (int)WiFi.channel());
            wifi_on_connected();   /* 硬重启后统一恢复数据面（Phase 2 P1） */
            return;
        }
        Serial.println("[wifi] fast reconnect failed, fall back to full scan");
        WiFi.disconnect(false);
    }
    start_wifi();               /* 慢路径：mode(STA) + 列表轮询 */
    Serial.printf("[wifi] hard restart done in %ums\n", (unsigned)(millis() - t0));
}

/* ★ 8-20（Phase 2 P1）WiFi 数据面健康探测：轻量 TCP 连网关。
 * 语义与 cam_client.cpp 的 local_tcp_stack_alive 一致，但独立实现避免跨模块耦合：
 *   - ECONNREFUSED(111) 也算健康（收到 RST = 包一来一回都通，仅网关没开 80 端口）
 *   - ETIMEDOUT(116) = SYN 无回应 → 数据面确实死了
 * 非阻塞 connect + select，超时 800ms（同网段网关 RTT 实测 <50ms，余量 16 倍）。
 * 只在 WiFi 关联时调用；每次新建短连接立即归还，无资源累积。 */
static bool wifi_data_alive(void)
{
    IPAddress gw = WiFi.gatewayIP();
    if ((uint32_t)gw == 0) return false;

    int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(80);
    sa.sin_addr.s_addr = (uint32_t)gw;

    int rc = lwip_connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc == 0) { lwip_close(fd); return true; }
    if (errno != EINPROGRESS) {
        bool ok = (errno == ECONNREFUSED);
        lwip_close(fd);
        return ok;
    }

    fd_set wr;
    FD_ZERO(&wr);
    FD_SET(fd, &wr);
    struct timeval tv = {0, 800000};   /* 800ms */
    rc = lwip_select(fd + 1, nullptr, &wr, nullptr, &tv);
    bool ok;
    if (rc <= 0) {
        ok = false;                    /* 超时/错误：SYN 无响应 */
    } else {
        int soerr = 0;
        socklen_t slen = sizeof(soerr);
        lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
        ok = (soerr == 0 || soerr == ECONNREFUSED);
    }
    lwip_close(fd);
    return ok;
}

// GPIO4 电池电压 ADC（3 倍分压）：18650 锂电 2.7V=空 ~ 4.2V=满
#define BAT_ADC_GPIO    4
#define BAT_DIVIDER     3.0f       // 硬件分压比
#define BAT_MV_EMPTY    2700.0f    // 空电电压 mV
#define BAT_MV_FULL     4200.0f    // 满电电压 mV

static uint8_t read_battery_percent(void)
{
    // 多次采样取平均，减少噪声
    int32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        sum += analogReadMilliVolts(BAT_ADC_GPIO);
        delay(2);
    }
    float avg_mv = (float)(sum / 8) * BAT_DIVIDER;  // 还原真实电池电压

    // 线性映射到百分比（可后续加 OCV 曲线更精确）
    int pct = (int)((avg_mv - BAT_MV_EMPTY) * 100.0f / (BAT_MV_FULL - BAT_MV_EMPTY));
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
}

/* 由 ui_clock 的 5s 定时器（LVGL 任务内）调用，读取 SHTC3 温湿度。
 * 所有 I²C 访问集中在该任务，避免与 RTC 读取跨任务争用 Wire 总线。 */
bool shtc3_read(float *temp, float *humi)
{
    if (g_shtc3 && g_shtc3->Shtc3_ReadTempHumi(temp, humi) == 0) return true;
    return false;
}

void setup()
{
    Serial.begin(115200);
    delay(300);

    setenv("TZ", "CST-8", 1);
    tzset();

    // PSRAM 此刻已就绪（Serial.begin + delay 之后），再分配显示驱动对象
    RlcdPort = new DisplayPort(12, 11, 5, 40, 41, SCREEN_W, SCREEN_H);
    RlcdPort->RLCD_Init();
    Lvgl_PortInit(SCREEN_W, SCREEN_H, Lvgl_FlushCallback);

    // SHTC3 温湿度：I2C 总线 SCL=14 SDA=13（与厂商示例一致），setup 内分配避免全局构造时序问题
    g_i2c_bus = new I2cMasterBus(14, 13, 0);
    g_shtc3 = new Shtc3Port(*g_i2c_bus);

    // PCF85063A RTC：复用同一根 Wire 总线（SDA=13/SCL=14，0x51）
    rtc_init(*g_i2c_bus->GetWire());

    // 音频：ES8311 CODEC + I²S + 功放（须在 Wire.begin 之后；I2cMasterBus 已建立总线）
    audio_init();
    // 办公室热闹雷达：启动后台 Wi-Fi/BLE 扫描任务（独立任务，不卡 LVGL 渲染）
    ui_ambient_start_radar();

    // SD 卡 + 本地日志 + 离线数据缓存（须在 ui_init 前挂载 SD；无卡时自动降级，不影响主功能）
    log_store_init();
    data_cache_init();

    if (Lvgl_lock(-1)) {
        ui_init();
        Lvgl_unlock();
    }

    pinMode(BTN_LEFT_GPIO, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BTN_LEFT_GPIO), btn_isr, CHANGE);

    pinMode(BTN_RIGHT_GPIO, INPUT_PULLUP);   // 右键（GPIO0 strapping 脚）：polling 检测，不用中断

    // 电池 ADC：GPIO4，12bit 精度，最大衰减（0-3.3V 量程）
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    pinMode(BAT_ADC_GPIO, INPUT);

    start_wifi();
    Serial.println("RLCD LVGL desk-panel started");

    // WiFi OTA：连上网络后才可用（未连网时跳过，不影响正常启动）
    if (WiFi.status() == WL_CONNECTED) {
        ArduinoOTA.setHostname("esp32-rlcd");
        // 可选：加密码防止局域网内误刷（去掉注释并改密码即可）
        // ArduinoOTA.setPassword("rlcd-ota");
        ArduinoOTA.onStart([]() {
            Serial.println("[OTA] 开始接收固件...");
            ota_backup_current_fw();   // 升级前把当前固件备份到 SD 卡
        });
        ArduinoOTA.onEnd([]() {
            Serial.println("[OTA] 烧写完成，即将重启");
        });
        ArduinoOTA.onError([](ota_error_t err) {
            Serial.printf("[OTA] 错误 %d\n", (int)err);
        });
        ArduinoOTA.begin();
        Serial.println("[OTA] WiFi OTA ready (esp32-rlcd)");
    }
}

void loop()
{
    ArduinoOTA.handle();   // 必须频繁调用，保证 OTA 能及时响应

    /* ★ 8-20（Phase 2 P1）健康度驱动 WiFi 自愈（替换原无条件 12s hard restart）。
     *
     * 背景/硬件证据（决定保留周期性检查节奏）：AP 对 ESP32 下发 A-MPDU 聚合下行帧，
     * 周期性 30~60s 让 RX 解不出 → "假在线"（关联在、beacon 收得到，但单播数据面
     * 双向死亡）。每次重关联重置 AP 聚合状态即恢复；12s < 楔死周期(~14s) 是实测
     * 有效余量。但**不再无条件每 12s 硬重启**——否则永远停在高频重启，任务书 P1
     * 明确要求"workaround 不应是最终架构"。
     *
     * 新逻辑：每 12s 用轻量 TCP 探测网关判断数据面是否真的健康：
     *   - HEALTHY（探测通过）→ 什么都不做（不重启，节省重启开销）
     *   - 连续 FAIL（数据面确死）→ wifi_hard_restart()（重关联重建 PTK/GTK）
     *   - 连续 hard restart 多次仍失败 → esp_restart()（最终兜底）
     * 探测只连网关（同网段 <50ms），开销远小于一次 2~4s 的重关联。 */
    {
        static uint32_t s_prev_health_check = 0;
        static uint32_t s_health_fail_cnt   = 0;   /* 连续失败次数 */
        if (millis() - s_prev_health_check > 12000UL) {
            s_prev_health_check = millis();
            if (WiFi.status() == WL_CONNECTED) {
                if (wifi_data_alive()) {
                    if (s_health_fail_cnt > 0) {
                        Serial.printf("[wifi] data plane healthy again (after %u fails)\n",
                                      (unsigned)s_health_fail_cnt);
                        s_health_fail_cnt = 0;
                    }
                } else {
                    s_health_fail_cnt++;
                    Serial.printf("[wifi] data plane probe FAIL #%u -> recover\n",
                                  (unsigned)s_health_fail_cnt);
                    /* 升级阶梯：确认失败才 hard restart（≥1 次失败即触发，
                     * 连续 6 次 hard restart 救不回才整机重启） */
                    if (s_health_fail_cnt >= 6) {
                        Serial.println("[wifi] recovery x6 ineffective, rebooting");
                        delay(200);
                        esp_restart();
                    } else {
                        wifi_hard_restart();   /* 重连成功会经 wifi_on_connected 恢复数据 */
                    }
                }
            } else {
                s_health_fail_cnt = 0;   /* 掉线由 loop 的 30s 重连分支处理 */
            }
        }
    }

    static uint32_t last_sec = 0;
    uint32_t now = millis();

    /* ----- 左键：短按=下一页；吉他页短按=切和弦；长按=下一页 ----- */
    if (g_btn_release) {
        g_btn_release = false;
        uint32_t dur = g_btn_dur_ms;
        if (dur >= LONG_PRESS_MS) {
            if (Lvgl_lock(10)) {
                ui_next_page();
                Lvgl_unlock();
            }
        } else if (dur >= 40) {
            if (ui_get_current_page() == 1) {
                /* 吉他页：KEY 键切换到下一个和弦 + 播放音频 */
                if (Lvgl_lock(100)) { ui_ambient_next_chord(); Lvgl_unlock(); }
            } else if (Lvgl_lock(10)) {
                ui_next_page();
                Lvgl_unlock();
            }
        }
    }

    /* ----- 右键（GPIO0/BOOT）轮询检测：GPIO0 strapping 脚中断不可靠，改用 polling ----- */
    {
        static int prev_level = HIGH;
        static uint32_t poll_down_ms = 0;
        int cur = digitalRead(BTN_RIGHT_GPIO);
        if (cur == LOW && prev_level == HIGH) {
            /* 下降沿：按下，记录时刻 */
            poll_down_ms = now;
        } else if (cur == HIGH && prev_level == LOW) {
            /* 上升沿：松开，计算持续时间 */
            uint32_t dur = (now > poll_down_ms) ? (now - poll_down_ms) : 0;
            if (dur >= 30) {  // 防抖，30ms 以上视为有效按键
                g_btn2_dur_ms  = dur;
                g_btn2_release = true;
            }
        }
        prev_level = cur;
    }

    /* ----- 右键：短按=上一页；吉他页短按=拨弦 / 长按=切和弦组 ----- */
    if (g_btn2_release) {
        g_btn2_release = false;
        uint32_t dur = g_btn2_dur_ms;
        if (dur >= LONG_PRESS_MS) {
            if (ui_get_current_page() == 1) {
                /* 吉他页：BOOT 长按切换 OPEN / 7TH 和弦练习组 */
                if (Lvgl_lock(100)) { ui_ambient_next_group(); Lvgl_unlock(); }
            }
        } else if (dur >= 40) {
            if (ui_get_current_page() == 1) {
                /* 吉他页：BOOT 短按播放当前和弦并增加练习计数 */
                if (Lvgl_lock(50)) { ui_ambient_tap(); Lvgl_unlock(); }
            } else if (Lvgl_lock(10)) {
                ui_prev_page();
                Lvgl_unlock();
            }
        }
    }

    /* ----- RLCD-004：USB-CDC 手势命令切页（M1 侧识别手指数量后下发 PAGE:xxx） -----
     * 命令由 cam_task 解析并登记，这里在主循环取走执行，切页与按键走同一套
     * Lvgl_lock + ui_goto_page 路径；已在目标页则不重复切，避免无谓刷屏。
     * RLCD-004.2 审核修正：ACK 只在**实际页面状态已确认**后发送——
     * 切页后校验 ui_get_current_page()==目标页才 ACK；Lvgl_lock 失败/切页
     * 未生效则不 ACK（M1 超时自动重发），彻底消除"命令已收到=已切页"的假 ACK。 */
    {
        int8_t req = cam_client_take_page_cmd();
        if (req >= 0) {
            /* 解析日志（rx_task 不再 printf，由 main 统一输出，避免三方并发访问 CDC） */
            const char *name = (req == 0) ? "HOME" :
                               (req == 1) ? "GUITAR" :
                               (req == 2) ? "CAMERA" : "?";
            cam_client_log("[cmd] PAGE:%s -> page %d\n", name, (int)req);
            if (req == (int8_t)ui_get_current_page()) {
                /* 已在目标页：页面状态已确认，直接 ACK */
                cam_client_send_ack(req);
                cam_client_log("[cmd] page %d already current, skip (gesture)\n", (int)req);
            } else if (Lvgl_lock(1000)) {   /* 反射屏全屏刷新可占锁数百 ms，100ms 偏短 */
                ui_goto_page((uint8_t)req);
                Lvgl_unlock();
                if ((int8_t)ui_get_current_page() == req) {
                    cam_client_send_ack(req);   /* 实际切换成功 -> ACK */
                    cam_client_log("[cmd] page -> %d (gesture)\n", (int)req);
                } else {
                    /* 切页未生效（异常）-> 不 ACK，M1 将超时重发 */
                    cam_client_log("[cmd] page -> %d FAILED (cur=%d), no ACK\n",
                                  (int)req, (int)ui_get_current_page());
                }
            } else {
                /* Lvgl_lock 失败 -> 不 ACK，M1 将超时重发 */
                cam_client_log("[cmd] Lvgl_lock failed, page %d not switched, no ACK\n", (int)req);
            }
        }
    }

    /* 进入页面检测：进入吉他页触发雷达扫描 */
    {
        static uint8_t last_pg = 0xFF;
        uint8_t pg = ui_get_current_page();
        if (pg != last_pg) {
            if (pg == 1) ui_ambient_on_show();
            last_pg = pg;
        }
    }

    /* 每秒：刷新时钟 / WiFi / 电量 */
    if (now - last_sec >= 1000) {
        last_sec = now;

        bool connected = (WiFi.status() == WL_CONNECTED);
        if (connected != g_wifi_prev) {
            g_wifi_prev = connected;
            if (connected) {
                /* 首次连接/掉线重连的恢复统一走 wifi_on_connected()，
                 * 避免与下方 30s 重连分支重复且不一致（Phase 2 P1） */
                wifi_on_connected();
            } else {
                Serial.println("WiFi disconnected");
            }
        }
        /* WiFi 掉线自动重连：每 30 秒尝试一次（避免频繁 begin 骚扰路由器） */
        if (!connected) {
            if (now - g_last_wifi_retry >= 30000UL) {
                g_last_wifi_retry = now;
                start_wifi();
                if (WiFi.status() == WL_CONNECTED) {
                    /* 统一恢复已由 start_wifi->wifi_on_connected 完成；
                     * 仅置位 g_wifi_prev 让 1s tick 状态一致（不重复恢复） */
                    g_wifi_prev = true;
                    Serial.printf("WiFi reconnected, IP=%s\n",
                                  WiFi.localIP().toString().c_str());
                }
            }
        }
        if (connected) {
            bool work_hours = is_work_hours();
            time_t t = time(nullptr);
            if (t > 1700000000UL) {
                g_epoch_base = t - (time_t)(now / 1000);  // NTP 校准基准
                if (!g_ntp_done) {
                    g_ntp_done = true;
                    struct tm *ti = localtime(&t);
                    char buf[32];
                    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ti);
                    Serial.printf("NTP synced: %s\n", buf);
                    rtc_request_set(t);   // 延迟到 LVGL 任务内写入 PCF85063A
                }
            }

            if (work_hours) {
                /* 天气刷新间隔（指数退避）：
                 *  - 成功 → 1 小时
                 *  - 可恢复失败（网络/429）→ 2^n 分钟（2/4/8/16/32/60 封顶）
                 *  - 永久失败（401/403/404）→ 24 小时后再试（key/额度/位置问题需人工处理） */
                uint32_t weather_interval = 3600000UL;
                if (!g_weather_ready) {
                    if (g_weather_permanent) {
                        weather_interval = 24UL * 3600 * 1000UL;   /* 24h */
                    } else {
                        uint32_t backoff = 2UL * 60 * 1000UL;
                        for (uint8_t i = 0; i < g_weather_fail_cnt && backoff < 60UL * 60 * 1000UL; ++i) {
                            backoff *= 2;
                        }
                        if (backoff > 60UL * 60 * 1000UL) backoff = 60UL * 60 * 1000UL;
                        weather_interval = backoff;
                    }
                }
                if (now - g_last_weather >= weather_interval) {
                    g_last_weather = now;
                    weather_result_t wr = fetch_weather_data();
                    g_weather_ready = (wr == WEATHER_OK);
                    g_weather_permanent = (wr == WEATHER_PERMANENT);
                    if (g_weather_ready) g_weather_fail_cnt = 0;
                    else g_weather_fail_cnt++;
                }
            }
            /* 股票行情 —— Phase 2 P2 交易所时钟槽位（替代相对定时器，消除漂移）。
             * 相对定时器缺陷：刷新时刻锚定"上次完成时刻"，若上次拉取耗时/失败，
             * 后续会逐次漂移（例：11:29 更新 → 下次 13:09 而非 13:00）。
             * 槽位方案：把交易时段切成 10 分钟网格（9:30,9:40,...11:30,13:00,...15:30），
             * 当前分钟向下取整得到当前槽位；槽位变化才刷新，刷新成功才推进槽位，
             * 失败保持原槽位下轮重试 → 时刻始终锚定交易所时钟，不累积漂移。
             * 非交易时段槽位重置为 -1，开盘后首个槽位（如 9:30）必刷。 */
            time_t st = time(nullptr);
            if (st > 1700000000UL) {
                struct tm *sti = localtime(&st);
                bool weekday = (sti->tm_wday >= 1 && sti->tm_wday <= 5);
                int h = sti->tm_hour, m = sti->tm_min;
                int t = h * 60 + m;
                /* 交易时段：上午 9:30-11:30、下午 13:00-15:30。
                 * 下午延长到 15:30：行情接口在 15:00 收盘后仍会短暂更新
                 * （约 15:15 才定格为最终收盘价），延长刷新窗口确保拿到收盘价，
                 * 避免屏幕卡在收盘前的中间值。 */
                bool trading = weekday &&
                               ((t >= 9 * 60 + 30 && t <= 11 * 60 + 30) ||
                                (t >= 13 * 60 && t <= 15 * 60 + 30));
                if (trading) {
                    int slot = (t / 10) * 10;   /* 当前交易所时钟槽位（分钟网格） */
                    if (slot != g_last_stocks_slot) {
                        if (fetch_stocks_data()) {
                            g_last_stocks_slot = slot;   /* 仅成功才推进槽位 */
                        }
                        g_last_stocks = millis();
                    }
                } else {
                    /* 非交易时段：重置槽位，下一交易段首槽必刷 */
                    g_last_stocks_slot = -1;
                }
            }
        }

        time_t cur = g_epoch_base + (time_t)(now / 1000);
        struct tm *ti = localtime(&cur);
        char tb[10], db[14];
        strftime(tb, sizeof(tb), "%H:%M", ti);
        strftime(db, sizeof(db), "%Y/%m/%d", ti);

        if (Lvgl_lock(10)) {
            ui_update_clock(tb, db);
            ui_update_wifi(WiFi.status() == WL_CONNECTED);
            uint8_t bat = read_battery_percent();
            ui_update_battery(bat);
            if (!g_bat_printed) {
                g_bat_printed = true;
                // 一次性打印原始 ADC 值用于验证
                int32_t raw = 0;
                for (int i = 0; i < 4; i++) { raw += analogReadMilliVolts(BAT_ADC_GPIO); delay(2); }
                Serial.printf("BAT raw=%dmV, estimated=%d%%, USB=%d\n", raw/4, bat, (int)(raw/4 * BAT_DIVIDER));
            }
            Lvgl_unlock();
        }
    }

    /* 注：SHTC3 温湿度现由 ui_clock 的 5s 定时器（LVGL 任务内）读取并更新，
     * 与 PCF85063A RTC 同享一根 I²C 总线，避免跨任务争用。 */

    delay(10);
}
