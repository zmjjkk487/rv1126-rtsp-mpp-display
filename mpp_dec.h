#ifndef MPP_DEC_H
#define MPP_DEC_H

#include "mpp_buffer.h"
#include "rk_mpi.h"
#include <stdint.h>

typedef struct {
    MppCtx ctx;             /* MPP 解码器上下文 */
    MppApi *mpi;            /* MPP API 函数表 */
    MppPacket packet;       /* 复用: 每帧复用，不反复 init/deinit */
    MppBufferGroup frm_grp; /* 输出帧 buffer group (info_change 时创建) */
    MppFrame _last_frame; /* 未释放的上一帧 (调用 mpp_dec_return 前) */
    uint8_t *y_base;      /* Y 平面指针 */
    uint8_t *uv_base;     /* UV 平面指针 */
    int width;
    int height;
    int hor_stride;
    int ver_stride;
    int info_ready;
} mpp_dec_t;

int mpp_dec_init(mpp_dec_t *dec, int width, int height);
void mpp_dec_deinit(mpp_dec_t *dec);

/*
 * 提交 H.264 Annex-B 帧数据到 MPP 解码器。
 * 返回: 1 = 产出帧 (y/uv/w/h/stride 有效), 0 = 暂无帧, <0 = 错误。
 * y/uv 指向 MPP 内部 buffer，在调用 mpp_dec_return() 前一直有效。
 */
int mpp_dec_decode(mpp_dec_t *dec, const uint8_t *data, int len, uint8_t **y,
                   uint8_t **uv, int *w, int *h, int *stride);

/* 释放上一帧 (每次显示后必须调用，否则 buffer 泄漏) */
int mpp_dec_return(mpp_dec_t *dec);

#endif
