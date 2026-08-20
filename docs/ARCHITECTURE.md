# 系统架构说明（ARCHITECTURE）

> 阅读顺序建议：`README.md`（概览）→ 本文（架构细节）→ `PROBLEM.md`（待解决问题与审查指引）→
> `docs/BOARD_REFERENCE.md` / `docs/LEARNING_NOTES.md`（硬件资料笔记）→ `docs/datasheets/`（官方数据手册）。

## 1. 系统总览

一个"桌面信息面板"：ESP32-S3 反射屏显示时间/天气/股票行情/法语学习卡，并实时预览 ESP32-CAM 摄像头画面（USB 全链路）。

```
┌─────────────────────────────┐   USB-CDC    ┌──────────────────────────────┐
│  rlcd-lvgl/  ESP32-S3 固件   │◄───────────►│  M1 Mac                     │
│  · LVGL v8 三页信息面板      │  串口帧+命令 │  · camusb_bridge (hub 8770)  │
│  · 时间/天气/行情/法语卡      │              │  · finger_page_control.py    │
│  · 摄像头预览 (USB 全链路)    │              │  · parse_schedule.py :8100   │
└────────────┬────────────────┘              └──────────────┬───────────────┘
             │ HTTPS 直连                                    │ USB-TTL 1M (帧协议)
             ▼                                              ▼
   ┌─────────────────────┐                       ┌──────────────────────┐
   │ 腾讯 qt.gtimg.cn     │                       │ esp32-cam-fw/         │
   │ 东财 push2.eastmoney │                       │ ESP32-CAM (OV3660)    │
   │ QWeather (天气)      │                       │ uart_frame_task 串口帧 │
   └─────────────────────┘                       └──────────────────────┘
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

### 2.3 Mac（M1 宿主）

- 运行 `parse_schedule.py`（FastAPI/uvicorn :8100），launchd 常驻（KeepAlive）。
- 作用是**数据软回退 + 摄像头 USB 桥接**：天气主路径已改 QWeather 直连，Mac `/api/weather_qh` 降级为软 fallback；
  会议接口 `/api/schedule` 已随会议页移除而不再被固件调用；摄像头帧走 USB 全链路（`camusb_bridge.py` 常驻，hub :8770）。

## 3. 通讯链路

### 3.1 板内总线（RLCD）

- **SPI** → ST7305 屏（400×300 1-bit，每 8 像素 1 字节；LVGL 单色 flush `<0x7fff→黑`、`≥0x7fff→白`）。
- **I²C** → RTC/温湿度/音频 Codec 寄存器（400kHz）。⚠️ I²C 访问全部收归 LVGL 任务（线程安全），主循环只置标志。
- **I²S** → ES8311 音频（16kHz，MCLK=256×16k=4.096MHz；`bits_per_chan` 必须 16BIT）。

### 3.2 网络链路（跨设备）

| 段 | 协议 | 说明 |
|---|---|---|
| RLCD → 外网 | HTTPS 直连 | 行情：腾讯 `qt.gtimg.cn` + 东财 `push2.eastmoney.com`（**固件直连，不走 Mac**）；天气：QWeather 直连为主，失败回退 Mac `/api/weather_qh` |
| ESP32-CAM → M1 | USB-TTL 串口 1M | 帧协议 `AA55 5AA5\|len(2B BE)\|JPEG\|crc16(2B BE)`，640×480 ~2fps；`camusb_bridge.py` 桥接 hub :8770 |
| M1 → RLCD | USB-CDC | bridge 将 hub 消息写回 RLCD USB-CDC；RLCD 侧 `cam_client` 解析帧 + `PAGE:X`/`ACK:PAGE:X` 命令 |
| RLCD → Mac | HTTP/JSON | 软 fallback（天气）与 `/api/camframe` 诊断通道（已非主链路） |

**为什么摄像头帧走 USB 全链路（2026-08-20 定案）**：办公室 AP（BTWIFI6 系列）对 ESP32 出站 TCP 做 per-device RST，
WiFi 直连摄像头 6 版固件均未根治周期楔死。USB-TTL 串口帧 + bridge 中转绕开 WiFi 数据面，实测 `ok~10 bad~0-2 ~2fps` 稳定。

### 3.3 数据格式

- 行情：JSON `{stocks:[{name,code,value,pct,up}], updated}`（腾讯 qt.gtimg.cn 3 指数 + 东财 AI 指数，**固件直连**）
- 天气：JSON（QWeather 直连为主，失败回退 Mac `/api/weather_qh`）
- 法语卡：200 组中法双语对话（`french_dialogues.h`，每 5 分钟换一组）
- 视频帧：JPEG（640×480 quality≈18，单帧 <60KB）经 USB-TTL 串口 → M1 bridge → RLCD USB-CDC → 板端解码为 1-bit 位图 → 最近邻缩放 200×150

## 4. 软件设计

### 4.1 RLCD 固件（rlcd-lvgl/，PlatformIO + Arduino + LVGL v8）

**UI**：三页 `lv_tileview`——首页(时间/天气/行情/法语学习卡/摄像头预览) → 环境/吉他页(温湿度+和弦) → 摄像头页(全屏画面)。
状态栏：日期(MM-DD+中文星期+农历)/WiFi/电量/CAM 指示(●/○)/时间。全部中文字体为 C 数组（`gen_chinese_font.py` 生成，改动文案必须重新生成）。

> ⚠️ 会议页已整体移除（2026-08，`ui_schedule.cpp/h`、`schedule_data.h` 删除）。页面顺序 0→1→2→0。

**任务划分**（多核安全）：
| 任务/上下文 | 职责 |
|---|---|
| LVGL 任务 | 渲染、I²C（RTC/SHTC3/ES8311 初始化）、1s/5s 定时器 |
| 主循环 (loop) | WiFi 管理（含 12s 预防性重关联 `wifi_hard_restart`）、数据拉取调度（weather/stocks 周期）、按键处理、页面切换 |
| rx_task (core0,prio6) | 读 USB-CDC RX + 解析帧/命令 + 回 ACK（`PAGE:X`→`ACK:PAGE:X`）；循环内必须 `vTaskDelay(2)` 让出 |
| cam_task (core1,prio4) | 只解码 JPEG；PSRAM 环形帧槽 4×96KB + 独立接收缓冲，环满丢帧不覆盖 |
| 音频任务 | 独立任务 + 队列，非阻塞；吉他拨弦/音效 |

**S3 双核解耦**：rx_task 与 cam_task 分离——单任务"解码期间不读 RX"必积压丢命令，双核是正解。

**数据流**：网络任务（非 LVGL）只写缓存/置标志 → LVGL 任务内渲染（`Lvgl_lock`）。关键缓存：
- `data_cache_*`：SD 卡离线快照（stocks/weather_qh），后端不可达时恢复
- `g_stk_*` / `g_wx_*`：行情/天气缓存，set（网络侧）与 update（LVGL 侧）分离
- 摄像头 1-bit 双缓冲（`g_th_src` / `g_th_dst`，PSRAM）

**关键坑（已固化）**：
- `DisplayPort` 必须 setup() 内 new（全局构造早于 PSRAM → 崩溃）
- LVGL 单色主题浅底深字；切页 `LV_ANIM_OFF`（反射屏慢）
- 无 `lv_obj_set_style_margin_*`（用 pad_row）；无 `lv_anim_create`（用 `lv_anim_t`+init）
- WiFi `setSleep(false)` 防企业 AP 踢除；**12s 定时 `wifi_hard_restart()` 预防性重关联**（AP 下发 deauth 被旧固件忽略 → ESP32 报 CONNECTED 但收不到下行帧，重关联即恢复）
- LVGL 依赖 registry `lvgl/lvgl@8.4.0` + `scripts/patch_lvgl.py` 钩子（重放偶数 flush 块高补丁，ST7305 旋转路径必需）

### 4.2 Mac 后端（rlcd-lvgl/parse_schedule.py）

- **接口**：`/api/weather_qh`（天气软回退，主路径已改 QWeather 直连）、`/api/camframe`（帧代理，已非主链路）、`/api/schedule`（历史接口，会议页移除后不再被固件调用）。
- **股票接口 `/api/stocks` 已弃用**（2026-08-20）：固件改为直连腾讯/东财 HTTPS，Mac 不再代理行情。
- **部署**：launchd `cn.qwenwork.schedule-api`（KeepAlive=true），环境变量 `CAM_CAPTURE_HOST` 可覆盖摄像头地址。

### 4.3 M1 摄像头桥接（camusb_bridge.py + finger_page_control.py）

- **`camusb_bridge.py`**：USB 串口桥，自动发现摄像头（CH340 `1A86:7523`）与 RLCD（S3 CDC `303A:*`），断线重连；launchd `cn.qwenwork.camusb-bridge`（KeepAlive）。把摄像头串口帧写 hub :8770，把 hub 命令回写 RLCD USB-CDC。
- **`finger_page_control.py`**：MediaPipe 数指（1/2/3 指 → PAGE:HOME/GUITAR/CAMERA，手指数=页面序号+1）；滚动窗口 3s + 多数表决(min_agree=3) + 冷却 1.5s；ACK 闭环（`confirmed_page` 只在收到 `ACK:PAGE:X` 后更新，超时 2.0s 重发最多 3 次）。

### 4.4 摄像头固件（esp32-cam-fw/，PlatformIO + esp32-cam 库）

- **帧缓存架构**：`cam_grab_task` 常驻抓帧（`fb_get` 按需阻塞会卡死）→ 串口 `uart_frame_task` 以 1M 波特率送出帧协议（`AA55 5AA5|len(2B BE)|JPEG|crc16(2B BE)`）；`uart_cmd_task` 收 USB 命令。
- **接口**：`GET /status`（JSON：mac/frames/seq/last_len/pid/sd，WiFi 仅留诊断用）；`seq` 持续增长 = DVP 帧通道正常；恒 0 = 排线/座子物理故障。
- **USB 全链路（2026-08-20 定案）**：`platformio.ini` 去掉 `-DCAM_WIFI_ONLY`（启 uart_frame_task/uart_cmd_task），`main.cpp` 用 `#ifndef CAM_WIFI_ONLY` 关掉 wifi_reassoc_task——摄像头 WiFi 只保留 /status 诊断，视频走 USB-TTL 串口 → M1 bridge → RLCD。
- **看门狗**：loop 心跳卡死 >15s 或连续请求失败 → 软件重启。
- **分区**：`huge_app.csv`（必须，esp32-cam 库体积大）。

## 5. 功能清单

| 功能 | 入口 | 说明 |
|---|---|---|
| 数字时钟 + 日期 | 首页 | RTC PCF85063A，NTP 校准后写回；掉电走时；状态栏含农历（1900-2099 查表） |
| 天气 | 首页/左下卡 | QWeather 直连为主，失败回退 Mac `/api/weather_qh`，指数退避重试 |
| 股票行情 | 首页右上 | 上证/沪深300/创业板/AI指数，**固件直连腾讯/东财**，交易时段 10min 刷新，上涨反色 |
| 法语学习卡 | 首页右下 | **200 组中法双语对话**（200×150），5 分钟换一组，右下角 N/200 计数 |
| 摄像头预览 | 首页/摄像头页 | **USB 全链路**（串口→bridge→USB-CDC）200×150 1-bit 缩略图 / 全屏 |
| 温湿度 | 环境页 | SHTC3 室内 + 室外天气 |
| 吉他拨弦 | 环境页 | 和弦循环 + 弦振动动画 + ES8311 音效 + 功德计数 |
| 状态栏 CAM 指示 | 全页 | ●/○（反色）由 1s 时钟定时器驱动 |

## 6. 按键交互

- 中键 PWR = 硬复位（回首页）。
- 左 KEY(GPIO18)：短按=下一页；吉他页短按=切和弦；长按=下一页。
- 右 BOOT(GPIO0)：短按=上一页；吉他页短按=拨弦（Cmaj7→Am7→Dm7→G7+弦振+音效）、长按=切和弦练习组(OPEN/7TH)。
- 另可手势切页：1/2/3 指 → 首页/吉他页/摄像头页（`finger_page_control.py` + ACK 闭环）。

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
| `docs/execution_reports/*.md` | 各任务单执行报告（RLCD-004 / 004.1 / 004.2） |
| `PROBLEM.md` | 摄像头→RLCD 显示问题：症状、已排查结论、请审查点 |

> 版权说明：datasheets 与学习笔记整理自 Waveshare 官方公开资料（https://docs.waveshare.net/ESP32-S3-RLCD-4.2/ ），
> 仅用于本项目开发与学习。
