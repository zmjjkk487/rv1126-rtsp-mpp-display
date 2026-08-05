/* nal.c — H.264 Annex-B 起始码扫描
 *
 * 裸流结构: [起始码][NAL头 1字节][负载] [起始码][NAL头][负载] ...
 *   起始码 = 00 00 01 (流开头常用 00 00 00 01, 多一个前导 0)
 *   NAL头  = 1 字节: [F 1位][nal_ref_idc 2位][nal_unit_type 5位]
 *
 * 契约: nals[n].data 指向 NAL 头 (含 1 字节头), len 含头。
 *       data 是原 buffer 的借用, 调用方须保持 buffer 存活。
 */
#include "nal.h"

#include <stdlib.h>

const char *nal_type_name(uint8_t type) {
    switch (type) {
        case 1:  return "非IDR切片";
        case 5:  return "IDR切片";
        case 6:  return "SEI";
        case 7:  return "SPS";
        case 8:  return "PPS";
        case 9:  return "AUD";
        default: return "其他";
    }
}

/* 尾部追加一个 NAL 单元, 必要时扩容; 失败返回 -1 */
static int nal_push(nal_unit_t **nals, size_t *cap, int *n,
                    const uint8_t *data, size_t len, uint8_t type) {
    if (*n == (int)*cap) {
        size_t nc = *cap * 2;
        nal_unit_t *tmp = realloc(*nals, sizeof(nal_unit_t) * nc);
        if (!tmp) return -1;
        *nals = tmp;
        *cap = nc;
    }
    (*nals)[*n].data = data;
    (*nals)[*n].len  = len;
    (*nals)[*n].type = type;
    (*n)++;
    return 0;
}

int nal_split(const uint8_t *buf, size_t size, nal_unit_t **out, int *count) {
    size_t cap = 64;
    nal_unit_t *nals = malloc(sizeof(nal_unit_t) * cap);
    if (!nals)
        return -1;

    int n = 0;
    int have = 0;              /* 是否已开启一个 NAL */
    size_t nal_start = 0;      /* 当前 NAL 的 NAL 头位置 */
    uint8_t nal_type = 0;

    /* 找起始码 00 00 01; 扫到就把上一个 NAL 结算掉, 开启下一个 */
    for (size_t i = 0; i + 3 < size; i++) {
        if (buf[i] != 0 || buf[i+1] != 0 || buf[i+2] != 1)
            continue;

        if (have) {
            size_t len = i - nal_start;      /* 上一条 NAL: [nal_start, i) */
            if (len > 0 &&                   /* 空 NAL 不产出 */
                nal_push(&nals, &cap, &n, buf + nal_start, len, nal_type) < 0) {
                free(nals);
                return -1;
            }
        }

        nal_type  = buf[i+3] & 0x1F;   /* NAL 头就在起始码后 1 字节 */
        nal_start = i + 3;             /* data 从 NAL 头开始 (含 1 字节头) */
        have = 1;
        i += 3;                        /* 跳过已消费的起始码 */
    }

    if (have) {                        /* 最后一段没有后续起始码 */
        size_t len = size - nal_start;
        if (len > 0 &&
            nal_push(&nals, &cap, &n, buf + nal_start, len, nal_type) < 0) {
            free(nals);
            return -1;
        }
    }

    *out = nals;
    *count = n;
    return n;
}
