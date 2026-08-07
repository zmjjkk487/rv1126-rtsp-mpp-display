#!/usr/bin/env python3
"""measure_mjpeg.py — MJPEG 低延迟预览反馈回路
拉 http://<ip>/preview 3 秒, 数 JPEG 帧数
GREEN: ≥3 帧 (预览链路通); RED: 无帧
"""
import socket
import sys
import time

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.50.132"

s = socket.create_connection((IP, 80), timeout=5)
s.sendall(b"GET /preview HTTP/1.1\r\nHost: x\r\n\r\n")
s.settimeout(1)
frames = 0
total = 0
t0 = time.time()
buf = b""
while time.time() - t0 < 3:
    try:
        d = s.recv(65536)
    except socket.timeout:
        continue
    if not d:
        break
    buf += d
    total = len(buf)
    frames = buf.count(b"Content-Length:")
print(f"3 秒收到 {total / 1024:.0f} KB, {frames} 个 JPEG 帧")
print("GREEN — MJPEG 预览通" if frames >= 3 else "RED — 无帧")
s.close()
sys.exit(0 if frames >= 3 else 1)
