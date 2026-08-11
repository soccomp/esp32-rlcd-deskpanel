# RLCD-004.1 执行报告：Gesture Trigger Stability Improvement

- **Task ID:** RLCD-004.1
- **Status:** 完成（待审核）
- **Branch:** `workbuddy-development`
- **日期：** 2026-08-11
- **前序任务：** RLCD-004 COMPLETE（手指计数→页面切换原型，commit `ea70a56`）

---

## 1. 修改总结

RLCD-004 初步人工验收发现：实际体验差——两三分钟只成功切换 3~4 次，大部分手势没触发，摄像头约 1~2fps。本任务**不改架构、不碰画质、不做复杂手势**，只解决"触发成功率低"。

**根因**：原 M1 触发要求"连续 5 帧严格一致"才发命令。在 1~2fps 下 5 帧 ≈ 2.5~5s；MediaPipe 在弱光/低分辨率/手晃动时**偶发漏检（NO hand）或误数**，任意一帧不一致即把连击清零，导致几乎永远凑不满 5 帧。

**修复**：
- M1 侧把"严格连续 N 帧"改为「滚动时间窗口 + 多数表决 + 命令冷却」：在最近 `win_sec`（默认 3s）窗口内，只要 ≥`min_agree`（默认 3）帧认同同一手指数即触发，**天然容忍窗口内的个别漏检/误数帧**；触发后清空窗口并进入 `cooldown`（默认 1.5s）冷却，避免快速跳页、避免同页刷屏。
- M1 侧日志大幅增强：每帧输出 `HAND / FINGER / WIN / MAJ` 及触发决策（`TRIGGER / SKIP(already-there) / COOLDOWN / no-trigger`）。
- ESP32 侧补一条 no-op 日志：收到命令但已在目标页时打印 `[cmd] page N already current, skip (gesture)`，便于排查"为何没切"。
- 触发逻辑抽成可单测的 `FingerTrigger` 类（不改变行为，仅为可测）。

---

## 2. 文件列表

| 文件 | 仓库 | 改动 |
|------|------|------|
| `esp32-cam-fw/finger_page_control.py` | 修改 | 触发策略改为滚动窗口多数表决 + 冷却；新增 `FingerTrigger` 类；增强逐帧日志；新增 `--min-agree/--win-sec/--cooldown` 参数（`--consec` 兼容旧用法） |
| `esp32-cam-fw/tests/test_trigger_window.py` | 新增 | 触发策略单测（12 项，含新旧策略对比、冷却、同页不重发、0/4 指不触发、切页顺序） |
| `rlcd-lvgl/src/main.cpp` | 修改 | 收到命令但已在该页时补 no-op 日志 |
| `docs/execution_reports/RLCD-004.1.md` | 新增 | 本报告 |
| `WORKBUDDY_TASK.md` | 修改 | 追加 RLCD-004.1 任务单（Previous = RLCD-004 COMPLETE） |

> `camusb_bridge.py`、`cam_client.cpp/.h`、USB/帧协议**均未改动**（符合"不改架构"约束）。无敏感文件。

---

## 3. 根因分析

| 现象 | 旧逻辑（RLCD-004） | 在低帧率下的后果 |
|------|--------------------|------------------|
| MediaPipe 偶发漏检（NO hand） | 连击清零 | 永远凑不满 5 帧 |
| MediaPipe 偶发误数（如 2 读成 3） | 连击清零 | 同上 |
| 1~2fps → 5 帧需 2.5~5s | 须这 2.5~5s 内**每一帧**都完美 | 实际做不到 → 两三分钟仅成功 3~4 次 |

核心矛盾：**"严格连续一致"假设了稳定 5fps+ 的检测质量**，而本机 1~2fps + 偶发抖动根本满足不了。

---

## 4. 技术方案

### 4.1 新触发策略（`FingerTrigger`）
- 维护一个滚动时间窗口 `window`（仅收 1/2/3 的 `(finger, t)`），按时间裁剪（> `win_sec` 丢弃）。
- 每帧多数表决：`maj_v` = 窗口内出现最多的手指数，`maj_n` = 其出现次数。
- 触发条件同时满足：① `len(window) >= min_agree` 且 `maj_n >= min_agree`（窗口样本够 + 多数派够）；② 多数派对应页面 ≠ 当前页（`last_sent`）；③ 距上次发命令已过 `cooldown`。
- 触发后：**清空窗口 + 记录 `last_sent`/`last_sent_t`** → 必须重新摆出手势才再触发（防快速跳页），冷却期内即便多数一致也不发（进一步防刷屏）。

### 4.2 默认参数（已适配 1~2fps）
`min_agree=3, win_sec=3.0s, cooldown=1.5s`。
- 在 1.5fps 下，3s 窗口约 4~5 帧，需其中 ≥3 帧认同同一数 → 容忍 1~2 帧噪点；
- 在 1fps 下，3s 窗口约 3 帧全认同即触发（约举 3s）；
- `cooldown=1.5s` + 触发即清窗 → 两次命令最小间隔 ≈ 重新累计 3 帧(~2s) 与冷却取大，杜绝狂跳页。

### 4.3 日志
M1 每帧（示例）：
```
HAND:NO FINGER:- WIN:[] MAJ:None(0/0) -> no trigger
HAND:YES FINGER:2 WIN:[2,2,1,2] MAJ:2(3/4) -> no trigger
TRIGGER: PAGE:MEETING | HAND:YES FINGER:2 WIN:[2,2,1,2] MAJ:2(3/4)
SKIP: PAGE:HOME already current | HAND:YES FINGER:1 WIN:[1,1,1] MAJ:1(3/3)
COOLDOWN: hold PAGE:GUITAR | HAND:YES FINGER:3 WIN:[3,3,2] MAJ:3(2/3) wait 1.2s
```
ESP32：`[cmd] PAGE:X -> page N`（收到并解析）、`[cmd] page -> N (gesture)`（执行切换）、`[cmd] page N already current, skip (gesture)`（已在该页 no-op）。

---

## 5. 编译结果

- `rlcd-lvgl` 用 PlatformIO 编译（`-DCAM_USB_INPUT` 已含）：**BUILD SUCCESS**，RAM 42.7% (139772/327680 B) / Flash 64.1% (2142005/3342336 B)。
- `finger_page_control.py` 语法检查 `py_compile` 通过。

---

## 6. 烧录结果

- 固件已烧录至 RLCD（`/dev/cu.usbmodem11301`）：`kill -STOP` 冻结常驻桥接（launchd 不重启）→ pio 显式 `--upload-port` 烧录成功（`Hash verified`, `Hard resetting via RTS pin`）→ `kill -CONT` 恢复桥接。
- 恢复后桥接重连摄像头/RLCD，hub `127.0.0.1:8770` 重新上线（日志 `ok=8 bad=0 ~1.6fps`）。

---

## 7. 实机测试结果

### 7.1 单元测试（触发策略，12/12 PASS）
用真实痛点场景对比新旧策略（`tests/test_trigger_window.py`）：
| 用例 | 新策略 | 旧策略(连续5) |
|------|--------|---------------|
| 稳定 1 指 @1fps ×5 帧 | ✅ 触发 HOME | ✅ 触发 |
| 稳定 1 指 @1fps ×4 帧（<5） | ✅ 触发 | ❌ 不触发（差距点） |
| 1.5fps + 漏检/误数流 | ✅ 触发 MEETING | ❌ 0 触发（根因复现） |
| 同手势 8s 不超 2 次（实际 1 次） | ✅ 冷却生效 | — |
| 已在 HOME 再举 1 指 | ✅ 无新命令 | — |
| 0/4 指 / 无手 | ✅ 不触发 | — |
| 1指→2指 切页顺序 | ✅ [HOME, MEETING] | — |

### 7.2 M1 实帧 dry-run
`finger_page_control.py --dry-run --max-frames 20`：连 hub 成功、MediaPipe(tasks) 运行无 traceback、新日志行（`HAND/FINGER/WIN/MAJ/-> no trigger`）全部出现。（本次无人手入镜，故无 positive 计数，符合预期。）

### 7.3 命令下行（桥接）
hub 测试客户端下发 `PAGE:HOME/MEETING/GUITAR/BOGUS:XYZ`：桥接转发前三条（`cmd -> RLCD: PAGE:...`），拒绝 `BOGUS:XYZ`（`hub rejected command`）。命令链路与白名单不受影响。

### 7.4 人工手势验收（待用户在摄像头前执行）
自动化环境无人手，下列步骤交用户复验切换成功率提升：
```
# 终端：运行识别（去掉 --dry-run 才真发命令）
cd esp32-cam-fw
/Users/m1work/.workbuddy/binaries/python/envs/default/bin/python finger_page_control.py --show
```
验收判据（相较 RLCD-004 应明显提升）：
1. 举 1/2/3 指保持约 2s → 分别切到首页/会议页/吉他页；
2. 大部分手势应能触发（不再两三分钟仅 3~4 次）；
3. 保持不动不重复刷屏（`already-there`）；切换页面不狂跳（`cooldown`）；
4. 想调灵敏度：降低 `--min-agree`（如 2）更易触发，提高 `--cooldown`（如 2.5）更防跳页。

---

## 8. 已知问题 / 限制

1. **最终手势成功率仍需人工复验**：自动化环境无人手，本报告的机器验证覆盖触发逻辑/编译/烧录/命令链路；举指切页的成功率提升由用户在摄像头前确认（§7.4）。
2. **未改架构/画质**：1~2fps 的底层帧率与识别质量未动；新策略只是让"漏检/误数"不再清零连击，从而把已有检测能力充分利用起来。若仍嫌不够，后续可降 MediaPipe 检测置信度阈值或提摄像头帧率（超出本任务范围）。
3. **ESP32 命令寄存器仍为单槽 last-wins**（继承自 RLCD-004），配合主循环高频轮询与"触发即清窗"，每个手势命令都会被捕获，不会快速跳页。

---

## 9. Commit

- commit message：`chore: issue RLCD-004.1 gesture trigger stability improvement`
- commit hash：`<待 push 后填入>`

---

## 10. 结论

RLCD-004.1 在不改架构/画质/复杂手势的前提下，将 M1 触发从"严格连续 5 帧"改为「滚动窗口多数表决 + 冷却」，**直接消解了低帧率下偶发漏检/误数导致连击清零的根因**。单元测试证明新策略在真实痛点场景下能触发而旧策略几乎不触发，且冷却、同页不重发、0/4 指不触发、切页顺序均正确；固件已编译并烧录，M1 新日志与命令下行链路均验证通过。最终举指切页的成功率提升由人工手势验收确认。
