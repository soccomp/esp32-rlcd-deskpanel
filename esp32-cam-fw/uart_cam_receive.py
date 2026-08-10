#!/usr/bin/env python3
"""串口直传 JPEG 接收器：解析 ESP32-CAM 经 USB-TTL(1M 波特率) 发送的 JPEG 帧。
帧协议：0xAA 0x55 | len(2B LE) | JPEG data | crc16(2B, len+data 累加)
功能：统计帧率/吞吐 + 每 N 帧保存一张 JPEG 到本地（验证画面）。
用法：python3 uart_cam_receive.py <串口> [保存目录]
"""
import serial, sys, time, os

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-1120"
SAVE_DIR = sys.argv[2] if len(sys.argv) > 2 else "/tmp/camtut/uart_frames"
BAUD = 1000000

os.makedirs(SAVE_DIR, exist_ok=True)
s = serial.Serial(PORT, BAUD, timeout=0.3)
s.rts = False
s.dtr = False
time.sleep(1)
s.reset_input_buffer()

buf = bytearray()
frames_ok = 0
frames_bad = 0
bytes_total = 0
sizes = []
t0 = time.time()
last_save = 0
last_report = t0

print(f"[recv] {PORT} @ {BAUD} baud -> {SAVE_DIR}")

try:
    while True:
        chunk = s.read(65536)
        if not chunk:
            continue
        buf.extend(chunk)
        bytes_total += len(chunk)

        while True:
            idx = buf.find(b"\xaa\x55\x5a\xa5")
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
            if crc_r == crc_c and frame[:2] == b"\xff\xd8":
                frames_ok += 1
                sizes.append(flen)
                # 每 10 帧存一张
                if frames_ok - last_save >= 10:
                    last_save = frames_ok
                    fn = os.path.join(SAVE_DIR, f"frame_{frames_ok:06d}.jpg")
                    with open(fn, "wb") as f:
                        f.write(frame)
            else:
                frames_bad += 1

        if time.time() - last_report >= 2:
            el = time.time() - t0
            print(f"  [{el:.0f}s] 帧 OK={frames_ok} BAD={frames_bad} "
                  f"吞吐={bytes_total/el/1024:.1f}KB/s"
                  + (f" 帧大小~{sum(sizes)//len(sizes)}B" if sizes else ""),
                  flush=True)
            last_report = time.time()

except KeyboardInterrupt:
    pass
finally:
    s.close()
    el = time.time() - t0
    print(f"\n=== 结果 ===")
    print(f"  时长 {el:.1f}s  有效帧 {frames_ok}  坏帧 {frames_bad}")
    if frames_ok:
        print(f"  平均帧率 {frames_ok/el:.1f} fps")
        print(f"  平均帧大小 {sum(sizes)//len(sizes)} B")
    print(f"  总吞吐 {bytes_total/el/1024:.1f} KB/s")
    print(f"  已保存帧到 {SAVE_DIR}")
