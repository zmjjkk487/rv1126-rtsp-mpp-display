# 学习资源

## 核心一手来源

### MPP 官方
- **Rockchip MPP 仓库**: https://github.com/rockchip-linux/mpp
- **MPP 开发者指南 (中文)**: `doc/Rockchip_Developer_Guide_MPP_CN.md` (在仓库内)
- **官方 demo (解码)**: `test/mpi_dec_test.c`
- **头文件**: `inc/rk_mpi.h`, `inc/rk_mpi_cmd.h`, `inc/mpp_frame.h`

### RGA
- **librga 仓库**: https://github.com/airockchip/librga
- **RGA 开发者指南**: `docs/Rockchip_Developer_Guide_RGA_EN.md` (在仓库内)
- **FAQ**: `docs/Rockchip_FAQ_RGA_EN.md`
- **头文件**: `include/rga.h`, `include/drmrga.h`, `include/im2d.h`

### GStreamer
- **官方文档**: https://gstreamer.freedesktop.org/documentation/
- **rtspsrc**: https://gstreamer.freedesktop.org/documentation/rtsp/rtspsrc.html
- **appsink**: https://gstreamer.freedesktop.org/documentation/app/gstappsink.html
- **视频框架**: https://gstreamer.freedesktop.org/documentation/video/

### 颜色空间
- **NV12 格式**: https://learn.microsoft.com/en-us/windows/win32/medfound/recommended-8-bit-yuv-formats-for-video-rendering#nv12
- **BT.601 转换矩阵**: `rga_convert.c:93-119` (本项目源码)

### 项目文档
- **开发日志**: `docs/dev-log.md`
- **开源组件原理**: `docs/opensource-principles.md`
- **学习笔记**: `docs/study.md`

## 工具

- **gst-launch-1.0**: 命令行测试 GStreamer 管线
- **ffprobe**: 探测视频流参数
- **gdb / strace**: 嵌入式调试
- **xxd**: 查看二进制文件头部
