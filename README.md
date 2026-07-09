# RV1126 RTSP 硬解码 MIPI 屏幕显示

> RTSP 网络摄像头拉流 → 硬件解码(H.264) → 格式转换(NV12→RGB) → MIPI LCD 实时显示
> 基于 GStreamer 管道，板端已验证通过。

## 快速开始

### 1. 交叉编译

```bash
# 编译 GStreamer 管线（默认，推荐）
./build.sh
```

工具链默认路径 `/opt/atk-dlrv1126b-toolchain`，产物在 `output/`。

### 2. 部署到板端

```bash
scp output/rv1126_gst_display config.ini root@<板端IP>:/root/
```

### 3. 板端运行

```bash
# 先停掉 Weston 桌面释放 /dev/fb0
killall weston 2>/dev/null
sleep 2

# 运行程序
./rv1126_gst_display config.ini
```

重启恢复桌面：`reboot`

## 配置文件

编辑 `config.ini`：

```ini
rtsp_url = rtsp://admin:password@192.168.50.54:554/stream1
rtsp_transport = tcp
log_level = info
```

## 数据流

```
RTSP摄像头 → rtspsrc → rtph264depay → h264parse → mppvideodec → videoconvert → fbdevsink → MIPI屏幕
                                                    (MPP硬解)        (RGA加速)
```

## 目录结构

```
├── main.c               # 主入口（GStreamer 管线，推荐）
├── main_native.c        # 备用入口（裸 MPP API，实验性）
├── mpp_dec.c/h          # MPP 硬解码封装
├── ffmpeg_demux.c/h     # FFmpeg RTSP 拉流
├── rga_convert.c/h      # RGA 硬件 NV12→RGB 转换
├── rga_info.h           # RGA ABI 定义（vendored）
├── fbdev.c/h            # fbdev 显示输出
├── config.c/h           # INI 配置解析
├── log.c/h              # 日志
├── build.sh             # 编译 GStreamer 管线
├── build_native.sh      # 编译裸 MPP 管线
├── config.ini           # 运行配置
├── docs/dev-log.md      # 详细开发日志（板端环境、排错等）
├── experimental/        # 未完成的实验模块
│   └── drm_display.c/h  # DRM/KMS 显示（RGA 未集成）
├── output/              # 编译产物
└── README.md
```

## 依赖

| 组件 | 用途 |
|------|------|
| GStreamer 1.24+ | 多媒体框架 |
| Rockchip MPP | H.264 硬件解码 |
| RGA (librga.so) | 硬件颜色转换 |
| fbdev (/dev/fb0) | 帧缓冲显示 |

## 更多信息

- 板端实测环境、方案对比、排错指南见 [docs/dev-log.md](docs/dev-log.md)
- 备用裸 MPP 管线编译：`./build_native.sh`
