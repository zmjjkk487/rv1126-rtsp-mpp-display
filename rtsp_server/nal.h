/* nal.h — H.264 裸流 NAL 单元切分 (Annex-B) */
#ifndef NAL_H
#define NAL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data;   /* 指向原 buffer, 不拷贝 (调用方须保持 buffer 存活) */
    size_t len;            /* NAL 单元长度 (含 1 字节 NAL 头) */
    uint8_t type;          /* NAL 类型: 1=非IDR 5=IDR 6=SEI 7=SPS 8=PPS 9=AUD */
} nal_unit_t;

/* 从 Annex-B 裸流切出全部 NAL 单元
 * 返回 NAL 个数 (≥0), 失败返回 -1 */
int nal_split(const uint8_t *buf, size_t size, nal_unit_t **out, int *count);

const char *nal_type_name(uint8_t type);

#endif /* NAL_H */
