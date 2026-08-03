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

#ifndef RGA_CONVERT_H
#define RGA_CONVERT_H

#include <stdint.h>

/* RGA 转换句柄 */
typedef struct {
    int src_w, src_h;
    int src_stride;  /* 源帧 Y 平面行步长 (字节), 从 MPP hor_stride 获取 */
    int src_vstride; /* 源帧垂直 stride (行), 从 MPP ver_stride 获取 */
    int dst_w, dst_h;
    int dst_stride; /* 目标行步长 (字节) */
} rga_ctx_t;

/**
 * rga_init — 初始化 RGA 转换上下文
 * @sw, sh       源帧宽高
 * @src_stride   源帧 Y 平面行步长 (通常 = MPP 输出的 hor_stride，可能 ≠ sw)
 * @src_vstride  源帧垂直 stride (通常 = MPP 输出的 ver_stride, 16 对齐后
 *              可能 > sh)。RGA 按 wstride*hstride 定位 NV12 的 UV 平面,
 *              填错会导致 UV 偏移→色偏, 必须传 MPP 的真实 ver_stride
 * @dw, dh       目标宽高
 */
int rga_init(rga_ctx_t *ctx, int sw, int sh, int src_stride, int src_vstride,
             int dw, int dh);

/*
 * 执行 NV12 → BGRX_8888 转换 + 缩放
 * y/uv:   MPP 解码输出的 NV12 帧 (Y 和 UV 平面)
 * rgb:    目标 BGRX_8888 缓冲区, 调用方分配 (dst_w * dst_h * 4 字节)
 * 返回: 0=成功, -1=失败
 */
int rga_nv12_to_rgb(rga_ctx_t *ctx, const uint8_t *y, const uint8_t *uv,
                    uint8_t *rgb);

/* 释放 RGA 上下文 */
void rga_deinit(rga_ctx_t *ctx);

#endif
