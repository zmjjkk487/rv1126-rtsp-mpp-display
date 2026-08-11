/* onvif.c — 极简 ONVIF 应答端
 *
 * 让板子对自家 web 平台 (web_rust) 呈现为一台标准摄像头:
 *   发现  → WS-Discovery Probe 应答 (UDP 239.255.255.250:3702)
 *   连接  → GetDeviceInformation / GetProfiles / GetStreamUri
 *   管理  → Get/SetImagingSettings (IRCUT) / Get/SetNetworkInterfaces
 *
 * 关键约定 (与 web_rust 的解析器对齐):
 *   - 任何请求都回 200 + 正常 SOAP 应答, 永不返回 Fault
 *     (web 的 has_fault 检查通过后第一级 WSS 认证即成功, 认证被忽略)
 *   - 响应元素必须用 web 解析器查找的精确前缀:
 *     trt:Profiles / tt:Name / tt:Width / tt:Height / tt:SourceToken /
 *     tt:Uri / tt:IrCutFilter / tds:NetworkInterfaces / tt:DHCP /
 *     tt:Address / tt:PrefixLength
 *   - GetStreamUri 返回 rtsp://<eth0-ip>:<port>/stream
 *
 * 纯 C11 + POSIX, 无第三方依赖。
 */
#define _DEFAULT_SOURCE   /* 暴露 struct ip_mreq (组播) 等接口 */

#include "onvif.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define WS_DISCO_PORT   3702
#define WS_DISCO_ADDR   "239.255.255.250"
#define HTTP_PORT       80
#define ONVIF_PATH      "/onvif/device_service"

struct onvif_server {
    int rtsp_port;
    onvif_profile_t profiles[8];   /* 码流列表 (字符串借用调用方的静态区) */
    int profile_count;
    char ircut[8];          /* 当前 IRCUT 模式 (仅状态, 未控硬件) */
    jpeg_provider_fn jpeg_fn;      /* MJPEG 预览帧提供者 */
    void *jpeg_ctx;
    ptz_cb_fn ptz_cb;              /* PTZ 指令回调 (接受指令后调用) */
    void *ptz_ctx;
    ptz_preset_cb_fn preset_cb;    /* 预置位回调 (Set/Goto 接受后调用) */
    void *preset_ctx;
    char presets[8][40];           /* 预置位表: "token|name" (最多 8 个) */
    int preset_count;
    pthread_t ws_tid, http_tid;
};

/* ---------------- 工具 ---------------- */

/* eth0 的 IPv4 地址与前缀长度; 返回 0 成功 */
static int eth0_addr(char *ip, size_t ip_cap, int *prefix) {
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) return -1;

    int rc = -1;
    for (struct ifaddrs *p = ifs; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(p->ifa_name, "eth0") != 0) continue;
        if (!p->ifa_netmask) continue;   /* getifaddrs 不保证 netmask 存在 */
        struct sockaddr_in *sin = (struct sockaddr_in *)p->ifa_addr;
        inet_ntop(AF_INET, &sin->sin_addr, ip, ip_cap);
        struct sockaddr_in *mask = (struct sockaddr_in *)p->ifa_netmask;
        uint32_t m = ntohl(mask->sin_addr.s_addr);
        int bits = 0;
        while (m & 0x80000000u) { bits++; m <<= 1; }
        *prefix = bits;
        rc = 0;
        break;
    }
    freeifaddrs(ifs);
    return rc;
}

/* eth0 是否 DHCP (connmanctl 状态); 查不到默认按 true */
static int eth0_dhcp(void) {
    FILE *p = popen("connmanctl config eth0 show 2>/dev/null", "r");
    if (!p) return 1;
    char line[128];
    int dhcp = 1;
    while (fgets(line, sizeof line, p)) {
        if (strstr(line, "IPv4.method")) {
            dhcp = (strstr(line, "dhcp") != NULL);
            break;
        }
    }
    pclose(p);
    return dhcp;
}

/* 前缀 → 点分掩码 */
static void prefix_to_mask(int prefix, char *out, size_t cap) {
    uint32_t m = prefix >= 32 ? 0xffffffffu : (0xffffffffu << (32 - prefix));
    snprintf(out, cap, "%u.%u.%u.%u",
             (m >> 24) & 0xff, (m >> 16) & 0xff, (m >> 8) & 0xff, m & 0xff);
}

/* 从 XML 提取 <tag>...</tag>; 返回 0 成功 */
static int xml_extract(const char *xml, const char *tag, char *out, size_t cap) {
    char open[64], close[64];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char *s = strstr(xml, open);
    if (!s) return -1;
    s += strlen(open);
    const char *e = strstr(s, close);
    if (!e) return -1;
    size_t n = (size_t)(e - s);
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
    return 0;
}

/* 大小写不敏感查找: HTTP 头名大小写不敏感,
 * reqwest/hyper 发送的是小写 "content-length:" */
static const char *ci_find(const char *haystack, const char *needle) {
    for (const char *p = haystack; *p; p++) {
        const char *a = p, *b = needle;
        while (*a && *b &&
               tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*b) return p;
    }
    return NULL;
}

/* 只保留 [0-9.] 字符 (防 shell 注入) */
static void sanitize_ipv4(char *s) {
    size_t w = 0;
    for (size_t i = 0; s[i]; i++)
        if ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')
            s[w++] = s[i];
    s[w] = '\0';
}

/* 从 SOAP 请求提取方法名与命名空间前缀:
 * 例: <tds:GetNTP/> → prefix="tds", name="GetNTP"
 * 返回 0 成功 */
static void extract_method(const char *body, char *prefix, size_t pcap,
                           char *name, size_t ncap) {
    prefix[0] = '\0';
    name[0] = '\0';
    if (!body[0]) return;   /* 空 body: 防越界读 */
    /* <s:Body> 可能带属性 (<s:Body xmlns:...>), 不能要求精确匹配 '>' */
    const char *b = strstr(body, "<s:Body");
    if (b) {
        char after = b[7];
        if (after != '>' && after != ' ' && after != '\t')
            b = NULL;
    }
    if (!b) b = body;
    const char *lt = strchr(b + 7, '<');   /* 跳过 <s:Body...> 本身 */
    if (!lt) return;
    const char *start = lt + 1;
    const char *end = start;
    while (*end && *end != '>' && *end != ' ' && *end != '/')
        end++;
    const char *colon = memchr(start, ':', (size_t)(end - start));
    if (colon) {
        size_t pn = (size_t)(colon - start);
        if (pn >= pcap) pn = pcap - 1;
        memcpy(prefix, start, pn);
        prefix[pn] = '\0';
        start = colon + 1;
    }
    size_t nn = (size_t)(end - start);
    if (nn >= ncap) nn = ncap - 1;
    memcpy(name, start, nn);
    name[nn] = '\0';
}

/* ---------------- SOAP 应答 ---------------- */

/* 组装完整 SOAP 响应体 (含 HTTP body 的 XML); 响应里绝不出现 "Fault" */
static void soap_response(const char *body_xml, char *out, size_t cap) {
    snprintf(out, cap,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"\n"
        " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\n"
        " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"\n"
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\"\n"
        " xmlns:t=\"http://www.onvif.org/ver10/schema\"\n"
        " xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">\n"
        "<s:Body>\n%s\n</s:Body>\n</s:Envelope>\n",
        body_xml);
}

static void hdr_device_info(onvif_server_t *o, char *out, size_t cap) {
    (void)o;
    snprintf(out, cap,
        "<tds:GetDeviceInformationResponse>\n"
        "<tds:Manufacturer>ATK</tds:Manufacturer>\n"
        "<tds:Model>RV1126B-IMX415</tds:Model>\n"
        "<tds:FirmwareVersion>1.0.0</tds:FirmwareVersion>\n"
        "<tds:SerialNumber>ATK-DLRV1126B-001</tds:SerialNumber>\n"
        "<tds:HardwareId>ATK-DLRV1126B</tds:HardwareId>\n"
        "</tds:GetDeviceInformationResponse>\n");
}

static void hdr_profiles(onvif_server_t *o, char *out, size_t cap) {
    size_t n = 0;
    n += snprintf(out + n, cap - n, "<trt:GetProfilesResponse>\n");
    for (int i = 0; i < o->profile_count && n < cap; i++) {
        const onvif_profile_t *p = &o->profiles[i];
        n += snprintf(out + n, cap - n,
            "<trt:Profiles token=\"%s\">\n"
            "<tt:Name>%s</tt:Name>\n"
            "<tt:Width>%d</tt:Width>\n"
            "<tt:Height>%d</tt:Height>\n"
            "<trt:VideoSourceConfiguration token=\"video_source_config1\">\n"
            "<tt:Name>VideoSourceConfig</tt:Name>\n"
            "<tt:SourceToken>video_source1</tt:SourceToken>\n"
            "<tt:Bounds x=\"0\" y=\"0\" width=\"%d\" height=\"%d\"/>\n"
            "</trt:VideoSourceConfiguration>\n"
            "<trt:VideoEncoderConfiguration token=\"video_encoder_config%d\">\n"
            "<tt:Name>%s</tt:Name>\n"
            "<tt:Encoding>H264</tt:Encoding>\n"
            "<tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>\n"
            "</trt:VideoEncoderConfiguration>\n"
            "</trt:Profiles>\n",
            p->token, p->name, p->width, p->height, p->width, p->height,
            i, p->name, p->width, p->height);
    }
    n += snprintf(out + n, cap - n, "</trt:GetProfilesResponse>\n");
}

/* GetStreamUri: 按 ProfileToken 映射到对应 RTSP 路径 */
static void hdr_stream_uri(onvif_server_t *o, const char *req,
                           char *out, size_t cap) {
    char ip[64] = "";
    int prefix = 24;
    if (eth0_addr(ip, sizeof ip, &prefix) != 0)
        snprintf(ip, sizeof ip, "192.168.1.100");

    char token[64] = "";
    xml_extract(req, "trt:ProfileToken", token, sizeof token);
    const onvif_profile_t *p = &o->profiles[0];
    for (int i = 0; i < o->profile_count; i++) {
        if (token[0] && strcmp(o->profiles[i].token, token) == 0) {
            p = &o->profiles[i];
            break;
        }
    }
    /* 规范要求 Uri 包在 MediaUri 里 — ODM 强解析器缺了它直接 NRE */
    snprintf(out, cap,
        "<trt:GetStreamUriResponse>\n"
        "<tt:MediaUri>\n"
        "<tt:Uri>rtsp://%s:%d%s</tt:Uri>\n"
        "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>\n"
        "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>\n"
        "<tt:Timeout>PT0S</tt:Timeout>\n"
        "</tt:MediaUri>\n"
        "</trt:GetStreamUriResponse>\n",
        ip, o->rtsp_port, p->path);
}

/* GetVideoSources: ODM 视频页必查 (取主码流分辨率) */
static void hdr_video_sources(onvif_server_t *o, char *out, size_t cap) {
    int w = o->profiles[0].width, h = o->profiles[0].height;
    snprintf(out, cap,
        "<trt:GetVideoSourcesResponse>\n"
        "<trt:VideoSources token=\"video_source1\">\n"
        "<tt:Name>IMX415</tt:Name>\n"
        "<tt:Framerate>30.0</tt:Framerate>\n"
        "<tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>\n"
        "</trt:VideoSources>\n"
        "</trt:GetVideoSourcesResponse>\n",
        w, h);
}

/* GetNetworkProtocols: ODM 添加时查网络服务端口 */
static void hdr_network_protocols(onvif_server_t *o, char *out, size_t cap) {
    (void)o;
    snprintf(out, cap,
        "<tds:GetNetworkProtocolsResponse>\n"
        "<tds:NetworkProtocols><tt:Name>RTSP</tt:Name><tt:Enabled>true</tt:Enabled>"
        "<tt:Port>%d</tt:Port></tds:NetworkProtocols>\n"
        "<tds:NetworkProtocols><tt:Name>HTTP</tt:Name><tt:Enabled>true</tt:Enabled>"
        "<tt:Port>80</tt:Port></tds:NetworkProtocols>\n"
        "</tds:GetNetworkProtocolsResponse>\n",
        o->rtsp_port);
}

static void hdr_get_imaging(onvif_server_t *o, char *out, size_t cap) {
    snprintf(out, cap,
        "<trt:GetImagingSettingsResponse>\n"
        "<tt:ImagingSettings>\n"
        "<tt:IrCutFilter>%s</tt:IrCutFilter>\n"
        "</tt:ImagingSettings>\n"
        "</trt:GetImagingSettingsResponse>\n",
        o->ircut);
}

static void hdr_set_imaging(char *out, size_t cap) {
    snprintf(out, cap,
        "<trt:SetImagingSettingsResponse/>\n");
}

static void hdr_get_network(onvif_server_t *o, char *out, size_t cap) {
    (void)o;
    char ip[64] = "";
    int prefix = 24;
    int dhcp = 1;
    if (eth0_addr(ip, sizeof ip, &prefix) != 0)
        snprintf(ip, sizeof ip, "0.0.0.0");
    dhcp = eth0_dhcp();
    snprintf(out, cap,
        "<tds:GetNetworkInterfacesResponse>\n"
        "<tds:NetworkInterfaces token=\"eth0\">\n"
        "<tt:Enabled>true</tt:Enabled>\n"
        "<tt:Info><tt:Name>eth0</tt:Name></tt:Info>\n"
        "<tt:IPv4><tt:Enabled>true</tt:Enabled>\n"
        "<tt:Config>\n"
        "<tt:DHCP>%s</tt:DHCP>\n"
        "<tt:Manual>\n"
        "<tt:Address>%s</tt:Address>\n"
        "<tt:PrefixLength>%d</tt:PrefixLength>\n"
        "</tt:Manual>\n"
        "</tt:Config></tt:IPv4>\n"
        "</tds:NetworkInterfaces>\n"
        "</tds:GetNetworkInterfacesResponse>\n",
        dhcp ? "true" : "false", ip, prefix);
}

/* GetSystemDateAndTime: ODM 等严格客户端添加设备时必查 */
static void hdr_system_date_time(onvif_server_t *o, char *out, size_t cap) {
    (void)o;
    time_t now = time(NULL);
    struct tm utc, loc;
    gmtime_r(&now, &utc);
    localtime_r(&now, &loc);
    /* 时区偏移 (秒) → +HH:MM */
    long off = loc.tm_gmtoff;
    char tz[32];   /* %+03ld 最多 17 字节 + 冒号分秒, 16 会截断 (Wformat-truncation) */
    snprintf(tz, sizeof tz, "%+03ld:%02ld", off / 3600, (off % 3600) / 60);
    snprintf(out, cap,
        "<tds:GetSystemDateAndTimeResponse>\n"
        "<tt:SystemDateAndTime>\n"
        "<tt:DateTimeType>Manual</tt:DateTimeType>\n"
        "<tt:UTCDateTime>"
        "<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date>"
        "<tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time>"
        "</tt:UTCDateTime>\n"
        "<tt:LocalDateTime>"
        "<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date>"
        "<tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time>"
        "<tt:TZ><tt:TZOffset>%s</tt:TZOffset></tt:TZ>"
        "</tt:LocalDateTime>\n"
        "</tt:SystemDateAndTime>\n"
        "</tds:GetSystemDateAndTimeResponse>\n",
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
        utc.tm_hour, utc.tm_min, utc.tm_sec,
        loc.tm_year + 1900, loc.tm_mon + 1, loc.tm_mday,
        loc.tm_hour, loc.tm_min, loc.tm_sec, tz);
}

/* ODM 添加设备时还会查询的一批 tds: 方法, 全部按规范给真实结构
 * (空壳响应会让 ODM 解析器拿 null → NRE 崩溃) */

static void hdr_get_scopes(onvif_server_t *o, char *out, size_t cap) {
    (void)o;
    snprintf(out, cap,
        "<tds:GetScopesResponse>\n"
        "<tds:Scopes>\n"
        "<tds:Scope><tt:ScopeItem>onvif://www.onvif.org/name/RV1126B</tt:ScopeItem>"
        "<tt:Configurable>false</tt:Configurable></tds:Scope>\n"
        "<tds:Scope><tt:ScopeItem>onvif://www.onvif.org/hardware/IMX415</tt:ScopeItem>"
        "<tt:Configurable>false</tt:Configurable></tds:Scope>\n"
        "<tds:Scope><tt:ScopeItem>onvif://www.onvif.org/Profile/Streaming</tt:ScopeItem>"
        "<tt:Configurable>false</tt:Configurable></tds:Scope>\n"
        "</tds:Scopes>\n"
        "</tds:GetScopesResponse>\n");
}

static void hdr_get_dns(onvif_server_t *o, char *out, size_t cap) {
    (void)o;
    snprintf(out, cap,
        "<tds:GetDNSResponse>\n"
        "<tds:DNS><tt:FromDHCP>true</tt:FromDHCP></tds:DNS>\n"
        "</tds:GetDNSResponse>\n");
}

static void hdr_get_hostname(onvif_server_t *o, char *out, size_t cap) {
    (void)o;
    snprintf(out, cap,
        "<tds:GetHostnameResponse>\n"
        "<tds:HostnameInformation><tt:FromDHCP>true</tt:FromDHCP>"
        "<tt:Name>rv1126b</tt:Name></tds:HostnameInformation>\n"
        "</tds:GetHostnameResponse>\n");
}

static void hdr_get_ntp(onvif_server_t *o, char *out, size_t cap) {
    (void)o;
    snprintf(out, cap,
        "<tds:GetNTPResponse>\n"
        "<tds:NTP><tt:FromDHCP>true</tt:FromDHCP></tds:NTP>\n"
        "</tds:GetNTPResponse>\n");
}

static void hdr_get_gateway(onvif_server_t *o, char *out, size_t cap) {
    (void)o;
    char gw[64] = "";
    FILE *p = popen("ip route show default | awk '{print $3; exit}'", "r");
    if (p) {
        if (fgets(gw, sizeof gw, p)) {
            size_t n = strlen(gw);
            if (n && gw[n-1] == '\n') gw[n-1] = '\0';
        }
        pclose(p);
    }
    snprintf(out, cap,
        "<tds:GetNetworkDefaultGatewayResponse>\n"
        "<tds:NetworkGateway><tt:IPv4Address>%s</tt:IPv4Address></tds:NetworkGateway>\n"
        "</tds:GetNetworkDefaultGatewayResponse>\n",
        gw[0] ? gw : "192.168.50.1");
}

/* GetSnapshotUri: ODM 设备列表缩略图; 注: /snapshot.jpg 尚未实现,
 * 返回合法结构防 ODM 解析 NRE */
static void hdr_snapshot_uri(onvif_server_t *o, char *out, size_t cap) {
    (void)o;
    char ip[64] = "";
    int prefix = 24;
    if (eth0_addr(ip, sizeof ip, &prefix) != 0)
        snprintf(ip, sizeof ip, "192.168.1.100");
    snprintf(out, cap,
        "<trt:GetSnapshotUriResponse>\n"
        "<tt:MediaUri>\n"
        "<tt:Uri>http://%s/snapshot.jpg</tt:Uri>\n"
        "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>\n"
        "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>\n"
        "<tt:Timeout>PT0S</tt:Timeout>\n"
        "</tt:MediaUri>\n"
        "</trt:GetSnapshotUriResponse>\n", ip);
}

/* WS-Eventing Subscribe: ODM 事件订阅; 声明订阅管理器地址即可 */
static void hdr_subscribe(onvif_server_t *o, char *out, size_t cap) {
    (void)o;
    char ip[64] = "";
    int prefix = 24;
    if (eth0_addr(ip, sizeof ip, &prefix) != 0)
        snprintf(ip, sizeof ip, "192.168.1.100");
    snprintf(out, cap,
        "<tds:SubscribeResponse>\n"
        "<wse:SubscriptionManager "
        "xmlns:wse=\"http://schemas.xmlsoap.org/ws/2004/08/eventing\">\n"
        "<wsa:Address "
        "xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\">"
        "http://%s/onvif/event_service</wsa:Address>\n"
        "</wse:SubscriptionManager>\n"
        "<tt:CurrentTime>PT0S</tt:CurrentTime>\n"
        "<tt:TerminationTime>PT3600S</tt:TerminationTime>\n"
        "</tds:SubscribeResponse>\n", ip);
}

/* GetCapabilities: ODM 添加设备时必查 (仅声明存在的服务, XAddr 指向本机) */
static void hdr_capabilities(onvif_server_t *o, char *out, size_t cap) {
    (void)o;
    char ip[64] = "";
    int prefix = 24;
    if (eth0_addr(ip, sizeof ip, &prefix) != 0)
        snprintf(ip, sizeof ip, "192.168.1.100");
    snprintf(out, cap,
        "<tds:GetCapabilitiesResponse>\n"
        "<tds:Capabilities>\n"
        "<tt:Device><tt:XAddr>http://%s/onvif/device_service</tt:XAddr></tt:Device>\n"
        "<tt:Media><tt:XAddr>http://%s/onvif/device_service</tt:XAddr>"
        "<tt:StreamingCapabilities><tt:RTPUnicast>true</tt:RTPUnicast>"
        "<tt:RTPMulticast>false</tt:RTPMulticast></tt:StreamingCapabilities></tt:Media>\n"
        "<tt:Imaging><tt:XAddr>http://%s/onvif/device_service</tt:XAddr></tt:Imaging>\n"
        "<tt:Events><tt:XAddr>http://%s/onvif/device_service</tt:XAddr></tt:Events>\n"
        "</tds:Capabilities>\n"
        "</tds:GetCapabilitiesResponse>\n",
        ip, ip, ip, ip);
}

/* GetServices: 声明支持的 ONVIF 服务列表 */
static void hdr_get_services(onvif_server_t *o, char *out, size_t cap) {
    (void)o;
    char ip[64] = "";
    int prefix = 24;
    if (eth0_addr(ip, sizeof ip, &prefix) != 0)
        snprintf(ip, sizeof ip, "192.168.1.100");
    snprintf(out, cap,
        "<tds:GetServicesResponse>\n"
        "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>"
        "<tds:XAddr>http://%s/onvif/device_service</tds:XAddr>"
        "<tds:Version><tt:Major>2</tt:Major><tt:Minor>0</tt:Minor></tds:Version></tds:Service>\n"
        "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>"
        "<tds:XAddr>http://%s/onvif/device_service</tds:XAddr>"
        "<tds:Version><tt:Major>2</tt:Major><tt:Minor>0</tt:Minor></tds:Version></tds:Service>\n"
        "</tds:GetServicesResponse>\n",
        ip, ip);
}

/* SetNetworkInterfaces: 解析请求并用 connmanctl 应用 (与 web 端 /api/network 同法) */
static void hdr_set_network(onvif_server_t *o, const char *req, char *out, size_t cap) {
    (void)o;
    char dhcp_s[8] = "false", ip[64] = "", prefix_s[16] = "24";
    xml_extract(req, "tt:DHCP", dhcp_s, sizeof dhcp_s);
    xml_extract(req, "tt:Address", ip, sizeof ip);
    xml_extract(req, "tt:PrefixLength", prefix_s, sizeof prefix_s);

    if (strstr(dhcp_s, "true")) {
        int rc = system("connmanctl config eth0 --ipv4 dhcp >/dev/null 2>&1");
        printf("[ONVIF] SetNetworkInterfaces → DHCP (rc=%d)\n", rc);
    } else {
        sanitize_ipv4(ip);
        int prefix = atoi(prefix_s);
        if (prefix < 1 || prefix > 30) prefix = 24;
        char mask[16];
        prefix_to_mask(prefix, mask, sizeof mask);
        char gw[64] = "";
        FILE *p = popen("ip route show default | awk '{print $3; exit}'", "r");
        if (p) {
            if (fgets(gw, sizeof gw, p)) {
                size_t n = strlen(gw);
                if (n && gw[n-1] == '\n') gw[n-1] = '\0';
            }
            pclose(p);
        }
        if (ip[0] && gw[0]) {
            char cmd[256];
            snprintf(cmd, sizeof cmd,
                     "connmanctl config eth0 --ipv4 manual %s %s %s >/dev/null 2>&1",
                     ip, mask, gw);
            printf("[ONVIF] SetNetworkInterfaces → 静态 %s/%s\n", ip, mask);
            int rc = system(cmd);
            if (rc != 0)
                printf("[ONVIF] connmanctl 执行失败 (rc=%d)\n", rc);
        } else {
            printf("[ONVIF] SetNetworkInterfaces 忽略 (地址/网关缺失)\n");
        }
    }
    snprintf(out, cap, "<tds:SetNetworkInterfacesResponse/>\n");
}

/* ---------------- PTZ 云台控制 (ver20/ptz/wsdl) ----------------
 * ContinuousMove: <tptz:Velocity><tt:PanTilt><tt:x>±1</tt:x></tt:PanTilt>
 *   x>0 右转, x<0 左转 — 解析成功 (= 接受指令) 后立即回调,
 *   由调用方决定动作 (本机无云台 → 屏幕标识验证; 真实云台 → 电机控制) */

static void hdr_ptz_continuous(onvif_server_t *o, const char *req,
                               char *out, size_t cap) {
    char xs[16] = "0";
    /* 前缀无关: 兼容 tt:x / t:x / 裸 x */
    if (xml_extract(req, "tt:x", xs, sizeof xs) != 0 &&
        xml_extract(req, "t:x", xs, sizeof xs) != 0)
        xml_extract(req, "x", xs, sizeof xs);
    double x = atof(xs);
    ptz_dir_t dir = x > 0.01 ? PTZ_RIGHT : (x < -0.01 ? PTZ_LEFT : PTZ_STOP);
    if (o->ptz_cb)
        o->ptz_cb(dir, x < 0 ? -x : x, o->ptz_ctx);   /* 接受指令后的回调 */
    snprintf(out, cap, "<tptz:ContinuousMoveResponse/>\n");
}

static void hdr_ptz_stop(onvif_server_t *o, char *out, size_t cap) {
    if (o->ptz_cb)
        o->ptz_cb(PTZ_STOP, 0, o->ptz_ctx);
    snprintf(out, cap, "<tptz:StopResponse/>\n");
}

/* SetPreset: 保存预置位 (token|name), 接受后回调 */
static void hdr_ptz_set_preset(onvif_server_t *o, const char *req,
                               char *out, size_t cap) {
    char token[32] = "", name[32] = "";
    if (xml_extract(req, "tptz:PresetToken", token, sizeof token) != 0)
        xml_extract(req, "PresetToken", token, sizeof token);
    if (xml_extract(req, "tptz:PresetName", name, sizeof name) != 0)
        xml_extract(req, "PresetName", name, sizeof name);
    if (!token[0]) {
        snprintf(out, cap, "<tptz:SetPresetResponse/>\n");
        return;
    }
    /* 更新或追加 (上限 8 个) */
    int found = -1;
    for (int i = 0; i < o->preset_count; i++)
        if (strncmp(o->presets[i], token, strlen(token)) == 0) { found = i; break; }
    if (found < 0 && o->preset_count < 8)
        found = o->preset_count++;
    if (found >= 0)
        snprintf(o->presets[found], sizeof o->presets[0], "%s|%s",
                 token, name[0] ? name : token);
    if (o->preset_cb)
        o->preset_cb(token, 0, o->preset_ctx);   /* 接受指令后的回调 */
    snprintf(out, cap,
             "<tptz:SetPresetResponse><tptz:PresetToken>%s</tptz:PresetToken>"
             "</tptz:SetPresetResponse>\n", token);
}

/* GotoPreset: 调用预置位, 接受后回调 */
static void hdr_ptz_goto_preset(onvif_server_t *o, const char *req,
                                char *out, size_t cap) {
    char token[32] = "";
    if (xml_extract(req, "tptz:PresetToken", token, sizeof token) != 0)
        xml_extract(req, "PresetToken", token, sizeof token);
    if (o->preset_cb)
        o->preset_cb(token[0] ? token : "?", 1, o->preset_ctx);
    snprintf(out, cap, "<tptz:GotoPresetResponse/>\n");
}

/* GetPresets: 返回已保存的预置位列表 (管理工具/ODM 用) */
static void hdr_ptz_get_presets(onvif_server_t *o, char *out, size_t cap) {
    char list[1024] = "";
    size_t used = 0;
    for (int i = 0; i < o->preset_count; i++) {
        char *sep = strchr(o->presets[i], '|');
        const char *name = sep ? sep + 1 : o->presets[i];
        char token[40];
        size_t tl = sep ? (size_t)(sep - o->presets[i]) : strlen(o->presets[i]);
        if (tl >= sizeof token) tl = sizeof token - 1;
        memcpy(token, o->presets[i], tl);
        token[tl] = '\0';
        int n = snprintf(list + used, sizeof list - used,
                         "<tptz:Preset><tptz:PresetToken>%s</tptz:PresetToken>"
                         "<tptz:PresetName>%s</tptz:PresetName></tptz:Preset>",
                         token, name);
        if (n <= 0 || used + (size_t)n >= sizeof list) break;
        used += (size_t)n;
    }
    snprintf(out, cap, "<tptz:GetPresetsResponse>%s</tptz:GetPresetsResponse>\n",
             list);
}

/* RemovePreset: 删除预置位 */
static void hdr_ptz_remove_preset(onvif_server_t *o, const char *req,
                                  char *out, size_t cap) {
    char token[32] = "";
    if (xml_extract(req, "tptz:PresetToken", token, sizeof token) != 0)
        xml_extract(req, "PresetToken", token, sizeof token);
    for (int i = 0; i < o->preset_count; i++) {
        if (strncmp(o->presets[i], token, strlen(token)) == 0) {
            for (int j = i; j < o->preset_count - 1; j++)
                memcpy(o->presets[j], o->presets[j + 1], sizeof o->presets[0]);
            o->preset_count--;
            break;
        }
    }
    if (o->preset_cb)
        o->preset_cb(token, 2, o->preset_ctx);
    snprintf(out, cap, "<tptz:RemovePresetResponse/>\n");
}

/* ---------------- MJPEG 低延迟预览 (GET /preview) ---------------- */

/* 单调时钟微秒 (onvif.c 无 glib 依赖, 不能用 g_get_monotonic_time —
 * 隐式声明会 32 位截断成负数) */
static int64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

typedef struct { onvif_server_t *o; int fd; } preview_arg_t;

static void *preview_thread(void *arg) {
    preview_arg_t *pa = arg;
    onvif_server_t *o = pa->o;
    int fd = pa->fd;
    free(pa);

    /* multipart/x-mixed-replace: 浏览器 <img src="/preview"> 直接显示 */
    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n";
    if (send(fd, hdr, strlen(hdr), MSG_NOSIGNAL) < 0) {
        close(fd);
        return NULL;
    }

    /* 帧缓冲归本线程所有: provider 锁内拷贝, 消除跨锁 UAF (扫1 #8) */
    uint8_t *frm = malloc(1 << 20);   /* 1MB: D1 480p JPEG 远小于此 */
    if (!frm) {
        close(fd);
        return NULL;
    }

    long nframe = 0;
    for (;;) {
        jpeg_frame_t f = { .data = frm, .cap = 1 << 20 };
        if (o->jpeg_fn && o->jpeg_fn(o->jpeg_ctx, &f) == 0 && f.len > 0) {
            char part[128];
            int n = snprintf(part, sizeof part,
                "--frame\r\nContent-Type: image/jpeg\r\n"
                "Content-Length: %zu\r\n\r\n", f.len);
            if (send(fd, part, n, MSG_NOSIGNAL) < 0) break;
            if (send(fd, f.data, f.len, MSG_NOSIGNAL) < 0) break;
            if (send(fd, "\r\n", 2, MSG_NOSIGNAL) < 0) break;
            if (++nframe == 1 || nframe % 50 == 0) {   /* 调试: 首帧+每50帧 */
                printf("[preview] 帧延迟 %lld ms (f.ts=%lld, now=%lld)\n",
                       (long long)(mono_us() - f.ts) / 1000,
                       (long long)f.ts,
                       (long long)mono_us());
            }
            usleep(100000);   /* 节流: 最高 10fps, 否则旧帧会被狂发 */
        } else {
            usleep(10000);   /* 无帧等 10ms */
        }
    }
    free(frm);
    close(fd);
    return NULL;
}

/* ---------------- HTTP :80 SOAP 分发 ---------------- */

static void dispatch(onvif_server_t *o, const char *body, char *resp, size_t cap) {
    char body_xml[2048];

    /* 请求日志 (调试用, 交付时可按需保留) */
    const char *m = "unknown";
    if (strstr(body, "SetImagingSettings")) m = "SetImagingSettings";
    else if (strstr(body, "GetImagingSettings")) m = "GetImagingSettings";
    else if (strstr(body, "SetNetworkInterfaces")) m = "SetNetworkInterfaces";
    else if (strstr(body, "GetNetworkInterfaces")) m = "GetNetworkInterfaces";
    else if (strstr(body, "ContinuousMove")) m = "ContinuousMove";
    else if (strstr(body, "Stop>")) m = "PTZ Stop";
    else if (strstr(body, "GetPresets")) m = "GetPresets";
    else if (strstr(body, "SetPreset")) m = "SetPreset";
    else if (strstr(body, "GotoPreset")) m = "GotoPreset";
    else if (strstr(body, "RemovePreset")) m = "RemovePreset";
    else if (strstr(body, "GetSystemDateAndTime")) m = "GetSystemDateAndTime";
    else if (strstr(body, "GetCapabilities")) m = "GetCapabilities";
    else if (strstr(body, "GetServices")) m = "GetServices";
    else if (strstr(body, "GetVideoSources")) m = "GetVideoSources";
    else if (strstr(body, "GetNetworkProtocols")) m = "GetNetworkProtocols";
    else if (strstr(body, "GetScopes")) m = "GetScopes";
    else if (strstr(body, "GetDNS")) m = "GetDNS";
    else if (strstr(body, "GetHostname")) m = "GetHostname";
    else if (strstr(body, "GetNTP")) m = "GetNTP";
    else if (strstr(body, "GetNetworkDefaultGateway")) m = "GetNetworkDefaultGateway";
    else if (strstr(body, "GetSnapshotUri")) m = "GetSnapshotUri";
    else if (strstr(body, "Subscribe")) m = "Subscribe";
    else if (strstr(body, "GetStreamUri")) m = "GetStreamUri";
    else if (strstr(body, "GetProfiles")) m = "GetProfiles";
    else if (strstr(body, "GetDeviceInformation")) m = "GetDeviceInformation";
    printf("[ONVIF] POST %s (%zu 字节)\n", m, strlen(body));

    if (strstr(body, "GetPresets")) {
        hdr_ptz_get_presets(o, body_xml, sizeof body_xml);
    } else if (strstr(body, "SetPreset")) {
        hdr_ptz_set_preset(o, body, body_xml, sizeof body_xml);
    } else if (strstr(body, "GotoPreset")) {
        hdr_ptz_goto_preset(o, body, body_xml, sizeof body_xml);
    } else if (strstr(body, "RemovePreset")) {
        hdr_ptz_remove_preset(o, body, body_xml, sizeof body_xml);
    } else if (strstr(body, "ContinuousMove")) {
        hdr_ptz_continuous(o, body, body_xml, sizeof body_xml);
    } else if (strstr(body, "Stop>")) {
        hdr_ptz_stop(o, body_xml, sizeof body_xml);
    } else if (strstr(body, "SetImagingSettings")) {
        /* 更新状态 (仅 UI 回显, 未控 IRCUT 硬件) */
        char mode[8] = "";
        if (xml_extract(body, "t:IrCutFilter", mode, sizeof mode) == 0 ||
            xml_extract(body, "tt:IrCutFilter", mode, sizeof mode) == 0) {
            if (strcmp(mode, "ON") == 0 || strcmp(mode, "OFF") == 0 ||
                strcmp(mode, "AUTO") == 0)
                snprintf(o->ircut, sizeof o->ircut, "%s", mode);
        }
        hdr_set_imaging(body_xml, sizeof body_xml);
    } else if (strstr(body, "GetImagingSettings")) {
        hdr_get_imaging(o, body_xml, sizeof body_xml);
    } else if (strstr(body, "SetNetworkInterfaces")) {
        hdr_set_network(o, body, body_xml, sizeof body_xml);
    } else if (strstr(body, "GetNetworkInterfaces")) {
        hdr_get_network(o, body_xml, sizeof body_xml);
    } else if (strstr(body, "GetVideoSources")) {
        hdr_video_sources(o, body_xml, sizeof body_xml);
    } else if (strstr(body, "GetNetworkProtocols")) {
        hdr_network_protocols(o, body_xml, sizeof body_xml);
    } else if (strstr(body, "GetScopes")) {
        hdr_get_scopes(o, body_xml, sizeof body_xml);
    } else if (strstr(body, "GetDNS")) {
        hdr_get_dns(o, body_xml, sizeof body_xml);
    } else if (strstr(body, "GetHostname")) {
        hdr_get_hostname(o, body_xml, sizeof body_xml);
    } else if (strstr(body, "GetNTP")) {
        hdr_get_ntp(o, body_xml, sizeof body_xml);
    } else if (strstr(body, "GetNetworkDefaultGateway")) {
        hdr_get_gateway(o, body_xml, sizeof body_xml);
    } else if (strstr(body, "GetSnapshotUri")) {
        hdr_snapshot_uri(o, body_xml, sizeof body_xml);
    } else if (strstr(body, "Subscribe")) {
        hdr_subscribe(o, body_xml, sizeof body_xml);
    } else if (strstr(body, "GetStreamUri")) {
        hdr_stream_uri(o, body, body_xml, sizeof body_xml);
    } else if (strstr(body, "GetProfiles")) {
        hdr_profiles(o, body_xml, sizeof body_xml);
    } else if (strstr(body, "GetDeviceInformation")) {
        hdr_device_info(o, body_xml, sizeof body_xml);
    } else if (strstr(body, "GetSystemDateAndTime")) {
        hdr_system_date_time(o, body_xml, sizeof body_xml);
    } else if (strstr(body, "GetCapabilities")) {
        hdr_capabilities(o, body_xml, sizeof body_xml);
    } else if (strstr(body, "GetServices")) {
        hdr_get_services(o, body_xml, sizeof body_xml);
    } else {
        /* 未知请求: 回一个同名的空响应元素 (如 <tds:GetNTPResponse/>),
         * 让 ODM 等强解析客户端能找到匹配元素 — 空 Body 会让它们
         * 反序列化拿到 null 直接 NRE 崩溃 */
        char mprefix[32], mname[64];
        extract_method(body, mprefix, sizeof mprefix, mname, sizeof mname);
        if (mname[0]) {
            if (!mprefix[0])
                snprintf(mprefix, sizeof mprefix, "tds");
            snprintf(body_xml, sizeof body_xml, "<%s:%sResponse/>\n",
                     mprefix, mname);
            printf("[ONVIF] 未实现方法 %s:%s → 回空响应\n", mprefix, mname);
        } else {
            body_xml[0] = '\0';
        }
    }

    char xml[2400];
    soap_response(body_xml, xml, sizeof xml);
    snprintf(resp, cap,
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: application/soap+xml; charset=utf-8\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n\r\n"
             "%s",
             strlen(xml), xml);
}

/* ---------------- Set* 管理操作鉴权 (HTTP Basic Auth) ----------------
 * 凭据与 web 后台共用 /root/camera-web/passwd (admin:<sha256 hex>),
 * 每次实时读文件校验 — 改密即时生效; 无文件/无头/校验失败一律拒绝
 * (fail closed)。Get* 保持开放 (ONVIF 发现兼容), Set* 必须鉴权。
 * 内嵌 SHA-256/Base64: 纯 C 零依赖, 板端编译无需额外库 */

typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t buflen;
} sha256_ctx_t;

static void sha256_init(sha256_ctx_t *c) {
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->buflen = 0;
}

static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define SHR(x,n)  ((x)>>(n))

static void sha256_block(sha256_ctx_t *c, const unsigned char *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|
               ((uint32_t)p[i*4+2]<<8)|(uint32_t)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i-15],7)^ROTR(w[i-15],18)^SHR(w[i-15],3);
        uint32_t s1 = ROTR(w[i-2],17)^ROTR(w[i-2],19)^SHR(w[i-2],10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=c->h[0]; b=c->h[1]; cc=c->h[2]; d=c->h[3];
    e=c->h[4]; f=c->h[5]; g=c->h[6]; h=c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e,6)^ROTR(e,11)^ROTR(e,25);
        uint32_t ch = (e&f)^((~e)&g);
        t1 = h + S1 + ch + SHA256_K[i] + w[i];
        uint32_t S0 = ROTR(a,2)^ROTR(a,13)^ROTR(a,22);
        uint32_t maj = (a&b)^(a&cc)^(b&cc);
        t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}
#undef ROTR
#undef SHR

static void sha256_update(sha256_ctx_t *c, const void *data, size_t n) {
    const unsigned char *p = data;
    c->len += n;
    while (n > 0) {
        size_t take = 64 - c->buflen;
        if (take > n) take = n;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take; p += take; n -= take;
        if (c->buflen == 64) { sha256_block(c, c->buf); c->buflen = 0; }
    }
}

static void sha256_final(sha256_ctx_t *c, unsigned char out[32]) {
    uint64_t bits = c->len * 8;
    unsigned char pad = 0x80;
    sha256_update(c, &pad, 1);
    unsigned char z = 0;
    while (c->buflen != 56) sha256_update(c, &z, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - i*8));
    sha256_update(c, lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (unsigned char)(c->h[i] >> 24);
        out[i*4+1] = (unsigned char)(c->h[i] >> 16);
        out[i*4+2] = (unsigned char)(c->h[i] >> 8);
        out[i*4+3] = (unsigned char)(c->h[i]);
    }
}

static int b64v(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int base64_decode(const char *in, size_t n, unsigned char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i + 4 <= n; i += 4) {
        int a = b64v(in[i]), b = b64v(in[i+1]);
        int c = in[i+2] == '=' ? 0 : b64v(in[i+2]);
        int d = in[i+3] == '=' ? 0 : b64v(in[i+3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return -1;
        if (o >= cap) return -1;
        out[o++] = (unsigned char)((a << 2) | (b >> 4));
        if (in[i+2] != '=') {
            if (o >= cap) return -1;
            out[o++] = (unsigned char)(((b & 0x0f) << 4) | (c >> 2));
        }
        if (in[i+3] != '=') {
            if (o >= cap) return -1;
            out[o++] = (unsigned char)(((c & 0x03) << 6) | d);
        }
    }
    return (int)o;
}

/* Authorization: Basic base64(user:pass) — 用户名不校验 (同 web), 只验密码 */
static int check_basic_auth(const char *hdr) {
    const char *ah = ci_find(hdr, "Authorization:");
    if (!ah) return 0;
    const char *b64 = strstr(ah, "Basic ");
    if (!b64) return 0;
    b64 += 6;
    const char *end = strchr(b64, '\r');
    if (!end) end = strchr(b64, '\n');
    if (!end) return 0;

    unsigned char dec[128];
    int dlen = base64_decode(b64, (size_t)(end - b64), dec, sizeof dec - 1);
    if (dlen <= 0) return 0;
    dec[dlen] = '\0';
    char *colon = memchr(dec, ':', dlen);
    if (!colon) return 0;
    const char *pass = (const char *)colon + 1;

    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, pass, strlen(pass));
    unsigned char digest[32];
    sha256_final(&c, digest);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
    hex[64] = '\0';

    FILE *fp = fopen("/root/camera-web/passwd", "r");
    if (!fp) return 0;
    char line[128];
    int ok = 0;
    if (fgets(line, sizeof line, fp)) {
        char *stored = strstr(line, ":");
        if (stored) {
            stored++;
            size_t l = strlen(stored);
            while (l > 0 && (stored[l-1] == '\n' || stored[l-1] == '\r'))
                stored[--l] = '\0';
            ok = (strcmp(stored, hex) == 0);
        }
    }
    fclose(fp);
    return ok;
}

/* 读满 n 字节 (简单阻塞读, HTTP 线程独立, 无并发问题) */
static int http_read_exact(int fd, char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static void *http_thread(void *arg) {
    onvif_server_t *o = arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("[ONVIF] socket"); return NULL; }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(HTTP_PORT),
    };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("[ONVIF] bind :80"); close(fd); return NULL;
    }
    if (listen(fd, 8) < 0) { perror("[ONVIF] listen"); close(fd); return NULL; }
    printf("[ONVIF] HTTP :%d%s 就绪\n", HTTP_PORT, ONVIF_PATH);

    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) continue;

        /* 500ms 超时: 单线程 accept, PTZ 等控制指令不能被慢连接拖住
         * (用户实测: 慢连接会拖出 1-2 秒的按钮卡顿) */
        struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

        /* 读请求头直到空行 (没读到空行一律关闭: 半头/超时/EOF
         * 都不再继续解析, 防止半开连接拖住单线程 HTTP 服务) */
        char hdr[4096];
        size_t n = 0;
        int got_hdr = 0;
        while (n < sizeof hdr - 1) {
            ssize_t r = recv(cfd, hdr + n, 1, 0);
            if (r <= 0) break;
            n++;
            if (n >= 4 && hdr[n-4] == '\r' && hdr[n-3] == '\n' &&
                hdr[n-2] == '\r' && hdr[n-1] == '\n') {
                got_hdr = 1;
                break;
            }
        }
        if (!got_hdr) { close(cfd); continue; }

        /* MJPEG 低延迟预览: GET /preview → 独立线程长连接推流,
         * 不占用 ONVIF HTTP 单线程服务 */
        if (strncmp(hdr, "GET /preview", 12) == 0) {
            struct timeval stv = { .tv_sec = 5, .tv_usec = 0 };
            setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);
            preview_arg_t *pa = malloc(sizeof *pa);
            if (pa) {
                pa->o = o;
                pa->fd = cfd;
                pthread_t pt;
                pthread_create(&pt, NULL, preview_thread, pa);
                pthread_detach(pt);
                continue;
            }
            close(cfd);
            continue;
        }

        /* Content-Length (上限 8191, 保证 body[clen] 定界不越界;
         * 头名大小写不敏感 — reqwest 发小写) */
        long clen = 0;
        const char *cl = ci_find(hdr, "Content-Length:");
        if (cl) clen = atol(cl + 15);
        if (clen < 0 || clen > 8191) { close(cfd); continue; }

        /* 读 body (SO_RCVTIMEO 兜底: 半开连接超时自动关闭) */
        char body[8192];
        if (clen > 0 && http_read_exact(cfd, body, (size_t)clen) < 0) {
            close(cfd);
            continue;
        }
        body[clen] = '\0';

        /* Set* 管理操作鉴权 (扫2 #4): SetNetworkInterfaces 真改板子网络,
         * SetImagingSettings 改状态 — 均需 HTTP Basic Auth (与 web 同凭据) */
        if ((strstr(body, "SetNetworkInterfaces") ||
             strstr(body, "SetImagingSettings")) && !check_basic_auth(hdr)) {
            static const char r401[] =
                "HTTP/1.1 401 Unauthorized\r\n"
                "Content-Type: application/soap+xml; charset=utf-8\r\n"
                "Content-Length: 0\r\n"
                "WWW-Authenticate: Basic realm=\"onvif\"\r\n"
                "Connection: close\r\n\r\n";
            send(cfd, r401, sizeof r401 - 1, MSG_NOSIGNAL);
            close(cfd);
            continue;
        }

        char resp[4096];
        dispatch(o, body, resp, sizeof resp);
        send(cfd, resp, strlen(resp), MSG_NOSIGNAL);
        close(cfd);
    }
    return NULL;
}

/* ---------------- WS-Discovery ---------------- */

static void *wsdisco_thread(void *arg) {
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("[ONVIF] udp socket"); return NULL; }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on);
#endif
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(WS_DISCO_PORT),
    };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("[ONVIF] bind 3702"); close(fd); return NULL;
    }
    struct ip_mreq mreq = {
        .imr_multiaddr.s_addr = inet_addr(WS_DISCO_ADDR),
        .imr_interface.s_addr = htonl(INADDR_ANY),
    };
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) < 0)
        perror("[ONVIF] 加入组播组");
    printf("[ONVIF] WS-Discovery %s:%d 就绪\n", WS_DISCO_ADDR, WS_DISCO_PORT);

    char buf[4096];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        ssize_t n = recvfrom(fd, buf, sizeof buf - 1, 0,
                             (struct sockaddr *)&from, &flen);
        if (n <= 0) continue;
        buf[n] = '\0';

        /* 匹配 Probe 元素 (前缀无关: 找 "Probe>" — <d:Probe>/<wsd:Probe>/<Probe>
         * 全命中, 且不会误匹配 "ProbeMatches>" 或 Action 里的 .../Probe<) */
        if (!strstr(buf, "Probe>")) continue;

        /* 回显请求的 MessageID 到 RelatesTo (前缀无关 + 过滤特殊字符防注入) */
        char msgid[256] = "uuid:onvif-probe";
        const char *mid = strstr(buf, "MessageID>");
        if (mid) {
            const char *gt = strchr(mid, '>');   /* 值从 MessageID> 之后的 > 开始 */
            if (gt) {
                const char *vs = gt + 1;
                const char *ve = strchr(vs, '<');   /* 到闭合标签的 < 结束 */
                if (ve && ve - vs < 200) {
                    size_t n = 0;
                    for (const char *q = vs; q < ve && n < sizeof msgid - 1; q++) {
                        if (*q == '<' || *q == '>' || *q == '&' || *q == '"')
                            continue;   /* 丢弃特殊字符 */
                        msgid[n++] = *q;
                    }
                    msgid[n] = '\0';
                }
            }
        }

        char ip[64] = "";
        int prefix = 24;
        if (eth0_addr(ip, sizeof ip, &prefix) != 0)
            snprintf(ip, sizeof ip, "0.0.0.0");

        char resp[2048];
        int rn = snprintf(resp, sizeof resp,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\"\n"
            " xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\"\n"
            " xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\"\n"
            " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">\n"
            "<e:Header>\n"
            "<w:MessageID>uuid:rv1126b-0001</w:MessageID>\n"
            "<w:RelatesTo>%s</w:RelatesTo>\n"
            "<w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>\n"
            "<w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</w:Action>\n"
            "</e:Header>\n"
            "<e:Body>\n"
            "<d:ProbeMatches>\n"
            "<d:ProbeMatch>\n"
            "<w:Address>urn:uuid:rv1126b-0001</w:Address>\n"
            "<d:Types>dn:NetworkVideoTransmitter</d:Types>\n"
            "<d:Scopes>onvif://www.onvif.org/name/RV1126B "
            "onvif://www.onvif.org/hardware/IMX415 "
            "onvif://www.onvif.org/Profile/Streaming</d:Scopes>\n"
            "<d:XAddrs>http://%s/onvif/device_service</d:XAddrs>\n"
            "<d:MetadataVersion>1</d:MetadataVersion>\n"
            "</d:ProbeMatch>\n"
            "</d:ProbeMatches>\n"
            "</e:Body>\n"
            "</e:Envelope>\n",
            msgid, ip);
        sendto(fd, resp, (size_t)rn, 0, (struct sockaddr *)&from, flen);
        printf("[ONVIF] 应答 WS-Discovery Probe (XAddrs http://%s/onvif/device_service)\n", ip);
    }
    return NULL;
}

/* ---------------- 公开接口 ---------------- */

onvif_server_t *onvif_create(int rtsp_port) {
    onvif_server_t *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->rtsp_port = rtsp_port ? rtsp_port : 8554;
    /* 默认单码流 (兼容未调 set_profiles 的场景) */
    o->profiles[0].token = "MainStream";
    o->profiles[0].name = "主码流";
    o->profiles[0].width = 2688;
    o->profiles[0].height = 1520;
    o->profiles[0].path = "/stream";
    o->profile_count = 1;
    snprintf(o->ircut, sizeof o->ircut, "ON");
    return o;
}

void onvif_set_profiles(onvif_server_t *o, const onvif_profile_t *profiles,
                        int count) {
    if (!o || !profiles || count <= 0) return;
    if (count > 8) count = 8;
    for (int i = 0; i < count; i++)
        o->profiles[i] = profiles[i];   /* 借用调用方字符串 (静态区) */
    o->profile_count = count;
}

void onvif_set_jpeg_provider(onvif_server_t *o, jpeg_provider_fn fn,
                             void *ctx) {
    if (!o) return;
    o->jpeg_fn = fn;
    o->jpeg_ctx = ctx;
}

void onvif_set_ptz_callback(onvif_server_t *o, ptz_cb_fn fn, void *ctx) {
    if (!o) return;
    o->ptz_cb = fn;
    o->ptz_ctx = ctx;
}

void onvif_set_ptz_preset_callback(onvif_server_t *o, ptz_preset_cb_fn fn,
                                   void *ctx) {
    if (!o) return;
    o->preset_cb = fn;
    o->preset_ctx = ctx;
}

int onvif_start(onvif_server_t *o) {
    if (!o) return -1;
    if (pthread_create(&o->ws_tid, NULL, wsdisco_thread, o) != 0) {
        perror("[ONVIF] wsdisco pthread_create");
        return -1;
    }
    pthread_detach(o->ws_tid);
    if (pthread_create(&o->http_tid, NULL, http_thread, o) != 0) {
        perror("[ONVIF] http pthread_create");
        return -1;
    }
    pthread_detach(o->http_tid);
    return 0;
}

void onvif_destroy(onvif_server_t *o) {
    free(o);
}
