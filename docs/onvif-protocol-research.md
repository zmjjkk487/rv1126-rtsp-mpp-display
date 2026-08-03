# ONVIF 协议研究（基于官方规范）

> 来源：OASIS WS-Discovery 1.1、ONVIF Core Spec v2.0、ONVIF Media Service WSDL、ONVIF Application Programmer's Guide

## 一、WS-Discovery — 设备发现

### 1.1 传输层

| 参数 | 值 |
|------|-----|
| 协议 | UDP |
| 目标地址 | **239.255.255.250**（IPv4 组播） |
| 目标端口 | **3702** |
| 消息格式 | SOAP 1.2 XML |

来源：OASIS WS-Discovery 1.1 §4.1 "Multicast Discovery"

### 1.2 发送端：Probe 消息

**必须设置的 header 字段**（OASIS WS-Discovery 1.1 §5.2）：

| Header 元素 | 值 |
|-------------|-----|
| `wsa:Action` | `http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01/Probe` |
| `wsa:MessageID` | 唯一 URI（建议 `urn:uuid:` + 随机 UUID） |
| `wsa:To` | `urn:docs-oasis-open-org:ws-dd:ns:discovery:2009:01` |

**Body 元素**（可选的 Types 过滤器）：

```
<d:Probe>
  <d:Types>dn:NetworkVideoTransmitter</d:Types>   <!-- ONVIF 网络摄像机 -->
</d:Probe>
```

**两个版本**：2005 版 namespace = `http://schemas.xmlsoap.org/ws/2005/04/discovery`，2009 版 = `http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01`。**ONVIF 设备至少响应一种**（Core Spec §7.3），**推荐两者都发**。

**Types 筛选**：
- `dn:NetworkVideoTransmitter` → 只匹配 IP 摄像机
- `tds:Device` → 匹配所有 ONVIF 设备（包括 NVR）
- 空 → 匹配所有 WS-Discovery 设备（可能包括打印机等非 ONVIF 设备）

来源：OASIS WS-Discovery 1.1 §5.2、ONVIF Core Spec §7.3

### 1.3 接收端：ProbeMatch 响应

**必须存在的字段**（OASIS WS-Discovery 1.1 §5.3）：

| 元素 | 说明 | 如何提取 |
|------|------|---------|
| `d:XAddrs` | **空格分隔的服务 URL**，必有 `http://host/onvif/device_service` | `strstr(xml, "<d:XAddrs>")` → 取 `</d:XAddrs>` 之前的内容 |
| `d:Types` | 设备类型，ONVIF 设备包含 `tds:Device` 和/或 `dn:NetworkVideoTransmitter` | 可跳过 |
| `d:Scopes` | `onvif://www.onvif.org/...` URI 列表，含硬件型号 | 可选，用于显示设备名称 |
| `a:EndpointReference/a:Address` | 设备唯一标识 UUID | 可选，用于去重 |
| `d:MetadataVersion` | 元数据版本号 | 可忽略 |

**Header 验证**：`a:RelatesTo` 必须等于本端 Probe 的 `MessageID`（用于匹配请求和响应）。

来源：OASIS WS-Discovery 1.1 §5.3、ONVIF Core Spec §7.4

### 1.4 精确的 XML 模板（Probe，2009 版）

```xml
<?xml version="1.0" encoding="utf-8"?>
<e:Envelope
  xmlns:e="http://www.w3.org/2003/05/soap-envelope"
  xmlns:w="http://schemas.xmlsoap.org/ws/2004/08/addressing"
  xmlns:d="http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01"
  xmlns:dn="http://www.onvif.org/ver10/network/wsdl">
  <e:Header>
    <w:MessageID>uuid:RANDOM-UUID-HERE</w:MessageID>
    <w:To>urn:docs-oasis-open-org:ws-dd:ns:discovery:2009:01</w:To>
    <w:Action>http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01/Probe</w:Action>
  </e:Header>
  <e:Body>
    <d:Probe>
      <d:Types>dn:NetworkVideoTransmitter</d:Types>
    </d:Probe>
  </e:Body>
</e:Envelope>
```

**C 代码构造**：用 `snprintf()` 格式化一个 `char` 数组，UUID 用 `/dev/urandom` 或 `time() + rand()` 生成。

来源：OASIS WS-Discovery 1.1 §5.2、生产代码 `discoverymanager.cpp:33-53`

---

## 二、ONVIF Media Service — 获取 RTSP 地址

### 2.1 传输层

| 参数 | 值 |
|------|-----|
| 协议 | HTTP/1.1 |
| 方法 | POST |
| URL | `http://{ip}/onvif/device_service`（从 XAddrs 获得） |
| Content-Type | `application/soap+xml; charset=utf-8` |
| SOAPAction | 见各操作 |

来源：ONVIF Media Service WSDL

### 2.2 GetProfiles — 获取所有码流配置

**SOAPAction**: `http://www.onvif.org/ver10/media/wsdl/GetProfiles`

**请求体**：

```xml
<e:Envelope xmlns:e="http://www.w3.org/2003/05/soap-envelope"
            xmlns:m="http://www.onvif.org/ver10/media/wsdl">
  <e:Header/>
  <e:Body>
    <m:GetProfiles/>
  </e:Body>
</e:Envelope>
```

**响应提取**：找 `<trt:Profiles token="MAIN_STREAM_TOKEN">`，token 值是后续 GetStreamUri 的输入。同时也包含 `<tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height></tt:Resolution>`（可选提取，供前端显示）。

来源：ONVIF Media Service WSDL §5.2

### 2.3 GetStreamUri — 获取 RTSP 地址

**SOAPAction**: `http://www.onvif.org/ver10/media/wsdl/GetStreamUri`

**请求体**（必须带 StreamSetup）：

```xml
<e:Envelope xmlns:e="http://www.w3.org/2003/05/soap-envelope"
            xmlns:m="http://www.onvif.org/ver10/media/wsdl"
            xmlns:t="http://www.onvif.org/ver10/schema">
  <e:Header/>
  <e:Body>
    <m:GetStreamUri>
      <m:StreamSetup>
        <t:Stream>RTP-Unicast</t:Stream>
        <t:Transport>
          <t:Protocol>RTSP</t:Protocol>
        </t:Transport>
      </m:StreamSetup>
      <m:ProfileToken>REPLACE_WITH_TOKEN</m:ProfileToken>
    </m:GetStreamUri>
  </e:Body>
</e:Envelope>
```

**响应提取**：找 `<tt:Uri>rtsp://...</tt:Uri>`，用 `strstr()` 定位 `<tt:Uri>` 和 `</tt:Uri>`。

来源：ONVIF Media Service WSDL §5.3

### 2.4 Media 服务地址

**标准路径**（ONVIF Core Spec §5.1.2）：
- Device Service: `/onvif/device_service`
- Media Service: `/onvif/Media` 或 `/onvif/media_service`

大多数摄像头两个服务在同一个 IP，只需要不同路径。**如果 GetProfiles 直接发到 `/onvif/device_service` 返回了 media 相关响应，说明设备支持单一服务端点**。

---

## 三、认证 — WS-Security

### 3.1 两种方式

| 方式 | 复杂度 | 兼容性 |
|------|--------|--------|
| HTTP Basic Auth | 极低（`Authorization: Basic base64(user:pass)`） | 部分设备支持 |
| WS-Security PasswordDigest | 中（SHA1+Nonce+Base64） | **必须支持**（Core Spec §6.1） |
| WS-Security PasswordText | 低（明文 Base64） | 部分设备支持（不安全） |

### 3.2 PasswordDigest 算法

```
PasswordDigest = Base64( SHA1( Nonce + Created + Password ) )
```

其中：
- **Nonce**: 16 字节随机数，Base64 编码
- **Created**: ISO 8601 UTC 时间戳，精确到秒（如 `2024-01-01T00:00:00Z`）
- **Password**: 原始密码（UTF-8 或 ASCII）

**C 实现**：`/dev/urandom` → 16 字节 nonce → `time()` → `gmtime()` → `strftime()` 格式化 Created → `SHA1(nonce_bytes + created_str + password)` → `base64_encode()`。

来源：OASIS WS-Security 1.0 §4.1、ONVIF Core Spec §6.1

### 3.3 注入位置

SOAP Envelope 的 `<e:Header>` 中，在 `<e:Body>` 之前插入：

```xml
<e:Header>
  <wsse:Security
    xmlns:wsse="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd"
    xmlns:wsu="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"
    e:mustUnderstand="true">
    <wsse:UsernameToken wsu:Id="UsernameToken-1">
      <wsse:Username>admin</wsse:Username>
      <wsse:Password Type="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest">
        BASE64_SHA1_HASH
      </wsse:Password>
      <wsse:Nonce EncodingType="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary">
        BASE64_16BYTES
      </wsse:Nonce>
      <wsu:Created>2024-01-01T00:00:00Z</wsu:Created>
    </wsse:UsernameToken>
  </wsse:Security>
</e:Header>
```

来源：OASIS WS-Security 1.0、ONVIF Core Spec §6.1.2

---

## 四、最小实现流程

```
步骤1: UDP sendto("239.255.255.250", 3702, PROBE_XML)
        ↓
步骤2: recvfrom() → 解析 XAddrs → 提取 http://IP/onvif/device_service
        ↓
步骤3: HTTP POST(IP/onvif/device_service, GET_PROFILES_XML)
        ↓  (若401则加WS-Security头重试)
步骤4: 解析 <Profiles token="MAIN"> → 选主码流 .token
        ↓
步骤5: HTTP POST(IP/onvif/device_service, GET_STREAM_URI_XML(token))
        ↓  (若401则加WS-Security头重试)
步骤6: 解析 <Uri>rtsp://IP:554/...</Uri> → 得到 RTSP 地址
```

## 五、与生产代码（T113 参考项目）的对照

参考项目 `discoverymanager.cpp` 实现了步骤 1-2，`onvifclient.cpp` 实现了步骤 3-6。关键差异：

| 官方规范要求 | 参考项目实现 | 评价 |
|-------------|-------------|------|
| Probe 发 2009 版 | 同时发 2005+2009，6 种 Probe | ✅ 更兼容 |
| Probe 发组播 | 组播 + 单播扫 254 个 IP | ✅ 兜底不回组播的设备 |
| XAddrs 解析 | 优先取 `/onvif/device_service` → 回退 HTTP GET 探测 | ✅ 符合规范 |
| GetStreamUri 带 StreamSetup | 带 `RTP-Unicast` + `RTSP` | ✅ 符合规范 |
| 认证 | 先试 HTTP Basic Auth → 再试 WS-Security PasswordDigest | ✅ 符合规范 |
| credentials 持久化 | JSON 文件按 host 存储 | ✅ 产品级 |

## 六、C 实现要点

1. **UDP 组播**：`socket(AF_INET, SOCK_DGRAM)` → `setsockopt(IPPROTO_IP, IP_MULTICAST_TTL, &ttl)` → `sendto()` 到 `239.255.255.250:3702`
2. **HTTP POST**：`libcurl`（板上已有）或手写 socket `send()/recv()`
3. **XML 解析**：不引入 libxml2，用 `strstr()` + `strchr()` 提取关键标签
4. **SHA1**：`openssl/sha.h` 的 `SHA1()` 或自行实现（RFC 3174，约 150 行 C）
5. **Base64**：自行实现（约 30 行 C）
6. **UUID**：`/dev/urandom` 读 16 字节 → `snprintf` 格式化为 UUID 字符串

## 参考文献

1. OASIS WS-Discovery 1.1: http://docs.oasis-open.org/ws-dd/discovery/1.1/wsdd-discovery-1.1-spec.html
2. OASIS WS-Security 1.0: http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd
3. ONVIF Core Specification v2.0: https://www.onvif.org/profiles/specifications/
4. ONVIF Application Programmer's Guide: https://www.onvif.org/wp-content/uploads/2016/12/ONVIF_WG-APG-Application_Programmers_Guide-1.pdf
5. ONVIF Profile S: camera streaming with RTSP/H.264
