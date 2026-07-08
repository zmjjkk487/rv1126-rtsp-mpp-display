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
    int rga_fd;  /* /dev/rga 文件描述符, -1 表示未打开 */
    int src_fmt; /* 源格式: RK_FORMAT_YCbCr_420_SP (NV12) */
    int dst_fmt; /* 目标格式: RK_FORMAT_RGB_888 / RK_FORMAT_BGR_888 */
    int src_w, src_h;
    int src_stride;
    int dst_w, dst_h;
    int dst_stride; /* 目标行步长 (字节) */
} rga_ctx_t;

/*
 * 初始化 RGA 转换上下文
 * src_w/src_h: 源 NV12 帧尺寸 (解码后尺寸)
 * dst_w/dst_h: 目标 RGB 帧尺寸 (屏幕尺寸)
 * 返回: 0=成功, -1=失败(可回退 CPU 转换)
 */
int rga_init(rga_ctx_t *ctx, int src_w, int src_h, int dst_w, int dst_h);

/*
 * 执行 NV12 → RGB 转换 + 缩放
 * y/uv:   MPP 解码输出的 NV12 帧 (Y 和 UV 平面)
 * rgb:    目标 RGB 缓冲区, 调用方分配 (dst_w * dst_h * 3 字节)
 * 返回: 0=成功, -1=失败
 */
int rga_nv12_to_rgb(rga_ctx_t *ctx, const uint8_t *y, const uint8_t *uv,
                    uint8_t *rgb);

/* 释放 RGA 上下文 */
void rga_deinit(rga_ctx_t *ctx);

#endif
