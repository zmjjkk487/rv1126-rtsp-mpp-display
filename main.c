/*
 * main_gst.c — RV1126 GStreamer RTSP解码 + RGA硬件转换 + fbdev显示
 *
 * 管道:
 *   rtspsrc → rtph264depay → h264parse → mppvideodec → appsink
 *                                                    (NV12 输出)
 *
 * appsink 回调取 NV12 帧 → fb_show_nv12 (RGA 硬转 + fbdev 直写)
 * 优势: MPP 硬解 + RGA 硬转 = 全硬件链路, CPU ~1%
 */

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "fbdev.h"
#include "log.h"

#define CONFIG_FILE "config.ini"

static GMainLoop *g_loop = NULL;
static GstElement *g_pipeline = NULL;
static int frame_count = 0;
static fb_t g_fb;

/* GStreamer bus 消息回调 */
static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus;
    (void)data;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
        LOGI("GStreamer: 流结束 (EOS)");
        g_main_loop_quit(g_loop);
        break;
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        LOGE("GStreamer 错误: %s", err->message);
        if (dbg)
            LOGD("调试: %s", dbg);
        g_error_free(err);
        g_free(dbg);
        g_main_loop_quit(g_loop);
        break;
    }
    case GST_MESSAGE_WARNING: {
        GError *warn = NULL;
        gchar *dbg = NULL;
        gst_message_parse_warning(msg, &warn, &dbg);
        LOGW("GStreamer 警告: %s", warn->message);
        g_error_free(warn);
        g_free(dbg);
        break;
    }
    case GST_MESSAGE_STATE_CHANGED:
        /* 静默忽略状态变更消息 */
        break;
    case GST_MESSAGE_QOS:
        /* 静默忽略 QoS 消息 */
        break;
    case GST_MESSAGE_STREAM_START:
        LOGI("GStreamer: 流开始");
        break;
    default:
        break;
    }
    return TRUE;
}

/* pad-added 回调: 将 rtspsrc 动态创建的 H264 视频 pad 链接到 depay */
static void on_pad_added(GstElement *src, GstPad *pad, gpointer data) {
    (void)src;
    GstElement *depay = (GstElement *)data;
    GstCaps *caps;
    GstStructure *s;

    /* 检查 pad 的类型，只链接 H264 视频流 */
    caps = gst_pad_get_current_caps(pad);
    if (!caps)
        caps = gst_pad_query_caps(pad, NULL);
    if (!caps)
        return;

    s = gst_caps_get_structure(caps, 0);
    if (s) {
        const char *name = gst_structure_get_name(s);
        if (!name || !g_str_has_prefix(name, "application/x-rtp")) {
            /* 不是 RTP 流，忽略 */
            gst_caps_unref(caps);
            return;
        }
    }
    gst_caps_unref(caps);

    GstPad *sinkpad = gst_element_get_static_pad(depay, "sink");
    if (sinkpad) {
        if (gst_pad_link(pad, sinkpad) != GST_PAD_LINK_OK) {
            LOGE("pad 链接失败");
        }
        gst_object_unref(sinkpad);
    }
}
/* appsink 回调: 收到 NV12 帧 → RGA 硬转 + fbdev 直写 */
static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer data) {
    (void)data;
    (void)sink;

    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample)
        return GST_FLOW_OK;

    GstBuffer *buf = gst_sample_get_buffer(sample);
    if (!buf) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstCaps *caps = gst_sample_get_caps(sample);
    if (!caps) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    /* 获取逻辑帧参数 (width/height) */
    GstVideoInfo vinfo;
    gst_video_info_init(&vinfo);
    if (!gst_video_info_from_caps(&vinfo, caps)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    int w = vinfo.width;
    int h = vinfo.height;

    /* 获取 dmabuf 真实 layout: GstVideoMeta 有 buffer 级 stride/offset */
    GstVideoMeta *meta = gst_buffer_get_video_meta(buf);
    int hs = 0, vs = h; /* hor_stride, ver_stride */
    gsize y_off = 0, uv_off = 0;

    if (meta) {
        hs = meta->stride[0];
        y_off = meta->offset[0];
        uv_off = meta->offset[1];
        if (hs > 0)
            vs = (int)((uv_off - y_off) / hs);
    } else {
        /* 回退: 手动 map GstVideoFrame */
        GstVideoFrame vf;
        if (!gst_video_frame_map(&vf, &vinfo, buf, GST_MAP_READ)) {
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }
        hs = GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 0);
        w = GST_VIDEO_FRAME_WIDTH(&vf);
        h = GST_VIDEO_FRAME_HEIGHT(&vf);
        const uint8_t *py = (const uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(&vf, 0);
        const uint8_t *puv = (const uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(&vf, 1);
        if (py && puv && hs > 0)
            vs = (int)((puv - py) / hs);
        y_off = uv_off = 0;
        gst_video_frame_unmap(&vf);
    }

    if (hs <= 0) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    /* 直接 map buffer, 用真实 offset 定位 Y/UV 平面 */
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    const uint8_t *y = map.data + y_off;
    const uint8_t *uv = map.data + uv_off;

    fb_show_nv12(&g_fb, y, uv, w, h, hs, vs);

    gst_buffer_unmap(buf, &map);
    gst_sample_unref(sample);

    frame_count++;
    if (frame_count % 300 == 0) {
        LOGI("帧=%d", frame_count);
    }
    return GST_FLOW_OK;
}

/* 信号处理 */
static void on_signal(int sig) {
    (void)sig;
    LOGI("收到退出信号, 正在停止...");
    if (g_loop)
        g_main_loop_quit(g_loop);
}

/**
 * build_pipeline — 构建 GStreamer 管道
 * 管道: rtspsrc → rtph264depay → h264parse → mppvideodec → appsink
 * appsink 输出 NV12 → 回调 on_new_sample → RGA 硬转 + fbdev 直写
 */
static GstElement *build_pipeline(const config_t *cfg) {
    GstElement *pipeline, *rtspsrc, *depay, *parse, *dec, *appsink;

    pipeline = gst_pipeline_new("rv1126-display");

    /* 1. rtspsrc */
    rtspsrc = gst_element_factory_make("rtspsrc", "src");
    if (!rtspsrc) {
        LOGE("缺少 rtspsrc");
        return NULL;
    }
    g_object_set(G_OBJECT(rtspsrc), "location", cfg->rtsp_url, "latency", 300,
                 "drop-on-latency", TRUE, "protocols", 4, NULL);

    /* 2 — 4. depay → parse → h264decode */
    depay = gst_element_factory_make("rtph264depay", "depay");
    parse = gst_element_factory_make("h264parse", "parse");
    dec = gst_element_factory_make("mppvideodec", "dec");
    if (!depay || !parse || !dec) {
        LOGE("缺少 GStreamer 插件");
        return NULL;
    }

    /* 5. appsink — 输出 NV12 */
    appsink = gst_element_factory_make("appsink", "sink");
    if (!appsink) {
        LOGE("缺少 appsink");
        return NULL;
    }
    g_object_set(G_OBJECT(appsink), "emit-signals", TRUE, "sync", FALSE,
                 "max-buffers", 2, "drop", TRUE, NULL);
    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), NULL);

    /* 组装 */
    gst_bin_add_many(GST_BIN(pipeline), rtspsrc, depay, parse, dec, appsink,
                     NULL);
    gst_element_link_many(depay, parse, dec, appsink, NULL);
    g_signal_connect(rtspsrc, "pad-added", G_CALLBACK(on_pad_added), depay);

    return pipeline;
}

int main(int argc, char *argv[]) {
    const char *cfg_path = CONFIG_FILE;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("RV1126 GStreamer RTSP 解码显示 Demo\n\n");
            printf("用法: %s [config.ini]\n\n", argv[0]);
            printf("管道: rtspsrc → rtph264depay → h264parse → mppvideodec "
                   "→ appsink → RGA(NV12→RGB) → fbdev\n");
            return 0;
        } else if (argv[i][0] != '-') {
            cfg_path = argv[i];
        }
    }

    config_t cfg;
    config_parse(cfg_path, &cfg);
    log_set_level(cfg.log_level);

    FILE *lf = fopen("/var/log/rv1126_gst.log", "a");
    log_set_file(lf);
    setbuf(stderr, NULL);

    LOGI("========================================");
    LOGI("RV1126 GStreamer RTSP 解码显示 Demo");
    LOGI("========================================");
    LOGI("RTSP: %s", cfg.rtsp_url);
    LOGI("管道: rtspsrc → depay → parse → mppvideodec → appsink(NV12) → RGA → fbdev");

    /* 初始化 GStreamer */
    gst_init(&argc, &argv);

    /* 注册信号处理 */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* 初始化显示设备 (RGA + fbdev) */
    LOGI("初始化 fbdev (%s)...", cfg.fb_device);
    if (fb_init(&g_fb, cfg.fb_device) < 0) {
        if (lf) fclose(lf);
        return 1;
    }

    /* 构建管道 */
    g_pipeline = build_pipeline(&cfg);
    if (!g_pipeline) {
        LOGE("管道构建失败");
        if (lf)
            fclose(lf);
        return 1;
    }

    /* 监听 bus 消息 */
    GstBus *bus = gst_element_get_bus(g_pipeline);
    guint watch_id = gst_bus_add_watch(bus, on_bus_message, NULL);
    gst_object_unref(bus);

    /* 启动管道 */
    LOGI("启动 GStreamer 管道...");
    GstStateChangeReturn ret =
        gst_element_set_state(g_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOGE("管道启动失败");
        gst_object_unref(g_pipeline);
        g_source_remove(watch_id);
        fb_deinit(&g_fb);
        if (lf) fclose(lf);
        return 1;
    }

    LOGI("管道运行中, Ctrl+C 退出");
    g_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(g_loop);

    /* 清理 */
    LOGI("停止管道...");
    gst_element_set_state(g_pipeline, GST_STATE_NULL);
    g_source_remove(watch_id);
    g_main_loop_unref(g_loop);
    gst_object_unref(g_pipeline);
    fb_deinit(&g_fb);

    LOGI("退出. 总帧≈%d", frame_count);
    if (lf)
        fclose(lf);
    return 0;
}
