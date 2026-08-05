/* nal_parse.c — H.264 裸流 NAL 解析器（RTSP server 第一块地基）
 *
 * 裸流结构:  [起始码][NAL头][负载][起始码][NAL头][负载] ...
 *   起始码 = 00 00 01 (3字节, 流开头常用 00 00 00 01 4字节版)
 *   NAL头  = 1 字节: [禁止位1][nal_ref_idc 2位][nal_unit_type 5位]
 *
 * 用法: ./nal_parse /tmp/test.h264
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static const char *nal_name(uint8_t type) {
    switch (type) {
        case 1:  return "非IDR切片(普通帧)";
        case 5:  return "IDR切片(关键帧)";
        case 6:  return "SEI";
        case 7:  return "SPS";
        case 8:  return "PPS";
        case 9:  return "AUD";
        default: return "其他";
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "用法: %s <file.h264>\n", argv[0]);
        return 1;
    }

    /* ① 读文件到堆: 内存所有权三步曲 = 问大小 → malloc → fread, 用完 free */
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror("fopen"); return 1; }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);          /* 文件有多少字节 */
    fseek(fp, 0, SEEK_SET);

    uint8_t *buf = malloc(size);    /* 在堆上要一块 size 字节的内存 */
    if (!buf) { perror("malloc"); return 1; }
    fread(buf, 1, size, fp);
    fclose(fp);

    /* ② 从头到尾扫 buffer, 找起始码 00 00 01 */
    long nal_start = -1;            /* 当前 NAL 负载在 buf 中的偏移 */
    uint8_t nal_type = 0;
    int count = 0, sps = 0, pps = 0, idr = 0;

    for (long i = 0; i + 4 < size; i++) {
        /* 找 00 00 01: 三个字节连起来等于 00 00 01 */
        if (buf[i] != 0 || buf[i+1] != 0 || buf[i+2] != 1)
            continue;

        /* 找到起始码了! 若前面还有一个没结算的 NAL, 先结算它:
         * 它的负载从 nal_start 到 i-1, 长度 = i - nal_start */
        if (nal_start >= 0) {
            if (nal_type == 7) sps++;
            if (nal_type == 8) pps++;
            if (nal_type == 5) idr++;
            printf("[%4d] 类型=%d (%s) 长度=%6ld 负载偏移=%ld\n",
                   ++count, nal_type, nal_name(nal_type),
                   i - nal_start, nal_start);
        }

        /* 新 NAL 的头就在起始码后面 1 字节 (i+3)
         * 低 5 位 = NAL 类型: 0x67 & 0x1F = 0x07 = SPS */
        nal_type = buf[i+3] & 0x1F;
        nal_start = i + 4;          /* 负载从 i+4 开始 */
        i = i + 3;                  /* 跳过已消费的起始码 */
    }

    /* ③ 最后一段 NAL 后面没有起始码了, 单独结算一次 */
    if (nal_start >= 0) {
        if (nal_type == 7) sps++;
        if (nal_type == 8) pps++;
        if (nal_type == 5) idr++;
        printf("[%4d] 类型=%d (%s) 长度=%6ld 负载偏移=%ld\n",
               ++count, nal_type, nal_name(nal_type),
               size - nal_start, nal_start);
    }

    printf("---\nNAL 总数=%d, SPS=%d, PPS=%d, IDR(关键帧)=%d\n",
           count, sps, pps, idr);

    free(buf);                      /* 借的堆内存要还 */
    return 0;
}
