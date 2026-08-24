#!/usr/bin/env python3
"""RLCD-004.1 触发策略单测：验证「滚动窗口多数表决 + 冷却」相对旧「严格连续 5 帧」
在低帧率 + 漏检/误数下的优势，以及稳定性约束（不快速跳页、同页不重发、0/4 指不触发）。

运行（需含 cv2/numpy 的 venv）：
  /Users/m1work/.workbuddy/binaries/python/envs/default/bin/python tests/test_trigger_window.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from finger_page_control import FingerTrigger  # noqa: E402

PASS = 0
FAIL = 0


def check(name, cond):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"[PASS] {name}")
    else:
        FAIL += 1
        print(f"[FAIL] {name}")


def simulate(trig, stream, dt):
    """stream: list of (present, fingers)；dt: 帧间隔秒。返回触发命令序列。"""
    cmds = []
    t = 0.0
    for present, fingers in stream:
        t += dt
        decision, cmd, _ = trig.update(present, fingers, t)
        if decision == "trigger":
            cmds.append(cmd)
    return cmds


def old_strict_triggers(stream, consec=5):
    """旧策略参考实现：任意非(有手且 1/2/3)帧清零连击，连击==consec 才触发。"""
    streak_val, streak_n = -1, 0
    n = 0
    for present, fingers in stream:
        if present and fingers in (1, 2, 3):
            if fingers == streak_val:
                streak_n += 1
            else:
                streak_val, streak_n = fingers, 1
        else:
            streak_val, streak_n = -1, 0
        if streak_n == consec:
            n += 1
            streak_val, streak_n = -1, 0
    return n


# ============================================================
# 场景 1：稳定 1 指 @1fps 持续 5 帧 —— 新/旧都应触发
dt = 1.0
stable_1 = [(True, 1)] * 5
trig = FingerTrigger(min_agree=3, win_sec=3.0, cooldown=1.5)
check("S1 新策略 稳定1指@1fps 触发 PAGE:HOME",
      simulate(trig, stable_1, dt) == ["PAGE:HOME"])
check("S1 旧策略 稳定1指@1fps(连续5) 触发",
      old_strict_triggers(stable_1, 5) == 1)

# 场景 2：稳定 1 指但只有 4 帧 @1fps（<5）—— 新触发，旧不触发（真实差距）
stable_1_4 = [(True, 1)] * 4
trig = FingerTrigger(min_agree=3, win_sec=3.0, cooldown=1.5)
check("S2 新策略 4帧1指@1fps 触发", simulate(trig, stable_1_4, dt) == ["PAGE:HOME"])
check("S2 旧策略 4帧1指@1fps 不触发(需连续5)", old_strict_triggers(stable_1_4, 5) == 0)

# 场景 3：真实痛点 —— 1.5fps + 漏检(NO hand)/误数(成3)
dt = 1 / 1.5
noisy = [(True, 2), (True, 2), (False, 0), (True, 2), (True, 3),
         (True, 2), (True, 2), (False, 0), (True, 2)]
trig = FingerTrigger(min_agree=3, win_sec=3.0, cooldown=1.5)
new_noisy = simulate(trig, noisy, dt)
check("S3 新策略 漏检/误数流 仍能触发 PAGE:GUITAR", "PAGE:GUITAR" in new_noisy)
check("S3 旧策略 同样流 几乎不触发(0)", old_strict_triggers(noisy, 5) == 0)

# 场景 4：冷却 + 触发后清窗 —— 同手势不会在 8s 内疯狂重发
trig = FingerTrigger(min_agree=3, win_sec=3.0, cooldown=1.5)
cmds = simulate(trig, [(True, 1)] * 8, 1.0)
check("S4 同手势8s内不超2次(实际1次)", len(cmds) <= 2 and cmds[0] == "PAGE:HOME")

# 场景 5：同页不重发（已在首页，再举1指 -> 无新命令）
trig = FingerTrigger(min_agree=3, win_sec=3.0, cooldown=1.5)
trig.last_sent = "PAGE:HOME"
trig.last_sent_t = 100.0
check("S5 已在HOME再举1指 不产生新命令", simulate(trig, [(True, 1)] * 4, 1.0) == [])

# 场景 6：0/4 指、无手 永远不触发
trig = FingerTrigger(min_agree=3, win_sec=3.0, cooldown=1.5)
check("S6 4指不触发", simulate(trig, [(True, 4)] * 6, 1.0) == [])
check("S6 0指(握拳)不触发", simulate(trig, [(True, 0)] * 6, 1.0) == [])
check("S6 无手不触发", simulate(trig, [(False, 0)] * 6, 1.0) == [])

# 场景 7：切页需要新多数 + 冷却，不瞬间跳（1指->2指）
trig = FingerTrigger(min_agree=3, win_sec=3.0, cooldown=1.5)
stream = [(True, 1)] * 4 + [(True, 2)] * 5
check("S7 1指->2指 顺序正确 [HOME, GUITAR]",
      simulate(trig, stream, 1.0) == ["PAGE:HOME", "PAGE:GUITAR"])

# ============================================================
# RLCD-004.2：confirmed_page 权威化（修复"命令已发送即认为已切页"漂移）
# 复现用户报告的 bug：TRIGGER PAGE:GUITAR 后首条命令 ACK 丢失，RLCD 仍是 HOME；
# 旧逻辑因 last_sent 乐观置为 GUITAR，下一相同手势判 already-there 而永久跳过。
# 预填窗口到 min_agree(3) 使 "enough" 成立，再断言决策（避免单帧 no-trigger 干扰）：
def fill_window(trig, finger, base, n=3):
    for i in range(n):
        trig.window.append((finger, base - i))


# S8：构造 drift —— last_sent 乐观=GUITAR，但设备真实页 confirmed=HOME
trig = FingerTrigger(min_agree=3, win_sec=3.0, cooldown=1.5)
trig.last_sent = "PAGE:GUITAR"          # 旧逻辑的乐观记录（漂移来源）
trig.last_sent_t = 0.0
fill_window(trig, 2, 100.0)
dec, _, _ = trig.update(True, 2, 100.0, confirmed_page="PAGE:HOME")
check("S8 drift: confirmed=HOME 举2指 仍触发(非 already-there)", dec == "trigger")

# S9：confirmed 与 desired 一致 -> already-there，且不受无关 last_sent 干扰
trig = FingerTrigger(min_agree=3, win_sec=3.0, cooldown=1.5)
trig.last_sent = "PAGE:HOME"             # 与之无关的乐观值
trig.last_sent_t = 0.0
fill_window(trig, 2, 100.0)
dec, _, _ = trig.update(True, 2, 100.0, confirmed_page="PAGE:GUITAR")
check("S9 confirmed=GUITAR 举2指 -> already-there", dec == "already-there")

# S10：confirmed=None 时回退乐观 last_sent（dry-run 兼容）
trig = FingerTrigger(min_agree=3, win_sec=3.0, cooldown=1.5)
trig.last_sent = "PAGE:GUITAR"
trig.last_sent_t = 0.0
fill_window(trig, 2, 100.0)
dec, _, _ = trig.update(True, 2, 100.0, confirmed_page=None)
check("S10 confirmed=None 回退 last_sent 举2指 -> already-there(dry-run兼容)",
      dec == "already-there")

print(f"\n=== {PASS} passed, {FAIL} failed ===")
sys.exit(1 if FAIL else 0)
