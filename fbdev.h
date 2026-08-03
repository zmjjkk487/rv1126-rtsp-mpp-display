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
} fb_t;

int fb_init(fb_t *f, const char *device);
void fb_deinit(fb_t *f);

/* NV12 → (RGA 硬件 / CPU 软件) → RGB → 显存
 * y_stride:  源帧 Y 平面行步长 (MPP hor_stride, 字节)
 * y_vstride: 源帧垂直 stride (MPP ver_stride, 行数, 可能 > src_h)
 *            必须原样传给 RGA, 否则 1080p 会 UV 偏移错位 */
void fb_show_nv12(fb_t *f, const uint8_t *y, const uint8_t *uv, int src_w,
                  int src_h, int y_stride, int y_vstride);

#endif
