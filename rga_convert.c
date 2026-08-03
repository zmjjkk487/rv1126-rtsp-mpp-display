/*
 * rga_convert.c — Rockchip RGA 硬件 NV12→RGB 转换 + 缩放
 *
 * 教程第5章核心：用 RGA 硬件替代 CPU 做颜色转换。
 * RV1126B 集成 RGA 2D 加速器，零 CPU 开销完成 NV12→RGB + 缩放。
 *
 * 使用 c_RkRgaBlit API (librga.so C 导出)，通过 dlopen 动态加载。
 * rga_info_t 结构体来自 vendored rga_info.h (官方 airockchip/librga)
 *
 * 板端验证: strings /usr/lib/librga.so | grep c_RkRga
 */

#include "rga_convert.h"
#include "log.h"
#include "rga_info.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 设计决策: RGA 库句柄和函数指针全局共享，非每实例独立。
 * 优点: librga.so 只加载一次，零开销。
 * 约束: 整个进程生命周期内只能有一个活跃的 RGA 上下文，
 *       rga_deinit() 会关闭共享句柄，所有上下文同时失效。
 * 如需多路 RGA(如 OSD 叠加): 将以下四个变量移入 rga_ctx_t，
 *       load_rga_library 改为每 ctx 独立 dlopen，rga_deinit 各关各的。
 */
/* === 动态加载 librga === */
static void *rga_lib = NULL;
static int (*p_RkRgaInit)(void) = NULL;
static int (*p_RkRgaBlit)(rga_info_t *, rga_info_t *, rga_info_t *) = NULL;
static void (*p_RkRgaDeInit)(void) = NULL;

static int load_rga_library(void) {
    if (rga_lib)
        return 0;

    const char *names[] = {"librga.so", "libRgaApi.so", "librockchip_rga.so",
                           NULL};
    for (int i = 0; names[i]; i++) {
        rga_lib = dlopen(names[i], RTLD_NOW);
        if (rga_lib) {
            LOGI("已加载 RGA 库: %s", names[i]);
            break;
        }
    }
    if (!rga_lib) {
        LOGW("未找到 RGA 库, 回退 CPU 转换");
        return -1;
    }

    p_RkRgaInit = dlsym(rga_lib, "c_RkRgaInit");
    p_RkRgaBlit = dlsym(rga_lib, "c_RkRgaBlit");
    p_RkRgaDeInit = dlsym(rga_lib, "c_RkRgaDeInit");

    if (!p_RkRgaInit || !p_RkRgaBlit) {
        LOGW("RGA 符号解析失败");
        dlclose(rga_lib);
        rga_lib = NULL;
        return -1;
    }
    if (p_RkRgaInit() < 0) {
        LOGW("c_RkRgaInit 失败");
        dlclose(rga_lib);
        rga_lib = NULL;
        return -1;
    }
    LOGI("RGA 硬件加速已就绪 (librga v1.10.5 兼容)");
    return 0;
}

/* === 公开接口 === */

int rga_init(rga_ctx_t *ctx, int sw, int sh, int src_stride, int src_vstride,
             int dw, int dh) {
    memset(ctx, 0, sizeof(*ctx));
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return -1;
    if (load_rga_library() < 0)
        return -1;

    ctx->src_w = sw;
    ctx->src_h = sh;
    /* src_stride 必须从 MPP hor_stride 获取，不能写死 width。
     * 硬件对齐可能导致 stride > width，写死 width 会导致 UV 偏移→色偏。 */
    ctx->src_stride = (src_stride > 0) ? src_stride : sw;
    /* src_vstride 必须从 MPP ver_stride 获取（16 对齐，可能 > height）。
     * RGA 按 wstride*hstride 定位 NV12 的 UV 平面：1080p 时 ver_stride=1088，
     * 若 hstride 用 height=1080，UV 会偏移 8 行导致色偏。 */
    ctx->src_vstride = (src_vstride > 0) ? src_vstride : sh;
    ctx->dst_w = dw;
    ctx->dst_h = dh;
    ctx->dst_stride = dw * 4; /* BGRX_8888: 4 字节/像素 */

    LOGI("RGA: NV12 %dx%d (hstride=%d vstride=%d) -> BGRX_8888 %dx%d", sw, sh,
         ctx->src_stride, ctx->src_vstride, dw, dh);
    return 0;
}

int rga_nv12_to_rgb(rga_ctx_t *ctx, const uint8_t *y, const uint8_t *uv,
                    uint8_t *rgb) {
    if (!p_RkRgaBlit || !y || !rgb)
        return -1;
    (void)uv; /* NV12: UV 紧接 Y 后面，RGA 库从 Y 指针自己定位 UV */

    rga_info_t src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));

    /* 源: NV12 (YUV420 semi-planar) */
    src.fd = -1;
    src.virAddr = (void *)y;
    src.mmuFlag = 1;
    src.sync_mode = 1;
    src.format = RK_FORMAT_YCbCr_420_SP;
    src.rect.xoffset = 0;
    src.rect.yoffset = 0;
    src.rect.width = ctx->src_w;
    src.rect.height = ctx->src_h;
    src.rect.wstride = ctx->src_stride;
    src.rect.hstride = ctx->src_vstride; /* 必须是 ver_stride, 不能是 height */
    src.rect.format = RK_FORMAT_YCbCr_420_SP;

    /* 目标: BGRX_8888 (与 fbdev 32bpp 显存格式一致) */
    dst.fd = -1;
    dst.virAddr = rgb;
    dst.mmuFlag = 1;
    dst.sync_mode = 1;
    dst.format = RK_FORMAT_BGRX_8888;
    dst.rect.xoffset = 0;
    dst.rect.yoffset = 0;
    dst.rect.width = ctx->dst_w;
    dst.rect.height = ctx->dst_h;
    dst.rect.wstride = ctx->dst_w;
    dst.rect.hstride = ctx->dst_h;
    dst.rect.format = RK_FORMAT_BGRX_8888;

    if (p_RkRgaBlit(&src, &dst, NULL)) {
        LOGW("RGA blit 失败, 回退 CPU (可能: IOMMU未开 / stride错配 / "
             "无/dev/rga)");
        return -1;
    }
    return 0;
}

void rga_deinit(rga_ctx_t *ctx) {
    if (p_RkRgaDeInit)
        p_RkRgaDeInit();
    /* 注意: 关闭的是全局共享句柄，会影响所有 RGA 上下文 */
    if (rga_lib) {
        dlclose(rga_lib);
        rga_lib = NULL;
    }
    p_RkRgaInit = NULL;
    p_RkRgaBlit = NULL;
    p_RkRgaDeInit = NULL;
    if (ctx)
        memset(ctx, 0, sizeof(*ctx));
}
