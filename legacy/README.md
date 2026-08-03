# Legacy — 原生 MPP 方案（2024-08 废弃）

这些文件是项目早期探索方案的产物，已不再使用和维护。

## 废弃原因

1. **FFmpeg 拉流性能不足**：实测 RTSP 实时流帧率 ~1fps，远低于 GStreamer rtspsrc
2. **alientek 定制 MPP 兼容问题**：
   - `mpp_dec_*` 函数名与 `librockchip_mpp.so` 符号冲突 → 栈溢出
   - `decode_put_packet` 单包 ~4KB 限制，需要 split_parse + chunk 输入
   - 维护成本高于收益
3. **功能已由正式方案覆盖**：`src/` 路径下的 GStreamer + RGA 混合管线

## 如需恢复

原生 MPP 路径的 MPP decoder wrapper（`mpp_dec.c`）和 FFmpeg demux（`ffmpeg_demux.c`）
可作为 API 使用参考，但正式方案请使用根目录 `main.c`。

```bash
# 编译（不推荐）
./legacy/build_native.sh

# 运行
./output_native/rv1126_rtsp_mpp config.ini
```
