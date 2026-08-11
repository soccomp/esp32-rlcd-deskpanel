#!/usr/bin/env python3
"""RLCD-004：手指数量 -> RLCD 页面切换（M1 侧视觉识别）。

闭环：ESP32-CAM 出图 --串口--> camusb_bridge 本地 hub --> 本程序识别手指数量
      --命令回注 hub--> bridge 写 RLCD USB-CDC --> RLCD 切页

职责分离（任务单要求）：视觉模型只跑在 M1，ESP32 侧只收 "PAGE:xxx" 简单命令。

映射：
  1 根手指 -> PAGE:HOME     首页
  2 根手指 -> PAGE:MEETING  会议页
  3 根手指 -> PAGE:GUITAR   吉他页

防抖：连续 N 帧（默认 5）识别到同一手指数才发命令；同一页面不重复发送。
      摄像头约 2fps，5 帧 ≈ 2.5s，对应"手势保持 2 秒"的验收要求。

手指判定：只统计食指/中指/无名指/小指四指，忽略拇指——拇指自然外张
      极易把"2"读成"3"。伸直判据用「指尖到腕距 > 近节指关节到腕距」，
      不依赖手的朝向，比常见的 y 坐标比较更耐旋转。

用法：
  python3 finger_page_control.py                      # 常规运行（控制台调试输出）
  python3 finger_page_control.py --dry-run            # 只识别不发命令（Phase 1 验证）
  python3 finger_page_control.py --save-dir /tmp/x    # 另存带标注的调试图
  python3 finger_page_control.py --show               # 打开 OpenCV 实时调试窗口
"""
import argparse
import math
import os
import socket
import sys
import time
import urllib.request

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
    """连接 camusb_bridge 的本地 hub：收帧 / 回传命令。"""

    def __init__(self):
        self.sock = None
        self.buf = bytearray()

    def connect(self):
        s = socket.create_connection((HUB_HOST, HUB_PORT), timeout=5)
        s.settimeout(5)
        self.sock = s
        self.buf.clear()
        log(f"connected to hub {HUB_HOST}:{HUB_PORT}")

    def close(self):
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass
        self.sock = None

    def send_cmd(self, cmd):
        self.sock.sendall(cmd.encode("ascii") + b"\n")

    def read_frame(self):
        """阻塞读一帧 JPEG（已按帧协议解包并校验 CRC）。"""
        while True:
            idx = self.buf.find(HEAD)
            if idx >= 0 and len(self.buf) >= idx + 6:
                flen = (self.buf[idx + 4] << 8) | self.buf[idx + 5]
                if 0 < flen <= 100000 and len(self.buf) >= idx + 8 + flen:
                    frame = bytes(self.buf[idx + 6:idx + 6 + flen])
                    crc_r = (self.buf[idx + 6 + flen] << 8) | self.buf[idx + 7 + flen]
                    crc_c = flen & 0xFFFF
                    for b in frame:
                        crc_c = (crc_c + b) & 0xFFFF
                    del self.buf[:idx + 8 + flen]
                    if crc_c == crc_r:
                        return frame
                    continue
                if flen <= 0 or flen > 100000:
                    del self.buf[:idx + 2]
                    continue
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("hub closed")
            self.buf.extend(chunk)


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--consec", type=int, default=5,
                    help="连续多少帧识别一致才发命令（默认 5）")
    ap.add_argument("--dry-run", action="store_true", help="只识别，不发命令")
    ap.add_argument("--show", action="store_true", help="打开 OpenCV 调试窗口")
    ap.add_argument("--save-dir", default=None, help="另存带标注的调试图到该目录")
    ap.add_argument("--save-every", type=int, default=1, help="每 N 帧存一张")
    ap.add_argument("--max-frames", type=int, default=0, help=">0 时处理够帧数即退出")
    args = ap.parse_args()

    if args.save_dir:
        os.makedirs(args.save_dir, exist_ok=True)

    det = HandDetector()
    log(f"mediapipe backend = {det.mode}, consec={args.consec}, dry_run={args.dry_run}")

    hub = HubClient()
    streak_val, streak_n = -1, 0
    last_sent = None
    frames = 0
    t0 = time.time()

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

        lm = det.detect(bgr)
        fingers = count_fingers(lm) if lm else 0
        hand = "YES" if lm else "NO"
        cmd_text = "-"

        # 防抖：只有 1/2/3 参与计数，其它（无手/0/4 指）打断连续性
        if lm and fingers in PAGE_CMD:
            if fingers == streak_val:
                streak_n += 1
            else:
                streak_val, streak_n = fingers, 1
        else:
            streak_val, streak_n = -1, 0

        if streak_n == args.consec:
            cmd = PAGE_CMD[streak_val]
            if cmd == last_sent:
                cmd_text = f"{cmd} (skip, already there)"
            else:
                cmd_text = cmd
                if args.dry_run:
                    cmd_text += " (dry-run)"
                else:
                    try:
                        hub.send_cmd(cmd)
                        last_sent = cmd
                    except OSError as e:
                        log(f"send failed: {e}")
                        hub.close()
            log(f"HAND: {hand}  FINGER: {streak_val}  CMD: {cmd_text}")
        else:
            log(f"HAND: {hand}  FINGER: {fingers if lm else '-'}  "
                f"streak={streak_n}/{args.consec}")

        if args.save_dir and frames % args.save_every == 0:
            out = annotate(bgr, lm, hand, fingers if lm else "-", cmd_text)
            cv2.imwrite(os.path.join(args.save_dir, f"f{frames:04d}.jpg"), out)
        if args.show:
            cv2.imshow("RLCD-004 finger control",
                       annotate(bgr, lm, hand, fingers if lm else "-", cmd_text))
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
