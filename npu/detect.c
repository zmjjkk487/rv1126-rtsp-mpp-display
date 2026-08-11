/* detect.c — NPU 人形检测实现 (yolov8n, RKNN)
 *
 * 流程: 共享帧缓冲 (detect_feed 写入) → 推理线程取帧 → letterbox
 * NV12→RGB640 → rknn_run → YOLOv8 后处理 → 结果共享 (源帧坐标)
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
#define MAX_SRC_W 1920
#define MAX_SRC_H 1088

/* ---------------- 共享状态 ---------------- */

static rknn_app_context_t g_app;
static pthread_t g_thread;
static int g_run = 0;

static pthread_mutex_t g_feed_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_feed_cv = PTHREAD_COND_INITIALIZER;
static uint8_t *g_frame = NULL;        /* NV12 帧缓冲 */
static int g_frame_w, g_frame_h, g_frame_hs, g_frame_ready;

static pthread_mutex_t g_res_lock = PTHREAD_MUTEX_INITIALIZER;
static det_box_t g_boxes[MAX_BOXES];
static int g_box_count = 0;

/* ---------------- letterbox: NV12 → RGB640 (黑边填充, 最近邻) ---------------- */

static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* fp16 → fp32 (位操作标准转换; 注意不能用 1<<(exp-15) — 负移位是 UB) */
static inline float fp16_to_fp32(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    if (e == 0) {
        if (m == 0) return s ? -0.0f : 0.0f;
        float f = m * 5.96046448e-8f;   /* 次正规 */
        return s ? -f : f;
    }
    if (e == 31) return 65504.0f;        /* inf/nan 就近 */
    uint32_t bits = (s << 31) | ((e + 112) << 23) | (m << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void nv12_letterbox_rgb(const uint8_t *y, const uint8_t *uv,
                               int sw, int sh, int hs,
                               uint8_t *rgb, letterbox_t *lb) {
    float k = fminf((float)MODEL_SIZE / sw, (float)MODEL_SIZE / sh);
    int nw = (int)(sw * k), nh = (int)(sh * k);
    lb->x_pad = (MODEL_SIZE - nw) / 2;
    lb->y_pad = (MODEL_SIZE - nh) / 2;
    lb->scale = (float)MODEL_SIZE / nw;   /* 模型坐标 → 源坐标除数 */
    memset(rgb, 0, MODEL_SIZE * MODEL_SIZE * 3);   /* 黑边 */

    for (int dy = 0; dy < nh; dy++) {
        int sy = dy * sh / nh;             /* 最近邻采样 */
        for (int dx = 0; dx < nw; dx++) {
            int sx = dx * sw / nw;
            int yy = y[sy * hs + sx];
            int up = (sy / 2) * hs + (sx / 2) * 2;
            int u = uv[up] - 128, v = uv[up + 1] - 128;
            int r = yy + (int)(1.402f * v);
            int g = yy - (int)(0.344f * u) - (int)(0.714f * v);
            int b = yy + (int)(1.772f * u);
            int idx = ((dy + lb->y_pad) * MODEL_SIZE + (dx + lb->x_pad)) * 3;
            rgb[idx]     = (uint8_t)clampi(r, 0, 255);
            rgb[idx + 1] = (uint8_t)clampi(g, 0, 255);
            rgb[idx + 2] = (uint8_t)clampi(b, 0, 255);
        }
    }
}

/* ---------------- 推理线程 ---------------- */

static void *detect_thread(void *arg) {
    (void)arg;
    static uint8_t rgb[MODEL_SIZE * MODEL_SIZE * 3];
    rknn_input inputs[1];
    /* 输出数量必须按模型实际 n_output 分配 — post_process 按 3 路
     * 循环访问 (yolov8n 转换后 3 个输出), 只申请 1 个会越界段错误 */
    rknn_output *outputs = (rknn_output *)calloc(g_app.io_num.n_output,
                                                 sizeof(rknn_output));

    memset(inputs, 0, sizeof inputs);
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = MODEL_SIZE * MODEL_SIZE * 3;
    inputs[0].buf = rgb;

    /* want_float=0: 拿原始 fp16 输出手动转换 — want_float=1 在本
     * 环境实测输出异常 (score 全接近 0, 与模拟器不符) */
    for (uint32_t i = 0; i < g_app.io_num.n_output; i++)
        outputs[i].want_float = 0;

    while (g_run) {
        pthread_mutex_lock(&g_feed_lock);
        while (!g_frame_ready && g_run)
            pthread_cond_wait(&g_feed_cv, &g_feed_lock);
        if (!g_run) { pthread_mutex_unlock(&g_feed_lock); break; }
        int w = g_frame_w, h = g_frame_h, hs = g_frame_hs;
        uint8_t *frame = g_frame;   /* 推理期间不允许 feed 覆盖 */
        g_frame_ready = 0;
        pthread_mutex_unlock(&g_feed_lock);

        letterbox_t lb;
        nv12_letterbox_rgb(frame, frame + w * h, w, h, hs, rgb, &lb);
        fprintf(stderr, "[DET] letterbox ok %dx%d\n", w, h);

        if (rknn_run(g_app.rknn_ctx, NULL) != RKNN_SUCC) continue;
        if (rknn_outputs_get(g_app.rknn_ctx, g_app.io_num.n_output,
                             outputs, NULL) != RKNN_SUCC) continue;

        {
            /* fp16 → float 手动转换 (want_float=1 在本环境输出异常):
             * 所有 9 路输出按 attrs dims 的元素数转换, 布局不变 (NCHW) */
            static float fbuf[64 * 80 * 80 + 80 * 80 * 80 + 80 * 80 +
                              64 * 40 * 40 + 80 * 40 * 40 + 40 * 40 +
                              64 * 20 * 20 + 80 * 20 * 20 + 20 * 20];
            size_t off = 0;
            float m0 = 0, m1 = 0;
            fprintf(stderr, "[DET] out[0].size=%d (fp16 应为 %d, fp32 应为 %d)\n",
                    outputs[0].size, (int)(64 * 80 * 80 * 2),
                    (int)(64 * 80 * 80 * 4));
            for (uint32_t i = 0; i < g_app.io_num.n_output; i++) {
                rknn_tensor_attr *a = &g_app.output_attrs[i];
                size_t n = 1;
                for (int d = 0; d < a->n_dims; d++) n *= (size_t)a->dims[d];
                const uint16_t *r = (const uint16_t *)outputs[i].buf;
                float *dst = fbuf + off;
                for (size_t k = 0; k < n; k++)
                    dst[k] = fp16_to_fp32(r[k]);
                outputs[i].buf = dst;
                if (i == 0)
                    for (size_t k = 0; k < n; k++)
                        if (dst[k] > m0) m0 = dst[k];
                if (i == 1)
                    for (size_t k = 0; k < n; k++)
                        if (dst[k] > m1) m1 = dst[k];
                off += n;
            }
            fprintf(stderr, "[DET] raw out0 max=%.3f out1 max=%.3f\n", m0, m1);
        }

        object_detect_result_list od;
        memset(&od, 0, sizeof od);
        post_process(&g_app, outputs, &lb, 0.45f, 0.45f, &od);
        static int dlog;
        if (++dlog % 100 == 1) {
            /* 调试: dump 推理输入帧, 验证喂给模型的图像是否正确 */
            FILE *df = fopen("/tmp/det_input.raw", "w");
            if (df) {
                fwrite(rgb, 1, MODEL_SIZE * MODEL_SIZE * 3, df);
                fclose(df);
            }
        }
        if (++dlog % 10 == 1)
            fprintf(stderr, "[DET] count=%d conf=%.2f box=(%d,%d,%d,%d)\n",
                    od.count, od.count ? od.results[0].prop : 0,
                    od.count ? od.results[0].box.left : 0,
                    od.count ? od.results[0].box.top : 0,
                    od.count ? od.results[0].box.right : 0,
                    od.count ? od.results[0].box.bottom : 0);

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
    free(outputs);
    return NULL;
}

/* ---------------- 对外接口 ---------------- */

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
    fprintf(stderr, "[DET] input type=%d fmt=%d (0=NCHW 1=NHWC), is_quant=%d, "
                    "n_output=%u, out[0] dims=[%d,%d,%d,%d] type=%d fmt=%d\n",
            g_app.input_attrs[0].type, g_app.input_attrs[0].fmt,
            g_app.is_quant,
            g_app.io_num.n_output,
            g_app.output_attrs[0].dims[0], g_app.output_attrs[0].dims[1],
            g_app.output_attrs[0].dims[2], g_app.output_attrs[0].dims[3],
            g_app.output_attrs[0].type, g_app.output_attrs[0].fmt);

    if (init_post_process() != 0) {
        fprintf(stderr, "[detect] 标签加载失败 (忽略, 仍可检测)\n");
    }

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
