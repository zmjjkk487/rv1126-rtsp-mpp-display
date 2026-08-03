/*
 * onvif_disco.h — ONVIF WS-Discovery 设备发现
 *
 * 发 UDP 组播 Probe → 收 ProbeMatch → 解析 XAddrs
 * 基于 OASIS WS-Discovery 1.1 规范
 */
#ifndef ONVIF_DISCO_H
#define ONVIF_DISCO_H

#include <stdint.h>

#define ONVIF_MULTICAST_ADDR "239.255.255.250"
#define ONVIF_DISCO_PORT      3702
#define ONVIF_PROBE_TIMEOUT_MS 4000
#define ONVIF_MAX_DEVICES     32

/* 发现的设备信息 */
typedef struct {
    char xaddrs[512];   /* 服务地址, 如 http://192.168.50.168/onvif/device_service */
    char ip[64];        /* 解析出的 IP */
    char scopes[512];   /* ONVIF scope (含设备型号/名称) */
    char types[256];    /* 设备类型 */
} onvif_device_t;

/*
 * 发起 ONVIF WS-Discovery 探测
 * @iface      网络接口名 ("eth0", "wlan0" 等, NULL=自动)
 * @timeout_ms 最长等待时间(毫秒)
 * @devices    输出: 发现的设备数组
 * @max        数组容量
 * 返回: 实际发现的设备数, <0 失败
 */
int onvif_discover(const char *iface, int timeout_ms,
                   onvif_device_t *devices, int max);

#endif
