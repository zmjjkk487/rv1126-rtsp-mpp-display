#include "fbdev.h"
#include "log.h"
#include "rga_convert.h"
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int fb_init(fb_t *f, const char *device) {
    memset(f, 0, sizeof(*f));
    f->fd = open(device, O_RDWR);
    if (f->fd < 0) {
        LOGE("打开 %s: %s", device, strerror(errno));
        return -1;
    }

    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(f->fd, FBIOGET_VSCREENINFO, &vi) < 0) {
        LOGE("FBIOGET_VSCREENINFO");
        goto fail;
    }
    if (ioctl(f->fd, FBIOGET_FSCREENINFO, &fi) < 0) {
        LOGE("FBIOGET_FSCREENINFO");
        goto fail;
    }

    f->w = vi.xres;
    f->h = vi.yres;
    f->bpp = vi.bits_per_pixel;
    f->line_len = fi.line_length;
    f->scr_size = fi.smem_len;
    LOGI("FB: %ux%u %ubpp line=%u", f->w, f->h, f->bpp, f->line_len);

    f->mem = mmap(0, f->scr_size, PROT_READ | PROT_WRITE, MAP_SHARED, f->fd, 0);
    if (f->mem == MAP_FAILED) {
        LOGE("mmap fb");
        goto fail;
    }

    /* back buffer 按显存行对齐分配，避免 line_length > w*4 时溢出 */
    f->size = f->line_len * f->h;
    f->back = malloc(f->size);
    if (!f->back) {
        LOGE("malloc backbuf");
        goto fail_mmap;
    }
    memset(f->back, 0, f->size);

    /* 初始化 RGA 上下文（可选，失败不致命）。
     * 源分辨率暂设 1920×1080（占位），实际每帧 fb_show_nv12 中动态更新。
     * src_stride / src_vstride 初始为占位值，后续会被 MPP 真实的
     * hor_stride / ver_stride 覆盖（1080p 对齐后 vstride = 1088）。 */
    f->rga_ok =
        (rga_init(&f->rga, 1920, 1080, 1920, 1088, (int)f->w, (int)f->h, (int)f->w) == 0);
    if (f->rga_ok)
        LOGI("RGA 硬件转换已启用 (NV12→RGB)");
    else
        LOGI("RGA 不可用, 使用 CPU 软件转换");

    return 0;

fail_mmap:
    munmap(f->mem, f->scr_size);
    f->mem = NULL;
fail:
    close(f->fd);
    f->fd = -1;
    return -1;
}

void fb_deinit(fb_t *f) {
    if (f->rga_ok)
        rga_deinit(&f->rga);
    if (f->back) {
        free(f->back);
        f->back = NULL;
    }
    if (f->mem) {
        munmap(f->mem, f->scr_size);
        f->mem = NULL;
    }
    if (f->fd >= 0) {
        close(f->fd);
        f->fd = -1;
    }
}

/* NV12 → RGB888 (BT.601) */
static void nv12_to_rgb888(const uint8_t *y, const uint8_t *uv, int w, int h,
                           int y_stride, uint32_t *dst, int dst_stride) {
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            int Y = y[r * y_stride + c];
            int U = uv[(r / 2) * y_stride + (c & ~1)];
            int V = uv[(r / 2) * y_stride + (c & ~1) + 1];
            int C = Y - 16, D = U - 128, E = V - 128;
            int R = (298 * C + 409 * E + 128) >> 8;
            int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int B = (298 * C + 516 * D + 128) >> 8;
            if (R < 0)
                R = 0;
            else if (R > 255)
                R = 255;
            if (G < 0)
                G = 0;
            else if (G > 255)
                G = 255;
            if (B < 0)
                B = 0;
            else if (B > 255)
                B = 255;
            dst[r * dst_stride + c] = (R << 16) | (G << 8) | B;
        }
    }
}

/* 最近邻缩放 — NV12 */
static void scale_nv12_nearest(const uint8_t *y, const uint8_t *uv, int sw,
                               int sh, int y_stride, uint8_t *dy, uint8_t *duv,
                               int dw, int dh) {
    int xr = ((sw << 16) / dw) + 1;
    int yr = ((sh << 16) / dh) + 1;

    for (int r = 0; r < dh; r++) {
        int sr = (r * yr) >> 16;
        if (sr >= sh)
            sr = sh - 1;
        for (int c = 0; c < dw; c++) {
            int sc = (c * xr) >> 16;
            if (sc >= sw)
                sc = sw - 1;
            dy[r * dw + c] = y[sr * y_stride + sc];
        }
    }
    for (int r = 0; r < dh / 2; r++) {
        int sr = (r * yr) >> 16;
        if (sr >= sh / 2)
            sr = sh / 2 - 1;
        for (int c = 0; c < dw / 2; c++) {
            int sc = (c * xr) >> 16;
            if (sc >= sw / 2)
                sc = sw / 2 - 1;
            int sp = sr * y_stride + sc * 2, dp = r * dw + c * 2;
            duv[dp] = uv[sp];
            duv[dp + 1] = uv[sp + 1];
        }
    }
}

/* ---------------- PTZ 箭头叠加 (BGRX back 缓冲) ----------------
 * 画在 RGA 转换后的 back 上 (我们自己的内存), 不碰解码 DMABUF —
 * 写 DMABUF 会破坏 DMA 一致性 → IOMMU 页错误 → 解码器挂死 (实测) */

/* 点是否在三角形内 (半平面测试, 整数运算) */
static int pt_in_tri(int px, int py, int x0, int y0, int x1, int y1,
                     int x2, int y2) {
    int d1 = (px - x0) * (y1 - y0) - (py - y0) * (x1 - x0);
    int d2 = (px - x1) * (y2 - y1) - (py - y1) * (x2 - x1);
    int d3 = (px - x2) * (y0 - y2) - (py - y2) * (x0 - x2);
    return !((d1 < 0) || (d2 < 0) || (d3 < 0)) ||
           !((d1 > 0) || (d2 > 0) || (d3 > 0));
}

/* 在 BGRX 缓冲 (行宽 lw 像素) 的 (cx,cy) 中心画 160x160 红色箭头 */
static void draw_arrow_bgrx(uint32_t *pix, int lw, int cx, int cy, int left) {
    const int B = 160;
    int ax = cx - B / 2, ay = cy - B / 2;
    for (int py = 0; py < B; py++) {
        for (int px = 0; px < B; px++) {
            /* 右箭头: 三角 (指向右) + 尾杆; left 时镜像 */
            int inside = pt_in_tri(px, py, 12, 12, B - 12, B / 2, 12, B - 12) ||
                         (px >= 12 && px <= B / 2 &&
                          py >= B / 2 - 15 && py <= B / 2 + 15);
            if (!inside) continue;
            int xx = ax + (left ? (B - 1 - px) : px);
            int yy = ay + py;
            if (xx < 0 || yy < 0 || xx >= lw) continue;
            pix[yy * lw + xx] = 0x00FF0000;   /* BGRX: R=255 亮红 */
        }
    }
}

void fb_show_nv12(fb_t *f, const uint8_t *y, const uint8_t *uv, int sw, int sh,
                  int y_stride, int y_vstride, int par_n, int par_d,
                  int arrow_dir) {
    if (!f->mem || !f->back)
        return;
    f->frame_nr++;
    int dw = (int)f->w, dh = (int)f->h;

    /* 等比缩放: 保持视频宽高比, 补黑边填满屏幕
     * 有效宽高比 = (sw × par_n/par_d) : sh  (SAR 校正)
     * PAL 704×576 按 4:3 (12:11) 显示, 否则画面上下拉伸
     * 注意: 浮点四舍五入 + 偶数对齐 (RGA2 对奇数目标尺寸会内部对齐) */
    if (par_n <= 0) par_n = 1;
    if (par_d <= 0) par_d = 1;
    int vw = dw, vh = dh, xoff = 0, yoff = 0;
    {
        float sa = (float)sw * par_n / par_d / sh, da = (float)dw / dh;
        if (sa > da) { /* 视频更宽 → 适配屏宽, 上下留黑 */
            vw = dw;
            vh = (int)((float)dw * sh * par_d / par_n / sw + 0.5f);
            if (vh & 1) vh++;
            yoff = (dh - vh) / 2;
        } else { /* 视频更高 → 适配屏高, 左右留黑 */
            vh = dh;
            vw = (int)((float)dh * sw * par_n / par_d / sh + 0.5f);
            if (vw & 1) vw++;
            xoff = (dw - vw) / 2;
        }
    }

    /*
     * 策略: RGA 硬件优先 (零 CPU), 失败回退 CPU 软件转换。
     *
     * 教程第5章: RGA 一次完成 NV12→RGB 转换 + 缩放，无需 CPU 参与。
     * 如果板端缺少 librga.so，自动回到 CPU 路径。
     */

    /* 尝试 RGA 硬件转换 */
    if (f->rga_ok && y && uv) {
        /* 更新 RGA 尺寸(防止分辨率变化 + 等比缩放尺寸变化) */
        if (sw != f->rga.src_w || sh != f->rga.src_h ||
            y_stride != f->rga.src_stride || y_vstride != f->rga.src_vstride ||
            vw != f->rga.dst_w || vh != f->rga.dst_h) {
            rga_deinit(&f->rga);
            f->rga_ok =
                (rga_init(&f->rga, sw, sh, y_stride, y_vstride, vw, vh, (int)f->w) == 0);
        }

        if (f->rga_ok) {
            /* 每秒清一次黑底 (30 帧), 非每帧; RGA 完整覆写视频区, 黑边不变 */
            if (f->frame_nr % 30 == 0)
                memset(f->back, 0, f->size);
            /* RGA: 手动偏移指针到视频区, wstride=屏幕宽保证行距正确 */
            uint8_t *dst = (uint8_t *)f->back + yoff * (int)f->w * 4 + xoff * 4;
            if (rga_nv12_to_rgb(&f->rga, y, uv, dst, 0, 0) == 0)
                goto write_fb;
        }
    }

    /* === CPU 软件回退路径 === */

    if (sw > dw || sh > dh) {
        /* 需要缩小: 先用最近邻缩放到屏幕尺寸 NV12，再转 RGB */
        int ysz = dw * dh, uvsz = (dw / 2) * (dh / 2) * 2;
        uint8_t *sy = malloc(ysz), *suv = malloc(uvsz);
        if (!sy || !suv) {
            free(sy);
            free(suv);
            return;
        }
        scale_nv12_nearest(y, uv, sw, sh, y_stride, sy, suv, dw, dh);
        if (f->frame_nr % 30 == 0)
            memset(f->back, 0, f->size);
        nv12_to_rgb888(sy, suv, dw, dh, dw, (uint32_t *)f->back, f->w);
        free(sy);
        free(suv);
    } else {
        /* 直接转换（源 ≤ 目标尺寸） */
        dw = sw < (int)f->w ? sw : (int)f->w;
        dh = sh < (int)f->h ? sh : (int)f->h;
        if (f->frame_nr % 30 == 0)
            memset(f->back, 0, f->size);
        nv12_to_rgb888(y, uv, dw, dh, y_stride, (uint32_t *)f->back, f->w);
    }

write_fb:
    /* PTZ 箭头叠加: 视频区中心, 画在 back (自己的内存), 再随帧写显存。
     * 必须放这里 — RGA 成功路径会 goto write_fb 跳过上面的代码 */
    if (arrow_dir) {
        int cx = xoff + vw / 2, cy = yoff + vh / 2;
        draw_arrow_bgrx((uint32_t *)f->back, (int)f->w, cx, cy,
                        arrow_dir == 1);
    }
    /* 写显存 — 处理 16bpp 和 32bpp 两种格式 */
    if (f->bpp == 16) {
        uint16_t *d = (uint16_t *)f->mem;
        uint32_t *s = (uint32_t *)f->back;
        for (int r = 0; r < dh; r++)
            for (int c = 0; c < dw; c++) {
                uint32_t p = s[r * f->w + c];
                d[r * f->line_len / 2 + c] =
                    (uint16_t)(((p >> 19) << 11) | (((p >> 10) & 0x3F) << 5) |
                               ((p >> 3) & 0x1F));
            }
    } else {
        uint32_t *d = (uint32_t *)f->mem, *s = (uint32_t *)f->back;
        for (int r = 0; r < dh; r++)
            memcpy(&d[r * f->line_len / 4], &s[r * f->w], dw * 4);
    }
}
