/* onvif.h — 极简 ONVIF 应答端 (让板子对自家 web 平台呈现为标准摄像头)
 *
 * 实现规格取自 web_rust 的发现/连接流程:
 *   1. WS-Discovery Probe (UDP 3702 组播) → ProbeMatch, 含 <d:XAddrs>
 *   2. HTTP POST /onvif/device_service → GetDeviceInformation /
 *      GetProfiles / GetStreamUri / GetImagingSettings / SetImagingSettings /
 *      GetNetworkInterfaces / SetNetworkInterfaces
 *   3. 任何请求都返回 200 + 正常 SOAP 应答, 永不返回 Fault
 *      (web 端 has_fault 检查通过后第一级 WSS 认证即视为成功, 认证被忽略)
 */
#ifndef ONVIF_H
#define ONVIF_H

#include <stddef.h>
#include <stdint.h>

typedef struct onvif_server onvif_server_t;

/* 一个码流 profile (GetProfiles / GetStreamUri 用) */
typedef struct {
    const char *token;      /* ProfileToken, 如 "MainStream" */
    const char *name;       /* 显示名 */
    int width, height;      /* 分辨率 */
    const char *path;       /* RTSP 路径, 如 "/stream" */
} onvif_profile_t;

/* rtsp_port: RTSP server 端口 (GetStreamUri 返回的 URL 用) */
onvif_server_t *onvif_create(int rtsp_port);

/* 设置码流列表 (GetProfiles 逐个返回, GetStreamUri 按 token 映射路径);
 * 默认 1 个: MainStream 2688x1520 /stream */
void onvif_set_profiles(onvif_server_t *o, const onvif_profile_t *profiles,
                        int count);

/* MJPEG 低延迟预览: 帧提供者 (producer 提供最新 JPEG 帧),
 * HTTP GET /preview 走 multipart/x-mixed-replace 推流, 延迟 100-300ms */
typedef struct {
    const uint8_t *data;
    size_t len;
    int64_t ts;   /* 编码完成时刻 (g_get_monotonic_time), 延迟测量用 */
} jpeg_frame_t;

typedef int (*jpeg_provider_fn)(void *ctx, jpeg_frame_t *out);   /* 0=有帧 */

void onvif_set_jpeg_provider(onvif_server_t *o, jpeg_provider_fn fn,
                             void *ctx);

/* 启动 UDP 3702 + HTTP :80 两个后台线程, 不阻塞 */
int onvif_start(onvif_server_t *o);

void onvif_destroy(onvif_server_t *o);

#endif /* ONVIF_H */
