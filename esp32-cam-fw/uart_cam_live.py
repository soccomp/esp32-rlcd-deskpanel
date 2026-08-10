#!/usr/bin/env python3
"""ESP32-CAM 串口实时视频播放器（Mac 桌面窗口）
读 USB 串口(1M) 帧协议：AA55 5AA5 | len(2B LE) | JPEG | crc16
显示 640x480 实时画面。测试流阶段(前30s)无 JPEG 属正常，等摄像头进 JPEG 阶段自动出画面。
"""
import serial, time, io, threading
import tkinter as tk
from PIL import Image, ImageTk

PORT = "/dev/cu.usbserial-1120"
BAUD = 1000000

latest_jpeg = None
frame_count = 0
start_time = time.time()
status = "连接中..."

def reader_thread():
    global latest_jpeg, frame_count, status
    try:
        s = serial.Serial(PORT, BAUD, timeout=0.3)
        s.rts = False
        s.dtr = False
        time.sleep(1)
        s.reset_input_buffer()
        status = "已连接串口，等待摄像头出帧（测试流 30s 内无画面属正常）..."
        buf = bytearray()
        while True:
            chunk = s.read(65536)
            if not chunk:
                continue
            buf.extend(chunk)
            while True:
                idx = buf.find(b"\xaa\x55\x5a\xa5")
                if idx < 0:
                    if len(buf) > 6:
                        del buf[:-5]
                    break
                if idx > 0:
                    del buf[:idx]
                if len(buf) < 6:
                    break
                flen = (buf[4] << 8) | buf[5]
                need = 6 + flen + 2
                if flen <= 0 or flen > 100000:
                    del buf[:2]
                    continue
                if len(buf) < need:
                    break
                frame = bytes(buf[6:6 + flen])
                crc_r = (buf[6 + flen] << 8) | buf[6 + flen + 1]
                crc_c = flen & 0xFFFF
                for b in frame:
                    crc_c = (crc_c + b) & 0xFFFF
                del buf[:need]
                if crc_r == crc_c and frame[:2] == b"\xff\xd8" and frame[-2:] == b"\xff\xd9":
                    latest_jpeg = frame
                    frame_count += 1
    except Exception as e:
        status = f"串口错误: {e}"

threading.Thread(target=reader_thread, daemon=True).start()

root = tk.Tk()
root.title("ESP32-CAM 串口实时视频")
root.geometry("680x560")
root.configure(bg="#111")

img_label = tk.Label(root, bg="#111")
img_label.pack(padx=10, pady=5)

info = tk.Label(root, text="", fg="#4caf50", bg="#111", font=("Menlo", 11))
info.pack()

last_t = [time.time()]
last_n = [0]

def update():
    global latest_jpeg
    if latest_jpeg:
        try:
            im = Image.open(io.BytesIO(latest_jpeg))
            photo = ImageTk.PhotoImage(im)
            img_label.config(image=photo)
            img_label.image = photo
            # fps 统计
            now = time.time()
            if now - last_t[0] >= 2:
                fps = (frame_count - last_n[0]) / (now - last_t[0])
                last_t[0] = now
                last_n[0] = frame_count
                info.config(text=f"{im.size[0]}x{im.size[1]} · {fps:.1f} fps · 帧 {frame_count}")
        except Exception:
            pass
    else:
        info.config(text=status)
    root.after(40, update)

root.after(40, update)
root.mainloop()
