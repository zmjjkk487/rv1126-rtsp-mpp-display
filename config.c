#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t')
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    while (strlen(s) > 0 &&
           (s[strlen(s) - 1] == ' ' || s[strlen(s) - 1] == '\t'))
        s[strlen(s) - 1] = '\0';
}

void config_parse(const char *path, config_t *c) {
    memset(c, 0, sizeof(*c));
    snprintf(c->rtsp_url, sizeof(c->rtsp_url), "rtsp://192.168.50.10/test.264");
    snprintf(c->fb_device, sizeof(c->fb_device), "/dev/fb0");
    c->rtsp_transport = 1;
    c->log_level = LOG_INFO;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOGW("无配置文件 %s, 用默认值", path);
        return;
    }

    char line[MAX_LINE], key[80], val[MAX_PATH];
    while (fgets(line, sizeof(line), fp)) {
        char *p = strchr(line, '#');
        if (p)
            *p = '\0';
        p = strchr(line, '\n');
        if (p)
            *p = '\0';
        if (sscanf(line, "%79[^=]=%271[^\n]", key, val) != 2)
            continue;
        trim(key);
        trim(val);
        if (!strcmp(key, "rtsp_url"))
            snprintf(c->rtsp_url, sizeof(c->rtsp_url), "%s", val);
        else if (!strcmp(key, "rtsp_user")) {
            strncpy(c->rtsp_user, val, sizeof(c->rtsp_user) - 1);
            c->rtsp_user[sizeof(c->rtsp_user) - 1] = '\0';
        } else if (!strcmp(key, "rtsp_pass")) {
            strncpy(c->rtsp_pass, val, sizeof(c->rtsp_pass) - 1);
            c->rtsp_pass[sizeof(c->rtsp_pass) - 1] = '\0';
        } else if (!strcmp(key, "rtsp_transport"))
            c->rtsp_transport = strcmp(val, "udp") ? 1 : 0;
        else if (!strcmp(key, "fb_device"))
            snprintf(c->fb_device, sizeof(c->fb_device), "%s", val);
        else if (!strcmp(key, "log_level")) {
            if (!strcmp(val, "debug"))
                c->log_level = LOG_DEBUG;
            else if (!strcmp(val, "info"))
                c->log_level = LOG_INFO;
            else if (!strcmp(val, "warn"))
                c->log_level = LOG_WARN;
            else if (!strcmp(val, "error"))
                c->log_level = LOG_ERROR;
        }
    }
    fclose(fp);
}
