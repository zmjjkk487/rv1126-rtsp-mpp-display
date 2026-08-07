/* webrtc_test.c — WebRTC 最小验证: 板子端 answer 服务
 *
 * 管线: videotestsrc → mpph264enc → rtph264pay → webrtcbin
 * 信令: TCP :12345, JSON 行协议
 *   收: {"type":"offer","sdp":"..."}
 *   回: {"type":"answer","sdp":"..."}
 *
 * 验证方式: 本机 aiortc 脚本做 offer 端, 收流数帧。
 * 用法: ./webrtc_test [端口]
 */
#define _POSIX_C_SOURCE 200809L

#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_PORT 12345

static GstElement *g_webrtc = NULL;
static GMainLoop *g_loop = NULL;
static int g_answer_fd = -1;
static char g_answer_sdp[16384] = "";

/* ---- 简单 JSON 提取: {"type":"xxx","sdp":"..."} ---- */
static void json_get(const char *json, const char *key, char *out, size_t cap) {
    out[0] = '\0';
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e) return;
    size_t n = (size_t)(e - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* ---- webrtcbin 信号 ---- */

static void on_negotiation_needed(GstElement *webrtc, gpointer user_data) {
    (void)webrtc; (void)user_data;
    /* answer 端: offer 由对端发, 不需要主动协商 */
}

static void on_answer_created(GstPromise *promise, gpointer user_data) {
    (void)user_data;
    GstWebRTCSessionDescription *desc = NULL;
    const GstStructure *reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                      &desc, NULL);

    gchar *sdp = gst_sdp_message_as_text(desc->sdp);
    g_print("[webrtc] answer 已创建 (%zu 字节)\n", strlen(sdp));
    if (g_answer_fd >= 0) {
        char resp[17000];
        snprintf(resp, sizeof resp,
                 "{\"type\":\"answer\",\"sdp\":\"%s\"}\n", sdp);
        write(g_answer_fd, resp, strlen(resp));
        close(g_answer_fd);
        g_answer_fd = -1;
    }
    g_free(sdp);
    gst_webrtc_session_description_free(desc);
}

static void on_offer_set(GstPromise *promise, gpointer user_data) {
    (void)user_data;
    if (gst_promise_wait(promise) != GST_PROMISE_RESULT_REPLIED) {
        g_printerr("[webrtc] 设置 offer 失败\n");
        return;
    }
    /* offer 设置成功 → 创建 answer */
    gst_webrtc_bin_create_answer(GST_WEBRTC_BIN(g_webrtc), NULL,
                                 on_answer_created, NULL);
}

static void on_ice_candidate(GstElement *webrtc, guint mlineindex,
                             gchar *candidate, gpointer user_data) {
    (void)webrtc; (void)mlineindex; (void)user_data;
    /* 局域网 host candidate 已包含在 SDP 中, trickle 可忽略;
     * 打印便于调试 */
    if (candidate)
        g_print("[webrtc] ICE candidate: %s\n", candidate);
}

/* ---- 信令: TCP 收 offer → 回 answer ---- */

static void *signaling_thread(void *arg) {
    int port = *(int *)arg;
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons((uint16_t)port),
    };
    if (bind(sfd, (struct sockaddr *)&a, sizeof a) < 0 ||
        listen(sfd, 4) < 0) {
        perror("bind/listen");
        return NULL;
    }
    g_print("[webrtc] 信令端口 %d, 等 offer...\n", port);

    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) continue;

        /* 读 JSON 行 */
        char buf[16384];
        size_t n = 0;
        while (n < sizeof buf - 1) {
            ssize_t r = read(cfd, buf + n, 1);
            if (r <= 0) break;
            n++;
            if (buf[n-1] == '\n') break;
        }
        buf[n] = '\0';

        char type[32] = "", sdp[16000] = "";
        json_get(buf, "type", type, sizeof type);
        if (strcmp(type, "offer") != 0) {
            g_printerr("[webrtc] 非 offer: %s\n", type);
            close(cfd);
            continue;
        }
        json_get(buf, "sdp", sdp, sizeof sdp);
        g_print("[webrtc] 收到 offer (%zu 字节)\n", strlen(sdp));

        GstSDPMessage *msg = NULL;
        if (gst_sdp_message_new(&msg) != GST_SDP_OK ||
            gst_sdp_message_parse_buffer((guint8 *)sdp, strlen(sdp), msg)
                != GST_SDP_OK) {
            g_printerr("[webrtc] SDP 解析失败\n");
            close(cfd);
            continue;
        }
        GstWebRTCSessionDescription *offer =
            gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, msg);

        g_answer_fd = cfd;
        gst_webrtc_bin_set_remote_description(GST_WEBRTC_BIN(g_webrtc),
                                              offer, on_offer_set, NULL);
        gst_webrtc_session_description_free(offer);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    gst_init(&argc, &argv);
    setvbuf(stdout, NULL, _IONBF, 0);

    /* 管线: 测试源 → 硬编码 → RTP H264 → webrtcbin */
    GstElement *pipe = gst_parse_launch(
        "videotestsrc is-live=true pattern=ball ! "
        "videoconvert ! video/x-raw,format=NV12 ! "
        "mpph264enc bps=2000000 ! "
        "rtph264pay config-interval=1 pt=96 ! "
        "webrtcbin name=wb bundle-policy=max-bundle", NULL);
    if (!pipe) {
        g_printerr("管线构建失败\n");
        return 1;
    }
    g_webrtc = gst_bin_get_by_name(GST_BIN(pipe), "wb");
    if (!g_webrtc) return 1;

    g_signal_connect(g_webrtc, "negotiation-needed",
                     G_CALLBACK(on_negotiation_needed), NULL);
    g_signal_connect(g_webrtc, "on-ice-candidate",
                     G_CALLBACK(on_ice_candidate), NULL);

    if (gst_element_set_state(pipe, GST_STATE_PLAYING)
            == GST_STATE_CHANGE_FAILURE) {
        g_printerr("管线启动失败\n");
        return 1;
    }

    pthread_t tid;
    pthread_create(&tid, NULL, signaling_thread, &port);

    g_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(g_loop);
    return 0;
}
