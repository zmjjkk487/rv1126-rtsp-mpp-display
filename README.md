# RV1126B 嵌入式 RTSP 硬解码显示系统 + Web 管理平台

基于正点原子 ATK-DLRV1126B 开发板，实现 RTSP 网络摄像头拉流 → MPP 硬件解码 → RGA 硬件颜色转换 → MIPI LCD 屏幕实时显示，
以及嵌入式 Web 管理后台（ONVIF 自动发现 + 一键连接 + HLS 预览 + 登录认证）。

---

## 架构总览

```
摄像头 (ONVIF/RTSP)
    │  RTSP/RTP over TCP
    ▼
rtsp_display (GStreamer + RGA 硬解硬转, 板子屏幕显示)
    ▲
    │ config.ini  /  last_connect.json
    │
rv1126_web (Web 管理后台, 端口 8080)
    ▲
浏览器 (http://板子IP:8080)
```

**两个进程**：
| 进程 | 功能 | 技术 |
|------|------|------|
| `rtsp_display` | 显示管线 | GStreamer + MPP 硬解 + RGA 硬转 + fbdev |
| `rv1126_web` | Web 后台 | Rust (axum) + ONVIF + HLS |

### 数据流

```
rtspsrc → rtph264depay → h264parse → mppvideodec → appsink(NV12 dmabuf)
    → RGA (NV12→BGRX + 等比缩放 + 黑边) → /dev/fb0 → MIPI LCD

RTSP 握手 (DESCRIBE→SETUP→PLAY)    缓冲 300ms     H.264 码流
    ↓                                           ↓
rtph264depay (RTP 拼包)        mppvideodec (MPP 硬件解码, <1ms/帧)
    ↓                                           ↓
h264parse (avcC→Annex-B)       RGA (NV12→RGB + 缩放, <1ms)
                                fbdev (mmap 直写显存, <2ms)
```

**延迟构成**: 总约 300-500ms（瓶颈在 jitterbuffer 300ms）

**硬件加速**: MPP (VPU 硬解码) + RGA (2D 加速器颜色转换) → CPU < 15% @ 1080p 25fps

---

## 硬件

| 硬件 | 型号/规格 |
|------|----------|
| 开发板 | 正点原子 ATK-DLRV1126B (RV1126B, 2GB DDR4L, 8GB eMMC) |
| 屏幕 | 5.5寸 720×1280 MIPI DSI |
| 摄像头 | 任何 RTSP 网络摄像头 (H.264) |
| 系统 | Buildroot Linux, BusyBox init |
| GStreamer | 1.24.13 (预装 mppvideodec, fbdevsink, kmssink) |
| MPP | `librockchip_mpp.so` (正点原子定制版) |
| RGA | `librga.so` (API v1.10.5), 设备 `/dev/rga` |

---

## 目录结构

```
rv1126_rtsp_mpp_demo/
├── main.c                  # GStreamer 显示管线入口 (断线重连, SAR 校正)
├── fbdev.c / fbdev.h       # fbdev 显示 (RGA 硬件优先, CPU 回退)
├── rga_convert.c / .h      # RGA 硬件加速封装 (dlopen, NV12→BGRX)
├── rga_info.h              # RGA ABI 兼容头 (vendor)
├── config.c / config.h     # INI 配置解析
├── log.c / log.h           # 四级日志
├── config.ini.sample       # 配置模板 (复制为 config.ini 使用)
├── build.sh                # 交叉编译脚本 (两个二进制)
│
├── src/web/
│   ├── web_rust/            # Web 管理后台 (Rust + axum)
│   │   └── src/main.rs       # 全部功能: HTTP/认证/ONVIF/HLS
│   ├── S99camera            # 开机自启脚本
│   └── static/
│       ├── index.html       # Web 管理界面 (扫描/连接/预览/状态)
│       └── hls.min.js       # HLS.js 播放器
│
├── legacy_web/              # 废弃的 C 版 Web 后台 (参考用)
│
├── docs/
│   ├── DEPLOY.md            # 部署与操作手册
│   └── dev-log.md           # 开发历史记录
│
└── lessons/                 # ONVIF 学习笔记 (HTML)
```

---

## 快速开始

### 1. 编译

```bash
# 开发机 (需要交叉工具链 /opt/atk-dlrv1126b-toolchain)
./build.sh
# 产物: output/rtsp_display, output/rv1126_web
```

### 2. 配置

```bash
cp config.ini.sample config.ini
# 编辑 config.ini, 修改 rtsp_url 为你的摄像头地址
```

```ini
[network]
rtsp_url = rtsp://admin:password@192.168.1.100:554/stream0
rtsp_transport = tcp

[log]
log_level = info
```

### 3. 部署

```bash
# 部署二进制
scp output/rtsp_display output/rv1126_web config.ini root@<板子IP>:/root/

# 部署 Web 前端
ssh root@<板子IP> "mkdir -p /root/camera-web/static"
scp src/web/static/* root@<板子IP>:/root/camera-web/static/

# 安装开机自启 (只需一次)
scp src/web/S99camera root@<板子IP>:/etc/init.d/
ssh root@<板子IP> "chmod +x /etc/init.d/S99camera"
```

### 4. 运行

```bash
# 板端手动启动
ssh root@<板子IP> "/etc/init.d/S99camera start"

# 浏览器打开
http://<板子IP>:8080
```

---

## Web 管理后台

| 功能 | 说明 |
|------|------|
| 扫描发现 | ONVIF 自动发现局域网摄像头 (~10 秒) |
| 连接 | 点主码流/子码流, 板子屏幕立即显示 |
| 凭据 | 每台设备可填账号/密码 |
| 预览 | 浏览器实时预览 (HLS, ~3-5 秒延迟) |
| 状态 | 管线运行状态 |

**默认登录**: 用户名 `admin`, 密码 `admin123`

---

## ONVIF 协议栈

自研实现（不依赖 gSOAP/ONVIF SDK）：

```
① WS-Discovery (UDP 组播, 239.255.255.250:3702)
   Probe 探测 → 摄像头回复 XAddrs (服务地址)

② SOAP/HTTP (TCP 80)
   GetProfiles → 获取码流配置 (主码流/子码流)
   GetStreamUri → 获取 RTSP URL

③ IP 探测回退 (海康等品牌不响应组播时自动探测常见 IP)
```

---

## 开机自启

`/etc/init.d/S99camera` (BusyBox init 自动执行)：

1. 杀 weston 释放 `/dev/fb0`
2. 打开 MIPI 屏背光（weston 退出会关背光）
3. 启动 `rv1126_web`（自动恢复上次连接）
4. 看门狗：每 15 秒检查，进程挂了自动拉起

手动操作：
```bash
/etc/init.d/S99camera start    # 启动
/etc/init.d/S99camera stop     # 停止
```

---

## 板端文件布局

| 路径 | 说明 |
|------|------|
| `/root/rtsp_display` | 显示管线二进制 |
| `/root/rv1126_web` | Web 后台二进制 |
| `/root/config.ini` | 显示管线配置 |
| `/root/last_connect.json` | 上次连接记录 (自动恢复用) |
| `/root/camera-web/static/` | Web 前端页面 |
| `/etc/init.d/S99camera` | 开机自启脚本 |
| `/root/hls/` | HLS 分片输出 (自动清理) |

---

## 常见问题

| 症状 | 原因 | 解决 |
|------|------|------|
| 屏幕黑屏但进程在跑 | weston 退出关了背光 | `echo 0 > /sys/class/backlight/backlight/bl_power` |
| 黑屏 + 视频没写屏 | 管线没连上摄像头 | 看 `/tmp/gst_web.log` |
| 管线断线 | 摄像头重启/断网 | 自动重连 (2s→30s 退避), 不用管 |
| 子码流变形 | 704×576 需 4:3 显示 | 已内置 PAL SAR 校正, 无需操作 |
| 重启后不自动恢复 | last_connect.json 损坏 | Web 重新连接一次 |
| HLS 预览报错 | 分片问题 | 前端自动重试; 检查 `/tmp/hls.log` |

---

## 日志位置

| 日志 | 内容 |
|------|------|
| `/tmp/gst_web.log` | 显示管线 (帧数/错误) |
| `/tmp/web.log` | Web 后台 |
| `/tmp/hls.log` | HLS 转码 |
| `/var/log/rv1126_gst.log` | 管线持久日志 |
