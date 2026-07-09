/*
 * main_gst.c — RV1126 GStreamer RTSP解码显示方案
 *
 * 板端已验证: gst-launch-1.0 ... ! fbdevsink device=/dev/fb0
 *
 * 管道结构:
 *   rtspsrc → rtph264depay → h264parse → mppvideodec → videoconvert → fbdevsink
 *
 * 优势:
 *   - 无需手写解码/拆帧/格式转换代码
 *   - GStreamer 自动管理 buffer 分配与内存池
 *   - MPP 硬件解码由 mppvideodec 内部处理
 *   - videoconvert 内部调用 RGA 硬件加速
 *   - fbdevsink 直接输出到 fbdev (/dev/fb0)
 *
 * 编译: ./build_gst.sh
 */

#include <gst/gst.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "log.h"

#define CONFIG_FILE "config.ini"

static GMainLoop *g_loop = NULL;
static GstElement *g_pipeline = NULL;
static int frame_count = 0;

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
static GstPadProbeReturn on_frame_probe(GstPad *pad, GstPadProbeInfo *info,
                                        gpointer data) {
    (void)pad;
    (void)info;
    (void)data;
    frame_count++;

    if (frame_count % 300 == 0) {
        GST_DEBUG("帧计数: %d", frame_count);
    }
    return GST_PAD_PROBE_OK;
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
 *
 * 管道: rtspsrc → rtph264depay → h264parse → mppvideodec → videoconvert →
 * fbdevsink
 *
 * 板端已验证: gst-launch-1.0 命令成功播放并显示到 MIPI 屏幕
 * 注意: 不要加 caps 限制, 否则会导致 not-negotiated 错误
 */
static GstElement *build_pipeline(const config_t *cfg) {
    GstElement *pipeline, *rtspsrc, *depay, *parse, *dec, *conv, *sink;

    pipeline = gst_pipeline_new("rv1126-display");

    /* 1. rtspsrc — RTSP 拉流 */
    rtspsrc = gst_element_factory_make("rtspsrc", "src");
    if (!rtspsrc) {
        LOGE("缺少 rtspsrc 插件");
        return NULL;
    }
    g_object_set(G_OBJECT(rtspsrc), "location", cfg->rtsp_url, "latency", 300,
                 "drop-on-latency", TRUE, NULL);
    /*
     * rtspsrc 默认先尝试 UDP，但防火墙通常会拦 UDP。
     * protocols 是 GFlags 属性，必须单独设置。
     * 值: GST_RTSP_LOWER_TRANS_TCP = 4 (见官方 GStreamer rtspsrc 文档)
     */
    g_object_set(G_OBJECT(rtspsrc), "protocols", 4, NULL);

    /* 2. rtph264depay — RTP 解包 */
    depay = gst_element_factory_make("rtph264depay", "depay");
    if (!depay) {
        LOGE("缺少 rtph264depay");
        return NULL;
    }

    /* 3. h264parse — H.264 解析 */
    parse = gst_element_factory_make("h264parse", "parse");
    if (!parse) {
        LOGE("缺少 h264parse");
        return NULL;
    }

    /* 4. mppvideodec — Rockchip MPP 硬件解码 */
    dec = gst_element_factory_make("mppvideodec", "dec");
    if (!dec) {
        LOGE("缺少 mppvideodec");
        return NULL;
    }

    /* 5. videoconvert — 颜色空间转换 (内部使用 RGA 加速) */
    conv = gst_element_factory_make("videoconvert", "conv");
    if (!conv) {
        LOGE("缺少 videoconvert");
        return NULL;
    }

    /* 6. fbdevsink — fbdev 显示输出到 /dev/fb0 */
    sink = gst_element_factory_make("fbdevsink", "sink");
    if (!sink) {
        LOGE("缺少 fbdevsink");
        return NULL;
    }
    g_object_set(G_OBJECT(sink), "device", "/dev/fb0", NULL);

    /* 组装管道 */
    gst_bin_add_many(GST_BIN(pipeline), rtspsrc, depay, parse, dec, conv, sink,
                     NULL);
    gst_element_link_many(depay, parse, dec, conv, sink, NULL);

    /* rtspsrc 的 src pad 是动态创建的 */
    g_signal_connect(rtspsrc, "pad-added", G_CALLBACK(on_pad_added), depay);

    /* 帧计数器 probe（可选） */
    GstPad *dec_src = gst_element_get_static_pad(dec, "src");
    if (dec_src) {
        gst_pad_add_probe(dec_src, GST_PAD_PROBE_TYPE_BUFFER, on_frame_probe,
                          NULL, NULL);
        gst_object_unref(dec_src);
    }

    return pipeline;
}

int main(int argc, char *argv[]) {
    const char *cfg_path = CONFIG_FILE;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("RV1126 GStreamer RTSP 解码显示 Demo\n\n");
            printf("用法: %s [config.ini]\n\n", argv[0]);
            printf("管道: rtspsrc → rtph264depay → h264parse → mppvideodec "
                   "→ videoconvert → fbdevsink → 屏幕\n");
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
    LOGI("管道: rtspsrc → h264depay → h264parse → mppvideodec → videoconvert → "
         "fbdevsink");

    /* 初始化 GStreamer */
    gst_init(&argc, &argv);

    /* 注册信号处理 */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

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
        if (lf)
            fclose(lf);
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

    LOGI("退出. 总帧≈%d", frame_count);
    if (lf)
        fclose(lf);
    return 0;
}
