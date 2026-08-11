#!/usr/bin/env python3
"""USB 全链路视频桥：ESP32-CAM 串口帧 -> M1 转发 -> RLCD USB-CDC。

链路（WiFi 被企业 AP RST 拦截时的替代视频通道）：
  ESP32-CAM (UART0, CH340, 1M) --USB扩展坞--> M1 本脚本 --> RLCD (USB-CDC 115200)
帧协议（两端一致）：AA 55 5A A5 | len(2B BE) | JPEG | crc16(2B BE, len+data 累加)

用法：python3 camusb_bridge.py [摄像头串口] [RLCD串口]
默认：/dev/cu.usbserial-1120 -> /dev/cu.usbmodem101
"""
import serial, sys, time

CAM_PORT  = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-1120"
RLCD_PORT = sys.argv[2] if len(sys.argv) > 2 else "/dev/cu.usbmodem101"
CAM_BAUD  = 1000000
RLCD_BAUD = 115200

HEAD = b"\xaa\x55\x5a\xa5"

cam = serial.Serial(CAM_PORT, CAM_BAUD, timeout=0.3)
cam.rts = False
cam.dtr = False
time.sleep(1.2)              # 打开 CH340 触发摄像头复位，等它启动
cam.reset_input_buffer()

rlcd = serial.Serial(RLCD_PORT, RLCD_BAUD, timeout=0.1)
time.sleep(0.3)
rlcd.reset_output_buffer()

buf = bytearray()
frames_ok = 0
frames_bad = 0
last_report = time.time()
print(f"[bridge] {CAM_PORT}@{CAM_BAUD} -> {RLCD_PORT}@{RLCD_BAUD}")

try:
    while True:
        chunk = cam.read(65536)
        if not chunk:
            continue
        buf.extend(chunk)

        while True:
            idx = buf.find(HEAD)
            if idx < 0:
                if len(buf) > 4:
                    del buf[:-3]
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

            if crc_c == crc_r:
                rlcd.write(HEAD + flen.to_bytes(2, "big") + frame + crc_r.to_bytes(2, "big"))
                rlcd.flush()
                frames_ok += 1
            else:
                frames_bad += 1

        now = time.time()
        if now - last_report >= 5:
            rate = frames_ok / (now - last_report)
            print(f"[bridge] ok={frames_ok} bad={frames_bad} ~{rate:.1f}fps", flush=True)
            frames_ok = 0
            frames_bad = 0
            last_report = now
except KeyboardInterrupt:
    print("\n[bridge] stopped")
finally:
    cam.close()
    rlcd.close()
