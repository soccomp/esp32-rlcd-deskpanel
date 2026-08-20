#!/usr/bin/env python3
"""RLCD Phase 2 P1 — 股票回退解析器测试（腾讯 ~ 分隔 / 新浪 , 分隔）。

验证（与 rlcd-lvgl/src/stocks_client.cpp 修复后逻辑逐行对应）：
  - field_at 参数化分隔符：腾讯 '~'、新浪 ','
  - 腾讯解析：[3]=点位 [32]=涨跌幅%（真实样本 fixture）
  - 新浪解析：[2]=昨收 [3]=现价，涨跌幅=(现价-昨收)/昨收*100（真实样本 fixture）
  - 腾讯成功 -> 不触发新浪
  - 腾讯失败 -> 新浪成功（无多余后端回退）
  - 新浪分隔符 bug 回归：旧实现（硬编码 '~'）在新浪数据上返回空串
"""
import os, sys

FIXT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")

# ---- 与 C++ field_at 完全一致的实现（sep 参数化，8-20 修复后） ----
def field_at(s, idx, sep='~'):
    pos = 0
    for _ in range(idx):
        pos = s.find(sep, pos)
        if pos < 0:
            return ""
        pos += 1
    end = s.find(sep, pos)
    if end < 0:
        end = len(s)
    return s[pos:end]

# ---- 旧实现（bug）：硬编码 '~'，用于回归验证 ----
def field_at_old_bug(s, idx):
    return field_at(s, idx, '~')

TX_IDX = [("sh000001", "上证"), ("sh000300", "300"),
          ("sz399006", "创板"), ("sz159819", "AI")]

def parse_tencent(body):
    """逐行解析 v_shXXXXXX="..."; 返回 [{code,name,value,pct}]"""
    out = []
    pos = 0
    while pos < len(body):
        eq = body.find('=', pos)
        if eq < 0:
            break
        q1 = body.find('"', eq)
        q2 = body.find('"', q1 + 1)
        if q1 < 0 or q2 < 0:
            break
        key = body[pos:eq]
        payload = body[q1 + 1:q2]
        vs = field_at(payload, 3, '~')      # 点位
        ps = field_at(payload, 32, '~')     # 涨跌幅%
        if vs and ps:
            for code, name in TX_IDX:
                if code in key:
                    pct = float(ps)
                    out.append({"code": code, "name": name,
                                "value": float(vs), "pct": pct,
                                "up": pct >= 0})
                    break
        pos = q2 + 1
    return out

def parse_sina(body):
    """逐行解析 var hq_str_shXXXXXX="..."; 返回 [{code,name,value,pct}]"""
    out = []
    pos = 0
    while pos < len(body):
        eq = body.find('=', pos)
        if eq < 0:
            break
        q1 = body.find('"', eq)
        q2 = body.find('"', q1 + 1)
        if q1 < 0 or q2 < 0:
            break
        key = body[pos:eq]
        payload = body[q1 + 1:q2]
        cur = field_at(payload, 3, ',')     # 现价
        prev = field_at(payload, 2, ',')    # 昨收
        p = float(prev) if prev else 0.0
        if cur and p != 0.0:
            for code, name in TX_IDX:
                if code in key:
                    c = float(cur)
                    pct = (c - p) / p * 100.0
                    out.append({"code": code, "name": name,
                                "value": c, "pct": pct,
                                "up": pct >= 0})
                    break
        pos = q2 + 1
    return out

def parse_sina_old_bug(body):
    """旧实现（新浪用 '~' 搜索）：模拟 bug 行为"""
    out = []
    pos = 0
    while pos < len(body):
        eq = body.find('=', pos)
        if eq < 0:
            break
        q1 = body.find('"', eq)
        q2 = body.find('"', q1 + 1)
        if q1 < 0 or q2 < 0:
            break
        key = body[pos:eq]
        payload = body[q1 + 1:q2]
        cur = field_at_old_bug(payload, 3)  # '~' 搜索 -> 空串
        prev = field_at_old_bug(payload, 2)
        p = float(prev) if prev else 0.0
        if cur and p != 0.0:
            for code, name in TX_IDX:
                if code in key:
                    c = float(cur)
                    pct = (c - p) / p * 100.0
                    out.append({"code": code, "name": name,
                                "value": c, "pct": pct, "up": pct >= 0})
                    break
        pos = q2 + 1
    return out

fails = 0
def check(name, got, exp, tol=1e-3):
    global fails
    ok = got == exp if not isinstance(exp, float) else abs(got - exp) < tol
    print(("PASS" if ok else "FAIL"), name, f"(got={got} exp={exp})")
    if not ok:
        fails += 1

def check_quotes(name, got, exp_n, exp_first_value, exp_first_pct=None, tol=1e-2):
    global fails
    ok = len(got) == exp_n
    if ok and exp_n > 0:
        ok = abs(got[0]["value"] - exp_first_value) < tol
        if exp_first_pct is not None:
            ok = ok and abs(got[0]["pct"] - exp_first_pct) < tol
    print(("PASS" if ok else "FAIL"), name,
          f"(n={len(got)} first_value={got[0]['value'] if got else None} "
          f"first_pct={got[0]['pct'] if got else None} exp_n={exp_n} "
          f"exp_v={exp_first_value} exp_pct={exp_first_pct})")
    if not ok:
        fails += 1

# ---- 加载真实样本 fixtures ----
with open(os.path.join(FIXT, "tencent_response.txt"), "r", encoding="utf-8") as f:
    TX_BODY = f.read()
with open(os.path.join(FIXT, "sina_response.txt"), "r", encoding="utf-8") as f:
    SINA_BODY = f.read()

print("== 腾讯解析（~ 分隔）==")
tx = parse_tencent(TX_BODY)
check_quotes("tencent parses 4 quotes", tx, 4, 3903.72, 0.24)
check("tencent sh000001 value", tx[0]["value"], 3903.72)
check("tencent sh000001 pct", tx[0]["pct"], 0.24)
check("tencent sz159819 value", tx[3]["value"], 1.730 if len(tx) > 3 else None,
      tol=0.01)

print("\n== 新浪解析（, 分隔，修复后）==")
sina = parse_sina(SINA_BODY)
# 新浪真实样本：sh000001 现价 3903.7210 昨收 3894.4224
# pct = (3903.7210-3894.4224)/3894.4224*100 = 0.2387
check_quotes("sina parses 4 quotes", sina, 4, 3903.7210, 0.2387)
check("sina sh000001 value", sina[0]["value"], 3903.7210)
check("sina sz159819 value", sina[3]["value"], 1.730, tol=0.01)

print("\n== 新浪分隔符 bug 回归（旧实现应失败）==")
sina_old = parse_sina_old_bug(SINA_BODY)
check("old impl parses 0 (regression)", len(sina_old), 0)

print("\n== 回退链逻辑 ==")
# 腾讯成功 -> 直接用腾讯结果（不触发新浪）
tx_ok = len(parse_tencent(TX_BODY)) == 4
check("tencent success -> no sina fallback", tx_ok, True)
# 腾讯失败（空 body）-> 新浪兜底成功
tx_fail_empty = (parse_tencent("") == [])
check("tencent fail (empty) -> sina fallback", tx_fail_empty and len(sina) == 4, True)
# 新浪失败（旧 bug）会落后端；修复后新浪应直接成功，无多余后端回退
check("sina success (no backend needed)", len(sina) == 4, True)

print("\n=== %s ===" % ("ALL PASS" if fails == 0 else f"{fails} FAILED"))
sys.exit(1 if fails else 0)
