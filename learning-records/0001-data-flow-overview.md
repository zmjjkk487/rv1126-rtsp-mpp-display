# 学习记录 0001: 数据流全景理解

## 日期
2026-08-03

## 背景
完成 RV1126 GStreamer+RGA 混合管线调试后，系统梳理数据在管线中的流转过程。

## 关键认知
1. 数据从摄像头 16KB/帧(压缩) 膨胀到 3.7MB/帧(原始) → **230× 膨胀**，这是必须硬件加速的根本原因
2. NV12 格式: Y 全分辨率 + UV 半分辨率，布局由 hor_stride × ver_stride 决定
3. RGA 的 wstride 必须匹配目标缓冲区全宽 (f-&gt;w)，不能用缩放宽 (vw)，否则斜切
4. MPP 硬件的 stride 对齐规则: ver_stride = ALIGN(height, 16), 所以 1080→1088
5. 项目中数据经过三次"搬家": dmabuf (零拷贝) → back buffer (DDR) → 显存 (mmap)

## 仍不清楚的问题
- NV12 的 UV 交错 (interleaved) 具体排列: U0V0 U1V1 还是 V0U0 V1U1？
- MPP 内部解码 pipeline 的工作流程
- RGA 硬件内部实现原理
