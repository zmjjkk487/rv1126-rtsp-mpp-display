# 更新日志

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
