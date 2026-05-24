# RTSP 实时流协议 — 学习笔记

> **文档类型**：技术预研 / 学习笔记
> **创建日期**：2026-05-24
> **参考标准**：IETF RFC 2326 (RTSP 1.0)、RFC 3550 (RTP)、RFC 3551 (RTP Profile)
> **目标**：在 SmartCam 项目 MJPEG-over-HTTP 基础上理解 RTSP，为后续 RTSP 模块开发做准备

---

## 一、什么是 RTSP？

RTSP（Real Time Streaming Protocol，实时流传输协议）是 IETF 制定的一种**应用层协议**，用于控制流媒体服务器。它本身不传输音视频数据，而是扮演「遥控器」的角色——负责建立/控制媒体流，真正的数据由 **RTP**（Real-time Transport Protocol）承载。

```
┌─────────────────────────────────────────────────┐
│                  RTSP 的角色                     │
│                                                 │
│  "遥控器" ── 播放/暂停/快进/停止                │
│  "RTP"    ── 音视频数据流（UDP/TCP）            │
│  "RTCP"   ── 质量反馈（丢包率、抖动、延迟）      │
│  "SDP"    ── 媒体能力描述（编码器、分辨率）      │
└─────────────────────────────────────────────────┘
```

### 1.1 与你已实现的 MJPEG-over-HTTP 的对比

| 维度 | MJPEG-over-HTTP（已有） | RTSP + RTP（学习目标） |
|------|------------------------|----------------------|
| 协议层次 | HTTP over TCP | RTSP(控制) + RTP(数据) + RTCP(反馈) |
| 数据格式 | multipart JPEG 帧 | RTP 封包（时间戳 + 序列号 + 载荷） |
| 播放控制 | ❌ 无（只能看/断） | ✅ PLAY / PAUSE / TEARDOWN |
| 时间戳 | ❌ 无 | ✅ RTP 每包精确到 90kHz 时钟 |
| 客户端 | 浏览器 `<img>` | VLC / ffplay / NVR 录像机 |
| 延迟 | ~200ms | ~50-100ms |
| 丢包容忍 | ❌ TCP 重传（可能卡顿） | ✅ 丢帧继续（UDP），RTCP 反馈 |
| 行业地位 | 取巧做法 | IPC 摄像头行业标准 |
| 传输层 | TCP（可靠但阻塞） | 通常 UDP（低延迟，可选 TCP） |

---

## 二、RTSP 协议详解

### 2.1 协议定位（OSI 模型）

```
OSI 七层模型                    RTSP 栈位置
┌──────────────┐              ┌──────────────────┐
│  7. 应用层    │              │  RTSP (控制)      │
├──────────────┤              │  RTP (数据)       │
│  6. 表示层    │              │  SDP (会话描述)   │
├──────────────┤              │  RTCP (控制/反馈) │
│  5. 会话层    │              └────────┬─────────┘
├──────────────┤                       │
│  4. 传输层    │              ┌────────┴─────────┐
├──────────────┤              │  TCP / UDP        │
│  3. 网络层    │              └────────┬─────────┘
├──────────────┤                       │
│  2. 数据链路层│              ┌────────┴─────────┐
├──────────────┤              │  IP (IPv4/IPv6)   │
│  1. 物理层    │              └──────────────────┘
└──────────────┘
```

**关键理解**：RTSP 的语法和语义类似 HTTP/1.1（文本协议，请求-响应），但它：
- 是有状态的（服务器需维护 session）
- 支持双向（服务器也可以向客户端发请求）
- 数据传输通常在**带外**（separate channel）

### 2.2 协议格式

#### 请求格式

```
方法 URI RTSP版本\r\n
头字段1: 值\r\n
头字段2: 值\r\n
...
\r\n
[可选的消息体，如 SDP]
```

#### 响应格式

```
RTSP版本 状态码 原因短语\r\n
头字段1: 值\r\n
...
\r\n
[可选的消息体]
```

#### 示例：一个完整的 PLAY 对话

```
C → S:  PLAY rtsp://192.168.1.100:8554/stream RTSP/1.0\r\n
        CSeq: 4\r\n
        Session: 12345678\r\n
        Range: npt=0.000-\r\n
        \r\n

S → C:  RTSP/1.0 200 OK\r\n
        CSeq: 4\r\n
        Session: 12345678\r\n
        Range: npt=0.000-\r\n
        RTP-Info: url=rtsp://192.168.1.100:8554/stream;seq=0;rtptime=0\r\n
        \r\n
```

### 2.3 RTSP 方法全集

| 方法 | 方向 | 含义 | 在 SmartCam 中的用途 |
|------|------|------|---------------------|
| **OPTIONS** | C→S | 查询服务器支持哪些方法 | VLC 连接时首先发送 |
| **DESCRIBE** | C→S | 获取媒体描述（SDP） | 告知 VLC 编码器是 MJPEG、分辨率 |
| **SETUP** | C→S | 协商传输参数（端口/协议） | 确定 RTP 用哪个 UDP 端口 |
| **PLAY** | C→S | 开始传输 | 开始推送 MJPEG 帧 |
| **PAUSE** | C→S | 暂停 | 暂停推送 |
| **TEARDOWN** | C→S | 结束会话 | 断开连接，释放资源 |
| **GET_PARAMETER** | C↔S | 查询参数 | 查询帧率/分辨率状态 |
| **SET_PARAMETER** | C↔S | 设置参数 | 远程调节亮度/对比度 |
| **ANNOUNCE** | C→S | 客户端主动告知 SDP | 服务器接收模式（罕见） |
| **RECORD** | C→S | 开始录制 | 服务器录制客户端推流（罕见） |

### 2.4 RTSP 状态机

```
                      ┌─────────┐
                      │  INIT   │ (初始)
                      └────┬────┘
                           │ SETUP
                           ▼
                      ┌─────────┐
             ┌───────│  READY  │◄──────┐
             │       └────┬────┘       │
             │            │ PLAY       │ PAUSE
             │            ▼            │
             │       ┌─────────┐      │
             │       │ PLAYING │──────┘
             │       └────┬────┘
             │            │ TEARDOWN
             │            ▼
             │       ┌─────────┐
             └──────►│  INIT   │
         TEARDOWN    └─────────┘

状态说明:
  INIT:    无 RTP 传输通道
  READY:   SETUP 完成，已分配端口，等待播放
  PLAYING: 正在传输 RTP 流

SmartCam 实现:
  你只需要 INIT → READY → PLAYING 的线性状态。
  PAUSE 在 MJPEG 直播场景可选（暂停 = 停止推帧即可）。
```

---

## 三、RTP — 实时传输协议（RFC 3550）

### 3.1 RTP 是什么？

RTP 是传输层协议（通常跑在 UDP 之上），负责在互联网上传输实时数据。它解决的核心问题：

| 问题 | RTP 的解法 |
|------|-----------|
| **网络丢包** | 序列号 → 检测丢失的包 |
| **网络抖动** | 时间戳 → 接收端重建播放时序 |
| **多路复用** | SSRC → 区分多个音视频源 |
| **组播** | RFC 3550 原生支持组播地址 |
| **帧边界** | Marker 位 → 标记帧的最后一包 |

### 3.2 RTP 固定头结构（12 字节）

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|X|  CC   |M|     PT      |        序列号 (16 bits)       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          时间戳 (32 bits)                      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          SSRC 标识符 (32 bits)                 |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           CSRC 列表 (0 ~ 15 项, 每项 32 bits)                  |
|                         ...                                   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| 字段 | 大小 | 含义 | 在 MJPEG 中的值 |
|------|------|------|----------------|
| V (版本) | 2 bits | 协议版本，固定 2 | 2 |
| P (填充) | 1 bit | 尾部是否有填充字节 | 0 |
| X (扩展) | 1 bit | 是否有扩展头 | 0 |
| CC (CSRC 数) | 4 bits | CSRC 列表项数 | 0 |
| **M (标记位)** | 1 bit | **帧结束标志** | 每 JPEG 最后一包 = 1 |
| **PT (载荷类型)** | 7 bits | 编码器类型 | **26 (JPEG)** |
| **序列号** | 16 bits | 每包递增，检测丢包 | 0..65535 循环 |
| **时间戳** | 32 bits | 采样时刻 | 基于 90kHz 时钟（JPEG 默认） |
| SSRC | 32 bits | 同步源标识（区分流） | 随机生成 |
| CSRC | 可变 | 贡献源列表 | 空 |

### 3.3 RTP 载荷：JPEG 封包 (RFC 2435)

MJPEG 封装到 RTP 需要特殊的 JPEG 专有头：

```
RTP 封包结构（JPEG 载荷）
┌────────────────────────────────────────────────────────────┐
│ RTP 固定头 (12 bytes)                                      │
├────────────────────────────────────────────────────────────┤
│ JPEG 专有头 (8 bytes)                                      │
│  ┌──────────────────────────────────────────────────────┐ │
│  │ 类型代码 (8) │ 片段偏移 (24) │ 类型 (8) │ Q (8)     │ │
│  │ 宽度 (8)    │ 高度 (8)      │         │           │ │
│  └──────────────────────────────────────────────────────┘ │
├────────────────────────────────────────────────────────────┤
│ JPEG 量化表 (可选, 可变长)                                  │
├────────────────────────────────────────────────────────────┤
│ JPEG 扫描数据（实际图像数据）                                │
└────────────────────────────────────────────────────────────┘
```

**对于你的项目（MJPEG 来自摄像头硬件）**：

- 如果一帧 JPEG < 1460 字节（单个 UDP 包能装下），直接放一包
- 如果一帧 > 1460 字节（640×480 MJPEG 约 40KB），需要**分片**：
  - 包 1: Fragment Offset = 0, M = 0
  - 包 2: Fragment Offset = 1460, M = 0
  - ...
  - 包 N: Fragment Offset = 最后一个片段的起始, M = 1（帧结束）

### 3.4 RTP 时间戳

```
时间戳时钟频率:
  JPEG 视频: 90,000 Hz（标准）
  H.264:    90,000 Hz
  G.711 音频: 8,000 Hz

实际运算:
  Frame #1 时间戳: 0
  Frame #2 时间戳: 0 + (90000 / fps) = 3000  (30fps)
  Frame #3 时间戳: 3000 + 3000 = 6000
  ...

作用:
  接收端根据时间戳差值重建播放节奏，消除网络抖动。
  即使包 3 比包 2 晚到 200ms，播放器也能按 3000 的间隔均匀渲染。
```

---

## 四、RTCP — 实时控制协议

### 4.1 作用和包类型

RTCP 周期性发送（通常每 5 秒），占带宽的 5%，用于质量反馈：

| RTCP 包类型 | 编号 | 含义 | 提供的信息 |
|-----------|------|------|-----------|
| **SR** (Sender Report) | 200 | 发送者报告 | 已发送字节数/包数、NTP 时间戳 ↔ RTP 时间戳映射 |
| **RR** (Receiver Report) | 201 | 接收者报告 | 丢包率、累计丢包数、最高序列号、到达间隔抖动 |
| **SDES** (Source Description) | 202 | 源描述 | CNAME（规范名称，唯一标识一个参与者） |
| **BYE** | 203 | 结束 | 通知离开（含离开原因字符串） |
| **APP** | 204 | 自定义 | 应用特定扩展 |

### 4.2 SR (Sender Report) 详解

```
SR 包提供两个关键映射:
  1. NTP 时间戳（墙上时钟） ↔ RTP 时间戳（媒体时钟）
  2. 累计发送字节数和包数

示例：
  发送了 300 帧（每帧 40KB 约 10 个 RTP 包）
  → 累计包数 = 3000
  → 累计字节数 = 300 × 40000 = 12 MB
  → 接收端据此计算实际带宽

接收端结构体:
  struct {
      uint32_t ssrc;                 // 谁的发送者报告
      uint64_t ntp_timestamp;        // NTP 时间戳（64 bit）
      uint32_t rtp_timestamp;        // 对应的 RTP 时间戳
      uint32_t sender_packet_count;  // 累计发送包数
      uint32_t sender_octet_count;   // 累计发送字节数
  };
```

### 4.3 RR (Receiver Report) 详解

```
RR 提供三段关键信息:

1. 丢包率 (fraction_lost)
   ── 从上次报告以来，丢失比例 × 256
   例: fraction_lost = 10 → 丢失 = 10/256 ≈ 3.9%

2. 累计丢包数 (cumulative_packets_lost)
   ── expected - received 的总和
   例: expected=1000, received=985 → lost=15

3. 到达间隔抖动 (interarrival_jitter)
   ── 连续包到达时间差的标准差（以时间戳单位表示）
   公式: J(i) = J(i-1) + (|D(i-1,i)| - J(i-1))/16
   值越大 → 网络越不稳定

VLC 使用这些信息决定是否降低码率或缓冲。
```

### 4.4 精简策略（对你们项目）

对于智能相机项目，RTCP 可以精简到最小：

```
必须实现: SR (发送者报告)
  理由: VLC 期望看到 SR 来同步时间戳

可以省略: RR 解析
  理由: 摄像头只是发送端，不需要根据接收端反馈调整

必须实现: SDES (CNAME)
  理由: VLC 通过 CNAME 识别服务器身份

可以省略: BYE / APP
  理由: TEARDOWN 方法已足够通知会话结束
```

---

## 五、SDP — 会话描述协议（RFC 4566）

### 5.1 什么是 SDP？

SDP 是文本格式的「媒体能力名片」，在 RTSP 的 DESCRIBE 响应中传输。它告诉客户端：这个流里有什么、用什么编码、是什么分辨率。

### 5.2 完整示例（MJPEG 流）

```
v=0
o=- 1234567890 1 IN IP4 192.168.1.100
s=SmartCam Live Stream
i=SmartCam MJPEG RTSP Stream
c=IN IP4 0.0.0.0
t=0 0
a=control:*
a=range:npt=0-
m=video 0 RTP/AVP 26
a=control:track0
a=rtpmap:26 JPEG/90000
a=fmtp:26 width=640;height=480
a=framerate:30.0
```

### 5.3 逐行解读

| 行 | 字段 | 值 | 含义 |
|----|------|----|------|
| `v=0` | 版本 | 0 | SDP 协议版本（永远是 0） |
| `o=- 1234567890 1 IN IP4 192.168.1.100` | 会话发起者 | username, sess-id, sess-version, net-type, addr-type, addr | 唯一标识一个会话 |
| `s=SmartCam Live Stream` | 会话名称 | 自由文本 | 播放器标题栏显示 |
| `i=...` | 会话描述 | 自由文本 | 可选的额外信息 |
| `c=IN IP4 0.0.0.0` | 连接信息 | 网络类型, 地址类型, 地址 | RTP 数据的目标 IP |
| `t=0 0` | 时间 | 开始, 结束 | 0 0 表示永久有效 |
| **`m=video 0 RTP/AVP 26`** | **媒体描述** | 媒体类型, 端口, 传输协议, 载荷类型 | **最关键一行** |
| `a=control:track0` | 轨道标识 | URI | 用于 SETUP 时指定轨道 |
| **`a=rtpmap:26 JPEG/90000`** | **编码器映射** | PT, 编码名, 时钟频率 | **26 = JPEG, 90kHz 时钟** |
| **`a=fmtp:26 width=640;height=480`** | **格式参数** | 分辨率 | **640×480** |
| `a=framerate:30.0` | 帧率 | float | 30fps（非标准属性，部分播放器支持） |

### 5.4 m= 行的含义

```
m=video 0 RTP/AVP 26
  │     │  │      └─ PT 值（26 = JPEG）
  │     │  └─ RTP/AVP = RTP over UDP
  │     └─ 端口号（0 = SETUP 时协商）
  └─ 媒体类型（video / audio / application）
```

端口为 0 表示「SETUP 时协商」，即客户端在 SETUP 请求中通过 `Transport` 头告知自己的接收端口，服务器在响应中确认。

---

## 六、完整交互流程（时序图）

```
客户端 (VLC/ffplay)                    SmartCam RTSP Server (你们的设备)
      │                                           │
      │  TCP 连接 (port 8554)                      │
      │───────────────────────────────────────────▶│ accept()
      │                                           │
      │  1️⃣  OPTIONS rtsp://192.168.1.100:8554    │
      │───────────────────────────────────────────▶│ 返回支持的方法列表
      │  ◀── 200 OK                                │
      │      Public: DESCRIBE,SETUP,PLAY,TEARDOWN  │
      │                                           │
      │  2️⃣  DESCRIBE rtsp://192.168.1.100:8554   │
      │───────────────────────────────────────────▶│ 组装 SDP
      │  ◀── 200 OK                                │ 返回上面那份 SDP
      │      Content-Type: application/sdp         │
      │      [SDP 体]                              │
      │                                           │
      │  3️⃣  SETUP rtsp://.../track0              │
      │      Transport: RTP/AVP;unicast;           │
      │        client_port=5000-5001               │
      │───────────────────────────────────────────▶│ 记录客户端 RTP 端口
      │  ◀── 200 OK                                │ 创建 session
      │      Session: 12345678                     │
      │      Transport: RTP/AVP;unicast;           │
      │        client_port=5000-5001;              │
      │        server_port=6000-6001               │
      │                                           │
      │  4️⃣  PLAY rtsp://192.168.1.100:8554       │
      │      Session: 12345678                     │
      │      Range: npt=0-                         │
      │───────────────────────────────────────────▶│ 开始推流
      │  ◀── 200 OK                                │
      │      RTP-Info: url=...;seq=0;rtptime=0    │
      │                                           │
      │  ╔═══════════════════════════════════════╗ │
      │  ║        RTP 数据流 (UDP)               ║ │
      │  ║  ┌─[seq=0,ts=0,    M=0, frag0]─┐     ║ │
      │  ║  ├─[seq=1,ts=0,    M=0, frag1]─┤     ║ │ 采集线程每帧
      │  ║  ├─[seq=2,ts=0,    M=1, frag2]─┤     ║ │ feedFrame() →
      │  ║  ├─[seq=3,ts=3000, M=0, frag0]─┤     ║ │ RTP 分片 + UDP send
      │  ║  ├─[seq=4,ts=3000, M=0, frag1]─┤     ║ │
      │  ║  ├─[seq=5,ts=3000, M=1, frag2]─┤     ║ │
      │  ║  ├─[seq=6,ts=6000, M=0, frag0]─┤     ║ │
      │  ║  └─ ...                         ┘     ║ │
      │  ╚═══════════════════════════════════════╝ │
      │                                           │
      │  ╔═══════════════════════════════════════╗ │
      │  ║        RTCP 反馈 (每 5s)              ║ │
      │  ║  ◀──── SR (sender report)             ║ │ 发送字节/包统计
      │  ║  ────▶ RR (receiver report)           ║ │ (VLC 可选的)
      │  ╚═══════════════════════════════════════╝ │
      │                                           │
      │  5️⃣  TEARDOWN rtsp://...                  │
      │      Session: 12345678                     │
      │───────────────────────────────────────────▶│ 释放 session
      │  ◀── 200 OK                                │ 关闭 RTP socket
      │                                           │
      │  TCP 断开                                  │
      │◀──────────────────────────────────────────│
```

---

## 七、与 SmartCam 现有架构的对接

### 7.1 数据流对比

```
当前架构（MJPEG-over-HTTP）:
  采集线程                                  HTTP Server
     │                                         │
     │── updateFrame(jpeg, len) ──────────────►│
     │                                         │── write(client_fd, "multipart...")
     │                                         │    (条件变量通知所有客户端)
     │                                         │
  每帧: 1 次 memcpy + 1 次 notify_all + N 次 write

RTSP 架构（计划）:
  采集线程                                  RTSP Server
     │                                         │
     │── feedFrame(jpeg, len) ────────────────►│ (仅 1 次传递)
     │                                         │── RTP 分片 (每帧 ~10 包)
     │                                         │── sendto(udp_sock, ...) × 10
     │                                         │    (不阻塞, UDP sendto 直接走)
     │                                         │
  每帧: 1 次 memcpy + 1 次分片 + N 次 sendto

关键区别:
  RTSP 使用 UDP → 不阻塞、低延迟、允许丢包
  MJPEG-HTTP 使用 TCP → 可靠但可能卡等待
```

### 7.2 接口设计（预览）

```cpp
// SmartCam 的 RTSP 服务器接口（与现有 MJPEGStreamServer 对称）
class RTSPServer {
public:
    RTSPServer();
    ~RTSPServer();

    // ---- 生命周期 ----
    int  start(int port = 8554);    // TCP: RTSP 控制, 类似 mjpegServer->start()
    void stop();

    // ---- 推帧 ----
    void feedFrame(const uint8_t* jpeg_data, size_t len);
    // → 内部: RTP 分片 → sendto() 到所有已 SETUP 的客户端

    // ---- 状态 ----
    bool isRunning() const;
    int  clientCount() const;

private:
    void eventLoop();               // TCP accept + RTSP 请求解析
    void handleDescribe(int fd);    // 组装 SDP
    void handleSetup(int fd);       // 记录 RTP/RTCP 端口
    void handlePlay(int fd);        // 开始推送 RTP 流
    void rtpSendLoop();             // 定时发送 RTP + RTCP
};
```

### 7.3 与 main.cpp 的集成点

```cpp
// 与 MJPEGStreamServer 完全对称的初始化:
rtspServer = new RTSPServer();
rtspServer->start(8554);

// 采集线程中，同一帧同时推两个目标:
// MJPEG:  mjpegServer->updateFrame(data, len);  → HTTP 浏览器
// RTSP:   rtspServer->feedFrame(data, len);      → VLC 播放器
```

---

## 八、你已有的优势（可以复用）

| 现有组件 | 在 RTSP 中的复用 |
|---------|-----------------|
| `control.h/cpp` (TCP 服务器 + epoll) | RTSP 的 TCP 控制连接完全相同套路 |
| `mjpeg_server.h/cpp` (条件变量广播) | RTSP 也需要采集线程推送帧到多客户端 |
| `processor.h` (findJPEGFrame) | RTP JPEG 封包前需要确认帧边界 |
| `logger.h` | 协议解析日志 |
| `ringbuf.h` | RTP 包缓冲队列 |

**核心区别只有一个**：把 `write(client_fd, multipart_header + jpeg)` 替换成 RTP 分片 + UDP `sendto()`。

---

## 九、实现路线图建议

### Phase 1：最小可用（1 天）

```
目标: VLC 能播放 rtsp://设备IP:8554/stream

实现:
  1. TCP 服务器 (复用 control.cpp 的 epoll 框架)
  2. OPTIONS 处理器 (返回公共方法列表)
  3. DESCRIBE 处理器 (返回写死的 SDP)
  4. SETUP 处理器 (记录客户端 UDP 端口)
  5. PLAY 处理器 (开始推送 RTP)
  6. RTP 固定头封装 (12 字节 struct)
  7. RTP JPEG 封包 (RFC 2435, 分片逻辑)
  8. sendto() UDP 发送

约 300-400 行代码
```

### Phase 2：完善（半天）

```
  9. TEARDOWN 处理器
  10. RTCP SR (Sender Report) 发送
  11. PAUSE 处理器
  12. 多客户端支持
  13. 完整错误处理 (400/404/500)
```

### Phase 3：进阶（按需）

```
  14. TCP 交错传输 (RTP over RTSP TCP, 穿透防火墙)
  15. RTCP RR 解析与自适应码率
  16. H.264 编码器支持 (需要硬件编码器或软编码)
  17. ONVIF Profile S 兼容层
```

---

## 十、参考资料

| 文档 | 链接/编号 | 说明 |
|------|----------|------|
| RTSP 1.0 | RFC 2326 | 基础协议规范 |
| RTSP 2.0 | RFC 7826 | 新版协议（VLC 也支持了） |
| RTP | RFC 3550 | RTP/RTCP 基础 |
| RTP JPEG | RFC 2435 | MJPEG 载荷格式（你的核心参考） |
| SDP | RFC 4566 | 会话描述格式 |
| Live555 | [live555.com](http://www.live555.com/) | 最常用的 C++ RTSP 库 |
| ffmpeg libavformat | RTSP muxer | 另一个实现参考 |

### 关键 RFC 章节速查

```
RFC 2326:
  §10.1  OPTIONS    →  Public 头
  §10.4  DESCRIBE   →  SDP 体
  §10.6  SETUP      →  Transport 头
  §10.7  PLAY       →  Range 头
  §10.11 TEARDOWN   →  清理 session
  §12    Header Fields → CSeq, Session 等

RFC 3550:
  §5.1   RTP Fixed Header    → 你的 struct 定义
  §5.3   RTP Header Extension
  §6.4   SR: Sender Report   → 发送者报告格式
  §6.4.1 RR: Receiver Report

RFC 2435:
  §3     JPEG Payload Format    → 你的 RTP JPEG 头
  §3.1   JPEG Header            → 8 字节专有头详解
  §4     Fragmentation Rules    → 分片规则
```

---

## 十一、变更记录

| 日期 | 变更内容 |
|------|----------|
| 2026-05-24 | 初始创建：RTSP/RTP/RTCP/SDP 协议全解，与现有 MJPEG-HTTP 对比，SmartCam 集成预研 |
