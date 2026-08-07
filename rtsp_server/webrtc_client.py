#!/usr/bin/env python3
"""webrtc_client.py — WebRTC 验证回路 (本机 offer 端)
用法: python3 webrtc_client.py [板子IP] [信令端口]
流程: 创建 offer → TCP 发板子 → 收 answer → 收 H.264 视频 8 秒 → 数帧
GREEN: 收到帧 > 0 (WebRTC 链路通); RED: 无帧 (链路断)
"""
import asyncio
import json
import socket
import sys
import time

from aiortc import RTCPeerConnection, RTCSessionDescription

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.50.132"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 12345
WATCH_SECS = 8


async def run():
    pc = RTCPeerConnection()
    frames = {"n": 0}

    @pc.on("track")
    def on_track(track):
        if track.kind != "video":
            return
        print(f"[client] 收到视频轨: {track.kind}")

        async def pump():
            while True:
                try:
                    await track.recv()
                    frames["n"] += 1
                except Exception:
                    break

        asyncio.ensure_future(pump())

    # 1. 创建 offer
    offer = await pc.createOffer()
    await pc.setLocalDescription(offer)
    print(f"[client] offer 已创建 ({len(offer.sdp)} 字节)")

    # 2. 信令: TCP 发 offer, 收 answer
    s = socket.create_connection((IP, PORT), timeout=10)
    s.sendall(json.dumps({"type": "offer", "sdp": offer.sdp}).encode() + b"\n")
    data = b""
    while b"\n" not in data:
        chunk = s.recv(16384)
        if not chunk:
            break
        data += chunk
    s.close()
    resp = json.loads(data.decode())
    print(f"[client] 收到 answer ({len(resp.get('sdp', ''))} 字节)")
    await pc.setRemoteDescription(
        RTCSessionDescription(resp["type"], resp["sdp"]))

    # 3. 数帧 WATCH_SECS 秒
    t0 = time.time()
    while time.time() - t0 < WATCH_SECS:
        await asyncio.sleep(0.5)
    fps = frames["n"] / WATCH_SECS
    print(f"[client] {WATCH_SECS} 秒收到 {frames['n']} 帧 ({fps:.1f} fps)")
    print("GREEN — WebRTC 链路通" if frames["n"] > 0 else "RED — 无帧到达")
    await pc.close()
    return frames["n"] > 0


if __name__ == "__main__":
    ok = asyncio.run(run())
    sys.exit(0 if ok else 1)
