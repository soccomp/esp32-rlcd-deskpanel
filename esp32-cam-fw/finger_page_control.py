#!/usr/bin/env python3
"""RLCD-004：手指数量 -> RLCD 页面切换（M1 侧视觉识别）。

闭环（方案 B，WiFi 直连，去掉 USB 串口链路）：
  ESP32-CAM /capture (HTTP) --> 本程序识别手指数量
      --命令 TCP--> RLCD:8771 命令服务 --> RLCD 切页
      <-- ACK:PAGE:X (同一 TCP 连接回传) --

职责分离（任务单要求）：视觉模型只跑在 M1，ESP32 侧只收 "PAGE:xxx" 简单命令。

映射：
  1 根手指 -> PAGE:HOME     首页
  2 根手指 -> PAGE:GUITAR   吉他页
  3 根手指 -> PAGE:CAMERA   摄像头页

防抖（RLCD-004.1 改进）：用「滚动时间窗口 + 多数表决 + 冷却」替代原先的严格连续 N 帧。
      1~2fps 下，偶发漏检/误数会让"连续 5 帧一致"几乎无法满足；新策略在最近 win_sec
      （默认 3s）窗口内，只要 ≥min_agree（默认 3）帧认同同一手指数即触发，天然容忍
      窗口内的个别噪点帧；触发后清空窗口并进入 cooldown（默认 1.5s）冷却，避免快速跳页。
      同一页面不重复发送。

投递可靠性（RLCD-004.2 改进）：M1 发出 PAGE 命令后**不再**乐观地认为已切页。
      RLCD 收到命令即经 WiFi TCP 命令服务回 ``ACK:PAGE:X``（同一连接），本程序
      只有收到对应 ACK 才更新权威当前页 confirmed_page，"Already current" 仅基于
      confirmed_page。
      未在 ack_timeout（默认 1.2s）内收到 ACK 则自动重发，最多 max_attempts（默认 3）
      次，仍无 ACK 记 ``PAGE ACK TIMEOUT``。彻底消除"命令已发送即认为已切页"的状态漂移。

手指判定：只统计食指/中指/无名指/小指四指，忽略拇指——拇指自然外张
      极易把"2"读成"3"。伸直判据用「指尖到腕距 > 近节指关节到腕距」，
      不依赖手的朝向，比常见的 y 坐标比较更耐旋转。

用法：
  python3 finger_page_control.py                      # 常规运行（控制台调试输出）
  python3 finger_page_control.py --dry-run            # 只识别不发命令
  python3 finger_page_control.py --show               # 打开 OpenCV 实时调试窗口
  python3 finger_page_control.py --save-dir /tmp/x    # 另存带标注的调试图
  # 触发调参（RLCD-004.1，默认即适合 1~2fps）：
  python3 finger_page_control.py --min-agree 3 --win-sec 3.0 --cooldown 1.5
  # 兼容旧参数：--consec 等同 --min-agree
"""
import argparse
import math
import os
import queue
import socket
import sys
import threading
import time
import http.server
import urllib.request
from collections import deque

import cv2
import numpy as np

# Plan B WiFi 直连传输参数：
#   帧来源：ESP32-CAM 的 /capture HTTP 接口（mDNS esp32cam / 回退 192.168.100.199）
#   命令出口 + 回执：RLCD 的 WiFi TCP 命令服务（默认 192.168.100.197:8771）
CAM_HOST, CAM_PORT = "esp32cam", 80
CAM_FALLBACK = "192.168.100.199"
CAM_PATH = "/capture"
RLCD_HOST, RLCD_PORT = "192.168.100.197", 8771

# ★ 8-18 必须绕过系统 HTTP 代理，否则内网取帧全部 502 Bad Gateway。
# 本机环境为 HTTP_PROXY=http://127.0.0.1:7897/（Clash），而
# NO_PROXY 只有 "localhost,127.0.0.1,::1" —— **不含 192.168.100.0/24**。
# urllib.request.urlopen() 默认读取系统/环境代理设置，于是对
# http://192.168.100.199/capture 的请求被交给代理，代理去公网找这个私有地址
# 自然失败，返回 502 Bad Gateway（现象：finger 持续 "frame error: camera
# capture failed: HTTP Error 502"，而同一 URL 用 curl 却是 200 —— 因为 Clash
# 对私有网段有 DIRECT 规则，curl 的请求形式被放行，urllib 的不被放行）。
# 摄像头与 RLCD 都在局域网内，任何情况下都不应该经过代理，故显式置空 ProxyHandler。
_DIRECT_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))

PAGE_CMD = {1: "PAGE:HOME", 2: "PAGE:GUITAR", 3: "PAGE:CAMERA"}

MODEL_URL = ("https://storage.googleapis.com/mediapipe-models/hand_landmarker/"
             "hand_landmarker/float16/1/hand_landmarker.task")
MODEL_PATH = os.path.expanduser("~/.cache/mediapipe/hand_landmarker.task")

# MediaPipe Hands 21 关键点：0=腕，(mcp, pip, tip) 四指
FINGERS = {"index": (5, 6, 8), "middle": (9, 10, 12),
           "ring": (13, 14, 16), "pinky": (17, 18, 20)}
EXTEND_RATIO = 1.05        # 指尖到腕距 / 近节到腕距 超过此值判为伸直


def log(msg):
    print(f"[finger {time.strftime('%H:%M:%S')}] {msg}", flush=True)


# ---------------------------------------------------------------- 手部识别
class HandDetector:
    """封装 MediaPipe：优先用新的 Tasks API，回退到旧的 solutions.hands。"""

    def __init__(self):
        self.mode = None
        try:
            from mediapipe.tasks import python as mp_python
            from mediapipe.tasks.python import vision
            import mediapipe as mp

            self._mp = mp
            self._ensure_model()
            opts = vision.HandLandmarkerOptions(
                base_options=mp_python.BaseOptions(model_asset_path=MODEL_PATH),
                running_mode=vision.RunningMode.IMAGE,
                num_hands=1,
                min_hand_detection_confidence=0.5,
                min_hand_presence_confidence=0.5,
                min_tracking_confidence=0.5)
            self._landmarker = vision.HandLandmarker.create_from_options(opts)
            self.mode = "tasks"
        except Exception as e:                       # 老版本或模型下载失败
            log(f"tasks API unavailable ({e}); falling back to solutions.hands")
            import mediapipe as mp
            self._mp = mp
            self._hands = mp.solutions.hands.Hands(
                static_image_mode=False, max_num_hands=1,
                min_detection_confidence=0.5, min_tracking_confidence=0.5)
            self.mode = "solutions"

    @staticmethod
    def _ensure_model():
        if os.path.exists(MODEL_PATH) and os.path.getsize(MODEL_PATH) > 100000:
            return
        os.makedirs(os.path.dirname(MODEL_PATH), exist_ok=True)
        log(f"downloading hand_landmarker model -> {MODEL_PATH}")
        urllib.request.urlretrieve(MODEL_URL, MODEL_PATH)
        log(f"model ready ({os.path.getsize(MODEL_PATH)} bytes)")

    def detect(self, bgr):
        """返回归一化关键点列表 [(x, y), ...21] 或 None。"""
        rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
        if self.mode == "tasks":
            image = self._mp.Image(image_format=self._mp.ImageFormat.SRGB, data=rgb)
            res = self._landmarker.detect(image)
            if not res.hand_landmarks:
                return None
            return [(p.x, p.y) for p in res.hand_landmarks[0]]
        res = self._hands.process(rgb)
        if not res.multi_hand_landmarks:
            return None
        return [(p.x, p.y) for p in res.multi_hand_landmarks[0].landmark]


def count_fingers(lm):
    """统计伸直的手指数（食指/中指/无名指/小指，忽略拇指）。"""
    wx, wy = lm[0]

    def d(i):
        return math.hypot(lm[i][0] - wx, lm[i][1] - wy)

    n = 0
    for _, (_mcp, pip, tip) in FINGERS.items():
        if d(tip) > d(pip) * EXTEND_RATIO:
            n += 1
    return n


# ---------------------------------------------------------------- WiFi 直连传输（Plan B）
def _resolve(host, fallback):
    """mDNS 解析摄像头主机：依次试 host.local 与 host，都不行回退 static IP。"""
    for cand in (host + ".local", host):
        try:
            return socket.gethostbyname(cand)
        except (socket.gaierror, OSError):
            continue
    return fallback


class WiFiLink:
    """Plan B 传输层：HTTP 取摄像头帧 + TCP 给 RLCD 发命令并收 ACK。

    与旧 HubClient 保持相同接口（connect / close / send_cmd / read_frame /
    ack_queue / sock），主循环逻辑无需改动。帧走 ESP32-CAM 的 /capture HTTP，
    命令与回执走 RLCD 的 WiFi TCP 命令服务（默认 :8771）。彻底去掉
    camusb_bridge 本地 hub / USB 串口链路。

    RLCD-004.2：单独 recv 线程拆出 ``ACK:PAGE:X`` 文本行入 ack_queue，不阻塞主循环。
    """

    def __init__(self, cam_host=CAM_HOST, cam_fallback=CAM_FALLBACK,
                 rld_host=RLCD_HOST, rld_port=RLCD_PORT):
        self.cam_host = cam_host
        self.cam_fallback = cam_fallback
        self.rld_host = rld_host
        self.rld_port = rld_port
        self.sock = None                 # RLCD TCP（主循环据此判断是否已连接）
        self.ack_queue = queue.Queue()   # RLCD 回执 ACK:PAGE:X
        self._recv = None
        self._closed = False
        self._cam_ip = cam_fallback
        # 帧端口：绑了本地代理 -> 直连摄像头(:80)；否则从本地代理(:8780)取帧
        self._cam_port = CAM_PORT if g_i_am_proxy else PROXY_PORT

    def ensure_cmd_link(self):
        """建立/复用 RLCD TCP 命令连接（持久；帧取回失败不会断开它）。

        Plan B 关键修正：旧 HubClient 把"帧(USB)"和"命令(TCP)"当作同一条链路，
        任一失败都整体重连。改为 WiFi 后帧走 HTTP、命令走 TCP 是**两条独立链路**，
        故这里只负责命令 TCP 的生命周期——帧失败只重试 HTTP GET，绝不碰命令连接。
        RLCD 的命令服务是单客户端模型，频繁 close/reconnect 会把它冲垮导致
        ``create_connection`` 超时，所以命令连接必须持久化。"""
        if self.sock is not None and not self._closed:
            return
        # 解析摄像头 IP（mDNS 优先，回退 static）——供 read_frame 取帧用，独立于命令链路
        if g_i_am_proxy:
            # 本实例绑定了本地代理 -> 它是摄像头唯一消费者，直连摄像头取帧
            self._cam_ip = _resolve(self.cam_host, self.cam_fallback)
            self._cam_port = CAM_PORT
            log(f"resolved camera -> {self._cam_ip} (mdns {self.cam_host})")
        else:
            # 没绑代理端口 -> 从本地代理取帧（摄像头唯一消费者是对端绑定者），避免双消费者
            self._cam_ip = "127.0.0.1"
            self._cam_port = PROXY_PORT
            log(f"frame source -> local proxy 127.0.0.1:{PROXY_PORT}")
        # 连接 RLCD TCP 命令服务
        s = socket.create_connection((self.rld_host, self.rld_port), timeout=5)
        s.settimeout(1.0)
        self.sock = s
        self._closed = False
        self._recv = threading.Thread(target=self._recv_loop, daemon=True)
        self._recv.start()
        log(f"connected to RLCD cmd server {self.rld_host}:{self.rld_port}")

    def close_cmd_link(self):
        """仅关闭命令 TCP 连接（由调用方在 send 失败/recv 线程退出后触发重连）。"""
        self._closed = True
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass
        self.sock = None

    def cmd_ready(self):
        """命令 TCP 当前是否可用（用于发送前判断，避免对死连接盲目 send）。"""
        return self.sock is not None and not self._closed

    def send_cmd(self, cmd):
        self.sock.sendall(cmd.encode("ascii") + b"\n")

    def _recv_loop(self):
        # 该 TCP 只传文本 ACK 行，按 \\n 拆出 ACK:PAGE:X；socket.timeout 是 OSError
        # 子类，必须单独捕获，否则误杀 recv 线程。
        while not self._closed:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue                    # 无数据：继续等，绝不退出
            except OSError:
                self._closed = True
                return
            if not chunk:
                self._closed = True
                return
            for line in chunk.decode("ascii", "ignore").splitlines():
                line = line.strip()
                if line.startswith("ACK:"):
                    self.ack_queue.put(line)

    def read_frame(self, timeout=8):
        """HTTP GET 摄像头 /capture 取一帧 JPEG；失败抛 ConnectionError。

        与命令 TCP 完全独立：本函数只做 HTTP 取帧，无论成败都不触动 RLCD 命令连接。
        """
        url = f"http://{self._cam_ip}:{self._cam_port}{CAM_PATH}"
        try:
            req = urllib.request.Request(
                url, headers={"User-Agent": "finger_page_control/1.1"})
            # 用 _DIRECT_OPENER 而非 urlopen：内网地址必须绕过系统代理（详见顶部注释）
            with _DIRECT_OPENER.open(req, timeout=timeout) as r:
                data = r.read()
        except (urllib.error.URLError, OSError, socket.timeout) as e:
            raise ConnectionError(f"camera capture failed: {e}")
        if len(data) < 64 or not data.startswith(b"\xff\xd8"):
            raise ConnectionError("camera returned non-JPEG payload")
        return data


# ---------------------------------------------------------------- 本地帧代理（Plan B+）
# ESP32-CAM 的 HTTP server 是单线程、一次只服务一个连接（/capture 单次 ~1s），
# 若 RLCD 与 M1 同时拉 /capture 会被挤爆（实测双消费者 60% 超时）。
# 故：M1 作为摄像头**唯一**外部消费者拉帧，并把最新帧缓存在本地 :8780 HTTP
# 代理；RLCD 改为从 M1 代理取帧。摄像头只剩 1 个消费者 -> 稳定；RLCD 走
# localhost 代理取帧也稳定。M1 宕机时 RLCD 回退直连摄像头（无手势，独占不挤）。
PROXY_PORT = 8780
g_latest_jpeg = [b""]          # 最新一帧 JPEG（列表容器，避免 global 声明）
g_jpeg_lock = threading.Lock()
g_i_am_proxy = False           # 本实例是否成功绑定 :8780 代理（唯一摄像头消费者）


class _QuietThreadingHTTPServer(http.server.ThreadingHTTPServer):
    """静默版 ThreadingHTTPServer。

    RLCD 端为根治 TIME_WAIT 耗尽 lwIP PCB 池，取帧连接改用 SO_LINGER=0 关闭
    （发 RST 而非 FIN）。服务端因此每帧都会收到 ConnectionResetError ——
    这是预期行为而非故障，若不静默，socketserver 会为每一帧打印一整段
    traceback，把日志彻底刷爆。handle_error 定义在 socketserver.BaseServer 上，
    必须覆盖 server 而不是 handler。"""
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        pass


class _FrameHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/capture"):
            with g_jpeg_lock:
                data = g_latest_jpeg[0]
            if not data:
                self.send_response(503)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        else:
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"ok")

    def log_message(self, *a):
        pass   # 静默，避免刷日志


def start_frame_proxy(host="0.0.0.0", port=PROXY_PORT, retries=8, delay=0.5):
    """启动本地帧代理。若端口被占用（多为被上一次 -9 杀掉的实例留下 TIME_WAIT
    或被另一个本机实例占用），带重试绑定，避免频繁 kill/重启导致永久绑不上。
    返回 server 表示本实例是摄像头唯一消费者；返回 None 表示端口已被别的实例
    占用，本实例改为从 127.0.0.1:port 取帧（不直连摄像头，杜绝双消费者挤爆）。"""
    global g_i_am_proxy
    last = None
    for attempt in range(1, retries + 1):
        try:
            srv = _QuietThreadingHTTPServer((host, port), _FrameHandler)
            t = threading.Thread(target=srv.serve_forever, daemon=True)
            t.start()
            g_i_am_proxy = True
            log(f"frame proxy started: http://{host}:{port}/capture (feeds latest frame to RLCD)")
            return srv
        except OSError as e:
            last = e
            log(f"frame proxy bind {host}:{port} failed (attempt {attempt}/{retries}): {e}; retry in {delay}s")
            time.sleep(delay)
    # 多次重试仍失败：端口确实被占用 -> 本实例不当摄像头消费者
    g_i_am_proxy = False
    log(f"frame proxy NOT started (port {port} busy) -> this instance will pull frames "
        f"from http://127.0.0.1:{port}/capture (camera sole consumer is the binder)")
    return None


# ---------------------------------------------------------------- 主流程
def annotate(bgr, lm, hand, fingers, cmd):
    img = bgr.copy()
    if lm:
        h, w = img.shape[:2]
        for i, (x, y) in enumerate(lm):
            px, py = int(x * w), int(y * h)
            cv2.circle(img, (px, py), 3, (0, 255, 0) if i else (0, 0, 255), -1)
    lines = [f"HAND:   {hand}", f"FINGER: {fingers}", f"CMD:    {cmd}"]
    for i, t in enumerate(lines):
        cv2.putText(img, t, (8, 22 + i * 22), cv2.FONT_HERSHEY_SIMPLEX,
                    0.6, (0, 0, 0), 3, cv2.LINE_AA)
        cv2.putText(img, t, (8, 22 + i * 22), cv2.FONT_HERSHEY_SIMPLEX,
                    0.6, (0, 255, 255), 1, cv2.LINE_AA)
    return img


# ---------------------------------------------------------------- 触发策略
class FingerTrigger:
    """RLCD-004.1 触发策略：滚动时间窗口 + 多数表决 + 冷却。

    每帧调用 update(hand_present, fingers, now, confirmed_page=None)
      -> (decision, cmd, diag)
      decision: 'no-trigger' | 'already-there' | 'cooldown' | 'trigger'
      cmd     : decision=='trigger' 时返回要发的 PAGE 命令，否则 None
      diag    : {"counts","maj_v","maj_n","wlen"} 仅供日志/测试
    设计要点（对应任务要求：提高成功率、不降稳定性、不快速跳页）：
      - 窗口只收 1/2/3；逐帧漏检/误数不再清零整个连击，只需窗口内多数一致，
        故在 1~2fps + 偶发抖动下也比旧的"严格连续 5 帧"容易满足。
      - 触发后立即清空窗口 -> 需重新摆出手势才再触发，天然防快速跳页。
      - cooldown 冷却期内即便多数一致也不发命令 -> 进一步防跳页/防刷屏。
      RLCD-004.2：'already-there' 以 **confirmed_page**（RLCD 已 ACK 确认的页）
      为准，而非"命令已发送"。confirmed_page 为 None 时退回乐观 last_sent
      （dry-run 兼容）。这彻底消除"命令已发送即认为已切页"的状态漂移。
    """

    def __init__(self, min_agree=3, win_sec=3.0, cooldown=2.5):
        self.min_agree = min_agree
        self.win_sec = win_sec
        self.cooldown = cooldown
        self.window = deque()
        self.last_sent = None          # 兼容 dry-run 的乐观回退值
        self.last_sent_t = 0.0

    def update(self, hand_present, fingers, now, confirmed_page=None):
        if hand_present and fingers in PAGE_CMD:
            self.window.append((fingers, now))
        while self.window and now - self.window[0][1] > self.win_sec:
            self.window.popleft()

        counts = [c for c, _ in self.window]
        tally = {}
        for c in counts:
            tally[c] = tally.get(c, 0) + 1
        maj_v, maj_n = (max(tally.items(), key=lambda kv: kv[1])
                        if tally else (None, 0))
        wlen = len(self.window)

        diag = {"counts": counts, "maj_v": maj_v, "maj_n": maj_n, "wlen": wlen}
        if maj_v is None:
            return "no-trigger", None, diag
        desired = PAGE_CMD[maj_v]
        # 当前页：以 RLCD 已确认页为准；尚无确认时退回乐观 last_sent（dry-run）
        cur = confirmed_page if confirmed_page is not None else self.last_sent
        enough = wlen >= self.min_agree and maj_n >= self.min_agree
        same_page = (desired == cur)
        cooled = (now - self.last_sent_t) >= self.cooldown

        if enough and same_page:
            return "already-there", None, diag
        if enough and not cooled:
            return "cooldown", None, diag
        if enough and not same_page and cooled:
            self.last_sent = desired           # 仅 dry-run 回退用
            self.last_sent_t = now
            self.window.clear()
            return "trigger", desired, diag
        return "no-trigger", None, diag


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--min-agree", type=int, default=3,
                    help="滚动窗口内至少多少帧认同同一手指数才发命令（默认 3）")
    ap.add_argument("--win-sec", type=float, default=3.0,
                    help="滚动时间窗口长度秒（默认 3.0）")
    ap.add_argument("--cooldown", type=float, default=2.5,
                    help="发送命令后冷却秒数，防快速跳页（默认 2.5：密集切页触发 RLCD "
                         "高负载供电复位，2.5s 冷却兼顾响应与稳定性）")
    ap.add_argument("--consec", type=int, default=None,
                    help="(兼容旧参数) 等同 --min-agree")
    ap.add_argument("--ack-timeout", type=float, default=2.0,
                    help="RLCD-004.2：发出 PAGE 后等待 ACK 的超时秒数，超时自动重发"
                         "（默认 2.0：单任务 pump 固件命令→ACK 往返含 JPEG 解码约 2~3s，"
                         "1.2 会误超时；2.0 + 重发3次 ≈ 6s 窗口覆盖最坏链路）")
    ap.add_argument("--max-attempts", type=int, default=3,
                    help="RLCD-004.2：单条命令最多发送次数（含首次），超过未收到 ACK "
                         "则记 PAGE ACK TIMEOUT（默认 3）")
    ap.add_argument("--dry-run", action="store_true", help="只识别，不发命令")
    ap.add_argument("--show", action="store_true", help="打开 OpenCV 调试窗口")
    ap.add_argument("--save-dir", default=None, help="另存带标注的调试图到该目录")
    ap.add_argument("--save-every", type=int, default=1, help="每 N 帧存一张")
    ap.add_argument("--max-frames", type=int, default=0, help=">0 时处理够帧数即退出")
    ap.add_argument("--cam-host", default=CAM_HOST,
                    help="摄像头 mDNS 主机名（默认 esp32cam，解析失败回退 192.168.100.199）")
    ap.add_argument("--rlcd-host", default=RLCD_HOST,
                    help="RLCD WiFi 命令服务主机（默认 192.168.100.197）")
    ap.add_argument("--rlcd-port", type=int, default=RLCD_PORT,
                    help="RLCD WiFi 命令服务端口（默认 8771）")
    args = ap.parse_args()

    if args.save_dir:
        os.makedirs(args.save_dir, exist_ok=True)

    det = HandDetector()
    # Plan B+：本地帧代理，供 RLCD 取帧（M1 是摄像头唯一外部消费者）
    start_frame_proxy()
    # ---- 触发参数（RLCD-004.1：滚动窗口多数表决 + 冷却）----
    min_agree = args.min_agree if args.consec is None else args.consec
    if min_agree < 1:
        min_agree = 1
    win_sec = args.win_sec
    cooldown = args.cooldown

    hub = WiFiLink(args.cam_host, CAM_FALLBACK, args.rlcd_host, args.rlcd_port)
    trig = FingerTrigger(min_agree, win_sec, cooldown)

    # ---- RLCD-004.2：命令投递可靠性状态 ----
    # confirmed_page：RLCD 已 ACK 确认的页（权威"当前页"），None = 尚未确认过
    # pending：已发送、等待 ACK 的命令 {cmd, sent_t, attempts}
    # 只有收到对应 ACK，confirmed_page 才会更新；"Already current" 仅基于 confirmed_page
    confirmed_page = None
    pending = None
    ACK_TIMEOUT = args.ack_timeout     # 未收到 ACK 的重发间隔（秒）
    MAX_ATTEMPTS = args.max_attempts   # 最多发送次数（含首发的 1 次）

    frames = 0
    t0 = time.time()
    log(f"mediapipe backend = {det.mode}, min_agree={min_agree}, "
        f"win_sec={win_sec}, cooldown={cooldown}, ack_timeout={ACK_TIMEOUT}, "
        f"max_attempts={MAX_ATTEMPTS}, dry_run={args.dry_run}")

    while True:
        # 1) 取帧（独立于 RLCD 命令链路：即便 RLCD 暂不可达也照常识别手势）
        try:
            jpg = hub.read_frame()
        except (OSError, ConnectionError) as e:
            log(f"frame error: {e} -- retry in 2s")
            time.sleep(2)
            continue

        bgr = cv2.imdecode(np.frombuffer(jpg, np.uint8), cv2.IMREAD_COLOR)
        if bgr is None:
            continue
        frames += 1
        with g_jpeg_lock:
            g_latest_jpeg[0] = jpg     # 缓存给本地帧代理（喂 RLCD）

        # 2) 确保 RLCD 命令链路（失败仅影响发命令，不阻断识别/代理）
        try:
            hub.ensure_cmd_link()
        except OSError as e:
            log(f"RLCD cmd link down: {e} (gesture still detected, commands paused)")

        now = time.time()

        lm = det.detect(bgr)
        fingers = count_fingers(lm) if lm else 0
        hand = "YES" if lm else "NO"

        # ---- 先消费 RLCD 回执（ACK），更新权威当前页 ----
        while not hub.ack_queue.empty():
            ack = hub.ack_queue.get_nowait()
            if not ack.startswith("ACK:"):
                continue
            page = ack[4:]                      # "ACK:PAGE:MEETING" -> "PAGE:MEETING"
            if page not in PAGE_CMD.values():
                continue
            if pending is not None and pending["cmd"] == page:
                confirmed_page = page
                log(f"ACK: {ack} | CONFIRMED <- {page} (was pending)")
                pending = None
            elif confirmed_page != page:
                confirmed_page = page
                log(f"ACK: {ack} | CONFIRMED <- {page} (late / no pending)")
            else:
                log(f"ACK: {ack} (duplicate)")

        # ---- 触发决策（委托 FingerTrigger：滚动窗口多数表决 + 冷却）----
        # confirmed_page 作为"当前页"权威输入，杜绝乐观状态漂移
        decision, cmd, diag = trig.update(lm is not None, fingers, now, confirmed_page)
        counts, maj_v, maj_n, wlen = (diag["counts"], diag["maj_v"],
                                      diag["maj_n"], diag["wlen"])

        if decision == "trigger":
            cmd_text = cmd
            if pending is not None and pending["cmd"] == cmd:
                # 上一条同页命令仍在等 ACK，交给定时重发，不要重复刷
                log(f"AWAIT-ACK: {cmd} (pending, retry handles resend) | "
                    f"HAND:{hand} FINGER:{fingers if lm else '-'} "
                    f"WIN:{counts} MAJ:{maj_v}({maj_n}/{wlen})")
            else:
                if pending is not None:
                    log(f"CANCEL pending {pending['cmd']} -> new {cmd}")
                    pending = None
                if not args.dry_run:
                    if hub.cmd_ready():
                        try:
                            hub.send_cmd(cmd)
                            pending = {"cmd": cmd, "sent_t": now, "attempts": 1}
                            log(f"SEND: {cmd} (attempt 1) | HAND:{hand} "
                                f"FINGER:{fingers if lm else '-'} "
                                f"WIN:{counts} MAJ:{maj_v}({maj_n}/{wlen})")
                        except OSError as e:
                            log(f"send failed: {e}")
                            hub.close_cmd_link()
                    else:
                        log(f"DEFER: {cmd} (RLCD cmd link down)")
                else:
                    log(f"TRIGGER(dry-run): {cmd} | HAND:{hand} "
                        f"FINGER:{fingers if lm else '-'} "
                        f"WIN:{counts} MAJ:{maj_v}({maj_n}/{wlen})")
        elif decision == "already-there":
            log(f"SKIP: {PAGE_CMD[maj_v]} already current (confirmed={confirmed_page}) | "
                f"HAND:{hand} WIN:{counts} MAJ:{maj_v}({maj_n}/{wlen})")
        elif decision == "cooldown":
            wait = cooldown - (now - trig.last_sent_t)
            log(f"COOLDOWN: hold {PAGE_CMD[maj_v]} | HAND:{hand} "
                f"WIN:{counts} MAJ:{maj_v}({maj_n}/{wlen}) wait {wait:.1f}s")
        else:
            log(f"HAND:{hand} FINGER:{fingers if lm else '-'} "
                f"WIN:{counts} MAJ:{maj_v}({maj_n}/{wlen}) -> no trigger")

        # ---- RLCD-004.2：ACK 超时自动重发（最多 MAX_ATTEMPTS 次）----
        if pending is not None:
            age = now - pending["sent_t"]
            if age >= ACK_TIMEOUT and pending["attempts"] < MAX_ATTEMPTS:
                if not args.dry_run:
                    if hub.cmd_ready():
                        try:
                            hub.send_cmd(pending["cmd"])
                            pending["sent_t"] = now
                            pending["attempts"] += 1
                            log(f"RESEND: {pending['cmd']} (attempt {pending['attempts']})")
                        except OSError as e:
                            log(f"resend failed: {e}")
                            hub.close_cmd_link()
                    # 链路不通则暂不重发，保留 pending 等链路恢复
                else:
                    pending["sent_t"] = now
                    pending["attempts"] += 1
                    log(f"RESEND(dry-run): {pending['cmd']} "
                        f"(attempt {pending['attempts']})")
            elif pending["attempts"] >= MAX_ATTEMPTS and age >= ACK_TIMEOUT:
                log(f"PAGE ACK TIMEOUT: {pending['cmd']} "
                    f"({MAX_ATTEMPTS} attempts, no ACK)")
                pending = None

        if args.save_dir and frames % args.save_every == 0:
            out = annotate(bgr, lm, hand, fingers if lm else "-", decision)
            cv2.imwrite(os.path.join(args.save_dir, f"f{frames:04d}.jpg"), out)
        if args.show:
            cv2.imshow("RLCD-004.1 finger control",
                       annotate(bgr, lm, hand, fingers if lm else "-", decision))
            if cv2.waitKey(1) & 0xFF == 27:
                break

        if args.max_frames and frames >= args.max_frames:
            break

    dt = time.time() - t0
    log(f"processed {frames} frames in {dt:.1f}s ({frames / dt if dt else 0:.2f}fps)")
    hub.close_cmd_link()
    if args.show:
        cv2.destroyAllWindows()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log("stopped")
        sys.exit(0)
