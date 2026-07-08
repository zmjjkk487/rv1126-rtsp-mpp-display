#ifndef LOG_H
#define LOG_H

#include <errno.h>
#include <stdio.h>
#include <time.h>


typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } log_level_t;

void log_set_level(log_level_t lv);
void log_set_file(FILE *fp);
void log_write(log_level_t lv, const char *fmt, ...);

#define LOGI(...) log_write(LOG_INFO, __VA_ARGS__)
#define LOGW(...) log_write(LOG_WARN, __VA_ARGS__)
#define LOGE(...) log_write(LOG_ERROR, __VA_ARGS__)
#define LOGD(...) log_write(LOG_DEBUG, __VA_ARGS__)

#endif
