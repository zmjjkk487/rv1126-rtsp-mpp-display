/*
 * drm_display.h — Linux DRM/KMS 显示封装
 *
 * 教程第6章: 使用 DRM 替代 fbdev 进行显示输出
 * 优势: 支持 vsync、多 plane、更低的 CPU 开销
 * 依赖: libdrm.so (Buildroot 通常自带)
 *
 * 板端验证 DRM 是否可用:
 *   ls /dev/dri/card0
 *   cat /sys/class/drm/card0/card0-DSI-1/status   # 应输出 "connected"
 *   cat /sys/class/drm/card0/card0-DSI-1/modes    # 支持的分辨率
 */

#ifndef DRM_DISPLAY_H
#define DRM_DISPLAY_H

#include <stdint.h>

typedef struct {
    int drm_fd;              /* DRM 设备文件描述符 */
    uint32_t crtc_id;        /* CRTC ID */
    uint32_t connector_id;   /* Connector ID */
    uint32_t fb_id;          /* Framebuffer ID */
    uint32_t handle;         /* GEM buffer handle */
    uint32_t pitch;          /* 行步长 (字节) */
    uint64_t size;           /* buffer 大小 */
    uint8_t *map;            /* mmap 映射的显存指针 */
    uint32_t fb_w, fb_h;     /* 帧缓冲宽高 */
    uint32_t crtc_w, crtc_h; /* CRTC 实际宽高 */
    uint8_t *back;           /* 后备缓冲 (用于 NV12→RGB 转换) */
} drm_t;

/*
 * 初始化 DRM 显示
 * device: 例如 "/dev/dri/card0"
 * 返回: 0=成功, -1=失败 (可回退 fbdev)
 */
int drm_init(drm_t *d, const char *device);

/* NV12 → (CPU NV12→RGB) → DRM 显示
 * 注意: DRM 路径暂未集成 RGA，使用 CPU 转换。
 * 如需 RGA 加速请使用 fbdev 路径 (fbdev 已集成 RGA)。 */
void drm_show_nv12(drm_t *d, const uint8_t *y, const uint8_t *uv, int src_w,
                   int src_h, int y_stride);

/* 将 RGB888 像素数据刷新到屏幕 */
void drm_show_rgb(drm_t *d, const uint8_t *rgb, int w, int h, int stride);

/* 释放资源 */
void drm_deinit(drm_t *d);

#endif
