# 部署与操作手册

> RV1126B RTSP 硬解码显示 + Web 管理后台 — 板端部署/维护文档
> 最后更新: 2026-08-03

## 1. 架构总览

```
摄像头 (ONVIF/RTSP)
   │
   ▼
rtsp_display (GStreamer + RGA 硬解硬转, 板子屏幕显示)
   │
rv1126_web (Web 管理后台: 发现/连接/预览, 端口 8080)
   │
浏览器 (http://板子IP:8080)
```

**两个进程**：
- `rtsp_display` — 显示管线（MPP 硬解 + RGA 硬转 + fbdev）
- `rv1126_web` — Web 后台（ONVIF 发现 + HLS 预览 + 配置管理）

## 2. 文件位置

### 开发机（本仓库）

| 路径 | 说明 |
|------|------|
| `main.c` + `fbdev.c` + `rga_convert.c` | 显示管线源码 |
| `src/web/web_main.c` | Web 后台主程序 |
| `src/web/onvif_disco.c` | ONVIF 组播发现 |
| `src/web/onvif_soap.c` | SOAP GetProfiles/GetStreamUri |
| `src/web/static/` | 前端页面 (index.html, hls.min.js) |
| `src/web/S99camera` | 开机自启脚本 |
| `build.sh` | 编译脚本 (产物在 output/) |

### 板端（/root）

| 路径 | 说明 |
|------|------|
| `/root/rtsp_display` | 显示管线二进制 |
| `/root/rv1126_web` | Web 后台二进制 |
| `/root/config.ini` | 显示管线配置 (rtsp_url 等) |
| `/root/last_connect.json` | 记忆的上次连接 (自动恢复用) |
| `/root/camera-web/static/` | 前端页面 (index.html, hls.min.js) |
| `/etc/init.d/S99camera` | **开机自启脚本 (必须在此位置)** |
| `/root/hls/` | HLS 预览分片 (ffmpeg/gst 输出, 自动清理) |

## 3. 编译与部署

```bash
# 开发机: 编译
./build.sh
# 产物: output/rtsp_display, output/rv1126_web

# 部署到板子
scp output/rtsp_display output/rv1126_web root@<板子IP>:/root/
scp -r src/web/static/* root@<板子IP>:/root/camera-web/static/

# 安装开机自启 (只做一次)
scp src/web/S99camera root@<板子IP>:/etc/init.d/
ssh root@<板子IP> "chmod +x /etc/init.d/S99camera"
```

## 4. 开机自启 (S99camera)

**位置**: `/etc/init.d/S99camera`（必须放这里才会开机执行）

功能：
1. 杀 weston（释放 /dev/fb0）
2. **打开 MIPI 屏背光**（weston 退出会关背光，`echo 0 > /sys/class/backlight/backlight/bl_power`）
3. 启动 `rv1126_web`（自动恢复上次连接，读 `last_connect.json`）
4. 看门狗：每 15 秒检查，进程挂了自动拉起

手动操作：
```bash
/etc/init.d/S99camera start    # 手动启动
/etc/init.d/S99camera stop     # 停止
```

## 5. 配置说明

### config.ini（显示管线用）

```ini
[network]
rtsp_url = rtsp://admin:password@192.168.1.100:554/stream0   # 摄像头地址 (凭据单独存 creds 文件)
rtsp_transport = tcp
[log]
log_level = info
```

### last_connect.json（Web 自动恢复用）

Web 每次连接成功自动写入。**改这个文件 = 改开机自动连接的摄像头**。

## 6. Web 后台使用

浏览器打开 `http://<板子IP>:8080`：

| 功能 | 说明 |
|------|------|
| 扫描发现 | ONVIF 自动发现局域网摄像头 (~10 秒) |
| 连接 | 点主码流/子码流, 板子屏幕立即显示 |
| 凭据 | 每台设备可填账号/密码 |
| 预览 | 浏览器实时预览 (HLS, ~3-5 秒延迟) |
| 状态 | 管线运行状态 |

## 7. 常见问题排查

| 症状 | 原因 | 解决 |
|------|------|------|
| 屏幕黑屏但进程在跑 | weston 退出关了背光 | `echo 0 > /sys/class/backlight/backlight/bl_power` |
| 黑屏 + 视频没写屏 | 管线没连上摄像头 | 看 `/tmp/gst_web.log` |
| 管线断线 | 摄像头重启/断网 | 自动重连 (2s→30s 退避), 不用管 |
| 预览出错 | HLS 分片问题 | 前端自动重试; 检查 `/tmp/hls.log` |
| 子码流变形 | 704×576 需 4:3 显示 | 已内置 PAL SAR 校正, 无需操作 |
| 重启后不自动恢复 | last_connect.json 被删/损坏 | Web 重新连接一次 |
| 进程名匹配不到 | Linux 15 字符截断 | 已用短名 rtsp_display, 勿改回长名 |

## 8. 日志位置

| 日志 | 内容 |
|------|------|
| `/tmp/gst_web.log` | 显示管线日志 (帧数/错误) |
| `/tmp/web.log` | Web 后台日志 |
| `/tmp/hls.log` | HLS 转码日志 |
| `/var/log/rv1126_gst.log` | 管线持久日志 |
