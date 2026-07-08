#include "ffmpeg_pipe.h"
#include "log.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


int ffmpeg_pipe_open(const config_t *c, pid_t *out_pid) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        LOGE("pipe: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOGE("fork: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        /* 脱离终端，避免 SIGTTOU */
        setsid();

        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        /* stdin 重定向到 /dev/null，stderr 保留以便调试 */
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) {
            dup2(nullfd, STDIN_FILENO);
            close(nullfd);
        }

        const char *t = c->rtsp_transport ? "tcp" : "udp";
        execlp("ffmpeg", "ffmpeg", "-loglevel", "warning", "-rtsp_transport", t,
               "-i", c->rtsp_url, "-c", "copy", "-an", "-f", "h264",
               "-flush_packets", "1", "-", (char *)NULL);
        _exit(1);
    }
    close(pipefd[1]);
    *out_pid = pid;
    LOGI("ffmpeg (pid=%d): -rtsp_transport %s -i %s -c copy -f h264 -", pid,
         c->rtsp_transport ? "tcp" : "udp", c->rtsp_url);
    return pipefd[0];
}

void ffmpeg_pipe_close(int fd, pid_t pid) {
    if (fd >= 0)
        close(fd);
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status, ok = 0;
        for (int i = 0; i < 10; i++) {
            if (waitpid(pid, &status, WNOHANG) == pid) {
                ok = 1;
                break;
            }
            usleep(100000);
        }
        if (!ok) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
        }
    }
}
