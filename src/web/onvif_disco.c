/*
 * onvif_disco.c — ONVIF WS-Discovery 实现
 *
 * 参考: OASIS WS-Discovery 1.1 规范
 * 实际测试: 99% 的摄像头响应 2009 版 Probe
 */

#define _GNU_SOURCE
#include "onvif_disco.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/select.h>
#include <linux/in.h>

/* === Probe XML 模板 (WS-Discovery 2009 版, 带 ONVIF 类型筛选) === */

static const char *PROBE_XML =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
    "<e:Envelope"
    " xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
    " xmlns:d=\"http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01\""
    " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">\r\n"
    "<e:Header>\r\n"
    "<w:MessageID>urn:uuid:%08x-%04x-%04x-%04x-%04x%08x</w:MessageID>\r\n"
    "<w:To>urn:docs-oasis-open-org:ws-dd:ns:discovery:2009:01</w:To>\r\n"
    "<w:Action>http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01/Probe</w:Action>\r\n"
    "</e:Header>\r\n"
    "<e:Body>\r\n"
    "<d:Probe>\r\n"
    "<d:Types>dn:NetworkVideoTransmitter</d:Types>\r\n"
    "</d:Probe>\r\n"
    "</e:Body>\r\n"
    "</e:Envelope>\r\n";

/* === 辅助函数: 生成 UUID === */
static void make_uuid(char *buf, size_t sz) {
    unsigned r[6];
    FILE *fp = fopen("/dev/urandom", "r");
    if (fp) { fread(r, sizeof(r), 1, fp); fclose(fp); }
    else { for (int i = 0; i < 6; i++) r[i] = (unsigned)(time(NULL) ^ rand()); }
    snprintf(buf, sz,
             "%08x-%04x-%04x-%04x-%04x%08x",
             r[0], r[1] & 0xffff, r[2] & 0xffff,
             r[3] & 0xffff, r[4], r[5]);
}

/* === 解析 XML 标签内容: 返回 <tag>value</tag> 中间的值 (复制到 dst, 最大 dsz-1) === */
static int xml_extract(const char *xml, const char *tag, char *dst, size_t dsz) {
    char open[128], close[128];
    snprintf(open,  sizeof(open),  "<%s>",  tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    /* 支持带属性的标签: <d:XAddrs>... 但 tag="XAddrs", 要匹配 "<XAddrs" 或 "<d:XAddrs" */
    char attr[128];
    snprintf(attr, sizeof(attr), "<%s", tag);

    const char *s = xml;
    /* 找开始标签: 尝试 <XAddrs, <d:XAddrs, <w:XAddrs */
    while (*s) {
        const char *start = NULL;
        for (int p = 0; (size_t)p < strlen(attr); p++) {
            if (strncmp(s, attr + p, strlen(attr) - p) == 0) {
                start = s + (strlen(attr) - p);
                break;
            }
        }
        if (!start) { s++; continue; }

        /* 跳过 > */
        const char *body = strchr(start, '>');
        if (!body) { s++; continue; }
        body++;

        /* 找结束标签 */
        const char *end = strstr(body, close);
        if (!end) {
            /* 也尝试命名空间变体: </d:XAddrs>, </w:XAddrs> 等 */
            for (const char *ns = xml; ns < body; ns++) {
                const char *colon = strchr(ns, ':');
                if (!colon) break;
                char nstag[256];
                snprintf(nstag, sizeof(nstag), "</%.*s>", (int)(colon - ns + 1), ns);
                end = strstr(body, nstag);
                if (end) break;
            }
        }
        if (!end) { s++; continue; }

        size_t len = end - body;
        if (len >= dsz) len = dsz - 1;
        memcpy(dst, body, len);
        dst[len] = '\0';

        /* 去首尾空格 */
        while (len > 0 && dst[len-1] == ' ') dst[--len] = '\0';
        while (*dst == ' ' || *dst == '\n' || *dst == '\r') { memmove(dst, dst+1, len); len--; }
        return 1;
    }
    return 0;
}

/* === 从 XAddrs 提取 IP === */
static void extract_ip(const char *url, char *ip, size_t sz) {
    const char *p = strstr(url, "://");
    if (!p) { snprintf(ip, sz, "%s", url); return; }
    p += 3;
    const char *end = strchr(p, '/');
    if (!end) end = strchr(p, ':');
    if (!end) { snprintf(ip, sz, "%s", p); return; }
    size_t len = end - p;
    if (len >= sz) len = sz - 1;
    memcpy(ip, p, len);
    ip[len] = '\0';
}

/* === 根据网卡名或自动选择网卡, 创建并绑定 UDP socket === */
static int create_udp_socket(const char *iface) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    /* 允许端口复用 (多个进程可同时监听) */
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* 绑定 0.0.0.0:0 (任意端口) */
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    /* 加入组播组 */
    struct ip_mreq mreq = {0};
    mreq.imr_multiaddr.s_addr = inet_addr(ONVIF_MULTICAST_ADDR);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (iface && iface[0]) {
        /* 指定网卡的话, 尝试获取其 IP */
        mreq.imr_interface.s_addr = inet_addr(iface);
    }
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        /* 部分驱动不支持 IP_ADD_MEMBERSHIP, 忽略错误继续 */
    }

    /* 设置组播 TTL */
    int ttl = 4;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    /* 关闭组播回环 */
    int loop = 0;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    return fd;
}

/* === 发送 Probe === */
static void send_probe(int fd) {
    char uuid[64];
    make_uuid(uuid, sizeof(uuid));
    char msg[2048];
    snprintf(msg, sizeof(msg), PROBE_XML,
             (unsigned)time(NULL), rand() & 0xffff,
             rand() & 0xffff, rand() & 0xffff,
             rand() & 0xffff, (unsigned)rand());

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = inet_addr(ONVIF_MULTICAST_ADDR);
    dst.sin_port = htons(ONVIF_DISCO_PORT);

    /* 多发几次确保到达 */
    for (int i = 0; i < 3; i++) {
        sendto(fd, msg, strlen(msg), 0, (struct sockaddr *)&dst, sizeof(dst));
        usleep(50000); /* 50ms 间隔 */
    }
}

/* === 去重: 检查 IP 是否已在列表中 === */
static int already_seen(const onvif_device_t *devs, int count, const char *ip) {
    for (int i = 0; i < count; i++)
        if (strcmp(devs[i].ip, ip) == 0) return 1;
    return 0;
}

/* === 公开接口 === */
int onvif_discover(const char *iface, int timeout_ms,
                   onvif_device_t *devices, int max) {
    if (timeout_ms <= 0) timeout_ms = ONVIF_PROBE_TIMEOUT_MS;
    if (max <= 0 || max > ONVIF_MAX_DEVICES) max = ONVIF_MAX_DEVICES;

    int fd = create_udp_socket(iface);
    if (fd < 0) return -1;

    send_probe(fd);

    int count = 0;
    char buf[8192];
    struct timeval tv;
    time_t start = time(NULL);

    while (count < max) {
        /* 动态计算剩余时间 */
        int elapsed = (int)(time(NULL) - start) * 1000;
        int remain = timeout_ms - elapsed;
        if (remain <= 0) break;

        tv.tv_sec = remain / 1000;
        tv.tv_usec = (remain % 1000) * 1000;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);

        int ret = select(fd + 1, &fds, NULL, NULL, &tv);
        if (ret < 0) break;
        if (ret == 0) continue; /* 超时 */

        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n <= 0) continue;
        buf[n] = '\0';

        /* 快速过滤: 必须包含 XAddrs */
        if (!strstr(buf, "XAddrs")) continue;

        onvif_device_t dev;
        memset(&dev, 0, sizeof(dev));

        if (!xml_extract(buf, "XAddrs", dev.xaddrs, sizeof(dev.xaddrs)))
            continue;

        extract_ip(dev.xaddrs, dev.ip, sizeof(dev.ip));
        if (already_seen(devices, count, dev.ip)) continue;

        /* 可选: 提取其他信息 */
        xml_extract(buf, "Scopes", dev.scopes, sizeof(dev.scopes));
        xml_extract(buf, "Types", dev.types, sizeof(dev.types));

        devices[count++] = dev;
    }

    close(fd);
    return count;
}
