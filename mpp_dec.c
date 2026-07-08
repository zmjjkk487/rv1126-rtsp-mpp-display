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

int mpp_dec_init(mpp_dec_t *dec, int width, int height) {
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

    /* === 解码器配置 === */
    MppDecCfg cfg = NULL;
    mpp_dec_cfg_init(&cfg);

    ret = mpi->control(ctx, MPP_DEC_GET_CFG, cfg);
    if (ret) {
        LOGE("MPP_DEC_GET_CFG ret=%d", ret);
        goto fail_cfg;
    }

    /* split_parse=1: MPP 内部自动拆分 Annex-B NAL 单元 */
    ret = mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1);
    if (ret) {
        LOGE("set split_parse ret=%d", ret);
        goto fail_cfg;
    }

    ret = mpi->control(ctx, MPP_DEC_SET_CFG, cfg);
    if (ret) {
        LOGE("MPP_DEC_SET_CFG ret=%d", ret);
        goto fail_cfg;
    }

    mpp_dec_cfg_deinit(cfg);

    /*
     * 注意: MPP_SET_INPUT_TIMEOUT 在正点原子 SDK MPP (alientek 2026-03-02)
     * 上可能导致段错误，已禁用。如需启用，确认 MPP 版本支持此控制命令。
     * 官方 rk_mpi_cmd.h: MPP_SET_INPUT_TIMEOUT, parameter type RK_S64
     */

    dec->ctx = ctx;
    dec->mpi = mpi;

    LOGI("MPP 解码器初始化完成: %dx%d split_parse=1", width, height);
    return 0;

fail_cfg:
    mpp_dec_cfg_deinit(cfg);
    mpp_packet_deinit(&dec->packet);
    dec->packet = NULL;
fail_ctx:
    mpp_destroy(ctx);
    return -1;
}

void mpp_dec_deinit(mpp_dec_t *dec) {
    if (!dec)
        return;

    /* 先释放上次未释放的帧 */
    if (dec->_last_frame) {
        mpp_frame_deinit(&dec->_last_frame);
        dec->_last_frame = NULL;
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
    dec->hor_stride = (int)hor_stride;
    dec->ver_stride = (int)ver_stride;

    LOGI("分辨率变更: %dx%d stride=%dx%d buf_size=%d", width, height,
         hor_stride, ver_stride, buf_size);

    /* 释放旧 buffer group */
    if (dec->frm_grp) {
        mpp_buffer_group_put(dec->frm_grp);
        dec->frm_grp = NULL;
    }

    /* 创建输出 buffer group: NV12 需要 stride*height*3/2 字节，
     * 这里用 buf_size (MPP 计算好的值)，缓冲池大小 24 */
    MppBufferGroup grp = NULL;
    MPP_RET ret = mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DRM);
    if (ret) {
        /* DRM 不可用则回退到 ion */
        ret = mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_ION);
    }
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

int mpp_dec_decode(mpp_dec_t *dec, const uint8_t *data, int len, uint8_t **y,
                   uint8_t **uv, int *w, int *h, int *stride) {
    *y = NULL;
    *uv = NULL;

    MppApi *mpi = (MppApi *)dec->mpi;
    MppPacket packet = dec->packet;
    MPP_RET ret;

    /* 复用 packet，设置新数据（官方 demo: mpp_packet_set_data/size/pos/length）
     */
    mpp_packet_set_data(packet, (void *)data);
    mpp_packet_set_size(packet, len);
    mpp_packet_set_pos(packet, (void *)data);
    mpp_packet_set_length(packet, len);

    /* 提交码流 */
    for (int try = 0; try < MAX_RETRY_PKT; try++) {
        ret = mpi->decode_put_packet(dec->ctx, packet);
        if (ret == MPP_OK)
            break;
        usleep(1000);
    }
    if (ret) {
        LOGD("put_packet ret=%d", ret);
        return -1;
    }

    /* 获取解码帧 */
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

    /* NV12: Y plane + interleaved UV plane */
    dec->y_base = base;
    dec->uv_base = base + dec->hor_stride * dec->ver_stride;

    *y = dec->y_base;
    *uv = dec->uv_base;
    *w = dec->width;
    *h = dec->height;
    *stride = dec->hor_stride;

    /*
     * 注意: 不能在这里 deinit frame，否则 MppBuffer 被回收导致 Y/UV 指针失效。
     * 调用方用完显示后必须调用 mpp_dec_return() 释放帧。
     */
    dec->_last_frame = frame;
    return 1;
}

int mpp_dec_return(mpp_dec_t *dec) {
    if (dec->_last_frame) {
        mpp_frame_deinit(&dec->_last_frame);
        dec->_last_frame = NULL;
        dec->y_base = NULL;
        dec->uv_base = NULL;
    }
    return 0;
}
