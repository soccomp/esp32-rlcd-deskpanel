# PROBLEM.md — 摄像头画面到 RLCD 显示的问题（请 AI 审查）

> 本文件写给代码审查者（AI 或人）。目标：理解摄像头画面如何从 ESP32-CAM 走到 RLCD 屏幕，
> 已知的症状、已排查结论，以及我们最想请你审查/改进的点。
> **重点文件：`rlcd-lvgl/src/ui_camera.cpp`、`rlcd-lvgl/src/cam_client.cpp`、
> `rlcd-lvgl/parse_schedule.py`（/api/camframe）、`esp32-cam-fw/src/main.cpp`。**

## 一、用户报告的症状（按困扰程度）

1. **画面异常模糊，灰阶点阵基本无法识别** —— 最困扰。预览几乎看不出内容，只有明暗点阵。
2. **黑屏 / 无画面** —— 有时完全没画面。
3. **卡顿 / 延迟明显** —— 帧率低，画面不流畅。
4. **WiFi 通道断流** —— 网络方式下不稳定，会断开。

## 二、当前架构（三段链路）

```
RLCD (ESP32-S3)                          Mac (后端代理)                     ESP32-CAM
┌──────────────────────┐   keep-alive   ┌──────────────────────┐   WiFi   ┌──────────────────┐
│ cam_client.cpp       │  HTTP GET      │ parse_schedule.py    │  HTTP    │ esp32-cam-fw     │
│ mDNS→Mac → :8100     │───────────────►│ /api/camframe        │────────►│ /capture (单帧)  │
│ /api/camframe        │  ~500ms 轮询   │ 后台线程每 500ms 抓帧 │          │ cam_grab_task    │
│ 1bit 缩放→ LVGL 渲染 │                │ 缓存最新 JPEG        │          │ 常驻抓帧队列     │
└──────────────────────┘                └──────────────────────┘          └──────────────────┘
```

- **为什么走 Mac 代理而不是直连**：办公室 AP（BTWIFI6 系列）对 ESP32 出站 TCP 做 per-device RST，
  RLCD→摄像头直连（`esp32cam:80`）不稳定（errno 113）。Mac 是普通电脑不受 RST 限制，
  RLCD→Mac 与 Mac→摄像头两段都稳定，因此定案为后端代理。
- RLCD 侧解析：mDNS 查 Mac 主机名（缓存 5 分钟）→ 失败回退固定 IP。
- 屏幕是 **ST7305 反射屏，400×300 单色 1-bit，无背光**——最终只能渲染黑白点阵。

## 三、显示链路细节（RLCD 侧）

1. `cam_client.cpp`：`cam_grab_task` 后台任务每 ~500ms GET `/api/camframe`（长连接 keep-alive），
   收到 JPEG 后解码为 1-bit 位图，写入 PSRAM 双缓冲（`g_th_src` / `g_th_dst`）。
2. `ui_camera.cpp`：`scale_1bit()` —— **1-bit 最近邻缩放** 320×240 → 200×150（`CAM_DISP_W×CAM_DISP_H`），
   渲染到首页右下角摄像头缩略卡（196,116 位置 200×160 区域）。
3. 刷新由 LVGL 定时器驱动，非 LVGL 任务改对象需 `Lvgl_lock`。

## 四、已排查结论（避免重复劳动）

- **帧通道判据**：摄像头 `/status` 的 `seq` 字段持续增长 = DVP 帧通道正常；
  `seq` 恒 0 = 排线/座子物理故障（曾因此换新板）。
- **`/capture` 秒回**：摄像头用 `cam_grab_task` 常驻抓帧 + 队列，`/capture` 只取最新帧；
  `fb_get` 按需阻塞式抓帧会卡死 HTTP 服务（已踩过）。
- **JPEG 质量**：VGA 640×480 quality≈18，单帧 <60KB。
- **串口直连方案已单独验证**：USB-TTL 1M 波特率（921600 对 CH340/ESP32 非整数分频会乱码），
  帧协议 `AA55 5AA5|len(2B)|JPEG|crc16`，640×480 约 2fps、连续 500 帧完整无错——
  说明**摄像头硬件本身无问题**，问题在软件链路与网络环境。
- **WiFi 掉线**：已关闭 modem sleep（`WiFi.setSleep(false)`），企业 AP 对省电设备有踢除策略。

## 五、请重点审查的问题

1. **画面模糊/灰阶点阵（最想解决）**
   - `scale_1bit()` 最近邻缩放：320×240 → 200×150 后直接 1-bit 阈值化，是否存在信息损失过大？
   - 1-bit 渲染是否有抖动/阈值处理？无背光反射屏上，是否应该用 **Bayer/Floyd-Steinberg 抖动**（dithering）
     或自适应阈值来保留画面结构？
   - JPEG 解码质量（`jpg_decode`）是否用了低质量配置？
   - 是否值得在 200×150 时用 2-bit/4-bit 灰度 + 屏上抖动？硬件只支持 1-bit，怎么权衡？
2. **卡顿/延迟**
   - 500ms 轮询间隔 + 双缓冲 + LVGL 刷新节奏是否合理？帧率瓶颈在哪？
   - `cam_client` 的 JPEG 解码是否阻塞了 LVGL 任务？
3. **黑屏/断流**
   - `/api/camframe` 的 keep-alive 与超时处理（8s）是否会导致连接泄漏或静默失效？
   - 后端抓帧线程在摄像头离线时的退避/恢复逻辑是否完善？
   - RLCD 侧 mDNS 缓存 5 分钟：Mac IP 变化后是否长时间连不上？
4. **摄像头固件**（`esp32-cam-fw/src/main.cpp`）
   - `cam_grab_task` 帧队列设计、JPEG 缓冲 96KB 是否够、WiFi 断线重连策略。

## 六、我们尝试过的方向（未完成/放弃）

- RLCD→摄像头**直连**：受 BTWIFI6 RST 影响，不可靠（保留 `CAM_FALLBACK_IP` 相关代码痕迹已移除）。
- 串口 1M 直连传图：硬件验证通过，但布线/常驻不如 WiFi 方便，当前主通道仍是 WiFi 代理。
- 摄像头免按键自举（'D' 字符 GPIO0 拉低 + `gpio_hold_en` + `esp_restart`）实测失败：
  SW_RESET 会清除 hold，需 deep sleep 方案（未验证）。

## 七、环境信息

- 办公室 AP：BTWIFI6 系列（对 ESP32 出站 TCP 有 RST）。
- Mac 后端 launchd 常驻（`cn.qwenwork.schedule-api`），Python + uvicorn :8100。
- RLCD 交易时段每 10 分钟拉 `/api/stocks`；摄像头帧 ~500ms 拉 `/api/camframe`。
