/* ✅ 主方案 — fbdev 显示 + RGA 硬转, main.c 使用 */

#ifndef FBDEV_H
#define FBDEV_H

#include "rga_convert.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fd;
    uint8_t *mem;
    size_t size;
    uint32_t w, h, bpp, line_len;
    size_t scr_size;
    uint8_t *back;
    rga_ctx_t rga; /* RGA 转换上下文 */
    int rga_ok;    /* RGA 是否可用 */
    int frame_nr;  /* 帧计数, 清屏节流用 (每 30 帧清一次) */
} fb_t;

int fb_init(fb_t *f, const char *device);
void fb_deinit(fb_t *f);

/* NV12 → (RGA 硬件 / CPU 软件) → RGB → 显存
 * y_stride:  源帧 Y 平面行步长 (MPP hor_stride, 字节)
 * y_vstride: 源帧垂直 stride (MPP ver_stride, 行数, 可能 > src_h)
 *            必须原样传给 RGA, 否则 1080p 会 UV 偏移错位
 * par_n/par_d: 像素宽高比 (SAR). PAL 704×576 通常按 4:3 (12:11),
 *            流无 SAR 元数据时必须由调用方按惯例传入, 否则画面变形
 * arrow_dir: PTZ 箭头叠加 0=无 1=左 2=右 — 画在 back 缓冲 (视频区中心),
 *            不碰解码 DMABUF (写 DMABUF 会破坏 DMA 一致性 → IOMMU 页错误卡死)
 * preset_label: 云台预置位文字 (如 "P1"), 非空时画在中心 (5x7 字体) */
void fb_show_nv12(fb_t *f, const uint8_t *y, const uint8_t *uv, int src_w,
                  int src_h, int y_stride, int y_vstride,
                  int par_n, int par_d, int arrow_dir,
                  const char *preset_label);

#endif
