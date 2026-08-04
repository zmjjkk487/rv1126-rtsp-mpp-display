/*
 * web_main.c — 嵌入式 Web 管理后台入口
 *
 * 编译: 加入 build.sh
 * 运行: ./rv1126_web
 * 端口: 8080
 *
 * 功能:
 *   GET  /              → 返回摄像头管理页面 (static/index.html)
 *   GET  /api/scan      → 触发 ONVIF 扫描, 返回 JSON 设备列表
 *   POST /api/connect   → 写入 config.ini + 重启 GStreamer 管线
 *   GET  /api/status    → 查询管线运行状态
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

#include "onvif_disco.h"
#include "onvif_soap.h"
#include <openssl/sha.h>
#include <openssl/rand.h>

#define PORT         8080
#define MAX_CLIENTS  8
#define BUF_SIZE     16384
#define STATIC_DIR   "/root/camera-web/static"
#define PASSWD_FILE  "/root/camera-web/passwd"
#define SESSION_FILE "/tmp/web_token"
#define DEFAULT_HASH "240be518fabd2724ddb6f04eeb1da5967448d7e831c08c8fa822809f74c720a9"
static char g_passwd_hash[65] = "";

/* 前向声明 — handle_login 在其定义前被 handle_request 调用 */
static void http_ok(int fd, const char *ct, const char *body);
static void http_err(int fd, int code, const char *msg);

/* === 工具: SHA256 哈希 → 64 字符 hex === */
static void sha256_hex(const char *input, char out[65]) {
    unsigned char hash[32];
    SHA256((const unsigned char *)input, strlen(input), hash);
    for (int i = 0; i < 32; i++)
        sprintf(out + i * 2, "%02x", hash[i]);
    out[64] = '\0';
}

/* === 工具: 生成 32 字符随机 session token === */
static void gen_token(char out[33]) {
    unsigned char buf[16];
    RAND_bytes(buf, sizeof(buf));
    for (int i = 0; i < 16; i++)
        sprintf(out + i * 2, "%02x", buf[i]);
    out[32] = '\0';
}

/* === 工具: 检查 HTTP Authorization Bearer token === */
static int check_auth(const char *req) {
    const char *auth = strstr(req, "Authorization: Bearer ");
    if (!auth) return 0;
    auth += 22;
    char token[64];
    int i;
    for (i = 0; i < 63 && auth[i] && auth[i] != '\r' && auth[i] != '\n'; i++)
        token[i] = auth[i];
    token[i] = '\0';
    if (!token[0]) return 0;
    FILE *fp = fopen(SESSION_FILE, "r");
    if (!fp) return 0;
    char stored[64] = "";
    fgets(stored, sizeof(stored), fp);
    fclose(fp);
    size_t sl = strlen(stored);
    while (sl > 0 && (stored[sl-1] == '\n' || stored[sl-1] == '\r')) stored[--sl] = '\0';
    return strcmp(token, stored) == 0;
}

/* === HTTP 401 未授权响应 === */
static void http_401(int fd) {
    const char *body = "{\"error\":\"unauthorized\"}";
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 401 Unauthorized\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n", strlen(body));
    send(fd, hdr, strlen(hdr), MSG_NOSIGNAL);
    send(fd, body, strlen(body), MSG_NOSIGNAL);
}

/* === API: POST /api/login — 验证账号密码, 返回 session token === */
static void handle_login(int fd, const char *body) {
    char user[64] = "", pass[64] = "";
    const char *u = strstr(body, "\"user\":\"");
    const char *p = strstr(body, "\"pass\":\"");
    if (u) { u += 8; const char *ue = strchr(u, '"'); if (ue) { size_t l = ue - u; if (l < 64) { memcpy(user, u, l); user[l] = 0; } } }
    if (p) { p += 8; const char *pe = strchr(p, '"'); if (pe) { size_t l = pe - p; if (l < 64) { memcpy(pass, p, l); pass[l] = 0; } } }
    if (!user[0] || !pass[0]) { http_err(fd, 400, "{\"error\":\"missing user/pass\"}"); return; }

    char hash[65];
    sha256_hex(pass, hash);
    if (strcmp(hash, g_passwd_hash) != 0) {
        http_err(fd, 403, "{\"error\":\"wrong user or password\"}");
        return;
    }

    char token[33];
    gen_token(token);
    FILE *fp = fopen(SESSION_FILE, "w");
    if (!fp) { http_err(fd, 500, "{\"error\":\"session error\"}"); return; }
    fprintf(fp, "%s\n", token);
    fclose(fp);

    char resp[256];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"token\":\"%s\"}", token);
    http_ok(fd, "application/json", resp);
}

/* === API: POST /api/logout — 注销 session 并清理 HLS === */
static void handle_logout(int fd) {
    remove(SESSION_FILE);
    remove("/tmp/hls_token");
    system("killall -9 gst-launch-1.0 2>/dev/null");
    http_ok(fd, "application/json", "{\"status\":\"ok\"}");
}
static void url_decode(char *dst, const char *src, size_t sz) {
    char a, b;
    while (*src && --sz > 0) {
        if (*src == '%' && ((a = src[1]) && (b = src[2]))
            && ((a >= '0' && a <= '9') || (a >= 'A' && a <= 'F') || (a >= 'a' && a <= 'f'))
            && ((b >= '0' && b <= '9') || (b >= 'A' && b <= 'F') || (b >= 'a' && b <= 'f'))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= 'A' - 10; else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= 'A' - 10; else b -= '0';
            *dst++ = (char)(16 * a + b);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* === 工具: 简易 JSON 字符串转义 + HTML 实体编码 === */
static void json_escape(char *dst, const char *src, size_t sz) {
    size_t pos = 0;
    while (*src && pos < sz - 8) {  /* 留足最坏 case: &#39; = 5 chars */
        if (*src == '"')  { dst[pos++] = '\\'; dst[pos++] = '"'; }
        else if (*src == '\\') { dst[pos++] = '\\'; dst[pos++] = '\\'; }
        else if (*src == '&')  { memcpy(dst+pos, "&amp;", 5); pos += 5; }
        else if (*src == '<')  { memcpy(dst+pos, "&lt;", 4);  pos += 4; }
        else if (*src == '>')  { memcpy(dst+pos, "&gt;", 4);  pos += 4; }
        else if (*src == '\'') { memcpy(dst+pos, "&#39;", 5); pos += 5; }
        else dst[pos++] = *src;
        src++;
    }
    dst[pos] = '\0';
}

/* === 工具: shell 单引号转义 (用于 safe system() 调用) === */
static void shell_escape_sq(char *dst, const char *src, size_t sz) {
    size_t pos = 0;
    while (*src && pos < sz - 5) {
        if (*src == '\'') {
            /* 在单引号内部嵌入字面单引号: 关引号 → 转义引号 → 开引号 */
            memcpy(dst + pos, "'\\''", 4);
            pos += 4;
        } else {
            dst[pos++] = *src;
        }
        src++;
    }
    dst[pos] = '\0';
}

/* === 工具: 从 URL query string 中提取 token 参数 === */
static int check_token_param(const char *path) {
    const char *t = strstr(path, "?t=");
    if (!t) t = strstr(path, "&t=");
    if (!t) return 0;
    t += 3;
    char token[64];
    int i;
    for (i = 0; i < 63 && t[i] && t[i] != '&' && t[i] != ' '; i++)
        token[i] = t[i];
    token[i] = '\0';
    if (!token[0]) return 0;
    FILE *fp = fopen("/tmp/hls_token", "r");
    if (!fp) return 0;
    char stored[64] = "";
    fgets(stored, sizeof(stored), fp);
    fclose(fp);
    size_t sl = strlen(stored);
    while (sl > 0 && (stored[sl-1] == '\n' || stored[sl-1] == '\r')) stored[--sl] = '\0';
    return strcmp(token, stored) == 0;
}

/* === HTTP 响应助手 === */
static void http_ok(int fd, const char *content_type, const char *body) {
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "\r\n",
             content_type, strlen(body));
    send(fd, hdr, strlen(hdr), MSG_NOSIGNAL);
    send(fd, body, strlen(body), MSG_NOSIGNAL);
}

static void http_err(int fd, int code, const char *msg) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "HTTP/1.1 %d\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
             code, strlen(msg), msg);
    send(fd, buf, strlen(buf), MSG_NOSIGNAL);
}

/* === 读文件到内存 === */
static char *read_file(const char *path, size_t *len_out) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    fread(buf, 1, sz, fp);
    fclose(fp);
    buf[sz] = '\0';
    if (len_out) *len_out = (size_t)sz;
    return buf;
}

/* === API: /api/scan — ONVIF 扫描 + 获取 Profiles === */
static void handle_scan(int fd) {
    onvif_device_t devs[32];
    /* 1a. WS-Discovery 组播 (3 秒足够) */
    int n = onvif_discover(NULL, 3000, devs, 32);

    /* 1b. 组播无果 → 快速 IP 探测 (海康等品牌不响应组播) */
    if (n <= 0) {
        /* 常见摄像头 IP: .54(大华默认), .64(海康默认), .168(你的), .10 */
        int common_ips[] = {54, 64, 168, 10, 100, 101, 200, 150, 1, 66};
        for (int k = 0; k < (int)(sizeof(common_ips)/sizeof(common_ips[0])); k++) {
            char test[256];
            snprintf(test, sizeof(test), "http://192.168.50.%d/onvif/device_service", common_ips[k]);
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "curl -s -m 0.5 '%s' 2>/dev/null", test);
            FILE *pp = popen(cmd, "r");
            char first[256] = {0};
            if (pp) { fgets(first, sizeof(first), pp); pclose(pp); }
            if (strstr(first, "<?xml") || strstr(first, "SOAP") || strstr(first, "html")) {
                snprintf(devs[n].xaddrs, sizeof(devs[n].xaddrs), "%s", test);
                snprintf(devs[n].ip, sizeof(devs[n].ip), "192.168.50.%d", common_ips[k]);
                snprintf(devs[n].scopes, sizeof(devs[n].scopes), "192.168.50.%d", common_ips[k]);
                n++;
            }
        }
    }
    if (n < 0) {
        http_err(fd, 500, "{\"error\":\"discovery failed\"}");
        return;
    }

    /* 2. 对每个设备获取 Profiles */
    char *json = malloc(65536);
    if (!json) { http_err(fd, 500, "{}"); return; }
    size_t pos = 0;
    pos += snprintf(json + pos, 65536 - pos, "{\"devices\":[");

    int total = 0;
    for (int i = 0; i < n; i++) {
        onvif_profile_t profs[8];
        int pn = onvif_get_profiles(devs[i].xaddrs, "admin", "", profs, 8);

        /* 尝试默认凭据 */
        if (pn <= 0)
            pn = onvif_get_profiles(devs[i].xaddrs, "admin", "123456", profs, 8);
        if (pn <= 0)
            pn = onvif_get_profiles(devs[i].xaddrs, "admin", "admin", profs, 8);

        /* 即使没有 profiles, 也列出设备 (可手动输入凭据) */
        char esc_ip[128], esc_name[256];
        json_escape(esc_ip, devs[i].ip, sizeof(esc_ip));
        json_escape(esc_name, devs[i].scopes, sizeof(esc_name));

        if (total > 0) pos += snprintf(json + pos, 65536 - pos, ",");
        pos += snprintf(json + pos, 65536 - pos,
            "{\"ip\":\"%s\",\"xaddrs\":\"%s\",\"name\":\"%s\",\"profiles\":[",
            esc_ip, devs[i].xaddrs, esc_name);

        for (int j = 0; j < pn; j++) {
            if (j > 0) pos += snprintf(json + pos, 65536 - pos, ",");

            /* 获取 RTSP URI, 清洗可能的 XML 标签残留 */
            char rtsp_uri[512] = "";
            onvif_get_stream_uri(devs[i].xaddrs, profs[j].token,
                                 "admin", "123456", rtsp_uri, sizeof(rtsp_uri));
            /* 去 XML 标签前缀 (如 <Uri>rtsp://...) */
            char *clean = rtsp_uri;
            if (clean[0] == '<') { clean = strchr(clean, '>'); if (clean) clean++; else clean = rtsp_uri; }

            char esc_tok[128], esc_uri[512], esc_name[256];
            json_escape(esc_tok, profs[j].token, sizeof(esc_tok));
            json_escape(esc_uri, clean[0] ? clean : "", sizeof(esc_uri));
            json_escape(esc_name, profs[j].name, sizeof(esc_name));
            pos += snprintf(json + pos, 65536 - pos,
                "{\"token\":\"%s\",\"name\":\"%s\",\"width\":%d,\"height\":%d,\"uri\":\"%s\"}",
                esc_tok, esc_name, profs[j].width, profs[j].height, esc_uri);
        }
        pos += snprintf(json + pos, 65536 - pos, "]}");
        total++;
    }
    pos += snprintf(json + pos, 65536 - pos, "],\"count\":%d}", total);
    http_ok(fd, "application/json", json);
    free(json);
}

/* === API: POST /api/connect — 写入 config.ini + 重启管线 === */
static void handle_connect(int fd, const char *body) {
    /* 从 POST body 解析 url */
    const char *url = strstr(body, "\"url\":\"");
    if (!url) { http_err(fd, 400, "{\"error\":\"missing url\"}"); return; }
    url += 7;
    const char *url_end = strchr(url, '"');
    if (!url_end) { http_err(fd, 400, "{\"error\":\"invalid json\"}"); return; }

    char rtsp[512];
    size_t len = url_end - url;
    if (len >= sizeof(rtsp)) len = sizeof(rtsp) - 1;
    memcpy(rtsp, url, len);
    rtsp[len] = '\0';

    /* 提取 user/pass (可选) */
    char user[64] = "admin", pass[64] = "";
    const char *u = strstr(body, "\"user\":\"");
    const char *p = strstr(body, "\"pass\":\"");
    if (u) { u += 8; const char *ue = strchr(u, '"'); if (ue) { size_t ul = ue - u; if (ul < 64) { memcpy(user, u, ul); user[ul] = 0; } } }
    if (p) { p += 8; const char *pe = strchr(p, '"'); if (pe) { size_t pl = pe - p; if (pl < 64) { memcpy(pass, p, pl); pass[pl] = 0; } } }

    /* 把 user/pass 注入 RTSP URL (rtsp://user:pass@host/path)
     * 否则摄像头返回 401 Unauthorized, 管线起不来 */
    char final_url[512];
    if (user[0] && strstr(rtsp, "://") && !strstr(rtsp, "@")) {
        const char *proto_end = strstr(rtsp, "://") + 3;
        snprintf(final_url, sizeof(final_url), "%.*s%s:%s@%s",
                 (int)(proto_end - rtsp), rtsp, user, pass, proto_end);
    } else {
        snprintf(final_url, sizeof(final_url), "%s", rtsp);
    }

    /* 写入 config.ini */
    FILE *fp = fopen("/root/config.ini", "w");
    if (!fp) { http_err(fd, 500, "{\"error\":\"cannot write config\"}"); return; }
    fprintf(fp, "[network]\n");
    fprintf(fp, "# 摄像头 RTSP 地址 (由 Web 管理后台自动配置)\n");
    fprintf(fp, "rtsp_url = %s\n", final_url);
    fprintf(fp, "rtsp_transport = tcp\n\n");
    fprintf(fp, "[log]\n");
    fprintf(fp, "log_level = info\n");
    fclose(fp);

    /* 重启管线 */
    /* 注意: 进程名被内核截断为 15 字符 rtsp_display,
     * 用全名 rtsp_display 会匹配不到 (Linux comm 限制) */
    system("killall -9 rtsp_display 2>/dev/null");
    usleep(500000);
    system("killall weston 2>/dev/null");
    usleep(200000);
    system("cd /root && nohup /root/rtsp_display /root/config.ini "
           "</dev/null >/tmp/gst_web.log 2>&1 &");

    /* 记忆上次连接: 开机自启脚本读它自动连回 */
    FILE *lf = fopen("/root/last_connect.json", "w");
    if (lf) {
        fprintf(lf, "{\"url\":\"%s\"}\n", final_url);
        fclose(lf);
    }

    /* 确认管线启动 */
    usleep(1000000);
    FILE *pp = popen("pgrep -f rtsp_display", "r");
    char pidbuf[32] = {0};
    if (pp) { fgets(pidbuf, sizeof(pidbuf), pp); pclose(pp); }
    /* 去掉尾部换行: \n 是 JSON 非法控制字符, 会导致前端解析失败 */
    size_t pl = strlen(pidbuf);
    while (pl > 0 && (pidbuf[pl-1] == '\n' || pidbuf[pl-1] == '\r')) pidbuf[--pl] = '\0';

    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"pid\":\"%s\",\"rtsp\":\"%s\"}",
             pidbuf[0] ? pidbuf : "0", final_url);
    http_ok(fd, "application/json", resp);
}

/* === 路由 === */
static void handle_request(int fd) {
    char buf[BUF_SIZE];
    ssize_t n = recv(fd, buf, BUF_SIZE - 1, 0);
    if (n <= 0) { close(fd); return; }
    buf[n] = '\0';

    /* 解析第一行: METHOD /path HTTP/1.1 */
    char method[16] = {0}, path[256] = {0};
    sscanf(buf, "%15s %255s", method, path);

    /* URL decode */
    char decoded[256];
    url_decode(decoded, path, sizeof(decoded));

    /* GET / → index.html */
    if (strcmp(method, "GET") == 0 &&
        (strcmp(decoded, "/") == 0 || strcmp(decoded, "/index.html") == 0)) {
        size_t len = 0;
        char *html = read_file(STATIC_DIR "/index.html", &len);
        if (html) {
            char hdr[256];
            snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                "Content-Length: %zu\r\nConnection: close\r\n\r\n", len);
            send(fd, hdr, strlen(hdr), MSG_NOSIGNAL);
            send(fd, html, len, MSG_NOSIGNAL);
            free(html);
        } else {
            http_err(fd, 404, "<h1>index.html not found</h1>");
        }
        close(fd);
        return;
    }

    /* GET /<file> — 通用静态文件 (index.html, hls.min.js 等) */
    if (strcmp(method, "GET") == 0 && decoded[1] != '\0' &&
        !strchr(decoded + 1, '/') && strchr(decoded + 1, '.')) {
        char file[512];
        snprintf(file, sizeof(file), STATIC_DIR "/%s", decoded + 1);
        if (strstr(decoded, "..")) {
            http_err(fd, 403, "Forbidden");
            close(fd);
            return;
        }
        size_t flen = 0;
        char *data = read_file(file, &flen);
        if (data) {
            const char *ct = "application/octet-stream";
            if (strstr(file, ".html")) ct = "text/html; charset=utf-8";
            else if (strstr(file, ".js")) ct = "application/javascript";
            else if (strstr(file, ".css")) ct = "text/css";
            else if (strstr(file, ".png")) ct = "image/png";
            else if (strstr(file, ".jpg") || strstr(file, ".jpeg")) ct = "image/jpeg";
            else if (strstr(file, ".ico")) ct = "image/x-icon";
            char hdr[256];
            snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                "Content-Length: %zu\r\nConnection: close\r\n\r\n", ct, flen);
            send(fd, hdr, strlen(hdr), MSG_NOSIGNAL);
            send(fd, data, flen, MSG_NOSIGNAL);
            free(data);
            close(fd);
            return;
        }
    }

    /* GET /api/scan */
    if (strcmp(method, "GET") == 0 && strcmp(decoded, "/api/scan") == 0) {
        if (!check_auth(buf)) { http_401(fd); close(fd); return; }
        handle_scan(fd);
        close(fd);
        return;
    }

    /* GET /api/status */
    if (strcmp(method, "GET") == 0 && strcmp(decoded, "/api/status") == 0) {
        if (!check_auth(buf)) { http_401(fd); close(fd); return; }
        FILE *pp = popen("pgrep -f rtsp_display", "r");
        char pid[32] = {0};
        if (pp) { fgets(pid, sizeof(pid), pp); pclose(pp); }
        /* 去尾部换行 (JSON 非法控制字符) */
        size_t plen = strlen(pid);
        while (plen > 0 && (pid[plen-1] == '\n' || pid[plen-1] == '\r')) pid[--plen] = '\0';
        char resp[128];
        snprintf(resp, sizeof(resp),
            "{\"running\":%s,\"pid\":\"%s\"}",
            pid[0] ? "true" : "false", pid[0] ? pid : "");
        http_ok(fd, "application/json", resp);
        close(fd);
        return;
    }

    /* POST /api/connect */
    if (strcmp(method, "POST") == 0 && strcmp(decoded, "/api/connect") == 0) {
        if (!check_auth(buf)) { http_401(fd); close(fd); return; }
        const char *body = strstr(buf, "\r\n\r\n");
        if (body) body += 4;
        else body = "";
        handle_connect(fd, body);
        close(fd);
        return;
    }

    /* GET /hls/stream.m3u8 — 动态生成 live playlist (需要 HLS token) */
    if (strcmp(method, "GET") == 0 && strcmp(decoded, "/hls/stream.m3u8") == 0) {
        if (!check_token_param(path)) { http_401(fd); close(fd); return; }
        /* 提取 token 用于注入到分片 URL */
        const char *tok = strstr(path, "?t=") ? strstr(path, "?t=") + 3 : "";
        char tok_val[64] = "";
        for (int ti = 0; ti < 63 && tok[ti] && tok[ti] != '&'; ti++)
            tok_val[ti] = tok[ti];
        tok_val[63] = '\0';

        char m3u8[8192];
        int pos = snprintf(m3u8, sizeof(m3u8),
            "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:3\n");
        /* 扫描 /root/hls/seg_*.ts, 取序号最大的 5 个 */
        int seqs[64], n = 0;
        DIR *dir = opendir("/root/hls");
        if (dir) {
            struct dirent *de;
            while ((de = readdir(dir)) && n < 64) {
                if (strncmp(de->d_name, "seg_", 4) != 0) continue;
                int s = atoi(de->d_name + 4);
                seqs[n++] = s;
            }
            closedir(dir);
        }
        /* 排序 */
        for (int i = 0; i < n - 1; i++)
            for (int j = i + 1; j < n; j++)
                if (seqs[j] < seqs[i]) { int t = seqs[i]; seqs[i] = seqs[j]; seqs[j] = t; }
        /* 跳过最新一个分片 (可能正在被 gst 写入, 引用会导致 fragParsingError),
         * 取它之前的 5 个 */
        int usable = n > 1 ? n - 1 : 0;
        int start = usable > 5 ? usable - 5 : 0;
        if (usable > 0) {
            pos += snprintf(m3u8 + pos, sizeof(m3u8) - pos,
                "#EXT-X-MEDIA-SEQUENCE:%d\n", seqs[start]);
            for (int i = start; i < usable; i++) {
                pos += snprintf(m3u8 + pos, sizeof(m3u8) - pos,
                    "\n#EXTINF:2,\nseg_%05d.ts?t=%s\n", seqs[i], tok_val);
            }
        } else {
            pos += snprintf(m3u8 + pos, sizeof(m3u8) - pos,
                "#EXT-X-MEDIA-SEQUENCE:0\n");
        }
        http_ok(fd, "application/vnd.apple.mpegurl", m3u8);
        close(fd);
        return;
    }

    /* GET /hls/*.ts — HLS 分片文件 (需要 HLS token) */
    if (strcmp(method, "GET") == 0 && strncmp(decoded, "/hls/", 5) == 0) {
        if (!check_token_param(path)) { http_401(fd); close(fd); return; }
        /* 去掉 ?t=... 参数获取真实文件名 */
        char clean[256];
        strncpy(clean, decoded + 5, sizeof(clean) - 1);
        clean[sizeof(clean) - 1] = '\0';
        char *q = strchr(clean, '?');
        if (q) *q = '\0';
        char file[512];
        snprintf(file, sizeof(file), "/root/hls/%s", clean);
        /* 防目录穿越 */
        if (strstr(decoded, "..")) {
            http_err(fd, 403, "Forbidden");
            close(fd);
            return;
        }
        size_t flen = 0;
        char *data = read_file(file, &flen);
        if (!data) {
            http_err(fd, 404, "Not Found");
            close(fd);
            return;
        }
        const char *ct = "application/octet-stream";
        if (strstr(file, ".m3u8")) ct = "application/vnd.apple.mpegurl";
        else if (strstr(file, ".ts")) ct = "video/MP2T";
        char hdr[256];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
            "Content-Length: %zu\r\nConnection: close\r\n\r\n", ct, flen);
        send(fd, hdr, strlen(hdr), MSG_NOSIGNAL);
        send(fd, data, flen, MSG_NOSIGNAL);
        free(data);
        close(fd);
        return;
    }

    /* POST /api/hls_start — 启动 GStreamer RTSP→HLS 转码
     * body: {"url":"rtsp://..."} */
    if (strcmp(method, "POST") == 0 && strcmp(decoded, "/api/hls_start") == 0) {
        if (!check_auth(buf)) { http_401(fd); close(fd); return; }
        const char *body = strstr(buf, "\r\n\r\n");
        if (body) body += 4;
        else body = "";
        char url[512] = "";
        char *p = strstr(body, "\"url\":\"");
        if (p) { p += 7; char *e = strchr(p, '"'); if (e) { size_t l = e - p; if (l < 512) { memcpy(url, p, l); url[l] = 0; } } }
        if (!url[0]) {
            http_err(fd, 400, "{\"error\":\"missing url\"}");
            close(fd);
            return;
        }

        /* Shell 单引号转义: 防命令注入 */
        char safe_url[1024];
        shell_escape_sq(safe_url, url, sizeof(safe_url));

        /* 生成 HLS 临时 token (用于视频流认证) */
        char hls_tok[33];
        gen_token(hls_tok);
        FILE *tf = fopen("/tmp/hls_token", "w");
        if (tf) { fprintf(tf, "%s\n", hls_tok); fclose(tf); }

        /* 先停旧转码 */
        system("killall -9 gst-launch-1.0 2>/dev/null");
        usleep(300000);
        system("mkdir -p /root/hls && rm -f /root/hls/*.ts /root/hls/*.m3u8");

        /* GStreamer: RTSP → HLS (hlssink2, 直通 H.264 不重编码) */
        char cmd[1200];
        snprintf(cmd, sizeof(cmd),
            "gst-launch-1.0 rtspsrc location='%s' latency=300 "
            "! rtph264depay ! h264parse "
            "! hlssink2 location='/root/hls/seg_%%05d.ts' "
            "playlist-location='/root/hls/stream.m3u8' "
            "target-duration=2 max-files=30 playlist-length=0 "
            "</dev/null >/tmp/hls.log 2>&1 &", safe_url);
        system(cmd);

        char resp[256];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"ok\",\"hls_token\":\"%s\"}", hls_tok);
        http_ok(fd, "application/json", resp);
        close(fd);
        return;
    }

    /* POST /api/hls_stop — 停止 HLS 转码 */
    if (strcmp(method, "POST") == 0 && strcmp(decoded, "/api/hls_stop") == 0) {
        if (!check_auth(buf)) { http_401(fd); close(fd); return; }
        system("killall -9 gst-launch-1.0 2>/dev/null");
        remove("/tmp/hls_token");
        http_ok(fd, "application/json", "{\"status\":\"ok\"}");
        close(fd);
        return;
    }

    /* POST /api/login — 登录 (不需要认证) */
    if (strcmp(method, "POST") == 0 && strcmp(decoded, "/api/login") == 0) {
        const char *body = strstr(buf, "\r\n\r\n");
        if (body) body += 4; else body = "";
        handle_login(fd, body);
        close(fd);
        return;
    }

    /* POST /api/logout — 注销 (需要认证) */
    if (strcmp(method, "POST") == 0 && strcmp(decoded, "/api/logout") == 0) {
        if (!check_auth(buf)) { http_401(fd); close(fd); return; }
        handle_logout(fd);
        close(fd);
        return;
    }

    /* 404 */
    http_err(fd, 404, "{\"error\":\"not found\"}");
    close(fd);
}

/* === 主循环 === */
int main(void) {
    signal(SIGPIPE, SIG_IGN);
    /* fork 的子进程退出后自动回收, 防止僵尸堆积 */
    signal(SIGCHLD, SIG_IGN);

    /* 初始化密码文件 (默认账号 admin, 默认密码 admin123) */
    FILE *pf = fopen(PASSWD_FILE, "r");
    if (!pf) {
        pf = fopen(PASSWD_FILE, "w");
        if (pf) {
            fprintf(pf, "admin:%s\n", DEFAULT_HASH);
            fclose(pf);
        }
        strncpy(g_passwd_hash, DEFAULT_HASH, 64);
        g_passwd_hash[64] = '\0';
    } else {
        char line[256];
        if (fgets(line, sizeof(line), pf)) {
            char *colon = strchr(line, ':');
            if (colon) {
                char *h = colon + 1;
                size_t hl = strlen(h);
                while (hl > 0 && (h[hl-1] == '\n' || h[hl-1] == '\r')) h[--hl] = '\0';
                strncpy(g_passwd_hash, h, 64);
                g_passwd_hash[64] = '\0';
            }
        }
        fclose(pf);
    }
    if (!g_passwd_hash[0]) {
        memcpy(g_passwd_hash, DEFAULT_HASH, 64);
        g_passwd_hash[64] = '\0';
    }

    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* FD_CLOEXEC: fork+exec 启动 GStreamer 时不能继承监听 fd,
     * 否则 GStreamer 进程占着 8080 端口, web server 重启 bind 失败 */
    int flags = fcntl(server, F_GETFD);
    fcntl(server, F_SETFD, flags | FD_CLOEXEC);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(server); return 1;
    }
    if (listen(server, 5) < 0) {
        perror("listen"); close(server); return 1;
    }

    printf("Web 管理后台已启动: http://0.0.0.0:%d\n", PORT);

    /* 自动恢复上次连接: 若管线未运行且有 last_connect.json */
    FILE *lf = fopen("/root/last_connect.json", "r");
    if (lf) {
        char lbuf[1024] = {0};
        fread(lbuf, 1, sizeof(lbuf) - 1, lf);
        fclose(lf);
        char *url = strstr(lbuf, "\"url\":\"");
        if (url) {
            url += 7;
            char *end = strchr(url, '"');
            if (end) *end = '\0';
            /* 写入 config.ini 并启动管线 */
            FILE *cf = fopen("/root/config.ini", "w");
            if (cf) {
                fprintf(cf, "[network]\nrtsp_url = %s\nrtsp_transport = tcp\n\n[log]\nlog_level = info\n", url);
                fclose(cf);
            }
            printf("自动恢复上次连接: %s\n", url);
            system("killall weston 2>/dev/null");
            /* 先杀旧管线, 防止双进程竞争 /dev/fb0 */
            system("for p in $(pidof rtsp_display); do kill -9 $p 2>/dev/null; done");
            system("cd /root && nohup /root/rtsp_display /root/config.ini "
                   "</dev/null >/tmp/gst_web.log 2>&1 &");
        }
    }

    while (1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server, &fds);
        if (select(server + 1, &fds, NULL, NULL, NULL) > 0) {
            int client = accept(server, NULL, NULL);
            if (client < 0) continue;

            /* fork 子进程处理请求: connect 里有 2 秒同步操作,
             * 单线程顺序处理会让其他请求(如 status)排队卡死 */
            pid_t pid = fork();
            if (pid == 0) {
                close(server);           /* 子进程关掉监听 fd */
                handle_request(client);  /* 处理完即退出 */
                _exit(0);
            }
            close(client);               /* 父进程关掉连接 fd */
        }
    }

    close(server);
    return 0;
}
