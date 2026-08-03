/*
 * rga_convert.h — Rockchip RGA 硬件颜色转换封装
 *
 * 教程第5章: RGA 负责 NV12→RGB 颜色空间转换 + 缩放
 * RV1126B 集成 RGA 2D 加速器，可以零 CPU 开销完成:
 *   - NV12 → RGB888/RGB565 转换
 *   - 任意缩放（1080P→720P）
 *   - 旋转、裁剪、镜像
 *
 * 板端验证 RGA 是否可用:
 *   ls /usr/lib/librga*
 *   ls /dev/rga
 *   cat /sys/kernel/debug/rga/version   # 如果有 debugfs
 */

/* ✅ 主方案 — RGA 硬件 NV12→BGRX 转换, fbdev.c 调用 */

#ifndef RGA_CONVERT_H
#define RGA_CONVERT_H

#include <stdint.h>

/* RGA 转换句柄 */
typedef struct {
    int src_w, src_h;
    int src_stride;  /* 源帧 Y 平面行步长 (字节), 从 MPP hor_stride 获取 */
    int src_vstride; /* 源帧垂直 stride (行), 从 MPP ver_stride 获取 */
    int dst_w, dst_h;
    int dst_stride;     /* 目标缓冲行宽 (像素), 通常 = 屏幕宽 f->w。
                           RGA 按此跳行, 必须用缓冲区全宽而非缩放宽 vw,
                           否则左右留黑边场景下每行错位 → 斜切花屏 */
} rga_ctx_t;

/**
 * rga_init — 初始化 RGA 转换上下文
 * @sw, sh          源帧宽高
 * @src_stride      源帧 Y 平面行步长 (对 NV12 = hor_stride)
 * @src_vstride     源帧垂直 stride (ver_stride, 16 对齐后可能 > sh)
 * @dw, dh          目标缩放后宽高 (等比缩放后的视频区尺寸)
 * @dst_buf_stride  目标缓冲每行像素数 (通常 = 屏幕宽 f->w, 不是 dw!)
 */
int rga_init(rga_ctx_t *ctx, int sw, int sh, int src_stride, int src_vstride,
             int dw, int dh, int dst_buf_stride);

/*
 * 执行 NV12 → BGRX_8888 转换 + 缩放
 * y/uv:   MPP 解码输出的 NV12 帧
 * rgb:    目标缓冲区基址 (屏幕 back buffer)
 * xoff/yoff: 视频区在 rgb 缓冲中的偏移 (像素), 用于留黑边
 * 返回: 0=成功, -1=失败
 */
int rga_nv12_to_rgb(rga_ctx_t *ctx, const uint8_t *y, const uint8_t *uv,
                    uint8_t *rgb, int xoff, int yoff);

/* 释放 RGA 上下文 */
void rga_deinit(rga_ctx_t *ctx);

#endif
