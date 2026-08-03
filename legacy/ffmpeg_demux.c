/*
 * ❌ 废弃 — FFmpeg C API RTSP 解复用器 (仅 main_native.c 使用)
 *
 * 问题: 实测 RTSP 拉流帧率过低 (~1fps), 不如 GStreamer rtspsrc。
 * 正式方案使用 GStreamer 管线拉流, 不经过本文件。
 */

#include "ffmpeg_demux.h"
#include "log.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <stdlib.h>
#include <string.h>


#define RTSP_TIMEOUT_MS 3000

ffmpeg_demux_t *ffmpeg_demux_open(const config_t *c) {
    static int registered = 0;
    if (!registered) {
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 0, 0)
        av_register_all();
#endif
        avformat_network_init();
        registered = 1;
    }

    ffmpeg_demux_t *d = (ffmpeg_demux_t *)calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->video_idx = -1;

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "rtsp_transport", c->rtsp_transport ? "tcp" : "udp", 0);

    /* 低延迟选项: 减小探测/缓冲, 启动快 + 实时性好 */
    av_dict_set(&opts, "stimeout", "3000000", 0);         /* 3 秒超时 */
    av_dict_set(&opts, "probesize", "32768", 0);           /* 32KB 探测 */
    av_dict_set(&opts, "max_delay", "500000", 0);          /* 500ms 最大缓冲 */
    av_dict_set(&opts, "analyzeduration", "1000000", 0);   /* 1 秒分析 */
    av_dict_set(&opts, "fflags", "nobuffer", 0);           /* 不缓冲 */

    AVFormatContext *fmt = NULL;
    int ret = avformat_open_input(&fmt, c->rtsp_url, NULL, &opts);

    if (ret < 0) {
        char eb[256];
        av_strerror(ret, eb, sizeof(eb));
        LOGW("avformat_open_input: %s, TCP 回退", eb);
        AVDictionary *retry = NULL;
        av_dict_set(&retry, "rtsp_transport", "tcp", 0);
        av_dict_set(&retry, "stimeout", "3000000", 0);
        av_dict_set(&retry, "probesize", "32768", 0);
        av_dict_set(&retry, "max_delay", "500000", 0);
        av_dict_set(&retry, "analyzeduration", "1000000", 0);
        av_dict_set(&retry, "fflags", "nobuffer", 0);
        ret = avformat_open_input(&fmt, c->rtsp_url, NULL, &retry);
        av_dict_free(&retry);
    }
    av_dict_free(&opts);

    if (ret < 0) {
        char eb[256];
        av_strerror(ret, eb, sizeof(eb));
        LOGE("avformat_open_input 失败: %s", eb);
        free(d);
        return NULL;
    }

    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0) {
        LOGW("avformat_find_stream_info 失败, 继续尝试...");
    }

    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            d->video_idx = (int)i;
            break;
        }
    if (d->video_idx < 0) {
        LOGE("未找到视频流");
        avformat_close_input(&fmt);
        free(d);
        return NULL;
    }

    AVCodecParameters *cp = fmt->streams[d->video_idx]->codecpar;
    d->width = cp->width;
    d->height = cp->height;
    d->codec_id = cp->codec_id;
    if (cp->extradata && cp->extradata_size > 0 &&
        cp->extradata_size <= (int)sizeof(d->sps_pps)) {
        memcpy(d->sps_pps, cp->extradata, cp->extradata_size);
        d->sps_pps_len = cp->extradata_size;
    }
    d->fmt_ctx = fmt;

    LOGI("ffmpeg_demux: %dx%d %s (extradata=%d bytes)", cp->width, cp->height,
         cp->codec_id == AV_CODEC_ID_H264 ? "H264" : "HEVC",
         cp->extradata_size);
    return d;
}

int ffmpeg_demux_read(ffmpeg_demux_t *d, uint8_t **data) {
    AVFormatContext *fmt = (AVFormatContext *)d->fmt_ctx;
    if (!fmt)
        return -1;

    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return -1;

    while (1) {
        int ret = av_read_frame(fmt, pkt);
        if (ret < 0) {
            char eb[256];
            av_strerror(ret, eb, sizeof(eb));
            LOGW("av_read_frame: %s", eb);
            av_packet_free(&pkt);
            return ret;
        }
        if (pkt->stream_index == d->video_idx) {
            if (pkt->size > d->pkt_cap) {
                d->pkt_cap = pkt->size + 4096;
                uint8_t *nb = (uint8_t *)realloc(d->pkt_buf, d->pkt_cap);
                if (!nb) {
                    av_packet_free(&pkt);
                    return -1;
                }
                d->pkt_buf = nb;
            }
            memcpy(d->pkt_buf, pkt->data, pkt->size);
            int size = pkt->size;
            av_packet_free(&pkt);
            *data = d->pkt_buf;
            return size;
        }
        av_packet_unref(pkt);
    }
}

void ffmpeg_demux_close(ffmpeg_demux_t *d) {
    if (!d)
        return;
    if (d->fmt_ctx)
        avformat_close_input((AVFormatContext **)&d->fmt_ctx);
    free(d->pkt_buf);
    free(d);
}
