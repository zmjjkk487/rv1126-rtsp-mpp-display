/*
 * rga_info.h — Rockchip RGA 数据结构定义 (Vendored from official librga)
 *
 * 来源: https://github.com/airockchip/librga/blob/main/include/rga.h
 *       https://github.com/airockchip/librga/blob/main/include/drmrga.h
 * 许可证: Apache License 2.0
 *
 * 只提取了 c_RkRgaBlit API 需要的 rga_info_t 及其嵌套结构体。
 * 此文件与板端 librga.so 的 ABI 严格一致，消除所有字节偏移量猜测。
 */

#ifndef RGA_INFO_H
#define RGA_INFO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* === 像素格式枚举 (来自 rga.h) === */
typedef enum {
    RK_FORMAT_RGBA_8888 = 0x0 << 8,
    RK_FORMAT_RGBX_8888 = 0x1 << 8,
    RK_FORMAT_RGB_888 = 0x2 << 8,
    RK_FORMAT_BGRA_8888 = 0x3 << 8,
    RK_FORMAT_RGB_565 = 0x4 << 8,
    RK_FORMAT_RGBA_5551 = 0x5 << 8,
    RK_FORMAT_RGBA_4444 = 0x6 << 8,
    RK_FORMAT_BGR_888 = 0x7 << 8,
    RK_FORMAT_YCbCr_422_SP = 0x8 << 8,
    RK_FORMAT_YCbCr_422_P = 0x9 << 8,
    RK_FORMAT_YCbCr_420_SP = 0xa << 8, /* NV12 */
    RK_FORMAT_YCbCr_420_P = 0xb << 8,
    RK_FORMAT_YCrCb_422_SP = 0xc << 8,
    RK_FORMAT_YCrCb_422_P = 0xd << 8,
    RK_FORMAT_YCrCb_420_SP = 0xe << 8,
    RK_FORMAT_YCrCb_420_P = 0xf << 8,
    RK_FORMAT_BPP1 = 0x10 << 8,
    RK_FORMAT_BPP2 = 0x11 << 8,
    RK_FORMAT_BPP4 = 0x12 << 8,
    RK_FORMAT_BPP8 = 0x13 << 8,
    RK_FORMAT_YCbCr_400 = 0x15 << 8,
    RK_FORMAT_BGRX_8888 = 0x16 << 8,
    RK_FORMAT_Y4 = 0x14 << 8,
    RK_FORMAT_ARGB_8888 = 0x28 << 8,
    RK_FORMAT_XRGB_8888 = 0x29 << 8,
    RK_FORMAT_ABGR_8888 = 0x2c << 8,
    RK_FORMAT_XBGR_8888 = 0x2d << 8,
    RK_FORMAT_A8 = 0x31 << 8,
    RK_FORMAT_UNKNOWN = 0x100 << 8,
} RgaSURF_FORMAT;

/* === rga_rect_t (来自 drmrga.h) === */
typedef struct rga_rect {
    int xoffset;
    int yoffset;
    int width;
    int height;
    int wstride;
    int hstride;
    int format;
    int size;
} rga_rect_t;

/* === rga_nn_t (来自 drmrga.h) === */
typedef struct rga_nn {
    int nn_flag;
    int scale_r;
    int scale_g;
    int scale_b;
    int offset_r;
    int offset_g;
    int offset_b;
} rga_nn_t;

/* === rga_dither_t (来自 drmrga.h) === */
typedef struct rga_dither {
    int enable;
    int mode;
    int lut0_l;
    int lut0_h;
    int lut1_l;
    int lut1_h;
} rga_dither_t;

/* === rga_mosaic_info (来自 drmrga.h) === */
struct rga_mosaic_info {
    uint8_t enable;
    uint8_t mode;
};

/* === rga_pre_intr_info (来自 drmrga.h) === */
struct rga_pre_intr_info {
    uint8_t enable;
    uint8_t read_intr_en;
    uint8_t write_intr_en;
    uint8_t read_hold_en;
    uint32_t read_threshold;
    uint32_t write_start;
    uint32_t write_step;
};

/* === rga_osd_invert_factor (来自 drmrga.h) === */
struct rga_osd_invert_factor {
    uint8_t alpha_max;
    uint8_t alpha_min;
    uint8_t yg_max;
    uint8_t yg_min;
    uint8_t crb_max;
    uint8_t crb_min;
};

/* === rga_color (来自 drmrga.h) === */
struct rga_color {
    union {
        struct {
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            uint8_t alpha;
        };
        uint32_t value;
    };
};

/* === rga_osd_bpp2 (来自 drmrga.h) === */
struct rga_osd_bpp2 {
    uint8_t ac_swap;
    uint8_t endian_swap;
    struct rga_color color0;
    struct rga_color color1;
};

/* === rga_osd_mode_ctrl (来自 drmrga.h) === */
struct rga_osd_mode_ctrl {
    uint8_t mode;
    uint8_t direction_mode;
    uint8_t width_mode;
    uint16_t block_fix_width;
    uint8_t block_num;
    uint16_t flags_index;
    uint8_t color_mode;
    uint8_t invert_flags_mode;
    uint8_t default_color_sel;
    uint8_t invert_enable;
    uint8_t invert_mode;
    uint8_t invert_thresh;
    uint8_t unfix_index;
};

/* === rga_osd_info (来自 drmrga.h) === */
struct rga_osd_info {
    uint8_t enable;
    struct rga_osd_mode_ctrl mode_ctrl;
    struct rga_osd_invert_factor cal_factor;
    struct rga_osd_bpp2 bpp2_info;
    union {
        struct {
            uint32_t last_flags1;
            uint32_t last_flags0;
        };
        uint64_t last_flags;
    };
    union {
        struct {
            uint32_t cur_flags1;
            uint32_t cur_flags0;
        };
        uint64_t cur_flags;
    };
};

/* === rga_gauss_config (来自 drmrga.h) === */
struct rga_gauss_config {
    uint32_t size;
    uint64_t coe_ptr;
};

/*
 * === rga_info_t (来自 drmrga.h) ===
 *
 * c_RkRgaBlit(src, dst, src1) 的参数类型。
 * 结构体末尾的 reserve[386] 确保跨版本 ABI 兼容。
 */
typedef struct rga_info {
    int fd;
    void *virAddr;
    void *phyAddr;
    unsigned hnd; /* Linux: unsigned; Android: buffer_handle_t */
    int format;
    rga_rect_t rect;
    unsigned int blend;
    int bufferSize;
    int rotation;
    int color;
    int testLog;
    int mmuFlag; /* 1 = 使用虚拟地址 */
    int colorkey_en;
    int colorkey_mode;
    int colorkey_max;
    int colorkey_min;
    int scale_mode;
    int color_space_mode;
    int sync_mode; /* 1 = 同步模式 */
    rga_nn_t nn;
    rga_dither_t dither;
    int rop_code;
    int rd_mode;
    unsigned short is_10b_compact;
    unsigned short is_10b_endian;
    int in_fence_fd;
    int out_fence_fd;
    int core;
    int priority;
    unsigned short enable;
    int handle;
    struct rga_mosaic_info mosaic_info;
    struct rga_osd_info osd_info;
    struct rga_pre_intr_info pre_intr;
    int mpi_mode;
    union {
        int ctx_id;
        int job_handle;
    };
    uint16_t rgba5551_flags;
    uint8_t rgba5551_alpha0;
    uint8_t rgba5551_alpha1;
    struct rga_gauss_config gauss_config;
    char reserve[386];
} rga_info_t;

/* === C API 函数声明 (来自 RgaApi.h) === */
int c_RkRgaInit(void);
void c_RkRgaDeInit(void);
void c_RkRgaGetContext(void **ctx);
int c_RkRgaBlit(rga_info_t *src, rga_info_t *dst, rga_info_t *src1);
int c_RkRgaColorFill(rga_info_t *dst);
int c_RkRgaFlush(void);

#ifdef __cplusplus
}
#endif

#endif /* RGA_INFO_H */
