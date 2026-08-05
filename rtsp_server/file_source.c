/* file_source.c — 用文件模拟摄像头源 (本机验证 RTSP server 用)
 *
 * 用法: ./file_source <file.h264> [端口]
 * 行为: 解析文件的全部 NAL, 按 25fps 循环喂给 RTSP server
 */
#define _POSIX_C_SOURCE 200809L   /* 声明 usleep 等 POSIX 接口 */

#include "nal.h"
#include "rtsp_server.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define FPS 25

/* 按 25fps 睡一帧 (POSIX nanosleep, 跨平台可移植) */
static void frame_sleep(void) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000000 / FPS };
    nanosleep(&ts, NULL);
}

static void *accept_loop(void *arg) {
    rtsp_server_start((rtsp_server_t *)arg);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <file.h264> [端口]\n", argv[0]);
        return 1;
    }
    int port = (argc > 2) ? atoi(argv[2]) : 8554;

    setvbuf(stdout, NULL, _IONBF, 0);   /* 日志即时输出 (nohup 上板也必须) */

    /* 读文件到堆 */
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror("fopen"); return 1; }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)size);
    if (!buf) { perror("malloc"); fclose(fp); return 1; }
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        perror("fread");
        free(buf);
        fclose(fp);
        return 1;
    }
    fclose(fp);

    /* 切成 NAL 单元表 (data 指向 buf, buf 要活到进程结束) */
    nal_unit_t *nals = NULL;
    int n = 0;
    if (nal_split(buf, (size_t)size, &nals, &n) < 0) {
        fprintf(stderr, "NAL 解析失败\n");
        return 1;
    }
    printf("已解析 %d 个 NAL (SPS/PPS/IDR 等)\n", n);

    rtsp_server_t *srv = rtsp_server_create(port);
    if (!srv) return 1;

    /* 注册两个挂载点, 同一文件喂两路 (验证多挂载) */
    rtsp_mount_t *m1 = rtsp_server_add_mount(srv, "/stream");
    rtsp_mount_t *m2 = rtsp_server_add_mount(srv, "/stream1");
    if (!m1 || !m2) { fprintf(stderr, "挂载点注册失败\n"); return 1; }

    pthread_t tid;
    pthread_create(&tid, NULL, accept_loop, srv);

    printf("RTSP server 就绪: rtsp://<本机IP>:%d/stream 和 /stream1  (25fps 循环)\n", port);

    /* 循环喂流, 模拟摄像头一直在出帧 */
    for (;;) {
        for (int i = 0; i < n; i++) {
            rtsp_server_feed_nal(m1, nals[i].data, nals[i].len);
            rtsp_server_feed_nal(m2, nals[i].data, nals[i].len);
            if (nals[i].type == 1 || nals[i].type == 5)
                frame_sleep();   /* 每帧 40ms */
        }
    }
    return 0;
}
