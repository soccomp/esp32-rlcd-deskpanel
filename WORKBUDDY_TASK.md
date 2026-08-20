# WORKBUDDY TASK

> This file is the single authoritative current task for WorkBuddy on the M1 development machine.
> ChatGPT reviews GitHub and writes the current task here. WorkBuddy reads it, executes locally, validates on real hardware when required, then commits and pushes the result to `workbuddy-development`. After push, stop and wait for the next ChatGPT review.

# RLCD-004 Finger Count Page Control Prototype

## Task

- **Task ID:** RLCD-004
- **Status:** OPEN
- **Branch:** `workbuddy-development`
- **Priority:** P1
- **Previous task:** RLCD-003 COMPLETE — reviewed and accepted. Its full execution report and the
  post-RLCD-003 operational addendum (camusb-bridge resident service) are preserved in git history
  at `ca360d2` / `44bccb0`.

---

## Goal

实现第一个视觉交互闭环：

摄像头输入 → M1 视觉识别 → 手指数量判断 → USB 命令 → RLCD 页面切换。

本任务不追求摄像头视频质量优化，而是验证设备具备"看见并响应"的能力。

---

## Functional Requirement

实现：

1 根手指：
`PAGE_HOME`
→ 首页

2 根手指：
`PAGE_MEETING`
→ 会议页

3 根手指：
`PAGE_GUITAR`
→ 吉他页

---

## Recommended Architecture

保持职责分离：

**ESP32-CAM:**
- 提供摄像头图像

**M1:**
- OpenCV
- MediaPipe Hands
- 手部关键点识别
- 手指数量判断

**ESP32-S3 RLCD:**
- 接收简单命令
- 执行页面切换

不要在 ESP32 上运行视觉模型。

---

## Implementation Requirements

### M1 side

实现一个最小手势识别程序：

输入：
当前摄像头帧

输出：

```
PAGE_HOME
PAGE_MEETING
PAGE_GUITAR
```

识别逻辑：

- 1 finger
- 2 fingers
- 3 fingers

要求：

连续多个检测结果一致后才发送命令。

必须有防抖：

例如：
连续 5 帧相同结果才触发。

同一个页面命令不要重复发送。

---

### USB Command Protocol

优先复用已有 USB-CDC 通道。

增加简单命令，例如：

```
PAGE:HOME
PAGE:MEETING
PAGE:GUITAR
```

协议保持简单。

---

### RLCD side

增加命令处理：

收到：

`PAGE:HOME`
→ 切换首页

`PAGE:MEETING`
→ 切换会议页

`PAGE:GUITAR`
→ 切换吉他页

不要重构整个 UI。

如果目标页面暂不存在：

允许先建立 placeholder 页面。

---

## Debug Requirement

保留摄像头调试能力。

建议增加调试显示：

```
HAND:   YES/NO
FINGER: 1/2/3
CMD:    PAGE_HOME
```

方便后续调试。

---

## Validation

必须真实硬件验证。

验收：

1 根手指保持 2 秒：
RLCD 进入首页

2 根手指保持 2 秒：
RLCD 进入会议页

3 根手指保持 2 秒：
RLCD 进入吉他页

要求：

- 不误触
- 不快速跳页
- 命令链路稳定

---

## Restrictions

本任务不做：

- 摄像头画质优化
- Floyd-Steinberg
- 人脸识别
- 物体识别
- 拨弦识别
- 大模型接入

这些作为后续任务。

---

## Execution Report Requirement

从本任务开始，完成后必须新增：

`docs/execution_reports/RLCD-004.md`

报告至少包含：

- 修改总结
- 文件列表
- 技术方案
- 编译结果
- 烧录结果
- 实机测试结果
- 已知问题
- commit hash

---

## Git checkpoint

完成任务单修改后：

1. `git diff` 检查
2. commit:
   - `chore: issue RLCD-004 finger count page control prototype`
3. push 到 `workbuddy-development`

完成后停止，不执行开发。
等待下一轮审核。

---

# RLCD-004.1 Gesture Trigger Stability Improvement

## Task

- **Task ID:** RLCD-004.1
- **Status:** OPEN（WorkBuddy 执行完成，待审核）
- **Branch:** `workbuddy-development`
- **Priority:** P1
- **Previous task:** RLCD-004 COMPLETE — 手指计数→页面切换原型已交付并初步人工验收（commit `ea70a56`）。

---

## Background（为何要做）

RLCD-004 初步人工验收结论：MediaPipe 能识别手、finger count 能变化、调试窗口 HAND/FINGER/CMD 正常、RLCD 偶尔能按手指数切页；但**实际体验差**——两三分钟只成功切换 3~4 次，大部分手势没触发，摄像头帧率约 1~2fps。

**根因（已分析，未改架构）**：原触发要求"连续 5 帧严格一致"才发命令。在 1~2fps 下，5 帧 ≈ 2.5~5s；而 MediaPipe 在弱光/低分辨率/手晃动时**偶发漏检（NO hand）或误数**，任意一帧不一致即把连击清零，导致几乎永远凑不满 5 帧。

---

## Goal

在不重新设计架构、不改摄像头画质、不做复杂手势的前提下，**提高真实使用时的手势触发成功率**。

---

## Requirement

### 1. M1 侧触发策略优化
- 用「滚动时间窗口 + 多数表决 + 命令冷却」替代"严格连续 N 帧"。
- 在 1~2fps 下，窗口内多数帧认同同一手指数即触发，天然容忍个别漏检/误数帧。
- 触发后清空窗口 + 进入冷却期，避免快速跳页、避免同页重复刷屏。
- 参数可调（默认应已适配 1~2fps）：`--min-agree`（默认 3）、`--win-sec`（默认 3.0s）、`--cooldown`（默认 1.5s）。
- 不降低稳定性；不快速跳页；不影响视频桥接。

### 2. 增加必要日志
M1 侧每帧输出：是否检测到手、当前 finger count、是否满足触发条件（及不满足原因）、实际发送的 PAGE 命令。
ESP32 侧确认：是否收到 PAGE 命令、是否执行页面切换（含"已在该页→不切"的 no-op 记录）。

### 3. 约束（与本任务范围）
- 不重新设计架构；不处理摄像头画质优化；不做复杂手势；不改整体架构。
- 职责分离不变：M1 跑视觉，RLCD 只收 `PAGE:xxx` 简单命令。
- USB 命令协议、帧协议、hub 转发机制保持不变。

---

## Validation

- 单元/逻辑测试：用真实痛点场景（1~2fps + 漏检/误数）对比新旧触发策略，证明新策略能触发而旧策略几乎不触发；并验证冷却、同页不重发、0/4 指不触发、切页顺序正确。
- 编译验证：固件 `pio run` 通过。
- 实机测试：
  - 自动可验：M1 侧新日志正常、MediaPipe 实帧无崩溃；桥接下发 PAGE 命令被正确转发、非法命令被拒。
  - 人工手势验收（自动化环境无人手，需用户在摄像头前执行）：举 1/2/3 指保持约 2s，观察切换成功率较 RLCD-004 明显提升、无快速跳页。runbook 见执行报告。

---

## Execution Report Requirement

完成后新增：`docs/execution_reports/RLCD-004.1.md`
至少包含：修改总结、文件列表、根因分析、技术方案（触发策略/日志）、编译结果、烧录结果、实机测试结果、已知问题、commit hash。

---

## Git checkpoint

1. `git diff` 检查
2. commit：`chore: issue RLCD-004.1 gesture trigger stability improvement`
3. push 到 `workbuddy-development`

完成后停止，等待审核。

---

# RLCD-004.2 Page Command Delivery Reliability

## Task

- **Task ID:** RLCD-004.2
- **Status:** OPEN
- **Branch:** `workbuddy-development`
- **Priority:** P1
- **Previous task:** RLCD-004.1 COMPLETE（真人验收已确认 M1 能识别手指数并产生 `TRIGGER: PAGE:X`，但 RLCD 经常不切页）

## Problem（真人验收定位）

M1 能输出 `TRIGGER: PAGE:MEETING` / `TRIGGER: PAGE:GUITAR`，但 RLCD 页面经常没变、仍停在首页；随后 M1 输出 `SKIP: PAGE:MEETING already current`。

根因：M1 用 `last_sent`（命令"已发送"）乐观地当作"RLCD 已切页"。当首条命令在链路某层丢失、RLCD 未切页，M1 仍把 `last_sent` 置为目标页，下一相同手势被判 `already current` 而**永不再发**，sender 状态与真实设备状态漂移。这是 ACK-less 投递的必然结果。

## Goal

让 M1 只有在 RLCD 确认页面真正切换后，才认为当前页面已改变——实现「命令投递可靠性」。

## Functional Requirement

1. **RLCD 侧 ACK 回执**：收到 `PAGE:HOME/MEETING/GUITAR` 并经 USB-CDC 返回 `ACK:PAGE:HOME` / `ACK:PAGE:MEETING` / `ACK:PAGE:GUITAR`；**即使本来就在目标页，也返回对应 ACK**。
2. **桥接转发**：`camusb_bridge` 读取 RLCD USB-CDC TX 中的 `ACK:PAGE:X`，转发给本地 hub 客户端。
3. **M1 双状态**：`finger_page_control.py` 区分 `desired_page` 与 `confirmed_page`：
   - 发送 `PAGE:X` 后**不允许**直接把 confirmed 改成 X；
   - 只有收到 `ACK:PAGE:X` 后才确认该页真正生效；
   - "Already current" 判断只基于 `confirmed_page`，不基于"曾经发过命令"。
4. **超时重发**：`PAGE` 发出后 `ack_timeout`（默认 0.9s）内无 ACK → 自动重发，最多 `max_attempts`（默认 3）次；超时后明确日志 `PAGE ACK TIMEOUT`。不每帧重复刷命令。
5. **不破坏**现有 JPEG/video bridge 帧协议；不优化摄像头画质；不扩展复杂手势；不重构 UI。

## Validation

1. 不依赖手势注入 `HOME → MEETING → GUITAR → HOME` 连续 ≥30 次，每次必须收到对应 ACK，不允许 sender 与 RLCD 状态漂移。
2. 真人手势 1 指→HOME、2 指→MEETING、3 指→GUITAR，连续完成至少 1→2→3→1 三轮。
3. 特别验证命令第一次丢失时能自动重发，而不是错误进入 "already current"。
4. 端到端日志需完整可见：`TRIGGER` / `SEND` / `ESP32 RX` / `UI SWITCH` / `ACK` / `CONFIRMED`。

## Execution Report Requirement

完成后新增：`docs/execution_reports/RLCD-004.2.md`
至少包含：诊断结论（命令在哪层丢失）、修改总结、文件列表、ACK 协议、重发策略、编译结果、烧录结果、实机测试结果、已知问题、commit hash。

## Git checkpoint

1. `git diff` 检查
2. commit：`chore: issue RLCD-004.2 page command delivery reliability`
3. push 到 `workbuddy-development`

完成后停止，等待审核。

---

## Addendum (2026-08-20, WorkBuddy) — sync current running state to GitHub

Out-of-band sync of the latest on-hardware state from the local dev repo `rlcd-lvgl`
(feat/dashboard-home-market) into this staging repo, so that GitHub reflects the code
currently running on the device. Changes below mirror the local commit
`b9f1339` ("feat: home page french dialogues (200 pairs), lunar calendar, stocks client, camera status"):

- **French learning card**: replaced the old 100-line SD-loaded quote library with **200 built-in
  bilingual dialogues** — `src/french_dialogues.h` (data, generated by `scripts/gen_french_dialogues.py`,
  data in `scripts/french_dialogues.py`); `src/french_lib.cpp/h` rewritten (no SD dependency); old
  `src/french_quotes.h` removed.
- **Lunar calendar** in status bar — `src/lunar.cpp/h` + `src/lunar_table.h` (table 1900-2099,
  generated by `scripts/gen_lunar_table.py`).
- **Stocks**: `src/stocks_client.cpp/h` now direct HTTPS (Tencent qt.gtimg.cn + Eastmoney
  push2.eastmoney.com); Mac `/api/stocks` no longer used.
- **Weather**: primary path direct QWeather HTTPS; Mac `/api/weather_qh` demoted to soft fallback.
- **Camera**: new CAM status indicator (●/○ inverted) in status bar on all pages, driven by the 1s
  clock timer.
- **Fonts**: regenerated `src/lv_font_chinese_14.c` (FULLWIDTH_PUNCT fallback added).
- `platformio.ini` / `gen_chinese_font.py` updated accordingly.
- Real local credentials (`src/local_config.h`, `src/wifi_config.h`) remain gitignored; public
  placeholders `src/local_config.example.h` / `src/wifi_config.example.h` are tracked.

Note: this is a housekeeping sync (user explicitly requested GitHub mirror the running state so
external AI review reflects current code). No new task is opened; RLCD-004.2 remains the active task.

---

## Addendum 2 (2026-08-20, WorkBuddy) — reproducibility + docs alignment (housekeeping)

User requested a full "GitHub as source of truth" alignment pass. Three commits on top of the
previous sync (cabada1):

1. `777e863` **sync: align CAM firmware with local runtime (USB full-link video)**
   - `esp32-cam-fw/src/main.cpp`: enable `uart_frame_task` + `uart_cmd_task` (USB full-link),
     disable `wifi_reassoc_task` (WiFi kept only for `/status` diag).
   - `esp32-cam-fw/platformio.ini`: drop `-DCAM_WIFI_ONLY` (serial frame path 1M).
   - `esp32-cam-fw/finger_page_control.py`: gesture → page mapping sync.

2. `ffccc60` **build: make repository reproducible from a clean checkout**
   - Removed `rlcd-lvgl/lib/lvgl` symlink (pointed to local absolute path, unusable on clean checkout).
   - `platformio.ini`: switch LVGL to registry `lvgl/lvgl@8.4.0` + `extra_scripts`.
   - Added `scripts/patch_lvgl.py`: re-applies the only local LVGL customization (`lv_refr.c` even
     flush chunk height for ST7305 rotated refresh) after libdeps download. Verified clean-build:
     `rm -rf .pio/libdeps && pio run` → auto-patch + SUCCESS (Flash 2162097B).

3. `docs` **align architecture documentation with current implementation**
   - `README.md` / `docs/ARCHITECTURE.md`: 3-page UI (HOME/GUITAR/CAMERA, meeting page removed),
     200-dialogue French card, USB full-link camera, stocks firmware-direct HTTPS, weather QWeather
     direct primary, CAM status indicator, gesture page control, 12s wifi_hard_restart.
   - `PROBLEM.md`: rewritten for the USB full-link camera chain (was WiFi proxy).

**Recorded but NOT fixed in this pass (per user's explicit scope)**: `parse_schedule.py` is absent
from the repo (referenced by docs/launchd but file deleted from disk; the running launchd process
holds it in memory — will fail to restart). Mitigation deferred to a future task.

Backup branch `backup/pre-github-sync-20260820` created at `cabada1` before this pass. Pushed to
`workbuddy-development` without force.
