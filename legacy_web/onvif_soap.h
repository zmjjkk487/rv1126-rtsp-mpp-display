/*
 * onvif_soap.h — ONVIF SOAP/HTTP 客户端
 *
 * 通过 SOAP/XML over HTTP 获取摄像头的 RTSP 地址
 * 基于 ONVIF Media Service WSDL
 */
#ifndef ONVIF_SOAP_H
#define ONVIF_SOAP_H

#include <stddef.h>
#include <stdint.h>

/* ONVIF 码流信息 */
typedef struct {
    char token[128];       /* Profile token, 如 "PROFILE_000" */
    char name[256];        /* 人类可读名称, 如 "mainStream" */
    int width, height;     /* 分辨率 */
    char uri[512];         /* RTSP URL */
} onvif_profile_t;

#define ONVIF_MAX_PROFILES 8

/*
 * 获取摄像头所有码流 Profile
 * @xaddrs    设备服务地址 (如 http://192.168.50.168/onvif/device_service)
 * @user/pass 摄像头凭据 (可为空, 401 时用 WS-Security 重试)
 * @profiles  输出: profile 数组
 * @max       数组容量
 * 返回: profile 数量, <0 失败
 */
int onvif_get_profiles(const char *xaddrs, const char *user, const char *pass,
                       onvif_profile_t *profiles, int max);

/*
 * 获取指定 Profile 的 RTSP 地址
 * @xaddrs       设备服务地址
 * @profile_token Profile token (从 get_profiles 获取)
 * @user/pass    凭据
 * @uri          输出: RTSP URL buffer
 * @uri_sz       buffer 大小
 * 返回: 0 成功, <0 失败
 */
int onvif_get_stream_uri(const char *xaddrs, const char *profile_token,
                         const char *user, const char *pass,
                         char *uri, size_t uri_sz);

#endif
