#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>     // WiFi OTA：连网后可免线烧录固件
#include <time.h>
#include <stdlib.h>
#include "driver/gpio.h"

#include "display_bsp.h"
#include "lvgl_bsp.h"
#include "ui.h"
#include "ui_schedule.h"
#include "ui_clock.h"          // ui_clock_init / ui_clock_sync_meeting / shtc3_read
#include "weather_client.h"   // fetch_weather_data：ESP32 直连 Open-Meteo
#include "rtc_pcf85063.h"     // PCF85063A RTC 驱动
#include "ui_ambient.h"       // 环境与趣味页（Page 2）：木鱼 / 雷达
#include "cam_client.h"       // 摄像头客户端：mDNS 发现 esp32cam + 拉帧解码
#include "audio_es8311.h"     // ES8311 CODEC + I2S 播放（非阻塞）
#include "shtc3.h"
#include "sd_card.h"          // SD 卡驱动（SDMMC 1线）
#include "log_store.h"        // 本地日志落盘（/sdcard/log）
#include "data_cache.h"       // 离线数据缓存（JSON 快照）
#include "ota_backup.h"       // OTA 固件备份（升级前存 SD）

/* ----- 硬件对象与引脚 ----- */
// 显示 SPI: MOSI=12 SCK=11 DC=5 CS=40 RST=41, 横屏 400x300（与厂商验证一致）
// 注意：必须是 setup() 内用 new 分配，不能做全局静态对象——C++ 全局构造在
// PSRAM 初始化之前运行，heap_caps_malloc(MALLOC_CAP_SPIRAM) 会返回 NULL 触发 assert 崩溃
static DisplayPort *RlcdPort = nullptr;
static I2cMasterBus *g_i2c_bus = nullptr;   // SHTC3 I2C: SCL=14, SDA=13
static Shtc3Port    *g_shtc3    = nullptr;

// 物理左键 = KEY GPIO18，右键 = BOOT GPIO0（Waveshare ESP32-S3-RLCD-4.2 实物，面对屏幕方向）
#define BTN_LEFT_GPIO  18    // 左键：短按=下一页(会议) / 会议页短按下卡 / 长按切筛选
#define BTN_RIGHT_GPIO 0     // 右键：短按=上一页(吉他) / 会议页短按上卡 / 吉他拨弦 / 长按离开会议页

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
static uint32_t g_last_sched = 0;    // 上次拉取会议日程的时刻
static uint32_t g_last_weather = 0;   // 上次直连尝试拉取天气的时刻
static uint32_t g_last_stocks = 0;    // 上次拉取股票行情的时刻（交易时段 10 分钟一次）
static uint32_t g_last_wifi_retry = 0;  // 上次 WiFi 重连尝试的时刻（掉线后 30s 重试）
static uint32_t g_last_backend_probe = 0;  // 非工作时段：上次探测后端可达性的时刻（15 分钟一次）
static bool g_weather_ready = false;  // 最近一次拉取是否成功（失败时 5 分钟重试）
static uint8_t g_weather_fail_cnt = 0; // 连续失败次数（指数退避：2^n 分钟，上限 1 小时）
static bool g_weather_permanent = false; // 永久失败（401/403/404）→ 24h 后再试

#define LONG_PRESS_MS 600             // 长按阈值：>=600ms 视为长按

/* ----- LVGL flush：把 RGB565 帧缓冲按阈值转成 ST7305 单色 ----- */
static void Lvgl_FlushCallback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    uint16_t *buffer = (uint16_t *)color_map;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
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
 * 非工作时段（工作日 18:00-次日 8:00 + 周末全天）：天气/会议/X帖文不周期刷新，
 * 仅每 15 分钟探测后端可达性，后端恢复（电脑开机）时强制全量刷新。 */
static bool is_work_hours(void)
{
    time_t now = time(nullptr);
    if (now <= 1700000000UL) return false;   // 时间未同步，保守按非工作时段
    struct tm *ti = localtime(&now);
    bool weekday = (ti->tm_wday >= 1 && ti->tm_wday <= 5);
    int h = ti->tm_hour;
    return weekday && (h >= 8 && h < 18);
}

static void start_wifi(void)
{    if (wifi_count == 0) return;
    setenv("TZ", "CST-8", 1);
    tzset();

    /* ★ 关闭 WiFi 省电模式（modem sleep）——关键！
     * ESP32 默认省电模式下周期休眠，BTWIFI6 这类企业级 AP 会把休眠设备
     * 判定为离线踢除（约 1-2 分钟一次，正是"摄像头几十秒卡死"的根因）。
     * 省电对桌面插电面板无意义，关闭后 WiFi 常驻在线。 */
    WiFi.setSleep(false);

    // 依次尝试列表中的 Wi-Fi，每个最多等 8 秒
    for (int i = 0; i < wifi_count; i++) {
        Serial.printf("[WiFi] 尝试连接 \"%s\" ...\n", wifi_list[i].ssid);
        WiFi.begin(wifi_list[i].ssid, wifi_list[i].password);
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
            delay(200);
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[WiFi] 已连接 \"%s\", IP=%s\n",
                          wifi_list[i].ssid, WiFi.localIP().toString().c_str());
            break;  // 连上了就不再试下一个
        }
        Serial.printf("[WiFi] \"%s\" 连接失败，尝试下一个\n", wifi_list[i].ssid);
        WiFi.disconnect(false);
    }

    configTime(8 * 3600, 0, "pool.ntp.org", "cn.ntp.org.cn");
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

    // SD 卡 + 本地日志 + 离线数据缓存（须在 ui_init 前：french_lib_init 需要 SD 已挂载；
    // 无卡时自动降级，不影响主功能）
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

    static uint32_t last_sec = 0;
    uint32_t now = millis();

    /* ----- 左键：会议页 -> 短按下页 / 长按筛选；其他页 -> 下一页(会议) ----- */
    if (g_btn_release) {
        g_btn_release = false;
        uint32_t dur = g_btn_dur_ms;
        bool is_sched = (ui_get_current_page() == 1);
        if (dur >= LONG_PRESS_MS) {
            if (is_sched) {
                if (Lvgl_lock(100)) { ui_schedule_toggle_filter(); Lvgl_unlock(); }
            } else if (Lvgl_lock(10)) {
                ui_next_page();
                Lvgl_unlock();
            }
        } else if (dur >= 40) {
            if (is_sched) {
                if (Lvgl_lock(100)) { ui_schedule_next_page(); Lvgl_unlock(); }
            } else if (ui_get_current_page() == 2) {
                /* Page 2：KEY 键切换到下一个和弦 + 播放音频 */
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

    /* ----- 右键：会议页 -> 短按上页 / 长按离开本页；其他页 -> 上一页(吉他) ----- */
    if (g_btn2_release) {
        g_btn2_release = false;
        uint32_t dur = g_btn2_dur_ms;
        bool is_sched = (ui_get_current_page() == 1);
        if (dur >= LONG_PRESS_MS) {
            if (is_sched) {
                /* 长按离开会议页，回到首页（不再需要硬重启） */
                if (Lvgl_lock(10)) { ui_goto_page(0); Lvgl_unlock(); }
            }
            else if (ui_get_current_page() == 2) {
                /* Page 2：BOOT 长按切换 OPEN / 7TH 和弦练习组 */
                if (Lvgl_lock(100)) { ui_ambient_next_group(); Lvgl_unlock(); }
            }
        } else if (dur >= 40) {
            if (is_sched) {
                if (Lvgl_lock(100)) { ui_schedule_prev_page(); Lvgl_unlock(); }
            } else if (ui_get_current_page() == 2) {
                /* Page 2：BOOT 短按播放当前和弦并增加练习计数 */
                if (Lvgl_lock(50)) { ui_ambient_tap(); Lvgl_unlock(); }
            } else if (Lvgl_lock(10)) {
                ui_prev_page();
                Lvgl_unlock();
            }
        }
    }

    /* ----- RLCD-004：USB-CDC 手势命令切页（M1 侧识别手指数量后下发 PAGE:xxx） -----
     * 命令由 cam_task 解析并登记，这里在主循环取走执行，切页与按键走同一套
     * Lvgl_lock + ui_goto_page 路径；已在目标页则不重复切，避免无谓刷屏。 */
    {
        int8_t req = cam_client_take_page_cmd();
        if (req >= 0 && req != (int8_t)ui_get_current_page()) {
            if (Lvgl_lock(100)) {
                ui_goto_page((uint8_t)req);
                Lvgl_unlock();
                Serial.printf("[cmd] page -> %d (gesture)\n", (int)req);
            }
        }
    }

    /* 进入页面检测：进入吉他页触发雷达扫描；进入会议页自动刷新日程 */
    {
        static uint8_t last_pg = 0xFF;
        uint8_t pg = ui_get_current_page();
        if (pg != last_pg) {
            if (pg == 2) ui_ambient_on_show();
            else if (pg == 1) { fetch_schedule_data(); }
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
                Serial.printf("WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
                /* 刚连上：立即拉取一次会议日程 + 直连天气 + 启动摄像头客户端 */
                g_last_sched = now;
                fetch_schedule_data();
                g_last_stocks = now;
                fetch_stocks_data();         /* 行情：连上即拉一次 */
                g_last_weather = now;
                weather_result_t wr = fetch_weather_data();
                g_weather_ready = (wr == WEATHER_OK);
                g_weather_permanent = (wr == WEATHER_PERMANENT);
                if (g_weather_ready) g_weather_fail_cnt = 0;
                else g_weather_fail_cnt++;
                cam_client_init();   // 幂等：内部防重复初始化，离线时任务自行退避重试
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
                    g_wifi_prev = true;   /* 让下一轮走"刚连上"分支立即拉数据 */
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
                /* 已连接且距上次拉取 >= 1 小时：周期刷新（仅工作时段） */
                if (now - g_last_sched >= 3600000UL) {
                    g_last_sched = now;
                    fetch_schedule_data();
                }
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
            } else {
                /* 非工作时段：不周期刷新。每 15 分钟轻量探测后端可达性（电脑开机检测）；
                 * 探测命中 → 强制全量刷新（天气/会议/行情），不等正常周期。 */
                if (now - g_last_backend_probe >= 15 * 60 * 1000UL) {
                    g_last_backend_probe = now;
                    if (probe_backend()) {
                        Serial.println("[probe] backend alive, force refresh");
                        g_last_sched = now;
                        fetch_schedule_data();
                        g_last_weather = now;
                        g_weather_ready = fetch_weather_data();
                        g_last_stocks = now;
                        fetch_stocks_data();
                    }
                }
            }
            /* 股票行情：交易时段（工作日 09:30-11:30 / 13:00-15:00）每 10 分钟刷新；
             * 非交易时段不刷新，保留最后一次数据。 */
            if (now - g_last_stocks >= 10 * 60 * 1000UL) {
                g_last_stocks = now;
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
                    if (trading) fetch_stocks_data();
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

        /* 会议 10 分钟预告：我的下一场会议进入 10 分钟窗口时，滴滴提醒一次 */
        {
            static time_t last_remind = 0;
            time_t mt = 0;
            if (ui_schedule_get_next_my_meeting_epoch(&mt)) {
                time_t diff = mt - time(nullptr);
                if (diff <= 600 && diff > -120 && mt != last_remind) {
                    last_remind = mt;
                    audio_play_beep();
                    Serial.println("[ambient] 会议 10 分钟预告提醒");
                }
            } else {
                last_remind = 0;   // 无未来会议，复位避免漏报
            }
        }
    }

    /* 注：SHTC3 温湿度现由 ui_clock 的 5s 定时器（LVGL 任务内）读取并更新，
     * 与 PCF85063A RTC 同享一根 I²C 总线，避免跨任务争用。 */

    delay(10);
}
