# PROBLEM.md — 摄像头画面到 RLCD 显示的问题（请 AI 审查）

> 本文件写给代码审查者（AI 或人）。目标：理解摄像头画面如何从 ESP32-CAM 走到 RLCD 屏幕，
> 已知的症状、已排查结论，以及我们最想请你审查/改进的点。
> **重点文件：`rlcd-lvgl/src/cam_client.cpp`、`rlcd-lvgl/src/ui_camera.cpp`、
> `rlcd-lvgl/src/main.cpp`（rx_task/cam_task）、`esp32-cam-fw/src/main.cpp`（uart_frame_task）、
> `esp32-cam-fw/camusb_bridge.py`。**

## 一、当前链路（USB 全链路，2026-08-20 定案）

办公室 AP（BTWIFI6 系列）对 ESP32 出站 TCP 做 per-device RST，WiFi 直连摄像头 6 版固件均未根治
周期楔死（ESP32 报 CONNECTED 但收不到下行帧）。因此视频**不走 WiFi**，改 USB-TTL 串口：

```
ESP32-CAM                          M1 Mac                                    RLCD (ESP32-S3)
┌──────────────────┐  USB-TTL 1M   ┌──────────────────────────────┐  USB-CDC   ┌──────────────────────┐
│ cam_grab_task    │ 帧协议        │ camusb_bridge.py            │  hub 回写  │ rx_task (core0,prio6)│
│ uart_frame_task  │──────────────►│ · 读摄像头串口帧 → hub:8770 │───────────►│ · 解析帧 + 命令 + ACK │
│ JPEG 640×480     │ AA55 5AA5|len │ · 收 hub 消息 → RLCD USB-CDC│            │ cam_task (core1,prio4)│
└──────────────────┘ |JPEG|crc16   └──────────────────────────────┘            │ · 只解码 JPEG        │
                                                                              └──────────────────────┘
```

- **帧协议**：`AA55 5AA5 | len(2B BE) | JPEG | crc16(2B BE)`，1M 波特率（921600 对 CH340 非整数分频会乱码）。
- **RLCD 双核解耦**：`rx_task`(core0) 读 USB-CDC + 解析 + 回 `ACK:PAGE:X`；`cam_task`(core1) 只解码 JPEG。
  PSRAM 环形帧槽 4×96KB + 独立接收缓冲，**环满丢帧不覆盖**（覆盖未读槽 → 解码坏帧 → 崩 → USB 断）。
- **判活**：bridge 日志 `ok~10 bad~0-2 ~2fps`；RLCD 串口 `[cam-ui] new seq=N invalidate+refr`。
  假卡死判据：USB 模式画面定格但时间仍刷新 = bridge 没跑（`/tmp/camusb_bridge.log` mtime 停滞），非固件 bug。

## 二、用户报告的症状（按困扰程度）

1. **画面异常模糊，灰阶点阵基本无法识别** —— 最困扰。预览几乎看不出内容，只有明暗点阵。
2. **黑屏 / 无画面** —— 有时完全没画面。
3. **卡顿 / 延迟明显** —— 帧率低，画面不流畅。
4. **USB 断流/重启** —— 环满覆盖未读槽导致解码坏帧崩，或 rx_task 饿死 core0 触发 TWDT。

## 三、已排查结论（避免重复劳动）

- **帧通道判据**：摄像头 `/status` 的 `seq` 持续增长 = DVP 帧通道正常；恒 0 = 排线/座子物理故障（曾因此换新板）。
- **USB 模式坏帧 0-2 不累积属正常**；持续增长才是问题。
- **rx_task 必须 `vTaskDelay(2)` 显式让出**：持续 pump 占满 core0 饿死 idle → TWDT esp_restart
  （判据：seq 反复归零 + decode failed 频发）。
- **JPEG 质量**：VGA 640×480 quality≈18，单帧 <60KB，640×480 约 2fps。
- **串口直连已验证**：连续 500 帧完整无错——摄像头硬件本身无问题，问题在软件链路。
- **WiFi 楔死真因**：AP 下发 deauth/管理帧被旧 esp-idf WiFi 固件忽略 → ESP32 报 CONNECTED 但收不到
  下行帧；重关联即恢复（周期 ~14s）。已排除频段干扰/AP 射频/40MHz/A-MPDU。RLCD 兜底 = 12s 定时
  `wifi_hard_restart()`（定 BSSID+信道直连 1~3s + 慢路径兜底）。摄像头已切 USB 全链路。

## 四、请重点审查的问题

1. **画面模糊/灰阶点阵（最想解决）**
   - `scale_1bit()` 最近邻缩放：320×240 → 200×150 后直接 1-bit 阈值化，是否存在信息损失过大？
   - 1-bit 渲染是否有抖动/阈值处理？无背光反射屏上，是否应该用 **Bayer/Floyd-Steinberg 抖动**（dithering）
     或自适应阈值来保留画面结构？
   - JPEG 解码质量（`jpg_decode`）是否用了低质量配置？
   - 是否值得在 200×150 时用 2-bit/4-bit 灰度 + 屏上抖动？硬件只支持 1-bit，怎么权衡？
2. **卡顿/延迟**
   - USB 帧率 ~2fps 的瓶颈在哪？PSRAM 环形槽 4×96KB 与解码节奏是否合理？
   - `cam_task` 的 JPEG 解码是否阻塞了 LVGL 任务？
3. **USB 断流/稳定性**
   - rx_task 与 cam_task 的同步（环形槽）在满速下是否安全？环满丢帧策略是否最优？
   - bridge（`camusb_bridge.py`）断线重连逻辑在摄像头/RLCD 拔插时的健壮性？
4. **摄像头固件**（`esp32-cam-fw/src/main.cpp`）
   - `cam_grab_task` 帧队列设计、JPEG 缓冲 96KB 是否够、串口 1M 发送节奏。
   - `#ifndef CAM_WIFI_ONLY` 分支下 wifi_reassoc_task 关闭后的诊断能力（/status 仍在）。

## 五、我们尝试过的方向（已放弃/未完成）

- **WiFi 直连摄像头**（v2-v7 六版固件）：受 BTWIFI6 RST 周期楔死，均未根治 → 8-20 切 USB 全链路。
- **RLCD→摄像头直连**（WiFi）：受 BTWIFI6 RST 影响，不可靠。
- 摄像头免按键自举（'D' 字符 GPIO0 拉低 + `gpio_hold_en` + `esp_restart`）实测失败：
  SW_RESET 会清除 hold，需 deep sleep 方案（未验证）。

## 六、环境信息

- 办公室 AP：BTWIFI6 系列（对 ESP32 出站 TCP 有 RST）。
- 摄像头：ZAVE AI-Thinker ESP32-CAM，OV3660，USB-TTL（CH340 `1A86:7523`）接 M1。
- M1 常驻：`camusb_bridge.py`（launchd `cn.qwenwork.camusb-bridge`）+ `parse_schedule.py`（:8100）。
- RLCD：USB-CDC（S3 CDC `303A:*`），烧录前 `kill -STOP <bridge PID>` 冻结、烧完 kickstart 重启。
