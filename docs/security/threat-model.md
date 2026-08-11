# 威胁模型 — rv1126_rtsp_mpp_demo

> 依据 Codex security-scan 工作流阶段 1 生成（2026-08-11）。
> 输入：4 个调研 agent 的架构通读报告（文档/生产者/NPU/显示端+Web）。

## 1. 系统与资产

RV1126B IP 摄像头原型，三个常驻进程（均以 **root** 运行，无降权）：

| 进程 | 语言 | 端口/接口 | 输入面 |
|---|---|---|---|
| producer | C | RTSP :8554（TCP）、HTTP :80（ONVIF SOAP + /preview）、UDP :3702（WS-Discovery） | LAN 任意主机 |
| rtsp_display | C | 无监听，**出站** RTSP 拉流 | 外部摄像头（网络不可信） |
| rv1126_web | Rust(axum) | :8090（除 /preview、/hls/*、/ 与静态外需 Bearer token） | 浏览器/LAN |

**资产**：root 文件系统（进程可写任意文件）、摄像头视频流、Web 凭据 `/root/camera-web/passwd`（admin:sha256）、RTSP 凭据 `/root/camera-web/creds`（明文 user:pass）、网络配置（connmanctl 可改 eth0 网络）、屏幕显示。

## 2. 信任边界

- **LAN 边界（主要）**：所有监听端口对局域网开放，无认证/加密（RTSP、ONVIF HTTP、WS-Discovery 均明文）。假定攻击者 = 同网段任意主机。
- **Web 认证边界**：:8090 大部分 API 需 Bearer token（内存 HashMap，24h）。/preview、/hls/*、静态文件例外。
- **出站边界**：rtsp_display 拉取用户配置的 RTSP URL —— URL 由 Web 写入 config.ini，攻击者若控制 web 或 config.ini 可注入恶意服务器 → 不可信流进入 MPP 解码器/GStreamer。
- **文件边界**：/tmp/detect_cfg、/tmp/ptz_dir 由 producer 读取，Web 侧写入；config.ini/creds 由显示端读取。任何进程以 root 写这些文件即影响其他进程。

## 3. 入口点清单

| # | 入口 | 位置 | 数据源 | 说明 |
|---|---|---|---|---|
| E1 | RTSP 会话 | rtsp_server.c 监听 :8554 | LAN | OPTIONS/DESCRIBE/SETUP/PLAY 手写解析 |
| E2 | ONVIF SOAP | onvif.c HTTP :80/onvif/device_service | LAN | 手写 XML 解析，13+ 方法，Set* 需 Basic Auth |
| E3 | /preview MJPEG | onvif.c | LAN | multipart 长连接，1MB 缓冲 |
| E4 | WS-Discovery | onvif.c UDP :3702 | LAN 组播 | Probe 应答 |
| E5 | Web API | web_rust main.rs :8090 | 浏览器（token） | login/connect/scan/hls/preview/ptz/network/logs 等 |
| E6 | Web 静态/预览 | web_rust | LAN | / 与静态无 token；web 侧 /preview 与 /hls/* 需 ?t= token（onvif.c :80 的 /preview 无 token，见 E3） |
| E7 | RTSP 出站拉流 | main.c rtspsrc | 外部摄像头 | 不可信 H.264/RTP |
| E8 | 文件输入 | config.ini/creds/last_connect.json//tmp/* | Web 写入 | root 文件写入即代码路径输入 |
| E9 | NPU 模型 | detect_init(/root/*.rknn) | 部署文件 | 解析模型格式 |

## 4. 高影响家族映射（发现阶段锚点）

### C 侧（producer / 显示端 / ONVIF）
- **F1 命令注入**：onvif.c 的 SetNetworkInterfaces/网关查询用 popen/system 执行 shell（sanitize_ipv4 过滤）；producer 无其它 shell。web_rust 有大量 Command 调用（见 F8）。
- **F2 协议解析内存安全**：RTSP 行解析（rtsp_server.c，8KB body 上限）、NAL 分包（nal.c）、RTP 头、ONVIF XML 手写解析（长度/嵌套/转义）、HTTP 头解析 —— C 手写解析器，边界错误 = 栈溢出/崩溃（DoS）。播放状态机：未 SETUP 直接 PLAY 回 455 ✓。
- **F3 鉴权缺口**：ONVIF Set* 只验密码不验用户名（sha256 比 passwd 文件）；GET 方法全部无鉴权（GetImagingSettings 等信息泄露）；/preview 无 token。Web 登录忽略 user 字段。
- **F4 文件影响**：/tmp/ptz_dir、/tmp/detect_cfg 由 web 写入、producer 无验证读取；onvif 的 GetSnapshotUri 指向未实现的 /snapshot.jpg。
- **F5 竞态/内存安全**：detect.c 锁、g_jpeg_data 共享、双进程读写 /tmp。
- **F6 出站 SSRF/流注入**：rtsp_display 拉流 URL 来自 config.ini（web 可控）→ 不可信流进 MPP 解码；重连逻辑解析 URL。

### Rust 侧（web_rust）
- **F7 认证/token**：Bearer token 内存存储；/api/logs 需 token；登录限流全局计数；默认密码常量。
- **F8 命令注入**：connect/hls_start/preview_start 等将 rtsp_url/摄像头 IP 拼接进 Command（killall、modetest、gst-launch、nohup）—— 参数过滤是否完整？
- **F9 路径遍历**：/api/logs?file= 读任意文件？静态文件路由的目录穿越防护？
- **F10 SSRF**：scan 的 onvif 探测 ip 参数（sanitize 语义错位复用 sanitize_ntp_server）；connect 的凭据存储。
- **F11 数据暴露**：creds 明文存储 chmod 600；passwd 明文可读路径；日志含 URL 凭据（log_redacted_url 已脱敏显示端，web 侧?）。

## 5. 范围与声明

- 覆盖：所有跟踪源文件（C/Rust），含部署脚本（S99camera、producer_watchdog.sh）。
- 不覆盖：GStreamer/MPP/rknn SDK 第三方库内部（视为依赖边界，仅在其调用处评估）。
- 部署假设：默认配置 admin/admin123 可改密；板子仅局域网部署；无防火墙假设（同网段攻击者视为可达）。
