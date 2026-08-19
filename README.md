# RV1126B IP Camera · 嵌入式网络摄像头完整原型

> ⚠️ **硬件要求：必须 RV1126B（64 位），RV1126 不兼容！**
>
> 本项目针对正点原子 ATK-DLRV1126B（RV1126B，四核 Cortex-A53 **64 位**）开发调试。
> **RV1126（四核 Cortex-A7，32 位）不能运行本项目**：仓库产物为 aarch64 二进制，
> 32 位系统无法执行；交叉工具链、NPU 模型（rv1126b 目标）、板载 GStreamer/MPP/RGA
> 库版本也完全不同。RV1126 已停产，官方替代型号为 RV1126B（引脚不兼容）/
> RV1126B-P（Pin2Pin 兼容），选购时认准 **B**。

基于正点原子 ATK-DLRV1126B 开发板，实现：
- **消费者**：RTSP 网络摄像头拉流 → MPP 硬件解码 → RGA 硬件颜色转换 → MIPI LCD 屏幕实时显示
- **生产者**：板载 IMX415 摄像头采集 → MPP 硬件编码 → RTSP 双码流推流（板子即一台标准 ONVIF 摄像头）
- **Web 管理后台**：ONVIF 自动发现 + 一键连接 + HLS 预览 + 登录认证

RTSP 服务器、RTP 分包、ONVIF 协议栈**全部从零手写**（不依赖 gSOAP / live555 / gst-rtsp-server）。

---

## 🎯 这个项目能让你学到什么

| 主题 | 在这里能看到 |
|------|-------------|
| 嵌入式 Linux | Buildroot 环境、交叉编译、fbdev 直写显示、nohup 部署 |
| GStreamer | 拉流/推流双管线、appsink/appsrc 回调、MPP 硬解硬编 |
| 硬件加速 | MPP (VPU) 硬解码+硬编码、RGA 2D 加速、ISP 双通道缩放 |
| RTSP/RTP 协议 | 从零手写 RTSP 状态机、RTP 分包 (FU-A)、SDP、TCP interleaved |
| ONVIF 协议 | WS-Discovery 发现 + SOAP 服务端（13+ 接口，严格 XML） |
| 系统编程 | 多线程/锁/原子变量、非阻塞 IO、慢客户端丢帧策略 |

**适合谁**：想完整走一遍"嵌入式摄像头"全流程的人 —— 学生、转行者、刚入职的嵌入式工程师。

**为什么从零写协议**：用现成库 30 行就能起服务，但协议从此是黑盒；手写一遍，RTSP/ONVIF 从此是"一页纸 + 两端代码"。

---

## ✨ 特性

- 🖥️ **RTSP 拉流硬解显示**：rtspsrc → mppvideodec → RGA → fbdev，CPU < 15%，断线自动重连
- 📷 **板子即摄像头**：IMX415 → mpph264enc 硬件编码 → RTSP 双码流（主码流 2K / 子码流 1080p）
- 🔌 **自研 RTSP 服务器**：多挂载点、TCP interleaved、FU-A 分包、慢客户端整帧丢弃不花屏
- 🌐 **自研 ONVIF 应答端**：WS-Discovery 组播发现 + SOAP 服务 13+ 接口，通过 ODM 严格 XML 校验
- 🎛️ **Web 管理平台**（Rust/axum）：ONVIF 自动发现摄像头、一键连接上屏、浏览器 HLS/MJPEG 预览、登录认证
- 🎥 **云台控制 (ONVIF PTZ)**：ContinuousMove 方向控制 + SetPreset/GotoPreset 预置位管理；
  本机无云台 → 屏幕箭头/预置位标识可视化验证，真实云台换回调实现即可
- 🤖 **NPU 人形检测**：板载 NPU (rknpu) 跑 yolov8n，挂在 producer 采集帧上
  （每 3 帧推理一次），检测到人实时打印；推理与 MPP 编解码完全独立
- 🛡️ **自愈守护**：producer 崩溃看门狗 + 显示帧看门狗（解码器卡死自动重启），开机自启
- 📊 **真机验证**：全部功能在 ATK-DLRV1126B 实板跑通，含帧率测量脚本（measure_streams.py）

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
rv1126_web (Web 管理后台, 端口 8090)
    ▲
浏览器 (http://板子IP:8090)

--- 生产者 (板子即摄像头) ---
IMX415 (MIPI) → ISP (mainpath/selfpath 双通道)
    ├─ 2688x1520 → mpph264enc → RTSP /stream0  (主码流 2K@15)
    └─ 1920x1080 → mpph264enc → RTSP /stream1  (子码流 1080p@10)
    └─ ONVIF: WS-Discovery 发现 + SOAP :80/onvif/device_service
```

**三个进程**：
| 进程 | 功能 | 技术 |
|------|------|------|
| `rtsp_display` | 显示管线 | GStreamer + MPP 硬解 + RGA 硬转 + fbdev |
| `rv1126_web` | Web 后台 | Rust (axum) + ONVIF + HLS |
| `producer` | 摄像头生产者 (采集+编码+RTSP+ONVIF) | 纯 C + GStreamer (rtsp_server/) |

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

## 摄像头生产者 (rtsp_server/)

板子把自己变成一台标准 ONVIF 摄像头，供 web 摄像头管理/ODM/任意播放器拉流。

### 编译与部署

```bash
cd rtsp_server
# 交叉编译 (工具链 /opt/atk-dlrv1126b-toolchain)
make CC=/opt/atk-dlrv1126b-toolchain/bin/aarch64-buildroot-linux-gnu-gcc \
     CFLAGS="--sysroot=<工具链sysroot> -O2 -std=c11 -Wall -Wextra" \
     GST_CFLAGS="-I<SYS>/usr/include/gstreamer-1.0 -I<SYS>/usr/include/glib-2.0 -I<SYS>/usr/lib/glib-2.0/include" \
     GST_LIBS="--sysroot=<SYS> -L<SYS>/usr/lib -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lgmodule-2.0 -lgstapp-1.0 -lgstvideo-1.0 -lm -ldl" \
     producer

# 板端部署运行
scp producer root@<板子IP>:/root/
ssh root@<板子IP> "nohup /root/producer 8554 > /tmp/producer.log 2>&1 &"
```

### 拉流地址 (海康风格)

| 码流 | 地址 | 分辨率/帧率 | 码率 |
|------|------|------------|------|
| 主码流 | `rtsp://<板子IP>:8554/stream0` | 2688x1520 @15 | 5Mbps |
| 子码流 | `rtsp://<板子IP>:8554/stream1` | 1920x1080 @10 | 2Mbps |

- 传输: TCP interleaved; 播放器: VLC(`--rtsp-tcp`)/ffplay(`-rtsp_transport tcp`)/gst 均可
- ONVIF: WS-Discovery 组播发现 + `http://<板子IP>/onvif/device_service` (GetProfiles/GetStreamUri 等 13+ 接口, 严格 XML)
- 本机验证工具: `rtsp_server/file_source` (文件喂流) + `rtsp_server/measure_streams.py` (帧率回路)

### 已知限制 (RV1126 芯片级)

- **MPP 多会话调度不保证公平**: 双码流实测多数时间达标, 偶发一路短暂掉到 1-2fps (大厂低端芯片同样受制, 故普遍采用双码流而非三码流)
- IMX415 定焦镜头, 无自动对焦 (模糊需物理调焦或换模组)
- ONVIF HTTP 占用 :80 — SDK 自带 nginx 开机自启会抢占该端口, 本仓库已禁用 (`S50nginx` 改名 `.disabled`)
- 板载 wlan0 与 eth0 同网段会造成组播回包来源漂移 (ODM 类工具可能搜不到) — 产品上 WiFi 应换独立网段

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
├── docs/
│   ├── DEPLOY.md            # 部署与操作手册
│   └── dev-log.md           # 开发历史记录
│
└── lessons/                 # ONVIF 学习笔记 (HTML)
```

---

## 快速开始

> 注: Web 后台实际端口为 **8090** (旧文档 8080 已过时)

### 0. 摄像头生产者 (板子即摄像头)

```bash
# 板端 (已部署 /root/producer 后)
ssh root@<板子IP> "nohup /root/producer 8554 > /tmp/producer.log 2>&1 &"
# 浏览器打开 http://<板子IP>:8090 → 摄像头管理 → 搜索 → 出现板子自己 → 连接显示
```

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
| **web 搜到设备但码流列表消失** | nginx 开机自启抢占了 :80 (ONVIF HTTP 端口) | `pkill nginx` + 重启 producer; 根治: 已禁用 S50nginx 自启 |
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
