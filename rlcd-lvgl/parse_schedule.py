#!/usr/bin/env python3
"""
parse_schedule.py — 会议日程表 解析 & API 服务

功能：
  1. 从图片/PDF 提取会议数据 → 结构化 JSON（schedule.json）
  2. 智能识别个人会议：检测 attendees/host/organizer 是否包含指定姓名
  3. 标记视频会议(★) / 涉密会议(▲)
  4. 启动 FastAPI HTTP 服务，暴露 GET /api/schedule 供 ESP32 拉取

用法：
  python parse_schedule.py              # 启动 API 服务（默认 http://0.0.0.0:8100）
  python parse_schedule.py --dump       # 仅输出 JSON 到 stdout，不启动服务
  python parse_schedule.py --name 张三   # 用其他名字检测个人会议（默认：你的姓名）

ESP32 端调用示例：
  GET http://<你的IP>:8100/api/schedule
  GET http://<你的IP>:8100/api/schedule?name=张三&mine_only=true   # 只返回我的会议
"""

import json
import re
import argparse
import os
import sys
import time
import threading
import urllib.request
import urllib.parse
from datetime import datetime
from pathlib import Path
import html as _html
import xml.etree.ElementTree as ET

# ============================================================
#  数据源：从「一周主要会议日程表」图片手工提取（已脱敏，见下方示例数据）
#  原始文档：2026年7月27日—2026年8月2日  第2版
# ============================================================

# ============================================================
#  数据源：示例会议数据（已脱敏）
#  原始实现为手工提取的真实会议日程（含个人隐私，公开仓库中已移除）。
#  正式运行请准备 schedule.json（见 load_meetings_from_file，文件存在时优先加载）。
# ============================================================

RAW_MEETINGS = [
    {
        "date": "2026-01-05", "weekday": "星期一",
        "time": "10:00",
        "location": "第一会议室",
        "title": "项目周例会（示例数据）",
        "host": "项目负责人",
        "attendees": "项目组全体成员",
        "organizer": "项目管理部",
    },
    {
        "date": "2026-01-06", "weekday": "星期二",
        "time": "14:00—16:00",
        "location": "视频会议室",
        "title": "技术评审会（示例数据，★视频会议）",
        "host": "技术总监",
        "attendees": "技术部、质量部",
        "organizer": "技术委员会",
        "is_video_conf": True,
    },
    {
        "date": "2026-01-07", "weekday": "星期三",
        "time": "09:00",
        "location": "第二会议室",
        "title": "保密专项检查部署会（示例数据，▲涉密）",
        "host": "保密办",
        "attendees": "各部门保密员",
        "organizer": "保密办公室",
        "is_secret_conf": True,
    },
]


# ============================================================
#  天气模块（Open-Meteo 免费 API，无需 Key）
#  - GET /api/schedule 时按需抓取，30 分钟本地缓存
#  - 网络失败时保留上一次成功数据（stale 也比没有强）
#  - WMO weathercode → ESP32 端 4 类图标 code: sun/cloud/rain/snow
# ============================================================

# 常用城市坐标表（可用 --city 指定；不在表中时走 Open-Meteo 地理编码）
CITY_COORDS = {
    "北京": (39.9042, 116.4074),
    "上海": (31.2304, 121.4737),
    "广州": (23.1291, 113.2644),
    "深圳": (22.5431, 114.0579),
    "杭州": (30.2741, 120.1551),
    "成都": (30.5728, 104.0668),
    "西安": (34.3416, 108.9398),
    "武汉": (30.5928, 114.3055),
    "南京": (32.0603, 118.7969),
    "廊坊": (39.5186, 116.7030),
}

# WMO Weather interpretation codes → (中文描述, 图标code)
# 图标 code 仅 4 类，对应 ESP32 端单色点阵图标：sun / cloud / rain / snow
WMO_CODE_MAP = {
    0:  ("晴",       "sun"),
    1:  ("晴",       "sun"),
    2:  ("多云",     "cloud"),
    3:  ("阴",       "cloud"),
    45: ("雾",       "cloud"),
    48: ("雾凇",     "cloud"),
    51: ("毛毛雨",   "rain"),
    53: ("毛毛雨",   "rain"),
    55: ("毛毛雨",   "rain"),
    56: ("冻雨",     "rain"),
    57: ("冻雨",     "rain"),
    61: ("小雨",     "rain"),
    63: ("中雨",     "rain"),
    65: ("大雨",     "rain"),
    66: ("冻雨",     "rain"),
    67: ("冻雨",     "rain"),
    71: ("小雪",     "snow"),
    73: ("中雪",     "snow"),
    75: ("大雪",     "snow"),
    77: ("霰",       "snow"),
    80: ("阵雨",     "rain"),
    81: ("阵雨",     "rain"),
    82: ("暴雨",     "rain"),
    85: ("阵雪",     "snow"),
    86: ("阵雪",     "snow"),
    95: ("雷阵雨",   "rain"),
    96: ("雷雨冰雹", "rain"),
    99: ("雷雨冰雹", "rain"),
}

WEATHER_CACHE_TTL = 30 * 60   # 30 分钟缓存

_weather_lock = threading.Lock()
_weather_cache = {
    "data": None,       # 上一次成功的 weather dict
    "fetched_at": 0.0,  # 上一次成功抓取时间戳
    "city": None,
}


def _http_get_json(url: str, timeout: float = 6.0):
    """标准库抓取 JSON（不引入 requests 依赖）"""
    req = urllib.request.Request(url, headers={"User-Agent": "rlcd-desk-panel/1.0"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


# ============================================================
#  摄像头帧代理（/api/camframe + /api/camstream）
#  ⚠️ 背景：BTWIFI6-169148 对 ESP32 出站 TCP 做 per-device RST
#  （RLCD 是 ESP32-S3 出站被重置），RLCD→摄像头直连不稳定。
#  方案：Mac 后端后台线程常驻抓摄像头 /capture，缓存最新 JPEG 到
#  内存，RLCD/浏览器 GET /api/camframe、/api/camstream 秒回——
#  RLCD→Mac 与 Mac→摄像头两条路径都稳定，彻底绕开 RST。
#  2026-08-07 改：Mac→摄像头 用【socket 长连接 keep-alive】抓帧——
#  摄像头 WebServer 是单客户端模型（高频新建连接会排队超时→花屏黑屏），
#  长连接反复 GET 同一连接完美匹配；Mac 是电脑不受 RST 限制，可行。
# ============================================================
import socket as _socket

# 摄像头地址：默认用 mDNS 主机名 esp32cam.local（摄像头固件注册），
# 可通过环境变量 CAM_CAPTURE_HOST 覆盖为固定 IP（公开仓库不暴露内网 IP）。
_CAM_CAPTURE_HOST = os.environ.get("CAM_CAPTURE_HOST", "esp32cam.local")
_CAM_CAPTURE_PORT = 80
_cam_lock = threading.Lock()
_cam_frame = None          # bytes: 最新 JPEG
_cam_frame_seq = 0         # 递增序号（RLCD 判断是否有新帧）
_cam_last_ok = 0.0         # 上次成功抓取时间戳
_cam_grabber_started = False


def _jpeg_valid(data: bytes) -> bool:
    """JPEG 完整性：FFD8 开头 + FFD9 结尾（防花屏=半帧）"""
    return (len(data) > 200 and data[:2] == b"\xff\xd8"
            and data[-2:] == b"\xff\xd9")


def _cam_grabber_loop():
    """后台线程：连接摄像头 /stream MJPEG 流持续收帧，缓存最新完整 JPEG。
    视频流架构——一条连接持续推帧，零请求开销、零时序竞争；
    连接断开自动重建。失败保留最后一帧并快速重试。
    RLCD-002 Issue B 修复：接收缓冲改为「连接局部」buf（bytearray），
    每个新连接从空缓冲开始——旧连接残留字节绝不会污染新流；
    同时校验 /stream 响应为 200 才解析 multipart。"""
    global _cam_frame, _cam_frame_seq, _cam_last_ok
    sock = None
    buf = None          # 连接局部接收缓冲（bytearray）；重连后重建为空
    fail_streak = 0
    while True:
        try:
            if sock is None:
                sock = _socket.create_connection(
                    (_CAM_CAPTURE_HOST, _CAM_CAPTURE_PORT), timeout=5.0)
                sock.settimeout(5.0)
                req = (
                    f"GET /stream HTTP/1.1\r\n"
                    f"Host: {_CAM_CAPTURE_HOST}\r\n"
                    "Connection: close\r\n\r\n"
                )
                sock.sendall(req.encode())
                # 新连接：全新空接收缓冲
                buf = bytearray()
                # 校验状态行确实是 200，否则不把非 MJPEG 响应当流解析
                status_line = _recv_line(sock, buf)
                parts = status_line.split()
                if len(parts) < 2 or parts[1] != b"200":
                    raise ConnectionError(f"stream bad status: {status_line[:60]!r}")
                # 跳过其余响应头（与 body 共用同一 buf，不丢字节）
                while True:
                    line = _recv_line(sock, buf)
                    if line in (b"\r\n", b""):
                        break
                fail_streak = 0
            # 从 multipart 流读一帧
            data = _cam_read_multipart(sock, buf)
            if _jpeg_valid(data):
                with _cam_lock:
                    _cam_frame = data
                    _cam_frame_seq += 1
                _cam_last_ok = time.time()
                fail_streak = 0
            else:
                fail_streak += 1
        except Exception:
            fail_streak += 1
            if sock is not None:
                try:
                    sock.close()
                except Exception:
                    pass
                sock = None
            buf = None            # 丢弃死连接的解析状态（新连接重建空 buf）
            # 失败退避：短暂重试，不长时间阻塞
            time.sleep(0.5 if fail_streak < 10 else 2.0)


def _cam_read_multipart(sock, buf) -> bytes:
    """从 MJPEG 流读取一帧完整 JPEG（boundary --camframe）。
    流格式：--camframe\\r\\n 头字段\\r\\n\\r\\n JPEG数据 \\r\\n
    容忍 boundary 前的空白行（上一帧尾部 \\r\\n）。
    buf: 连接局部 bytearray 接收缓冲（与 _recv_line 共享）。"""
    # 跳过空白行，直到 boundary
    while True:
        line = _recv_line(sock, buf)
        if line.startswith(b"--camframe"):
            break
        if line not in (b"\r\n", b""):
            raise ValueError(f"bad boundary: {line[:40]!r}")
    # 读帧头字段（Content-Length）
    clen = None
    while True:
        line = _recv_line(sock, buf)
        if line in (b"\r\n", b""):
            break
        low = line.lower()
        if low.startswith(b"content-length:"):
            clen = int(line.split(b":", 1)[1].strip())
    if clen is None or clen <= 0:
        raise ValueError(f"bad Content-Length: {clen}")
    # 精确读 JPEG body
    body = b""
    while len(body) < clen:
        chunk = sock.recv(min(65536, clen - len(body)))
        if not chunk:
            raise ConnectionError("stream closed in body")
        body += chunk
    return body[:clen]


def _recv_line(sock, buf, maxlen=4096) -> bytes:
    """读一行（到 \\n），缓冲式读取（避免 recv(1) 逐字节慢吞），带长度保护。
    buf: 连接局部 bytearray（就地累积/消费），调用方持有——不再使用模块全局，
    从根本上杜绝跨连接的状态污染。"""
    while b"\n" not in buf:
        chunk = sock.recv(8192)
        if not chunk:
            raise ConnectionError("stream closed in line")
        buf += chunk
        if len(buf) > 65536:
            raise ValueError("recv buffer overflow")
    idx = buf.index(b"\n") + 1
    line = bytes(buf[:idx])
    del buf[:idx]
    if len(line) > maxlen:
        raise ValueError("line too long")
    return line

def _ensure_cam_grabber():
    """惰性启动抓帧线程（幂等）"""
    global _cam_grabber_started
    if not _cam_grabber_started:
        _cam_grabber_started = True
        t = threading.Thread(target=_cam_grabber_loop, daemon=True)
        t.start()


def get_cam_frame() -> dict:
    """供 /api/camframe 调用：返回 (bytes, seq)；无帧返回 (None, 0)"""
    _ensure_cam_grabber()
    with _cam_lock:
        return _cam_frame, _cam_frame_seq


def _geocode_city(city: str):
    """城市名 → 经纬度。先查内置表，查不到走 Open-Meteo 地理编码。"""
    if city in CITY_COORDS:
        return CITY_COORDS[city]
    url = (
        "https://geocoding-api.open-meteo.com/v1/search?"
        + urllib.parse.urlencode({"name": city, "count": 1, "language": "zh"})
    )
    data = _http_get_json(url)
    results = data.get("results") or []
    if not results:
        raise ValueError(f"地理编码失败：找不到城市 {city}")
    return results[0]["latitude"], results[0]["longitude"]


def _wmo_to_text_code(wmo: int):
    return WMO_CODE_MAP.get(int(wmo), ("多云", "cloud"))


def fetch_weather(city: str) -> dict:
    """
    抓取今日 + 明日天气预报（Open-Meteo daily 接口）。
    返回结构与 /api/schedule 的 weather 字段一致；失败时抛异常，由调用方兜底。
    """
    lat, lon = _geocode_city(city)
    url = (
        "https://api.open-meteo.com/v1/forecast?"
        + urllib.parse.urlencode({
            "latitude": lat,
            "longitude": lon,
            "daily": "weathercode,temperature_2m_max,temperature_2m_min",
            "timezone": "Asia/Shanghai",
            "forecast_days": 2,
        })
    )
    data = _http_get_json(url)
    daily = data["daily"]

    today_text, today_code = _wmo_to_text_code(daily["weathercode"][0])
    tomorrow_text, tomorrow_code = _wmo_to_text_code(daily["weathercode"][1])

    return {
        "city": city,
        "today_text": today_text,
        "today_code": today_code,
        "today_high": round(daily["temperature_2m_max"][0]),
        "today_low": round(daily["temperature_2m_min"][0]),
        "tomorrow_text": tomorrow_text,
        "tomorrow_code": tomorrow_code,
        "tomorrow_high": round(daily["temperature_2m_max"][1]),
        "tomorrow_low": round(daily["temperature_2m_min"][1]),
    }


def get_weather_cached(city: str):
    """
    带缓存的天气获取：
    - 30 分钟内命中缓存直接返回（ESP32 每小时拉一次，不会打爆第三方 API）
    - 抓取失败时返回上一次成功数据（stale）；从未成功过则返回 None
    """
    now = time.time()
    with _weather_lock:
        fresh = (
            _weather_cache["data"] is not None
            and _weather_cache["city"] == city
            and (now - _weather_cache["fetched_at"]) < WEATHER_CACHE_TTL
        )
        if fresh:
            return _weather_cache["data"]

    try:
        w = fetch_weather(city)
        with _weather_lock:
            _weather_cache["data"] = w
            _weather_cache["fetched_at"] = now
            _weather_cache["city"] = city
        print(f"[weather] {city}: 今 {w['today_text']} {w['today_low']}~{w['today_high']}°C / "
              f"明 {w['tomorrow_text']} {w['tomorrow_low']}~{w['tomorrow_high']}°C",
              file=sys.stderr)   # 日志走 stderr，保证 --dump 的 stdout 是纯 JSON
        return w
    except Exception as e:
        print(f"[weather] 抓取失败（{e}），使用上次缓存", file=sys.stderr)
        with _weather_lock:
            return _weather_cache["data"]   # 可能为 None，调用方需容忍


# ============================================================
#  和风天气（QWeather）代理：Mac 出口正常可抓，供 ESP32 回退使用
# ============================================================
QW_HOST = "ku3h2tg63t.re.qweatherapi.com"
QW_KEY  = "6e111479f6fd439684e205e40d36ebe4"
QW_LOC  = "101010200"          # 北京海淀
QW_CITY = "海淀"

_qw_lock = threading.Lock()
_qw_cache = {"data": None, "fetched_at": 0.0}
QW_TTL = 2 * 60 * 60            # 逐天预报官方建议缓存 1-6 小时；取 2 小时省配额


class QWeatherPermanentError(Exception):
    """和风永久性错误（401/403/404）：key/额度/位置问题，需人工处理，不反复重试。"""
    pass


def _fetch_qh_weather():
    """抓和风 3d 预报 → 返回 {today_text, today_icon, today_high, today_low,
    tomorrow_text, tomorrow_icon, tomorrow_high, tomorrow_low, city}"""
    import gzip as _gzip
    url = (
        f"https://{QW_HOST}/v7/weather/3d?location={QW_LOC}"
        f"&key={QW_KEY}&lang=zh&unit=m"
    )
    req = urllib.request.Request(url, headers={"User-Agent": "rlcd-desk-panel/1.0"})
    with urllib.request.urlopen(req, timeout=8.0) as resp:
        raw = resp.read()
    if raw[:2] == b"\x1f\x8b":           # gzip 解压
        raw = _gzip.decompress(raw)
    d = json.loads(raw.decode("utf-8"))
    rc = d.get("code")
    if rc != "200":
        # 永久性错误（401 key错/403 额度冻结/404 位置错）：标记避免反复抓取
        if rc in ("401", "403", "404"):
            raise QWeatherPermanentError(f"和风 API code={rc}")
        raise ValueError(f"和风 API code={rc}")
    daily = d.get("daily") or []
    if len(daily) < 2:
        raise ValueError("和风 daily < 2")
    t0, t1 = daily[0], daily[1]
    return {
        "city": QW_CITY,
        "today_text": t0.get("textDay", "多云"),
        "today_icon": t0.get("iconDay", ""),
        "today_high": int(t0.get("tempMax", 0)),
        "today_low": int(t0.get("tempMin", 0)),
        "tomorrow_text": t1.get("textDay", "多云"),
        "tomorrow_icon": t1.get("iconDay", ""),
        "tomorrow_high": int(t1.get("tempMax", 0)),
        "tomorrow_low": int(t1.get("tempMin", 0)),
    }


def get_qh_weather_cached():
    """带 2 小时缓存的和风天气；失败返回上一次成功数据（可为 None）。
    永久错误（401/403/404）记录时间戳，24 小时内不再重复抓取（避免被视作攻击）。"""
    now = time.time()
    with _qw_lock:
        if (_qw_cache["data"] is not None
                and (now - _qw_cache["fetched_at"]) < QW_TTL):
            return _qw_cache["data"]
        if _qw_cache.get("perm_at") and (now - _qw_cache["perm_at"]) < 24 * 3600:
            # 永久错误冷却期内：不再尝试，返回上次数据（可为 None）
            return _qw_cache["data"]
    try:
        w = _fetch_qh_weather()
        with _qw_lock:
            _qw_cache["data"] = w
            _qw_cache["fetched_at"] = now
            _qw_cache.pop("perm_at", None)   # 成功则清除永久错误标记
        print(f"[weather_qh] {w['city']}: 今 {w['today_text']} "
              f"{w['today_low']}~{w['today_high']}°C", file=sys.stderr)
        return w
    except QWeatherPermanentError as e:
        print(f"[weather_qh] 永久错误（{e}），24h 内不再重试", file=sys.stderr)
        with _qw_lock:
            _qw_cache["perm_at"] = now
            return _qw_cache["data"]
    except Exception as e:
        print(f"[weather_qh] 抓取失败（{e}），使用上次缓存", file=sys.stderr)
        with _qw_lock:
            return _qw_cache["data"]


# ============================================================
#  X(Twitter) 最新帖文模块（第三方 RSS 桥，免费、无需登录）
#  - 主源 nitter.net/<user>/rss，备用 xcancel.com/<user>/rss
#  - 取 feed 第一条 <item> 的 <title>（nitter 已按时间倒序）
#  - 清洗：“用户名: ”前缀、HTML 实体(&amp; &lt; 等)、多余空白
#  - 5 分钟本地缓存：ESP32 每小时拉一次，不刷爆第三方实例
#  - 失败时返回上一次成功数据；从未成功过返回友好占位
# ============================================================

XFEED_USER = "thsottiaux"
XFEED_SOURCES = [
    "https://nitter.net/thsottiaux/rss",
    "https://xcancel.com/thsottiaux/rss",   # 需设 UA（脚本已带）
]
XFEED_CACHE_TTL = 5 * 60
XFEED_TEXT_MAX = 130    # X 卡已全给帖文（124px 高 ≈ 6-7 行），英文 130 字符≈6 行；超长加 "…"

_xfeed_lock = threading.Lock()
_xfeed_cache = {
    "data": None,        # 上一次成功的 xfeed dict
    "fetched_at": 0.0,
}


def _clean_tweet_text(raw: str) -> str:
    """清洗 RSS <title>：去前缀、反转义 HTML 实体、压缩空白。"""
    if not raw:
        return ""
    prefix = XFEED_USER + ":"
    if raw.startswith(prefix):
        raw = raw[len(prefix):].lstrip()
    raw = _html.unescape(raw)            # &amp; -> &, &lt; -> <, &#39; -> '
    raw = " ".join(raw.split())          # 折叠连续空白/换行
    if len(raw) > XFEED_TEXT_MAX:
        raw = raw[:XFEED_TEXT_MAX].rstrip() + "…"
    return raw


def _fmt_pubdate(pub: str) -> str:
    """RSS pubDate -> 本地简短时间（MM-DD HH:MM），只有日期则 MM-DD。

    各 nitter 实例的 pubDate 格式不完全一致，逐个尝试；
    解析失败才原样截断。
    """
    if not pub:
        return ""
    candidates = [
        ("%a, %d %b %Y %H:%M:%S %z", True),
        ("%a, %d %b %Y %H:%M:%S %Z", True),
        ("%a, %d %b %Y %H:%M", True),
        ("%Y-%m-%dT%H:%M:%S%z", True),
        ("%a, %d %b %Y", False),
    ]
    for fmt, has_time in candidates:
        try:
            dt = datetime.strptime(pub, fmt).astimezone()
            return dt.strftime("%m-%d %H:%M") if has_time else dt.strftime("%m-%d")
        except Exception:
            continue
    return pub[:16]


def fetch_xfeed(force: bool = False) -> dict:
    """
    抓取 @thsottiaux 最新帖文。
    返回 {"user","text","time","source"}；全源失败返回占位。
    """
    now = time.time()
    with _xfeed_lock:
        if not force and _xfeed_cache["data"] is not None \
                and (now - _xfeed_cache["fetched_at"]) < XFEED_CACHE_TTL:
            return _xfeed_cache["data"]

    last_err = "unknown"
    for url in XFEED_SOURCES:
        try:
            req = urllib.request.Request(
                url, headers={"User-Agent": "Mozilla/5.0 (rlcd-xfeed/1.0)"}
            )
            with urllib.request.urlopen(req, timeout=10) as resp:
                xml_bytes = resp.read()
            root = ET.fromstring(xml_bytes)

            # RSS 2.0: channel/item/title
            items = root.findall(".//item")
            if not items:
                # 退路：Atom 格式 entry/title
                atom_ns = "{http://www.w3.org/2005/Atom}"
                items = root.findall(f".//{atom_ns}entry")
            if not items:
                last_err = f"{url}: no items"
                continue

            first = items[0]
            title_el = first.find("title")
            date_el = first.find("pubDate")
            # Atom 命名空间下的子元素
            if title_el is None:
                title_el = first.find("{http://www.w3.org/2005/Atom}title")
            if date_el is None:
                date_el = first.find("{http://www.w3.org/2005/Atom}updated")

            text = _clean_tweet_text(title_el.text if title_el is not None else "")
            pub = date_el.text if date_el is not None else ""
            result = {
                "user": XFEED_USER,
                "text": text or "（帖子内容为空）",
                "time": _fmt_pubdate(pub),
                "source": url,
            }
            with _xfeed_lock:
                _xfeed_cache["data"] = result
                _xfeed_cache["fetched_at"] = now
            print(f"[xfeed] {XFEED_USER}: {result['text'][:40]}… ({result['time']})",
                  file=sys.stderr)
            return result
        except Exception as e:
            last_err = f"{url}: {e}"
            print(f"[xfeed] 抓取失败 {last_err}", file=sys.stderr)
            continue

    # 全源失败：返回上次缓存（若有），否则占位
    with _xfeed_lock:
        if _xfeed_cache["data"] is not None:
            return _xfeed_cache["data"]
    return {
        "user": XFEED_USER,
        "text": "（X 帖子获取失败，稍后重试）",
        "time": "",
        "source": "error:" + last_err,
    }


# ============================================================
#  股票指数行情模块（腾讯 + 东方财富双源，免费、无需 key）
#  - 上证 sh000001 / 沪深300 sh000300 / 创业板 sz399006 → 腾讯行情
#  - CS人工智能 930713（简称 AI）→ 东方财富 push2（腾讯未收录该指数）
#  - 返回 [{name, code, value, change, pct}]；value/pct 为 float
#  - 60 秒本地缓存：ESP32 每 10 分钟拉一次，不刷爆上游接口
#  - 失败时返回上一次成功数据；从未成功过返回占位（全 0）
# ============================================================

STOCK_CODES = [
    ("sh000001", "上证"),
    ("sh000300", "300"),
    ("sz399006", "创板"),
]
STOCK_EXTRA = [
    ("2.930713", "AI"),        # 东方财富 secid：CS人工智（中证AI主题指数）
]
STOCK_CACHE_TTL = 60          # 秒
STOCK_API_URL = "https://qt.gtimg.cn/q={codes}"

_stock_lock = threading.Lock()
_stock_cache = {
    "data": None,
    "fetched_at": 0.0,
}


def _fetch_stock_quotes() -> list:
    """抓取腾讯(3指数) + 东方财富(930713) 行情并合并为 [{name,code,value,pct,up}]。

    腾讯返回形如：
      v_sh000001="1~上证指数~000001~3822.28~3809.66~...~12.62~0.33~...";
    字段（~ 分隔，索引）：
      [0] 市场标志, [1] 名称, [2] 代码, [3] 当前点位, [4] 昨收,
      [31] 涨跌额, [32] 涨跌幅(%)
    东方财富 push2 返回 JSON：f43=最新(×100), f57=代码, f58=名称,
      f60=昨收(×100), f170=涨跌幅(×100)
    """
    out = []

    # ---- 腾讯：3 个常见指数 ----
    codes = ",".join(c for c, _ in STOCK_CODES)
    url = STOCK_API_URL.format(codes=codes)
    req = urllib.request.Request(url, headers={"User-Agent": "rlcd-desk-panel/1.0"})
    with urllib.request.urlopen(req, timeout=6.0) as resp:
        raw = resp.read().decode("gbk", errors="replace")   # 腾讯行情 GBK 编码

    for line in raw.splitlines():
        line = line.strip()
        if not line or "=" not in line:
            continue
        key, _, payload = line.partition("=")
        code = key.replace("v_", "").strip()
        if not payload.startswith('"'):
            continue
        fields = payload.strip('"').split("~")
        if len(fields) < 33:
            continue
        try:
            value = float(fields[3])
            change = float(fields[31])   # 涨跌额
            pct = float(fields[32])      # 涨跌幅 %
        except ValueError:
            continue
        # 名称映射：腾讯给全名（上证指数/沪深300/创业板指），映射成缩写
        name = code
        for c, short in STOCK_CODES:
            if c == code:
                name = short
                break
        out.append({
            "name": name,
            "code": code,
            "value": value,
            "pct": pct,
            "up": pct >= 0,
        })

    # ---- 东方财富：930713 CS人工智能（腾讯未收录），带重试抗限流 ----
    for secid, short in STOCK_EXTRA:
        last_err = "unknown"
        ok = False
        em_headers = {
            "User-Agent": ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                           "AppleWebKit/537.36 (KHTML, like Gecko) "
                           "Chrome/126.0.0.0 Safari/537.36"),
            "Accept": "*/*",
            "Accept-Language": "zh-CN,zh;q=0.9",
            "Referer": "https://quote.eastmoney.com/zs930713.html",
        }
        # 东财主域名(push2)偶发限流断连；备用域名 push2delay 稳定，失败切换
        em_hosts = ["push2.eastmoney.com", "push2delay.eastmoney.com"]
        for em_host in em_hosts:
            if ok:
                break
            em_url = (
                f"https://{em_host}/api/qt/stock/get?"
                f"secid={secid}&fields=f43,f57,f58,f60,f170&invt=2"
            )
            for attempt in range(2):      # 每域名重试 2 次
                try:
                    em_req = urllib.request.Request(em_url, headers=em_headers)
                    with urllib.request.urlopen(em_req, timeout=6.0) as resp:
                        d = json.loads(resp.read().decode("utf-8"))
                    data = d.get("data")
                    if data and data.get("f43") is not None and data.get("f170") is not None:
                        out.append({
                            "name": short,
                            "code": str(data.get("f57", secid)),
                            "value": data["f43"] / 100.0,
                            "pct": data["f170"] / 100.0,
                            "up": data["f170"] >= 0,
                        })
                        ok = True
                    else:
                        last_err = "empty data"
                    break
                except Exception as e:
                    last_err = str(e)
                    time.sleep(1.0 + attempt)   # 退避：1s, 2s
        if not ok:
            print(f"[stocks] 东财抓取 {secid} 失败: {last_err}", file=sys.stderr)
            # 东财失败：用上次成功的 AI 数据补位（避免屏幕 AI 行消失）
            with _stock_lock:
                old = _stock_cache.get("data")
            if old:
                for oq in old.get("stocks", []):
                    if oq.get("code") == str(secid).split(".")[-1] and oq.get("value"):
                        out.append(oq)
                        break
    return out


_STOCK_CACHE_FILE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "stocks_cache.json")


def _stock_disk_save(result: dict):
    """把最近一次成功行情持久化到磁盘：进程重启后仍可恢复（不再返回空数组）。"""
    try:
        with open(_STOCK_CACHE_FILE, "w", encoding="utf-8") as f:
            json.dump(result, f, ensure_ascii=False)
    except Exception as e:
        print(f"[stocks] 磁盘缓存写入失败: {e}", file=sys.stderr)


def _stock_disk_load():
    try:
        if os.path.exists(_STOCK_CACHE_FILE):
            with open(_STOCK_CACHE_FILE, "r", encoding="utf-8") as f:
                return json.load(f)
    except Exception:
        pass
    return None


def fetch_stocks(force: bool = False) -> dict:
    """返回 {"stocks": [...], "updated": "HH:MM"}。

    全源失败时：优先内存缓存 → 磁盘缓存 → 最后才返回带 error 的空占位
    （handler 会把 error 转成 HTTP 503，避免 ESP32 渲染 "-- 0.00 0.00%" 假数据）。
    """
    now = time.time()
    with _stock_lock:
        if not force and _stock_cache["data"] is not None \
                and (now - _stock_cache["fetched_at"]) < STOCK_CACHE_TTL:
            return _stock_cache["data"]

    try:
        quotes = _fetch_stock_quotes()
        if not quotes:
            raise RuntimeError("行情源返回空数据")
        result = {
            "stocks": quotes,
            "updated": datetime.now().strftime("%H:%M"),
        }
        with _stock_lock:
            _stock_cache["data"] = result
            _stock_cache["fetched_at"] = now
        _stock_disk_save(result)
        print(f"[stocks] OK: " + " / ".join(
            f"{q['name']} {q['value']:.2f} {q['pct']:+.2f}%" for q in quotes))
        return result
    except Exception as e:
        print(f"[stocks] 抓取失败: {e}", file=sys.stderr)
        with _stock_lock:
            if _stock_cache["data"] is not None:
                return _stock_cache["data"]
        # 内存缓存也没有（进程刚重启）：尝试磁盘持久化缓存
        disk = _stock_disk_load()
        if disk and disk.get("stocks"):
            with _stock_lock:
                _stock_cache["data"] = disk
                _stock_cache["fetched_at"] = now
            print(f"[stocks] 恢复磁盘缓存 {len(disk['stocks'])} 条")
            return disk
        return {"stocks": [], "updated": "", "error": str(e)}


# ============================================================
#  核心逻辑
# ============================================================

def detect_video_conf(title: str) -> bool:
    """检测是否为视频会议（★ 标记）"""
    return "★" in title


def detect_secret_conf(title: str) -> bool:
    """检测是否为涉密会议（▲ 标记）"""
    return "▲" in title


def is_my_meeting(meeting: dict, name: str) -> bool:
    """
    智能识别个人会议：
    检测 attendees / host / organizer 字段中是否包含指定姓名。
    支持中文全名匹配，忽略括号内的补充说明。
    """
    if not name:
        return False

    # 需要检查的字段
    fields_to_check = [
        meeting.get("host", ""),
        meeting.get("organizer", ""),
        meeting.get("attendees", ""),
    ]

    for field in fields_to_check:
        if not field:
            continue
        # 直接包含名字
        if name in field:
            return True
        # 处理括号内名单格式："xxx（张三、李四）"
        paren_match = re.search(r"[（(](.+?)[)）]", field)
        if paren_match:
            names_in_paren = paren_match.group(1)
            # 支持顿号/逗号分隔
            for n in re.split(r"[、，,]", names_in_paren):
                if n.strip() == name:
                    return True

    return False


def enrich_meetings(raw_meetings: list, target_name: str) -> list:
    """对原始数据进行字段补全和智能标记"""
    enriched = []
    for m in raw_meetings:
        entry = dict(m)  # 浅拷贝，不污染原始数据
        entry["is_video_conf"] = detect_video_conf(entry.get("title", ""))
        entry["is_secret_conf"] = detect_secret_conf(entry.get("title", ""))
        entry["is_my_meeting"] = is_my_meeting(entry, target_name)
        enriched.append(entry)
    return enriched


def load_meetings_from_file(filepath: str = None) -> list:
    """
    从 schedule.json 文件动态加载会议数据。
    文件不存在或解析失败时回退到 RAW_MEETINGS（硬编码兜底）。
    """
    if filepath is None:
        filepath = str(Path(__file__).parent / "schedule.json")
    try:
        with open(filepath, "r", encoding="utf-8") as f:
            data = json.load(f)
        meetings = data.get("meetings")
        if meetings and isinstance(meetings, list) and len(meetings) > 0:
            # 去掉可能已存在的标记字段，交给 enrich_meetings 重新计算
            clean = []
            for m in meetings:
                clean.append({k: v for k, v in m.items()
                              if k not in ("is_video_conf", "is_secret_conf", "is_my_meeting")})
            return clean
    except (FileNotFoundError, json.JSONDecodeError, KeyError):
        pass
    return RAW_MEETINGS


def load_source_from_file(filepath: str = None) -> str:
    """从 schedule.json 文件读取 source 字段，失败时回退到默认值。"""
    if filepath is None:
        filepath = str(Path(__file__).parent / "schedule.json")
    try:
        with open(filepath, "r", encoding="utf-8") as f:
            data = json.load(f)
        src = data.get("source")
        if src and isinstance(src, str):
            return src
    except (FileNotFoundError, json.JSONDecodeError):
        pass
    return "一周主要会议日程表"


def build_schedule_json(target_name: str = "你的姓名", city: str = None) -> dict:
    """构建完整的 schedule.json 结构。city 非空时附带 weather 字段（失败为 None）。"""
    meetings = enrich_meetings(load_meetings_from_file(), target_name)

    my_count = sum(1 for m in meetings if m["is_my_meeting"])
    video_count = sum(1 for m in meetings if m["is_video_conf"])
    secret_count = sum(1 for m in meetings if m["is_secret_conf"])

    result = {
        "update_time": datetime.now().strftime("%Y-%m-%d %H:%M"),
        "source": load_source_from_file(),
        "target_name": target_name,
        "statistics": {
            "total": len(meetings),
            "my_meetings": my_count,
            "video_conferences": video_count,
            "secret_conferences": secret_count,
        },
        "meetings": meetings,
    }
    if city:
        # 天气获取失败时为 None：ESP32 端优雅退化显示“天气获取中...”
        result["weather"] = get_weather_cached(city)
    return result


def save_schedule_json(filepath: str, target_name: str = "你的姓名") -> str:
    """保存 JSON 到文件，返回实际写入路径"""
    data = build_schedule_json(target_name)
    p = Path(filepath)
    p.parent.mkdir(parents=True, exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    return str(p.absolute())


# ============================================================
#  可选：从 PDF/图片自动提取（需要额外依赖）
# ============================================================

def extract_from_pdf(pdf_path: str) -> list:
    """
    从 PDF 文件提取表格数据。
    需要：pip install pdfplumber
    注意：此函数为预留接口，当前主数据源为手工提取的 RAW_MEETINGS。
    如需启用自动提取，取消注释下方代码。
    """
    # try:
    #     import pdfplumber
    #     results = []
    #     with pdfplumber.open(pdf_path) as pdf:
    #         for page in pdf.pages:
    #             tables = page.extract_tables()
    #             for table in tables:
    #                 # TODO: 根据 actual 表格结构解析
    #                 pass
    #     return results
    # except ImportError:
    #     print("[WARN] pdfplumber 未安装，无法提取 PDF。运行: pip install pdfplumber")
    #     return []
    raise NotImplementedError(
        "PDF 自动提取需安装 pdfplumber。"
        "当前版本使用手工提取数据（RAW_MEETINGS），如需自动提取请自行实现。"
    )


def extract_from_image(image_path: str) -> list:
    """
    从图片提取表格数据。
    需要：pip install paddleocr 或使用云端 OCR API。
    注意：此函数为预留接口。
    """
    raise NotImplementedError(
        "图片 OCR 提取需安装 PaddleOCR 或接入云端 API。"
        "当前版本使用手工提取数据（RAW_MEETINGS）。"
    )


# ============================================================
#  FastAPI HTTP 服务
# ============================================================

def create_app(target_name: str = "你的姓名", city: str = "北京"):
    """创建 FastAPI 应用实例"""
    from fastapi import FastAPI, Query
    from fastapi.responses import JSONResponse
    import uvicorn

    app = FastAPI(
        title="会议日程 API",
        description="为 ESP32-S3 RLCD 桌面面板提供会议日程 + 天气数据",
        version="1.1.0",
    )

    @app.get("/")
    def root():
        return {
            "service": "Schedule API",
            "version": "1.1.0",
            "target_name": target_name,
            "city": city,
            "endpoints": {
                "/api/schedule": "获取完整会议日程 JSON（含 weather 天气字段）",
                "/api/schedule?mine_only=true": "仅返回我的会议",
                "/api/schedule?name=XXX": "用其他名字检测",
                "/api/schedule?city=XXX": "指定天气城市（覆盖默认值）",
                "/api/weather": "仅返回天气",
                "/api/stats": "统计摘要",
            },
        }

    @app.get("/api/schedule")
    def get_schedule(
        name: str = None,
        mine_only: bool = False,
        city_q: str = Query(None, alias="city"),
    ):
        """
        获取会议日程（根节点含 weather 字段，ESP32 一次拉取会议+天气）。

        Query params:
          name:      指定检测名字（覆盖默认值）
          mine_only: true 时只返回 is_my_meeting=true 的记录
          city:      指定天气城市（覆盖默认值）
        """
        query_name = name or target_name
        data = build_schedule_json(query_name, city=(city_q or city))

        if mine_only:
            data["meetings"] = [m for m in data["meetings"] if m["is_my_meeting"]]
            data["statistics"]["returned"] = len(data["meetings"])
            data["filter"] = "mine_only"

        return JSONResponse(content=data)

    @app.get("/api/weather")
    def get_weather(city_q: str = Query(None, alias="city")):
        """仅返回天气（调试用）"""
        w = get_weather_cached(city_q or city)
        return JSONResponse(content={"weather": w})

    @app.get("/api/weather_qh")
    def get_weather_qh():
        """和风天气（QWeather）代理：Mac 出口抓取，供 ESP32 回退。
        返回 {city, today_text, today_icon, today_high, today_low,
              tomorrow_text, tomorrow_icon, tomorrow_high, tomorrow_low}"""
        w = get_qh_weather_cached()
        if w is None:
            return JSONResponse(content={"error": "weather unavailable"}, status_code=503)
        return JSONResponse(content=w)

    @app.get("/api/xfeed")
    def get_xfeed():
        """
        X(Twitter) @thsottiaux 最新帖文（第三方 RSS 桥，免费）。
        返回 {"user","text","time","source"}
        """
        return JSONResponse(content=fetch_xfeed())

    @app.get("/api/camframe")
    def get_camframe():
        """
        摄像头帧代理：后台线程每 500ms 抓 esp32cam /capture 缓存最新 JPEG，
        RLCD 从这里拉帧（绕开 BTWIFI6 对 ESP32 出站 TCP 的 per-device RST）。
        返回 image/jpeg 原始帧；无帧返回 503。
        """
        from fastapi.responses import Response
        data, seq = get_cam_frame()
        if data is None:
            return JSONResponse(content={"error": "no cam frame"}, status_code=503)
        return Response(
            content=data,
            media_type="image/jpeg",
            headers={"X-Cam-Seq": str(seq)},
        )

    @app.get("/api/camstream")
    async def get_camstream():
        """
        MJPEG 实时流：multipart/x-mixed-replace 无限推帧，浏览器
        <img src="/api/camstream"> 即可实时播放（0 延迟、无 JS）。
        帧更新时才推送（seq 变化），客户端断开自动停止。
        ⚠️ 必须用 async generator + await asyncio.sleep——sync generator
        的 time.sleep 会阻塞 uvicorn 事件循环（黑屏/数秒一帧的根因）。
        """
        from fastapi.responses import StreamingResponse
        import asyncio

        BOUNDARY = b"camframe"
        last_seq = -1

        async def gen():
            nonlocal last_seq
            while True:
                data, seq = get_cam_frame()
                if data is not None and seq != last_seq:
                    last_seq = seq
                    head = (
                        b"--" + BOUNDARY + b"\r\n"
                        b"Content-Type: image/jpeg\r\n"
                        b"Content-Length: " + str(len(data)).encode() + b"\r\n"
                        b"X-Cam-Seq: " + str(seq).encode() + b"\r\n"
                        b"\r\n"
                    )
                    yield head + data + b"\r\n"
                await asyncio.sleep(0.05)   # 不阻塞事件循环，快速轮询新帧

        return StreamingResponse(
            gen(),
            media_type="multipart/x-mixed-replace; boundary=camframe",
            headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
        )

    @app.get("/api/stocks")
    def get_stocks():
        """
        三指数行情：上证/沪深300/创业板指（腾讯行情 API，免费）。
        返回 {"stocks":[{name,code,value,pct,up}], "updated":"HH:MM"}
        行情源全失败且无任何缓存 → 503（ESP32 端走缓存恢复路径，不渲染空数据）。
        """
        result = fetch_stocks()
        if result.get("error"):
            return JSONResponse(content=result, status_code=503)
        return JSONResponse(content=result)

    @app.get("/api/stats")
    def get_stats(name: str = None):
        """返回统计摘要"""
        query_name = name or target_name
        data = build_schedule_json(query_name)
        return JSONResponse(content={
            "update_time": data["update_time"],
            "target_name": query_name,
            **data["statistics"],
        })

    @app.get("/health")
    def health():
        return {"status": "ok", "timestamp": datetime.now().isoformat()}

    return app


# ============================================================
#  CLI 入口
# ============================================================

def main():
    parser = argparse.ArgumentParser(
        description="会议日程表 解析 & API 服务",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python parse_schedule.py                  # 启动 API 服务 (http://0.0.0.0:8100)
  python parse_schedule.py --dump           # 输出 JSON 到 stdout
  python parse_schedule.py --dump -o schedule.json  # 保存到文件
  python parse_schedule.py --port 9000       # 自定义端口
  python parse_schedule.py --name 张三       # 用其他名字检测
        """,
    )
    parser.add_argument(
        "--dump", action="store_true",
        help="仅输出 JSON 到 stdout，不启动服务",
    )
    parser.add_argument(
        "-o", "--output", default=None,
        help="输出 JSON 文件路径（默认: ./schedule.json）",
    )
    parser.add_argument(
        "--name", default="你的姓名",
        help="用于检测个人会议的名字（默认: 你的姓名）",
    )
    parser.add_argument(
        "--city", default="北京",
        help="天气预报城市（默认: 北京；Open-Meteo 免费接口，无需 Key）",
    )
    parser.add_argument(
        "--host", default="0.0.0.0",
        help="API 监听地址（默认: 0.0.0.0）",
    )
    parser.add_argument(
        "--port", type=int, default=8100,
        help="API 监听端口（默认: 8100）",
    )
    args = parser.parse_args()

    if args.dump:
        # 仅输出模式（--dump 也带天气，便于本地验证 weather 字段）
        data = build_schedule_json(args.name, city=args.city)
        json_str = json.dumps(data, ensure_ascii=False, indent=2)
        if args.output:
            save_schedule_json(args.output, args.name)
            print(f"[OK] 已保存到 {args.output} ({len(json_str)} bytes)")
        else:
            print(json_str)
        return

    # 服务模式
    output_path = args.output or "./schedule.json"
    saved = save_schedule_json(output_path, args.name)
    print(f"=" * 55)
    print(f"  会议日程 API 服务")
    print(f"=" * 55)
    print(f"  目标姓名 : {args.name}")
    print(f"  天气城市 : {args.city}")
    print(f"  数据文件 : {saved}")
    print(f"  监听地址 : http://{args.host}:{args.port}")
    print(f"  API 端点 :")
    print(f"    GET /               服务信息")
    print(f"    GET /api/schedule   完整日程 JSON（含 weather 天气）")
    print(f"    GET /api/schedule?mine_only=true  仅我的会议")
    print(f"    GET /api/weather    仅天气（调试）")
    print(f"    GET /api/stats      统计摘要")
    print(f"    GET /health         健康检查")
    print(f"=" * 55)
    print(f"  [提示] ESP32 端将 WiFi 连上后访问此 URL 即可拉取日程")
    print()

    import uvicorn
    app = create_app(args.name, args.city)
    uvicorn.run(app, host=args.host, port=args.port, log_level="info")


if __name__ == "__main__":
    main()
