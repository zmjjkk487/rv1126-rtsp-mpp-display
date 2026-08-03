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
    MppFrame _pending;    /* put 腾队列时收下的暂存帧, 下次调用优先返回 */
    MppFrame _last_frame; /* 未释放的上一帧 (调用 mpp_dec_return 前) */
    uint8_t *y_base;      /* Y 平面指针 */
    uint8_t *uv_base;     /* UV 平面指针 */
    int width;
    int height;
    int hor_stride;
    int ver_stride;
    int info_ready;
} mpp_dec_t;

/*
 * 注意: 函数名不能以 mpp_dec_ 开头! 正点原子 legacy 版 librockchip_mpp.so
 * 导出了 mpp_dec_init / mpp_dec_deinit / mpp_dec_decode 等全局符号,
 * 同名会导致 ELF 符号插入 (symbol interposition): MPP 内部 mpp_ctx_init
 * 调用 mpp_dec_init 时会被劫持到我们的函数 → 无限递归 → 栈溢出段错误。
 * 因此这里统一使用 mpphw_ 前缀。参见 docs/dev-log.md "符号冲突" 一节。
 */
int mpphw_dec_init(mpp_dec_t *dec, int width, int height);
void mpphw_dec_deinit(mpp_dec_t *dec);

/*
 * 提交 H.264 Annex-B 帧数据到 MPP 解码器。
 * 返回: 1 = 产出帧 (y/uv/w/h/hstride/vstride 有效), 0 = 暂无帧, <0 = 错误。
 * y/uv 指向 MPP 内部 buffer，在调用 mpp_dec_return() 前一直有效。
 * hstride: hor_stride (Y 平面行步长, 字节, 可能 > w)
 * vstride: ver_stride (垂直 stride, 行数, 16 对齐后可能 > h)
 *          —— 两者必须传给 RGA, 否则 1080p 会 UV 偏移错位
 */
int mpphw_dec_decode(mpp_dec_t *dec, const uint8_t *data, int len, uint8_t **y,
                     uint8_t **uv, int *w, int *h, int *hstride, int *vstride);

/* 释放上一帧 (每次显示后必须调用，否则 buffer 泄漏) */
int mpphw_dec_return(mpp_dec_t *dec);

#endif
