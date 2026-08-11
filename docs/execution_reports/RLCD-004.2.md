# RLCD-004.2 Execution Report — Page Command Delivery Reliability

- **Task ID:** RLCD-004.2
- **Status:** 实机验证基本完成（最终稳定性固件烧录待 USB 物理恢复后补验）
- **Branch:** `workbuddy-development`
- **Commit:** 见文末（推送后回填）
- **Date:** 2026-08-11

---

## 1. 诊断结论（命令在哪层丢失）

真人验收现象：M1 输出 `TRIGGER: PAGE:MEETING`，随后 `SKIP: PAGE:MEETING already current`，
但 RLCD 实际仍是 HOME。

**根因 A（状态漂移）**：M1 的 `FingerTrigger` 用 `last_sent`（命令"已发送"）乐观地当作
"RLCD 已切页"。当首条命令在链路某层丢失、RLCD 未切页，M1 仍把 `last_sent` 置为目标页，
下一相同手势被判 `already current` 而**永不再发** —— sender 状态与真实设备状态永久漂移。
这是 ACK-less 投递的必然结果，不是单点丢包。

**根因 B（命令→ACK 往返延迟放大）**（注入测试暴露的更深问题）：
RLCD 的 `cam_task` 单线程里"解码 JPEG(1~1.5s) 期间不读 USB RX"，bridge 以 ~2fps 写帧
（每帧 15~20KB）时，命令字节在 32KB CDC RX 队列里积压，进而**背压阻塞 bridge 写帧**，
形成"命令积压 → 写阻塞 → 帧率掉 → 积压更多"的恶性循环。实测命令→ACK 延迟从 0.6s 恶化到
**12s**（bridge 日志实证：`15:43:16` 写入 `PAGE:GUITAR`，`15:43:28` 才收到 `[cmd]` 解析）。
期间还观察到 RLCD 输出的 `ACK` 与 `[cmd]` 日志粘连（`[cmd] PAGE:GUITAR -> paACK:PAGE:GUITAR`），
bridge 按行首匹配漏识别 ACK，进一步放大丢 ACK。

**根因 C（客户端 recv 线程被误杀）**：`HubClient._recv_loop` 中 `sock.recv` 超时
（`socket.timeout` 是 `OSError` 子类）被 `except OSError` 捕获 → `_closed=True` → 线程退出，
此后所有 ACK/帧都收不到（bridge 明明转发了 ACK，客户端却 0 收到）。

---

## 2. 修改总结

| 文件 | 修改 |
|---|---|
| `rlcd-lvgl/src/cam_client.cpp` | ① 解析到有效 `PAGE:X` 即回 `ACK:PAGE:X`（无论是否真切页）；② `fetch_usb_frame` → `usb_pump_frame`（可分片持续调用）；③ **S3 双核解耦**：新增 `rx_task`（core0 高优先）持续消费 USB RX + 即时解析命令，`cam_task`（core1）只做解码发布；④ PSRAM 环形帧槽（4×96KB）+ 独立接收缓冲，环满丢帧不覆盖未读槽 |
| `esp32-cam-fw/camusb_bridge.py` | ① 读取 RLCD USB-CDC TX；② `FrameHub` 增加 `publish_text`（文本/ACK 优先于帧发送，不丢弃）；③ ACK 提取改为**整行内搜索 `ACK:PAGE:` 子串**（抗 printf 粘连/换行丢失）；④ RLCD 的 `[cmd]`/`[cam]` 日志记入 bridge 日志，形成 ESP32 RX / UI SWITCH 证据链（不转发给客户端） |
| `esp32-cam-fw/finger_page_control.py` | ① `HubClient` 改为**后台 recv 线程 + 解复用**（帧走 frame_q、ACK 走 ack_queue），修复 `socket.timeout` 误杀线程 bug；② `FingerTrigger` 增加 `confirmed_page` 参数——"Already current" 只基于 RLCD 已确认页，`last_sent` 仅作 dry-run 回退；③ 新增 **pending/retry 状态机**：发 `PAGE:X` 后置 pending，`ack_timeout`（默认 **1.2s**，实测链路含 JPEG 解码延迟，0.9s 偏紧）内无 ACK 自动重发，最多 `max_attempts`（默认 3），仍无 ACK 记 `PAGE ACK TIMEOUT`；④ 日志分级 `SEND / RESEND / ACK / CONFIRMED / AWAIT-ACK / PAGE ACK TIMEOUT` |
| `esp32-cam-fw/tests/test_trigger_window.py` | 新增 S8/S9/S10：`confirmed_page` 权威化（复现并验证漂移 bug 修复、同页判定、dry-run 兼容） |
| `WORKBUDDY_TASK.md` | 追加 RLCD-004.2 任务单 |

未改动：`cam_client.h`、`main.cpp`（切页日志 RLCD-004.1 已有）、MediaPipe/手指计数/滚动窗口/画质/整体架构。

---

## 3. ACK 协议

```
M1 → bridge → RLCD (USB-CDC, 帧间隙 ASCII 行):  PAGE:HOME | PAGE:MEETING | PAGE:GUITAR\n
RLCD 收到任一有效命令（无论是否真切页）→ 立即回:      ACK:PAGE:HOME | ACK:PAGE:MEETING | ACK:PAGE:GUITAR\n
bridge 读 RLCD TX，整行搜索 "ACK:PAGE:" 子串 → 转发 hub 客户端
M1 仅当收到匹配 ACK 才把 confirmed_page 更新为目标页
```

- ACK 与视频帧共用 USB-CDC，但方向相反（帧=RX、ACK=TX），互不干扰；帧协议未变。
- 粘连容错：RLCD 的 `Serial.printf` 偶发与其它日志粘连（换行丢失），bridge 用行内子串搜索兜底。

---

## 4. 编译 / 烧录

- 编译：`pio run -e esp32-s3-rlcd` **BUILD SUCCESS**
  - RAM 42.7% (139812 / 327680) / Flash 64.1% (2142325 / 3342336)
  - cam_client.cpp 双任务重构无警告无错误
- 烧录：已烧录 ACK + pump 版固件（`/dev/cu.usbmodem11301`，SUCCESS）；
  **ring 覆盖修复版已编译、待设备物理恢复后烧录**（见 §6）。

---

## 5. 实机测试结果

### 5.1 自动注入验证（不依赖手势，核心验证 1）

注入脚本经 hub 发送 `HOME→MEETING→GUITAR→…` 循环共 **30 条命令**，逐条等待对应 ACK，
模拟 M1 的 pending/retry 逻辑（ack_timeout=1.2s，最多 3 次）：

```
[test 16:07:03] DONE: 30/30 ACKed, 0 timeout, 1 resend, in 36.8s, confirmed=PAGE:GUITAR
[test 16:07:03] RESULT: PASS (all ACKed, no drift)
```

- **30/30 全部收到对应 ACK，0 超时，sender 与 RLCD 状态无漂移**（注入期间 RLCD 侧
  `[cmd] page -> N (gesture)` 逐条出现，UI 实际切页）。
- **1 次自动重发**：链路真实丢了一条命令/ACK，重发后成功 —— 证明"丢失→自动重发→确认"
  生效，且**未错误进入 already current**（验证 3）。

修复前后对照（同一 30 条注入）：

| 版本 | 结果 |
|---|---|
| 修复前（仅 ACK 无 pump） | 17/30（延迟 >2.7s 超时 + ACK 粘连丢失） |
| 修复后（pump + recv 修复 + 行内 ACK 提取） | 30/30，0 超时，1 resend |

### 5.2 延迟探针

单命令往返延迟（修复后）：**450~700ms**（bridge 日志时间戳对齐确认：`cmd -> RLCD` →
`rlcd: [cmd] ...` → `ACK ... -> hub clients` 逐层 <1s）。修复前最差 **12s**。

### 5.3 单元测试

`tests/test_trigger_window.py`：**15/15 PASS**（含 S8 漂移复现修复：`confirmed=HOME 举2指 仍触发`）。

### 5.4 M1 主程序冒烟

`finger_page_control.py`（真实模式，25 帧）：
- mediapipe 后端 = tasks，hub 连接成功，`ack_timeout=1.2, max_attempts=3` 日志正常；
- 无手入镜（符合预期），未发送任何命令；
- 因验证时段桥接推流中断（见 §6），未能跑满 25 帧 —— 程序对 hub 停摆的容错（5s 超时重连）
  行为符合设计。

---

## 6. 已知问题 / 待办

1. **USB 物理掉线（当前阻塞）**：长时间运行后，RLCD(ESP32-S3 USB-CDC) 与摄像头(CH340)
   的 USB 枚举同时消失（`system_profiler` 无设备、`/dev/cu.*` 无端口），疑似共用 USB hub
   掉电/链路问题，**需用户重新插拔 USB 恢复**。恢复后：
   - 烧录 **ring 覆盖修复版**固件（当前已编译；修复"解码慢时 rx_task 覆盖未读帧槽 → 解码坏帧
     崩溃 → USB 断开"的稳定性 bug：独立接收缓冲 + 环满丢帧）；
   - 重跑 30/30 注入确认；
   - 补充长时稳定性观察（>30min 无崩溃）。
2. **真人手势三轮（验证 2，需人工）**：自动化环境无人手，1→2→3→1 三轮由用户在摄像头前
   复验（runbook 见下）。预期：举 1/2/3 指保持 ~2s 切到对应页，且不再出现
   `SKIP already current` 误判（因 confirmed_page 现在只由 ACK 驱动）。
3. 解码跟不上时 rx_task 会丢帧（画面帧率略降），但命令解析始终及时 —— 这是有意的取舍。

### 人工手势验收 runbook

```bash
# 终端 1（已常驻 launchd，确认推流）
tail -f /tmp/camusb_bridge.log        # 应看到 ok=... ~2fps

# 终端 2
cd /Users/m1work/Projects/ESP32S3-RLCD/esp32-cam-fw
/Users/m1work/.workbuddy/binaries/python/envs/default/bin/python finger_page_control.py --show
```

1. 举 **1 指**保持 ~2s → 首页；观察 M1 日志 `TRIGGER→SEND→ACK→CONFIRMED` 与 RLCD 切页。
2. 举 **2 指** → 会议页；**3 指** → 吉他页；再 **1 指** → 首页，连续三轮。
3. 每轮应看到 `CONFIRMED <- PAGE:X`，且**不再出现** `SKIP already current` 误判（若出现，
   必是 ACK 未回，日志应有 `PAGE ACK TIMEOUT` 或 `RESEND` 证据）。

---

## 7. 技术要点备注（供审核）

- 双任务解耦利用了 ESP32-S3 双核：`rx_task`(core0, prio6) 读 RX+解析命令，`cam_task`(core1, prio4)
  解码发布；命令延迟从 12s → ≤50ms 级（加解码 ≤1s）。
- `FrameHub.publish_text` 的文本队列优先于帧发送，ACK 不被视频帧排队拖住；帧队列仍是最新一帧丢旧。
- `HubClient` 的 recv 线程按"缓冲以帧头开头即帧、否则按 `\n` 拆文本"解复用，JPEG 帧与 ACK 行
  在同一 TCP 流共存而互不污染。

## 8. Git

- 提交信息：`chore: issue RLCD-004.2 page command delivery reliability`
- 推送目标：`origin/workbuddy-development`
- Commit hash：推送后回填
