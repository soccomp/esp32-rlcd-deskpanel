# ESP32-RLCD DeskPanel

一个**桌面信息面板**：Waveshare ESP32-S3-RLCD-4.2 反射屏（400×300 单色、无背光）显示时间/天气/股票行情/法语学习卡，并实时预览 ESP32-CAM 摄像头画面（USB 全链路）。

> 📖 **给代码审查者（AI）**：想快速理解全貌，请按顺序读
> `README.md`（本文件）→ **`docs/ARCHITECTURE.md`**（硬件/通讯/软件设计/功能详解）→ **`PROBLEM.md`**（摄像头→RLCD 显示问题与审查指引）→
> `docs/BOARD_REFERENCE.md` + `docs/LEARNING_NOTES.md` + `docs/datasheets/`（硬件官方资料）。

## 系统概览

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

**三段角色**：
- **RLCD（ESP32-S3）**：显示终端 + 数据消费者（时间/天气/行情/法语卡/摄像头画面）。
- **M1 Mac**：摄像头 USB 桥接（`camusb_bridge.py`）+ 手势切页（`finger_page_control.py`）+ 天气软回退后端。
- **ESP32-CAM**：图像采集端（OV3660 sensor，DVP 并口，JPEG 输出，USB-TTL 串口上送）。

## 硬件组成

| 板卡 | 芯片/器件 | 关键接口 |
|---|---|---|
| RLCD 板 | ESP32-S3-WROOM-1 N16R8（16MB Flash + 8MB OPI PSRAM） | — |
| | ST7305 反射屏 400×300 1-bit | SPI（MOSI=12/SCK=11/DC=5/CS=40/RST=41） |
| | PCF85063A RTC / SHTC3 温湿度 / ES8311 音频 Codec | I²C（SDA=13/SCL=14）+ I²S（DOUT=8/BCLK=9/MCLK=16/LRCLK=45/PA_EN=46） |
| | microSD / 18650 电池(ADC=GPIO4) / 三按键 | PWR=CHIP_PU 硬复位、KEY=GPIO18、BOOT=GPIO0 |
| 摄像头板 | ESP32 classic + **OV3660** sensor + 4MB PSRAM | DVP（VSYNC=25/HREF=23/PCLK=22/Y2..Y9），闪光灯 GPIO4，TF 32G |
| Mac | Python 后端进程（uvicorn :8100） | mDNS `esp32cam.local` / Mac mDNS 主机名 |

## 通讯链路

1. **板内**：SPI→屏幕；I²C→RTC/温湿度/Codec；I²S→音频（16kHz，MCLK=4.096MHz）。
2. **RLCD → 外网**：HTTPS 直连——行情走腾讯 `qt.gtimg.cn` + 东财 `push2.eastmoney.com`（**固件直连，不走 Mac**）；天气 QWeather 直连为主，失败回退 Mac `/api/weather_qh`。
3. **ESP32-CAM → M1**：USB-TTL 串口 1M，帧协议 `AA55 5AA5|len(2B BE)|JPEG|crc16(2B BE)`，640×480 ~2fps；`camusb_bridge.py` 常驻桥接。
4. **M1 → RLCD**：USB-CDC，bridge 回写 hub 消息（JPEG 帧 + `PAGE:X`/`ACK:PAGE:X` 命令）。

> ⚠️ 摄像头帧走 **USB 全链路**（2026-08-20 定案）：办公室 AP（BTWIFI6 系列）对 ESP32 出站 TCP 做 per-device RST，WiFi 直连 6 版固件均未根治周期楔死；USB-TTL + bridge 绕开 WiFi 数据面。详见 `PROBLEM.md`。

## 软件设计（摘要，详见 docs/ARCHITECTURE.md）

- **RLCD 固件**（Arduino + LVGL v8）：三页 `lv_tileview`（首页/环境吉他页/摄像头页）；多任务（LVGL / 主循环 / rx_task / cam_task / 音频）；网络侧写缓存 + LVGL 侧渲染（`Lvgl_lock`）；SD 离线缓存；中文字体 C 数组；LVGL 走 registry `lvgl@8.4.0` + `scripts/patch_lvgl.py` 补丁钩子（可复现）。
- **M1 Mac**：`camusb_bridge.py`（USB 桥接，hub :8770，launchd 常驻）+ `finger_page_control.py`（MediaPipe 手势→页面命令，ACK 闭环）+ `parse_schedule.py`（FastAPI :8100，天气软回退）。
- **摄像头固件**：`cam_grab_task` 常驻抓帧 → `uart_frame_task` 串口 1M 上送；`/status` 的 `seq` 为帧通道判据；看门狗软重启。

## 功能清单

数字时钟/日期/农历（RTC+NTP）· 天气（室内 SHTC3 + 室外 QWeather 直连）· 股票行情（4 指数，固件直连腾讯/东财）· 法语学习卡（200 组中法对话，5 分钟轮换）· 摄像头预览（USB 全链路 200×150 1-bit + 全屏页）· 吉他拨弦（和弦+音效+功德计数）· 状态栏 CAM 指示（●/○）· 手势切页（1/2/3 指）。

## 目录结构

| 路径 | 内容 |
|---|---|
| `rlcd-lvgl/` | RLCD 固件 + Mac 后端 `parse_schedule.py` + 字体/图标生成脚本（`gen_*.py`） |
| `esp32-cam-fw/` | ESP32-CAM 固件 + USB 桥接/串口工具（`camusb_bridge.py`、`uart_cam_*.py`，1M 波特率帧协议） |
| `docs/` | **`ARCHITECTURE.md`**（架构详解）、`BOARD_REFERENCE.md`、`LEARNING_NOTES.md`、`execution_reports/`、`datasheets/`（官方 PDF） |
| `PROBLEM.md` | 摄像头→RLCD 显示问题：症状 / 已排查结论 / 请审查点 |

## 构建与烧录

```bash
# RLCD（自定义 board 见 rlcd-lvgl/boards/；PSRAM 必须 OPI）
cd rlcd-lvgl && platformio run -e esp32-s3-rlcd && platformio run -e esp32-s3-rlcd -t upload

# ESP32-CAM（huge_app.csv 分区；按住 IO0 上电进下载模式）
cd esp32-cam-fw && platformio run && platformio run -t upload
```

中文字体为 C 数组（`src/lv_font_chinese_*.c`），改动文案后用 `gen_chinese_font.py` 重新生成。

## 配置（本地化，不提交）

真实凭据/主机名在**本地 gitignored 文件**中，仓库只有占位模板：

| 模板 | 复制为 | 内容 |
|---|---|---|
| `rlcd-lvgl/src/wifi_config.example.h` | `wifi_config.h` | WiFi SSID/密码 |
| `rlcd-lvgl/src/local_config.example.h` | `local_config.h` | Mac 后端 mDNS 主机名 / IP |

后端环境变量：`CAM_CAPTURE_HOST`（摄像头地址，默认 `esp32cam.local`）。

## Mac 后端运行

```bash
python3 parse_schedule.py --host 0.0.0.0 --port 8100 --name 你的名字 --city 城市
```

> 摄像头帧通道的架构与已知问题见 **[PROBLEM.md](PROBLEM.md)**——本项目公开的核心目的：请外部 AI 审查"摄像头→RLCD 显示"链路。
