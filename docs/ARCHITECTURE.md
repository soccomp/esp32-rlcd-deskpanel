# 系统架构说明（ARCHITECTURE）

> 阅读顺序建议：`README.md`（概览）→ 本文（架构细节）→ `PROBLEM.md`（待解决问题与审查指引）→
> `docs/BOARD_REFERENCE.md` / `docs/LEARNING_NOTES.md`（硬件资料笔记）→ `docs/datasheets/`（官方数据手册）。

## 1. 系统总览

一个"桌面信息面板"：ESP32-S3 反射屏显示时间/天气/会议/股票行情，并实时预览 ESP32-CAM 摄像头画面。

```
┌─────────────────────────────┐         ┌──────────────────────────────┐
│  rlcd-lvgl/  ESP32-S3 固件   │  WiFi   │  Mac 后端 (parse_schedule.py) │
│  · LVGL v8 三页信息面板      │◄───────►│  · FastAPI/uvicorn :8100      │
│  · 时间/天气/会议/行情        │  mDNS   │  · 会议/天气/行情/摄像头帧代理  │
│  · 摄像头缩略预览 (1bit)      │         │  · launchd 常驻，日志 /tmp    │
└─────────────────────────────┘         └──────────────┬───────────────┘
                                                        │ WiFi (Mac 不受 AP RST 限制)
                                                        ▼
                                        ┌──────────────────────────────┐
                                        │  esp32-cam-fw/ ESP32-CAM 固件  │
                                        │  · AI-Thinker ESP32-CAM       │
                                        │  · /capture 单帧 /status 诊断  │
                                        │  · cam_grab_task 常驻抓帧      │
                                        └──────────────────────────────┘
```

## 2. 硬件组成

### 2.1 RLCD 板 —— Waveshare ESP32-S3-RLCD-4.2

| 外设 | 型号 | 总线 | 备注 |
|---|---|---|---|
| 主控 | ESP32-S3-WROOM-1 N16R8 | — | 16MB Flash + 8MB OPI PSRAM，双核 LX7 240MHz |
| 显示屏 | 4.2" 反射式单色液晶 | SPI | 驱动 IC **ST7305**，原生 300×400 竖屏 → 400×300 横屏，**无背光**，1-bit 黑白 |
| 温湿度 | SHTC3 | I²C 0x70 | |
| RTC | PCF85063A | I²C 0x51 | 掉电走时 |
| 音频 Codec | ES8311 | I²C 0x18 + I²S | 驱动扬声器（PA_EN 拉高才出声） |
| 存储 | microSD 卡槽 | SPI | 离线缓存 / 日志落盘 |
| 电池 | 18650 + ADC 分压 | GPIO4 | 3× 分压，软件换算 |
| 按键 | POWER×1 / BOOT×1 / KEY×1 | — | POWER 连 CHIP_PU 硬复位；BOOT=GPIO0、KEY=GPIO18（低有效） |

**关键 GPIO**（详见 `docs/BOARD_REFERENCE.md`）：

| GPIO | 功能 | GPIO | 功能 |
|---|---|---|---|
| 12/11 | SPI MOSI/SCK（屏） | 5/40/41 | 屏 DC/CS/RST |
| 13/14 | I²C SDA/SCL | 8/9/16/45 | I²S DOUT/BCLK/MCLK/LRCLK |
| 10 | I²S DIN（麦克风） | 46 | 扬声器 PA_EN |
| 18/0 | KEY / BOOT 按键 | 4 | 电池 ADC |

### 2.2 摄像头板 —— AI-Thinker ESP32-CAM

| 项 | 值 |
|---|---|
| 主控 | ESP32 classic（4MB Flash + 4MB PSRAM） |
| Sensor | **OV3660**（实测 PID 0x3660；资料常标 OV2640） |
| 接口 | DVP 并口：Y2..Y9=5,18,19,21,34,35,36,39；VSYNC=25 / HREF=23 / PCLK=22；XCLK=0；SIOD/SIOC=26/27 |
| 闪光灯 | GPIO4（低电平亮） |
| TF 槽 | SDMMC，32G SDHC/FAT32 实测可用（官方标注 ≤4G） |
| mDNS | 注册 `esp32cam.local` |

### 2.3 Mac（后端宿主）

- 运行 `parse_schedule.py`（FastAPI/uvicorn :8100），launchd 常驻（KeepAlive）。
- 作用是**网络代理 + 数据聚合**：会议/天气/行情抓外网，摄像头帧从摄像头抓取后转给 RLCD。

## 3. 通讯链路

### 3.1 板内总线（RLCD）

- **SPI** → ST7305 屏（400×300 1-bit，每 8 像素 1 字节；LVGL 单色 flush `<0x7fff→黑`、`≥0x7fff→白`）。
- **I²C** → RTC/温湿度/音频 Codec 寄存器（400kHz）。⚠️ I²C 访问全部收归 LVGL 任务（线程安全），主循环只置标志。
- **I²S** → ES8311 音频（16kHz，MCLK=256×16k=4.096MHz；`bits_per_chan` 必须 16BIT）。

### 3.2 网络链路（跨设备）

| 段 | 协议 | 说明 |
|---|---|---|
| RLCD → Mac | HTTP/JSON + 长连接 | mDNS 解析 Mac 主机名（缓存 5min，失败回退固定 IP） |
| Mac → 摄像头 | HTTP/JPEG | 后台线程每 500ms GET `/capture` 缓存最新帧 |
| 摄像头 → Mac | 同段 | 摄像头 WebServer 单客户端模型，长连接复用避免排队超时 |
| RLCD → 外网 | 直连（天气 Open-Meteo）或经 Mac 代理 | 交易时段每 10min 拉行情；会议/天气可直连或走代理 |

**为什么摄像头帧走 Mac 代理**：办公室 AP（BTWIFI6 系列）对 ESP32 出站 TCP 做 per-device RST，
RLCD→摄像头直连不稳定（errno 113）。Mac 不受限，两段（RLCD→Mac、Mac→摄像头）都稳定。详见 `PROBLEM.md`。

### 3.3 数据格式

- 会议：JSON `{meetings:[{date,weekday,time,location,title,host,attendees,organizer,is_video_conf,is_secret_conf,is_my_meeting}], statistics, update_time}`
- 行情：JSON `{stocks:[{name,code,value,pct,up}], updated}`（腾讯 3 指数 + 东财 AI 指数）
- 天气：JSON（QWeather，经 Mac 代理 `/api/weather_qh` 或 ESP32 直连 Open-Meteo）
- 视频帧：JPEG（640×480 quality≈18，单帧 <60KB）→ 板端解码为 1-bit 位图 → 最近邻缩放 200×150

## 4. 软件设计

### 4.1 RLCD 固件（rlcd-lvgl/，PlatformIO + Arduino + LVGL v8）

**UI**：三页 `lv_tileview`——首页(时间/天气/行情/摄像头预览) → 会议页(日程卡片) → 环境/吉他页(温湿度+和弦)。
状态栏：WiFi/电量/时间。全部中文字体为 C 数组（`gen_chinese_font.py` 生成，改动文案必须重新生成）。

**任务划分**（多核安全）：
| 任务/上下文 | 职责 |
|---|---|
| LVGL 任务 | 渲染、I²C（RTC/SHTC3/ES8311 初始化）、1s/5s 定时器 |
| 主循环 (loop) | WiFi 管理、数据拉取调度（schedule/weather/stocks 周期）、按键处理、页面切换 |
| cam_client 任务 | 独立 core1 任务：~500ms 拉 `/api/camframe`，JPEG 解码 → 1-bit 双缓冲（PSRAM） |
| 音频任务 | 独立任务 + 队列，非阻塞；吉他拨弦/音效 |

**数据流**：网络任务（非 LVGL）只写缓存/置标志 → LVGL 任务内渲染（`Lvgl_lock`）。关键缓存：
- `data_cache_*`：SD 卡离线快照（schedule/stocks/weather_qh），后端不可达时恢复
- `g_stk_*` / `g_wx_*`：行情/天气缓存，set（网络侧）与 update（LVGL 侧）分离
- 摄像头 1-bit 双缓冲（`g_th_src` / `g_th_dst`，PSRAM）

**关键坑（已固化）**：
- `DisplayPort` 必须 setup() 内 new（全局构造早于 PSRAM → 崩溃）
- LVGL 单色主题浅底深字；切页 `LV_ANIM_OFF`（反射屏慢）
- 无 `lv_obj_set_style_margin_*`（用 pad_row）；无 `lv_anim_create`（用 `lv_anim_t`+init）
- WiFi `setSleep(false)` 防企业 AP 踢除

### 4.2 Mac 后端（rlcd-lvgl/parse_schedule.py）

- **接口**：`/api/schedule`（会议）、`/api/stocks`（行情，60s 缓存+磁盘持久化）、`/api/weather_qh`（天气代理）、`/api/camframe`（帧代理，500ms 抓帧缓存）、`/api/camframe_stream`（MJPEG 流）。
- **行情缓存**：内存 `_stock_cache`（TTL 60s）+ 磁盘 `stocks_cache.json`（进程重启恢复）；全源失败且无缓存 → HTTP 503（不返回空数组，避免 ESP32 渲染假数据）。
- **帧代理**：后台线程每 500ms `GET esp32cam/capture`，校验 JPEG 有效后更新 `_cam_frame`；RLCD 长连接拉帧。
- **部署**：launchd `cn.qwenwork.schedule-api`（KeepAlive=true），环境变量 `CAM_CAPTURE_HOST` 可覆盖摄像头地址。

### 4.3 摄像头固件（esp32-cam-fw/，PlatformIO + esp32-cam 库）

- **帧缓存架构**：`cam_grab_task` 常驻抓帧入队列（`fb_get` 按需阻塞会卡死 HTTP）→ `/capture` 秒回最新帧。
- **接口**：`GET /capture`（单帧 JPEG）、`GET /status`（JSON：mac/frames/seq/last_len/pid/sd）。`seq` 持续增长 = DVP 正常；恒 0 = 排线/座子物理故障。
- **WiFi**：多网络轮询 + mDNS(`esp32cam`)；BTWIFI6 下静态 IP（防 DHCP 跳变），其余 DHCP。
- **看门狗**：loop 心跳卡死 >15s 或连续请求失败 → 软件重启。
- **分区**：`huge_app.csv`（必须，esp32-cam 库体积大）。

## 5. 功能清单

| 功能 | 入口 | 说明 |
|---|---|---|
| 数字时钟 + 日期 | 首页 | RTC PCF85063A，NTP 校准后写回；掉电走时 |
| 天气 | 首页/底部卡 | QWeather（代理）或 Open-Meteo（直连），指数退避重试 |
| 会议日程 | 会议页 | 3 张/屏卡片，下翻/筛选"全部⇄我的会议"，★视频 ▲涉密标记 |
| 股票行情 | 首页右上 | 上证/沪深300/创业板/AI指数，交易时段 10min 刷新，上涨黑底白字反显 |
| 摄像头预览 | 首页右下 | 200×150 1-bit 缩略图（Mac 代理通道） |
| 温湿度 | 环境页 | SHTC3 室内 + 室外天气 |
| 吉他拨弦 | 环境页 | 和弦循环 + 弦振动动画 + ES8311 音效 + 功德计数 |
| 法语短句 | 首页 | 每 10 分钟随机一句（50s 法语 + 10s 中文轮换） |

## 6. 按键交互

- 中键 PWR = 硬复位（回首页）。
- 左 KEY(GPIO18)：非会议页=下一页；会议页 短按=下翻卡、长按=切换筛选。
- 右 BOOT(GPIO0)：首页=上一页；吉他页=拨弦；会议页 短按=上翻卡、长按=刷新。
- ⚠️ 已知缺陷：进入会议页后无键可离开（页内操作占用了两键），只能 PWR 重启——待改进。

## 7. 配置与构建

| 配置 | 文件 | 说明 |
|---|---|---|
| WiFi 凭据 | `rlcd-lvgl/src/wifi_config.h`（本地，gitignored） | 模板 `wifi_config.example.h` |
| Mac 后端地址 | `rlcd-lvgl/src/local_config.h`（本地，gitignored） | 模板 `local_config.example.h` |
| 摄像头地址 | 环境变量 `CAM_CAPTURE_HOST`（默认 `esp32cam.local`） | launchd plist 中设置 |
| 城市/姓名 | 后端启动参数 `--name` / `--city` | |

构建：`platformio run`（RLCD 需自定义 board `boards/esp32-s3-rlcd.json`，PSRAM 必须 qio_opi）。
烧录：RLCD 用 USB-CDC（esptool 自动复位）；ESP32-CAM 需按住 IO0 上电进下载模式（`--before no_reset`）。

## 8. 文档索引

| 文档 | 内容 |
|---|---|
| `docs/BOARD_REFERENCE.md` | 板子 GPIO/外设/驱动要点（实机验证） |
| `docs/LEARNING_NOTES.md` | 官方资料清单、U8g2/LVGL 移植笔记、示例结构 |
| `docs/datasheets/*.pdf` | 官方数据手册：原理图 / ST7305 / ES8311 / PCF85063 / SHTC3 |
| `PROBLEM.md` | 摄像头→RLCD 显示问题：症状、已排查结论、请审查点 |

> 版权说明：datasheets 与学习笔记整理自 Waveshare 官方公开资料（https://docs.waveshare.net/ESP32-S3-RLCD-4.2/ ），
> 仅用于本项目开发与学习。
