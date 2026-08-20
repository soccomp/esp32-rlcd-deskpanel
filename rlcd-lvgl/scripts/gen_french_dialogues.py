#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成法语对话内置词库头文件。
用法: python3 gen_french_dialogues.py
产出: src/french_dialogues.h（200 组对话结构体数组 A/B 法语 + A/B 中文）
字体: 法语卡用已有的 14px chinese_14（全量 CJK+Latin），无需额外生成字体。
"""
import os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # rlcd-lvgl/
SCRIPTS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPTS)
from french_dialogues import DIALOGUES


def cstr(s):
    # 转义 C 字符串；本词库无 " 与 \ 与换行，安全起见仍处理
    return s.replace("\\", "\\\\").replace('"', '\\"')


lines = []
lines.append("#pragma once")
lines.append("")
lines.append("/* 200 组法语对话（A/B 法语 + A/B 中文），由 scripts/french_dialogues.py 生成。")
lines.append(" * 数据来源: scripts/french_dialogues.py（勿手改本文件，改后重跑 gen_french_dialogues.py）。")
lines.append(" * french_dialogue_t 定义在 french_lib.h；本文件仅 french_lib.cpp 包含，避免数据重复编入。 */")
lines.append("")
lines.append('#include "french_lib.h"')
lines.append("")
lines.append("static const french_dialogue_t kFrenchDialogues[] = {")
for a, b, ac, bc in DIALOGUES:
    lines.append(f'    {{ "{cstr(a)}", "{cstr(b)}", "{cstr(ac)}", "{cstr(bc)}" }},')
lines.append("};")
lines.append("")
lines.append(f"#define FRENCH_DIALOGUES_COUNT {len(DIALOGUES)}")
lines.append("")

h_path = os.path.join(ROOT, "src", "french_dialogues.h")
with open(h_path, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
print(f"generated: {h_path} ({len(DIALOGUES)} dialogues)")
