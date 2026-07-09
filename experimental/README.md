# experimental/

本目录存放未完成或待验证的实验模块。

## drm_display.c / drm_display.h

**状态：未完成，暂不可用。**

DRM/KMS 显示实现，当前用 dumb buffer + CPU 做 NV12→RGB 转换。

### 已知问题

1. **RGA 未集成** — NV12→RGB 目前走 CPU 路径，性能差
2. **kmssink connector-id 兼容问题** — 正点原子 Buildroot 的 kmssink 插件与当前 DRM connector 不兼容
3. **板端验证未通过** — 当前未在实际设备上跑通

### 保留原因

未来产品化可能需要：
- DRM plane 硬件叠加（OSD、时间戳、检测框）
- fbdev 是遗留接口，DRM/KMS 是 Linux 显示标准方向

如需启用，优先将 RGA 结果通过 DRM prime buffer 配合 plane 使用，避免 CPU 全帧拷贝。
