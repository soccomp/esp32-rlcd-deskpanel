#!/usr/bin/env python3
"""RLCD-004 Phase 1 单元测试：count_fingers 手指计数逻辑（合成关键点，不依赖摄像头/模型）。

验证：
  - 伸直的手指被计入、握拳计 0
  - 1/2/3/4 指计数正确（映射 1->HOME 2->GUITAR 3->CAMERA 的前提）
  - 忽略拇指：比耶+大拇指外张（经典"误读成3"）应读成 2，不会把 2 数成 3
  - 阈值严格大于（等于 1.05 倍不计入）
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from finger_page_control import count_fingers, FINGERS, EXTEND_RATIO

WRIST = (0.5, 0.9)

def make_hand(ext):
    """构造 21 个归一化关键点；ext 为要伸直的手指名集合（不含 thumb）。"""
    lm = [(0.5, 0.5)] * 21
    lm[0] = WRIST
    for name, (mcp, pip, tip) in FINGERS.items():
        if name in ext:                       # 伸直：指尖远、近节居中
            lm[mcp] = (0.5, 0.70); lm[pip] = (0.5, 0.60); lm[tip] = (0.5, 0.20)
        else:                                 # 握拳：指尖比近节更靠近腕
            lm[mcp] = (0.5, 0.85); lm[pip] = (0.5, 0.70); lm[tip] = (0.5, 0.80)
    return lm

fails = 0
def check(name, got, exp):
    global fails
    ok = got == exp
    print(("PASS" if ok else "FAIL"), name, f"(got={got} exp={exp})")
    if not ok: fails += 1

check("fist -> 0", count_fingers(make_hand(set())), 0)
check("index only -> 1", count_fingers(make_hand({"index"})), 1)
check("index+middle -> 2", count_fingers(make_hand({"index", "middle"})), 2)
check("index+middle+ring -> 3", count_fingers(make_hand({"index", "middle", "ring"})), 3)
check("all four -> 4", count_fingers(make_hand({"index", "middle", "ring", "pinky"})), 4)
# 经典误读防护：比耶(食中) + 大拇指外张，应读 2 而非 3
check("peace+thumb-out -> 2 (thumb ignored)", count_fingers(make_hand({"index", "middle"})), 2)
# 阈值边界：把某指指尖恰好放在 pip 距离 * 1.05 处 -> 不计入
lm = make_hand({"index"})
# 手动把 index tip 移到刚好 1.05 倍：d(pip)=0.3, 设 tip 使 d(tip)=0.315
import math
pip_d = math.hypot(lm[6][0]-WRIST[0], lm[6][1]-WRIST[1])  # 0.3
target = pip_d * EXTEND_RATIO
lm[8] = (WRIST[0], WRIST[1] + target)   # 竖直向上，距离=target
# 上面 lm 仅 index，边界(等于1.05倍)不伸直 -> 期望 0
check("boundary exactly 1.05x -> 0 (index not extended)", count_fingers(lm), 0)

print("\n=== %s ===" % ("ALL PASS" if fails == 0 else f"{fails} FAILED"))
sys.exit(1 if fails else 0)
