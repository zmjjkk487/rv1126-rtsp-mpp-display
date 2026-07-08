#ifndef CONFIG_H
#define CONFIG_H

#include "log.h"

#define MAX_PATH 272
#define MAX_LINE 512
#define DEFAULT_FPS 30

typedef enum {
    DISPLAY_FBDEV = 0, /* 传统 fbdev 帧缓冲 */
    DISPLAY_DRM = 1,   /* DRM/KMS (需要 libdrm) */
} display_type_t;

typedef struct {
    char rtsp_url[MAX_PATH];
    char rtsp_user[64];
    char rtsp_pass[64];
    int rtsp_transport;
    int display_width;
    int display_height;
    char fb_device[MAX_PATH]; /* fbdev: /dev/fb0   DRM: /dev/dri/card0 */
    int display_type;         /* DISPLAY_FBDEV / DISPLAY_DRM */
    int target_fps;
    log_level_t log_level;
} config_t;

void config_parse(const char *path, config_t *c);

#endif
