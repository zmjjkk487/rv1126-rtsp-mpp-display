# 安全扫描报告 — rv1126_rtsp_mpp_demo（三轮交叉验证最终版）

> 依据 Codex security-scan 工作流执行（2026-08-11）：威胁建模 → 发现（**3 轮共 12 个发现子代理**，每轮不同切分角度）→ 对抗性验证（3 验证代理 + 总复核代理）→ 攻击路径/严重度 → 本报告。
> 范围：仓库全部跟踪源文件（C/Rust/部署脚本/git 历史）。第三方库（GStreamer/MPP/rknn SDK）仅在其调用处评估。

## 方法说明（为什么跑三轮）

单轮 LLM 子代理扫描有随机性，存在漏报。三轮采用不同切分：轮 1 按子系统（producer/显示端/web/NPU）、轮 2 按横切视角（协议状态机/跨进程数据流/机械扫描/逻辑深挖）、轮 3 按漏洞面补强（前端 XSS 上下文穷举/文件系统全审计/解析器存疑点复核/全候选总复核）。**效果：轮 1 完全漏掉的高危 XSS 家族（3 个实例）与 /tmp 符号链接提权家族（F2-01 等 6 个实例）由轮 2/3 补出；4 处双独立命中增强置信度。**

## 最终严重度总览（31 项确认 + 1 待板端确认）

### Critical

| # | 标题 | 位置 | 证据 |
|---|---|---|---|
| C-01 | 出厂默认口令 admin123 硬编码 | main.rs:28-29, 1790-1797 | 哈希逐字节验证；所有需登录链的放大器 |
| C-02 | **/tmp 符号链接预置 → root 任意文件覆写 → 提权**（F2-01 家族） | producer.c:111/123、main.rs:617/1149、S99camera、producer_watchdog.sh | 6+ 实例：C fopen 写 /tmp 无 O_NOFOLLOW、Rust fs::write 跟链、shell > / >> 重定向跟链；未鉴权 SetPreset 的 token 可含换行 → 第 2 行可控 → 覆写 S99camera/crontab → root 代码执行；FIFO 变体永久 DoS |

### High

| # | 标题 | 位置 | 证据 |
|---|---|---|---|
| H-01 | 认证后 SSRF + 摄像头凭据泄露 | main.rs:1350-1378, 339-386 | sanitize 只做字符集过滤；Basic Auth base64 明文；默认口令下免鉴权 |
| H-02 | **前端 DOM XSS ×3**（摄像头数据未转义进 JS/HTML 上下文） | index.html:812-813（preset onclick JS 串）、:375（loadSysInfo innerHTML）、:562-564（loadCamNetwork innerHTML） | esc() 上下文误用（JS 字符串需 \x27 转义，实体转义无效）/完全漏转义；恶意设备→管理员会话任意 JS→窃 token |
| H-03 | RTSP fd 耗尽 → accept break → 推流永久死亡 | rtsp_server.c:584-592 | 无会话上限；PLAYING 静默不踢；无鉴权 |
| H-04 | ONVIF HTTP 单线程慢速滴灌独占 | onvif.c:1034-1058 | 逐字节读头 + 每 recv 独立 500ms 超时；body 阶段同样无总时限 |

### Medium

| # | 标题 | 位置 |
|---|---|---|
| M-01 | PTZ 五方法完全无鉴权（真实部署=无鉴权电机控制；预置位表可被污染/删除） | onvif.c:1095-1106, 555-654 |
| M-02 | PTZ 预置位 token 前缀匹配：短 token 覆盖/删除、空 token 删第 0 位、存储型 XML 反射 | onvif.c:589-590, 643-644 |
| M-03 | Basic Auth 只验密码不验用户名 | onvif.c:975-977 |
| M-04 | 登录失败全局计数 → 永久锁死新登录 | main.rs:36, 796-824 |
| M-05 | 明文 HTTP + 24h 长效 token（嗅探接管）；?t= token 仅视频流权 | main.rs:1840, 820 |
| M-06 | /tmp token 0644 + 可覆写 → 窃取/伪造 → 绕过鉴权看流 + symlink 覆写 | main.rs:617, 1149 |
| M-07 | /tmp/detect_cfg FIFO 预置 → 读侧 open 阻塞 → 全码流停摆 | producer.c:48, 193-194 |
| M-08 | passwd 无盐 SHA-256 + 永不 chmod（0644）→ 哈希泄露离线爆破；creds 0644→600 窗口 | main.rs:854, 1791-1795, 692-693 |
| M-09 | 子进程带凭据 URL 进 cmdline 与世界可读日志（/proc 泄露） | main.rs:638-647, 1126-1131, /tmp/*.log |
| M-10 | 重连整条 watch 泄漏 + 双重启竞态 | main.c:61-63, 85-99 |
| M-11 | 日志脱敏绕过（3 路径）+ GStreamer 错误消息整条 URI 带凭据 + stderr 双写 /tmp | main.c:321-345, 93-95, 471 |
| M-12 | 重连后凭据丢失 → 401 无限重连显示永久失效 | main.c:45, 442-468 |
| M-13 | split_url_creds 切片越界 panic（已实测复现） | main.rs:753-770 |
| M-14 | **git 历史含摄像头明文凭据**（初始提交 MD5 密码、8+ 提交明文 admin:123456；config.ini.sample 传播弱凭据） | git 历史 + config.ini.sample |
| M-15 | HTTP 头未 NUL 终止 → ci_find 越界栈读（双独立命中；已本地实证） | onvif.c:1045-1058, 125-136 |
| M-16 | /preview 无鉴权 + 每连接 1MB 无界线程 | onvif.c:1062-1075, 668-717 |
| M-17 | send_rtsp_frame 头/负载分离发送：负载 EAGAIN 后 4 字节头已入 TCP → 客户端交错流永久错位 | rtsp_server.c:149-162 |

### Low

| # | 标题 | 位置 |
|---|---|---|
| L-01 | 登录忽略 user 字段 | main.rs:813 |
| L-02 | session 表永不清理 | main.rs:820 |
| L-03 | WS-Discovery UDP 反射放大（~8 倍）+ msgid CR/LF 未滤 | onvif.c:1144-1209 |
| L-04 | push_src 接线数据竞争（启动丢帧） | producer.c:373, 434 |
| L-05 | onvif_set_network XML 未转义（SOAP 注入发往摄像头） | main.rs:552-578 |
| L-06 | HLS 并发 start 竞态 → 双转码进程 | main.rs:615-650 |
| L-07 | killall -9 互踩 + 孤儿转码进程 + 双看门狗启动竞态 | main.rs:626-629, S99camera |
| L-08 | detect_cfg 幽灵接口（无写入方，无条件信任） | producer.c:46-57 |
| L-09 | hdr_profiles snprintf 链 size_t 下溢栈越界写（潜伏，不可网络触发） | onvif.c:213-236 |
| L-10 | 构建无加固（无 -fstack-protector/-fPIE/-D_FORTIFY_SOURCE） | rtsp_server/Makefile, build.sh |
| L-11 | /tmp 日志读取侧 symlink → 诱导管理员读任意文件（低置信） | main.rs:1548 |
| L-12 | /var/log 追加无 O_NOFOLLOW（条件性，/var/log 可写时） | main.c:471 |

### 待板端确认

| # | 标题 | 位置 |
|---|---|---|
| P-01 | 16bpp 屏幕下 back 缓冲 2 倍堆越界写（CPU+RGA 双路径；仓库证据指向板端 32bpp 不触发） | fbdev.c:46-47, 94-120 — 板端 `cat /sys/class/graphics/fb0/bits_per_pixel` 定案 |

---

## 多轮交叉验证记录（置信度依据）

| 候选 | 独立命中轮次/agent | 结论 |
|---|---|---|
| M-15 HTTP 头未 NUL 终止 | 轮2 协议 agent + 轮2 机械 agent | 双独立命中，实证复现 |
| /tmp token 0644 | 轮2 Web agent（W2-04）+ 轮2 数据流 agent（F2-04）+ 轮3 文件 agent（T3-02） | 三命中 |
| PTZ 未鉴权 | 轮1（P-03）+ 轮2（F2-02） | 双命中 |
| loadCamNetwork XSS | 轮3 前端 agent（X3-02）+ 轮3 总复核 agent（V3-01） | 双命中 |
| push_src 接线竞争 | 轮1 备注 + 轮2（C2-04） | 升级为候选 |
| 预置位前缀匹配 | 轮2（C2-02）+ 轮3（R3-02, V3-05） | 家族确认（Set/Remove/Goto 多实例） |

## Suppressed（11 项，精确反证均经总复核维持）

- P-06 Get* 拓扑泄露：ONVIF 惯例有意设计，响应无凭据数据
- D-01 sw<<16 整数溢出越界读：H.264 管线 max 8192px << 32768 溢出阈值，超大分辨率 MPP 分配即失败
- D-03 frame_count 数据竞争：ARMv7 对齐 int 实际原子，仅计数误差
- D-05 ptz_dir 时间戳溢出：ts 由 producer root 写入（0644 文件），攻击者不可控数值
- N-05 detect_get 负参数：全仓唯一调用点恒传 8
- N-06 deinit/feed 竞态：仅退出路径，且 producer 从不调用 detect_deinit
- N-07 test_detect 栈数组：独立工具，未编入部署
- W2-05/06 等低置信项复核后并入上表（XML 转义、HLS 竞态确认为低危）

## Deferred（5 项）

- N-01 letterbox stride 越界读：当前固定 1080p 管线 stride==width，无网络改 stride 路径
- N-03 模型路数校验：模型固定捆绑 yolov8n（9 路），换模型需同步改
- N-04 错误路径 rknn_outputs_release：NPU 故障路径，防御性
- hdr_profiles snprintf 链：profile_count 不可网络控制（**总复核确认**），但模式不安全 → 已列 L-09
- 16bpp 屏切换：S-15/P-01 板端确认前视为条件性缺陷

## Not applicable

- N-02 独立版 invalid free + 缺 rknn_inputs_set：调试残留未链接进 producer，**建议删除 npu/detect.c**

## 修复优先级（按家族，一行级优先）

1. **认证家族**（阻断一切）：C-01 默认口令强制改密（一行级）→ M-01 PTZ 五方法并入鉴权门（一行方法列表）→ M-04 登录限流按 IP
2. **文件系统家族**（root 提权链）：C-02 全部 /tmp 写改 O_EXCL|O_NOFOLLOW/mkstemp + token/passwd 0600 + 日志脱敏重写（M-11）→ M-07 FIFO 阻塞加 O_NONBLOCK/超时
3. **DoS 家族**：H-03 accept 错误 continue + 会话上限；H-04 HTTP 整请求总时限 + 并发上限
4. **XSS 家族**：H-02 三处统一修复——JS 上下文用 JSON.stringify、innerHTML 统一 esc()、或服务端对摄像头字段格式校验
5. **SSRF 家族**：H-01 ip 校验加私网/环回黑名单 + 凭据不发送至非摄像头 IP
6. **仓库卫生**：M-14 重写 git 历史或接受（学习仓库可留作教学）+ config.ini.sample 改占位符
7. **显示端**：M-10 重连去重/watch 清理、M-12 重连重注入凭据、M-17 send_rtsp_frame 合并发送
8. **加固**：L-10 加编译缓解；M-02 预置位比较改精确匹配

## 方法局限

- 无板端运行时验证（P-01 需板端 bpp 定案；H-03/H-04 的精确 fd 阈值 ~1000 需实测）
- 第三方库内部不在范围；ONVIF 客户端（ODM/平台）对存储型 XML 注入的解析器行为仅代码级评估
- 三轮共 16 个子代理，未做第四轮——收敛信号：轮 3 的新发现均为已知家族的实例/细节（XSS 家族 3 实例收敛、文件家族枚举收敛），无新家族出现
