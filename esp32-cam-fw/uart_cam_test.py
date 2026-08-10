#!/usr/bin/env python3
"""串口直传带宽测试：解析 ESP32-CAM 经 USB-TTL 发送的 JPEG 帧。
帧协议：0xAA 0x55 | len(2B LE) | JPEG data | crc16(2B, len+data 累加)
用法：python3 uart_cam_test.py <串口> <波特率> <测试秒数>
"""
import serial, sys, time

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-1120"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 921600
DURATION = float(sys.argv[3]) if len(sys.argv) > 3 else 10

s = serial.Serial(PORT, BAUD, timeout=0.5)
s.reset_input_buffer()

buf = bytearray()
frames_ok = 0
frames_bad = 0
bytes_total = 0
t0 = time.time()
last_report = t0

print(f"[test] {PORT} @ {BAUD} baud, {DURATION}s")

while time.time() - t0 < DURATION:
    chunk = s.read(65536)
    if not chunk:
        continue
    buf.extend(chunk)
    bytes_total += len(chunk)

    # 状态机解析：找 AA55 头 -> 读长度 -> 读帧 -> CRC 校验
    while True:
        # 1. 找帧头
        idx = buf.find(b"\xaa\x55\x5a\xa5")
        if idx < 0:
            if len(buf) > 4:
                del buf[:-3]   # 保留末尾可能的分片头
            break
        if idx > 0:
            del buf[:idx]      # 丢弃噪声（日志等）
        if len(buf) < 6:
            break
        flen = (buf[4] << 8) | buf[5]
        need = 6 + flen + 2    # 头 + data + crc
        if flen <= 0 or flen > 100 * 1024:
            del buf[:2]        # 非法长度，头可能误判
            continue
        if len(buf) < need:
            break              # 帧未收全
        frame = bytes(buf[4:4+flen])
        crc_recv = (buf[4+flen] << 8) | buf[4+flen+1]
        crc_calc = flen & 0xFFFF
        for b in frame:
            crc_calc = (crc_calc + b) & 0xFFFF
        del buf[:need]
        if crc_recv == crc_calc and frame[:2] == b"\xff\xd8":
            frames_ok += 1
        else:
            frames_bad += 1

    # 每秒进度
    if time.time() - last_report >= 1:
        el = time.time() - t0
        print(f"  [{el:.0f}s] 帧 OK={frames_ok} BAD={frames_bad} "
              f"吞吐={bytes_total/el/1024:.1f}KB/s", flush=True)
        last_report = time.time()

el = time.time() - t0
print(f"\n=== 结果 ===")
print(f"  时长 {el:.1f}s  有效帧 {frames_ok}  坏帧 {frames_bad}")
print(f"  平均帧率 {frames_ok/el:.1f} fps")
print(f"  总吞吐 {bytes_total/el/1024:.1f} KB/s")
if frames_ok:
    print(f"  平均帧大小 {bytes_total//max(frames_ok,1)} B")
s.close()
