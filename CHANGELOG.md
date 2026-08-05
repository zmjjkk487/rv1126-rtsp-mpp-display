# 更新日志

## 2026-08-05 (晚)

### 新增: 摄像头生产者 (rtsp_server/) — 板子即摄像头

- **RTSP server** (纯 C, TCP interleaved, 多挂载点)
  - RFC 2326/4566/6184: OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN + SDP + RTP (单NAL/FU-A)
  - 多挂载点: `/stream0` (2K@15) + `/stream1` (1080p@10), 海康风格路径
  - 非阻塞发送 + 慢客户端丢帧; atomic 状态; 入站 RTCP 分路丢弃
  - 审查 3 轮通过 (致命 F1-F5 / 严重 S1-S2 全修复), 本机 gst/VLC 验证
- **采集编码**: IMX415 → ISP 双通道 (mainpath 2K + selfpath 1080p) → mpph264enc
- **ONVIF 应答端** (WS-Discovery + SOAP :80/onvif/device_service)
  - 前缀无关的 Probe 应答 (ODM/微软栈兼容)
  - 13+ 接口: GetDeviceInformation/Profiles/StreamUri/SystemDateAndTime/
    Capabilities/Services/Scopes/DNS/Hostname/NTP/Gateway/VideoSources/
    NetworkInterfaces/Imaging/Subscribe/... 全部严格 XML, 永不返回 Fault
  - GetStreamUri 按 ProfileToken 映射双码流路径
- **产品闭环验证**: web 摄像头管理搜索到板子 → 一键连接 → LCD 显示 IMX415 画面;
  VLC/ffplay/gst 拉流均可

### 修复 (真机验证暴露)
- **mpph264enc 属性名是 `bps` 不是 `bitrate`** — 进程内 gst_parse_launch 只警告不报错,
  CLI 才报错, 导致 feed 静默死亡
- **HTTP 头大小写**: reqwest/hyper 发小写 `content-length:`, ONVIF 解析需大小写不敏感
- **GstAppSinkCallbacks 签名**: new_sample 必须返回 GST_FLOW_OK, 写成 void 导致
  流水线第一个 buffer 后停摆
- **GetStreamUri 需 MediaUri 包裹** + 未知方法回同名空响应元素 — ODM 强解析器
  NRE 的根因
- **gst_bus_add_watch 需要 GLib 主循环** — 用 pause() 时管线错误全静默

### 已知限制 (RV1126 芯片级)
- MPP 多会话调度不保证公平: 双码流多数时间达标, 偶发一路短暂掉帧;
  三码流实测间歇饥饿, 故采用大厂标准的双码流
- IMX415 定焦, 无自动对焦
- ONVIF :80 与 SDK nginx 冲突 (S50nginx 需禁用)
- wlan0 与 eth0 同网段导致组播回包来源漂移 (ODM 搜不到)

## 2026-08-05

### 修复
- **黑屏根因**: 杀 weston 后 CRTC (显示控制器) 未激活, 屏幕无画面
  - 修复: 启动时 `modetest -M rockchip -s 96@73:720x1280` 激活 CRTC
  - 背光 bl_power 一直正常, 之前误判为背光问题
- **开机自启**: S99camera 等待 weston 完全退出后再开背光, 防止退出时覆盖

### 新增
- **Web 修改密码** (`/api/change_password`)
  - 验证旧密码 → 写新 hash → 清空所有 session 强制重新登录
- **摄像头网段设置** (`/api/camera_network`)
  - ONVIF `GetNetworkInterfaces` / `SetNetworkInterfaces`
  - 支持 DHCP / 静态 IP 切换
- **板子网络设置** (`/api/network`)
  - connman 管理 eth0, DHCP / 静态 IP / 子网掩码 / 网关
- **前端界面**: 状态页新增修改密码 + 网络设置卡片

### 预留
- **标准 ONVIF IRCUT** (`/api/ircut`)
  - 动态获取 VideoSourceToken, `t:` 命名空间
  - 等支持 Imaging 服务的标准摄像头到货后可用
