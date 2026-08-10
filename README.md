# ESP32-RLCD DeskPanel

桌面信息面板：Waveshare ESP32-S3-RLCD-4.2 反射屏（400×300 单色，无背光）+ ESP32-CAM 摄像头实时预览。

```
┌─────────────────────────────┐         ┌──────────────────────────────┐
│  rlcd-lvgl/  ESP32-S3 固件   │  WiFi   │  Mac 后端 (parse_schedule.py) │
│  · 3页 tileview 信息面板     │◄───────►│  · :8100 /api/schedule        │
│  · 时间/天气/会议/行情        │  mDNS   │  · :8100 /api/stocks          │
│  · 摄像头缩略预览 (1bit)      │         │  · :8100 /api/camframe 帧代理  │
└─────────────────────────────┘         └──────────────┬───────────────┘
                                                        │ WiFi (Mac 不受 RST 限制)
                                                        ▼
                                        ┌──────────────────────────────┐
                                        │  esp32-cam-fw/ ESP32-CAM 固件  │
                                        │  · /capture 单帧 /status 诊断  │
                                        │  · cam_grab_task 常驻抓帧      │
                                        └──────────────────────────────┘
```

## 目录结构

| 路径 | 内容 |
|---|---|
| `rlcd-lvgl/` | RLCD 固件（PlatformIO，Arduino 框架）+ Mac 后端 `parse_schedule.py` + 字体/图标生成脚本 |
| `esp32-cam-fw/` | ESP32-CAM 固件（esp32-cam 库）+ 串口直连工具（`uart_cam_*.py`，1M 波特率帧协议） |

## 硬件

- **RLCD**：Waveshare ESP32-S3-RLCD-4.2，ESP32-S3-WROOM-1-N16R8（16MB Flash + 8MB OPI PSRAM），ST7305 反射屏 400×300 无背光。SPI: MOSI=12 / SCK=11 / DC=5 / CS=40 / RST=41。
- **音频**：ES8311（I²C 0x18）+ I²S（DOUT=8 / BCLK=9 / DIN=10 / MCLK=16 / LRCLK=45 / PA_EN=46）。
- **RTC/Sensor**：PCF85063A（0x51）、SHTC3（0x70），I²C SDA=13 / SCL=14。
- **摄像头**：AI-Thinker ESP32-CAM（OV3660），闪光灯 GPIO4，TF 槽 32G SDHC 实测可用。
- **按键**：中键 PWR=CHIP_PU 硬复位；左 KEY=GPIO18、右 BOOT=GPIO0（低有效）。

## 构建与烧录

```bash
# RLCD（注意：需自定义 board，详见 rlcd-lvgl/boards/）
cd rlcd-lvgl && platformio run -e esp32-s3-rlcd && platformio run -e esp32-s3-rlcd -t upload

# ESP32-CAM（注意：huge_app.csv 分区；下载模式需按住 IO0 上电）
cd esp32-cam-fw && platformio run && platformio run -t upload
```

中文字体为 C 数组（`src/lv_font_chinese_*.c`），改动文案后用 `gen_chinese_font.py` 重新生成。

## 配置（本地化，不提交）

所有真实凭据/主机名都在**本地文件**里（已被 `.gitignore` 忽略），公开仓库只有占位模板：

| 模板文件 | 复制为 | 内容 |
|---|---|---|
| `rlcd-lvgl/src/wifi_config.example.h` | `wifi_config.h` | WiFi SSID/密码列表 |
| `rlcd-lvgl/src/local_config.example.h` | `local_config.h` | Mac 后端 mDNS 主机名 / 局域网 IP |

Mac 后端环境变量：`CAM_CAPTURE_HOST`（摄像头地址，默认 `esp32cam.local`）。

## Mac 后端

```bash
python3 parse_schedule.py --host 0.0.0.0 --port 8100 --name 你的名字 --city 城市
```

提供接口：`/api/schedule`（会议）、`/api/stocks`（指数行情）、`/api/weather_qh`（天气）、`/api/camframe`（摄像头帧代理）、`/api/camframe_stream`（MJPEG 流）。

> 摄像头帧通道的架构与已知问题，见 **[PROBLEM.md](PROBLEM.md)**（本项目公开的核心目的：请外部 AI 审查摄像头→RLCD 显示链路）。
