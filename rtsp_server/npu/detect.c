/* detect.c — NPU 人形检测 (挂在 producer 采集帧上, 独立于显示端)
 *
 * 架构: producer 每 3 帧喂一帧 NV12 (detect_feed), 检测线程在 NPU
 * 推理 (独立线程不阻塞采集), 结果共享 (detect_get)。
 * 推理/后处理复用官方 yolov8.cc 的流程 (outputs index + want_float)。
 */
#include "detect.h"
#include "yolov8.h"
#include "rknn_api.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODEL_SIZE 640          /* yolov8n 输入 640x640 */
#define MAX_BOXES 32
#define MAX_SRC_W 2688
#define MAX_SRC_H 1520

static rknn_app_context_t g_app;
static pthread_t g_thread;
static int g_run = 0;

static pthread_mutex_t g_feed_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_feed_cv = PTHREAD_COND_INITIALIZER;
static uint8_t *g_frame = NULL;
static int g_frame_w, g_frame_h, g_frame_hs, g_frame_ready;

static pthread_mutex_t g_res_lock = PTHREAD_MUTEX_INITIALIZER;
static det_box_t g_boxes[MAX_BOXES];
static int g_box_count = 0;

/* letterbox: NV12 → RGB640 (最近邻缩放 + 黑边), 与模型预处理一致 */
static void nv12_letterbox_rgb(const uint8_t *y, const uint8_t *uv,
                               int sw, int sh, int hs, uint8_t *rgb) {
    float k = fminf((float)MODEL_SIZE / sw, (float)MODEL_SIZE / sh);
    int nw = (int)(sw * k), nh = (int)(sh * k);
    int xp = (MODEL_SIZE - nw) / 2, yp = (MODEL_SIZE - nh) / 2;
    memset(rgb, 0, MODEL_SIZE * MODEL_SIZE * 3);
    for (int dy = 0; dy < nh; dy++) {
        int sy = dy * sh / nh;
        for (int dx = 0; dx < nw; dx++) {
            int sx = dx * sw / nw;
            int Y = y[sy * hs + sx];
            int U = uv[(sy / 2) * hs + (sx / 2) * 2] - 128;
            int V = uv[(sy / 2) * hs + (sx / 2) * 2 + 1] - 128;
            int R = Y + (int)(1.402f * V);
            int G = Y - (int)(0.344f * U) - (int)(0.714f * V);
            int B = Y + (int)(1.772f * U);
            int idx = ((dy + yp) * MODEL_SIZE + dx + xp) * 3;
            rgb[idx]     = (uint8_t)(R < 0 ? 0 : (R > 255 ? 255 : R));
            rgb[idx + 1] = (uint8_t)(G < 0 ? 0 : (G > 255 ? 255 : G));
            rgb[idx + 2] = (uint8_t)(B < 0 ? 0 : (B > 255 ? 255 : B));
        }
    }
}

static void *detect_thread(void *arg) {
    (void)arg;
    static uint8_t rgb[MODEL_SIZE * MODEL_SIZE * 3];
    rknn_input inputs[1];
    rknn_output outputs[9];

    memset(inputs, 0, sizeof inputs);
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = MODEL_SIZE * MODEL_SIZE * 3;
    inputs[0].buf = rgb;

    /* 必须清零: outputs[9] 是栈数组, buf 字段的垃圾值会误导 SDK
     * (code-review 发现: 未初始化的 buf 让 outputs_get 结果错乱) */
    memset(outputs, 0, sizeof outputs);
    for (uint32_t i = 0; i < g_app.io_num.n_output; i++) {
        outputs[i].index = i;                  /* 必须设置 index */
        outputs[i].want_float = 1;             /* fp 模型直接转 float */
    }

    while (g_run) {
        int w, h, hs;
        uint8_t *frame;
        pthread_mutex_lock(&g_feed_lock);
        while (!g_frame_ready && g_run)
            pthread_cond_wait(&g_feed_cv, &g_feed_lock);
        if (!g_run) { pthread_mutex_unlock(&g_feed_lock); break; }
        w = g_frame_w; h = g_frame_h; hs = g_frame_hs;
        frame = g_frame;
        g_frame_ready = 0;
        /* 锁内完成 letterbox 读取: feed 的 3MB memcpy 不会撕裂本帧 */
        nv12_letterbox_rgb(frame, frame + (size_t)w * h, w, h, hs, rgb);
        pthread_mutex_unlock(&g_feed_lock);

        /* 关键: 每次推理前必须 rknn_inputs_set 把当前帧交给 rknn —
         * 漏掉会推理旧输入 → 恒 0 检测 (踩坑) */
        if (rknn_inputs_set(g_app.rknn_ctx, 1, inputs) != RKNN_SUCC) {
            fprintf(stderr, "[detect] inputs_set 失败\n");
            rknn_outputs_release(g_app.rknn_ctx, g_app.io_num.n_output,
                                 outputs);
            continue;
        }
        if (rknn_run(g_app.rknn_ctx, NULL) != RKNN_SUCC) {
            fprintf(stderr, "[detect] rknn_run 失败\n");
            rknn_outputs_release(g_app.rknn_ctx, g_app.io_num.n_output,
                                 outputs);
            continue;
        }
        if (rknn_outputs_get(g_app.rknn_ctx, g_app.io_num.n_output,
                             outputs, NULL) != RKNN_SUCC) {
            fprintf(stderr, "[detect] outputs_get 失败\n");
            rknn_outputs_release(g_app.rknn_ctx, g_app.io_num.n_output,
                                 outputs);
            continue;
        }

        letterbox_t lb = { 0, 0, 1.0f };
        /* 坐标反映射: post_process 的 box = (model - pad) / scale,
         * scale 必须是源→模型的缩放系数 k (1080p 时 = 1/3) —
         * 注意不是 1/k: 除以 1/k 等于乘 k, 框会缩小 k² 倍 (踩坑) */
        float k = fminf((float)MODEL_SIZE / w, (float)MODEL_SIZE / h);
        int nw = (int)(w * k), nh = (int)(h * k);
        lb.x_pad = (MODEL_SIZE - nw) / 2;
        lb.y_pad = (MODEL_SIZE - nh) / 2;
        lb.scale = (float)nw / w;   /* = k: 模型坐标 → 源帧坐标 */

        object_detect_result_list od;
        memset(&od, 0, sizeof od);
        post_process(&g_app, outputs, &lb, 0.25f, 0.45f, &od);

        pthread_mutex_lock(&g_res_lock);
        g_box_count = 0;
        for (int i = 0; i < od.count && g_box_count < MAX_BOXES; i++) {
            g_boxes[g_box_count].left = od.results[i].box.left;
            g_boxes[g_box_count].top = od.results[i].box.top;
            g_boxes[g_box_count].right = od.results[i].box.right;
            g_boxes[g_box_count].bottom = od.results[i].box.bottom;
            g_boxes[g_box_count].conf = od.results[i].prop;
            g_boxes[g_box_count].cls = od.results[i].cls_id;
            g_box_count++;
        }
        pthread_mutex_unlock(&g_res_lock);

        rknn_outputs_release(g_app.rknn_ctx, g_app.io_num.n_output, outputs);
    }
    return NULL;
}

int detect_init(const char *model_path) {
    memset(&g_app, 0, sizeof g_app);
    if (rknn_init(&g_app.rknn_ctx, (char *)model_path, 0, 0, NULL) != RKNN_SUCC) {
        fprintf(stderr, "[detect] rknn_init 失败: %s\n", model_path);
        return -1;
    }
    if (rknn_query(g_app.rknn_ctx, RKNN_QUERY_IN_OUT_NUM, &g_app.io_num,
                   sizeof g_app.io_num) != RKNN_SUCC) {
        fprintf(stderr, "[detect] 查询 IO 失败\n");
        return -1;
    }
    if (g_app.io_num.n_output > 9) {
        /* 检测线程 outputs[9] 固定数组 + post_process 按 3 路/分支 —
         * 只支持 yolov8n 转换后的 9 路输出, 换模型需同步改 */
        fprintf(stderr, "[detect] 模型输出 %u 路, 超出支持上限 9 (yolov8n)\n",
                g_app.io_num.n_output);
        return -1;
    }
    g_app.input_attrs = (rknn_tensor_attr *)calloc(g_app.io_num.n_input,
                                                   sizeof(rknn_tensor_attr));
    g_app.output_attrs = (rknn_tensor_attr *)calloc(g_app.io_num.n_output,
                                                    sizeof(rknn_tensor_attr));
    for (uint32_t i = 0; i < g_app.io_num.n_input; i++) {
        g_app.input_attrs[i].index = i;
        rknn_query(g_app.rknn_ctx, RKNN_QUERY_INPUT_ATTR,
                   &g_app.input_attrs[i], sizeof(rknn_tensor_attr));
    }
    for (uint32_t i = 0; i < g_app.io_num.n_output; i++) {
        g_app.output_attrs[i].index = i;
        rknn_query(g_app.rknn_ctx, RKNN_QUERY_OUTPUT_ATTR,
                   &g_app.output_attrs[i], sizeof(rknn_tensor_attr));
    }
    g_app.model_channel = g_app.input_attrs[0].dims[3];
    g_app.model_width = g_app.input_attrs[0].dims[2];
    g_app.model_height = g_app.input_attrs[0].dims[1];
    g_app.is_quant = (g_app.input_attrs[0].type == RKNN_TENSOR_INT8);

    if (init_post_process() != 0)
        fprintf(stderr, "[detect] 标签加载失败 (忽略, 仍可检测)\n");

    g_frame = (uint8_t *)malloc(MAX_SRC_W * MAX_SRC_H * 3 / 2);
    if (!g_frame) return -1;
    g_run = 1;
    pthread_create(&g_thread, NULL, detect_thread, NULL);
    fprintf(stderr, "[detect] NPU 就绪 (yolov8n %dx%d)\n",
            g_app.model_width, g_app.model_height);
    return 0;
}

void detect_deinit(void) {
    g_run = 0;
    pthread_cond_broadcast(&g_feed_cv);
    pthread_join(g_thread, NULL);
    free(g_frame);
    g_frame = NULL;
    deinit_post_process();
    if (g_app.rknn_ctx) rknn_destroy(g_app.rknn_ctx);
    free(g_app.input_attrs);
    free(g_app.output_attrs);
}

int detect_feed(const uint8_t *y, const uint8_t *uv, int w, int h, int hs) {
    if (!g_run || !g_frame) return -1;
    if (w > MAX_SRC_W || h > MAX_SRC_H) return -1;
    pthread_mutex_lock(&g_feed_lock);
    size_t ysz = (size_t)w * h;
    memcpy(g_frame, y, ysz);
    memcpy(g_frame + ysz, uv, ysz / 2);
    g_frame_w = w; g_frame_h = h; g_frame_hs = hs;
    g_frame_ready = 1;
    pthread_cond_signal(&g_feed_cv);
    pthread_mutex_unlock(&g_feed_lock);
    return 0;
}

int detect_get(det_box_t *out, int max) {
    pthread_mutex_lock(&g_res_lock);
    int n = g_box_count < max ? g_box_count : max;
    memcpy(out, g_boxes, (size_t)n * sizeof(det_box_t));
    pthread_mutex_unlock(&g_res_lock);
    return n;
}
