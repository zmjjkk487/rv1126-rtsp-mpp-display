#ifndef FFMPEG_DEMUX_H
#define FFMPEG_DEMUX_H

#include "config.h"
#include <stdint.h>

typedef struct {
    void *fmt_ctx; /* AVFormatContext* */
    int video_idx;
    int width, height;
    uint8_t sps_pps[512]; /* extradata (AVCC 格式 SPS+PPS) */
    int sps_pps_len;
    int codec_id;     /* AV_CODEC_ID_H264 / AV_CODEC_ID_HEVC */
    uint8_t *pkt_buf; /* 帧数据缓冲 */
    int pkt_cap;
} ffmpeg_demux_t;

/* 打开 RTSP 流，返回 demux 句柄，失败返回 NULL */
ffmpeg_demux_t *ffmpeg_demux_open(const config_t *c);

/* 读取一帧完整 H.264 Annex-B 数据。
 * 返回: >0 帧大小，0 EOF，<0 错误。
 * *data 指向内部缓冲区，调用方不需要释放。 */
int ffmpeg_demux_read(ffmpeg_demux_t *d, uint8_t **data);

/* 关闭并释放资源 */
void ffmpeg_demux_close(ffmpeg_demux_t *d);

#endif
