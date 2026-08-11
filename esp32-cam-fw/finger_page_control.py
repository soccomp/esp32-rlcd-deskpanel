#!/usr/bin/env python3
"""RLCD-004：手指数量 -> RLCD 页面切换（M1 侧视觉识别）。

闭环：ESP32-CAM 出图 --串口--> camusb_bridge 本地 hub --> 本程序识别手指数量
      --命令回注 hub--> bridge 写 RLCD USB-CDC --> RLCD 切页

职责分离（任务单要求）：视觉模型只跑在 M1，ESP32 侧只收 "PAGE:xxx" 简单命令。

映射：
  1 根手指 -> PAGE:HOME     首页
  2 根手指 -> PAGE:MEETING  会议页
  3 根手指 -> PAGE:GUITAR   吉他页

防抖（RLCD-004.1 改进）：用「滚动时间窗口 + 多数表决 + 冷却」替代原先的严格连续 N 帧。
      1~2fps 下，偶发漏检/误数会让"连续 5 帧一致"几乎无法满足；新策略在最近 win_sec
      （默认 3s）窗口内，只要 ≥min_agree（默认 3）帧认同同一手指数即触发，天然容忍
      窗口内的个别噪点帧；触发后清空窗口并进入 cooldown（默认 1.5s）冷却，避免快速跳页。
      同一页面不重复发送。

投递可靠性（RLCD-004.2 改进）：M1 发出 PAGE 命令后**不再**乐观地认为已切页。
      RLCD 收到命令即经 USB-CDC 回 ``ACK:PAGE:X``，桥接转发给本程序；只有收到对应
      ACK 才更新权威当前页 confirmed_page，"Already current" 仅基于 confirmed_page。
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
import urllib.request
from collections import deque

import cv2
import numpy as np

HUB_HOST, HUB_PORT = "127.0.0.1", 8770
HEAD = b"\xaa\x55\x5a\xa5"

PAGE_CMD = {1: "PAGE:HOME", 2: "PAGE:MEETING", 3: "PAGE:GUITAR"}

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


# ---------------------------------------------------------------- hub 客户端
class HubClient:
    """连接 camusb_bridge 的本地 hub：收帧 / 回传命令 / 收 RLCD 回执(ACK)。

    RLCD-004.2：hub 在同一 TCP 流里既发二进制 JPEG 帧，又发文本行 ``ACK:PAGE:X``。
    这里用一条后台 recv 线程做解复用——缓冲以帧头(HEAD)开头就当帧，否则按
    ``\\n`` 切出文本行、挑出 ACK 入 ack_queue——避免阻塞在主循环里漏收 ACK。
    """

    def __init__(self):
        self.sock = None
        self.buf = bytearray()
        self.frame_q = queue.Queue(maxsize=1)    # 只留最新一帧
        self.ack_queue = queue.Queue()           # RLCD 回执 ACK:PAGE:X
        self._recv = None
        self._closed = False

    def connect(self):
        s = socket.create_connection((HUB_HOST, HUB_PORT), timeout=5)
        s.settimeout(1.0)
        self.sock = s
        self.buf.clear()
        self._closed = False
        self._recv = threading.Thread(target=self._recv_loop, daemon=True)
        self._recv.start()
        log(f"connected to hub {HUB_HOST}:{HUB_PORT}")

    def close(self):
        self._closed = True
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass
        self.sock = None

    def send_cmd(self, cmd):
        self.sock.sendall(cmd.encode("ascii") + b"\n")

    def _recv_loop(self):
        while not self._closed:
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue                    # 无数据：继续等，绝不退出（曾误杀 recv 线程）
            except OSError:
                self._closed = True
                return
            if not chunk:
                self._closed = True
                return
            self.buf.extend(chunk)
            self._demux()

    def _demux(self):
        while True:
            if self.buf.startswith(HEAD) and len(self.buf) >= 8:
                flen = (self.buf[4] << 8) | self.buf[5]
                if 0 < flen <= 100000 and len(self.buf) >= 8 + flen:
                    frame = bytes(self.buf[6:6 + flen])
                    crc_r = (self.buf[6 + flen] << 8) | self.buf[7 + flen]
                    crc_c = flen & 0xFFFF
                    for b in frame:
                        crc_c = (crc_c + b) & 0xFFFF
                    del self.buf[:8 + flen]
                    if crc_c == crc_r:
                        try:
                            self.frame_q.put_nowait(frame)
                        except queue.Full:
                            try:
                                self.frame_q.get_nowait()   # 丢旧帧
                            except queue.Empty:
                                pass
                            try:
                                self.frame_q.put_nowait(frame)
                            except queue.Full:
                                pass
                    continue
                if flen <= 0 or flen > 100000:
                    del self.buf[:2]
                    continue
                break   # 帧未收全，等更多数据
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line = bytes(self.buf[:nl])
                del self.buf[:nl + 1]
                s = line.strip().decode("ascii", "ignore")
                if s.startswith("ACK:"):
                    self.ack_queue.put(s)
                continue
            break

    def read_frame(self, timeout=5):
        """阻塞取一帧 JPEG（来自帧队列，已校验 CRC）；超时/断链抛 ConnectionError。"""
        try:
            return self.frame_q.get(timeout=timeout)
        except queue.Empty:
            if self._closed:
                raise ConnectionError("hub closed")
            raise ConnectionError("frame timeout (hub stalled?)")


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

    def __init__(self, min_agree=3, win_sec=3.0, cooldown=1.5):
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
    ap.add_argument("--cooldown", type=float, default=1.5,
                    help="发送命令后冷却秒数，防快速跳页（默认 1.5）")
    ap.add_argument("--consec", type=int, default=None,
                    help="(兼容旧参数) 等同 --min-agree")
    ap.add_argument("--ack-timeout", type=float, default=1.2,
                    help="RLCD-004.2：发出 PAGE 后等待 ACK 的超时秒数，超时自动重发"
                         "（默认 1.2：实测链路含 RLCD 端 JPEG 解码，往返常达 0.6~1.5s，"
                         "0.9 偏紧；1.2 与任务建议 0.8~1.0 同量级且更稳）")
    ap.add_argument("--max-attempts", type=int, default=3,
                    help="RLCD-004.2：单条命令最多发送次数（含首次），超过未收到 ACK "
                         "则记 PAGE ACK TIMEOUT（默认 3）")
    ap.add_argument("--dry-run", action="store_true", help="只识别，不发命令")
    ap.add_argument("--show", action="store_true", help="打开 OpenCV 调试窗口")
    ap.add_argument("--save-dir", default=None, help="另存带标注的调试图到该目录")
    ap.add_argument("--save-every", type=int, default=1, help="每 N 帧存一张")
    ap.add_argument("--max-frames", type=int, default=0, help=">0 时处理够帧数即退出")
    args = ap.parse_args()

    if args.save_dir:
        os.makedirs(args.save_dir, exist_ok=True)

    det = HandDetector()
    # ---- 触发参数（RLCD-004.1：滚动窗口多数表决 + 冷却）----
    min_agree = args.min_agree if args.consec is None else args.consec
    if min_agree < 1:
        min_agree = 1
    win_sec = args.win_sec
    cooldown = args.cooldown

    hub = HubClient()
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
        try:
            if hub.sock is None:
                hub.connect()
            jpg = hub.read_frame()
        except (OSError, ConnectionError) as e:
            log(f"hub link lost: {e} -- retry in 3s")
            hub.close()
            time.sleep(3)
            continue

        bgr = cv2.imdecode(np.frombuffer(jpg, np.uint8), cv2.IMREAD_COLOR)
        if bgr is None:
            continue
        frames += 1
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
                    try:
                        hub.send_cmd(cmd)
                        pending = {"cmd": cmd, "sent_t": now, "attempts": 1}
                        log(f"SEND: {cmd} (attempt 1) | HAND:{hand} "
                            f"FINGER:{fingers if lm else '-'} "
                            f"WIN:{counts} MAJ:{maj_v}({maj_n}/{wlen})")
                    except OSError as e:
                        log(f"send failed: {e}")
                        hub.close()
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
                    try:
                        hub.send_cmd(pending["cmd"])
                        pending["sent_t"] = now
                        pending["attempts"] += 1
                        log(f"RESEND: {pending['cmd']} (attempt {pending['attempts']})")
                    except OSError as e:
                        log(f"resend failed: {e}")
                        hub.close()
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
    hub.close()
    if args.show:
        cv2.destroyAllWindows()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log("stopped")
        sys.exit(0)
