# 运维说明（OPERATIONS）—— M1 侧常驻服务

> 本文档描述**开发机（Mac M1）侧**需要长期运行的进程，以及它们与固件行为的关系。
> 固件代码本身不在本文范围内，请见 `docs/ARCHITECTURE.md`。

## 1. 为什么需要看这份文档

RLCD 屏上的摄像头画面"卡在一帧不动"，有两种完全不同的成因：

| 现象 | 成因 | 归属 |
| --- | --- | --- |
| 画面定格，屏幕其他部分（时间/会议）仍正常刷新 | **M1 侧转发进程没在跑**，RLCD 收不到新帧，保持最后一帧 | 运维问题 |
| 画面定格且整屏无响应 | 固件卡死 / 看门狗 | 代码问题 |

第一种在 USB 传输模式下最常见，且极易被误判成固件 bug。
**排查第一步永远是确认 M1 侧桥接进程是否存活**，不要一上来改固件。

## 2. 常驻服务清单

### 2.1 `cn.qwenwork.schedule-api` —— 会议/天气/行情后端

- 脚本：`rlcd-lvgl/parse_schedule.py`
- 端口：`8100`
- 日志：`/tmp/sched_api.log`
- 作用：给 RLCD 提供会议、天气、股票数据；WiFi 模式下还通过 `/api/camframe` 代理摄像头帧。

### 2.2 `cn.qwenwork.camusb-bridge` —— USB 视频桥（本次新增）

- 脚本：`esp32-cam-fw/camusb_bridge.py`
- plist：`esp32-cam-fw/launchd/cn.qwenwork.camusb-bridge.plist`
- 日志：`/tmp/camusb_bridge.log`
- 作用：USB 传输模式下，把 ESP32-CAM 串口帧转发给 RLCD 的 USB-CDC。

```
ESP32-CAM (UART0, CH340, 1M) ──USB Hub──> M1: camusb_bridge.py ──> RLCD (USB-CDC 115200)
```

**没有这个进程，USB 模式下 RLCD 永远收不到新帧。**

## 3. camusb_bridge.py 的常驻改造

原脚本硬编码串口路径、无重连，不具备常驻条件。现已改为：

### 3.1 端口自动发现（按 USB VID:PID，不看端口号）

macOS 的串口设备名会随插拔变化（`/dev/cu.usbmodem101` → `11301`），硬编码必然失效。现按 VID:PID 识别：

| 设备 | VID:PID | glob 回退 |
| --- | --- | --- |
| ESP32-CAM（CH340 USB-TTL） | `1A86:7523` | `/dev/cu.usbserial*` |
| RLCD（ESP32-S3 原生 USB-CDC） | `303A:*` | `/dev/cu.usbmodem*` |

不带参数运行即全自动；仍可用 `camusb_bridge.py <cam口> <rlcd口>` 手动指定，或用 `auto` 占位。

### 3.2 断线重连

设备未插入、被拔出、串口 IO 异常时**不再退出**：关闭句柄 → 等待 3 秒 → 重新发现端口 → 重连。设备插回即自动恢复，无需人工干预。

> 说明：`/dev/tty.*` 打开时会阻塞等待 DCD 信号，脚本统一转用 `/dev/cu.*`（callout）。

## 4. 安装与管理

plist 已随仓库提供。安装到用户 LaunchAgents 并启动：

```bash
cp esp32-cam-fw/launchd/cn.qwenwork.camusb-bridge.plist ~/Library/LaunchAgents/
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/cn.qwenwork.camusb-bridge.plist
```

> **注意**：`launchctl bootstrap` 必须在**用户自己的图形会话终端**（Terminal.app 等）里执行。
> 从受限的自动化/沙箱上下文调用会报 `Bootstrap failed: 5: Input/output error`——
> 该上下文对用户 launchd domain 只有读权限，无法注册新服务。这与 plist 内容无关
> （已用最小 plist 验证同样失败）。
> 若不便手动执行：plist 放进 `~/Library/LaunchAgents/` 后，**下次登录会自动加载**（`RunAtLoad`）。

常用操作：

```bash
# 状态
launchctl print gui/$(id -u)/cn.qwenwork.camusb-bridge | grep -E "state|pid"
# 改完脚本重启
launchctl kickstart -k gui/$(id -u)/cn.qwenwork.camusb-bridge
# 停止并注销
launchctl bootout gui/$(id -u)/cn.qwenwork.camusb-bridge
# 看日志
tail -f /tmp/camusb_bridge.log
```

健康日志长这样（约 2fps 为当前链路正常水平）：

```
[bridge 10:24:46] /dev/cu.usbserial-1120@1000000 -> /dev/cu.usbmodem11301@115200
[bridge 10:24:56] ok=9 bad=0 ~1.8fps
```

- `ok` 持续增长 = 链路正常
- `waiting for devices` = 有设备没插上
- `link lost: ... retry in 3s` = 设备被拔出或异常，会自动重连

## 5. 未纳入本次改动的建议

RLCD 固件已有 `cam_client_is_fresh()` 新鲜度判断，但 UI 层尚未据此做兜底显示。
建议后续在预览区加"帧超时 → 显示 `CAM OFFLINE`"，让**链路中断**和**程序死机**在屏幕上一眼可分，
免去每次都要登录开发机查进程。此项属固件改动，留待后续任务单决定。

## 6. 环境备注

- Python 解释器：`/Users/m1work/.workbuddy/binaries/python/envs/default/bin/python`（含 pyserial 3.5）。系统 `python3` **没有** pyserial，直接跑会 ImportError。
- 路径按本机开发环境书写，换机需同步修改 plist 中的 `ProgramArguments` / `WorkingDirectory`。
