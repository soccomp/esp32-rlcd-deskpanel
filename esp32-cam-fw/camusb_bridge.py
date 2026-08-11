#!/usr/bin/env python3
"""USB 全链路视频桥：ESP32-CAM 串口帧 -> M1 转发 -> RLCD USB-CDC。

链路（WiFi 被企业 AP RST 拦截时的替代视频通道）：
  ESP32-CAM (UART0, CH340, 1M) --USB扩展坞--> M1 本脚本 --> RLCD (USB-CDC 115200)
帧协议（两端一致）：AA 55 5A A5 | len(2B BE) | JPEG | crc16(2B BE, len+data 累加)

RLCD-004 新增「本地 hub」（127.0.0.1:8770）：两个串口只能被一个进程独占，
因此手势识别程序不直接开串口，而是接本 hub：
  hub -> 客户端：每帧原样下发（同一套帧协议，客户端可复用解析器）
  客户端 -> hub：ASCII 命令行（白名单 PAGE:HOME|PAGE:MEETING|PAGE:GUITAR），
                 由本脚本写入 RLCD USB-CDC，与视频帧共用同一条通道。
视频转发逻辑、帧协议、CRC 均未改动；无客户端连接时行为与改造前完全一致。

用法：
  python3 camusb_bridge.py                      # 两端口都自动发现（launchd 常驻用这个）
  python3 camusb_bridge.py <摄像头串口> <RLCD串口>   # 手动指定
  python3 camusb_bridge.py auto auto            # 同全自动

自动发现依据 USB VID:PID，不依赖会变化的端口号：
  摄像头 CH340    = 1A86:7523  (回退 glob /dev/cu.usbserial*)
  RLCD ESP32-S3   = 303A:xxxx  (回退 glob /dev/cu.usbmodem*)

常驻行为：设备未插、被拔出、IO 异常时不退出，清理后等待重试，
插回即自动恢复转发。配合 launchd KeepAlive 使用（见 docs/OPERATIONS.md）。
"""
import glob
import queue
import socket
import sys
import threading
import time

import serial
from serial.tools import list_ports

CAM_BAUD = 1000000
RLCD_BAUD = 115200
HEAD = b"\xaa\x55\x5a\xa5"

CH340_VID, CH340_PID = 0x1A86, 0x7523   # 摄像头 USB-TTL
ESPRESSIF_VID = 0x303A                  # ESP32-S3 原生 USB-CDC

RETRY_WAIT = 3.0        # 端口缺失/异常后的重试间隔（秒）
REPORT_EVERY = 5.0      # 帧率日志间隔（秒）

HUB_HOST, HUB_PORT = "127.0.0.1", 8770  # 本地帧分流 / 命令回注
ALLOWED_CMDS = ("PAGE:HOME", "PAGE:MEETING", "PAGE:GUITAR")


def log(msg):
    print(f"[bridge {time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _cu(dev):
    """macOS 上统一用 /dev/cu.*（callout），避免 /dev/tty.* 打开时阻塞等待 DCD。"""
    return dev.replace("/dev/tty.", "/dev/cu.") if dev.startswith("/dev/tty.") else dev


def find_cam_port():
    for p in list_ports.comports():
        if p.vid == CH340_VID and p.pid == CH340_PID:
            return _cu(p.device)
    hits = sorted(glob.glob("/dev/cu.usbserial*"))
    return hits[0] if hits else None


def find_rlcd_port():
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID:
            return _cu(p.device)
    hits = sorted(glob.glob("/dev/cu.usbmodem*"))
    return hits[0] if hits else None


def resolve_ports(cam_arg, rlcd_arg):
    cam = cam_arg if cam_arg and cam_arg != "auto" else find_cam_port()
    rlcd = rlcd_arg if rlcd_arg and rlcd_arg != "auto" else find_rlcd_port()
    return cam, rlcd


class FrameHub:
    """本地 TCP hub：向订阅者广播帧，并收集订阅者回传的命令。

    每个客户端只保留「最新一帧」（满则丢旧），慢客户端不会拖慢转发主循环。
    命令走独立队列，由主循环统一写串口，避免多线程同时写 RLCD。
    """

    def __init__(self):
        self._clients = []                  # [(sock, deque-like Queue(1))]
        self._lock = threading.Lock()
        self.commands = queue.Queue()       # 主循环消费

    def start(self):
        t = threading.Thread(target=self._serve, daemon=True)
        t.start()

    def _serve(self):
        try:
            srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind((HUB_HOST, HUB_PORT))
            srv.listen(4)
        except OSError as e:
            log(f"hub disabled: {e}")        # 端口被占用不影响视频转发
            return
        log(f"hub listening on {HUB_HOST}:{HUB_PORT}")
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                return
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            q = queue.Queue(maxsize=1)
            with self._lock:
                self._clients.append((conn, q))
            log(f"hub client connected ({len(self._clients)} total)")
            threading.Thread(target=self._send_loop, args=(conn, q), daemon=True).start()
            threading.Thread(target=self._recv_loop, args=(conn, q), daemon=True).start()

    def _drop(self, conn, q):
        with self._lock:
            self._clients = [c for c in self._clients if c[1] is not q]
        try:
            conn.close()
        except OSError:
            pass

    def _send_loop(self, conn, q):
        while True:
            packet = q.get()
            if packet is None:
                return
            try:
                conn.sendall(packet)
            except OSError:
                self._drop(conn, q)
                log("hub client disconnected (send)")
                return

    def _recv_loop(self, conn, q):
        buf = b""
        while True:
            try:
                chunk = conn.recv(256)
            except OSError:
                chunk = b""
            if not chunk:
                self._drop(conn, q)
                q.put(None)
                log("hub client disconnected (recv)")
                return
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                cmd = line.strip().decode("ascii", "ignore").upper()
                if cmd in ALLOWED_CMDS:
                    self.commands.put(cmd)
                elif cmd:
                    log(f"hub rejected command: {cmd!r}")

    def publish(self, packet):
        with self._lock:
            clients = list(self._clients)
        for _, q in clients:
            if q.full():                    # 慢客户端只丢旧帧，不阻塞主循环
                try:
                    q.get_nowait()
                except queue.Empty:
                    pass
            try:
                q.put_nowait(packet)
            except queue.Full:
                pass


def pump(cam, rlcd, hub):
    """一次连接内的转发循环；串口异常时抛出，由外层重连。"""
    buf = bytearray()
    ok = bad = 0
    last_report = time.time()

    while True:
        chunk = cam.read(65536)
        if chunk:
            buf.extend(chunk)

        while True:
            idx = buf.find(HEAD)
            if idx < 0:
                if len(buf) > 4:
                    del buf[:-3]       # 只留可能被截断的帧头前缀
                break
            if idx > 0:
                del buf[:idx]
            if len(buf) < 6:
                break
            flen = (buf[4] << 8) | buf[5]
            if flen <= 0 or flen > 100000:
                del buf[:2]            # 伪帧头，跳过重新搜索
                continue
            need = 6 + flen + 2
            if len(buf) < need:
                break

            frame = bytes(buf[6:6 + flen])
            crc_r = (buf[6 + flen] << 8) | buf[6 + flen + 1]
            crc_c = flen & 0xFFFF
            for b in frame:
                crc_c = (crc_c + b) & 0xFFFF
            del buf[:need]

            if crc_c == crc_r:
                packet = HEAD + flen.to_bytes(2, "big") + frame + crc_r.to_bytes(2, "big")
                rlcd.write(packet)
                rlcd.flush()
                hub.publish(packet)
                ok += 1
            else:
                bad += 1

        # 订阅者回传的页面命令：与视频帧共用 USB-CDC，写在帧边界之间
        while True:
            try:
                cmd = hub.commands.get_nowait()
            except queue.Empty:
                break
            rlcd.write(cmd.encode("ascii") + b"\n")
            rlcd.flush()
            log(f"cmd -> RLCD: {cmd}")

        now = time.time()
        if now - last_report >= REPORT_EVERY:
            log(f"ok={ok} bad={bad} ~{ok / (now - last_report):.1f}fps")
            ok = bad = 0
            last_report = now


def main():
    cam_arg = sys.argv[1] if len(sys.argv) > 1 else None
    rlcd_arg = sys.argv[2] if len(sys.argv) > 2 else None
    waiting_logged = False

    hub = FrameHub()
    hub.start()

    while True:
        cam = rlcd = None
        try:
            cam_port, rlcd_port = resolve_ports(cam_arg, rlcd_arg)
            if not cam_port or not rlcd_port:
                if not waiting_logged:      # 只打一次，避免刷爆日志
                    log(f"waiting for devices (cam={cam_port or 'missing'} "
                        f"rlcd={rlcd_port or 'missing'})")
                    waiting_logged = True
                time.sleep(RETRY_WAIT)
                continue
            waiting_logged = False

            cam = serial.Serial(cam_port, CAM_BAUD, timeout=0.3)
            cam.rts = False
            cam.dtr = False
            time.sleep(1.2)                 # 打开 CH340 会触发摄像头复位，等它启动
            cam.reset_input_buffer()

            rlcd = serial.Serial(rlcd_port, RLCD_BAUD, timeout=0.1)
            time.sleep(0.3)
            rlcd.reset_output_buffer()

            log(f"{cam_port}@{CAM_BAUD} -> {rlcd_port}@{RLCD_BAUD}")
            pump(cam, rlcd, hub)

        except KeyboardInterrupt:
            log("stopped")
            return
        except (serial.SerialException, OSError) as e:
            log(f"link lost: {e} -- retry in {RETRY_WAIT:.0f}s")
            time.sleep(RETRY_WAIT)
        finally:
            for port in (cam, rlcd):
                try:
                    if port and port.is_open:
                        port.close()
                except Exception:
                    pass


if __name__ == "__main__":
    main()
