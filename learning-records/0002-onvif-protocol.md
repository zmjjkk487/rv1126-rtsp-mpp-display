# 学习记录 0002: ONVIF 协议原理

## 日期
2026-08-03

## 来源
- 生产项目 /tmp/t113_ref (discoverymanager.cpp, onvifclient.cpp, simplewebserver.cpp)
- ONVIF 官方规范 (Core Spec v2.0, Media Service WSDL)
- WS-Discovery RFC

## 关键认知

### WS-Discovery (UDP 层)
- 组播地址: 239.255.255.250:3702
- 消息格式: SOAP/XML over UDP
- 回复字段: XAddrs = 摄像头服务地址 (http://ip/onvif/device_service)
- 兼容性: 需发送 6 种变体 (2005/2009 WS-Discovery × 3 种 Type)
- 兜底: 单播扫描同网段 IP + 纯 HTTP GET 尝试常见路径

### ONVIF SOAP (HTTP 层)
- Content-Type: application/soap+xml
- 三步取 RTSP URL: GetProfiles → 选主码流 → GetStreamUri(profileToken, "RTSP")
- 响应格式: XML, 用 strstr() 提取 tds:Uri 字段
- 认证: HTTP Basic Auth 或 WS-Security UsernameToken (PasswordDigest)

### 最小实现
- UDP socket → sendto 组播 Probe → recvfrom 解析 XAddrs
- HTTP POST SOAP → GetStreamUri
- 约 500 行 C, 依赖 libcurl (板上已有)
