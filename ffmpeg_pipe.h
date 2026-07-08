#ifndef FFMPEG_PIPE_H
#define FFMPEG_PIPE_H

#include "config.h"
#include <unistd.h>

/* 返回: 管道读端 fd, fork 的 pid 写入 *out_pid。失败返回 -1 */
int ffmpeg_pipe_open(const config_t *c, pid_t *out_pid);
void ffmpeg_pipe_close(int fd, pid_t pid);

#endif
