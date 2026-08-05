#!/usr/bin/env python3
"""measure_streams.py — 三码流帧率反馈回路 (诊断用)
用法: python3 measure_streams.py [ip] [target0 target1 target2]
拉三路流各 5 秒, 统计 RTP 时间戳数 = 实际帧率,
对比目标帧率, 低于目标输出 RED, 全部达标输出 GREEN。
"""
import socket, struct, sys, time

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.50.132"
TARGETS = [15, 10] if len(sys.argv) < 4 else [int(x) for x in sys.argv[2:4]]
PATHS = ["stream0", "stream1"]

def measure(path):
    s = socket.create_connection((IP, 8554), timeout=5)
    def req(m, cseq, extra=""):
        r = f"{m} rtsp://{IP}:8554/{path} RTSP/1.0\r\nCSeq: {cseq}\r\n"
        if extra: r += extra + "\r\n"
        return (r + "\r\n").encode()
    def sr(r):
        s.sendall(r)
        return s.recv(8192)
    sr(req("OPTIONS", 1)); sr(req("DESCRIBE", 2, "Accept: application/sdp"))
    sr(req("SETUP", 3, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1"))
    sr(req("PLAY", 4))
    s.settimeout(1)
    ts = set(); t0 = time.time(); buf = b""
    while time.time() - t0 < 5:
        try: d = s.recv(65536)
        except socket.timeout: continue
        if not d: break
        buf += d
        while len(buf) >= 4 and buf[0] == 0x24:
            ln = (buf[2] << 8) | buf[3]
            if len(buf) < 4 + ln: break
            pkt = buf[4:4+ln]; buf = buf[4+ln:]
            if len(pkt) >= 12:
                ts.add(struct.unpack(">I", pkt[4:8])[0])
    s.close()
    return len(ts) / 5.0

red = False
for path, tgt in zip(PATHS, TARGETS):
    fps = measure(path)
    ok = fps >= tgt * 0.9   # 90% 容差
    red = red or not ok
    print(f"  /{path}: {fps:5.1f} fps (目标 {tgt}) {'✓' if ok else '✗ RED'}")
print("GREEN — 三路达标" if not red else "RED — 有码流未达标")
sys.exit(1 if red else 0)
