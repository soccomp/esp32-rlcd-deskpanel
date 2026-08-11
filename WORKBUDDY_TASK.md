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
