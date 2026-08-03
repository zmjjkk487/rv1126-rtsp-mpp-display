/*
 * mpp_dec.c — Rockchip MPP H.264 硬解码封装
 *
 * 参考: rockchip-linux/mpp/test/mpi_dec_test.c (官方 demo, dec_simple 模式)
 *
 * 解码流程:
 *   1. mpp_create → mpp_init(MPP_CTX_DEC, MPP_VIDEO_CodingAVC)
 *   2. split_parse=1: MPP 内部处理 NAL 拆分
 *   3. 设置输入超时 + 码流缓冲属性
 *   4. 循环: decode_put_packet(帧数据) → decode_get_frame(NV12)
 *   5. info_change 时: 申请 buffer group → SET_EXT_BUF_GROUP →
 * SET_INFO_CHANGE_READY
 */

#include "mpp_dec.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mpp_buffer.h"
#include "rk_mpi.h"

#define MAX_RETRY_PKT 20 /* put_packet 重试次数（增大以容忍瞬时繁忙） */
#define MAX_RETRY_FRM 30
#define INPUT_TIMEOUT_MS 500 /* 输入超时 500ms */

int mpphw_dec_init(mpp_dec_t *dec, int width, int height) {
    memset(dec, 0, sizeof(*dec));

    if (width <= 0)
        width = 1920;
    if (height <= 0)
        height = 1080;
    dec->width = width;
    dec->height = height;

    MppApi *mpi = NULL;
    MppCtx ctx = NULL;

    MPP_RET ret = mpp_create(&ctx, &mpi);
    if (ret) {
        LOGE("mpp_create ret=%d", ret);
        return -1;
    }

    /*
     * split_parse=1 必须在 mpp_init 之前设置 (官方 rk_mpi_cmd.h:
     * "Need to setup before init")。用 control 直接设, 不走 GET_CFG
     * /SET_CFG (alientek 定制版 GET_CFG 后状态异常)。
     */
    ret = mpi->control(ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &(RK_U32){1});
    if (ret)
        LOGW("SET_PARSER_SPLIT_MODE ret=%d", ret);

    ret = mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);
    if (ret) {
        LOGE("mpp_init ret=%d", ret);
        goto fail_ctx;
    }

    /* 创建可复用的 MppPacket（官方 demo: mpp_packet_init(&packet, NULL, 0)） */
    ret = mpp_packet_init(&dec->packet, NULL, 0);
    if (ret) {
        LOGE("mpp_packet_init ret=%d", ret);
        goto fail_ctx;
    }

    /*
     * split_parse 已在 mpp_init 之前通过 MPP_DEC_SET_PARSER_SPLIT_MODE 设置,
     * 此处无需再调 GET_CFG/SET_CFG。
     */

    /*
     * 注意: MPP_SET_INPUT_TIMEOUT 在正点原子 SDK MPP (alientek 2026-03-02)
     * 上可能导致段错误，已禁用。如需启用，确认 MPP 版本支持此控制命令。
     * 官方 rk_mpi_cmd.h: MPP_SET_INPUT_TIMEOUT, parameter type RK_S64
     */

    dec->ctx = ctx;
    dec->mpi = mpi;

    LOGI("MPP 解码器初始化完成: %dx%d", width, height);
    return 0;

    mpp_packet_deinit(&dec->packet);
    dec->packet = NULL;
fail_ctx:
    mpp_destroy(ctx);
    return -1;
}

void mpphw_dec_deinit(mpp_dec_t *dec) {
    if (!dec)
        return;

    /* 先释放上次未释放的帧 (含 pending 暂存帧) */
    if (dec->_last_frame) {
        mpp_frame_deinit(&dec->_last_frame);
        dec->_last_frame = NULL;
    }
    if (dec->_pending) {
        mpp_frame_deinit(&dec->_pending);
        dec->_pending = NULL;
    }

    if (dec->ctx && dec->mpi) {
        mpp_destroy(dec->ctx);
    }

    if (dec->packet) {
        mpp_packet_deinit(&dec->packet);
        dec->packet = NULL;
    }

    if (dec->frm_grp) {
        mpp_buffer_group_put(dec->frm_grp);
        dec->frm_grp = NULL;
    }

    dec->ctx = NULL;
    dec->mpi = NULL;
}

/*
 * 处理 info_change: MPP 通知分辨率变化，需要准备输出 buffer group
 * 参考官方 demo: MPP_DEC_SET_EXT_BUF_GROUP → MPP_DEC_SET_INFO_CHANGE_READY
 */
static int handle_info_change(mpp_dec_t *dec, MppFrame frame) {
    MppApi *mpi = (MppApi *)dec->mpi;

    RK_U32 width = mpp_frame_get_width(frame);
    RK_U32 height = mpp_frame_get_height(frame);
    RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
    RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
    RK_U32 buf_size = mpp_frame_get_buf_size(frame);

    dec->width = (int)width;
    dec->height = (int)height;
    /* info_change 的 stride 仅作 buffer 分配参考, 实际 stride
     * 在 process_frame 中从每帧 MppFrame 读取 (alientek MPP
     * 的 info_change 可能报告 1920 但硬件实际输出 1984) */
    dec->hor_stride = (int)hor_stride;
    dec->ver_stride = (int)ver_stride;

    LOGI("分辨率变更: %dx%d stride=%dx%d buf_size=%d", width, height,
         hor_stride, ver_stride, buf_size);

    if (dec->frm_grp) {
        mpp_buffer_group_put(dec->frm_grp);
        dec->frm_grp = NULL;
    }

    /* 半内部分配 (官方模式二): 创建 buffer group, 用 info_change
     * 的 buf_size, stride 在帧级校正. buf_size 偏大无害 (实际帧
     * 放得下), mismatch 警告不影响解码. */
    MppBufferGroup grp = NULL;
    MPP_RET ret = mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DRM);
    if (ret)
        ret = mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_ION);
    if (ret) {
        LOGE("mpp_buffer_group_get_internal ret=%d", ret);
        goto ready;
    }

    ret = mpp_buffer_group_limit_config(grp, buf_size, 24);
    if (ret) {
        LOGE("mpp_buffer_group_limit_config ret=%d", ret);
        mpp_buffer_group_put(grp);
        goto ready;
    }

    ret = mpi->control(dec->ctx, MPP_DEC_SET_EXT_BUF_GROUP, grp);
    if (ret) {
        LOGE("MPP_DEC_SET_EXT_BUF_GROUP ret=%d", ret);
        mpp_buffer_group_put(grp);
        goto ready;
    }

    dec->frm_grp = grp;
    LOGI("buffer group: buf_size=%d count=24 OK", buf_size);

ready:
    ret = mpi->control(dec->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
    if (ret) {
        LOGE("info change ready ret=%d", ret);
        return -1;
    }
    dec->info_ready = 1;
    return 0;
}

/* 非阻塞收一帧: info_change 就地处理, 有效帧暂存 pending, 其余释放。
 * 维持输入/输出队列流动的关键 (官方文档: MPP 输入队列仅 4 包)。 */
static void drain_one(mpp_dec_t *dec) {
    MppApi *mpi = (MppApi *)dec->mpi;
    MppFrame f = NULL;
    if (mpi->decode_get_frame(dec->ctx, &f) != MPP_OK || !f)
        return;
    if (mpp_frame_get_info_change(f)) {
        handle_info_change(dec, f);
        mpp_frame_deinit(&f);
    } else if (!mpp_frame_get_errinfo(f) && !mpp_frame_get_discard(f) &&
               mpp_frame_get_buffer(f)) {
        /* 先归还旧 pending, 否则 buffer group 耗尽 → 解码 stall → 画面定格 */
        if (dec->_pending)
            mpp_frame_deinit(&dec->_pending);
        dec->_pending = f;
    } else {
        mpp_frame_deinit(&f);
    }
}

/* 处理一帧: 1=产出帧(输出参数有效), 0=帧已处理/丢弃, -1=错误 */
static int process_frame(mpp_dec_t *dec, MppFrame frame, uint8_t **y,
                         uint8_t **uv, int *w, int *h, int *hstride,
                         int *vstride) {
    if (mpp_frame_get_info_change(frame)) {
        handle_info_change(dec, frame);
        mpp_frame_deinit(&frame);
        return 0;
    }

    if (mpp_frame_get_errinfo(frame) || mpp_frame_get_discard(frame)) {
        mpp_frame_deinit(&frame);
        return 0;
    }

    MppBuffer buf = mpp_frame_get_buffer(frame);
    if (!buf) {
        mpp_frame_deinit(&frame);
        return 0;
    }

    uint8_t *base = (uint8_t *)mpp_buffer_get_ptr(buf);

    /*
     * 从实际帧读真实 stride, 不能用 info_change 时存的旧值.
     * alientek 定制 MPP 的 info_change 帧可能报告 stride=1920,
     * 但硬件实际输出 stride=1984 (vdpu384a 64 对齐加 padding),
     * 拿错值会导致 UV 偏移全错 → 画面满屏色块/花屏。
     */
    RK_U32 real_hs = mpp_frame_get_hor_stride(frame);
    RK_U32 real_vs = mpp_frame_get_ver_stride(frame);
    if (real_hs > 0) dec->hor_stride = (int)real_hs;
    if (real_vs > 0) dec->ver_stride = (int)real_vs;

    /* NV12: Y plane + interleaved UV plane */
    dec->y_base = base;
    dec->uv_base = base + dec->hor_stride * dec->ver_stride;

    *y = dec->y_base;
    *uv = dec->uv_base;
    *w = dec->width;
    *h = dec->height;
    *hstride = dec->hor_stride;
    *vstride = dec->ver_stride;

    /*
     * 注意: 不能在这里 deinit frame，否则 MppBuffer 被回收导致 Y/UV 指针失效。
     * 调用方用完显示后必须调用 mpp_dec_return() 释放帧。
     */
    dec->_last_frame = frame;
    return 1;
}

int mpphw_dec_decode(mpp_dec_t *dec, const uint8_t *data, int len, uint8_t **y,
                     uint8_t **uv, int *w, int *h, int *hstride, int *vstride) {
    *y = NULL;
    *uv = NULL;

    MppApi *mpi = (MppApi *)dec->mpi;
    MppPacket packet = dec->packet;
    MPP_RET ret;

    /* 0. 优先返回暂存帧 (上次 put 腾队列时收下的) */
    if (dec->_pending) {
        MppFrame f = dec->_pending;
        dec->_pending = NULL;
        return process_frame(dec, f, y, uv, w, h, hstride, vstride);
    }

    /*
     * 1. 切块提交 — 官方 mpi_dec_test 的 dec_simple 节奏:
     *    a) 按 PUT_CHUNK 小块输入 + split_parse=1 内部分帧
     *       (alientek 定制 MPP 拒收大包/完整帧);
     *    b) 每 put 一块立即 get 一帧 (drain_one), 保持输入/输出
     *       两个队列始终流动 — 官方文档: MPP 输入队列默认只有 4 包,
     *       只灌不取会让解码器 stall, put 全部被拒 (-1012) 丢帧,
     *       画面等 I 帧恢复 → "几秒一跳";
     *    c) put 失败时持续 get 腾位再重试, 不丢帧。
     */
#define PUT_CHUNK 4096
    for (int off = 0; off < len; off += PUT_CHUNK) {
        int clen = (len - off) > PUT_CHUNK ? PUT_CHUNK : (len - off);

        /* 复用 packet，设置新数据（官方 demo: set_data/size/pos/length） */
        mpp_packet_set_data(packet, (void *)(data + off));
        mpp_packet_set_size(packet, clen);
        mpp_packet_set_pos(packet, (void *)(data + off));
        mpp_packet_set_length(packet, clen);

        ret = MPP_NOK;
        for (int try = 0; try < MAX_RETRY_PKT; try++) {
            ret = mpi->decode_put_packet(dec->ctx, packet);
            if (ret == MPP_OK)
                break;

            /* 队列满: 收帧腾位再重试, 不丢帧 */
            drain_one(dec);
            usleep(1000);
        }
        if (ret) {
            LOGD("put_packet ret=%d (off=%d clen=%d)", ret, off, clen);
            return -1;
        }

        /* 每块 put 后顺带收一帧, 保持输出队列流动 */
        drain_one(dec);
    }
#undef PUT_CHUNK

    /* 2. 获取解码帧 */
    MppFrame frame = NULL;
    for (int try = 0; try < MAX_RETRY_FRM; try++) {
        ret = mpi->decode_get_frame(dec->ctx, &frame);
        if (ret == MPP_OK && frame)
            break;
        if (ret == MPP_ERR_TIMEOUT) {
            usleep(1000);
            continue;
        }
        break;
    }

    if (!frame)
        return 0; /* 暂无帧，数据已提交 */

    return process_frame(dec, frame, y, uv, w, h, hstride, vstride);
}

int mpphw_dec_return(mpp_dec_t *dec) {
    if (dec->_last_frame) {
        mpp_frame_deinit(&dec->_last_frame);
        dec->_last_frame = NULL;
        dec->y_base = NULL;
        dec->uv_base = NULL;
    }
    return 0;
}
