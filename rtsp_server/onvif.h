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
/* 帧传递契约: data/cap 由调用方提供, provider 锁内拷贝到此处并写 len/ts;
 * 帧生命周期归调用方 — 消除跨锁共享指针导致的 UAF (扫1 #8) */
typedef struct {
    uint8_t *data;   /* 调用方缓冲 */
    size_t cap;      /* 缓冲容量 */
    size_t len;      /* 实际帧长 (provider 写入) */
    int64_t ts;      /* 编码完成时刻 (g_get_monotonic_time), 延迟测量用 */
} jpeg_frame_t;

typedef int (*jpeg_provider_fn)(void *ctx, jpeg_frame_t *out);   /* 0=有帧 */

void onvif_set_jpeg_provider(onvif_server_t *o, jpeg_provider_fn fn,
                             void *ctx);

/* PTZ 云台控制: 摄像头接受指令 (解析成功) 后立即回调 —
 * 本机无云台时用屏幕标识验证, 真实云台时代替为电机控制 */
typedef enum { PTZ_NONE = 0, PTZ_LEFT, PTZ_RIGHT, PTZ_STOP } ptz_dir_t;
typedef void (*ptz_cb_fn)(ptz_dir_t dir, double speed, void *ctx);
void onvif_set_ptz_callback(onvif_server_t *o, ptz_cb_fn fn, void *ctx);

/* 启动 UDP 3702 + HTTP :80 两个后台线程, 不阻塞 */
int onvif_start(onvif_server_t *o);

void onvif_destroy(onvif_server_t *o);

#endif /* ONVIF_H */
