/*
 * 🔬 实验 — DRM/KMS 显示 (未完成, RGA 未集成, 不编译)
 *
 * 当前正式方案使用 fbdev (fbdev.c), 更简单稳定。
 * 如需 DRM: 需实现 page flip + RGA dma-buf 导入, 工作量较大。
 */

#include "drm_display.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

/* 尝试在 DRM 设备上找到第一个已连接的 connector，
 * 并获取其首选分辨率和 CRTC。 */
static int find_connector(int fd, drmModeRes *res, uint32_t *crtc_id,
                          uint32_t *conn_id, uint32_t *w, uint32_t *h) {
    drmModeConnector *conn = NULL;
    int found = 0;

    for (int i = 0; i < res->count_connectors && !found; i++) {
        conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn)
            continue;

        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            drmModeModeInfo *mode = &conn->modes[0]; /* 首选模式 */
            LOGI("DRM connector %u: %s %dx%d", conn->connector_id,
                 drmModeGetConnectorTypeName(conn->connector_type),
                 mode->hdisplay, mode->vdisplay);

            *conn_id = conn->connector_id;
            *w = mode->hdisplay;
            *h = mode->vdisplay;

            /* 找到对应的 encoder 和 crtc */
            drmModeEncoder *enc = NULL;
            for (int j = 0; j < res->count_encoders; j++) {
                enc = drmModeGetEncoder(fd, res->encoders[j]);
                if (enc && enc->encoder_id == conn->encoder_id) {
                    *crtc_id = enc->crtc_id;
                    drmModeFreeEncoder(enc);
                    found = 1;
                    break;
                }
                if (enc)
                    drmModeFreeEncoder(enc);
            }
        }
        drmModeFreeConnector(conn);
        conn = NULL;
    }

    if (!found) {
        LOGE("未找到已连接的 DRM 显示器");
        return -1;
    }
    return 0;
}

int drm_init(drm_t *d, const char *device) {
    memset(d, 0, sizeof(*d));
    d->drm_fd = -1;

    int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOGE("打开 DRM 设备 %s: %s", device, strerror(errno));
        return -1;
    }

    /* 检查 DRM 能力 */
    uint64_t has_dumb = 0;
    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 || !has_dumb) {
        LOGE("DRM 不支持 dumb buffer");
        close(fd);
        return -1;
    }

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        LOGE("drmModeGetResources 失败");
        close(fd);
        return -1;
    }

    uint32_t w = 0, h = 0;
    if (find_connector(fd, res, &d->crtc_id, &d->connector_id, &w, &h) < 0) {
        drmModeFreeResources(res);
        close(fd);
        return -1;
    }
    drmModeFreeResources(res);

    d->fb_w = w;
    d->fb_h = h;

    /* 创建 dumb buffer 作为帧缓冲 */
    struct drm_mode_create_dumb create = {0};
    create.width = w;
    create.height = h;
    create.bpp = 32; /* XRGB8888 */

    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        LOGE("DRM_IOCTL_MODE_CREATE_DUMB: %s", strerror(errno));
        close(fd);
        return -1;
    }

    d->handle = create.handle;
    d->pitch = create.pitch;
    d->size = create.size;

    /* 创建 framebuffer 对象 */
    uint32_t handles[4] = {d->handle, 0, 0, 0};
    uint32_t pitches[4] = {d->pitch, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};

    if (drmModeAddFB2(fd, w, h, DRM_FORMAT_XRGB8888, handles, pitches, offsets,
                      &d->fb_id, 0) < 0) {
        LOGE("drmModeAddFB2: %s", strerror(errno));
        goto fail_dumb;
    }

    /* mmap dumb buffer */
    struct drm_mode_map_dumb mmap_req = {0};
    mmap_req.handle = d->handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mmap_req) < 0) {
        LOGE("DRM_IOCTL_MODE_MAP_DUMB: %s", strerror(errno));
        goto fail_fb;
    }

    d->map = (uint8_t *)mmap(0, d->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                             mmap_req.offset);
    if (d->map == MAP_FAILED) {
        LOGE("DRM mmap: %s", strerror(errno));
        d->map = NULL;
        goto fail_fb;
    }
    memset(d->map, 0, d->size);

    /* 激活显示 */
    if (drmModeSetCrtc(fd, d->crtc_id, d->fb_id, 0, 0, &d->connector_id, 1, &w,
                       &h) < 0) {
        LOGE("drmModeSetCrtc: %s", strerror(errno));
        goto fail_map;
    }

    d->drm_fd = fd;
    d->crtc_w = w;
    d->crtc_h = h;

    /* 分配后备缓冲 (用于 NV12→RGB 转换) */
    d->back = (uint8_t *)calloc(1, w * h * 3); /* RGB888 */
    if (!d->back) {
        LOGW("DRM back buffer alloc failed");
    }

    LOGI("DRM 初始化: %s %ux%u pitch=%u size=%llu", device, w, h, d->pitch,
         (unsigned long long)d->size);
    return 0;

fail_map:
    munmap(d->map, d->size);
    d->map = NULL;
fail_fb:
    drmModeRmFB(fd, d->fb_id);
    d->fb_id = 0;
fail_dumb: {
    struct drm_mode_destroy_dumb destroy = {.handle = d->handle};
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
}
    d->handle = 0;
    close(fd);
    return -1;
}

void drm_show_rgb(drm_t *d, const uint8_t *rgb, int w, int h, int stride) {
    if (!d->map || !rgb)
        return;

    int dw = w < (int)d->fb_w ? w : (int)d->fb_w;
    int dh = h < (int)d->fb_h ? h : (int)d->fb_h;
    int src_bytes = stride * 3; /* RGB888: 每像素 3 字节 */
    int dst_bytes = d->pitch;

    /* 逐行拷贝 RGB888 → XRGB8888 帧缓冲 */
    for (int r = 0; r < dh; r++) {
        uint8_t *dst = d->map + r * dst_bytes;
        const uint8_t *src = rgb + r * src_bytes;
        for (int c = 0; c < dw; c++) {
            dst[c * 4 + 0] = src[c * 3 + 0]; /* B (或 R, 取决于格式) */
            dst[c * 4 + 1] = src[c * 3 + 1]; /* G */
            dst[c * 4 + 2] = src[c * 3 + 2]; /* R (或 B) */
            dst[c * 4 + 3] = 0xFF;           /* Alpha */
        }
    }

    /* 刷新: 重新设置 CRTC */
    drmModeSetCrtc(d->drm_fd, d->crtc_id, d->fb_id, 0, 0, &d->connector_id, 1,
                   &d->crtc_w, &d->crtc_h);
}

void drm_deinit(drm_t *d) {
    if (!d)
        return;

    if (d->back) {
        free(d->back);
        d->back = NULL;
    }
    if (d->map) {
        munmap(d->map, d->size);
        d->map = NULL;
    }
    if (d->fb_id && d->drm_fd >= 0) {
        drmModeRmFB(d->drm_fd, d->fb_id);
        d->fb_id = 0;
    }
    if (d->handle && d->drm_fd >= 0) {
        struct drm_mode_destroy_dumb destroy = {.handle = d->handle};
        drmIoctl(d->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        d->handle = 0;
    }
    if (d->drm_fd >= 0) {
        /* 恢复文本模式（可选） */
        drmModeSetCrtc(d->drm_fd, d->crtc_id, 0, 0, 0, NULL, 0, NULL);
        close(d->drm_fd);
        d->drm_fd = -1;
    }
}

/* ---- NV12 → RGB888 (BT.601) CPU 转换 (DRM 专用) ---- */
static void nv12_to_rgb888_drm(const uint8_t *y, const uint8_t *uv, int w,
                               int h, int y_stride, uint8_t *dst,
                               int dst_stride) {
    for (int r = 0; r < h; r++) {
        uint8_t *d = dst + r * dst_stride;
        for (int c = 0; c < w; c++) {
            int Y = y[r * y_stride + c];
            int U = uv[(r / 2) * y_stride + (c & ~1)];
            int V = uv[(r / 2) * y_stride + (c & ~1) + 1];
            int C = Y - 16, D = U - 128, E = V - 128;
            int R = (298 * C + 409 * E + 128) >> 8;
            int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int B = (298 * C + 516 * D + 128) >> 8;
            d[c * 3 + 0] = (uint8_t)(R < 0 ? 0 : (R > 255 ? 255 : R));
            d[c * 3 + 1] = (uint8_t)(G < 0 ? 0 : (G > 255 ? 255 : G));
            d[c * 3 + 2] = (uint8_t)(B < 0 ? 0 : (B > 255 ? 255 : B));
        }
    }
}

void drm_show_nv12(drm_t *d, const uint8_t *y, const uint8_t *uv, int sw,
                   int sh, int y_stride) {
    if (!d->map || !d->back || !y || !uv)
        return;

    int dw = sw < (int)d->fb_w ? sw : (int)d->fb_w;
    int dh = sh < (int)d->fb_h ? sh : (int)d->fb_h;

    /* NV12 → RGB888 (CPU, 后续可加 RGA 加速) */
    nv12_to_rgb888_drm(y, uv, dw, dh, y_stride, d->back, dw * 3);

    /* RGB888 → XRGB8888 + 写入 DRM buffer */
    for (int r = 0; r < dh; r++) {
        uint8_t *dst_line = d->map + r * d->pitch;
        const uint8_t *src_line = d->back + r * dw * 3;
        for (int c = 0; c < dw; c++) {
            dst_line[c * 4 + 0] = src_line[c * 3 + 2]; /* R */
            dst_line[c * 4 + 1] = src_line[c * 3 + 1]; /* G */
            dst_line[c * 4 + 2] = src_line[c * 3 + 0]; /* B */
            dst_line[c * 4 + 3] = 0xFF;
        }
    }

    /* 刷新 CRTC */
    drmModeSetCrtc(d->drm_fd, d->crtc_id, d->fb_id, 0, 0, &d->connector_id, 1,
                   &d->crtc_w, &d->crtc_h);
}
