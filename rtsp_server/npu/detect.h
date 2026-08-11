/* detect.h — NPU 人形检测模块 (RV1126B, yolov8n)
 *
 * 架构: 显示端每 N 帧喂一帧 NV12 (detect_feed), 检测线程在 NPU 上
 * 推理 (独立线程, 不阻塞解码), 结果以源帧坐标系共享 (detect_get)。
 * 依赖: librknnrt.so (板端 /usr/lib) + yolov8n_rv1126b.rknn 模型
 */
#ifndef DETECT_H
#define DETECT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int left, top, right, bottom;   /* 源帧坐标系 */
    float conf;
    int cls;
} det_box_t;

/* 初始化: 加载 RKNN 模型 + 启动推理线程; 返回 0 成功 */
int detect_init(const char *model_path);
void detect_deinit(void);

/* 喂一帧 NV12 (解码回调调用, 建议每 3-5 帧一次):
 * 拷贝到共享缓冲并唤醒推理线程 (返回 0 成功) */
int detect_feed(const uint8_t *y, const uint8_t *uv, int w, int h, int hs);

/* 取最新检测结果 (线程安全), 返回框数量; 无检测返回 0 */
int detect_get(det_box_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DETECT_H */
