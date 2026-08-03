/*
 * RV1126 RTSP 硬解码 显示 Demo
 * ============================================
 * 平台: Rockchip RV1126, Buildroot Linux
 * 架构: FFmpeg avformat 拆帧 → MPP 硬解 H.264 → (RGA|CPU) NV12→RGB →
 * (DRM|fbdev) 显示 编译: ./build.sh    运行: ./rv1126_rtsp_mpp config.ini
 *
 * 数据流:
 *   RTSP 摄像头 → FFmpeg(avformat) 拉流 → H.264 Annex-B 裸码流
 *   → MPP decode_put_packet / decode_get_frame → NV12 YUV 帧
 *   → RGA(硬件) / CPU(软件) NV12→RGB 颜色转换
 *   → DRM(硬件) / fbdev(软件) 显示到 MIPI 屏幕
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "fbdev.h"
#include "ffmpeg_demux.h"
#include "log.h"
#include "mpp_dec.h"

#define CONFIG_FILE "config.ini"
#define MAX_RECONNECT_DELAY 30

static volatile int running = 0;
static int frame_cnt = 0;
static time_t t_start = 0;

static void on_signal(int s) {
    (void)s;
    running = 0;
}

int main(int argc, char *argv[]) {
    const char *cfg_path = CONFIG_FILE;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("RV1126 RTSP 硬解码 显示 Demo (Rockchip MPP)\n\n");
            printf("用法: %s [config.ini]\n\n", argv[0]);
            printf(
                "数据流: FFmpeg(拉RTSP) → MPP(硬解H.264) → RGA/CPU(NV12→RGB) → "
                "DRM/fbdev → 屏幕\n");
            return 0;
        } else if (argv[i][0] != '-') {
            cfg_path = argv[i];
        }
    }

    config_t cfg;
    config_parse(cfg_path, &cfg);
    log_set_level(cfg.log_level);

    FILE *lf = fopen("/var/log/rv1126_rtsp_mpp.log", "a");
    log_set_file(lf);
    setbuf(stderr, NULL);

    LOGI("========================================");
    LOGI("RV1126 RTSP 硬解码 显示 Demo (Rockchip MPP)");
    LOGI("========================================");
    LOGI("RTSP: %s", cfg.rtsp_url);
    LOGI("显示: %s (%dx%d)", cfg.fb_device, cfg.display_width,
         cfg.display_height);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* 1. 显示设备初始化 */
    fb_t fb;
    LOGI("[1/3] 初始化 fbdev (%s)...", cfg.fb_device);
    if (fb_init(&fb, cfg.fb_device) < 0)
        goto exit;
    running = 1;
    t_start = time(NULL);

    int backoff = 1;
    mpp_dec_t dec;
    int mpp_ok = 0;

    while (running) {
        /* 2. FFmpeg 拉流 */
        LOGI("--- 连接 RTSP (backoff=%ds) ---", backoff);
        ffmpeg_demux_t *demux = ffmpeg_demux_open(&cfg);
        if (!demux) {
            if (!running)
                break;
            sleep(backoff);
            if (backoff < MAX_RECONNECT_DELAY)
                backoff *= 2;
            continue;
        }
        backoff = 1;

        /* 3. 初始化 MPP（只做一次，避免反复 create/destroy 导致段错误）
         * 正点原子 SDK MPP (alientek) 可能不支持多次 create/destroy。
         * 分辨率变化由 MPP 内部 info_change 机制自动处理。 */
        if (!mpp_ok) {
            LOGI("[2/3] 初始化 MPP 硬解码器 (%dx%d)...", demux->width,
                 demux->height);
            if (mpphw_dec_init(&dec, demux->width, demux->height) < 0) {
                LOGW("MPP 初始化失败");
                ffmpeg_demux_close(demux);
                sleep(backoff);
                if (backoff < MAX_RECONNECT_DELAY)
                    backoff *= 2;
                continue;
            }
            mpp_ok = 1;
        }

        /* 4. 解码+显示循环 */
        LOGI("[3/3] 解码循环开始 %dx%d", demux->width, demux->height);
        int stream_dead = 0;

        while (running && !stream_dead) {
            uint8_t *frame_data;
            int frame_size = ffmpeg_demux_read(demux, &frame_data);
            if (frame_size <= 0) {
                stream_dead = 1;
                break;
            }

            uint8_t *y = NULL, *uv = NULL;
            int fw = 0, fh = 0, fs = 0, fvs = 0;

            int ret = mpphw_dec_decode(&dec, frame_data, frame_size, &y, &uv,
                                       &fw, &fh, &fs, &fvs);
            if (ret < 0)
                continue;
            if (ret > 0) {
                if (y && uv && fw > 0 && fh > 0) {
                    fb_show_nv12(&fb, y, uv, fw, fh, fs, fvs);
                    frame_cnt++;
                }
                mpphw_dec_return(&dec); /* 无论是否显示，必须释放帧 */
            }

            /* 帧率控制（放在 decode 之后，避免阻塞解码） */
            if (cfg.target_fps > 0) {
                static struct timespec last;
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long el = (now.tv_sec - last.tv_sec) * 1000000L +
                          (now.tv_nsec - last.tv_nsec) / 1000L;
                long tgt = 1000000L / cfg.target_fps;
                if (el > 0 && el < tgt)
                    usleep(tgt - el);
                last = now;
            }

            /* 定期统计 */
            if (frame_cnt % 300 == 0) {
                double el = difftime(time(NULL), t_start);
                LOGI("帧=%d 运行=%.0fs fps=%.1f 分辨率=%dx%d", frame_cnt, el,
                     el > 0 ? frame_cnt / el : 0, fw, fh);
            }
        }

        ffmpeg_demux_close(demux);
        if (!running)
            break;
        LOGW("%ds 后重连...", backoff);
        sleep(backoff);
        if (backoff < MAX_RECONNECT_DELAY)
            backoff *= 2;
    }

    if (mpp_ok)
        mpphw_dec_deinit(&dec);
    fb_deinit(&fb);
exit: {
    double el = difftime(time(NULL), t_start);
    LOGI("退出. 总帧=%d, 运行=%.0fs, fps=%.1f", frame_cnt, el,
         el > 0 ? frame_cnt / el : 0);
}
    if (lf)
        fclose(lf);
    return 0;
}
