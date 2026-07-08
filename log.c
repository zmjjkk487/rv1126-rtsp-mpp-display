#include "log.h"
#include <stdarg.h>
#include <string.h>

static log_level_t g_log_level = LOG_INFO;
static FILE *g_log_file = NULL;

void log_set_level(log_level_t lv) { g_log_level = lv; }
void log_set_file(FILE *fp) { g_log_file = fp; }

void log_write(log_level_t lv, const char *fmt, ...) {
    if (lv < g_log_level)
        return;
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    fprintf(stderr, "[%02d:%02d:%02d] %s\n", t.tm_hour, t.tm_min, t.tm_sec,
            buf);
    if (g_log_file) {
        fprintf(g_log_file, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
                t.tm_sec, buf);
        fflush(g_log_file);
    }
}
