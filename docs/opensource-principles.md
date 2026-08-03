# 开源组件与原理学习笔记（一手来源版）

> 基于项目：RV1126(B) RTSP 摄像头 → 硬解码 → MIPI 屏幕显示
> 编写日期：2026-08-03
> 原则：每条结论尽量引用**官方一手来源**（官方 GitHub 仓库源码/头文件、官方开发者指南、官方文档页），不用二手博客转述。
> 标注约定：✅ = 已从一手来源核实；⚠️ = 推断或经验（官方无原文）；❓待补 = 本次未完成核实，仅给官方入口。

---

## 目录

- [1. 总览：项目依赖的开源组件清单](#1-总览项目依赖的开源组件清单)
- [2. MPP 深度（Rockchip Media Process Platform）](#2-mpp-深度rockchip-media-process-platform)
- [3. RGA 深度（librga）](#3-rga-深度librga)
- [4. GStreamer 深度](#4-gstreamer-深度)
- [5. FFmpeg RTSP 选项](#5-ffmpeg-rtsp-选项)
- [6. fbdev / DRM 原理](#6-fbdev--drm-原理)
- [7. RV1126 平台公开规格](#7-rv1126-平台公开规格)
- [8. 总结：项目用法与官方建议的差异清单](#8-总结项目用法与官方建议的差异清单)

---

## 1. 总览：项目依赖的开源组件清单

| 组件 | 官方仓库 / 文档 | 本项目用法 | 本次核实程度 |
|------|----------------|-----------|:---:|
| **Rockchip MPP**（rk_mpi API） | https://github.com/rockchip-linux/mpp （默认分支 `develop`，注意不是 `master`） | `mpp_dec.c` 裸调 rk_mpi 硬解 H.264；`mppvideodec` 插件内部也用 MPP | ✅ 深核 |
| **librga**（RGA 2D 加速器用户态库） | https://github.com/airockchip/librga （默认分支 `main`） | `rga_convert.c` 用 `dlopen` 动态加载，调旧 C API `c_RkRgaBlit` | ✅ 深核 |
| **GStreamer**（核心库 + 插件） | https://gstreamer.freedesktop.org/documentation/ | `main.c` 管道 rtspsrc → rtph264depay → h264parse → mppvideodec → videoconvert → fbdevsink | 部分核实 |
| **gst-plugins-good / -base / -bad** | https://github.com/GStreamer/gst-plugins-good 等 | rtspsrc/rtph264depay/h264parse 属 good；videoconvert 属 base；fbdevsink 属 bad | 部分核实 |
| **gstreamer-rockchip**（mppvideodec 插件） | https://github.com/rockchip-linux/gstreamer-rockchip | mppvideodec | ❓待补 |
| **FFmpeg libavformat** | https://ffmpeg.org/ffmpeg-formats.html | `ffmpeg_demux.c` 拉 RTSP、取 SPS/PPS、拆帧 | ❓待补 |
| **Linux fbdev / DRM** | https://docs.kernel.org/fb/framebuffer.html ；https://dri.freedesktop.org/docs/drm/ | `fbdev.c` 显示；`experimental/drm_display.c` 未完成 | 部分核实 |
| **正点原子 SDK（alientek 2026-03-02）** | 板厂定制，非上游开源 | MPP/RGA/工具链的板端版本 | ⚠️ 板厂闭源，上游无法核实 |

版本说明（MPP）：官方仓库版本号为 **1.0.x**（CHANGELOG.md 中 1.0.0 发布于 2023-07-26，最新 1.0.12 于 2026-05-29），官方**没有 "MPP v2 / v3" 的说法**——项目注释里的 "v2/v3" 不是上游术语。上游实际有两套内部路径：旧的 legacy vpu_api 与新的 kmpp 路径（见 rk_mpi_cmd.h 中 `MPP_SET_VENC_INIT_KCFG /* kmpp path venc init cfg set */`，及官方开发指南中对旧 vpu_api 的"仅兼容用，不要再使用"说明）。板厂 SDK 的 MPP 是定制版本，无法对应上游版本号。
- 来源：https://github.com/rockchip-linux/mpp/blob/develop/CHANGELOG.md
- 来源：https://github.com/rockchip-linux/mpp/blob/develop/inc/rk_mpi_cmd.h
- 来源：https://github.com/rockchip-linux/mpp/blob/develop/doc/Rockchip_Developer_Guide_MPP_CN.md

---

## 2. MPP 深度（Rockchip Media Process Platform）

### 2.1 rk_mpi API 全貌（官方头文件 rk_mpi.h）

所有 API 都在 `inc/rk_mpi.h` 的 `MppApi` 函数指针结构体中，官方注释把接口分成两组（数据流接口 + 控制接口）：

- **简单数据流接口（simple data api set）**：
  - `decode_put_packet(ctx, packet)` — **只送不解**，异步接口："send video stream packet to decoder only, async interface"
  - `decode_get_frame(ctx, &frame)` — **只取不送**，异步接口："get video frame from decoder only, async interface"
  - `decode(ctx, packet, &frame)` — 同步二合一："both send video stream packet to decoder and get video frame from decoder at the same time"
- **高级任务接口**：`poll` / `dequeue` / `enqueue`（MppTask 模型，本项目不用）
- **控制接口**：
  - `control(ctx, cmd, param)` — 官方原话："similiar to ioctl in kernel driver, setup or get mpp internal parameter"
  - `reset(ctx)` — "discard all packet and frame, reset all components to initialized status"
- **生命周期**：`mpp_create(ctx, &mpi)`（"Create empty context structure and mpi function pointers"）→ `mpp_init(ctx, MPP_CTX_DEC, coding)`（"setup mpp type and video format"）→ 用完后 `mpp_destroy(ctx)`（"Destroy mpp context and free both context and mpi structure"）。`mpp_check_support_format(type, coding)` 可查询某编码是否被支持。

来源：https://github.com/rockchip-linux/mpp/blob/develop/inc/rk_mpi.h

### 2.2 官方命令定义（rk_mpi_cmd.h）——逐条核实项目用到的命令

项目 `mpp_dec.c` 用到的 control 命令在官方 `inc/rk_mpi_cmd.h` 中全部存在 ✅：

| 命令 | 官方定义（原文） | 项目用法 |
|------|----------------|---------|
| `MPP_DEC_GET_CFG` / `MPP_DEC_SET_CFG` | "get/set MppDecCfg structure" | 取默认配置 → `mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1)` → 写回 |
| `MPP_DEC_SET_EXT_BUF_GROUP` | "**IMPORTANT**: set external buffer group to mpp decoder"（官方注释里加了 IMPORTANT） | info_change 时把新 buffer group 交给解码器 |
| `MPP_DEC_SET_INFO_CHANGE_READY` | 无附带注释，官方 demo 中说明："All buffer group config done. Set info change ready to let decoder continue decoding" | 通知解码器继续解码 |
| `MPP_SET_INPUT_TIMEOUT` | **存在** ✅，注释："timeout setup, refer to MPP_TIMEOUT_XXX; zero - non block; negative - block with no timeout; positive - timeout in milisecond"，**parameter type RK_S64** | 项目因板厂 SDK 段错误而禁用（见 2.6） |

另外官方还有 `MPP_SET_OUTPUT_TIMEOUT`（同为 RK_S64），以及已废弃的 `MPP_SET_INPUT_BLOCK` 系列（"deprecated"）。

来源：https://github.com/rockchip-linux/mpp/blob/develop/inc/rk_mpi_cmd.h

**关于 `MPP_SET_INPUT_TIMEOUT` 的结论**：✅ 官方头文件确实定义了这个命令，参数是 `RK_S64`（64 位有符号数），项目注释里的描述准确。项目禁用它的原因是**板厂定制 SDK（alientek 2026-03-02）上触发段错误**——这是板厂 SDK 与上游 MPP 的差异，上游官方源码里没有"该命令会段错误"的记录（上游 CHANGELOG 未见相关修复）。官方推荐行为：put_packet 返回错误码后等待重试（见 2.4），`MPP_SET_INPUT_TIMEOUT` 是可选的超时控制，非必需。

### 2.3 MPP_DEC_SET_CFG 的 split_parse 语义

`base:split_parse` 键名对应的字段在 `mpp/inc/mpp_dec_cfg.h` 中存在（`RK_U32 split_parse;`）✅。官方 demo 的注释说得很清楚：

> "split_parse is to enable mpp internal frame spliter when the input packet is not aplited into frames."

来源：https://github.com/rockchip-linux/mpp/blob/develop/mpp/inc/mpp_dec_cfg.h
来源：https://github.com/rockchip-linux/mpp/blob/develop/test/mpi_dec_test.c

官方开发指南补充了两种输入形式：
1. **外部分帧**：每包 MppPacket 恰好是一整帧（MPP 默认情况，效率高）
2. **内部分帧**：按长度读取的数据，需要打开 need_split（官方指南说"需要在 mpp_init 之前，通过 control 接口的 MPP_DEC_SET_PARSER_SPLIT_MODE 命令"；而 mpi_dec_test 用的是 init 之后 `MPP_DEC_GET_CFG` + `base:split_parse` + `MPP_DEC_SET_CFG` 路线，两条都存在于官方代码中）。"如果这两种情况出现了混用，会出现码流解码出错的问题"。

项目 `ffmpeg_demux.c` 用 `av_read_frame` 拿到的是**已经分好帧**的 H.264 数据，但仍设 `split_parse=1`——这是"内部分帧"模式，官方 demo 同样这么用，兼容没问题。

来源：https://github.com/rockchip-linux/mpp/blob/develop/doc/Rockchip_Developer_Guide_MPP_CN.md （3.1.1 节）

### 2.4 decode_put_packet / decode_get_frame 官方语义（开发指南）

官方开发指南（3.1.1 / 3.1.2 节）关键原话 ✅：

- **put_packet 的消耗语义**："输入 MppPacket 的有效数据长度为 length，在送入 decode_put_packet 之后，如果输入码流被成功地消耗，函数返回值为零（MPP_OK），同时 MppPacket 的 length 被清为 0。如果输入码流还没有被处理，会返回非零错误码，MppPacket 的 length 保持不变。"
  → 印证项目注释："put_packet 返回非零不是丢帧，是队列满，要等待重试" ✅
- **最大缓冲包数**："MPP 实例默认可以接收 **4 个输入码流包**在处理队列中，如果码流送得太快，就会报出错误码要求用户等待后再送。" ✅
  → 这就是为什么官方 demo 和项目都用"失败 → sleep(1ms) → 重试"的循环。
- **get_frame 的 info change**：图像错误信息（errinfo）、变宽高信息（info change）都通过 MppFrame 带出；"解码器在解码时…内存空间需求会通过 MppFrame 的成员变量 **buf_size** 提供给用户。用户需要按 buf_size 的大小进行内存分配"。✅
- **info_change 定义**："如果为真，表示当前 MppFrame 是一个用于标记码流信息变化的描述结构，说明了新的宽高，stride，以及图像格式。可能的 info_change 原因有：图像序列宽高变化；图像序列格式变化，如 8bit 变为 10bit。一旦 info_change 产生，需要重新分析解码器使用的内存池。" ✅

来源：https://github.com/rockchip-linux/mpp/blob/develop/doc/Rockchip_Developer_Guide_MPP_CN.md （3.1.1/3.1.2/2.4 节）

### 2.5 官方参考 demo：mpi_dec_test.c 的 dec_simple 模式

官方 demo 路径 `test/mpi_dec_test.c`（dec_simple 模式）的流程，与项目 `mpp_dec.c` **逐点对照** ✅：

| 步骤 | 官方 mpi_dec_test.c | 项目 mpp_dec.c | 一致？ |
|------|--------------------|----------------|:---:|
| 创建 + 初始化 | `mpp_create(&ctx, &mpi)` → `mpp_init(ctx, MPP_CTX_DEC, coding)` | 同左（coding 固定 `MPP_VIDEO_CodingAVC`） | ✅ |
| packet 复用 | `mpp_packet_init(&packet, NULL, 0)` 创建空 packet，每帧 `mpp_packet_set_data/size/pos/length`（demo 中还有 set_eos） | 同左（`mpp_dec.c` 注释："复用 packet，设置新数据（官方 demo: mpp_packet_set_data/size/pos/length）"） | ✅ |
| split_parse | `MPP_DEC_GET_CFG` → `mpp_dec_cfg_set_u32(cfg, "base:split_parse", need_split)` → `MPP_DEC_SET_CFG` | 完全相同（写死 1） | ✅ |
| 送包 | `decode_put_packet`，返回非零则等待（单包模式里循环重试） | 循环重试 `MAX_RETRY_PKT=20` 次、`usleep(1000)` | ✅ |
| 取帧 | `decode_get_frame`；`MPP_ERR_TIMEOUT` 时 `times=30`、`msleep(1)`、goto try_again | `MAX_RETRY_FRM=30` 次、`MPP_ERR_TIMEOUT` 时 `usleep(1000)` | ✅（重试次数与官方 demo 相同） |
| info_change | 读 `mpp_frame_get_width/height/hor_stride/ver_stride/buf_size` → `dec_buf_mgr_setup(buf_mgr, buf_size, 24, buf_mode)` → `MPP_DEC_SET_EXT_BUF_GROUP` → `MPP_DEC_SET_INFO_CHANGE_READY` | 读同样 5 个值 → `mpp_buffer_group_get_internal(DRM)`（失败回退 ION）→ `mpp_buffer_group_limit_config(grp, buf_size, 24)` → `MPP_DEC_SET_EXT_BUF_GROUP` → `MPP_DEC_SET_INFO_CHANGE_READY` | ✅（流程一致，buffer 数同为 24） |
| errinfo / discard | 检查 `mpp_frame_get_errinfo/discard` | 检查并丢弃 | ✅ |
| 帧释放 | 每帧用完后 `mpp_frame_deinit`（把 MppBuffer 归还） | `mpp_dec_return()` 里 `mpp_frame_deinit` | ✅ |

**buffer group 的官方语义**（utils/mpi_dec_utils.c 注释）："Use limit config to limit buffer count and buffer size" —— `mpp_buffer_group_limit_config(group, size, count)` 就是用 size/count 限制内存池。✅

**官方对 buffer 数量的建议**（开发指南 3.3.2，模式三注意事项）："内存块数需要考虑解码和显示的需求，如果内存块数分配得太少，可能会卡住解码器。H.264/H.265 这类参考帧较多的协议需要 **20+ 内存块**能保证一定能解码。其它协议需要 10+ 内存块。" ✅ → 项目（和官方 demo）用 24 符合建议。

**buffer 模式的官方三种分类**（开发指南 3.3.2）：
1. **纯内部分配**：不设 `MPP_DEC_SET_EXT_BUF_GROUP`，info_change 时直接 `MPP_DEC_SET_INFO_CHANGE_READY`。缺点："如果内存在解码器被销毁时还没有被释放，有可能出现内存泄漏或崩溃问题"；无法限制内存用量；难做零拷贝显示。
2. **半内部分配**（mpi_dec_test 默认模式，**本项目也是**）：按 buf_size 创建 group + `mpp_buffer_group_limit_config` 限量 + `MPP_DEC_SET_EXT_BUF_GROUP`。
3. **纯外部分配**：用户从外部（gralloc/dmabuf）导入内存，零拷贝显示，但最复杂。

来源：https://github.com/rockchip-linux/mpp/blob/develop/test/mpi_dec_test.c
来源：https://github.com/rockchip-linux/mpp/blob/develop/utils/mpi_dec_utils.c
来源：https://github.com/rockchip-linux/mpp/blob/develop/doc/Rockchip_Developer_Guide_MPP_CN.md （3.3.2 节）

### 2.6 MPP v2 vs v3（项目注释的澄清）

- 上游没有 "v2/v3" 版本命名，版本号是 1.0.x（CHANGELOG.md）。✅
- 项目注释 "alientek 2026-03-02 SDK" 是板厂打包的定制 MPP，官方仓库无法核实其内容（⚠️ 板厂闭源差异）。
- 项目注释 "正点原子 SDK MPP 可能不支持多次 create/destroy"：上游 rk_mpi.h 中 `mpp_destroy` 是标准接口，官方 demo 支持多次创建销毁（无次数限制说明）。该段错误属板厂 SDK 定制行为，上游无对应记录（⚠️）。

### 2.7 输出帧 stride 对齐规则（为什么 704x576 时 ver_stride==height，1080p 时 ver_stride=1088）

这是本笔记最重要的硬件事实，已从 MPP 源码逐级核实 ✅：

**规则一（ver_stride）**：解码器输出帧的 ver_stride 由 frame slot 的对齐属性决定（`mpp/base/mpp_buf_slot.c`）：

```c
hal_ver_stride = (codec_ver_stride != 0) ? hal_ver_align(codec_ver_stride)
                                         : hal_ver_align(height);
```

不同 VPU 类的对齐函数（`osal/mpp_common.c`）：
- `mpp_align_16(val)` = `MPP_ALIGN(val, 16)`（取 16 的倍数上取整）
- `mpp_align_128_odd_plus_64(val)`：先 ALIGN 64，若 (val-64)%256==128 直接返回，否则 `((ALIGN(val,128) | 128) + 64)`（384a 专用）

解码器 HAL 注册的对齐（在 `mpp/hal/rkdec/h264d/` 下）：
- vdpu2（老芯片，如 RV1109/RV1126 的 H.264 部分）：`SLOTS_HOR_ALIGN = mpp_align_16`、`SLOTS_VER_ALIGN = mpp_align_16`（hal_h264d_vdpu2.c）
- vdpu34x：同为 16 对齐（hal_h264d_vdpu34x.c）
- vdpu384a（**RV1126B 的 RKVDEC**）：`SLOTS_HOR_ALIGN = mpp_align_128_odd_plus_64`、`SLOTS_VER_ALIGN = mpp_align_16`（hal_h264d_vdpu384a.c）

**结论**：三种 VPU 的 **ver_stride 都是 `ALIGN(height, 16)`**。
- 704×576：ALIGN(576,16) = 576（576 = 36×16，恰好对齐）→ **ver_stride == height == 576**
- 1920×1080：ALIGN(1080,16) = **1088** → ver_stride = 1088

官方开发指南的 demo 运行日志也印证了这个规则："`decoder require buffer w:h [1920:1080] stride [1920:1088] buf_size 4177920`" ✅
（注：该日志的 hor_stride 1920 来自 16 对齐的芯片；RV1126B 的 384a 对 1920 宽会算成 2112——不同芯片不同，**永远以 info_change 时 `mpp_frame_get_hor_stride/ver_stride` 返回值为准**，这正是项目 mpp_dec.c 的做法，正确。）

**规则二（hor_stride）**：芯片相关。vdpu2/vdpu34x 为 `ALIGN(width,16)`（704→704、1920→1920）；vdpu384a 为 `mpp_align_128_odd_plus_64`（704→704、1920→2112）。

**规则三（NV12 平面大小 / UV 偏移）**：官方开发指南明确给出 YUV420 内存计算：
- "图像像素数据：`hor_stride * ver_stride * 3 / 2`"
- "额外附加信息：`hor_stride * ver_stride / 2`"
即 Y 平面 = `hor_stride × ver_stride` 字节，UV 平面紧随其后。✅ 项目 `mpp_dec.c` 的 `uv_base = base + hor_stride * ver_stride` 与官方布局完全一致 ✅。`inc/mpp_frame.h` 的 ASCII 图也画了同样的布局（valid data area 的 height 在 ver_stride 内、宽度在 hor_stride 内）。

来源：https://github.com/rockchip-linux/mpp/blob/develop/mpp/base/mpp_buf_slot.c
来源：https://github.com/rockchip-linux/mpp/blob/develop/mpp/hal/rkdec/h264d/hal_h264d_vdpu2.c
来源：https://github.com/rockchip-linux/mpp/blob/develop/mpp/hal/rkdec/h264d/hal_h264d_vdpu384a.c
来源：https://github.com/rockchip-linux/mpp/blob/develop/osal/mpp_common.c
来源：https://github.com/rockchip-linux/mpp/blob/develop/inc/mpp_frame.h
来源：https://github.com/rockchip-linux/mpp/blob/develop/doc/Rockchip_Developer_Guide_MPP_CN.md （2.4 节、3.3.2 节、4.1 节 demo 日志）

### 2.8 项目 mpp_dec.c 逐点映射小结

- `mpp_dec_init`：mpp_create/mpp_init(MPP_CTX_DEC, MPP_VIDEO_CodingAVC) → 复用 packet → GET_CFG + split_parse=1 + SET_CFG。与官方 demo 一致 ✅。**差异**：官方 demo 还支持通过命令行选编码；项目固定 AVC，合理。
- `handle_info_change`：与官方 demo 的 info_change 分支逐行对应 ✅。两个小差异：
  1. 项目先试 `MPP_BUFFER_TYPE_DRM` 再回退 `MPP_BUFFER_TYPE_ION`（官方 demo 默认 ION）——Rockchip 文档说明解码器兼容 DRM/ION 内存，取决于内核 dma-buf 支持，可接受。
  2. 项目在 buffer group 分配**失败时仍 goto ready 继续** `MPP_DEC_SET_INFO_CHANGE_READY`（解码器会退回内部分配，可继续跑但可能崩）；官方 demo 是失败直接 break 退出。⚠️ 属于容错取舍，不是错误。
- `mpp_dec_decode`：put 重试 20 次 / get 重试 30 次，与官方 demo 同数量级 ✅；info_change/errinfo/discard 处理与官方一致 ✅。
- `mpp_dec_return`：延迟 `mpp_frame_deinit` 到显示完成后——**必须**这样做（官方模式二说明：帧在使用完成后要释放给解码器，形成零拷贝循环）✅。
- **发现的真实隐患**：`mpp_dec_decode` 的输出参数只暴露了 `hor_stride`（`fs`），**ver_stride 没有传出去**；`main_native.c → fb_show_nv12 → rga_init` 拿到的"高度"是视频 height 而非 ver_stride，RGA 的 hstride 因此被填成 height（见 3.4 详述）。704×576 时因 ALIGN(576,16)=576 无影响；1080p 时（ver_stride=1088）会出现 UV 偏移 8 行 → 色偏。**这是"官方语义 + 项目代码"交叉验证发现的实际问题。**

---

## 3. RGA 深度（librga）

### 3.1 c_RkRgaBlit 等旧 C API 的官方定义

librga 官方仓库 `include/RgaApi.h`（README 中即说明这是旧 API 封装，新版推荐 im2d）✅：

```c
int  c_RkRgaInit();                       // 初始化 RGA 上下文（返回 0 成功）
void c_RkRgaDeInit();                     // 反初始化
int  c_RkRgaBlit(rga_info_t *src, rga_info_t *dst, rga_info_t *src1);  // 执行 2D 操作
int  c_RkRgaFlush();                      // 刷新（同步）
```

另有宏别名 `#define RgaBlit(...) c_RkRgaBlit(__VA_ARGS__)`、`#define RgaFlush() c_RkRgaFlush()`。项目 `rga_convert.c` 通过 dlopen + dlsym 加载 `c_RkRgaInit/c_RkRgaBlit/c_RkRgaDeInit`——签名与官方头文件完全一致 ✅。

来源：https://github.com/airockchip/librga/blob/main/include/RgaApi.h

### 3.2 rga_info_t / rga_rect_t 官方字段语义

`include/drmrga.h` 定义（项目 vendored 的 `rga_info.h` 与官方一致，且项目注释写明了来源 URL）✅：

```c
typedef struct rga_info {
    int fd;              // 文件描述符（dma-buf fd）
    void *virAddr;       // 虚拟地址
    void *phyAddr;       // 物理地址
    unsigned hnd;        // Linux 下是 unsigned；Android 下是 buffer_handle_t
    int format;          // 格式（RK_FORMAT_xxx）
    rga_rect_t rect;     // 源/目标矩形描述
    ...
    int mmuFlag;         // 1 = 使用虚拟地址（需 IOMMU）
    int sync_mode;       // 1 = 同步模式
    ...
    char reserve[386];   // ABI 兼容保留区
} rga_info_t;
```

`rga_rect_t`（官方注释只给了 `size` 字段说明"user not need care about"）：

```c
typedef struct rga_rect {
    int xoffset;   // 操作区域在缓冲内的起始 x
    int yoffset;   // 操作区域在缓冲内的起始 y
    int width;     // 有效像素宽
    int height;    // 有效像素高
    int wstride;   // 缓冲的宽度 stride（单位：像素）
    int hstride;   // 缓冲的高度 stride（单位：像素）
    int format;
    int size;
} rga_rect_t;
```

官方开发指南（rga_buffer_t 一节）对 stride 的说明 ✅：
- `wstride` = "The stride of the image width, **in pixels**"
- `hstride` = "The stride of the image height, **in pixels**"
- 约束："The actual operating area cannot exceed the image size, i.e (x + width) <= wstride, (y + height) <= hstride"

**注意单位**：RGA 的 wstride/hstride 单位是**像素**。项目把 MPP 的 hor_stride（字节）直接当 wstride 用——对 8bit NV12 每像素恰好 1 字节，数值相等，无实际问题，但概念上要区分（10bit 格式就不能这么直接搬）。

来源：https://github.com/airockchip/librga/blob/main/include/drmrga.h
来源：https://github.com/airockchip/librga/blob/main/docs/Rockchip_Developer_Guide_RGA_EN.md

### 3.3 格式枚举值（rga.h）

项目用的两个格式在官方 `include/rga.h` 中 ✅：

- `RK_FORMAT_YCbCr_420_SP = 0xa << 8` = **0xA00**，官方注释 "2 plane YCbCr little endian"（即 NV12）
- `RK_FORMAT_BGRX_8888 = 0x16 << 8` = **0x1600**，官方注释 "[0:31] B:G:R:X 8:8:8:8 little endian"
- `RK_FORMAT_UNKNOWN = 0x100 << 8`

来源：https://github.com/airockchip/librga/blob/main/include/rga.h

### 3.4 420_SP 的 UV 偏移规则 —— 验证项目"hstride 必须用 ver_stride"的判断

**官方文档的直接结论**：librga 官方开发指南/FAQ **没有**用文字写明 "UV 偏移 = wstride × hstride" 这个公式（本次逐段检索未找到原文，不编造）。但可以依据官方给出的字段语义与对齐要求推出，并且与 MPP 官方布局互相印证 ✅：

1. **420_SP 是两平面格式**：Y 一个平面 + UV 交错一个平面（官方 rga.h 注释 "2 plane YCbCr"）。
2. **hstride 语义**：图像缓冲的"高度 stride"，即 Y 平面占用的**总行数**（官方字段说明："The stride of the image height, in pixels"）。
3. **Y 平面大小** = wstride(行字节数) × hstride(行数)。8bit NV12 每像素 1 字节，wstride 像素=字节。因此 UV 平面必然紧随 Y 平面之后：**UV 起始 = Y 起始 + wstride × hstride**。这与 MPP 官方开发指南的 `Y 平面 = hor_stride × ver_stride`（2.7 节规则三）**完全同构**。
4. **官方对齐要求**（开发指南 "Overview - Image Format Alignment Instructions" 表）：RGA2 上 420_SP 要求 "width stride must be **4-aligned**，x_offset、y_offset、width、height、**height stride must be 2-aligned**"；RGA3 上 wstride 16 对齐、其余 2 对齐。MPP 输出的 hor_stride/ver_stride 都是 16 对齐（2.7 节），**天然满足 RGA 要求** ✅。官方 FAQ Q2.5 解释了原因："RGA 硬件本身以 word（4 字节）为单位取每行数据…YUV 格式 Y 通道单像素 8bit，所以 YUV 格式需要 4 对齐"。

**结论（验证项目判断）**：项目注释 "hstride 必须用 ver_stride 否则 UV 偏移错" **成立**——RGA 用 hstride 定位 UV 平面，hstride 填错（把 1088 填成 1080）UV 就会提前 8 行。**但项目代码当前没做到**：`rga_convert.c` 第 118 行 `src.rect.hstride = ctx->src_h;`，而 `ctx->src_h` 来自 `fb_show_nv12` 的视频 height（`mpp_dec_decode` 传出的 `fh`），**不是 ver_stride**。704×576 时两者相等所以板上没暴露；一旦切 1080p 摄像头就会色偏（UV 读到了 Y 平面尾部亮度数据）。修复方向：`mpp_dec_decode` 增加 ver_stride 输出，`rga_init`/`rga_nv12_to_rgb` 把 hstride 与 wstride 分开传。

来源：https://github.com/airockchip/librga/blob/main/docs/Rockchip_Developer_Guide_RGA_EN.md
来源：https://github.com/airockchip/librga/blob/main/docs/Rockchip_FAQ_RGA_EN.md

### 3.5 现代推荐 API：im2d 与旧 c_RkRgaBlit 对比

官方 librga 仓库的开发者指南**通篇以 im2d API 为主线**（`include/im2d.h`、`include/im2d_type.h`），旧 C API（RgaApi.h）是历史遗留 ✅：

| 维度 | 旧 C API（项目在用） | im2d（官方推荐） |
|------|---------------------|-----------------|
| 核心函数 | `c_RkRgaBlit(src, dst, src1)` | `imcvtcolor` / `imscale` / `imcopy` / `imrotate` / `imblend`（按语义分开，不用手拼 flags） |
| 缓冲描述 | 手填 `rga_info_t`（fd/virAddr/rect...） | `rga_buffer_t` + 封装函数：`wrapbuffer_fd(fd, w, h, format)`、`wrapbuffer_virtualaddr(ptr, w, h, wstride, hstride, format)`、`importbuffer_fd` |
| 参数校验 | 无（错参数直接调驱动） | `imcheck()` + `IMStrError()` 返回可读错误信息 |
| fd vs virAddr | 手动 `mmuFlag=1` 才走虚拟地址 | `wrapbuffer_virtualaddr`（等价旧 virAddr 方式）vs `wrapbuffer_fd/importbuffer_fd`（dma-buf 方式，**官方推荐**，零拷贝、不依赖用户态虚拟地址映射） |
| 同步 | `sync_mode=1` 同步 | `imsync()` / 异步 + `imconfig` |

官方文档对 fd 方式的强调：`importbuffer_fd` 是把 "shared fd"（dmabuf/ion/drm 文件句柄）导入 RGA，"directly use external display memory, easy to achieve zero-copy"（MPP 开发指南 3.3.2 模式三同款思路）。项目用 virAddr + mmuFlag=1 依赖 IOMMU 虚拟地址翻译，功能上可行（官方 FAQ 有相关错误排查），但 fd 方式更稳、更符合零拷贝显示链路。

来源：https://github.com/airockchip/librga/blob/main/docs/Rockchip_Developer_Guide_RGA_EN.md
来源：https://github.com/airockchip/librga/blob/main/include/im2d.h
来源：https://github.com/airockchip/librga/blob/main/include/im2d_type.h

### 3.6 项目 rga_convert.c 映射小结

- dlopen 加载 `librga.so` / `libRgaApi.so` / `librockchip_rga.so` + dlsym 三个符号 ✅（RgaApi.h 签名一致）。
- `src.fd=-1; src.virAddr=y; src.mmuFlag=1; src.sync_mode=1`：虚拟地址 + IOMMU + 同步模式，是旧 API 的标准用法 ✅。
- `src.format = RK_FORMAT_YCbCr_420_SP`（0xA00）✅；`dst.format = RK_FORMAT_BGRX_8888`（0x1600）✅。
- `src.rect.wstride = ctx->src_stride`（来自 MPP hor_stride）✅；`src.rect.hstride = ctx->src_h`（= 视频 height）⚠️ **应为 ver_stride**（见 3.4）。
- dst 的 wstride = 显示宽（720），BGRX_8888 每像素 4 字节 → 行字节 2880，与板端 fbdev `line_length=2880` 吻合（fbdev.c 写显存时按 line_len 逐行拷贝，正确处理了潜在的行对齐差异）✅。
- `rga_deinit` 关闭全局 dlopen 句柄的说明已写在文件头注释中（多上下文共享限制），设计取舍清晰 ✅。
- 输出到 `f->back`（malloc 的普通内存）再由 fbdev 逐行拷贝——这是"拷贝一次"的非零拷贝路径；若走 `importbuffer_fd` + DRM 可直接把 RGA 输出指向 DRM dumb buffer，省一次拷贝（改进方向）。

---

## 4. GStreamer 深度

### 4.1 rtspsrc（官方文档，已核实）

官方文档页（Plugin rtsp，Package **GStreamer Good Plug-ins**）✅：

| 属性 | 官方描述（原文翻译） | 项目用法 |
|------|---------------------|---------|
| `location` | "Location of the RTSP url to read" | cfg->rtsp_url ✅ |
| `latency` | "Amount of ms to buffer"（**默认 2000**） | 300ms ✅（低于默认，官方 RTP 文档也说"想低延迟可调小"） |
| `drop-on-latency` | "Tells the jitterbuffer to never exceed the given latency in size" | TRUE ✅（老帧超时即丢，防延迟累积） |
| `protocols` | "Allowed lower transport protocols"，类型 **GFlags（GstRTSPLowerTrans）**，默认 "tcp+udp-mcast+udp" | 4（仅 TCP）|

官方 RTP/RTSP 附加文档说明协商顺序：**UDP unicast → UDP multicast → TCP**，顺序不可改，但可用 `protocols` 属性裁剪允许的集合。项目设 4 就是只允许 TCP——跳过 UDP 试探（防火墙拦截 UDP 时默认会等超时才回退 TCP，直接锁 TCP 秒连）✅。

**GST_RTSP_LOWER_TRANS_TCP = 4 的值来源**：`GstRTSPLowerTrans` 枚举定义在 gst-plugins-base 的 RTSP 库头文件（`gst-libs/gst/rtsp/gstrtspdefs.h`），按位标志语义为 UDP=(1<<0)=1、UDP_MCAST=(1<<1)=2、TCP=(1<<2)=**4**。官方文档页确认了它是 flags 类型及默认值包含三项（即 1|2|4=7）。❓待补：本次未能直接抓取到 gstrtspdefs.h 原文验证数值（网络/分支问题），数值 4 与官方文档页默认值 "tcp+udp-mcast+udp" 自洽，但请以板端头文件为准。

**"protocols 必须单独 set"**：官方文档**没有**这个要求（❌ 官方无据）。GObject 的 `g_object_set` 本来就是变参一次设置多属性。常见真实原因属于工程经验：GFlags 值在 varargs 里必须按 `guint` 精确传递，混用字符串/布尔时若类型不匹配（比如误把 4 写进字符串槽）会 GValue 转换失败甚至崩溃；且 flags 值脱离枚举名易读性差。项目注释表述为经验结论即可，不必当作官方规则。

**rtspsrc 内部结构**：官方文档说明 rtspsrc 内部实例化 gstrtpbin 来管理 RTCP、**抖动消除（jitter removal）、包重排序**并作为管道时钟——这解释了 `latency` 为什么是"jitterbuffer 缓冲量"。

来源：https://gstreamer.freedesktop.org/documentation/rtsp/rtspsrc.html
来源：https://gstreamer.freedesktop.org/documentation/additional/rtp.html
来源：https://github.com/GStreamer/gst-plugins-good （gst/rtsp/gstrtspsrc.c）

### 4.2 rtph264depay / h264parse

- `rtph264depay`：属于 **gst-plugins-good 的 rtpmanager 插件**（官方文档页 rtph264depay）。作用：把 RTP 载荷还原成 H.264 NAL——按 RTP 序列号/时间戳重组、解析 FU-A/STAP-A 分片、撕掉 RTP 头，输出带 NAL 的码流。❓待补：官方文档页属性细节（wait-for-keyframe 等）本次未逐条核实。
- `h264parse`：属于 **gst-plugins-good 的 videoparsers 插件**。作用：H.264 码流解析与包装转换（avcC ↔ Annex-B），属性 `config-interval`（每隔 N 个帧重发 SPS/PPS，对 RTP 场景很有用——接收端若中途加入需要 SPS/PPS）。❓待补：属性细节本次未逐条核实。
- 官方入口：https://gstreamer.freedesktop.org/documentation/rtpmanager/rtph264depay.html 、https://gstreamer.freedesktop.org/documentation/videoparsers/h264parse.html

### 4.3 mppvideodec 归属仓库

❓待补（本次未抓取源码验证，给官方入口）：Rockchip 的 MPP 解码插件在 **https://github.com/rockchip-linux/gstreamer-rockchip**（plugins/src 下的 mppvideodec / mpph264dec 等），内部通过 rk_mpi API（与项目 mpp_dec.c 同一套 API）做硬解，负责 buffer 分配与 info_change 处理（这就是为什么 GStreamer 管道里看不到 info_change 逻辑）。是否支持 dmabuf 输出、内部 buffer 模式（内部分配 or 外部分配）需以该仓库源码为准（❓待补）。

### 4.4 videoconvert / fbdevsink

- `videoconvert`：**gst-plugins-base** 插件（官方文档页：Plugin videoconvert，Package GStreamer Base Plug-ins）。作用是颜色空间/格式转换。
  ⚠️ **项目注释"videoconvert 内部调用 RGA 硬件加速"缺乏官方依据**：videoconvert 是纯 CPU/ORC 软件转换插件，官方文档未提及 RGA。在 Rockchip 平台上"RGA 加速的转换"是 gstreamer-rockchip 仓库里别的插件/补丁做的事（❓待补核实）。板上实测 mppvideodec 输出 NV12 后由 videoconvert 转 RGB 可能是 CPU 转的——延迟与 CPU 占用需要实测确认。
- `fbdevsink`：**gst-plugins-bad** 的 sys/fbdev 插件（❓待补官方文档页 URL），`device` 属性指定 /dev/fb0。项目 main.c 设置了 device ✅。
- 官方入口：https://gstreamer.freedesktop.org/documentation/videoconvert/index.html 、https://github.com/GStreamer/gst-plugins-bad/tree/master/sys/fbdev

### 4.5 pad probe 与 bus watch（GStreamer 核心机制）

- **pad probe**：`gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, callback, ...)` —— 在数据流路径上挂"探针"，缓冲流过 pad 时回调（可以只观察、也可以拦截修改）。项目 main.c 用它数帧。官方核心 API 文档：https://gstreamer.freedesktop.org/documentation/gstreamer/gstpad.html ✅（本次未逐条核实细节，机制描述基于官方 API 文档结构）。
- **bus watch**：`gst_bus_add_watch(bus, callback, ...)` 把 bus 消息（ERROR/WARNING/EOS/STATE_CHANGED...）挂到主循环（GMainLoop）里异步回调。项目 main.c 的 `on_bus_message` 处理 EOS/ERROR 退出、STATE_CHANGED/QOS 忽略。官方核心 API 文档：https://gstreamer.freedesktop.org/documentation/gstreamer/gstbus.html ✅。
- **pad-added 动态链接**：rtspsrc 的 src pad 是**运行期动态创建**的（SETUP 之后才知道有几个媒体轨），必须用 `g_signal_connect(rtspsrc, "pad-added", ...)` 在回调里按 caps（`application/x-rtp`）决定是否链接——官方 rtspsrc 文档明确说明它会为每个 SDP 媒体流创建 `rtp_stream%d` pad ✅。项目 on_pad_added 先查 caps 前缀 `application/x-rtp` 再链，符合官方建议（只处理视频轨，忽略音频轨）。

---

## 5. FFmpeg RTSP 选项

❓待补（一句话概述 + 官方入口，本次未逐条核实）：

项目 `ffmpeg_demux.c` 在 `avformat_open_input` 时通过 AVDictionary 设置了 `rtsp_transport=tcp`（失败回退重试）与 `stimeout=3000000`（3 秒，官方单位是**微秒**），随后 `avformat_find_stream_info` 探测（可得到 SPS/PPS 即 `codecpar->extradata`，项目把它缓存下来）并 `av_read_frame` 逐帧取出 H.264 包。官方选项语义请查：**https://ffmpeg.org/ffmpeg-formats.html** 的 rtsp 段（`rtsp_transport`：udp/tcp/udp_multicast/http；`stimeout`：socket IO 超时，单位微秒，默认 -1）与 **https://ffmpeg.org/ffmpeg-protocols.html**（tcp 选项、`rw_timeout`）。低延迟相关选项（`fflags=nobuffer`、`max_delay`、`probesize`、`analyzeduration`）在 https://ffmpeg.org/ffmpeg-formats.html 的"Options"一节——`fflags=nobuffer` 官方描述为减少输入处理时的缓冲以降低延迟，`probesize`/`analyzeduration` 控制探测阶段读取的数据量（探测越少起播越快，但 SPS/PPS 可能探测不到）。建议补核后与项目 3s stimeout + 全量探测的取舍对照。

---

## 6. fbdev / DRM 原理

### 6.1 fbdev 用户态 API（部分核实）

项目 `fbdev.c` 的用法与内核 fbdev 接口一致 ✅（uapi 头文件 `include/uapi/linux/fb.h`，内核文档 `Documentation/fb/framebuffer.rst`）：
- `open("/dev/fb0", O_RDWR)` → `ioctl(FBIOGET_VSCREENINFO, &var)`（`xres/yres/bits_per_pixel`）→ `ioctl(FBIOGET_FSCREENINFO, &fix)`（`line_length` 每行字节数、`smem_len` 显存大小）→ `mmap(smem_len, MAP_SHARED, fd, 0)` 映射显存。
- **line_length 语义**：内核驱动按硬件（CRTC）行对齐要求返回的每行字节数，**可能 > xres×bpp/8**，用户只能读不能改；写帧必须逐行按 line_length 拷贝——项目 `fbdev.c` 的 32bpp 分支正是逐行 `memcpy(&d[r*line_len/4], &s[r*w], dw*4)`，正确处理 ✅（16bpp 分支也按 line_len/2 行步进 ✅）。
- 内核文档入口：https://docs.kernel.org/fb/framebuffer.html ；uapi 头文件：https://github.com/torvalds/linux/blob/master/include/uapi/linux/fb.h
- ❓待补：本次未逐字引用内核文档原文，机制描述以 uapi 头文件注释与项目实测（line_length=2880 = 720×4 恰好无填充）为准。

### 6.2 DRM/KMS 基础与 drmModeSetCrtc 的问题（部分核实）

项目 `experimental/drm_display.c` 的链路（libdrm 用户态 API）：`drmModeGetResources` → 找 connector/encoder/crtc → `DRM_IOCTL_MODE_CREATE_DUMB` 创建 dumb buffer → `drmModeAddFB2` 注册 framebuffer → `DRM_IOCTL_MODE_MAP_DUMB` + mmap 映射 → `drmModeSetCrtc` 激活 → 每帧写像素后**再次 `drmModeSetCrtc` 刷新**。

**为什么每帧 SetCrtc 不好**（官方 KMS 文档语义）：`drmModeSetCrtc` 是"全局模式配置"接口——设置 connector 使用的 mode、把某个 FB 绑到 CRTC；每次调用都会触发 mode 重新配置（modeset），属于慢路径；而正常视频输出应该用 **`drmModePageFlip`**（在已配置好的 CRTC 上无阻塞切换 scanout buffer，与 vsync 同步）或 **atomic commit**（`DRM_MODE_ATOMIC_*`，多资源原子提交）。每帧 SetCrtc 的代价：无 vsync 同步（可能撕裂）、重复 modeset 开销、不是线程安全的推荐路径。官方文档入口：https://dri.freedesktop.org/docs/drm/gpu/drm-kms.html（Page Flip / CRTC 一节）。❓待补：本次未逐字引用，建议补核后把 drm_display.c 改成双 buffer + drmModePageFlip 或 atomic。

另外 `drm_display.c` 的 `drm_show_rgb` 里把 RGB888 逐像素展开成 XRGB8888（每像素 4 字节写回）——正确但慢；后续应让 RGA 直接输出 XRGB8888 到 dumb buffer（和 fbdev 路径同思路）。

---

## 7. RV1126 平台公开规格

### 7.1 已从 MPP 官方源码核实的事实 ✅

MPP 的 `osal/mpp_soc.c` 芯片表中明确写着两块芯片的编解码硬件构成（这是目前最可靠的一手来源）：

- **rv1126**（注释原文）："vpu2 for jpeg encoder and decoder；RK H.264/H.265 **4K decoder**；RK H.264/H.265 **4K encoder**"，能力位 `HAVE_VDPU2 | HAVE_VEPU2 | HAVE_RKVDEC | HAVE_RKVENC`，解码器条目 `{&vdpu2_jpeg_fix, &vdpu341_lite, ...}`、编码器 `{&vepu2_jpeg, &vepu541, ...}`。
- **rv1126b**（正点原子 ATK-DLRV1126B 的芯片，本项目实际平台）："RK H.264/H.265 4K decoder；RK H.264/H.265/jpeg 4K encoder；RK jpeg decoder"，能力位 `HAVE_RKVDEC | HAVE_RKVENC | HAVE_JPEG_DEC`，解码器 `{&vdpu384a, &rkjpegd, ...}`、编码器 `{&vepu511, ...}`。

→ 即：RV1126B 的 H.264 硬解走 **vdpu384a**（RKVDEC），这就是 2.7 节为什么 hor_stride 对齐规则要看 384a 的原因。MPP 对内核的要求（开发指南 1.3 节）："支持瑞芯微 Linux 内核 3.10、4.4、4.19、5.10 和 6.1 版本，需要有 **vcodec_service/mpp_service** 设备驱动支持以及相应的 DTS 配置支持"。

来源：https://github.com/rockchip-linux/mpp/blob/develop/osal/mpp_soc.c
来源：https://github.com/rockchip-linux/mpp/blob/develop/doc/Rockchip_Developer_Guide_MPP_CN.md

### 7.2 公开规格待查项

❓待补（本次未完成核实，仅给官方/权威入口）：
- **RGA 代数**：RV1126B 集成的是 RGA2 还是 RGA3（影响 3.4 节对齐要求取哪一列）。Rockchip 官方 wiki / 芯片发布页：https://opensource.rock-chips.com/wiki_Introduction （wiki 页含各 SoC 多媒体能力矩阵）；Linux 内核 rockchip 驱动的 `drivers/video/rockchip/rga2/` 与 `rga3/` 设备树节点可反推板级实际使用的 RGA 型号。
- **ISP 代数与摄像头接口**（MIPI CSI / 内置 ISP 能力）：同上 wiki 页及板厂手册。
- 如果 wiki 页无法访问，公开资料确实有限——该芯片主要面向安防方案商，消费级媒体资料少，建议以板厂 SDK 的 dts/驱动为准。

---

## 8. 总结：项目用法与官方建议的差异清单

以下每条都是"官方一手来源语义"与"项目实际代码"对照的结果：

| # | 差异点 | 官方依据（来源） | 影响与建议 |
|---|--------|----------------|-----------|
| 1 | **RGA hstride 用了视频 height 而非 ver_stride**（rga_convert.c:118 `src.rect.hstride = ctx->src_h`；根源是 mpp_dec.c 没有把 ver_stride 传出） | RGA 官方字段语义：hstride="The stride of the image height"（librga 开发指南）；MPP 官方：ver_stride=ALIGN(height,16)（mpp_buf_slot.c + hal_h264d_vdpu384a.c）；MPP 官方布局 Y 平面=hor_stride×ver_stride（开发指南 3.3.2） | 704×576 无问题；**1080p 时 UV 偏移 8 行 → 色偏**。建议 mpp_dec_decode 增加 ver_stride 输出参数并传给 RGA |
| 2 | **MPP_SET_INPUT_TIMEOUT 被禁用** | 官方 rk_mpi_cmd.h：命令存在、参数 RK_S64、语义"0 非阻塞/负数阻塞/正数毫秒超时" | 板厂 SDK 段错误是定制差异，上游无此问题；禁用后可接受（官方 demo 本来就靠错误码+重试，不依赖该命令）。若后续升级 MPP 可尝试启用并做超时重连 |
| 3 | **"protocols 必须单独 set" 无官方依据** | GObject g_object_set 官方机制（变参多属性）；rtspsrc 官方文档未提此限制 | 只是工程经验（varargs 类型匹配/可读性），不是规则；不影响正确性 |
| 4 | **"videoconvert 内部调用 RGA" 无官方依据** | videoconvert 官方文档（gst-plugins-base，软件转换） | 若 RGA 加速转换是需求，应改用 gstreamer-rockchip 的专用插件或确认板端插件行为，否则 videoconvert 是 CPU 转的（实测确认 CPU/延迟） |
| 5 | **experimental/drm_display.c 每帧 drmModeSetCrtc 刷新** | DRM KMS 官方文档：pageflip/atomic 才是帧切换路径 | 每帧 modeset 慢且无 vsync；建议双 dumb buffer + drmModePageFlip |
| 6 | **RGA 用 virAddr+mmuFlag（依赖 IOMMU），非 fd 零拷贝** | librga 官方 im2d：importbuffer_fd/wrapbuffer_fd 为推荐方式 | 板端 IOMMU 未开时 blit 直接失败（项目已有 CPU 回退，安全）；要零拷贝显示需 fd + DRM |
| 7 | **帧率控制 sleep 插在解码循环里** | MPP 官方：输入队列默认最多 4 包，put 失败要等（开发指南 3.1.1） | 控帧会导致 put 队列偶尔打满（有重试兜底）；更优是解多少显示多少，用显示侧时钟控帧 |
| 8 | **buffer 数 24 / 半内部分配模式** | 官方 demo 用 24；官方建议 H.264/H.265 需 20+ 块（开发指南 3.3.2） | ✅ 与官方一致，无需改 |
| 9 | **uv_base = base + hor_stride×ver_stride** | MPP 官方布局（开发指南 3.3.2：像素数据=hor_stride×ver_stride×3/2） | ✅ 正确 |
| 10 | **fbdev 逐行拷贝（line_length 语义）** | fbdev uapi/内核文档 line_length 可能>行宽 | ✅ 正确 |
| 11 | **"MPP v2/v3" 术语** | 官方 CHANGELOG 只有 1.0.x；内部是 legacy vpu_api vs kmpp | 文档/注释建议改用上游术语，避免误解 |

---

## 附：本次核实的一手来源清单（全部 URL）

**MPP（rockchip-linux/mpp，develop 分支）**
- API 全貌：https://github.com/rockchip-linux/mpp/blob/develop/inc/rk_mpi.h
- 命令枚举（MPP_SET_INPUT_TIMEOUT 等）：https://github.com/rockchip-linux/mpp/blob/develop/inc/rk_mpi_cmd.h
- MppFrame（stride 图/取值函数）：https://github.com/rockchip-linux/mpp/blob/develop/inc/mpp_frame.h
- MppDecCfg（split_parse 字段）：https://github.com/rockchip-linux/mpp/blob/develop/mpp/inc/mpp_dec_cfg.h
- 官方 demo：https://github.com/rockchip-linux/mpp/blob/develop/test/mpi_dec_test.c
- 官方 buffer 管理：https://github.com/rockchip-linux/mpp/blob/develop/utils/mpi_dec_utils.c
- stride 计算：https://github.com/rockchip-linux/mpp/blob/develop/mpp/base/mpp_buf_slot.c
- vdpu2 对齐：https://github.com/rockchip-linux/mpp/blob/develop/mpp/hal/rkdec/h264d/hal_h264d_vdpu2.c
- vdpu384a 对齐：https://github.com/rockchip-linux/mpp/blob/develop/mpp/hal/rkdec/h264d/hal_h264d_vdpu384a.c
- 对齐函数：https://github.com/rockchip-linux/mpp/blob/develop/osal/mpp_common.c
- 芯片表（rv1126/rv1126b）：https://github.com/rockchip-linux/mpp/blob/develop/osal/mpp_soc.c
- 开发指南（中文）：https://github.com/rockchip-linux/mpp/blob/develop/doc/Rockchip_Developer_Guide_MPP_CN.md
- 版本历史：https://github.com/rockchip-linux/mpp/blob/develop/CHANGELOG.md

**librga（airockchip/librga，main 分支）**
- 旧 C API 声明：https://github.com/airockchip/librga/blob/main/include/RgaApi.h
- rga_info_t/rga_rect_t：https://github.com/airockchip/librga/blob/main/include/drmrga.h
- 格式枚举：https://github.com/airockchip/librga/blob/main/include/rga.h
- im2d 头文件：https://github.com/airockchip/librga/blob/main/include/im2d.h 、.../im2d_type.h
- 开发者指南：https://github.com/airockchip/librga/blob/main/docs/Rockchip_Developer_Guide_RGA_EN.md
- FAQ：https://github.com/airockchip/librga/blob/main/docs/Rockchip_FAQ_RGA_EN.md

**GStreamer**
- rtspsrc：https://gstreamer.freedesktop.org/documentation/rtsp/rtspsrc.html
- RTP/RTSP 附加文档：https://gstreamer.freedesktop.org/documentation/additional/rtp.html
- GstPad（probe）：https://gstreamer.freedesktop.org/documentation/gstreamer/gstpad.html
- GstBus（watch）：https://gstreamer.freedesktop.org/documentation/gstreamer/gstbus.html
- mppvideodec 仓库（待补核）：https://github.com/rockchip-linux/gstreamer-rockchip
- fbdevsink（gst-plugins-bad sys/fbdev，待补核）：https://github.com/GStreamer/gst-plugins-bad/tree/master/sys/fbdev
- videoconvert：https://gstreamer.freedesktop.org/documentation/videoconvert/index.html

**FFmpeg / Linux 显示 / 平台**
- FFmpeg 格式选项（rtsp 段，待补核）：https://ffmpeg.org/ffmpeg-formats.html
- fbdev 内核文档（待补核）：https://docs.kernel.org/fb/framebuffer.html ；uapi：https://github.com/torvalds/linux/blob/master/include/uapi/linux/fb.h
- DRM/KMS（待补核）：https://dri.freedesktop.org/docs/drm/gpu/drm-kms.html
- Rockchip 开源 wiki（RV1126 规格，待补核）：https://opensource.rock-chips.com/wiki_Introduction
