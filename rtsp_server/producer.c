/* producer.c — 板子生产者主程序 (双码流)
 *
 * 数据流:
 *   主码流:  v4l2src(/dev/video-camera0, ISP mainpath 2688x1520)
 *            → videorate(15fps) → mpph264enc → /stream0   (2K, 5Mbps)
 *   子码流:  v4l2src(/dev/video32, ISP selfpath 1920x1080)
 *            → videorate(10fps) → mpph264enc → /stream1   (1080p, 2Mbps)
 *
 * 说明: RV1126 MPP 编码器多会话调度不保证公平, 三码流实测间歇饥饿;
 *       双码流 (大厂低端芯片标准做法) 下两路稳定。
 *
 * 用法: ./producer [端口]
 * 客户端: rtsp://<板子IP>:8554/stream0 (主码流 2K)
 *         rtsp://<板子IP>:8554/stream1 (子码流 1080p)
 *
 * 编译: make producer  (见 Makefile, 需要 GStreamer 交叉编译头)
 */
#define _POSIX_C_SOURCE 200809L

#include "nal.h"
#include "onvif.h"
#include "rtsp_server.h"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_PORT 8554

/* ---------------- 双码流定义 ---------------- */

typedef struct {
    const char *path;        /* RTSP 挂载路径 */
    const char *device;      /* 采集设备 */
    int w, h, bps, fps;
    rtsp_mount_t *mount;
    GstElement *pipe;
    GstAppSink *sink;
    unsigned long feed_count;
} stream_ctx_t;

static rtsp_server_t *g_srv = NULL;
static onvif_server_t *g_onvif = NULL;
static stream_ctx_t g_st[2];

#define ST_MAIN  0   /* /stream0  2688x1520 主码流 */
#define ST_1080  1   /* /stream1  1920x1080 子码流 */

static const onvif_profile_t g_profiles[2] = {
    { "MainStream", "主码流",    2688, 1520, "/stream0" },
    { "Sub1080",    "子码流1080", 1920, 1080, "/stream1" },
};

/* ---------------- appsink 回调 ---------------- */

static GstFlowReturn on_new_sample(GstAppSink *appsink, gpointer user_data) {
    stream_ctx_t *st = user_data;
    (void)appsink;

    GstSample *sample = gst_app_sink_pull_sample(st->sink);
    if (!sample) return GST_FLOW_OK;

    GstBuffer *buf = gst_sample_get_buffer(sample);
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
    char launch[512];
    snprintf(launch, sizeof launch,
             "v4l2src device=%s ! "
             "video/x-raw,format=NV12,width=%d,height=%d,framerate=30/1 ! "
             "videorate ! "
             "video/x-raw,format=NV12,framerate=%d/1 ! "
             "mpph264enc bps=%d ! "
             "appsink name=sink",
             st->device, st->w, st->h, st->fps, st->bps);

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

    GstBus *bus = gst_element_get_bus(pipe);
    gst_bus_add_watch(bus, on_bus_message, NULL);

    if (gst_element_set_state(pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "[producer] %s 启动失败\n", st->path);
        return NULL;
    }
    printf("[producer] %s 启动: %s %dx%d@%dfps %dMbps\n",
           st->path, st->device, st->w, st->h, st->fps, st->bps / 1000000);
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

    /* RTSP server + 两个挂载点 */
    g_srv = rtsp_server_create(port);
    if (!g_srv) {
        fprintf(stderr, "[producer] RTSP server 创建失败 (端口 %d 被占?)\n", port);
        return 1;
    }
    for (int i = 0; i < 2; i++) {
        g_st[i].path = g_profiles[i].path;
        g_st[i].mount = rtsp_server_add_mount(g_srv, g_st[i].path);
        rtsp_server_set_frame_step(g_st[i].mount, 90000 / g_st[i].fps);
        if (!g_st[i].mount) {
            fprintf(stderr, "[producer] 挂载点 %s 注册失败\n", g_st[i].path);
            return 1;
        }
    }

    /* 双码流参数 */
    g_st[ST_MAIN].device = "/dev/video-camera0";
    g_st[ST_MAIN].w = 2688; g_st[ST_MAIN].h = 1520;
    g_st[ST_MAIN].bps = 5000000; g_st[ST_MAIN].fps = 15;

    g_st[ST_1080].device = "/dev/video32";
    g_st[ST_1080].w = 1920; g_st[ST_1080].h = 1080;
    g_st[ST_1080].bps = 2000000; g_st[ST_1080].fps = 10;

    for (int i = 0; i < 2; i++) {
        g_st[i].pipe = build_pipeline(&g_st[i]);
        if (!g_st[i].pipe)
            return 1;
    }

    /* ONVIF: 两个 profile */
    g_onvif = onvif_create(port);
    onvif_set_profiles(g_onvif, g_profiles, 2);
    if (onvif_start(g_onvif) != 0)
        fprintf(stderr, "[producer] ONVIF 启动失败 (摄像头管理将搜不到本机)\n");
    else
        printf("[producer] ONVIF 就绪: 2 profile (主码流/子码流1080)\n");

    printf("[producer] 拉流地址:\n"
           "  rtsp://<板子IP>:%d/stream0  (主码流 2K@15)\n"
           "  rtsp://<板子IP>:%d/stream1  (子码流 1080p@10)\n",
           port, port);

    pthread_t tid;
    pthread_create(&tid, NULL, accept_loop, g_srv);

    /* GLib 主循环: 让 gst_bus_add_watch 的管线错误/警告真正送达日志 */
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
