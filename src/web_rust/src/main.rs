// rv1126_web — 嵌入式 Web 管理后台 (Rust 重构)
// 端口 :8090 | ONVIF 发现 | RTSP 连接 | HLS 预览 | 登录认证

use axum::{
    extract::{Path, State},
    http::{header, StatusCode},
    response::{IntoResponse, Json, Response},
    routing::{get, post},
    Router,
};
use base64::Engine;
use rand::Rng;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::io::Read;
use std::sync::Arc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tokio::fs;
use tokio::net::UdpSocket;
use tokio::process::Command;
use tokio::sync::Mutex;

const STATIC_DIR: &str = "/root/camera-web/static";
const PASSWD_FILE: &str = "/root/camera-web/passwd";
const CREDS_FILE: &str = "/root/camera-web/creds";
const DEVICES_FILE: &str = "/root/camera-web/devices.json";
const DEFAULT_HASH: &str =
    "240be518fabd2724ddb6f04eeb1da5967448d7e831c08c8fa822809f74c720a9";

// ─── App State ─────────────────────────────────────────────

struct AppState {
    pw_hash: Mutex<String>,
    sessions: Mutex<HashMap<String, u64>>,   // token → expiry timestamp
    login_fails: Mutex<Vec<u64>>,
    devices: Mutex<Vec<DeviceEntry>>,        // 已保存摄像头列表
}

#[derive(Clone, serde::Serialize, serde::Deserialize)]
struct DeviceEntry {
    id: String,
    name: String,
    ip: String,
    rtsp_url: String,
    user: String,
    pass: String,
    added_at: String,
}

#[derive(Clone, serde::Serialize)]
struct DeviceStatus {
    #[serde(flatten)]
    device: DeviceEntry,
    online: bool,
}

// ─── JSON Types ─────────────────────────────────────────────

#[derive(Deserialize)]
struct LoginReq {
    user: Option<String>,
    pass: Option<String>,
}

#[derive(Serialize)]
struct LoginResp {
    status: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    token: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<String>,
}

#[derive(Deserialize)]
struct ConnectReq {
    url: Option<String>,
    #[serde(default)]
    user: Option<String>,
    #[serde(default)]
    pass: Option<String>,
}

#[derive(Serialize)]
struct ConnectResp {
    status: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pid: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    rtsp: Option<String>,
}

#[derive(Deserialize)]
struct HlsReq {
    url: Option<String>,
}

#[derive(Serialize)]
struct HlsStartResp {
    status: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    hls_token: Option<String>,
}

#[derive(Serialize)]
struct DeviceInfo {
    ip: String,
    xaddrs: String,
    name: String,
    profiles: Vec<ProfileInfo>,
}

#[derive(Serialize)]
struct ProfileInfo {
    token: String,
    name: String,
    width: u32,
    height: u32,
    uri: String,
}

#[derive(Serialize)]
struct ScanResp {
    devices: Vec<DeviceInfo>,
    count: usize,
}

// ─── Auth helpers ───────────────────────────────────────────

fn sha256_hex(s: &str) -> String {
    let hash = Sha256::digest(s.as_bytes());
    hex::encode(hash)
}

fn gen_token() -> String {
    let mut rng = rand::thread_rng();
    let bytes: [u8; 16] = rng.gen();
    hex::encode(bytes)
}

fn now_secs() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

fn parse_bearer(req: &axum::http::Request<axum::body::Body>) -> Option<String> {
    req.headers()
        .get(header::AUTHORIZATION)?
        .to_str()
        .ok()
        .and_then(|v| v.strip_prefix("Bearer ").map(|t| t.to_string()))
}

// ─── ONVIF WS-Discovery ─────────────────────────────────────

const WS_PROBE: &str = r#"<?xml version="1.0" encoding="utf-8"?>
<e:Envelope xmlns:e="http://www.w3.org/2003/05/soap-envelope"
            xmlns:w="http://schemas.xmlsoap.org/ws/2004/08/addressing"
            xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery"
            xmlns:dn="http://www.onvif.org/ver10/network/wsdl">
<e:Header>
<w:MessageID>uuid:rv1126-probe</w:MessageID>
<w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>
<w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>
</e:Header>
<e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></e:Body>
</e:Envelope>"#;

async fn onvif_discover() -> Vec<(String, String)> {
    let mut devices = Vec::new();
    let socket = match UdpSocket::bind("0.0.0.0:0").await {
        Ok(s) => s,
        Err(_) => return devices,
    };
    let _ = socket.set_broadcast(true);
    let _ = socket.join_multicast_v4(
        "239.255.255.250".parse().unwrap(),
        std::net::Ipv4Addr::UNSPECIFIED,
    );

    // Send probe 3× (间隔 200ms)
    for _ in 0..3 {
        let _ = socket
            .send_to(WS_PROBE.as_bytes(), "239.255.255.250:3702")
            .await;
        tokio::time::sleep(Duration::from_millis(200)).await;
    }

    // Collect responses (timeout 2s)
    let mut buf = [0u8; 4096];
    let deadline = tokio::time::Instant::now() + Duration::from_secs(2);
    while tokio::time::Instant::now() < deadline {
        match tokio::time::timeout(Duration::from_millis(500), socket.recv_from(&mut buf)).await {
            Ok(Ok((n, _src))) => {
                let xml = String::from_utf8_lossy(&buf[..n]);
                // Extract XAddrs
                for cap in xml.match_indices("<d:XAddrs>") {
                    let start = cap.0 + 10;
                    if let Some(end) = xml[start..].find("</d:XAddrs>") {
                        let addrs = &xml[start..start + end];
                        // XAddrs 可能包含多个地址 (空格分隔, 如 http + https)
                        // 取第一个, 提取纯 IP
                        let first = addrs.split_whitespace().next().unwrap_or("");
                        let ip = first
                            .split("://")
                            .nth(1)
                            .and_then(|s| s.split('/').next())
                            .and_then(|s| s.split(':').next())
                            .unwrap_or("")
                            .to_string();
                        if !devices.iter().any(|(_, a)| a == first) {
                            devices.push((ip, first.to_string()));
                        }
                    }
                }
                // Extract Scopes for name
            }
            _ => break,
        }
    }

    // 补全: 无论组播是否有结果, 都探测常见 IP (海康/大华不响应组播)
    // 注意: 必须发 SOAP POST 探测, GET 会返回 HTML 欢迎页 (不是 ONVIF 设备)
    // 并发探测: 10 个 IP 同时发请求, 总耗时 ≈ 单个超时 (2s) 而非 10×2s
    const SOAP_PROBE: &str = r#"<?xml version="1.0"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope">
<s:Body xmlns:tds="http://www.onvif.org/ver10/device/wsdl">
<tds:GetDeviceInformation/>
</s:Body>
</s:Envelope>"#;
    let ips: [i32; 10] = [54, 64, 168, 10, 100, 101, 200, 150, 1, 66];
    let mut probes = Vec::new();
    for &last in &ips {
        let url = format!("http://192.168.50.{}/onvif/device_service", last);
        probes.push(async move {
            if let Ok(resp) = reqwest::Client::new()
                .post(&url)
                .header("Content-Type", "application/soap+xml; charset=utf-8")
                .body(SOAP_PROBE)
                .timeout(Duration::from_millis(800))  // 0.8s 超时, 够局域网内响应
                .send()
                .await
            {
                let body = resp.text().await.unwrap_or_default();
                // 必须是 SOAP Envelope 响应才是 ONVIF 设备
                if body.contains("Envelope") && body.contains("GetDeviceInformationResponse") {
                    return Some((format!("192.168.50.{}", last), url));
                }
            }
            None
        });
    }
    // 并发执行所有探测
    let results = futures::future::join_all(probes).await;
    for r in results.into_iter().flatten() {
        if !devices.iter().any(|(_, a)| a == &r.1) {
            devices.push(r);
        }
    }

    devices
}

// ─── ONVIF SOAP ─────────────────────────────────────────────

fn wsse_password_digest(nonce: &[u8], created: &str, password: &str) -> String {
    let mut hasher = sha1::Sha1::new();
    sha1::Digest::update(&mut hasher, nonce);
    sha1::Digest::update(&mut hasher, created.as_bytes());
    sha1::Digest::update(&mut hasher, password.as_bytes());
    base64::engine::general_purpose::STANDARD.encode(sha1::Digest::finalize(hasher))
}

fn make_nonce() -> Vec<u8> {
    rand::random::<[u8; 16]>().to_vec()
}

/// 构造带 WS-Security 的 ONVIF SOAP 请求体
fn make_soap_request(user: &str, pass: &str, body: &str) -> String {
    let nonce = make_nonce();
    let created = chrono::Utc::now().format("%Y-%m-%dT%H:%M:%SZ").to_string();
    let digest = wsse_password_digest(&nonce, &created, pass);
    let nonce_b64 = base64::engine::general_purpose::STANDARD.encode(&nonce);

    format!(
        r#"<?xml version="1.0" encoding="utf-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
            xmlns:trt="http://www.onvif.org/ver10/media/wsdl"
            xmlns:tds="http://www.onvif.org/ver10/device/wsdl"
            xmlns:t="http://www.onvif.org/ver10/schema"
            xmlns:tt="http://www.onvif.org/ver10/schema">
<s:Header>
<Security s:mustUnderstand="1" xmlns="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd">
<UsernameToken>
<Username>{}</Username>
<Password Type="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest">{}</Password>
<Nonce EncodingType="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary">{}</Nonce>
<Created xmlns="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd">{}</Created>
</UsernameToken>
</Security>
</s:Header>
<s:Body>{}</s:Body>
</s:Envelope>"#,
        user, digest, nonce_b64, created, body
    )
}

/// 发送 ONVIF SOAP 请求, 返回响应文本
/// 认证策略: 先 WS-Security (WSS), 失败/401 回退 HTTP Basic Auth
/// (海康等品牌对 WSS 支持不稳定, Basic Auth 更可靠)
/// 构造无认证的裸 SOAP 请求 (带 trt/tds/t 命名空间)
fn make_plain_soap(body_xml: &str) -> String {
    format!(
        r#"<?xml version="1.0" encoding="utf-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
            xmlns:trt="http://www.onvif.org/ver10/media/wsdl"
            xmlns:tds="http://www.onvif.org/ver10/device/wsdl"
            xmlns:t="http://www.onvif.org/ver10/schema"
            xmlns:tt="http://www.onvif.org/ver10/schema">
<s:Body>{}</s:Body>
</s:Envelope>"#,
        body_xml
    )
}

/// 判断 SOAP 响应是否含 Fault (认证失败/不支持)
fn has_fault(text: &str) -> bool {
    text.contains("<SOAP-ENV:Fault>") || text.contains("Fault")
}

/// ONVIF SOAP 请求, 三级认证回退:
/// 1. WS-Security (海康等)
/// 2. Basic Auth (部分老设备)
/// 3. 无认证裸 SOAP (新设备/测试相机, 不支持 WSS, 但允许匿名访问)
async fn soap_post(xaddr: &str, body_xml: &str, user: &str, pass: &str) -> Result<String, ()> {
    let client = reqwest::Client::new();

    // 1. 尝试 WS-Security
    let soap = make_soap_request(user, pass, body_xml);
    let resp = client
        .post(xaddr)
        .header("Content-Type", "application/soap+xml; charset=utf-8")
        .body(soap)
        .timeout(Duration::from_secs(5))
        .send()
        .await
        .map_err(|_| ())?;
    let text = resp.text().await.map_err(|_| ())?;

    if !has_fault(&text) {
        return Ok(text);
    }

    // 2. WSS 失败 → Basic Auth
    let soap2 = make_plain_soap(body_xml);
    let resp2 = client
        .post(xaddr)
        .basic_auth(user, Some(pass))
        .header("Content-Type", "application/soap+xml; charset=utf-8")
        .body(soap2.clone())
        .timeout(Duration::from_secs(5))
        .send()
        .await
        .map_err(|_| ())?;
    let text2 = resp2.text().await.map_err(|_| ())?;

    if !has_fault(&text2) {
        return Ok(text2);
    }

    // 3. Basic 也失败 → 无认证裸 SOAP (不带 Authorization 头)
    let resp3 = client
        .post(xaddr)
        .header("Content-Type", "application/soap+xml; charset=utf-8")
        .body(soap2)
        .timeout(Duration::from_secs(5))
        .send()
        .await
        .map_err(|_| ())?;
    let text3 = resp3.text().await.map_err(|_| ())?;
    Ok(text3)
}

async fn onvif_get_profiles(xaddr: &str, user: &str, pass: &str) -> Vec<ProfileInfo> {
    let body = "<trt:GetProfiles/>";
    let xml = match soap_post(xaddr, body, user, pass).await {
        Ok(x) => x,
        Err(_) => return vec![],
    };

    let mut profiles = parse_profiles(&xml);

    // 对每个 profile 获取 RTSP URI (GetStreamUri)
    // 注意: StreamSetup 内的 Stream/Transport 属于 schema 命名空间 (t:),
    // 不是 media 命名空间 (trt:) — 用错前缀会导致 Validation constraint violation
    for p in &mut profiles {
        let sbody = format!(
            r#"<trt:GetStreamUri><trt:StreamSetup><t:Stream>RTP-Unicast</t:Stream>
<t:Transport><t:Protocol>RTSP</t:Protocol></t:Transport></trt:StreamSetup>
<trt:ProfileToken>{}</trt:ProfileToken></trt:GetStreamUri>"#,
            p.token
        );
        if let Ok(sxml) = soap_post(xaddr, &sbody, user, pass).await {
            if let Some(uri) = extract_xml_val(&sxml, "tt:Uri", |v| Some(v.to_string())) {
                p.uri = uri;
            }
        }
    }
    profiles
}

fn parse_profiles(xml: &str) -> Vec<ProfileInfo> {
    let mut profiles = Vec::new();
    let mut pos = 0;
    while let Some(start) = xml[pos..].find("<trt:Profiles") {
        let seg_start = pos + start;
        pos = seg_start + 1;
        // token 是 Profiles 元素的属性, 如 <trt:Profiles token="MainStream">
        let token = if let Some(ts) = xml[seg_start..].find("token=\"") {
            let from = seg_start + ts + 7;
            if let Some(te) = xml[from..].find('"') {
                xml[from..from + te].to_string()
            } else { String::new() }
        } else { String::new() };
        let name = extract_xml_val(&xml[seg_start..], "tt:Name", |v| Some(v.to_string()))
            .unwrap_or_else(|| token.clone());
        let width: u32 = extract_xml_val(&xml[seg_start..], "tt:Width", |v| v.parse().ok())
            .unwrap_or(0);
        let height: u32 = extract_xml_val(&xml[seg_start..], "tt:Height", |v| v.parse().ok())
            .unwrap_or(0);
        // GetStreamUri 在 onvif_get_profiles 中填充
        let uri = String::new();
        profiles.push(ProfileInfo { token, name, width, height, uri });
    }
    profiles
}

fn extract_xml_val<T>(xml: &str, tag: &str, f: impl Fn(&str) -> Option<T>) -> Option<T> {
    let open = format!("<{}>", tag);
    let close = format!("</{}>", tag);
    let start = xml.find(&open)? + open.len();
    let end = xml[start..].find(&close)?;
    f(xml[start..start + end].trim())
}

// ─── IRCUT 红外控制 (标准 ONVIF Imaging) ─────────────────────

/// 获取摄像头 IP (从 config.ini 解析)
fn camera_ip() -> String {
    if let Ok(cfg) = std::fs::read_to_string("/root/config.ini") {
        for line in cfg.lines() {
            if let Some(url) = line.trim().strip_prefix("rtsp_url = ") {
                // rtsp://192.168.x.x/... 提取 IP
                if let Some(rest) = url.strip_prefix("rtsp://") {
                    let host = rest.split(['/', ':', '@']).next().unwrap_or("");
                    if !host.is_empty() {
                        return host.to_string();
                    }
                }
            }
        }
    }
    "192.168.50.168".to_string()
}

/// 获取 VideoSourceToken (从 GetProfiles 响应解析, 不同摄像头 token 不同)
/// 海康: VideoSourceMain | 标准相机: video_source1 / video_source_config1 等
/// 解析策略: 找 VideoSourceConfiguration 里的 SourceToken (视频源 token, 用于 Imaging)
async fn get_video_source_token(ip: &str, user: &str, pass: &str) -> String {
    let body = "<trt:GetProfiles/>";
    if let Ok(xml) = soap_post(&format!("http://{}/onvif/device_service", ip), body, user, pass).await {
        // 逐个尝试 SourceToken (不同前缀: tt: 或 t: 或无前缀)
        for tag in ["tt:SourceToken", "t:SourceToken", "SourceToken"] {
            if let Some(src) = extract_xml_val(&xml, tag, |v| Some(v.to_string())) {
                if !src.is_empty() {
                    return src;
                }
            }
        }
    }
    // 回退: 常见 token 名
    "VideoSourceMain".to_string()
}

/// 读取当前 IRCUT 状态 (ONVIF GetImagingSettings → IrCutFilter)
async fn onvif_get_ircut(ip: &str, user: &str, pass: &str) -> Option<String> {
    let token = get_video_source_token(ip, user, pass).await;
    let body = format!(
        "<trt:GetImagingSettings><trt:VideoSourceToken>{}</trt:VideoSourceToken></trt:GetImagingSettings>",
        token
    );
    let xml = soap_post(&format!("http://{}/onvif/device_service", ip), &body, user, pass).await.ok()?;
    // 提取 <tt:IrCutFilter>ON/OFF/AUTO</tt:IrCutFilter>
    extract_xml_val(&xml, "tt:IrCutFilter", |v| Some(v.to_string()))
}

/// 设置 IRCUT (ONVIF SetImagingSettings)
/// mode: "ON"=红外 / "OFF"=彩色 / "AUTO"=自动
async fn onvif_set_ircut(ip: &str, user: &str, pass: &str, mode: &str) -> bool {
    let token = get_video_source_token(ip, user, pass).await;
    let body = format!(
        r#"<trt:SetImagingSettings>
<trt:VideoSourceToken>{}</trt:VideoSourceToken>
<trt:ImagingSettings><t:IrCutFilter>{}</t:IrCutFilter></trt:ImagingSettings>
<trt:ForcePersistence>true</trt:ForcePersistence>
</trt:SetImagingSettings>"#,
        token, mode
    );
    let xml = match soap_post(&format!("http://{}/onvif/device_service", ip), &body, user, pass).await {
        Ok(x) => x,
        Err(_) => return false,
    };
    // 成功: 无 Fault
    !xml.contains("Fault")
}

/// 读取摄像头网卡配置 (ONVIF GetNetworkInterfaces)
/// 返回 (interface_token, dhcp, ip, prefix)
async fn onvif_get_network(ip: &str, user: &str, pass: &str) -> Option<(String, bool, String, String)> {
    let body = "<tds:GetNetworkInterfaces/>";
    let xml = soap_post(&format!("http://{}/onvif/device_service", ip), body, user, pass).await.ok()?;
    if xml.contains("Fault") { return None; }

    let token = extract_xml_val(&xml, "tds:NetworkInterfaces", |v| Some(v.to_string()))
        .and_then(|_| {
            // 提取 token 属性
            let start = xml.find("<tds:NetworkInterfaces")?;
            let seg = &xml[start..];
            let ts = seg.find("token=\"")? + 7;
            let te = seg[ts..].find('"')?;
            Some(seg[ts..ts + te].to_string())
        })
        .unwrap_or_else(|| "eth0".to_string());

    let dhcp = extract_xml_val(&xml, "tt:DHCP", |v| Some(v.to_string()))
        .map(|v| v == "true")
        .unwrap_or(false);
    let addr = extract_xml_val(&xml, "tt:Address", |v| Some(v.to_string()))
        .unwrap_or_default();
    let prefix = extract_xml_val(&xml, "tt:PrefixLength", |v| Some(v.to_string()))
        .unwrap_or_else(|| "24".to_string());

    Some((token, dhcp, addr, prefix))
}

/// 设置摄像头网段 (ONVIF SetNetworkInterfaces)
/// mode: "dhcp" 或 "static" (static 需 ip/prefix/gateway)
async fn onvif_set_network(ip: &str, user: &str, pass: &str,
                           interface: &str, dhcp: bool,
                           address: &str, prefix: &str) -> bool {
    let body = if dhcp {
        format!(
            r#"<tds:SetNetworkInterfaces><tds:InterfaceToken>{}</tds:InterfaceToken>
<tds:NetworkInterface><tt:Enabled>true</tt:Enabled>
<tt:IPv4><tt:Enabled>true</tt:Enabled><tt:Config><tt:DHCP>true</tt:DHCP></tt:Config></tt:IPv4>
</tds:NetworkInterface></tds:SetNetworkInterfaces>"#,
            interface
        )
    } else {
        format!(
            r#"<tds:SetNetworkInterfaces><tds:InterfaceToken>{}</tds:InterfaceToken>
<tds:NetworkInterface><tt:Enabled>true</tt:Enabled>
<tt:IPv4><tt:Enabled>true</tt:Enabled><tt:Config><tt:DHCP>false</tt:DHCP>
<tt:Manual><tt:Address>{}</tt:Address><tt:PrefixLength>{}</tt:PrefixLength></tt:Manual>
</tt:Config></tt:IPv4></tds:NetworkInterface></tds:SetNetworkInterfaces>"#,
            interface, address, prefix
        )
    };
    let xml = match soap_post(&format!("http://{}/onvif/device_service", ip), &body, user, pass).await {
        Ok(x) => x,
        Err(_) => return false,
    };
    !xml.contains("Fault")
}

// ─── HLS Pipeline ───────────────────────────────────────────

/// 解码 XML 转义 (&amp; → & 等), ONVIF GetStreamUri 返回的 Uri 里有 XML 转义
fn xml_unescape(s: &str) -> String {
    s.replace("&amp;", "&")
        .replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", "\"")
        .replace("&#39;", "'")
}

/// 从 URL 提取 host/path, 注入凭据 (从 creds 文件读取, 不在 URL 暴露明文)
fn inject_creds(url: &str) -> String {
    let url = xml_unescape(url);
    // 已带 user:pass@ 凭据 → 直接用
    if url.contains('@') {
        return url.to_string();
    }
    // 已带 ?username= 凭据 → 直接用 (ONVIF GetStreamUri 返回的形式)
    if url.contains("username=") {
        return url.to_string();
    }
    // 读取保存的凭据注入
    if let Ok(creds) = std::fs::read_to_string(CREDS_FILE) {
        let creds = creds.trim();
        if !creds.is_empty() {
            if let Some(at) = url.find("://") {
                let at = at + 3;
                return format!("{}{}@{}", &url[..at], creds, &url[at..]);
            }
        }
    }
    url.to_string()
}

async fn hls_start(url: &str) -> (String, String) {
    let tok = gen_token();
    let _ = tokio::fs::write("/tmp/hls_token", &tok).await;

    // 注入凭据 (creds 文件: user:pass)
    let authed_url = inject_creds(url);

    // Shell-escape single quotes
    let safe_url = authed_url.replace('\'', "'\\''");

    // Kill old transcoder
    let _ = Command::new("killall")
        .args(["-9", "gst-launch-1.0"])
        .output()
        .await;

    tokio::time::sleep(Duration::from_millis(300)).await;
    let _ = Command::new("sh")
        .arg("-c")
        .arg("mkdir -p /root/hls && rm -f /root/hls/*.ts /root/hls/*.m3u8")
        .output()
        .await;

    let cmd = format!(
        "gst-launch-1.0 rtspsrc location='{}' latency=100 \
         ! rtph264depay ! h264parse \
         ! hlssink2 location='/root/hls/seg_%05d.ts' \
         playlist-location='/root/hls/stream.m3u8' \
         target-duration=1 max-files=30 playlist-length=4 \
         </dev/null >/tmp/hls.log 2>&1 &",
        safe_url
    );
    let _ = Command::new("sh").arg("-c").arg(&cmd).output().await;

    (tok, safe_url)
}

fn hls_playlist(token: &str) -> String {
    let mut m3u8 = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:3\n".to_string();
    let mut seqs: Vec<i32> = Vec::new();

    if let Ok(dir) = std::fs::read_dir("/root/hls") {
        for entry in dir.flatten() {
            let name = entry.file_name().to_string_lossy().to_string();
            if let Some(num) = name.strip_prefix("seg_").and_then(|s| s.strip_suffix(".ts")) {
                if let Ok(n) = num.parse::<i32>() {
                    seqs.push(n);
                }
            }
        }
    }

    seqs.sort_unstable();
    let usable = if seqs.len() > 1 { seqs.len() - 1 } else { 0 };
    let start = if usable > 5 { usable - 5 } else { 0 };

    if usable > 0 {
        m3u8.push_str(&format!("#EXT-X-MEDIA-SEQUENCE:{}\n", seqs[start]));
        for i in start..usable {
            m3u8.push_str(&format!(
                "\n#EXTINF:2,\nseg_{:05}.ts?t={}\n",
                seqs[i], token
            ));
        }
    } else {
        m3u8.push_str("#EXT-X-MEDIA-SEQUENCE:0\n");
    }

    m3u8
}

// ─── Pipeline Control ───────────────────────────────────────

async fn pipeline_connect(base_url: &str, user: &str, pass: &str) -> Option<String> {
    // Save credentials — 显式 0600, 防同机其他用户读取 (扫1 #21)
    let _ = fs::write(CREDS_FILE, format!("{}:{}\n", user, pass)).await;
    let _ = Command::new("chmod").args(["600", CREDS_FILE]).output().await;

    // Save config.ini
    let cfg = format!(
        "[network]\nrtsp_url = {}\nrtsp_transport = tcp\n\n[log]\nlog_level = info\n",
        base_url
    );
    let _ = fs::write("/root/config.ini", &cfg).await;

    // Save last_connect
    let _ = fs::write(
        "/root/last_connect.json",
        format!("{{\"url\":\"{}\"}}\n", base_url),
    )
    .await;

    // Restart pipeline
    let _ = Command::new("killall").args(["-9", "rtsp_display"]).output().await;
    let _ = Command::new("killall").args(["weston"]).output().await;
    // 等 weston 完全退出 (最多 5 秒), 否则它退出时会写 bl_power=4 关背光
    for _ in 0..10 {
        let alive = Command::new("sh").arg("-c").arg("pidof weston").output().await
            .map(|o| !o.stdout.is_empty()).unwrap_or(false);
        if !alive { break; }
        tokio::time::sleep(Duration::from_millis(500)).await;
    }
    // weston 已退出, 开背光不会被覆盖
    let _ = Command::new("sh")
        .arg("-c")
        .arg("echo 0 > /sys/class/backlight/backlight/bl_power 2>/dev/null")
        .output()
        .await;
    // 激活 CRTC: 杀 weston 释放显示控制器, 不激活则黑屏
    let _ = Command::new("sh")
        .arg("-c")
        .arg("modetest -M rockchip -s 96@73:720x1280 >/tmp/modetest.log 2>&1 &")
        .output()
        .await;

    let _ = Command::new("sh")
        .arg("-c")
        .arg("cd /root && nohup /root/rtsp_display /root/config.ini </dev/null >/tmp/gst_web.log 2>&1 &")
        .output()
        .await;

    tokio::time::sleep(Duration::from_secs(1)).await;

    // Read PID
    let output = Command::new("sh")
        .arg("-c")
        .arg("pgrep -f rtsp_display")
        .output()
        .await
        .ok()?;
    let pid = String::from_utf8_lossy(&output.stdout).trim().to_string();
    Some(if pid.is_empty() { "0".into() } else { pid })
}

/// 从 URL 中提取 user:pass@ 并返回 (base_url, user, pass)
fn split_url_creds(url: &str) -> (String, String, String) {
    let mut user = String::new();
    let mut pass = String::new();
    if let Some(at_pos) = url.find('@') {
        if let Some(proto) = url.find("://") {
            let creds = &url[proto + 3..at_pos];
            if let Some(colon) = creds.find(':') {
                user = creds[..colon].to_string();
                pass = creds[colon + 1..].to_string();
            } else {
                user = creds.to_string();
            }
            let base = format!("{}{}", &url[..proto + 3], &url[at_pos + 1..]);
            return (base, user, pass);
        }
    }
    (url.to_string(), user, pass)
}

// ─── Auth helper ────────────────────────────────────────────

async fn check_auth(state: &AppState, headers: &axum::http::HeaderMap) -> bool {
    if let Some(auth) = headers.get(header::AUTHORIZATION) {
        if let Ok(val) = auth.to_str() {
            if let Some(token) = val.strip_prefix("Bearer ") {
                let sessions = state.sessions.lock().await;
                /* 过期检查: 登录时存入的 expiry 超时即失效 (扫1 #19) */
                return match sessions.get(token) {
                    Some(&exp) => now_secs() < exp,
                    None => false,
                };
            }
        }
    }
    false
}

fn unauthorized() -> Response {
    (StatusCode::UNAUTHORIZED, Json(serde_json::json!({"error":"unauthorized"}))).into_response()
}

// ─── HTTP Handlers ──────────────────────────────────────────

async fn handle_login(
    State(state): State<Arc<AppState>>,
    Json(req): Json<LoginReq>,
) -> Response {
    let fails = state.login_fails.lock().await;
    let now = now_secs();
    let recent: usize = fails.iter().filter(|&&t| now - t < 300).count();
    drop(fails);
    if recent >= 5 {
        return (StatusCode::TOO_MANY_REQUESTS, Json(serde_json::json!({"error":"too many attempts, try later"}))).into_response();
    }

    let pass = req.pass.unwrap_or_default();
    if pass.is_empty() {
        return (StatusCode::BAD_REQUEST, Json(serde_json::json!({"error":"missing password"}))).into_response();
    }

    if sha256_hex(&pass) != *state.pw_hash.lock().await {
        state.login_fails.lock().await.push(now);
        return (StatusCode::FORBIDDEN, Json(serde_json::json!({"error":"wrong user or password"}))).into_response();
    }

    state.login_fails.lock().await.clear();
    let token = gen_token();
    state.sessions.lock().await.insert(token.clone(), now + 86400);
    /* 默认密码提醒: 前端收到 default_pw=true 强制引导改密 (扫1 #5) */
    let is_default = *state.pw_hash.lock().await == DEFAULT_HASH;
    (StatusCode::OK, Json(serde_json::json!({"status":"ok","token":token,"default_pw":is_default}))).into_response()
}

/// POST /api/change_password — 修改登录密码
/// body: {"old_pass":"...","new_pass":"..."}
async fn handle_change_password(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
    Json(body): Json<serde_json::Value>,
) -> Response {
    // 需要登录
    if !check_auth(&state, &headers).await { return unauthorized(); }

    let old_pass = body["old_pass"].as_str().unwrap_or("").to_string();
    let new_pass = body["new_pass"].as_str().unwrap_or("").to_string();

    if old_pass.is_empty() || new_pass.is_empty() {
        return (StatusCode::BAD_REQUEST, Json(serde_json::json!({"error":"missing old_pass/new_pass"}))).into_response();
    }
    if new_pass.len() < 6 {
        return (StatusCode::BAD_REQUEST, Json(serde_json::json!({"error":"new password too short (min 6)"}))).into_response();
    }

    // 验证旧密码
    if sha256_hex(&old_pass) != *state.pw_hash.lock().await {
        return (StatusCode::FORBIDDEN, Json(serde_json::json!({"error":"old password wrong"}))).into_response();
    }

    // 写新密码 hash 到文件
    let new_hash = sha256_hex(&new_pass);
    let content = format!("admin:{}\n", new_hash);
    if let Err(_) = tokio::fs::write(PASSWD_FILE, content).await {
        return (StatusCode::INTERNAL_SERVER_ERROR, Json(serde_json::json!({"error":"write failed"}))).into_response();
    }

    // 更新内存
    *state.pw_hash.lock().await = new_hash;
    // 清空所有 session (强制重新登录)
    state.sessions.lock().await.clear();

    (StatusCode::OK, Json(serde_json::json!({"status":"ok"}))).into_response()
}

/// 获取 eth0 的 connman service 名 (按 MAC 动态生成)
async fn get_eth_service() -> String {
    let out = Command::new("connmanctl").arg("services").output().await;
    if let Ok(o) = out {
        let text = String::from_utf8_lossy(&o.stdout);
        for line in text.lines() {
            if line.contains("Wired") {
                if let Some(svc) = line.split_whitespace().find(|s| s.starts_with("ethernet_")) {
                    return svc.to_string();
                }
            }
        }
    }
    String::new()
}

/// GET /api/network — 查询当前网络配置
async fn handle_network_get(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
) -> Response {
    if !check_auth(&state, &headers).await { return unauthorized(); }

    let svc = get_eth_service().await;
    let mut method = "unknown";
    let mut ip = String::new();
    let mut netmask = String::new();
    let mut gateway = String::new();

    // 从 connmanctl services 输出解析 IPv4
    if !svc.is_empty() {
        let out = Command::new("connmanctl").arg("services").arg(&svc).output().await;
        if let Ok(o) = out {
            let text = String::from_utf8_lossy(&o.stdout);
            for line in text.lines() {
                let t = line.trim();
                if t.starts_with("IPv4 =") && t.contains("Method=") {
                    if t.contains("Method=dhcp") { method = "dhcp"; }
                    else if t.contains("Method=manual") { method = "static"; }
                    if let Some(pos) = t.find("Address=") {
                        ip = t[pos + 8..].split(',').next().unwrap_or("").trim().to_string();
                    }
                    if let Some(pos) = t.find("Netmask=") {
                        netmask = t[pos + 8..].split(',').next().unwrap_or("").trim().to_string();
                    }
                    if let Some(pos) = t.find("Gateway=") {
                        gateway = t[pos + 8..].split(',').next().unwrap_or("").trim().to_string();
                    }
                }
            }
        }
    }

    Json(serde_json::json!({
        "method": method,
        "ip": ip,
        "netmask": netmask,
        "gateway": gateway,
        "service": svc
    })).into_response()
}

/// POST /api/network — 修改网络配置
/// body: {"method":"dhcp"} 或 {"method":"static","ip":"...","netmask":"...","gateway":"..."}
async fn handle_network_set(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
    Json(body): Json<serde_json::Value>,
) -> Response {
    if !check_auth(&state, &headers).await { return unauthorized(); }

    let svc = get_eth_service().await;
    if svc.is_empty() {
        return (StatusCode::BAD_GATEWAY, Json(serde_json::json!({"error":"ethernet service not found"}))).into_response();
    }

    let method = body["method"].as_str().unwrap_or("").to_string();

    if method == "dhcp" {
        // 恢复 DHCP
        let _ = Command::new("connmanctl").args(["config", &svc, "--ipv4", "dhcp"]).output().await;
        return (StatusCode::OK, Json(serde_json::json!({"status":"ok","method":"dhcp"}))).into_response();
    }

    if method == "static" {
        let ip = body["ip"].as_str().unwrap_or("").to_string();
        let netmask = body["netmask"].as_str().unwrap_or("255.255.255.0").to_string();
        let gateway = body["gateway"].as_str().unwrap_or("").to_string();

        if ip.is_empty() || gateway.is_empty() {
            return (StatusCode::BAD_REQUEST, Json(serde_json::json!({"error":"ip and gateway required"}))).into_response();
        }

        let _ = Command::new("connmanctl")
            .args(["config", &svc, "--ipv4", "manual", &ip, &netmask, &gateway])
            .output()
            .await;

        return (StatusCode::OK, Json(serde_json::json!({"status":"ok","method":"static","ip":ip}))).into_response();
    }

    (StatusCode::BAD_REQUEST, Json(serde_json::json!({"error":"method must be dhcp or static"}))).into_response()
}

async fn handle_logout(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
) -> Response {
    if !check_auth(&state, &headers).await { return unauthorized(); }
    if let Some(auth) = headers.get(header::AUTHORIZATION) {
        if let Ok(val) = auth.to_str() {
            if let Some(tok) = val.strip_prefix("Bearer ") {
                state.sessions.lock().await.remove(tok);
            }
        }
    }
    let _ = std::fs::remove_file("/tmp/hls_token");
    let _ = Command::new("killall").args(["-9", "gst-launch-1.0"]).output().await;
    Json(serde_json::json!({"status":"ok"})).into_response()
}

async fn handle_scan(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
) -> Response {
    if !check_auth(&state, &headers).await { return unauthorized(); }

    // 发现设备 + 获取 profiles 全部并行
    let discovered = onvif_discover().await;
    let mut tasks = Vec::new();
    for (ip, xaddr) in &discovered {
        let xaddr = xaddr.clone();
        let ip2 = ip.clone();
        tasks.push(async move {
            // 尝试默认凭据 (并行尝试, 取第一个成功的)
            let blank = onvif_get_profiles(&xaddr, "admin", "");
            let pass1 = onvif_get_profiles(&xaddr, "admin", "123456");
            let pass2 = onvif_get_profiles(&xaddr, "admin", "admin");
            let (blank, pass1, pass2) = tokio::join!(blank, pass1, pass2);
            let profs = if !blank.is_empty() { blank }
                        else if !pass1.is_empty() { pass1 }
                        else { pass2 };
            DeviceInfo { ip: ip2.clone(), xaddrs: xaddr, name: ip2, profiles: profs }
        });
    }
    let devices: Vec<DeviceInfo> = futures::future::join_all(tasks).await;
    let count = devices.len();
    Json(ScanResp { devices, count }).into_response()
}

async fn handle_connect(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
    Json(body): Json<serde_json::Value>,
) -> Response {
    if !check_auth(&state, &headers).await { return unauthorized(); }

    let url = body["url"].as_str().unwrap_or("").to_string();
    if url.is_empty() {
        return (StatusCode::BAD_REQUEST, Json(serde_json::json!({"error":"missing url"}))).into_response();
    }
    /* 防 config.ini 注入 (扫2 #9): URL 带换行/控制字符直接拒绝 */
    if url.contains('\n') || url.contains('\r') || url.contains('\0') {
        return (StatusCode::BAD_REQUEST, Json(serde_json::json!({"error":"invalid url"}))).into_response();
    }

    // JSON 里的 user/pass 优先, 否则从 URL 提取
    let json_user = body["user"].as_str().unwrap_or("").to_string();
    let json_pass = body["pass"].as_str().unwrap_or("").to_string();
    let (base_url, url_user, url_pass) = split_url_creds(&url);
    let user = if json_user.is_empty() { if url_user.is_empty() { "admin".to_string() } else { url_user } } else { json_user };
    let pass = if json_pass.is_empty() { url_pass } else { json_pass };

    let pid = pipeline_connect(&base_url, &user, &pass).await;
    (StatusCode::OK, Json(serde_json::json!({"status":"ok","pid":pid,"rtsp":base_url}))).into_response()
}

async fn handle_status(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
) -> Response {
    if !check_auth(&state, &headers).await { return unauthorized(); }
    let running = Command::new("pgrep").args(["-f", "rtsp_display"]).output().await
        .map(|o| !o.stdout.is_empty()).unwrap_or(false);
    Json(serde_json::json!({"running":running})).into_response()
}

async fn handle_hls_start(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
    Json(body): Json<serde_json::Value>,
) -> Response {
    if !check_auth(&state, &headers).await { return unauthorized(); }
    let url = body["url"].as_str().unwrap_or("").to_string();
    if url.is_empty() {
        return (StatusCode::BAD_REQUEST, Json(serde_json::json!({"error":"missing url"}))).into_response();
    }
    let (tok, _) = hls_start(&url).await;
    (StatusCode::OK, Json(serde_json::json!({"status":"ok","hls_token":tok}))).into_response()
}

async fn handle_hls_stop(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
) -> Response {
    if !check_auth(&state, &headers).await { return unauthorized(); }
    let _ = Command::new("killall").args(["-9", "gst-launch-1.0"]).output().await;
    let _ = std::fs::remove_file("/tmp/hls_token");
    // 停止预览后清理分片缓存 (避免残留最多 30 个 ts 文件)
    let _ = Command::new("sh")
        .arg("-c")
        .arg("rm -f /root/hls/*.ts /root/hls/*.m3u8")
        .output()
        .await;
    Json(serde_json::json!({"status":"ok"})).into_response()
}

// ─── MJPEG 低延迟预览 (转码代理: 任意 RTSP → JPEG 流) ─────────

/// 全局: 转码进程 + JPEG 帧广播 (读线程推帧, 每个 /preview 连接 subscribe;
/// 慢消费者自动 lag 丢旧帧 — 实时预览语义)
static PREVIEW_CHILD: std::sync::Mutex<Option<std::process::Child>> =
    std::sync::Mutex::new(None);
static PREVIEW_TX: std::sync::Mutex<
    Option<tokio::sync::broadcast::Sender<Vec<u8>>>,
> = std::sync::Mutex::new(None);

/// POST /api/preview_start — 启动低延迟预览 (body: {"url":"rtsp://..."})
/// 成功返回 preview_token: /preview 流需带 ?t=<token> 鉴权
/// gst-launch 拉流 → 硬解 → mppjpegenc → JPEG 帧流
async fn handle_preview_start(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
    Json(body): Json<serde_json::Value>,
) -> Response {
    if !check_auth(&state, &headers).await { return unauthorized(); }
    let url = body["url"].as_str().unwrap_or("").to_string();
    if url.is_empty() {
        return (StatusCode::BAD_REQUEST,
                Json(serde_json::json!({"error":"missing url"}))).into_response();
    }

    // 停旧的
    if let Some(mut old) = PREVIEW_CHILD.lock().unwrap().take() {
        let _ = old.kill();
        let _ = old.wait();
    }
    *PREVIEW_TX.lock().unwrap() = None;

    // shell 转义单引号 (URL 可能含 ?username= 等)
    let safe_url = url.replace('\'', "'\\''");
    /* 注意: 不要加 videoscale/videorate — A7 上 CPU 缩放 ~24fps 降到
     * 9fps, videorate 对 mppvideodec 输出的时间戳误丢帧 (~4.7fps)。
     * D1 子码流原尺寸 JPEG 带宽 <1MB/s, 直出即可 */
    let cmd = format!(
        "gst-launch-1.0 -q rtspsrc location='{}' latency=100 protocols=4 \
         ! rtph264depay ! h264parse ! mppvideodec ! videoconvert \
         ! mppjpegenc ! fdsink fd=1 \
         </dev/null 2>/tmp/preview_gst.log",
        safe_url);

    let mut child = match std::process::Command::new("sh")
        .arg("-c")
        .arg(&cmd)
        .stdout(std::process::Stdio::piped())
        .spawn()
    {
        Ok(c) => c,
        Err(e) => return (StatusCode::INTERNAL_SERVER_ERROR,
                Json(serde_json::json!({"error": format!("spawn: {e}")}))).into_response(),
    };

    let stdout = child.stdout.take().expect("stdout piped");
    *PREVIEW_CHILD.lock().unwrap() = Some(child);

    /* 预览 token: <img> 带不了 Authorization 头, /preview 走 ?t= 鉴权 (同 HLS 机制) */
    let pv_tok = gen_token();
    let _ = tokio::fs::write("/tmp/preview_token", &pv_tok).await;

    let (tx, _) = tokio::sync::broadcast::channel::<Vec<u8>>(8);
    *PREVIEW_TX.lock().unwrap() = Some(tx.clone());

    // 读线程: 按 JPEG 标记 (FFD8 开头 / FFD9 结尾) 切帧, 组装 multipart 段
    std::thread::spawn(move || {
        let mut reader = std::io::BufReader::new(stdout);
        let mut state: u8 = 0;      // 0=找帧头 1=FF后 2=帧内 3=帧内FF后
        let mut frame: Vec<u8> = Vec::new();
        let mut byte = [0u8; 1];
        loop {
            match reader.read(&mut byte) {
                Ok(0) => break,
                Ok(_) => {
                    let b = byte[0];
                    match state {
                        0 => { if b == 0xFF { frame.push(b); state = 1; } }
                        1 => { frame.push(b);
                               state = if b == 0xD8 { 2 } else { frame.clear(); 0 }; }
                        2 => { frame.push(b);
                               if b == 0xFF { state = 3; }
                               else if frame.len() > 1_000_000 { frame.clear(); state = 0; } }
                        _ => { frame.push(b);
                               if b == 0xD9 {
                                   /* 一帧完整: 组 multipart 段 */
                                   let head = format!("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: {}\r\n\r\n", frame.len());
                                   let mut out = head.into_bytes();
                                   out.extend_from_slice(&frame);
                                   out.extend_from_slice(b"\r\n");
                                   let _ = tx.send(out);   /* 广播; 无接收者/慢接收者不影响 */
                                   frame.clear(); state = 0;
                               } else {
                                   state = if b == 0xD8 { 2 } else { 2 };
                               } }
                    }
                }
                Err(_) => break,
            }
        }
    });

    (StatusCode::OK, Json(serde_json::json!({"status":"ok","preview_token":pv_tok}))).into_response()
}

/// GET /preview — 浏览器低延迟预览 (multipart/x-mixed-replace 流)
/// 鉴权: ?t=<token> 必须匹配 /tmp/preview_token (与 HLS 同机制)
async fn handle_preview(raw_query: axum::extract::RawQuery) -> Response {
    let params = parse_query(raw_query.0.as_deref());
    if !check_preview_token(&params).await {
        return (StatusCode::UNAUTHORIZED, "unauthorized").into_response();
    }
    let tx = PREVIEW_TX.lock().unwrap().clone();
    let Some(tx) = tx else {
        return (StatusCode::NOT_FOUND, "preview not started").into_response();
    };
    let rx = tx.subscribe();
    let stream = futures::stream::unfold(rx, |mut rx| async move {
        loop {
            match rx.recv().await {
                Ok(frame) => {
                    return Some((Ok::<_, std::convert::Infallible>(
                        axum::body::Bytes::from(frame)), rx));
                }
                Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => {
                    continue;   /* 慢消费者丢帧: 跳过继续 (实时预览语义) */
                }
                Err(_) => return None,   /* channel 关闭: 流结束 */
            }
        }
    });
    Response::builder()
        .header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        .header("Cache-Control", "no-cache")
        .body(axum::body::Body::from_stream(stream))
        .unwrap()
}

/// POST /api/preview_stop — 停止低延迟预览 (需登录)
async fn handle_preview_stop(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
) -> Response {
    if !check_auth(&state, &headers).await { return unauthorized(); }
    if let Some(mut c) = PREVIEW_CHILD.lock().unwrap().take() {
        let _ = c.kill();
        let _ = c.wait();
    }
    *PREVIEW_TX.lock().unwrap() = None;
    let _ = std::fs::remove_file("/tmp/preview_token");
    Json(serde_json::json!({"status":"ok"})).into_response()
}

/// POST /api/ircut — 切换日夜模式 (body: {"mode":"day"} / {"mode":"night"})
async fn handle_ircut(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
    Json(body): Json<serde_json::Value>,
) -> Response {
    if !check_auth(&state, &headers).await { return unauthorized(); }

    let mode = body["mode"].as_str().unwrap_or("").to_string();
    if mode != "day" && mode != "night" {
        return (StatusCode::BAD_REQUEST, Json(serde_json::json!({"error":"mode must be day or night"}))).into_response();
    }

    let ip = camera_ip();
    // 凭据: 从 creds 文件读 user:pass
    let creds = std::fs::read_to_string(CREDS_FILE).unwrap_or_default();
    let (user, pass) = match creds.trim().split_once(':') {
        Some((u, p)) => (u.to_string(), p.to_string()),
        None => ("admin".to_string(), "123456".to_string()),
    };

    // 标准 ONVIF: day → OFF (彩色), night → ON (红外)
    let onvif_mode = if mode == "day" { "OFF" } else { "ON" };
    let ok = onvif_set_ircut(&ip, &user, &pass, onvif_mode).await;

    if ok {
        Json(serde_json::json!({"status":"ok","mode":mode})).into_response()
    } else {
        (StatusCode::BAD_GATEWAY, Json(serde_json::json!({"error":"ircut command failed"}))).into_response()
    }
}

/// GET /api/system_info — 系统信息 (版本/内存/运行时长/网络)
async fn handle_system_info(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
) -> Response {
    if !check_auth(&state, &headers).await { return unauthorized(); }

    // 内存
    let mut mem_total = String::new();
    let mut mem_free = String::new();
    if let Ok(data) = std::fs::read_to_string("/proc/meminfo") {
        for line in data.lines() {
            if line.starts_with("MemTotal:") { mem_total = line.split_whitespace().nth(1).unwrap_or("").to_string(); }
            if line.starts_with("MemAvailable:") { mem_free = line.split_whitespace().nth(1).unwrap_or("").to_string(); }
        }
    }

    // 运行时长
    let mut uptime = String::new();
    if let Ok(data) = std::fs::read_to_string("/proc/uptime") {
        let secs: f64 = data.split_whitespace().next().unwrap_or("0").parse().unwrap_or(0.0);
        uptime = format!("{:.0} 秒 ({:.1} 小时)", secs, secs / 3600.0);
    }

    // 固件版本 (构建时间)
    let build = env!("CARGO_PKG_VERSION");

    // 当前摄像头
    let camera = camera_ip();
    let pipeline_running = Command::new("pgrep").args(["-f", "rtsp_display"]).output().await
        .map(|o| !o.stdout.is_empty()).unwrap_or(false);

    Json(serde_json::json!({
        "status":"ok",
        "version": build,
        "uptime": uptime,
        "mem_total_kb": mem_total,
        "mem_free_kb": mem_free,
        "camera": camera,
        "pipeline_running": pipeline_running
    })).into_response()
}

/// POST /api/power — 重启或关机 (body: {"action":"reboot"} / {"action":"shutdown"})
async fn handle_power(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
    Json(body): Json<serde_json::Value>,
) -> Response {
    if !check_auth(&state, &headers).await { return unauthorized(); }

    let action = body["action"].as_str().unwrap_or("").to_string();
    if action != "reboot" && action != "shutdown" {
        return (StatusCode::BAD_REQUEST, Json(serde_json::json!({"error":"action must be reboot or shutdown"}))).into_response();
    }

    // 先响应, 再执行 (否则连接会断)
    let cmd = if action == "reboot" { "reboot" } else { "poweroff -f" };
    let _ = Command::new("sh").arg("-c").arg(format!("sleep 1 && {}", cmd)).output().await;
    Json(serde_json::json!({"status":"ok","action":action})).into_response()
}

/// 校验 NTP 服务器字段: 只允许 域名/IPv4/IPv6 字符 (字母数字 . - : _),
/// 其余一律拒绝 — 防 server 参数注入 shell (扫1 #4 高危)
fn sanitize_ntp_server(s: &str) -> String {
    let ok = !s.is_empty()
        && s.len() <= 253
        && s.bytes().all(|c| c.is_ascii_alphanumeric() || b".-:_".contains(&c));
    if ok { s.to_string() } else { String::new() }
}

/// GET /api/time — 获取当前系统时间
/// POST /api/time — NTP 校时 (body: {"server":"ntp.aliyun.com"} 可选)
async fn handle_time(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
    req: axum::http::Request<axum::body::Body>,
) -> Response {
    if !check_auth(&state, &headers).await { return unauthorized(); }

    if req.method() == axum::http::Method::GET {
        // 读当前时间
        let out = Command::new("date").arg("+%Y-%m-%d %H:%M:%S").output().await;
        let time_str = out.map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
            .unwrap_or_else(|_| "unknown".to_string());
        return Json(serde_json::json!({"status":"ok","time":time_str})).into_response();
    }

    // POST: NTP 校时 — 白名单校验 + 参数数组执行 (不经过 shell)
    let body_bytes = axum::body::to_bytes(req.into_body(), 4096).await.unwrap_or_default();
    let server = serde_json::from_slice::<serde_json::Value>(&body_bytes)
        .ok()
        .and_then(|v| v["server"].as_str().map(|s| s.to_string()))
        .unwrap_or_else(|| "ntp.aliyun.com".to_string());
    let server = sanitize_ntp_server(&server);
    let server = if server.is_empty() { "ntp.aliyun.com".to_string() } else { server };

    let _ = Command::new("ntpd")
        .args(["-q", "-p", &server])
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .output()
        .await;

    // 读校时后时间
    let out = Command::new("date").arg("+%Y-%m-%d %H:%M:%S").output().await;
    let time_str = out.map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
        .unwrap_or_else(|_| "unknown".to_string());

    Json(serde_json::json!({"status":"ok","time":time_str,"server":server})).into_response()
}

/// GET /api/logs — 查看系统日志 (尾 N 行)
/// query: ?file=gst|web|hls|all&lines=100
async fn handle_logs(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
    raw_query: axum::extract::RawQuery,
) -> Response {
    if !check_auth(&state, &headers).await { return unauthorized(); }

    let params = parse_query(raw_query.0.as_deref());
    let file = params.get("file").map(|s| s.as_str()).unwrap_or("all");
    let lines: usize = params.get("lines").and_then(|s| s.parse().ok()).unwrap_or(100);

    // 日志文件映射
    let files: Vec<(&str, &str)> = match file {
        "gst" => vec![("显示管线", "/tmp/gst_web.log")],
        "web" => vec![("Web 后台", "/tmp/web.log")],
        "hls" => vec![("HLS 转码", "/tmp/hls.log")],
        _ => vec![
            ("显示管线", "/tmp/gst_web.log"),
            ("Web 后台", "/tmp/web.log"),
            ("HLS 转码", "/tmp/hls.log"),
        ],
    };

    let mut result = serde_json::Map::new();
    for (name, path) in &files {
        let content = std::fs::read_to_string(path).unwrap_or_default();
        let tail: Vec<&str> = content.lines().rev().take(lines).collect();
        let mut tail_vec = tail.clone();
        tail_vec.reverse();
        result.insert(name.to_string(), serde_json::json!(tail_vec));
    }

    Json(serde_json::json!({"status":"ok","logs":result})).into_response()
}

/// GET /api/camera_network — 读摄像头网卡配置
async fn handle_camera_network_get(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
) -> Response {
    if !check_auth(&state, &headers).await { return unauthorized(); }

    let ip = camera_ip();
    let creds = std::fs::read_to_string(CREDS_FILE).unwrap_or_default();
    let (user, pass) = match creds.trim().split_once(':') {
        Some((u, p)) => (u.to_string(), p.to_string()),
        None => ("admin".to_string(), "123456".to_string()),
    };

    match onvif_get_network(&ip, &user, &pass).await {
        Some((token, dhcp, addr, prefix)) => {
            Json(serde_json::json!({
                "status":"ok", "interface":token, "dhcp":dhcp,
                "ip":addr, "prefix":prefix
            })).into_response()
        }
        None => (StatusCode::BAD_GATEWAY, Json(serde_json::json!({"error":"get network failed"}))).into_response(),
    }
}

/// POST /api/camera_network — 修改摄像头网段
/// body: {"method":"dhcp"} 或 {"method":"static","ip":"...","prefix":"24"}
async fn handle_camera_network_set(
    State(state): State<Arc<AppState>>,
    headers: axum::http::HeaderMap,
    Json(body): Json<serde_json::Value>,
) -> Response {
    if !check_auth(&state, &headers).await { return unauthorized(); }

    let ip = camera_ip();
    let creds = std::fs::read_to_string(CREDS_FILE).unwrap_or_default();
    let (user, pass) = match creds.trim().split_once(':') {
        Some((u, p)) => (u.to_string(), p.to_string()),
        None => ("admin".to_string(), "123456".to_string()),
    };

    // 先读当前网卡 token
    let (interface, _, _, _) = match onvif_get_network(&ip, &user, &pass).await {
        Some(v) => v,
        None => ("eth0".to_string(), false, String::new(), "24".to_string()),
    };

    let method = body["method"].as_str().unwrap_or("").to_string();
    let ok = if method == "dhcp" {
        onvif_set_network(&ip, &user, &pass, &interface, true, "", "").await
    } else if method == "static" {
        let addr = body["ip"].as_str().unwrap_or("").to_string();
        let prefix = body["prefix"].as_str().unwrap_or("24").to_string();
        if addr.is_empty() {
            return (StatusCode::BAD_REQUEST, Json(serde_json::json!({"error":"ip required"}))).into_response();
        }
        onvif_set_network(&ip, &user, &pass, &interface, false, &addr, &prefix).await
    } else {
        return (StatusCode::BAD_REQUEST, Json(serde_json::json!({"error":"method must be dhcp or static"}))).into_response();
    };

    if ok {
        Json(serde_json::json!({"status":"ok","method":method})).into_response()
    } else {
        (StatusCode::BAD_GATEWAY, Json(serde_json::json!({"error":"set network failed"}))).into_response()
    }
}

// ─── HLS auth middleware ────────────────────────────────────

async fn check_hls_token(params: &HashMap<String, String>) -> bool {
    if let Some(tok) = params.get("t") {
        if let Ok(stored) = tokio::fs::read_to_string("/tmp/hls_token").await {
            return stored.trim() == tok.trim();
        }
    }
    false
}

/// MJPEG 预览流 token 校验 (与 HLS 同机制, 独立文件 /tmp/preview_token)
async fn check_preview_token(params: &HashMap<String, String>) -> bool {
    if let Some(tok) = params.get("t") {
        if let Ok(stored) = tokio::fs::read_to_string("/tmp/preview_token").await {
            return stored.trim() == tok.trim();
        }
    }
    false
}

async fn handle_hls_m3u8(raw_query: axum::extract::RawQuery) -> impl IntoResponse {
    let params = parse_query(raw_query.0.as_deref());
    if !check_hls_token(&params).await {
        return (StatusCode::UNAUTHORIZED, "unauthorized").into_response();
    }
    let tok = params.get("t").map(|s| s.as_str()).unwrap_or("");
    (StatusCode::OK, hls_playlist(tok)).into_response()
}

/// 静态文件服务 (替代 nest_service: 它会把 /hls/* 请求也吞掉导致 404)
async fn handle_static(
    Path(filename): Path<String>,
) -> impl IntoResponse {
    // 防目录穿越
    if filename.contains("..") {
        return (StatusCode::FORBIDDEN, "forbidden").into_response();
    }
    serve_file(&format!("{}/{}", STATIC_DIR, filename)).await
}

/// 首页 (/) → index.html
async fn handle_index() -> impl IntoResponse {
    serve_file(&format!("{}/index.html", STATIC_DIR)).await
}

async fn serve_file(path: &str) -> Response {
    match tokio::fs::read(path).await {
        Ok(data) => {
            let ct = if path.ends_with(".html") {
                "text/html; charset=utf-8"
            } else if path.ends_with(".js") {
                "application/javascript"
            } else if path.ends_with(".css") {
                "text/css"
            } else {
                "application/octet-stream"
            };
            (StatusCode::OK, [(header::CONTENT_TYPE, ct)], data).into_response()
        }
        Err(_) => (StatusCode::NOT_FOUND, "not found").into_response(),
    }
}

/// 手动解析 query string (axum 0.7 中 Path+Query 组合有兼容问题,
/// query 会被并进 Path, 导致带 ?t= 的分片请求 404)
fn parse_query(raw: Option<&str>) -> HashMap<String, String> {
    let mut map = HashMap::new();
    if let Some(q) = raw {
        for pair in q.split('&') {
            if let Some(eq) = pair.find('=') {
                map.insert(pair[..eq].to_string(), pair[eq + 1..].to_string());
            }
        }
    }
    map
}

async fn handle_hls_ts(
    Path(filename): Path<String>,
    raw_query: axum::extract::RawQuery,
) -> impl IntoResponse {
    let params = parse_query(raw_query.0.as_deref());
    if !check_hls_token(&params).await {
        return (StatusCode::UNAUTHORIZED, "unauthorized").into_response();
    }
    // Sanitize
    if filename.contains("..") {
        return (StatusCode::FORBIDDEN, "forbidden").into_response();
    }
    let path = format!("/root/hls/{}", filename);
    match tokio::fs::read(&path).await {
        Ok(data) => (
            StatusCode::OK,
            [(header::CONTENT_TYPE, "video/MP2T")],
            data,
        )
            .into_response(),
        Err(_) => (StatusCode::NOT_FOUND, "not found").into_response(),
    }
}

// ─── Main ───────────────────────────────────────────────────

async fn auto_recover() {
    if Command::new("pgrep").args(["-f", "rtsp_display"]).output().await
        .map(|o| !o.stdout.is_empty()).unwrap_or(false) {
        return; // already running
    }

    if let Ok(data) = tokio::fs::read_to_string("/root/last_connect.json").await {
        if let Some(url_start) = data.find("\"url\":\"") {
            let url = &data[url_start + 7..];
            if let Some(url_end) = url.find('"') {
                let base_url = &url[..url_end];
                let cfg = format!(
                    "[network]\nrtsp_url = {}\nrtsp_transport = tcp\n\n[log]\nlog_level = info\n",
                    base_url
                );
                let _ = tokio::fs::write("/root/config.ini", &cfg).await;
                println!("自动恢复: {}", base_url);

                let _ = Command::new("killall").args(["weston"]).output().await;
                // 等 weston 完全退出, 否则退出时写 bl_power=4 关背光
                for _ in 0..10 {
                    let alive = Command::new("sh").arg("-c").arg("pidof weston").output().await
                        .map(|o| !o.stdout.is_empty()).unwrap_or(false);
                    if !alive { break; }
                    tokio::time::sleep(Duration::from_millis(500)).await;
                }
                let _ = Command::new("killall").args(["-9", "rtsp_display"]).output().await;
                // weston 已退出, 开背光不会被覆盖
                let _ = Command::new("sh")
                    .arg("-c")
                    .arg("echo 0 > /sys/class/backlight/backlight/bl_power 2>/dev/null")
                    .output()
                    .await;
                // 激活 CRTC: 杀 weston 释放显示控制器, 不激活则黑屏
                let _ = Command::new("sh")
                    .arg("-c")
                    .arg("modetest -M rockchip -s 96@73:720x1280 >/tmp/modetest.log 2>&1 &")
                    .output()
                    .await;
                let _ = Command::new("sh")
                    .arg("-c")
                    .arg("cd /root && nohup /root/rtsp_display /root/config.ini </dev/null >/tmp/gst_web.log 2>&1 &")
                    .output()
                    .await;
            }
        }
    }
}

#[tokio::main]
async fn main() {
    // Init password
    let pw_hash = match tokio::fs::read_to_string(PASSWD_FILE).await {
        Ok(data) => {
            if let Some(colon) = data.find(':') {
                data[colon + 1..].trim().to_string()
            } else {
                DEFAULT_HASH.to_string()
            }
        }
        Err(_) => {
            let _ = tokio::fs::write(
                PASSWD_FILE,
                format!("admin:{}\n", DEFAULT_HASH),
            )
            .await;
            DEFAULT_HASH.to_string()
        }
    };

    let state = Arc::new(AppState {
        pw_hash: Mutex::new(pw_hash),
        sessions: Mutex::new(HashMap::new()),
        login_fails: Mutex::new(Vec::new()),
        devices: Mutex::new(Vec::new()),
    });

    // Auto-recover last camera
    auto_recover().await;

    // Build router
    let app = Router::new()
        .route("/api/login", post(handle_login))
        .route("/api/logout", post(handle_logout))
        .route("/api/change_password", post(handle_change_password))
        .route("/api/network", get(handle_network_get).post(handle_network_set))
        .route("/api/camera_network", get(handle_camera_network_get).post(handle_camera_network_set))
        .route("/api/system_info", get(handle_system_info))
        .route("/api/power", post(handle_power))
        .route("/api/logs", get(handle_logs))
        .route("/api/time", get(handle_time).post(handle_time))
        .route("/api/scan", get(handle_scan))
        .route("/api/connect", post(handle_connect))
        .route("/api/status", get(handle_status))
        .route("/api/hls_start", post(handle_hls_start))
        .route("/api/hls_stop", post(handle_hls_stop))
        .route("/api/preview_start", post(handle_preview_start))
        .route("/api/preview_stop", post(handle_preview_stop))
        .route("/preview", get(handle_preview))
        .route("/api/ircut", post(handle_ircut))
        .route("/hls/stream.m3u8", get(handle_hls_m3u8))
        .route("/hls/:filename", get(handle_hls_ts))
        .route("/", get(handle_index))
        .route("/:filename", get(handle_static))
        .with_state(state);

    // bind 失败不 panic: 看门狗或部署脚本可能留了旧进程占着 8090,
    // panic 会掩盖真正的问题 (端口冲突), 优雅报错退出即可
    let listener = match tokio::net::TcpListener::bind("0.0.0.0:8090").await {
        Ok(l) => l,
        Err(e) => {
            eprintln!("ERROR: bind 8090 失败: {}", e);
            eprintln!("提示: 可能有旧 rv1126_web 进程还在, 执行 killall -9 rv1126_web 后重试");
            std::process::exit(1);
        }
    };
    println!("rv1126_web (Rust) 已启动: http://0.0.0.0:8090");
    if let Err(e) = axum::serve(listener, app).await {
        eprintln!("ERROR: 服务异常退出: {}", e);
        std::process::exit(1);
    }
}
