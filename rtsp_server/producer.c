/* producer.c — 板子生产者主程序 (三码流, 预算内配置)
 *
 * 数据流:
 *   主码流:  v4l2src(/dev/video-camera0, ISP mainpath 2688x1520)
 *            → videorate(15) → mpph264enc → /stream0   (2K, 5Mbps)
 *   子码流1: v4l2src(/dev/video32, ISP selfpath 1920x1080)
 *            → tee → videorate(20) → mpph264enc → /stream1  (1080p, 2Mbps)
 *   子码流2: 1080p 原始帧 → videoscale(CPU) → videorate(30)
 *            → mpph264enc → /stream2                      (480p, 1Mbps, LCD 用)
 *
 * MPP 预算 (编码+解码共用, 实测 ≈127M 像素/秒):
 *   主线禁用时: 1080p@15 (31M) + 480p@30 编码 (10.5M)
 *   + 显示解码 海康 D1@25 (10M) + 预览转码 解码+JPEG (20M) ≈ 72M ✓
 *   注意: 总吞吐低于预算但并发仍可能饿死 H264 编码器 (h264e_dpb
 *   看门狗崩溃) — 1080p 编码从 20fps 降到 15fps 留调度余量 (实测验证)
 *   1080p 单路 60fps 需 124M, 超出预算 → 不可行;
 *   480p 单路 60fps 仅 21M → 可行, 但会挤占主码流/1080p 的预算。
 *
 * 用法: ./producer [端口]
 * 客户端: rtsp://<板子IP>:8554/stream0 | /stream1 | /stream2
 *
 * 编译: make producer  (见 Makefile, 需要 GStreamer 交叉编译头)
 */
#define _POSIX_C_SOURCE 200809L

#include "nal.h"
#include "onvif.h"
#include "rtsp_server.h"
#include "npu/detect.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned long det_feed_cnt = 0;   /* NPU 检测喂帧计数 (每 3 帧一次) */

#define DEFAULT_PORT 8554

/* ---------------- 三码流定义 ---------------- */

typedef struct {
    const char *path;        /* RTSP 挂载路径 */
    const char *device;      /* 采集设备 (480p 分支为 NULL: 从 1080p 推帧) */
    int w, h, bps, fps;
    rtsp_mount_t *mount;
    GstElement *pipe;
    GstAppSink *sink;        /* 编码出口 (H.264 → feed 挂载点) */
    GstAppSink *raw_sink;    /* 原始帧出口 (NV12 → 推给 480p 管线) */
    GstAppSink *draw_sink;   /* 画框源 (原始帧 → 画检测框 → JPEG 预览) */
    GstAppSink *jpeg_sink;   /* JPEG 预览出口 (最新帧 → /preview) */
    GstElement *push_src;      /* stream2 管线的 appsrc (画框帧) */
    GstElement *main_push_src; /* stream1 管线的 appsrc (画框帧) */
    GstElement *jpeg_push_src; /* JPEG 管线的 appsrc (画框帧) */
    int raw_caps_set;
    unsigned long feed_count;
} stream_ctx_t;

static rtsp_server_t *g_srv = NULL;
static onvif_server_t *g_onvif = NULL;
static stream_ctx_t g_st[3];

/* ---- MJPEG 预览: 最新 JPEG 帧缓冲 (producer ↔ onvif HTTP 共享) ---- */
static pthread_mutex_t g_jpeg_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_jpeg_data = NULL;
static size_t g_jpeg_len = 0;
static gint64 g_jpeg_ts = 0;   /* 最近 JPEG 帧编码完成时刻 */

static int jpeg_get_latest(void *ctx, jpeg_frame_t *out) {
    (void)ctx;
    pthread_mutex_lock(&g_jpeg_lock);
    /* 锁内拷贝到调用方缓冲 (out->data/cap): 帧生命周期归调用方,
     * 不再共享 g_jpeg_data 裸指针 — 消除 UAF/撕裂帧 (扫1 #8) */
    if (g_jpeg_data && g_jpeg_len > 0 && g_jpeg_len <= out->cap) {
        memcpy(out->data, g_jpeg_data, g_jpeg_len);
        out->len = g_jpeg_len;
        out->ts = g_jpeg_ts;
        pthread_mutex_unlock(&g_jpeg_lock);
        return 0;
    }
    pthread_mutex_unlock(&g_jpeg_lock);
    return -1;
}

/* ---- PTZ 指令回调: 摄像头接受指令 (onvif.c 解析成功) 后立即调用 ----
 * 本机无云台 → 写 /tmp/ptz_dir, rtsp_display 在屏幕画箭头证明收到;
 * 真实云台时代替为电机控制 (同一回调, 换实现不换接口) */
static void on_ptz_command(ptz_dir_t dir, double speed, void *ctx) {
    (void)ctx;
    const char *s = dir == PTZ_LEFT ? "LEFT"
                  : dir == PTZ_RIGHT ? "RIGHT"
                  : "STOP";
    FILE *f = fopen("/tmp/ptz_dir", "w");
    if (f) {
        fprintf(f, "%s %lld\n", s, (long long)g_get_monotonic_time());
        fclose(f);
    }
    printf("[producer] PTZ 指令: %s (speed=%.2f) → 屏幕显示标识\n", s, speed);
}

/* 预置位回调: 摄像头接受 Set/GotoPreset 后调用 — 屏幕显示预置位标识,
 * 真实云台时代替为电机转到该位置 */
static void on_ptz_preset(const char *token, int goto_mode, void *ctx) {
    (void)ctx;
    FILE *f = fopen("/tmp/ptz_dir", "w");
    if (f) {
        fprintf(f, "PRESET %s %s %lld\n", token,
                goto_mode == 1 ? "goto" : (goto_mode == 2 ? "remove" : "set"),
                (long long)g_get_monotonic_time());
        fclose(f);
    }
    printf("[producer] PTZ 预置位: %s (%s) → 屏幕显示标识\n", token,
           goto_mode == 1 ? "调用" : (goto_mode == 2 ? "删除" : "设置"));
}

#define ST_MAIN  0   /* /stream0  2688x1520 主码流 */
#define ST_1080  1   /* /stream1  1920x1080 子码流 */
#define ST_480   2   /* /stream2  720x480   显示用 (1080p 帧 CPU 缩放) */

static const onvif_profile_t g_profiles[3] = {
    { "MainStream", "主码流",    2688, 1520, "/stream0" },
    { "Sub1080",    "子码流1080", 1920, 1080, "/stream1" },
    { "Sub720",     "子码流720",  1280, 720,  "/stream2" },
};

/* NV12 帧上画绿色矩形框 (4px 粗) — 检测可视化 (画在自有拷贝上) */
static void draw_rect_nv12(uint8_t *y, uint8_t *uv, int w, int h, int hs,
                           int x0, int y0, int x1, int y1) {
    const uint8_t YV = 150, UV = 50, VV = 30;   /* 绿色 NV12 */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= w) x1 = w - 1;
    if (y1 >= h) y1 = h - 1;
    if (x1 < x0 || y1 < y0) return;
    for (int t = 0; t < 4; t++) {
        for (int x = x0; x <= x1; x++) {
            y[y0 * hs + x] = YV;
            y[y1 * hs + x] = YV;
            int up = (y0 / 2) * hs + (x / 2) * 2;
            uv[up] = UV; uv[up + 1] = VV;
            up = (y1 / 2) * hs + (x / 2) * 2;
            uv[up] = UV; uv[up + 1] = VV;
        }
        for (int yy = y0; yy <= y1; yy++) {
            y[yy * hs + x0] = YV;
            y[yy * hs + x1] = YV;
            int up = (yy / 2) * hs + (x0 / 2) * 2;
            uv[up] = UV; uv[up + 1] = VV;
            up = (yy / 2) * hs + (x1 / 2) * 2;
            uv[up] = UV; uv[up + 1] = VV;
        }
        x0++; y0++; x1--; y1--;
    }
}

/* ---------------- appsink 回调 ---------------- */

static GstFlowReturn on_new_sample(GstAppSink *appsink, gpointer user_data) {
    stream_ctx_t *st = user_data;

    GstSample *sample = gst_app_sink_pull_sample(appsink);   /* 从触发的 sink 拉 */
    if (!sample) return GST_FLOW_OK;

    GstBuffer *buf = gst_sample_get_buffer(sample);

    if (appsink == st->sink) {
        /* 编码出口: H.264 → feed 挂载点 */
        GstMapInfo map;
        if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
            if (map.size > 0) {
                nal_unit_t *nals = NULL;
                int n = 0;
                if (nal_split(map.data, map.size, &nals, &n) >= 0) {
                    for (int i = 0; i < n; i++)
                        rtsp_server_feed_nal(st->mount, nals[i].data, nals[i].len);
                    free(nals);
                }
                st->feed_count++;
                if (st->feed_count == 1)
                    printf("[producer] %s 编码链路正常\n", st->path);
                else if (st->feed_count % 900 == 0)
                    printf("[producer] %s 心跳: %lu 帧\n", st->path, st->feed_count);
            }
            gst_buffer_unmap(buf, &map);
        }
    } else if (st->draw_sink && appsink == st->draw_sink) {
        /* 画框源: 拷贝帧 → NPU 检测 + 绿框叠加 → 分发三条编码管线
         * (stream1 主码流 / stream2 子码流 / JPEG 预览 — 全带框;
         * 拷贝到自有缓冲再画, 不写 DMABUF) */
        GstMapInfo dm;
        if (buf && gst_buffer_map(buf, &dm, GST_MAP_READ)) {
            size_t ysz = (size_t)st->w * st->h;
            static uint8_t *cpy = NULL;
            static size_t cpy_cap = 0;
            if (cpy_cap < ysz * 3 / 2) {
                free(cpy);
                cpy = malloc(ysz * 3 / 2);
                cpy_cap = ysz * 3 / 2;
            }
            if (cpy && ysz * 3 / 2 <= dm.size) {
                memcpy(cpy, dm.data, ysz);
                memcpy(cpy + ysz, dm.data + ysz, ysz / 2);

                /* NPU 检测: 每 3 帧喂一次, 结果画绿框 */
                if (++det_feed_cnt % 3 == 0)
                    detect_feed(cpy, cpy + ysz, st->w, st->h, st->w);
                det_box_t boxes[8];
                int nb = detect_get(boxes, 8);
                for (int i = 0; i < nb; i++)
                    draw_rect_nv12(cpy, cpy + ysz, st->w, st->h,
                                   st->w, boxes[i].left, boxes[i].top,
                                   boxes[i].right, boxes[i].bottom);
                if (nb > 0) {
                    printf("[detect] %d 个目标 (帧 %lu): ",
                           nb, st->feed_count);
                    for (int i = 0; i < nb; i++)
                        printf("c%d@%.2f ", boxes[i].cls, boxes[i].conf);
                    printf("\n");
                }

                /* 三路分发 (共享 buffer, ref 计数) */
                GstBuffer *dup = gst_buffer_new_allocate(
                    NULL, ysz * 3 / 2, NULL);
                GstMapInfo wm;
                gst_buffer_map(dup, &wm, GST_MAP_WRITE);
                memcpy(wm.data, cpy, ysz * 3 / 2);
                gst_buffer_unmap(dup, &wm);

                static guint64 mpts = 0, spts = 0, jpts = 0;
                if (st->main_push_src) {
                    GstBuffer *b = gst_buffer_ref(dup);
                    GST_BUFFER_PTS(b) = mpts;
                    mpts += GST_SECOND / st->fps;
                    gst_app_src_push_buffer(GST_APP_SRC(st->main_push_src), b);
                }
                if (st->push_src) {
                    GstBuffer *b = gst_buffer_ref(dup);
                    GST_BUFFER_PTS(b) = spts;
                    spts += GST_SECOND / 30;
                    if (gst_app_src_get_current_level_bytes(
                            GST_APP_SRC(st->push_src)) < 1024 * 1024)
                        gst_app_src_push_buffer(GST_APP_SRC(st->push_src), b);
                    else
                        gst_buffer_unref(b);
                }
                if (st->jpeg_push_src) {
                    GstBuffer *b = gst_buffer_ref(dup);
                    GST_BUFFER_PTS(b) = jpts;
                    jpts += GST_SECOND / 5;
                    gst_app_src_push_buffer(GST_APP_SRC(st->jpeg_push_src), b);
                }
                gst_buffer_unref(dup);
            }
            gst_buffer_unmap(buf, &dm);
        }
    } else if (st->jpeg_sink && appsink == st->jpeg_sink) {
        /* JPEG 预览出口: 保存最新帧, 供 HTTP /preview 推送 */
        GstMapInfo map;
        if (buf && gst_buffer_map(buf, &map, GST_MAP_READ) && map.size > 0) {
            pthread_mutex_lock(&g_jpeg_lock);
            if (!g_jpeg_data || g_jpeg_len < map.size) {
                free(g_jpeg_data);
                g_jpeg_data = malloc(map.size);
            }
            if (g_jpeg_data) {
                memcpy(g_jpeg_data, map.data, map.size);
                g_jpeg_len = map.size;
                g_jpeg_ts = g_get_monotonic_time();
            }
            pthread_mutex_unlock(&g_jpeg_lock);
            gst_buffer_unmap(buf, &map);
        }
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer user_data) {
    (void)bus; (void)user_data;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        fprintf(stderr, "[producer] 管线错误: %s (%s)\n",
                err ? err->message : "?", dbg ? dbg : "");
        if (err) g_error_free(err);
        if (dbg) g_free(dbg);
    } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_warning(msg, &err, &dbg);
        fprintf(stderr, "[producer] 管线警告: %s\n",
                err ? err->message : "?");
        if (err) g_error_free(err);
        if (dbg) g_free(dbg);
    }
    return TRUE;
}

/* 构建并启动一条管线 (videorate 硬限帧率, 防止霸占 MPP 调度) */
static GstElement *build_pipeline(stream_ctx_t *st) {
    char launch[640];
    if (st->raw_sink) {
        /* 1080p 采集管线: 原始帧 → drawsink (拷贝画框后分发三条编码管线:
         * stream1 主码流 + stream2 子码流 + JPEG 预览 — 全带检测框) */
        snprintf(launch, sizeof launch,
                 "v4l2src device=%s ! "
                 "video/x-raw,format=NV12,width=%d,height=%d,framerate=30/1 ! "
                 "queue ! appsink name=drawsink",
                 st->device, st->w, st->h);
    } else if (st->device) {
        snprintf(launch, sizeof launch,
                 "v4l2src device=%s ! "
                 "video/x-raw,format=NV12,width=%d,height=%d,framerate=30/1 ! "
                 "videorate ! "
                 "video/x-raw,format=NV12,framerate=%d/1 ! "
                 "mpph264enc bps=%d ! "
                 "appsink name=sink",
                 st->device, st->w, st->h, st->fps, st->bps);
    } else {
        snprintf(launch, sizeof launch,
                 "appsrc name=src480 is-live=true format=time ! "
                 "queue ! videoscale ! videorate ! "
                 "video/x-raw,format=NV12,width=%d,height=%d,framerate=%d/1 ! "
                 "mpph264enc bps=%d ! "
                 "appsink name=sink",
                 st->w, st->h, st->fps, st->bps);
    }

    GError *err = NULL;
    GstElement *pipe = gst_parse_launch(launch, &err);
    if (!pipe) {
        fprintf(stderr, "[producer] %s 管线构建失败: %s\n",
                st->path, err ? err->message : "未知错误");
        if (err) g_error_free(err);
        return NULL;
    }

    GstAppSink *sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipe), "sink"));
    if (!sink) {
        fprintf(stderr, "[producer] %s 找不到 appsink\n", st->path);
        return NULL;
    }
    static const GstAppSinkCallbacks callbacks = { .new_sample = on_new_sample };
    gst_app_sink_set_callbacks(sink, &callbacks, st, NULL);
    gst_app_sink_set_max_buffers(sink, 1);
    gst_app_sink_set_drop(sink, TRUE);
    st->sink = sink;

    if (st->raw_sink) {
        /* 画框源: 采集帧 → 拷贝画框 → 分发三条编码管线 */
        GstAppSink *draw = GST_APP_SINK(
            gst_bin_get_by_name(GST_BIN(pipe), "drawsink"));
        if (!draw) {
            fprintf(stderr, "[producer] %s 找不到 drawsink\n", st->path);
            return NULL;
        }
        gst_app_sink_set_callbacks(draw, &callbacks, st, NULL);
        gst_app_sink_set_max_buffers(draw, 1);
        gst_app_sink_set_drop(draw, TRUE);
        st->draw_sink = draw;

        /* stream1 编码管线: 画框帧 → videorate → mpph264enc (主码流带框) */
        char mlaunch[640];
        snprintf(mlaunch, sizeof mlaunch,
                 "appsrc name=main_src is-live=true format=time "
                 "caps=\"video/x-raw,format=NV12,width=%d,height=%d,"
                 "framerate=%d/1\" ! queue ! videorate ! "
                 "video/x-raw,format=NV12,framerate=%d/1 ! "
                 "mpph264enc bps=%d ! appsink name=sink",
                 st->w, st->h, st->fps, st->fps, st->bps);
        GError *merr = NULL;
        GstElement *mpipe = gst_parse_launch(mlaunch, &merr);
        if (!mpipe) {
            fprintf(stderr, "[producer] stream1 编码管线失败: %s\n",
                    merr ? merr->message : "?");
            return NULL;
        }
        GstElement *msrc = gst_bin_get_by_name(GST_BIN(mpipe), "main_src");
        GstAppSink *msink = GST_APP_SINK(
            gst_bin_get_by_name(GST_BIN(mpipe), "sink"));
        if (!msrc || !msink) {
            fprintf(stderr, "[producer] stream1 管线元素缺失\n");
            return NULL;
        }
        gst_app_sink_set_callbacks(msink, &callbacks, st, NULL);
        gst_app_sink_set_max_buffers(msink, 1);
        gst_app_sink_set_drop(msink, TRUE);
        st->sink = msink;                 /* NAL 出口 (feed /stream1) */
        st->main_push_src = msrc;
        gst_element_set_state(mpipe, GST_STATE_PLAYING);

        /* JPEG 预览管线: 画框帧 → 缩放 720x480 → CPU jpegenc */
        char jlaunch[640];
        snprintf(jlaunch, sizeof jlaunch,
                 "appsrc name=jpegsrc is-live=true format=time "
                 "caps=\"video/x-raw,format=NV12,width=%d,height=%d,"
                 "framerate=5/1\" ! queue ! videoscale ! "
                 "video/x-raw,format=NV12,width=720,height=480 ! "
                 "videoconvert ! video/x-raw,format=I420 ! "
                 "jpegenc ! appsink name=jpegsink",
                 st->w, st->h);
        GError *jerr = NULL;
        GstElement *jpipe = gst_parse_launch(jlaunch, &jerr);
        if (!jpipe) {
            fprintf(stderr, "[producer] JPEG 管线构建失败: %s\n",
                    jerr ? jerr->message : "?");
        } else {
            GstElement *jsrc = gst_bin_get_by_name(GST_BIN(jpipe), "jpegsrc");
            GstAppSink *jpeg = GST_APP_SINK(gst_bin_get_by_name(
                GST_BIN(jpipe), "jpegsink"));
            if (jsrc && jpeg) {
                gst_app_sink_set_callbacks(jpeg, &callbacks, st, NULL);
                gst_app_sink_set_max_buffers(jpeg, 1);
                gst_app_sink_set_drop(jpeg, TRUE);
                st->jpeg_push_src = jsrc;
                st->jpeg_sink = jpeg;
                gst_element_set_state(jpipe, GST_STATE_PLAYING);
            } else {
                fprintf(stderr, "[producer] JPEG 管线元素缺失\n");
            }
        }
    }

    if (!st->device && !st->raw_sink) {
        /* 480p 分支: 拿到 appsrc */
        GstElement *src = gst_bin_get_by_name(GST_BIN(pipe), "src480");
        if (!src) {
            fprintf(stderr, "[producer] %s 找不到 appsrc\n", st->path);
            return NULL;
        }
        st->push_src = src;
    }

    GstBus *bus = gst_element_get_bus(pipe);
    gst_bus_add_watch(bus, on_bus_message, NULL);

    if (gst_element_set_state(pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "[producer] %s 启动失败\n", st->path);
        return NULL;
    }
    printf("[producer] %s 启动: %s %dx%d@%dfps %dMbps\n",
           st->path, st->device ? st->device : "appsrc(CPU缩放)",
           st->w, st->h, st->fps, st->bps / 1000000);
    return pipe;
}

static void *accept_loop(void *arg) {
    rtsp_server_start((rtsp_server_t *)arg);
    return NULL;
}

int main(int argc, char *argv[]) {
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    setvbuf(stdout, NULL, _IONBF, 0);   /* nohup 日志即时可见 */
    gst_init(&argc, &argv);

    /* RTSP server + 三个挂载点 */
    g_srv = rtsp_server_create(port);
    if (!g_srv) {
        fprintf(stderr, "[producer] RTSP server 创建失败 (端口 %d 被占?)\n", port);
        return 1;
    }
    for (int i = 0; i < 3; i++) {
        g_st[i].path = g_profiles[i].path;
        g_st[i].mount = rtsp_server_add_mount(g_srv, g_st[i].path);
        if (!g_st[i].mount) {
            fprintf(stderr, "[producer] 挂载点 %s 注册失败\n", g_st[i].path);
            return 1;
        }
    }

    /* 三码流参数 (1080p@15: 降帧率给 MPP 编码器留调度余量,
     * 三合一并发 (producer+显示+预览) 下 20fps 会饿死 rkvenc2) */
    g_st[ST_MAIN].device = "/dev/video-camera0";
    g_st[ST_MAIN].w = 2688; g_st[ST_MAIN].h = 1520;
    g_st[ST_MAIN].bps = 5000000; g_st[ST_MAIN].fps = 15;

    g_st[ST_1080].device = "/dev/video32";
    g_st[ST_1080].w = 1920; g_st[ST_1080].h = 1080;
    g_st[ST_1080].bps = 2000000; g_st[ST_1080].fps = 15;
    g_st[ST_1080].raw_sink = (GstAppSink *)1;   /* 标记: 构建时创建 rawsink */

    g_st[ST_480].device = NULL;              /* 1080p 帧 CPU 缩放 */
    g_st[ST_480].w = 1280; g_st[ST_480].h = 720;
    g_st[ST_480].bps = 1500000; g_st[ST_480].fps = 30;

    for (int i = 0; i < 3; i++) {
        if (i == ST_MAIN) {          /* 实验: 主线禁用, 预算全给子线 */
            printf("[producer] 主线禁用 (实验: 只跑子线)\n");
            continue;
        }
        g_st[i].pipe = build_pipeline(&g_st[i]);
        if (!g_st[i].pipe)
            return 1;
        rtsp_server_set_frame_step(g_st[i].mount, 90000 / g_st[i].fps);
    }
    /* 480p 管线的 appsrc 接给 1080p 的原始帧出口 */
    g_st[ST_1080].push_src = g_st[ST_480].push_src;
    printf("[producer] 480p 分支已接: 1080p 原始帧 → videoscale → 编码\n");

    /* ONVIF: 三个 profile + MJPEG 预览源 */
    g_onvif = onvif_create(port);
    onvif_set_profiles(g_onvif, g_profiles, 3);
    onvif_set_jpeg_provider(g_onvif, jpeg_get_latest, NULL);
    onvif_set_ptz_callback(g_onvif, on_ptz_command, NULL);
    onvif_set_ptz_preset_callback(g_onvif, on_ptz_preset, NULL);
    if (onvif_start(g_onvif) != 0)
        fprintf(stderr, "[producer] ONVIF 启动失败 (摄像头管理将搜不到本机)\n");
    else
        printf("[producer] ONVIF 就绪: 3 profile\n");

    /* NPU 人形检测 (模型缺失时跳过, 不影响推流) */
    if (detect_init("/root/yolov8n_rv1126b_fp.rknn") != 0)
        printf("[producer] NPU 检测不可用 (模型未部署?), 跳过\n");

    printf("[producer] 拉流地址:\n"
           "  rtsp://<板子IP>:%d/stream0  (主码流 2K@15)\n"
           "  rtsp://<板子IP>:%d/stream1  (子码流 1080p@20)\n"
           "  rtsp://<板子IP>:%d/stream2  (子码流 480p@30, LCD 用)\n",
           port, port, port);

    pthread_t tid;
    pthread_create(&tid, NULL, accept_loop, g_srv);

    /* GLib 主循环: 让 gst_bus_add_watch 的管线错误/警告真正送达日志 */
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
