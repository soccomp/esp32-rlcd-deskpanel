#!/usr/bin/env python3
"""RLCD 键盘控制：用 Mac 键盘快捷键给 RLCD 发送切页命令。

原理：RLCD 有两个接收切页命令的通道：
  [USB 通道（默认）]  RLCD 的 USB-CDC 串口由 camusb_bridge 独占，其本地
      hub(127.0.0.1:8770) 接受白名单命令 PAGE:HOME/GUITAR/CAMERA（与手势切页
      同一通道），bridge 把命令写进 RLCD USB-CDC 实现切页。稳定可靠。
  [WiFi 通道（--wifi）] RLCD 固件内建的 WiFi TCP 命令服务(端口 8771)，
      直接连 RLCD 的 IP 发 PAGE 命令，带 ACK 回执。纯无线、不依赖桥接/USB，
      但受 WiFi 稳定性影响（办公室 AP 有周期性 deauth 楔死问题）。

本脚本用 pynput 监听全局键盘快捷键，命中即发送对应命令。

默认快捷键（可用 --bind 覆盖前 3 个）：
  Ctrl+1 -> PAGE:HOME      首页
  Ctrl+2 -> PAGE:GUITAR    吉他/环境页
  Ctrl+3 -> PAGE:CAMERA    摄像头页
  Ctrl+4 -> GTR:STRUM      拨弦（不在吉他页会自动跳过去再弹）
  Ctrl+5 -> GTR:CHORD      下一个和弦
  Ctrl+6 -> GTR:GROUP      切练习组 OPEN/7TH
  stdin 模式（--stdin）：1/2/3=切页，4/空格=拨弦，5/n=下一和弦，6/g=切组

用法：
  python3 rlcd_key_control.py                    # 走 USB hub（默认）
  python3 rlcd_key_control.py --wifi             # 走 WiFi 8771 直连
  python3 rlcd_key_control.py --wifi --rlcd-ip 192.168.100.197
  python3 rlcd_key_control.py --bind 1 2 3       # 用 Ctrl+1/2/3（默认）
  python3 rlcd_key_control.py --dry-run          # 只打印不发命令

依赖：pynput（pip install pynput；macOS 首次运行需在 系统设置-隐私与安全性
-> 辅助功能 中给 Python/终端 授权）。
"""
import argparse
import socket
import sys
import time

from pynput import keyboard

HUB_HOST, HUB_PORT = "127.0.0.1", 8770   # camusb_bridge 本地命令 hub
RLCD_IP, RLCD_PORT = "192.168.100.197", 8771  # RLCD 固件 WiFi 命令服务
RECONNECT_DELAY = 3.0                    # 连不上时的重连间隔
ALLOWED = {"PAGE:HOME", "PAGE:GUITAR", "PAGE:CAMERA"}


def log(msg):
    print(f"[key {time.strftime('%H:%M:%S')}] {msg}", flush=True)


class RlcdLink:
    """到命令接收端的持久 TCP 连接；断开自动重连。

    mode="usb"  -> 连本地 hub（127.0.0.1:8770，桥接转发到 RLCD USB-CDC）
    mode="wifi" -> 直连 RLCD 固件 WiFi 命令服务（<ip>:8771）
    """
    def __init__(self, mode="usb", rlcd_ip=RLCD_IP):
        self.mode = mode
        self.rlcd_ip = rlcd_ip
        self.sock = None

    def _target(self):
        if self.mode == "wifi":
            return (self.rlcd_ip, RLCD_PORT)
        return (HUB_HOST, HUB_PORT)

    def _name(self):
        return "RLCD WiFi" if self.mode == "wifi" else "RLCD cmd hub"

    def ensure(self):
        if self.sock is not None:
            return True
        try:
            s = socket.create_connection(self._target(), timeout=5)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.sock = s
            log(f"connected to {self._name()} {self._target()[0]}:{self._target()[1]}")
            return True
        except OSError as e:
            self.sock = None
            log(f"{self._name()} connect failed: {e} -- retry in {RECONNECT_DELAY:.0f}s")
            return False

    def send(self, cmd):
        if not self.ensure():
            return False
        try:
            self.sock.sendall(cmd.encode("ascii") + b"\n")
            return True
        except OSError as e:
            log(f"send failed: {e}; reconnecting")
            self.close()
            return False

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None


class KeyController:
    """把快捷键映射到 PAGE 命令并发送。"""
    def __init__(self, link, bind, dry_run=False, modifier="ctrl", min_interval=0.6):
        self.link = link
        self.dry = dry_run
        self.min_interval = min_interval   # 最小切页间隔（秒），防快速连按
        self.last_sent = 0.0               # 上次发送命令的时间戳
        # 当前按住的修饰键：Ctrl / Cmd / Alt / Shift
        self.mod = set()
        # 所需修饰键与冲突修饰键：Ctrl 组合时按住 Cmd 不算命中，反之亦然
        self.need = {"ctrl"} if modifier == "ctrl" else {"cmd"}
        self.block = {"cmd"} if modifier == "ctrl" else {"ctrl"}
        # bind: {触发键(单字符或'1'..): PAGE命令}
        self.bind = {str(k).lower(): v for k, v in bind.items()}

    def _combo_ok(self):
        # 严格匹配：按住的修饰键须包含所需键，且不含冲突键
        return self.need <= self.mod and not (self.block & self.mod)

    def handle_char(self, ch):
        """处理一个触发键字符 -> 发送对应 PAGE 命令（与输入来源无关）。"""
        if not ch or ch not in self.bind:
            return
        cmd = self.bind[ch]
        # 反射屏全刷耗时（~0.5~1s），快速连按会触发固件高频全量布局
        # -> 栈溢出崩溃。做最小间隔防抖，间隔内的重复按键忽略。
        now = time.time()
        if now - self.last_sent < self.min_interval:
            log(f"THROTTLE: {cmd} (interval {now - self.last_sent:.2f}s < {self.min_interval:.1f}s)")
            return
        self.last_sent = now
        if self.dry:
            log(f"DRY-RUN: {cmd}")
        elif self.link.send(cmd):
            log(f"SEND: {cmd}")

    def on_press(self, key):
        try:
            if key == keyboard.Key.ctrl_l or key == keyboard.Key.ctrl_r:
                self.mod.add("ctrl")
            elif key == keyboard.Key.cmd or key == keyboard.Key.cmd_r:
                self.mod.add("cmd")
            elif isinstance(key, keyboard.KeyCode) and self._combo_ok():
                self.handle_char(key.char)
        except Exception as e:
            log(f"on_press error: {e}")

    def on_release(self, key):
        if key == keyboard.Key.ctrl_l or key == keyboard.Key.ctrl_r:
            self.mod.discard("ctrl")
        elif key == keyboard.Key.cmd or key == keyboard.Key.cmd_r:
            self.mod.discard("cmd")
        elif key == keyboard.Key.esc:
            return False                     # Esc 退出


def stdin_loop(ctrl):
    """9-4 备用模式：从终端标准输入读键，**不需要 macOS 辅助功能授权**。

    pynput 的全局监听依赖 CGEventTap，必须在 系统设置-隐私与安全性-辅助功能
    里给对应 Python 解释器授权；launchd 拉起的后台进程常常静默拿不到授权
    （Listener 能 start、"listening..." 也打了，但一个按键事件都收不到），
    表现为"按了没反应"。本模式退化为「终端窗口持有焦点时按键生效」：
    按 1/2/3 切页，q 或 Ctrl+C 退出。牺牲全局性，换取零授权、立即可用。
    """
    import termios
    import tty

    fd = sys.stdin.fileno()
    try:
        old = termios.tcgetattr(fd)
    except termios.error as e:
        log(f"--stdin 必须在真实终端里运行（不能在管道/后台进程中）：{e}")
        return
    log("stdin mode: 1/2/3=切页  4=拨弦 5=下一个和弦 6=切练习组"
        "（空格=拨弦 n=下一个和弦 g=切组），q 退出（本窗口需保持焦点）")
    try:
        tty.setraw(fd)
        while True:
            ch = sys.stdin.read(1)
            if not ch or ch in ("q", "Q", "\x03"):     # q / Ctrl+C
                break
            if ch == "\x1b":                            # 忽略方向键等转义序列首字节
                continue
            ctrl.handle_char(ch)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", nargs="+", default=["1", "2", "3"],
                    help="快捷键数字（默认 1 2 3，即 Ctrl+1/2/3）")
    ap.add_argument("--dry-run", action="store_true", help="只打印不发命令")
    ap.add_argument("--wifi", action="store_true",
                    help="走 WiFi 通道直连 RLCD（8771），默认走 USB hub")
    ap.add_argument("--stdin", action="store_true",
                    help="备用模式：从终端标准输入读键（无需辅助功能授权，"
                         "但需终端窗口保持焦点）；全局监听授权不通时用这个")
    ap.add_argument("--rlcd-ip", default=RLCD_IP,
                    help=f"RLCD 的 WiFi IP（默认 {RLCD_IP}，配合 --wifi）")
    args = ap.parse_args()

    # 键位映射（9-4 扩展）：
    #   1/2/3        切页（--bind 可覆盖前 3 个）
    #   4/5/6        吉他控制：拨弦 / 下一个和弦 / 切练习组（=设备右键短按/左键短按/右键长按）
    #   空格 / n / g  吉他控制的 stdin 模式等价键（全局模式下裸键不满足 Ctrl 组合，
    #                永不触发，不会往文档里"打空格弹琴"）
    pages = ["PAGE:HOME", "PAGE:GUITAR", "PAGE:CAMERA"]
    bind = {}
    for i, k in enumerate(args.bind[:3]):
        bind[k] = pages[i]
    bind.setdefault("4", "GTR:STRUM")
    bind.setdefault("5", "GTR:CHORD")
    bind.setdefault("6", "GTR:GROUP")
    bind.setdefault(" ", "GTR:STRUM")
    bind.setdefault("n", "GTR:CHORD")
    bind.setdefault("g", "GTR:GROUP")

    mode = "WiFi" if args.wifi else "USB"

    def klabel(ch):
        return "Space" if ch == " " else f"Ctrl+{ch}"

    log("bindings: " + ", ".join(f"{klabel(k)}->{v}" for k, v in bind.items())
        + f" [{mode} 通道]" + (" (dry-run)" if args.dry_run else ""))

    link = RlcdLink(mode="wifi" if args.wifi else "usb", rlcd_ip=args.rlcd_ip)
    if not args.dry_run:
        link.ensure()
    ctrl = KeyController(link, bind, dry_run=args.dry_run)

    if args.stdin:
        stdin_loop(ctrl)
        log("stopped")
        return

    # macOS 全局监听：首次需在 系统设置-隐私与安全性-辅助功能 授权
    with keyboard.Listener(on_press=ctrl.on_press, on_release=ctrl.on_release) as listener:
        log("listening... (Esc 退出)")
        listener.join()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log("stopped")
        sys.exit(0)
