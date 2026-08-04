/*
 * onvif_soap.c — ONVIF SOAP/HTTP 客户端实现
 *
 * 依赖: libcurl (HTTP), libcrypto (SHA1/Base64)
 * 基于 ONVIF Media Service WSDL
 */

#include "onvif_soap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>

/* OpenSSL 头: 用于 WS-Security PasswordDigest */
#include <openssl/evp.h>

/* === 全局: libcurl 初始化 (进程启动时调一次) === */
static int g_curl_inited = 0;
static void curl_init_once(void) {
    if (!g_curl_inited) {
        curl_global_init(CURL_GLOBAL_ALL);
        g_curl_inited = 1;
    }
}

/* === CURL write callback: 收集响应体 === */
struct curl_buffer {
    char *data;
    size_t len;
    size_t cap;
};

static size_t curl_write_cb(void *ptr, size_t sz, size_t nmemb, void *user) {
    struct curl_buffer *cb = (struct curl_buffer *)user;
    size_t total = sz * nmemb;
    if (cb->len + total + 1 > cb->cap) {
        size_t newcap = (cb->len + total) * 2 + 4096;
        char *nd = realloc(cb->data, newcap);
        if (!nd) return 0;
        cb->data = nd;
        cb->cap = newcap;
    }
    memcpy(cb->data + cb->len, ptr, total);
    cb->len += total;
    cb->data[cb->len] = '\0';
    return total;
}

/* === Base64 编码 (OpenSSL EVP) === */
static char *base64_encode(const unsigned char *data, size_t len) {
    int outlen = ((len + 2) / 3) * 4 + 1;
    char *out = malloc(outlen);
    if (!out) return NULL;
    EVP_EncodeBlock((unsigned char *)out, data, len);
    return out;
}

/* === 生成 WS-Security PasswordDigest 头 ===
 * 算法: PasswordDigest = Base64(SHA1(Nonce + Created + Password))
 * 参考 OASIS WS-Security 1.0 §4.1
 */
static char *wsse_header(const char *user, const char *pass) {
    if (!user || !pass || !*user) return NULL;

    /* 1. 生成 16 字节随机 Nonce */
    unsigned char nonce[16];
    FILE *fp = fopen("/dev/urandom", "r");
    if (fp) { fread(nonce, 1, 16, fp); fclose(fp); }
    else { for (int i = 0; i < 16; i++) nonce[i] = (unsigned char)(rand() ^ time(NULL)); }

    /* 2. 生成 ISO 8601 Created 时间戳 */
    time_t now = time(NULL);
    struct tm gmt;
    gmtime_r(&now, &gmt);
    char created[64];
    strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", &gmt);

    /* 3. SHA1(Nonce(16B raw) + Created(UTF-8) + Password(UTF-8)) */
    unsigned char sha1[20];
    unsigned int sha1_len = 0;
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha1(), NULL);
    EVP_DigestUpdate(mdctx, nonce, 16);
    EVP_DigestUpdate(mdctx, created, strlen(created));
    EVP_DigestUpdate(mdctx, pass, strlen(pass));
    EVP_DigestFinal_ex(mdctx, sha1, &sha1_len);
    EVP_MD_CTX_free(mdctx);

    /* 4. Base64 编码 nonce 和 digest */
    char *nonce_b64 = base64_encode(nonce, 16);
    char *digest_b64 = base64_encode(sha1, sha1_len);
    if (!nonce_b64 || !digest_b64) {
        free(nonce_b64); free(digest_b64);
        return NULL;
    }

    /* 5. 构造完整的 SOAP Header */
    char *header = malloc(2048);
    if (!header) { free(nonce_b64); free(digest_b64); return NULL; }

    snprintf(header, 2048,
        "<s:Header>"
        "<wsse:Security"
        " xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-wssecurity-secext-1.0.xsd\""
        " xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-wssecurity-utility-1.0.xsd\""
        " s:mustUnderstand=\"true\">"
        "<wsse:UsernameToken wsu:Id=\"UsernameToken-1\">"
        "<wsse:Username>%s</wsse:Username>"
        "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%s</wsse:Password>"
        "<wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">%s</wsse:Nonce>"
        "<wsu:Created>%s</wsu:Created>"
        "</wsse:UsernameToken>"
        "</wsse:Security>"
        "</s:Header>",
        user, digest_b64, nonce_b64, created);

    free(nonce_b64);
    free(digest_b64);
    return header;
}

/* === 核心: 发送 SOAP 请求并获取响应 === */
static int soap_post(const char *url, const char *soap_action,
                     const char *body, const char *auth_header,
                     char **response) {
    curl_init_once();
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_buffer cb = {0};
    cb.data = malloc(4096);
    cb.cap = 4096;
    cb.len = 0;
    cb.data[0] = '\0';

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/soap+xml; charset=utf-8");
    char action_hdr[512];
    snprintf(action_hdr, sizeof(action_hdr), "SOAPAction: %s", soap_action);
    headers = curl_slist_append(headers, action_hdr);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    /* HTTP Basic Auth 后备 */
    if (auth_header) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        /* 提取 user:pass (假设格式 "user:pass") */
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth_header);
    }

    CURLcode ret = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (ret != CURLE_OK || (http_code != 200 && http_code != 400 && http_code != 500)) {
        free(cb.data);
        return -1;
    }

    *response = cb.data;
    return 0;
}

/* === XML 标签提取 (支持 <tag>, <ns:tag>, <NS:tag>) === */
static int xml_tag_parse(const char *xml, const char *tag, char *dst, size_t sz) {
    size_t tlen = strlen(tag);

    /* 搜索所有可能的开始标签格式, 找到第一个匹配的 body 内容 */
    const char *pos = xml;
    while (*pos) {
        /* 找 '<' 后跟 tag 名或 ns:tag 名的位置 */
        const char *lt = strchr(pos, '<');
        if (!lt) break;
        lt++; /* 跳过 '<' */
        pos = lt;

        /* 跳过标签名开头的非字母字符 (如 '/') */
        if (*lt == '/') continue;

        /* 这个标签名末尾必须匹配我们的 tag */
        const char *end_of_name = lt;
        while (*end_of_name && *end_of_name != '>' && *end_of_name != ' ' && *end_of_name != '\t')
            end_of_name++;

        /* 检查是否以 tag 结尾 (支持 <Name> 或 <tt:Name>) */
        int found = 0;
        if (end_of_name - lt >= (int)tlen &&
            strncmp(end_of_name - tlen, tag, tlen) == 0)
            found = 1;

        if (!found) continue;

        /* 跳过属性/命名空间声明, 找到 '>' */
        const char *body = strchr(lt, '>');
        if (!body) continue;
        body++;

        /* 找结束标签: </ 后跟 [ns:]tag > */
        const char *end = NULL;
        const char *c = body;
        while ((c = strstr(c, "</")) != NULL) {
            c += 2;
            /* 找到这个结束标签的 '>' */
            const char *gt = strchr(c, '>');
            if (!gt) break;
            /* 检查结束标签名是否匹配 (支持 </tag> 和 </ns:tag>) */
            const char *tag_start = c;
            const char *colon = NULL;  /* find last colon before > */
            for (const char *p = c; p < gt; p++) if (*p == ':') colon = p;
            if (colon && colon + 1 + tlen == gt &&
                strncmp(colon + 1, tag, tlen) == 0) {
                end = c - 2; /* point back to '<' */
                break;
            }
            if (gt - c == (int)tlen && strncmp(c, tag, tlen) == 0) {
                end = c - 2;
                break;
            }
            c = gt;
        }

        if (!end) continue;

        size_t len = end - body;
        if (len >= sz) len = sz - 1;
        memcpy(dst, body, len);
        dst[len] = '\0';
        return 1;
    }
    return 0;
}

/* === 公开接口: 获取 Profiles === */
int onvif_get_profiles(const char *xaddrs, const char *user, const char *pass,
                       onvif_profile_t *profiles, int max) {
    if (!xaddrs || max <= 0) return -1;

    /* SOAP Body */
    const char *body_tpl =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope"
        " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:m=\"http://www.onvif.org/ver10/media/wsdl\">"
        "%s"
        "<s:Body>"
        "<m:GetProfiles/>"
        "</s:Body>"
        "</s:Envelope>";

    char auth_buf[256] = {0};
    snprintf(auth_buf, sizeof(auth_buf), "%s:%s", user ? user : "", pass ? pass : "");

    char *wsse = (user && *user) ? wsse_header(user, pass) : NULL;
    char body[4096];
    snprintf(body, sizeof(body), body_tpl, wsse ? wsse : "<s:Header/>");
    free(wsse);

    char *resp = NULL;
    if (soap_post(xaddrs,
                  "http://www.onvif.org/ver10/media/wsdl/GetProfiles",
                  body,
                  (user && *user) ? auth_buf : NULL,
                  &resp) < 0) {
        /* 重试: 部分摄像头 media 在 /onvif/Media 而非 /onvif/device_service */
        char alt_url[512];
        const char *slash = strrchr(xaddrs, '/');
        if (slash) {
            size_t blen = slash - xaddrs;
            snprintf(alt_url, sizeof(alt_url), "%.*s/onvif/Media", (int)blen, xaddrs);
        } else {
            snprintf(alt_url, sizeof(alt_url), "%s", xaddrs);
        }
        if (strcmp(alt_url, xaddrs) != 0) {
            char *wsse2 = (user && *user) ? wsse_header(user, pass) : NULL;
            char body2[4096];
            snprintf(body2, sizeof(body2), body_tpl, wsse2 ? wsse2 : "<s:Header/>");
            free(wsse2);
            char *resp2 = NULL;
            if (soap_post(alt_url,
                          "http://www.onvif.org/ver10/media/wsdl/GetProfiles",
                          body2,
                          (user && *user) ? auth_buf : NULL,
                          &resp2) == 0) {
                free(resp);
                resp = resp2;
            } else {
                free(resp);
                return -1;
            }
        } else {
            return -1;
        }
    }

    /* 解析: 提取每个 Profile 的 token */
    int count = 0;
    const char *p = resp;
    while (count < max) {
        /* 找 <trt:Profiles token="..."> 或 <Profiles token="..."> */
        const char *tok_start = strstr(p, "Profiles");
        if (!tok_start) break;
        const char *eq = strstr(tok_start, "token=\"");
        if (!eq) { p = tok_start + 1; continue; }
        eq += 7;
        const char *eq_end = strchr(eq, '"');
        if (!eq_end) { p = eq; continue; }
        size_t tlen = eq_end - eq;
        if (tlen >= sizeof(profiles[count].token)) tlen = sizeof(profiles[count].token) - 1;
        memcpy(profiles[count].token, eq, tlen);
        profiles[count].token[tlen] = '\0';

        /* 可选: 提取 Name (支持 <Name> / <tt:Name> / <trt:Name>) */
        if (strstr(tok_start, "<Name") || strstr(tok_start, ":Name")) {
            xml_tag_parse(tok_start, "Name",
                          profiles[count].name, sizeof(profiles[count].name));
        }
        if (!profiles[count].name[0]) {
            snprintf(profiles[count].name, sizeof(profiles[count].name),
                     "Profile %d", count + 1);
        }

        /* 可选: 提取分辨率 */
        const char *w_tag = strstr(tok_start, ":Width>");
        const char *h_tag = strstr(tok_start, ":Height>");
        if (!w_tag) w_tag = strstr(tok_start, "<Width>");
        if (!h_tag) h_tag = strstr(tok_start, "<Height>");
        if (w_tag && h_tag) {
            char wbuf[16], hbuf[16];
            if (xml_tag_parse(tok_start, "Width", wbuf, sizeof(wbuf)))
                profiles[count].width = atoi(wbuf);
            if (xml_tag_parse(tok_start, "Height", hbuf, sizeof(hbuf)))
                profiles[count].height = atoi(hbuf);
        }

        count++;
        p = eq_end + 1;
    }

    free(resp);
    return count;
}

/* === 公开接口: 获取 RTSP URI === */
int onvif_get_stream_uri(const char *xaddrs, const char *profile_token,
                         const char *user, const char *pass,
                         char *uri, size_t uri_sz) {
    if (!xaddrs || !profile_token || !uri) return -1;

    const char *body_tpl =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope"
        " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:m=\"http://www.onvif.org/ver10/media/wsdl\""
        " xmlns:t=\"http://www.onvif.org/ver10/schema\">"
        "%s"
        "<s:Body>"
        "<m:GetStreamUri>"
        "<m:StreamSetup>"
        "<t:Stream>RTP-Unicast</t:Stream>"
        "<t:Transport><t:Protocol>RTSP</t:Protocol></t:Transport>"
        "</m:StreamSetup>"
        "<m:ProfileToken>%s</m:ProfileToken>"
        "</m:GetStreamUri>"
        "</s:Body>"
        "</s:Envelope>";

    char auth_buf[256] = {0};
    snprintf(auth_buf, sizeof(auth_buf), "%s:%s", user ? user : "", pass ? pass : "");

    char *wsse = (user && *user) ? wsse_header(user, pass) : NULL;
    char body[4096];
    snprintf(body, sizeof(body), body_tpl, wsse ? wsse : "<s:Header/>", profile_token);
    free(wsse);

    char *resp = NULL;
    if (soap_post(xaddrs,
                  "http://www.onvif.org/ver10/media/wsdl/GetStreamUri",
                  body,
                  (user && *user) ? auth_buf : NULL,
                  &resp) < 0) {
        return -1;
    }

    /* 提取 <Uri>rtsp://...</Uri> */
    int found = xml_tag_parse(resp, "Uri", uri, uri_sz);

    /* 也尝试直接搜 rtsp:// (在 free 之前) */
    if (!found) {
        const char *rtsp = strstr(resp, "rtsp://");
        if (rtsp) {
            const char *end = strchr(rtsp, '"');
            if (!end) end = strchr(rtsp, '<');
            if (!end) end = rtsp + strlen(rtsp);
            size_t len = end - rtsp;
            if (len >= uri_sz) len = uri_sz - 1;
            memcpy(uri, rtsp, len);
            uri[len] = '\0';
            found = 1;
        }
    }

    free(resp);
    return found ? 0 : -1;
}
