# legacy_web — 废弃的 C 版 Web 后台

> ⚠️ 已废弃, 保留作参考。正式版本为 `src/web_rust/` (Rust + axum)。

## 废弃原因

2026-08-04 用 Rust (axum + tokio) 重写了 Web 后台, 取代此 C 版:

| 对比 | C 版 (此处) | Rust 版 (src/web_rust/) |
|------|------------|------------------------|
| HTTP Server | 手写 socket + select + fork | axum 框架 |
| JSON 解析 | 手写 strstr 切片 | serde |
| Session | 文件目录 /tmp/web_sessions | 内存 HashMap |
| ONVIF XML | 手写字符串解析 | quick-xml + 字符串解析 |
| 编译 | 交叉 gcc | cargo 交叉编译 |
| 大小 | 129K | 3MB |

## 文件

- `web_main.c` — 手写 HTTP Server + 路由 + 认证 (SHA-256 + session)
- `onvif_disco.c/h` — WS-Discovery 组播发现
- `onvif_soap.c/h` — SOAP GetProfiles/GetStreamUri + WS-Security

## 参考价值

- 嵌入式 C 手写 HTTP 的完整示例
- fork-per-connection + FD_CLOEXEC 模式
- 无第三方库的 ONVIF 协议实现
