#ifndef CONFIG_H
#define CONFIG_H

#include "log.h"

#define MAX_PATH 272
#define MAX_LINE 512
#define DEFAULT_FPS 30

typedef struct {
    char rtsp_url[MAX_PATH];
    int  rtsp_transport;       /* 1=tcp, 0=udp */
    char fb_device[MAX_PATH];  /* /dev/fb0 */
    log_level_t log_level;
} config_t;

void config_parse(const char *path, config_t *c);

#endif
