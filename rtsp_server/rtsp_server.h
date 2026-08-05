/* rtsp_server.h — 极简 RTSP 服务器 (H.264, TCP interleaved, 多挂载点)
 *
 * 用法: 创建 → add_mount 注册码流路径 → start(阻塞 accept) →
 *       对每个挂载点反复 feed_nal(mount, ...) 喂流
 * 客户端: PLAY 后该挂载点的 NAL 会被分包发给所有播放中的会话
 */
#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <stddef.h>
#include <stdint.h>

typedef struct rtsp_server rtsp_server_t;
typedef struct rtsp_mount rtsp_mount_t;

/* 创建服务器, port=0 用默认 8554 */
rtsp_server_t *rtsp_server_create(int port);

/* 注册一个码流挂载点, 如 "/stream"、"/stream1"; 返回挂载句柄 */
rtsp_mount_t *rtsp_server_add_mount(rtsp_server_t *srv, const char *path);

/* 设置该挂载点的 RTP 时间戳每帧步进, 默认 3600 (= 90000/25, 25fps);
 * 30fps 应传 3000。feed 每收到一帧切片步进一次 */
void rtsp_server_set_frame_step(rtsp_mount_t *mount, uint32_t step);

/* 阻塞: 监听 + accept 循环 (每个客户端一个线程) */
int rtsp_server_start(rtsp_server_t *srv);

/* 向指定挂载点喂一个 NAL 单元 (含 1 字节 NAL 头),
 * 分发给该挂载点上所有 PLAYING 客户端 */
void rtsp_server_feed_nal(rtsp_mount_t *mount, const uint8_t *nal, size_t len);

void rtsp_server_destroy(rtsp_server_t *srv);

#endif /* RTSP_SERVER_H */
