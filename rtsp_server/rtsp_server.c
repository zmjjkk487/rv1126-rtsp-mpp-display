/* rtsp_server.c — 极简 RTSP 服务器 (H.264, TCP interleaved)
 *
 * 协议依据:
 *   RFC 2326  RTSP 1.0  (OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN)
 *   RFC 4566  SDP
 *   RFC 6184  H.264 over RTP  (单 NAL + FU-A 分片)
 *
 * 核心思路 ("RTSP 怎么控制 RTP"):
 *   每个客户端一个结构体 rtsp_session, 它同时装着
 *   "RTSP 请求改的字段"(state, session_id) 和
 *   "RTP 发送用的字段"(ssrc, seq, ts)。
 *   RTSP 线程收到 PLAY 就把 state 置为 PLAYING,
 *   feed_nal() 遍历会话表, 只给 PLAYING 的会话分包发送。
 *   两个协议的联系 = 这个结构体。
 *
 * 传输: 只实现 TCP interleaved (RTSP 与 RTP 共用一条 TCP 连接,
 *       媒体帧格式: 0x24 + channel(1B) + 长度(2B大端) + RTP包)。
 *
 * 并发模型 (v2, 审查修复后):
 *   - 客户端 socket 全程 O_NONBLOCK + poll; 读请求有超时,
 *     PLAY 后客户端安静不踢 (播放中静默是正常的)。
 *   - RTP 发送非阻塞: 慢客户端 → send EAGAIN → 丢弃本帧 (实时丢帧策略,
 *     与真实摄像头一致), 绝不阻塞 feed 线程和其他会话。
 *   - state 用 C11 atomic; sps/pps 副本读写均在锁内。
 *
 * 无 GStreamer 依赖, 纯 C11 + POSIX, 可直接交叉编译上板。
 */
#include "rtsp_server.h"
#include "nal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define RTP_PT         96       /* SDP 里声明的负载类型 */
#define RTP_CLOCK      90000    /* H.264 的 RTP 时钟 */
#define FRAME_TS_STEP  3600     /* 默认 25fps → 90000/25 (可用 set_frame_step 改) */
#define MAX_PAYLOAD    1400     /* 单 RTP 包负载上限 */
#define SOCK_TIMEOUT_MS 5000    /* 读请求超时 */
#define REQ_BUF        4096

enum { ST_DEAD = -1, ST_INIT = 0, ST_READY, ST_PLAYING };

/* ---------------- 会话: RTSP 与 RTP 的"联系" ---------------- */
struct rtsp_mount {
    char path[64];               /* 如 "/stream" */
    uint8_t *sps, *pps;          /* SDP 用的参数集副本 (锁内读写) */
    size_t sps_len, pps_len;
    uint32_t frame_step;         /* RTP 时间戳每帧步进 (默认 3600 = 25fps) */
    pthread_mutex_t lock;        /* 保护 sps/pps/frame_step */
    rtsp_server_t *owner;        /* 所属服务器 (feed 遍历会话用) */
    struct rtsp_mount *next;
};

typedef struct rtsp_session {
    int fd;
    struct sockaddr_in peer;
    uint32_t ssrc, seq, ts;      /* RTP 头字段 */
    atomic_int state;            /* RTSP 线程写, feed 线程读 (C11 原子) */
    rtsp_mount_t *mount;         /* SETUP 时按 URL 路径绑定 */
    char session_id[32];
    rtsp_server_t *srv;
    struct rtsp_session *next;
    pthread_mutex_t send_lock;   /* 串行化同一 socket 的写入 */
    unsigned long drops;         /* 丢弃帧计数 (慢客户端) */
} rtsp_session;

struct rtsp_server {
    int listen_fd;
    int port;
    rtsp_mount_t *mounts;        /* 挂载点链表 */
    uint32_t next_ssrc;
    pthread_mutex_t lock;
    rtsp_session *sessions;      /* 会话链表 */
};

/* ---------------- 小工具 ---------------- */

/* SDP 的 sprop-parameter-sets 需要 SPS/PPS 的 base64
 * out 容量 cap, 最多写 cap-1 字节 + 结尾 '\0'; 返回实际长度 */
static size_t b64_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;
    while (i + 2 < n && o + 4 < cap - 1) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = t[(v >> 6) & 63];
        out[o++] = t[v & 63];
        i += 3;
    }
    if (i < n && o + 4 < cap - 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        int two = (n - i == 2);
        if (two) v |= (uint32_t)in[i+1] << 8;
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = two ? t[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

/* 非阻塞写全部 (RTSP 应答用): EAGAIN 时 poll 等可写, 有界等待
 * 返回 0 成功, -1 失败(对端死亡/超时) */
static int write_all(int fd, const uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t w = send(fd, buf, n, MSG_NOSIGNAL);
        if (w > 0) { buf += w; n -= (size_t)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            if (poll(&p, 1, SOCK_TIMEOUT_MS) <= 0)
                return -1;
            continue;
        }
        return -1;   /* EOF / ECONNRESET / EPIPE / EINTR(简单处理) */
    }
    return 0;
}

/* RTP 帧发送: 满则丢弃余下部分 (慢客户端丢帧, 不阻塞)
 * 返回 0 成功, -1 失败; errno 保留给调用方判断是 EAGAIN 还是真错误 */
static int write_rtp(int fd, const uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t w = send(fd, buf, n, MSG_NOSIGNAL);
        if (w > 0) { buf += w; n -= (size_t)w; continue; }
        return -1;   /* EAGAIN → 丢帧; 其他错误 → 判死 */
    }
    return 0;
}

/* TCP interleaved 帧: $ + channel + 2 字节大端长度 + RTP 包 */
static int send_rtsp_frame(rtsp_session *s, const uint8_t *rtp, size_t n) {
    uint8_t hdr[4] = { 0x24, 0 /* channel 0 */,
                       (uint8_t)(n >> 8), (uint8_t)(n & 0xff) };
    int err = 0;
    if (write_rtp(s->fd, hdr, 4) < 0) err = errno;
    else if (write_rtp(s->fd, rtp, n) < 0) err = errno;
    if (err) {
        s->drops++;
        if (err != EAGAIN && err != EWOULDBLOCK)
            atomic_store(&s->state, ST_DEAD);   /* 真错误: 会话报废 */
        return -1;
    }
    return 0;
}

/* ---------------- RTP 打包 ---------------- */

/* 12 字节 RTP 头: V=2 P=0 X=0 CC=0, M 标记, PT, seq, ts, ssrc (全部大端) */
static void fill_rtp_hdr(uint8_t *h, uint32_t seq, uint32_t ts,
                         uint32_t ssrc, int mark) {
    h[0] = 0x80;
    h[1] = (uint8_t)((mark ? 0x80 : 0) | RTP_PT);
    h[2] = (uint8_t)(seq >> 8);  h[3] = (uint8_t)seq;
    h[4] = (uint8_t)(ts >> 24);  h[5] = (uint8_t)(ts >> 16);
    h[6] = (uint8_t)(ts >> 8);   h[7] = (uint8_t)ts;
    h[8] = (uint8_t)(ssrc >> 24); h[9] = (uint8_t)(ssrc >> 16);
    h[10] = (uint8_t)(ssrc >> 8); h[11] = (uint8_t)ssrc;
}

/* 小 NAL (≤1400): 原样装进一个 RTP 包 (载荷 = 完整 NAL 含头) */
static void send_single(rtsp_session *s, const uint8_t *nal, size_t len) {
    uint8_t pkt[12 + MAX_PAYLOAD];
    fill_rtp_hdr(pkt, s->seq++, s->ts, s->ssrc, 1);
    memcpy(pkt + 12, nal, len);
    send_rtsp_frame(s, pkt, 12 + len);
}

/* 大 NAL: FU-A 分片。拆成 N 个包, 每包 14 字节头 + 一块负载 */
static void send_fua(rtsp_session *s, const uint8_t *nal, size_t len) {
    uint8_t type = nal[0] & 0x1F;
    uint8_t fu_ind = (uint8_t)((nal[0] & 0xE0) | 28);  /* F|NRI + FU-A=28 */
    uint8_t pkt[14 + MAX_PAYLOAD];
    size_t off = 1;   /* 跳过 NAL 头, 负载从第 2 字节开始切 */
    int first = 1;

    while (off < len) {
        size_t chunk = len - off;
        if (chunk > MAX_PAYLOAD) chunk = MAX_PAYLOAD;
        int last = (off + chunk >= len);

        fill_rtp_hdr(pkt, s->seq++, s->ts, s->ssrc, last);
        pkt[12] = fu_ind;
        pkt[13] = (uint8_t)((first ? 0x80 : 0) | (last ? 0x40 : 0) | type);
        memcpy(pkt + 14, nal + off, chunk);
        if (send_rtsp_frame(s, pkt, 14 + chunk) < 0)
            break;   /* 中途失败: 丢弃剩余分片, 不产生半截 NAL (防花屏) */

        off += chunk;
        first = 0;
    }
}

/* ---------------- SDP ---------------- */

static void build_sdp(rtsp_mount_t *m, char *out, size_t n) {
    char sps_b64[512] = "", pps_b64[512] = "", pli[16] = "";

    /* 快照参数集 (锁内读), 避免与 feed 写入竞争 */
    pthread_mutex_lock(&m->lock);
    const uint8_t *sps = m->sps;
    size_t sps_len = m->sps_len;
    const uint8_t *pps = m->pps;
    size_t pps_len = m->pps_len;
    pthread_mutex_unlock(&m->lock);

    if (sps && sps_len >= 4) {   /* profile-level-id 取 SPS 的 1~3 字节 */
        b64_encode(sps, sps_len, sps_b64, sizeof sps_b64);
        snprintf(pli, sizeof pli, "%02X%02X%02X", sps[1], sps[2], sps[3]);
    }
    if (pps)
        b64_encode(pps, pps_len, pps_b64, sizeof pps_b64);

    snprintf(out, n,
        "v=0\r\n"
        "o=- 0 0 IN IP4 0.0.0.0\r\n"
        "s=RV1126 Live Stream\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP %d\r\n"
        "a=rtpmap:%d H264/90000\r\n"
        "a=fmtp:%d packetization-mode=1;profile-level-id=%s;"
        "sprop-parameter-sets=%s,%s\r\n",
        RTP_PT, RTP_PT, RTP_PT,
        pli[0] ? pli : "000000",
        sps_b64[0] ? sps_b64 : "AAAA",
        pps_b64[0] ? pps_b64 : "AAAA");
}

/* ---------------- RTSP 应答 ---------------- */

/* 有界追加, 防止 snprintf 链越界 */
static void resp_append(char *resp, size_t cap, int *n,
                        const char *fmt, ...) {
    if (*n >= (int)cap) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(resp + *n, cap - (size_t)*n, fmt, ap);
    va_end(ap);
    if (w < 0) { *n = (int)cap; return; }   /* 出错即封死 */
    *n += w;
    if (*n >= (int)cap) *n = (int)cap;
}

/* 返回 0 成功, -1 发送失败 (调用方应断开) */
static int rtsp_reply(rtsp_session *s, int cseq, const char *status,
                      const char *extra, const char *body) {
    char resp[2048];
    int n = 0;
    resp_append(resp, sizeof resp, &n,
                "RTSP/1.0 %s\r\nCSeq: %d\r\n", status, cseq);
    if (extra)
        resp_append(resp, sizeof resp, &n, "%s", extra);
    if (body)
        resp_append(resp, sizeof resp, &n,
                    "Content-Type: application/sdp\r\n"
                    "Content-Length: %zu\r\n", strlen(body));
    resp_append(resp, sizeof resp, &n, "\r\n");
    if (body)
        resp_append(resp, sizeof resp, &n, "%s", body);

    pthread_mutex_lock(&s->send_lock);
    int rc = write_all(s->fd, (const uint8_t *)resp, (size_t)n);
    pthread_mutex_unlock(&s->send_lock);
    return rc;
}

/* ---------------- RTSP 请求处理 ---------------- */

static void remove_session(rtsp_server_t *srv, rtsp_session *s) {
    pthread_mutex_lock(&srv->lock);
    rtsp_session **pp = &srv->sessions;
    while (*pp && *pp != s) pp = &(*pp)->next;
    if (*pp) *pp = s->next;
    pthread_mutex_unlock(&srv->lock);
}

/* 等 socket 可读 (SOCK_TIMEOUT_MS)
 * 返回 1 可读, -1 超时, -2 错误/对端关闭 */
static int wait_readable(rtsp_session *s) {
    struct pollfd p = { .fd = s->fd, .events = POLLIN };
    int pr = poll(&p, 1, SOCK_TIMEOUT_MS);
    if (pr == 0) return -1;
    if (pr < 0) { if (errno == EINTR) return wait_readable(s); return -2; }
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return -2;
    return 1;
}

/* 非阻塞读满 len 字节 (EAGAIN 时重新 poll, 不忙等)
 * 返回 0 成功, -1 超时, -2 EOF/错误 */
static int recv_exact(rtsp_session *s, uint8_t *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t r = recv(s->fd, buf + got, len - got, 0);
        if (r > 0) { got += (size_t)r; continue; }
        if (r == 0) return -2;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            int wr = wait_readable(s);
            if (wr < 0) return wr;
            continue;
        }
        return -2;
    }
    return 0;
}

/* 非阻塞读 1 字节; 返回 0 成功, -1 超时, -2 EOF/错误 */
static int recv_byte(rtsp_session *s, uint8_t *out) {
    ssize_t r = recv(s->fd, out, 1, 0);
    if (r > 0) return 0;
    if (r == 0) return -2;
    if (errno == EINTR) return recv_byte(s, out);
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        int wr = wait_readable(s);
        if (wr < 0) return wr;
        return recv_byte(s, out);
    }
    return -2;
}

/* 读下一段入站数据, 分两种情况:
 *   a) 首字节 0x24 → TCP interleaved 帧 (客户端回传的 RTCP RR):
 *      按 [channel][2字节大端长度][负载] 读掉并丢弃, 返回 0
 *      注意: 一旦开始读一个 interleaved 帧就必须读完 (字节精确定位),
 *      中途超时视为帧错位 → 返回 -2 断开 (会话已不可信)
 *   b) 其他 → RTSP 请求, 读到空行 (\r\n\r\n 或 \n\n), 返回 1
 * 返回 -1 超时(会话可继续), -2 EOF/错误。
 * 关键: PLAY 后客户端会在同一连接上发 RTCP, 若不分流会把它当
 * 请求解析并回 405, 污染交错流导致 live555 等客户端断流。 */
static int read_next(rtsp_session *s, char *buf, size_t cap) {
    for (;;) {
        int wr = wait_readable(s);
        if (wr < 0) return wr;              /* -1 超时, -2 错误 */

        uint8_t first;
        if (recv_byte(s, &first) != 0)      /* 刚 poll 过, 正常立即成功 */
            return -2;

        if (first == 0x24) {                /* interleaved 帧: 丢弃 */
            uint8_t hdr[3];
            if (recv_exact(s, hdr, sizeof hdr) != 0) return -2;
            uint32_t flen = ((uint32_t)hdr[1] << 8) | hdr[2];
            uint8_t junk[512];
            while (flen > 0) {
                size_t want = (flen > sizeof junk) ? sizeof junk : flen;
                if (recv_exact(s, junk, want) != 0) return -2;
                flen -= (uint32_t)want;
            }
            return 0;
        }

        /* RTSP 请求: 继续读直到空行 */
        size_t n = 0;
        buf[n++] = (char)first;
        for (;;) {
            if (n >= cap - 1) { buf[n] = '\0'; return 1; }   /* 过长截断 */
            if (buf[n-1] == '\n' &&
                ((n >= 4 && buf[n-4] == '\r' && buf[n-3] == '\n' &&
                  buf[n-2] == '\r') ||
                 (n >= 2 && buf[n-2] == '\n')))
                break;
            uint8_t ch;
            int r = recv_byte(s, &ch);      /* EAGAIN → 内部重新 poll */
            if (r != 0) return r;           /* -1 超时(保留调用方语义), -2 EOF */
            buf[n++] = (char)ch;
        }
        buf[n] = '\0';
        return 1;
    }
}

/* 从请求行提取 URL 路径: rtsp://host:port/path → "/path" */
static void extract_path(const char *req, char *out, size_t cap) {
    out[0] = '\0';
    const char *p = strstr(req, "rtsp://");
    if (!p) return;
    const char *slash = strchr(p + 7, '/');
    if (!slash) return;
    const char *end = slash;
    while (*end && *end != ' ' && *end != '\r' && *end != '\n')
        end++;
    size_t n = (size_t)(end - slash);
    while (n > 1 && slash[n - 1] == '/')   /* 去掉尾部斜杠: /stream/ == /stream */
        n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, slash, n);
    out[n] = '\0';
}

static rtsp_mount_t *find_mount(rtsp_server_t *srv, const char *path) {
    pthread_mutex_lock(&srv->lock);
    for (rtsp_mount_t *m = srv->mounts; m; m = m->next) {
        if (strcmp(m->path, path) == 0) {
            pthread_mutex_unlock(&srv->lock);
            return m;
        }
    }
    pthread_mutex_unlock(&srv->lock);
    return NULL;
}

/* 返回 -1 → 断开连接 */
static int handle_request(rtsp_session *s, const char *req) {
    char method[16] = "";
    int cseq = 0;
    sscanf(req, "%15s", method);
    const char *cs = strstr(req, "CSeq:");
    if (cs) cseq = atoi(cs + 5);

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &s->peer.sin_addr, ip, sizeof ip);
    printf("[RTSP] %-9s cseq=%d 来自 %s:%d\n", method, cseq, ip,
           ntohs(s->peer.sin_port));

    if (strcmp(method, "OPTIONS") == 0) {
        return rtsp_reply(s, cseq, "200 OK",
                          "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n",
                          NULL);
    }
    if (strcmp(method, "DESCRIBE") == 0) {
        char path[64];
        extract_path(req, path, sizeof path);
        rtsp_mount_t *m = find_mount(s->srv, path);
        if (!m) {
            printf("[RTSP] 未知路径 \"%s\" → 404\n", path);
            return rtsp_reply(s, cseq, "404 Not Found", NULL, NULL);
        }
        char sdp[2048];
        build_sdp(m, sdp, sizeof sdp);
        printf("[RTSP] --- SDP 已下发 (%s) ---\n", path);
        return rtsp_reply(s, cseq, "200 OK", NULL, sdp);
    }
    if (strcmp(method, "SETUP") == 0) {
        char path[64];
        extract_path(req, path, sizeof path);
        rtsp_mount_t *m = find_mount(s->srv, path);
        if (!m) {
            printf("[RTSP] 未知路径 \"%s\" → 404\n", path);
            return rtsp_reply(s, cseq, "404 Not Found", NULL, NULL);
        }
        s->mount = m; 
        
        /* 会话绑定到挂载点: 只收该码流的 feed */
        /* 只支持 TCP interleaved, 无论客户端请求什么, 都回交错通道 */
        snprintf(s->session_id, sizeof s->session_id, "rv1126-%08x", s->ssrc);
        char extra[384];
        snprintf(extra, sizeof extra,
                 "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                 "Session: %s\r\n", s->session_id);
        int rc = rtsp_reply(s, cseq, "200 OK", extra, NULL);
        if (rc == 0)
            atomic_store(&s->state, ST_READY);
        return rc;
    }
    if (strcmp(method, "PLAY") == 0) {
        /* 状态机校验 (RFC 2326 §13): 没 SETUP(READY) 就 PLAY 是非法迁移,
         * 显式拒绝, 而不是默默接受后收不到数据 (曾经的缺陷) */
        int st = atomic_load(&s->state);
        if (st != ST_READY && st != ST_PLAYING) {
            printf("[RTSP] 非法状态迁移: PLAY 但 state=%d → 455\n", st);
            return rtsp_reply(s, cseq, "455 Method Not Valid in This State",
                              NULL, NULL);
        }
        char extra[384];
        snprintf(extra, sizeof extra,
                 "Session: %s\r\n"
                 "RTP-Info: url=rtsp://0.0.0.0:%d%s;seq=%u;rtptime=%u\r\n",
                 s->session_id, s->srv->port,
                 s->mount ? s->mount->path : "/stream", s->seq, s->ts);
        int rc = rtsp_reply(s, cseq, "200 OK", extra, NULL);
        if (rc == 0) {
            atomic_store(&s->state, ST_PLAYING);   /* ← RTSP 控制 RTP 的开关 */
            printf("[RTSP] ▶ PLAY → 开始推流 (ssrc=%08x)\n", s->ssrc);
        }
        return rc;
    }
    if (strcmp(method, "TEARDOWN") == 0) {
        rtsp_reply(s, cseq, "200 OK", NULL, NULL);
        return -1;
    }
    if (strcmp(method, "PAUSE") == 0 ||
        strcmp(method, "GET_PARAMETER") == 0) {
        return rtsp_reply(s, cseq, "200 OK", NULL, NULL);
    }
    return rtsp_reply(s, cseq, "405 Method Not Allowed", NULL, NULL);
}

static void *client_thread(void *arg) {
    rtsp_session *s = arg;
    char req[REQ_BUF];

    for (;;) {
        int r = read_next(s, req, sizeof req);
        if (r == 0) continue;                /* interleaved 帧 (RTCP) 已丢弃 */
        if (r == -1) {                       /* 客户端安静超时 */
            int st = atomic_load(&s->state);
            if (st == ST_DEAD) break;        /* feed 端已判死 */
            if (st != ST_PLAYING) break;     /* 握手期静默 → 断开 */
            continue;                        /* 播放中静默是正常的 */
        }
        if (r <= 0) break;                   /* EOF/错误 */
        if (handle_request(s, req) < 0) break;
    }

    printf("[RTSP] ■ 客户端断开 (ssrc=%08x, 丢帧=%lu)\n", s->ssrc, s->drops);
    remove_session(s->srv, s);
    close(s->fd);
    free(s);
    return NULL;
}

/* ---------------- 公开接口 ---------------- */

rtsp_server_t *rtsp_server_create(int port) {
    if (port == 0) port = 8554;

    rtsp_server_t *srv = calloc(1, sizeof *srv);
    if (!srv) return NULL;
    srv->port = port;
    srand((unsigned)(getpid() ^ (uintptr_t)srv));   /* seq/ts 随机起点 */

    signal(SIGPIPE, SIG_IGN);   /* 对端断开时 send 返回错误而非杀进程 */

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) { perror("socket"); free(srv); return NULL; }
    int on = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons((uint16_t)port),
    };
    if (bind(srv->listen_fd, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("bind"); close(srv->listen_fd); free(srv); return NULL;
    }
    if (listen(srv->listen_fd, 8) < 0) {
        perror("listen"); close(srv->listen_fd); free(srv); return NULL;
    }

    pthread_mutex_init(&srv->lock, NULL);
    return srv;
}

rtsp_mount_t *rtsp_server_add_mount(rtsp_server_t *srv, const char *path) {
    if (!srv || !path || !path[0]) return NULL;
    rtsp_mount_t *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    snprintf(m->path, sizeof m->path, "%s", path);
    m->frame_step = FRAME_TS_STEP;
    m->owner = srv;
    pthread_mutex_init(&m->lock, NULL);

    pthread_mutex_lock(&srv->lock);
    m->next = srv->mounts;
    srv->mounts = m;
    pthread_mutex_unlock(&srv->lock);
    return m;
}

int rtsp_server_start(rtsp_server_t *srv) {
    printf("[RTSP] 监听 %d — 客户端可用 "
           "rtsp://<板子IP>:%d/<路径> 拉流 (TCP interleaved)\n",
           srv->port, srv->port);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int fd = accept(srv->listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        /* 非阻塞 + TCP keepalive: 慢客户端丢帧不阻塞, 死对端最终被内核清理 */
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int ka = 1;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof ka);
#ifdef __linux__
        int idle = 30, ivl = 10, cnt = 3;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &ivl, sizeof ivl);
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
#endif

        rtsp_session *s = calloc(1, sizeof *s);
        if (!s) { close(fd); continue; }
        s->fd = fd;
        s->peer = peer;
        s->srv = srv;
        pthread_mutex_init(&s->send_lock, NULL);
        atomic_init(&s->state, ST_INIT);

        pthread_mutex_lock(&srv->lock);
        s->ssrc = 0x1000 + srv->next_ssrc++;   /* 每个会话一个同步源 */
        pthread_mutex_unlock(&srv->lock);
        s->seq = (uint16_t)(rand() & 0xffff);
        s->ts  = (uint32_t)(rand() % RTP_CLOCK);

        pthread_mutex_lock(&srv->lock);
        s->next = srv->sessions;
        srv->sessions = s;
        pthread_mutex_unlock(&srv->lock);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        printf("[RTSP] 新客户端 %s:%d (ssrc=%08x)\n", ip,
               ntohs(peer.sin_port), s->ssrc);

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, s) != 0) {
            perror("pthread_create");
            remove_session(srv, s);
            close(fd);
            free(s);
            continue;
        }
        pthread_detach(tid);
    }
    return 0;
}

/* feed: 编码器/文件源每产出一个 NAL 调一次。
 * 只给该挂载点上 PLAYING 状态的会话发; SPS/PPS 首次出现时存副本供 SDP 用。
 * 发送非阻塞: 慢客户端丢帧, 不影响其他会话和 feed 线程。 */
void rtsp_server_feed_nal(rtsp_mount_t *mount, const uint8_t *nal, size_t len) {
    if (!mount || len == 0) return;
    uint8_t type = nal[0] & 0x1F;

    pthread_mutex_lock(&mount->lock);
    if (type == 7 && !mount->sps) {
        mount->sps = malloc(len);
        if (mount->sps) { memcpy(mount->sps, nal, len); mount->sps_len = len; }
    }
    if (type == 8 && !mount->pps) {
        mount->pps = malloc(len);
        if (mount->pps) { memcpy(mount->pps, nal, len); mount->pps_len = len; }
    }
    uint32_t step = mount->frame_step;
    pthread_mutex_unlock(&mount->lock);

    rtsp_server_t *srv = mount->owner;
    if (!srv) return;

    pthread_mutex_lock(&srv->lock);
    for (rtsp_session *s = srv->sessions; s; s = s->next) {
        if (s->mount != mount)
            continue;
        if (atomic_load(&s->state) != ST_PLAYING)
            continue;

        pthread_mutex_lock(&s->send_lock);
        if (type == 1 || type == 5)          /* 每帧推进 RTP 时间戳 */
            s->ts += step;
        /* 注意: 不要发送前 poll 检查可写性 — 客户端解码器初始化时
         * 还没开始读 RTP, socket 满会误判为"慢客户端"整帧丢弃,
         * 连 SPS/PPS 都丢 → 解码器永远无法初始化 → 启动卡死。
         * 花屏防护只在 send_fua 中途失败时 break (不留半截 NAL) */
        if (len <= MAX_PAYLOAD)
            send_single(s, nal, len);
        else
            send_fua(s, nal, len);
        pthread_mutex_unlock(&s->send_lock);
    }
    pthread_mutex_unlock(&srv->lock);
}

void rtsp_server_set_frame_step(rtsp_mount_t *mount, uint32_t step) {
    if (!mount || step == 0) return;
    pthread_mutex_lock(&mount->lock);
    mount->frame_step = step;
    pthread_mutex_unlock(&mount->lock);
}

void rtsp_server_destroy(rtsp_server_t *srv) {
    if (!srv) return;
    close(srv->listen_fd);
    for (rtsp_mount_t *m = srv->mounts; m; ) {
        rtsp_mount_t *next = m->next;
        free(m->sps);
        free(m->pps);
        free(m);
        m = next;
    }
    free(srv);
}
