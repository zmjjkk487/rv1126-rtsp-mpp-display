# RV1126 RTSP 网络摄像头 硬解码 MIPI 屏幕显示

> 正点原子 ATK-DLRV1126B 开发板  
> RTSP 网络摄像头拉流 → 硬件解码(H.264) → 格式转换(NV12→RGB) → MIPI LCD 实时显示  
> **最终方案: GStreamer 管道 (已验证通过)**

---

## 目录

1. [项目概述](#1-项目概述)
2. [平台实测环境](#2-平台实测环境)
3. [方案选择与数据流](#3-方案选择与数据流)
4. [方案一: GStreamer 管道 (推荐, 已验证)](#4-方案一-gstreamer-管道-推荐-已验证)
5. [方案二: FFmpeg C API + 裸 MPP API (开发中)](#5-方案二-ffmpeg-c-api--裸-mpp-api-开发中)
6. [开发板验证流程](#6-开发板验证流程)
7. [编译与部署](#7-编译与部署)
8. [配置文件](#8-配置文件)
9. [文件结构](#9-文件结构)
10. [常见问题排查](#10-常见问题排查)
11. [参考资源](#11-参考资源)

---

## 1. 项目概述

### 1.1 项目目标

实现一条完整的实时视频显示管道: RTSP 网络摄像头 → 网线 → RV1126B → 硬件解码(H.264) → 颜色空间转换(NV12→RGB) → 5.5寸 MIPI DSI 屏幕实时显示。

整个管道的核心处理（解码 + 颜色转换 + 显示）全部由硬件完成（MPP + RGA + DRM），CPU 占用极低。

### 1.2 硬件清单

| 硬件 | 型号/规格 |
|------|----------|
| 开发板 | 正点原子 ATK-DLRV1126B (RV1126B, 2GB DDR4L, 8GB eMMC) |
| 屏幕 | 5.5寸 720P MIPI DSI 屏幕 (720×1280 竖屏) |
| 摄像头 | RTSP 网络摄像头 (海康威视/大华/其他品牌均可) |
| 连接 | 网线直连或通过交换机连接 |

---

## 2. 平台实测环境

以下所有数据均在板端实际测量验证:

### 2.1 系统状态

| 组件 | 版本/状态 | 验证命令 |
|------|----------|---------|
| 系统 | Buildroot Linux | `cat /proc/version` |
| 内核 | Linux 4.19 (正点原子) | `uname -a` |
| GStreamer | **1.24.13** | `gst-launch-1.0 --version` |
| GCC | **无** (板端无编译器) | `which gcc` |
| 交叉工具链 | `/opt/atk-dlrv1126b-toolchain` | — |
| 显示服务器 | **Weston** (Wayland) | `ps aux \| grep weston` |
| Qt 桌面 | **systemui + controlcenter** | `ps aux \| grep systemui` |

### 2.2 多媒体库

| 组件 | 路径/版本 | 用途 |
|------|----------|------|
| MPP 库 | `/usr/lib/librockchip_mpp.so.0` | H.264 硬件解码 |
| MPP 版本 | `alientek 2026-03-02` (正点原子定制) | — |
| RGA 库 | `/usr/lib/librga.so.2.1.0` (API v1.10.5) | 硬件 RGB 转换/缩放 |
| RGA 设备 | `/dev/rga` (字符设备) | 硬件 2D 加速 |
| DRM 库 | `/usr/lib/libdrm.so.2.124.0` | DRM/KMS 显示 |
| FFmpeg | `/usr/lib/libavformat.so.58.76.100` | RTSP 拉流/拆帧 |
| rkmedia | **不存在** | 官方多媒体框架不可用 |

### 2.3 显示设备

| 项目 | 值 |
|------|-----|
| DRM 设备 | `/dev/dri/card0` |
| 连接器 | `card0-DSI-1` (MIPI DSI) |
| 连接状态 | **connected** |
| 分辨率 | **720×1280** (5.5寸竖屏) |
| fbdev 设备 | `/dev/fb0` |

### 2.4 摄像头参数 (实际测量)

| 参数 | 值 |
|------|-----|
| 分辨率 | **704×576** (PAL D1) |
| 编码 | **H.264 High Profile** |
| 帧率 | **15 fps** |
| 码流 | `tbr 20, tbn 90k, tbc 30` |
| RTSP URL | `rtsp://admin:password@192.168.50.54:554/stream1` |

---

## 3. 方案选择与数据流

### 3.1 方案对比

本项目存在两套代码方案，实际验证结果:

| 方案 | 状态 | 说明 |
|------|------|------|
| **GStreamer 管道** | ✅ **已验证通过** | 命令行和 C 程序均能正常显示画面 |
| FFmpeg C + 裸 MPP API | ❌ 开发中止 | 正点原子 MPP 定制版不支持反复 create/destroy，段错误 |

**最终采用 GStreamer 方案。**

### 3.2 数据流

```
RTSP 网络摄像头 (704×576 H.264)
        │ TCP (RTSP/RTP 协议)
        ▼
 rtspsrc (GStreamer RTSP 拉流)
        │  动态 pad 创建
        ▼
 rtph264depay (RTP 解包 → H.264 裸码流)
        │
        ▼
 h264parse (H.264 码流解析)
        │
        ▼
 mppvideodec (Rockchip MPP 硬件 H.264 解码器)
        │  内部调用 librockchip_mpp.so
        │  输出: NV12 YUV 帧
        ▼
 videoconvert (颜色空间转换)
        │  内部调用 librga.so (RGA 硬件加速)
        │  NV12 → RGB888 (可选用 RGB16)
        ▼
 fbdevsink (fbdev 帧缓冲输出)
        │  写入 /dev/fb0
        ▼
 5.5寸 MIPI DSI 屏幕 (720×1280)
```

### 3.3 各级延迟说明

| 环节 | 延迟 | 说明 |
|------|------|------|
| RTSP 网络传输 | ~30-100ms | 取决于网络状况 |
| RTP 缓冲 (latency=300) | 300ms | rtspsrc 内部 jitterbuffer |
| MPP 硬件解码 | ~1-5ms/帧 | 704×576 解码 |
| RGA 颜色转换 | <1ms | 硬件加速 |
| 总延迟 | ~300-500ms | 实时监控可接受 |

---

## 4. 方案一: GStreamer 管道 (推荐, 已验证)

### 4.1 概述

GStreamer 是 Rockchip 官方推荐的框架。正点原子 Buildroot 系统已预装 GStreamer 1.24.13 及以下插件:

- **mppvideodec**: Rockchip MPP 硬件解码插件
- **fbdevsink**: fbdev 显示输出插件
- **kmssink**: DRM/KMS 显示输出插件 (当前有 connector-id 兼容问题)
- **videoconvert**: 颜色空间转换 (内部调用 RGA 硬件加速)

### 4.2 命令行验证 (已验证通过)

以下命令已在板端验证，可直接使用:

```bash
# 1. 先停掉 Weston 桌面 (释放 /dev/fb0)
killall weston 2>/dev/null
sleep 2

# 2. 运行 GStreamer 管道
gst-launch-1.0 \
    rtspsrc location="rtsp://admin:password@192.168.50.54:554/stream1" \
            protocols=tcp latency=300 ! \
    rtph264depay ! \
    h264parse ! \
    mppvideodec ! \
    videoconvert ! \
    fbdevsink device=/dev/fb0

# 3. 看完后按 Ctrl+C 停止, 重启恢复桌面
reboot
```

**验证输出** (实际板端日志):
```
mpp: mpp version: 15bf88a author: alientek 2026-03-02
mpp: h264d_api: is_avcC=1
mpp: mpp_buf_slot: mismatch size_total 608256 - 811008
current rga_api version 1.10.5_[9]
0:00:01.5 / 99:99:99.     ← 实时帧率显示，畫面正常
```

### 4.3 各 GStreamer 插件详解

#### rtspsrc

| 属性 | 值 | 说明 |
|------|-----|------|
| location | RTSP URL | 摄像头地址 |
| protocols | tcp | 强制 TCP 传输，UDP 可能被防火墙拦截 |
| latency | 300 | jitterbuffer 缓冲时长 (ms) |
| drop-on-latency | TRUE | 超时丢帧避免累积延迟 |
| tcp-timeout | 20000000 | TCP 超时 (us) |

**说明**: rtspsrc 是 GStreamer Good Plugins 中的网络源插件，遵循 RFC 2326 RTSP 协议。它内部会实例化 rtpbin 管理 RTCP 和 jitterbuffer。默认先尝试 UDP，被防火墙挡后会回退 TCP。指定 `protocols=tcp` 可以跳过 UDP 尝试阶段。

**pad-added 信号**: rtspsrc 的 src pad 是动态创建的，需要连接 `pad-added` 信号来将动态 pad 链接到下游的 rtph264depay。每个 RTP 流会创建一个独立的 pad。

#### rtph264depay

从 RTP 包中提取 H.264 裸码流 (去除 RTP 头部)。输出是 H.264 Annex-B 格式码流 (含 `00 00 00 01` 起始码)。

#### h264parse

H.264 码流解析器，确保码流以正确的格式传递给解码器。当码流为 `avcC` 格式时 (如海康摄像头)，h264parse 会将其转换为 Annex-B 格式。

#### mppvideodec

Rockchip MPP 硬件解码器 GStreamer 插件。内部流程:

```
输入: H.264 Annex-B 码流
    ↓
mpp_create → mpp_init(MPP_CTX_DEC, MPP_VIDEO_CodingAVC)
    ↓
decode_put_packet (提交码流)
    ↓
decode_get_frame (获取解码帧 NV12)
    ↓
输出: NV12 YUV 帧
```

mppvideodec 内部实现了 info_change 处理、buffer group 管理、帧重排等逻辑，应用层无需关注。

#### videoconvert

颜色空间转换和图像缩放。在 RV1126 上`videoconvert`内部使用 librga 硬件加速。日志中可见:
```
current rga_api version 1.10.5_[9]
The called RockchipRga API is deprecated...
```
这说明 RGA 硬件正在工作中。这个 deprecation 警告来自 librga 自身，不影响功能。

#### fbdevsink

将视频帧写入 `/dev/fb0` 帧缓冲设备。需要确保没有其他进程 (如 Weston) 同时占用 `/dev/fb0`。

### 4.4 C 程序: main_gst.c

将上述命令行翻译为 C 代码，提供更好的错误处理和日志功能。

**编译 (交叉编译)**:
```bash
# 需要 GStreamer 头文件在 sysroot 中
# 从源码下载:
wget https://gstreamer.freedesktop.org/src/gstreamer/gstreamer-1.24.13.tar.xz
tar -xf gstreamer-1.24.13.tar.xz
sudo cp -r gstreamer-1.24.13/gst /opt/atk-dlrv1126b-toolchain/.../sysroot/usr/include/gstreamer-1.0/

# 编译
./build_gst.sh
```

**编译 (板端本地)**:
```bash
cd /root
gcc -o rv1126_gst main_gst.c config.c log.c \
    -I/usr/include/gstreamer-1.0 \
    -I/usr/include/glib-2.0 \
    -I/usr/lib/glib-2.0/include \
    -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 \
    -lm
```

**运行**:
```bash
# 先停 Weston
killall weston 2>/dev/null
sleep 2
# 运行
./rv1126_gst_display
```

### 4.5 Shell 脚本包装 (最简方案)

```bash
#!/bin/sh
# 文件: /root/rv1126_gst.sh
killall weston 2>/dev/null
sleep 2
gst-launch-1.0 \
    rtspsrc location="rtsp://admin:password@192.168.50.54:554/stream1" \
            protocols=tcp latency=300 ! \
    rtph264depay ! h264parse ! mppvideodec ! videoconvert ! \
    fbdevsink device=/dev/fb0
```

```bash
chmod +x /root/rv1126_gst.sh
./rv1126_gst.sh
```

---

## 5. 方案二: FFmpeg C API + 裸 MPP API (开发中)

### 5.1 架构

```
avformat_open_input(RTSP) → av_read_frame → H.264 Annex-B 帧
    ↓
mpp_packet_set_data → decode_put_packet → decode_get_frame → NV12
    ↓
RGA c_RkRgaBlit / CPU nv12_to_rgb888 → RGB888
    ↓
fbdev mmap(/dev/fb0) → 显存
```

### 5.2 开发状态

| 模块 | 文件 | 状态 | 问题 |
|------|------|------|------|
| FFmpeg 拉流 | ffmpeg_demux.c | ✅ 可用 | 已验证 |
| MPP 解码 | mpp_dec.c | ❌ 段错误 | 正点原子 MPP 不支持反复 create/destroy |
| RGA 转换 | rga_convert.c | ❌ 未验证 | 结构体来自官方，运行时未测试 |
| fbdev 显示 | fbdev.c | ⚠️ 部分 | RGA 集成代码未验证 |
| DRM 显示 | drm_display.c | ❌ 头文件缺失 | 需要 libdrm 头文件 |

### 5.3 已发现的问题

1. **MPP_SET_INPUT_TIMEOUT 段错误**: 正点原子定制 MPP (alientek 2026-03-02) 上调用 `MPP_SET_INPUT_TIMEOUT` 会导致段错误
2. **MPP 反复 create/destroy 崩溃**: mpp_create/mpp_destroy 循环调用超过 2 次后段错误
3. **DRM 头文件缺失**: Buildroot 系统精简了 /usr/include

---

## 6. 开发板验证流程

### 6.1 系统准备

```bash
# 查看系统信息
cat /proc/version
uname -a

# 查看 MPP 版本
dmesg | grep mpp

# 查看 GStreamer 版本
gst-launch-1.0 --version

# 查看 DRM 显示状态
cat /sys/class/drm/card0/card0-DSI-1/status
cat /sys/class/drm/card0/card0-DSI-1/modes

# 查看 RGA 设备
ls -la /dev/rga
strings /usr/lib/librga.so | grep "rga_api version"

# 查看 MPP 库
ls -la /usr/lib/librockchip_mpp*
```

### 6.2 网络配置

```bash
# 配置开发板 IP
ifconfig eth0 192.168.50.xxx

# 确认与摄像头连通
ping 192.168.50.54

# 查看摄像头 RTSP 地址 (PC 端)
ffprobe rtsp://admin:password@192.168.50.54:554/stream1
```

### 6.3 验证 MPP 解码

```bash
# 使用 MPP 官方测试工具解码本地文件
mpi_dec_test -i /userdata/test.h264 -t 7 -v q
```

### 6.4 验证 GStreamer 管道

```bash
# 1. 停 Weston (释放 fb0)
killall weston 2>/dev/null
sleep 2

# 2. 运行 GStreamer (看到摄像头画面即成功)
gst-launch-1.0 \
    rtspsrc location="rtsp://admin:password@192.168.50.54:554/stream1" \
            protocols=tcp latency=300 ! \
    rtph264depay ! h264parse ! \
    mppvideodec ! videoconvert ! \
    fbdevsink device=/dev/fb0

# 3. 按 Ctrl+C 停止, 重启恢复桌面
reboot
```

### 6.5 编译与部署 C 程序

```bash
# 编译机 (Ubuntu)
cd ~/rv1126_rtsp_mpp_demo
./build_gst.sh

# 部署到板子
scp output_gst/rv1126_gst_display config.ini root@192.168.50.132:/root/

# 板端运行
killall weston 2>/dev/null
sleep 2
./rv1126_gst_display
```

---

## 7. 编译与部署

### 7.1 方案一: GStreamer C 程序 (build_gst.sh)

**交叉编译依赖**:

| 依赖 | 来源 | 说明 |
|------|------|------|
| GStreamer 头文件 | gstreamer-1.24.13 源码包 | `/usr/include/gstreamer-1.0/` |
| GLib 头文件 | glib-2.80.0 源码包 | `/usr/include/glib-2.0/` |
| glibconfig.h | Buildroot sysroot 自带 | `/usr/lib/glib-2.0/include/` |
| libgstreamer-1.0.so | Buildroot sysroot 自带 | 链接需要 |
| libglib-2.0.so | Buildroot sysroot 自带 | 链接需要 |
| libgobject-2.0.so | Buildroot sysroot 自带 | 链接需要 |

```bash
chmod +x build_gst.sh
./build_gst.sh
```

### 7.2 方案二: FFmpeg + 裸 MPP (build.sh)

**依赖**: MPP 头文件 + FFmpeg 头文件 + libdrm 头文件 (可选)

```bash
chmod +x build.sh
./build.sh
```

---

## 8. 配置文件

`config.ini`:

```ini
[network]
# 摄像头 RTSP 地址 (704×576 H.264 High, 15fps)
rtsp_url = rtsp://192.168.50.54:554/stream1?username=admin&password=E10ADC3949BA59ABBE56E057F20F883E
# 传输协议: tcp (可靠) 或 udp (低延迟)
rtsp_transport = tcp

[display]
# 显示设备路径
#   fbdev: /dev/fb0
#   DRM:   /dev/dri/card0
fb_device = /dev/fb0
# 显示类型: fbdev (传统帧缓冲) 或 drm (DRM/KMS)
display_type = fbdev
# 屏幕分辨率 (正点原子 5.5寸 MIPI DSI 屏幕: 720x1280)
display_width = 720
display_height = 1280
# 目标帧率 (摄像头 15fps, 0=不限速)
target_fps = 0

[log]
# 日志级别: debug, info, warn, error
log_level = info
```

---

## 9. 文件结构

```
rv1126_rtsp_mpp_demo/
│
├── main_gst.c          # [推荐] GStreamer 管道 C 实现 (已验证)
├── build_gst.sh        # [推荐] GStreamer 交叉编译脚本
│
├── main.c              # FFmpeg + 裸 MPP 方案主入口 (开发中)
├── build.sh            # FFmpeg + 裸 MPP 交叉编译脚本
│
├── ffmpeg_demux.c/h    # FFmpeg C API RTSP 解复用器
├── mpp_dec.c/h         # MPP H.264 硬解码封装 (split_parse + info_change)
├── rga_info.h          # Vendored RGA 结构体 (来自 airockchip/librga)
├── rga_convert.c/h     # RGA 硬件 NV12→RGB 转换 (dlopen c_RkRgaBlit)
├── fbdev.c/h           # Framebuffer 显示 (集成 RGA 回退 CPU)
├── drm_display.c/h     # DRM/KMS 显示 (dumb buffer, 可选, 头文件依赖)
│
├── config.c/h          # INI 配置文件解析
├── config.ini          # 运行配置 (RTSP URL, 分辨率等)
├── log.c/h             # 四级日志 (DEBUG/INFO/WARN/ERROR)
│
├── ffmpeg_pipe.c/h     # [废弃] fork+pipe 方案
├── output/             # 编译输出 (交叉编译)
├── output_gst/         # GStreamer 编译输出
│
└── README.md           # 本文档
```

---

## 10. 常见问题排查

### 10.1 fbdevsink: Device or resource busy

**原因**: Weston (Wayland 合成器) 占用 /dev/fb0
**解决**:
```bash
killall weston 2>/dev/null
sleep 2
# 然后重新运行
```

### 10.2 fbdevsink: not-negotiated

**原因**: pipes 格式不匹配，或加了错误的 caps 限制
**解决**: 不要手动指定 caps，让 videoconvert 自动协商

### 10.3 kmssink: could not set property connector-id

**原因**: kmssink 期望数字 ID 而非字符串
**解决**: 使用 fbdevsink 替代

### 10.4 GStreamer: Error sending UDP packets

**原因**: rtspsrc 默认先尝试 UDP (被防火墙拦截)
**解决**: 添加 `protocols=tcp`

### 10.5 gst-launch-1.0: command not found

**原因**: GStreamer 未安装或 PATH 未设置
**解决**: 确认板端系统是否支持: `which gst-launch-1.0`

### 10.6 mpp: unable to create enc vp8 for soc rv1126b unsupported

**原因**: RV1126 不支持 VP8 编码 (这是 mpp 初始化时的探测日志，不影响解码)
**解决**: **忽略，这是正常日志**。RV1126 仅支持 H.264/H.265 编解码

### 10.7 rga_api deprecation warning

**原因**: 当前 librga 使用旧版 C API，新版本推荐 IM2D API
**解决**: **忽略，不影响功能**

### 10.8 Qt 界面占用显示的问题

```bash
# 查看占用 fb0 的进程
fuser /dev/fb0 2>/dev/null

# 停掉 Weston 桌面
killall weston 2>/dev/null
killall weston-desktop-shell 2>/dev/null
# 或直接杀 systemui
killall systemui 2>/dev/null

# 正常重启恢复
reboot
```

### 10.9 SSH 连接不上

板端 IP 可能因为 DHCP 变化。使用串口连接查看:
```bash
ifconfig eth0 | grep "inet "
```

---

## 11. 参考资源

### 11.1 官方文档

| 资源 | 链接 |
|------|------|
| GStreamer rtspsrc 文档 | [gstreamer.freedesktop.org](https://gstreamer.freedesktop.org/documentation/rtsp/rtspsrc.html) |
| GStreamer 基础教程 | [Basic tutorials](https://gstreamer.freedesktop.org/documentation/tutorials/basic/index.html) |
| Rockchip MPP 官方仓库 | [rockchip-linux/mpp](https://github.com/rockchip-linux/mpp) |
| MPP 官方解码 demo | [mpi_dec_test.c](https://github.com/rockchip-linux/mpp/blob/develop/test/mpi_dec_test.c) |
| MPP 开发文档 (CN) | [Rockchip_Developer_Guide_MPP_CN.md](https://github.com/rockchip-linux/mpp/blob/develop/doc/Rockchip_Developer_Guide_MPP_CN.md) |
| RGA 开源库 | [airockchip/librga](https://github.com/airockchip/librga) |
| 正点原子 MPP 测试手册 | [alientek.yuque.com](https://alientek.yuque.com/nfzuim/tsl6qb/fryhgi8mqtpl2gzo) |
| 正点原子音视频手册 | [alientek.yuque.com](https://alientek.yuque.com/nfzuim/tsl6qb/zzzvd1yqqgenc4if) |

### 11.2 相关社区项目

| 项目 | 链接 |
|------|------|
| atk_rkmedia_rv1126_yolov5_rtsp | [Leonoek/atk_rkmedia_rv1126_yolov5_rtsp](https://github.com/Leonoek/atk_rkmedia_rv1126_yolov5_rtsp) |
| EASY-EAI-Toolkit-C-Solution | [EASY-EAI/EASY-EAI-Toolkit-C-Solution](https://github.com/EASY-EAI/EASY-EAI-Toolkit-C-Solution) |

---

## 附录 A: GStreamer 调试技巧

```bash
# 查看所有视频 sink
gst-inspect-1.0 | grep -i sink | grep -i video

# 查看 mppvideodec 详细信息
gst-inspect-1.0 mppvideodec

# 查看 fbdevsink 详细信息
gst-inspect-1.0 fbdevsink

# 查看 kmssink 详细信息
gst-inspect-1.0 kmssink

# 启用 GStreamer 详细日志
GST_DEBUG=*:5 ./rv1126_gst_display
# 或
GST_DEBUG=mppvideodec:5 ./rv1126_gst_display

# 查看管道拓扑
GST_DEBUG_BIN_TO_DOT_FILE=1 ./rv1126_gst_display
```

## 附录 B: 现场验证记录

时间: 2026-07-07

| 验证项 | 结果 |
|--------|------|
| GStreamer 命令行 (fbdevsink) | ✅ 画面正常显示 |
| main_gst.c 编译 (交叉编译) | ✅ 编译通过 |
| main_gst.c 运行 (板端) | ✅ 画面显示 (有 pad 链接警告, 不影响) |
| FFmpeg + 裸 MPP | ❌ 段错误 (正点原子 MPP 定制版兼容问题) |
| RGA 硬件加速 | ✅ librga.so 存在, API v1.10.5 |
| DRM 显示 | ⚠️ kmssink 有 connector-id 兼容问题 |
| 摄像头分辨率 | 704×576 H.264 High 15fps |
