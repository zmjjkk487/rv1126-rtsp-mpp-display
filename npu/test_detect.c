/* test_detect.c — NPU 检测独立验证程序
 * 用法: ./test_detect <model.rknn> <input_640x640_rgb.raw>
 * 读 RGB 640x640 原始文件 → 推理 → fp16 手动转换 → 后处理 → 打印框
 * (独立于显示端, 验证 fp16 转换修复与后处理链路)
 */
#include "yolov8.h"
#include "rknn_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float fp16_to_fp32(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    if (e == 0) {
        if (m == 0) return s ? -0.0f : 0.0f;
        float f = m * 5.96046448e-8f;
        return s ? -f : f;
    }
    if (e == 31) return 65504.0f;
    uint32_t bits = (s << 31) | ((e + 112) << 23) | (m << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "用法: %s <model.rknn> <input.raw>\n", argv[0]);
        return 1;
    }
    rknn_app_context_t app;
    memset(&app, 0, sizeof app);
    if (rknn_init(&app.rknn_ctx, argv[1], 0, 0, NULL) != RKNN_SUCC) {
        fprintf(stderr, "rknn_init 失败\n");
        return 1;
    }
    rknn_query(app.rknn_ctx, RKNN_QUERY_IN_OUT_NUM, &app.io_num,
               sizeof app.io_num);
    app.input_attrs = (rknn_tensor_attr *)calloc(app.io_num.n_input,
                                                 sizeof(rknn_tensor_attr));
    app.output_attrs = (rknn_tensor_attr *)calloc(app.io_num.n_output,
                                                  sizeof(rknn_tensor_attr));
    for (uint32_t i = 0; i < app.io_num.n_input; i++) {
        app.input_attrs[i].index = i;
        rknn_query(app.rknn_ctx, RKNN_QUERY_INPUT_ATTR, &app.input_attrs[i],
                   sizeof(rknn_tensor_attr));
    }
    for (uint32_t i = 0; i < app.io_num.n_output; i++) {
        app.output_attrs[i].index = i;
        rknn_query(app.rknn_ctx, RKNN_QUERY_OUTPUT_ATTR, &app.output_attrs[i],
                   sizeof(rknn_tensor_attr));
    }
    app.model_channel = app.input_attrs[0].dims[3];
    app.model_width = app.input_attrs[0].dims[2];
    app.model_height = app.input_attrs[0].dims[1];
    app.is_quant = (app.input_attrs[0].type == RKNN_TENSOR_INT8);
    fprintf(stderr, "模型: %dx%d, n_output=%u, is_quant=%d\n",
            app.model_width, app.model_height, app.io_num.n_output,
            app.is_quant);

    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror("打开输入"); return 1; }
    static uint8_t rgb[640 * 640 * 3];
    size_t got = fread(rgb, 1, sizeof rgb, f);
    fclose(f);
    fprintf(stderr, "输入: %zu 字节\n", got);

    rknn_input inputs[1];
    memset(inputs, 0, sizeof inputs);
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = 640 * 640 * 3;
    inputs[0].buf = rgb;
    rknn_inputs_set(app.rknn_ctx, 1, inputs);

    if (rknn_run(app.rknn_ctx, NULL) != RKNN_SUCC) {
        fprintf(stderr, "rknn_run 失败\n");
        return 1;
    }
    rknn_output outputs[16];
    memset(outputs, 0, sizeof outputs);
    for (uint32_t i = 0; i < app.io_num.n_output; i++) {
        outputs[i].index = i;        /* 关键: 必须设置 index! */
        outputs[i].want_float = 1;
    }

    if (rknn_outputs_get(app.rknn_ctx, app.io_num.n_output, outputs, NULL)
        != RKNN_SUCC) {
        fprintf(stderr, "rknn_outputs_get 失败\n");
        return 1;
    }
    for (uint32_t i = 0; i < app.io_num.n_output; i++) {
        size_t n = 1;
        for (int d = 0; d < app.output_attrs[i].n_dims; d++)
            n *= (size_t)app.output_attrs[i].dims[d];
        float *b = (float *)outputs[i].buf;
        float mx = 0;
        for (size_t k = 0; k < n; k++) if (b[k] > mx) mx = b[k];
        fprintf(stderr, "out[%u] max=%.3f\n", i, mx);
    }

    letterbox_t lb = { 0, 0, 1.0f };   /* 输入已是 640x640, 无 pad */
    object_detect_result_list od;
    memset(&od, 0, sizeof od);
    init_post_process();
    post_process(&app, outputs, &lb, 0.45f, 0.45f, &od);
    printf("检测到 %d 个目标:\n", od.count);
    for (int i = 0; i < od.count; i++)
        printf("  cls=%d conf=%.2f box=(%d,%d,%d,%d)\n",
               od.results[i].cls_id, od.results[i].prop,
               od.results[i].box.left, od.results[i].box.top,
               od.results[i].box.right, od.results[i].box.bottom);

    rknn_outputs_release(app.rknn_ctx, app.io_num.n_output, outputs);
    return 0;
}
