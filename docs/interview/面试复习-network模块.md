# src/network/ 模块面试复习指南（网络服务）

> 定位：以「资深嵌入式网络与流媒体协议面试官 + 应聘者」双视角，对 `src/network/` 三个独立网络服务（`mjpeg_server.cpp` / `rtsp_server.cpp` / `control.cpp`）做**逐函数级**拆解，全程结合代码。
> 阅读前建议先通读 `src/network/*.cpp`、`include/network/*.h`，并对照 `src/main.cpp` 的线程编排（RTSP/控制线程、处理线程推流调用点）、`configs/smartcam.service`（systemd 集成）、`docs/learn/03-mjpeg-stream-module-implementation.md`、`docs/learn/05-control-module-implementation.md`、`docs/learn/08-rtsp-module-implementation.md`、`docs/debug-summary.md`（网络排障）。
>
> 组织方式：**模块全景（概览）→ 三个服务分模块详解（功能概述 + 关键实现 + 追问应答）→ 综合追问（攻防演练）→ 综合思辨（系统观）**。

---

## 目录

1. [第一部分 模块全景（概览）](#第一部分-模块全景概览)
   - 1.1 三个服务与端口共存
   - 1.2 线程模型对比（每客户端一线程 vs epoll 单线程）
   - 1.3 数据流路径与拷贝分析
   - 1.4 模块接口与解耦
2. [第二部分 MJPEG-over-HTTP（mjpeg_server.cpp）](#第二部分-mjpeg-over-httpmjpeg_servercpp)
   - 2.1 功能概述
   - 2.2 关键实现
   - 2.3 追问与应答
3. [第三部分 RTSP/RTP（rtsp_server.cpp）](#第三部分-rtsprtprtsp_servercpp)
   - 3.1 功能概述
   - 3.2 关键实现
   - 3.3 追问与应答
4. [第四部分 TCP 控制协议（control.cpp）](#第四部分-tcp-控制协议controlcpp)
   - 4.1 功能概述
   - 4.2 关键实现
   - 4.3 追问与应答
5. [第五部分 面试攻防演练（综合追问）](#第五部分-面试攻防演练综合追问)
6. [第六部分 深度思考与横向关联（系统观）](#第六部分-深度思考与横向关联系统观)
7. [一句话总结](#一句话总结)

---

# 第一部分 模块全景（概览）

## 1.1 三个服务与端口共存

| 服务 | 文件 | 协议 | 默认端口 | 承载内容 |
|------|------|------|---------|---------|
| MJPEG-over-HTTP | `mjpeg_server.cpp` | HTTP 1.0 + multipart | **8080** | 浏览器直看 `/stream`、单帧 `/snapshot`、状态 `/status`、首页 `/` |
| RTSP/RTP | `rtsp_server.cpp` | RTSP 1.0（RFC 2326）+ RTP（RFC 3550/2435） | **8554** | VLC/ffplay 标准播放器拉流 |
| TCP 控制 | `control.cpp` | 自定义二进制协议 | **9000** | 远程拍照/录像/参数/状态/心跳 |

端口由命令行或配置文件注入（`main.cpp:298-308`：`http_port`/`control_port`/`rtsp_port`，优先级命令行 > 配置 > 默认）。三个服务互不抢占端口，**共享同一个 `INADDR_ANY`，无端口冲突**。

**三个服务如何共存？** 由 `main.cpp` 统一编排：MJPEG 服务在主线程直接 `start()`（内部自建 accept 线程 + 每客户端线程）；RTSP 和控制各占一个**独立 `std::thread`**（阻塞式事件循环）：

```cpp
// main.cpp:712-716  RTSP 独立线程
rtspThread = new std::thread([rtspServer, rtspPort]() {
    rtspServer->start(rtspPort);   // 内部 epoll 事件循环，阻塞
});
// main.cpp:859-862  控制独立线程
controlThread = new std::thread([controlSrv, ctrlPort]() {
    controlSrv->start(ctrlPort);   // 内部 epoll 事件循环，阻塞
});
```

**线程总数**：Qt GUI 主线程 + 采集线程 + 处理线程 + 解码线程 + RTSP 线程 + 控制线程 = 6 个固定线程，**外加 MJPEG 的动态线程**（1 个 accept 线程 + N 个客户端线程）。

## 1.2 线程模型对比

| 维度 | MJPEG（accept + 每客户端一线程） | RTSP（epoll ET 单线程） | 控制（epoll ET 单线程） |
|------|--------------------------------|------------------------|------------------------|
| 连接处理 | 1 accept 线程 + N 客户端线程（detach） | 1 事件循环线程 | 1 事件循环线程 |
| I/O 模式 | 客户端线程**阻塞**在条件变量上 | 非阻塞 + epoll_wait | 非阻塞 + epoll_wait |
| 数据方向 | 服务器**持续推送**（一帧接一帧） | 信令请求-响应 + UDP 推送 | 命令请求-响应 |
| 客户端数上限 | 受线程/内存限制（低） | 可支撑数百连接（高） | 可支撑数百连接（高） |

**为什么 MJPEG 用每客户端一线程？**
1. **业务形态是"推流"**：每个客户端连上后，服务器就持续往这个 TCP 连接推 multipart 帧，客户端几乎不发数据（只在开始时发一个 HTTP GET）。每个连接是一个**长时间运行、方向固定**的流——用线程 `while(m_running) { wait_frame; write; }` 直白自然。
2. **条件变量广播天然贴合**：采集侧 `updateFrame()` 写帧后 `notify_all()`，所有客户端线程从阻塞中醒来取最新帧。每客户端一线程时，每线程只需在锁内 `frame.assign(m_currentFrame)` 拷一份自己的帧，**没有 fd 集合管理、没有事件循环状态机**，代码量小、正确性易保证。
3. **客户端数据少，不需要 epoll 的事件驱动优势**：epoll 的收益在于"大量 fd、事件稀疏、需要复用线程"。MJPEG 场景每个连接都是持续占用，事件模型反而复杂。

**为什么 RTSP/控制用 epoll 而非每客户端一线程？**
1. **信令是"请求-响应"**：客户端发一个命令、服务器回一个响应，是**稀疏、突发、低频**的交互。用单线程 epoll 可以同时管理几十个空闲连接，事件来了才处理——**空闲连接零线程开销**。
2. **控制/信令连接生命周期短**（RTSP 会话虽长，但信令交互稀疏；控制命令突发），epoll 的"一个线程管所有"避免线程风暴。
3. 真正的视频数据走 **UDP sendto()**，根本不占 epoll——RTSP 线程只负责信令 TCP 与周期 RTCP，负载很轻。

> **一句话权衡**：**"持续推送型"用每客户端线程（简单、直白），"突发请求-响应型"用 epoll（省线程、可扩展）**。这是嵌入式场景"按业务形态选模型"的经典判断。

## 1.3 数据流路径与拷贝分析

**整条链路（MJPEG 模式，摄像头硬件直出 JPEG）**：

```
摄像头 DMA → V4L2 mmap（零拷贝）
  → 采集线程：assign 深拷贝 → g_state.frameData（拷贝①）
  → 处理线程：localFrame = g_state.frameData（拷贝②，锁外）
      ├─ hasHttpViewer → mjpegServer->updateFrame → m_currentFrame.assign（拷贝③）
      │     └─ 每客户端线程：frame.assign(m_currentFrame)（拷贝④/客户端）
      │            └─ write() → TCP → 浏览器
      ├─ hasRtspViewer → rtspServer->feedFrame → m_latestJpeg.assign（拷贝③'）
      │     └─ 锁外 rtpSendFrame：组 pkt vector（拷贝④'）→ sendto → UDP
      └─ g_recording → writeRecordFrame（fwrite 直写，无额外拷贝）
```

**结论：不是零拷贝**，全链路每帧约 **4~5 次 memcpy**（采集→状态、状态→处理、处理→服务内部、服务→客户端）。但每个拷贝都有其**存在理由**：
- 拷贝①②：采集线程要尽快归还 V4L2 mmap 缓冲（否则丢帧），必须深拷贝脱离 mmap 生命周期；
- 拷贝③：服务内部保留"最新帧"，让客户端线程在锁外安全使用；
- 拷贝④：客户端线程拿到**自己的副本**，才能与别的客户端线程互不干扰地 write。

**关键优化**（`main.cpp:984-992`）——**无人观看零开销**：

```cpp
const bool hasHttpViewer = mjpegServerOk && mjpegServer->clientCount() > 0;
const bool hasRtspViewer = rtspServer && rtspServer->clientCount() > 0;
if (!hasHttpViewer && !hasRtspViewer && !g_recording) {
    continue;   // 无人消费，直接跳过深拷贝/编码/推流，给单核 CPU 减负
}
```

**编码是否共享**：
- MJPEG 模式：摄像头硬件直出 JPEG，**HTTP 与 RTSP 推的是同一份 JPEG 字节**（各拷一份，但**不重复编码**）；
- YUYV 模式：处理线程每帧调用一次 `encodeYUYVtoJPEG()`（`main.cpp:1011-1015`），编码结果**同时喂给 HTTP 和 RTSP**——**一次编码、多方复用**；
- MJPEG 的 `?quality=N` 重编码是**例外**：仅当有客户端请求 quality<100 才额外编一次，且**多客户端同 quality 共享一份缓存**（`m_qualityCache`）。

## 1.4 模块接口与解耦

| 服务 | 对外接口 | 谁调用 | 数据方向 |
|------|---------|--------|---------|
| MJPEG | `updateFrame(data,len)` | 处理线程 | 推帧（拷贝入内） |
| MJPEG | `setStatusProvider(cb)` | main 主线程 | 回调取状态 |
| RTSP | `feedFrame(data,len,w,h)` | 处理线程 | 推帧（拷贝入内） |
| RTSP | `setStreamInfo(w,h,fps)` | main 主线程 | 配置流参数 |
| 控制 | `setCommandHandler(cmd, cb)` / `setStatusProvider(cb)` | main 主线程 | 注册命令回调 |

**解耦设计**：
- 三个服务**不直接依赖采集/存储模块**——它们只收 `uint8_t*` 字节流（JPEG），"数据从哪来"由 main.cpp 的处理线程统一喂入；
- 控制协议的**命令动作**（拍照/录像/设置参数）通过**函数表注册回调**注入，`ControlServer` 完全不认识 `StorageManager`/`CameraCapture`；
- MJPEG 的 `/status` 通过 `std::function` 回调反向拉取设备状态——服务与业务彻底解耦，**"推流只认字节，控制只认回调"**。

---

# 第二部分 MJPEG-over-HTTP（mjpeg_server.cpp）

## 2.1 功能概述

一个极简嵌入式 HTTP 服务器，支持 4 个路由：

| 路由 | 行为 |
|------|------|
| `GET /` | 返回内嵌 HTML 播放页（`<img src='/stream'>`） |
| `GET /stream[?quality=N]` | **MJPEG 无限流**（multipart/x-mixed-replace） |
| `GET /snapshot` | 单帧 JPEG（无帧时 503） |
| `GET /status` | JSON 设备状态（通过回调获取） |

架构：`start()` 建监听 socket → `acceptLoop` 线程 accept → 每连接 `clientHandler` 线程（**detach**）→ 采集侧 `updateFrame()` 推帧 → 条件变量广播 → 客户端线程取最新帧写 socket。

## 2.2 关键实现

### 2.2.1 multipart 机制：`multipart/x-mixed-replace`

`sendHttpHeader`（`mjpeg_server.cpp:551-568`）先发一个"永不结束"的 HTTP 响应头：

```http
HTTP/1.0 200 OK
Content-Type: multipart/x-mixed-replace; boundary=SmartCamFrame
Cache-Control: no-cache, no-store, must-revalidate
Connection: close
```

之后每一帧 `sendMJPEGFrame`（`574-614`）发一个 part：

```
--SmartCamFrame\r\n
Content-Type: image/jpeg\r\n
Content-Length: 12345\r\n
\r\n
[JPEG 二进制数据]\r\n
```

**边界字符串（`kBoundary = "SmartCamFrame"`）的作用**：`multipart/x-mixed-replace` 的核心是"一个响应体里可以替换多个 part"。浏览器/客户端靠**边界分隔符**识别每个 part 的起止——看到 `--SmartCamFrame` 就知道新的一帧开始了。**每次推送新 part，浏览器就用新内容替换 `img` 元素里的旧图**，视觉上就是"视频"。边界串选无歧义字符串，避免与 JPEG 二进制数据中的内容混淆（`Content-Length` 同时辅助定界）。

**为什么能"无限推送"**：HTTP/1.0 + `Connection: close` 但**不关闭连接**——服务器在同一个 TCP 连接上持续写 part，客户端持续解析，直到连接断开。这是 MJPEG 流的标准做法（老式网络摄像头/家用监控均如此）。

### 2.2.2 JPEG 质量重编码（`?quality=N`）

`reencodeJpegQuality`（`43-124`）实现"解码→重编码"：

```cpp
if (quality >= 100) {
    // quality=100 → 直接返回原数据（零开销直通）
    *dst = malloc(srcLen); memcpy(*dst, src, srcLen); *dstLen = srcLen;
    return 0;
}
// quality<100: jpeg_read_header → jpeg_start_decompress → 逐行读 RGB24
//              → jpeg_set_quality(quality) → 逐行写 JPEG
```

**设计考量**：
- **quality=100 直通**：MJPEG 模式摄像头硬件直出 JPEG，本就走"零编码"路径；quality=100 时解码再编码纯属浪费（i.MX6ULL 上解码 ~25ms + 编码 ~25ms = 每帧 50ms，30fps 根本跑不动）。直通让默认路径**零额外开销**；
- **quality<100 才重编码**：目的是**降低带宽**（低画质 → 小体积 JPEG → 小带宽），适合弱网客户端/低分辨率场景。这是"牺牲画质换带宽"的显式选项，**默认不开启**；
- **按需预生成 + 多客户端共享**（`updateFrame` 的 `263-298`）：`updateFrame` 时收集所有活跃客户端的 quality，`std::map<int,bool> needed`，**每个需要的 quality 只重编码一次**存入 `m_qualityCache`；客户端线程发帧时从缓存取（`479-484`），**不再重复计算**；无客户端使用的缓存条目自动清理。

**性能代价**：i.MX6ULL（Cortex-A7 792MHz）上 libjpeg-turbo 640x480 解码 ~25ms、编码 ~25ms，**重编码一帧 ~50ms**——这决定了 quality<100 的流帧率会被拉到 ~15-20fps 以下。所以该功能**只能用于"低分辨率低帧率弱网预览"**，不能用于主推流路径。

### 2.2.3 客户端生命周期管理

- `addClient`（`843-858`）：置 `O_NONBLOCK`、`std::thread(...).detach()`、`m_clients` 记录 `{fd, active, lastSentIndex, quality}`；
- 客户端线程退出路径：`sendMJPEGFrame` 返回 false（write 失败/对端断开）→ 退出循环 → `removeClient`（`860-869`）：加锁、`close(fd)`、`erase`；
- **延迟清理的真相**（`cleanupDisconnected`，`871-873`）：

```cpp
void MJPEGStreamServer::cleanupDisconnected() {
    // 标记客户端不活跃后延迟清理（当前简化：在移除时立即清理）
}
```

**为什么设计上要"延迟"而不是"立即"？** 客户端线程正在 `write()` 时，如果主线程同时 `close(fd)` 并 `erase`，会产生**竞态**：fd 被关闭后可能被系统**重新分配给新连接**，旧线程的 write 就可能写进新连接的 socket（灾难）。安全的做法是"标记不活跃 → 等线程自然退出 → 再真正清理"。

**当前实现为什么敢"立即清理"？** 因为 `removeClient` 只在**客户端线程自己**的退出路径上调用（该线程已停止使用 fd），`close` 是"自己关自己的 fd"，天然无竞态。真正的隐患点在于 `stop()`（`223-232`）：主线程直接关闭所有 `m_clients` 的 fd——此时客户端线程可能正在 write。代码的缓解是：先 `notify_all()` 让所有客户端线程退出循环，再 `join` accept 线程，最后关 fd。**但客户端线程是 detached 的，无法 join**——这里"关 fd 打断正在 write 的线程"是**可接受的风险点**（write 返回 EBADF 即退出，不会写错对象，因为 fd 已从监听侧关闭不再复用）。面试时如实讲清这个边界。

### 2.2.4 HTML 页面服务（`GET /`）

`sendIndexPage`（`744-837`）返回一个内嵌 `<style>` 的完整 HTML 页：`<img src='/stream'>` 实时预览 + 状态点 + API 链接（snapshot/status/quality=50）。

**价值**：**"零客户端安装"**——用户浏览器打开 `http://ip:8080/` 直接看画面，无需 VLC、无需写代码。这对演示（求职展示）、调试、手机端查看意义重大。内嵌单文件 HTML 也**零额外静态资源**，符合嵌入式"一个二进制 + 一个端口"的极简哲学。

## 2.3 追问与应答

【面试官追问】"10 个客户端同时观看，线程总数是多少？内存压力多大？"

> 【理想应答】线程数 = 1（accept）+ 10（客户端）= 11 个。每个线程默认栈 8MB **虚拟**地址空间（实际按页使用，通常几十 KB），10 个客户端线程的实际开销约 1MB 内 + 每帧 10 次拷贝。**真正的瓶颈不是线程本身，而是每帧 N 次 memcpy + N 次 socket write 的 CPU/带宽放大**：640x480 MJPEG 单帧 ~50KB，10 客户端 = 每帧 500KB 写量，10fps = 5MB/s 出站带宽 + 5 次用户态拷贝，对 792MHz 单核是明显负担。若要支撑更多客户端，应改用 epoll 事件循环 + 每连接输出队列（send 用 EPOLLOUT 驱动）。

【面试官追问】"为什么没有用 epoll 统一管理 MJPEG 的所有客户端连接？"

> 【理想应答】见 1.2。核心是**业务形态**：MJPEG 是"每连接持续推送"，客户端几乎不发数据；epoll 的优势场景是"大量空闲 fd + 稀疏事件 + 单线程复用"。MJPEG 每个连接都持续占用，事件模型省不下线程，反而引入"每连接发送队列 + EPOLLOUT 管理"的复杂度。用每客户端线程 + 条件变量，代码直观且符合推送语义。**何时该改 epoll**：客户端数量级上百、或需要精细的背压控制（慢客户端不拖累快客户端）时——届时每客户端一个发送队列 + 单线程 epoll 是标准演进。

【面试官追问】"慢客户端（网速慢）会不会拖慢整个服务？"

> 【理想应答】**会，这是当前实现的真实短板**。客户端线程在 `write()` 大帧时若 TCP 发送缓冲满会阻塞（虽然设了 O_NONBLOCK，但 `sendMJPEGFrame` 循环 write 返回 EAGAIN 时直接返回 false → 断开该客户端，**没有排队重试**）。也就是说：慢客户端要么被断开，要么占用自己的线程阻塞——**不会阻塞别的客户端线程**（各自独立），但会**浪费一个线程**。真正的风险是"对端接收窗口为 0 时 `write` 返回 EAGAIN，我们的代码判定为发送失败直接断开"——慢客户端会被粗暴踢掉。改进方向：写失败时等待可写事件（poll EPOLLOUT）或增加发送队列。

【面试官追问】"HTTP/1.0 和 HTTP/1.1 在这个服务里有什么区别？"

> 【理想应答】本项目固定 `HTTP/1.0 + Connection: close`。HTTP/1.1 默认 `keep-alive`，浏览器会复用连接发多个请求，而我们的流式响应需要"连接只服务一个 /stream 请求"；1.0 + close 语义简单明确，避免 keep-alive 下"流结束后连接复用"的复杂度。代价是每个请求（snapshot/status）都要新建 TCP 连接，握手开销在低频请求下可忽略。这是嵌入式"够用即可"的选型。

---

# 第三部分 RTSP/RTP（rtsp_server.cpp）

## 3.1 功能概述

纯手写（零第三方库）RTSP 1.0 服务器：

- **控制面**：TCP 8554，epoll ET + 非阻塞，处理 `OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN`；
- **数据面**：UDP `sendto()` 发送 RTP（RFC 2435 JPEG 载荷），`marker` 标记帧尾，每个客户端独立维护 `seq / ts / ssrc`；
- **RTCP**：Sender Report 每 5s 一次（`kRTCPSRInterval = 5`）；
- **状态机**：`INIT → READY → PLAYING`（`ClientState`，`rtsp_server.h:198`）。

## 3.2 关键实现

### 3.2.1 RTSP 信令处理

`handleClientData`（`296-373`）：epoll ET → 循环 read 到 EAGAIN → `recv_buf` 累积 → 以 `\r\n\r\n` 为界拆出完整请求 → `parseRequest`（`641-691`）解析方法/URI/CSeq/头 → 按方法路由到 handler。

各方法要点：

| 方法 | 处理器 | 行为 |
|------|--------|------|
| `OPTIONS` | `handleOptions` | 回 `Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN` |
| `DESCRIBE` | `handleDescribe` | 用 `getsockname` 拿本机 IP → `buildSDP()` 动态生成 SDP → 回 `Content-Type: application/sdp` |
| `SETUP` | `handleSetup` | 解析 `Transport` 头（client_port=xxxx-yyyy）→ 建 RTP/RTCP UDP socket → 生成 session + SSRC → 回 `Transport`（含 server_port + ssrc） |
| `PLAY` | `handlePlay` | 校验 session → 状态 READY→PLAYING → 回 `RTP-Info`（seq/rtptime）→ 立即发一个 RTCP SR |
| `TEARDOWN` | `handleTeardown` | 回 200 → 断开连接 |

**状态机校验**（`handlePlay`，`599-603`）：非 READY/PLAYING 状态收到 PLAY → 回 `455 Method Not Valid in This State`——体现"会话状态合法性检查"，是协议健壮性的关键。

**SDP 动态生成**（`buildSDP`，`968-987`）：

```
v=0
o=- <time> 1 IN IP4 <server_ip>
s=SmartCam Live Stream
c=IN IP4 0.0.0.0
t=0 0
a=control:*
m=video 0 RTP/AVP 26          ← PT=26 = JPEG
a=rtpmap:26 JPEG/90000        ← 时钟 90kHz
a=fmtp:26 width=640;height=480
a=framerate:30.0
```

**为什么是"动态"**：分辨率/帧率来自 `setStreamInfo()`（main 主线程在启动时从 V4L2 查询实际值传入，`main.cpp:700-711`；帧率变更回调里也会同步更新 `rtspServer->setStreamInfo`，`main.cpp:641-647`）。SDP 里的 `width/height/framerate` 每次 DESCRIBE 都按当前值生成。

### 3.2.2 RTP 打包（RFC 2435 JPEG 载荷）

`rtpSendFrame`（`789-866`）在**处理线程**中被 `feedFrame` 调用（注意：**RTP 数据不在 RTSP 线程发送**，RTSP 线程只做信令 + RTCP 定时检查）。

```cpp
// 每帧 RTP 时间戳按固定步长递增
ci->rtp_ts += m_tsPerFrame;                     // 90000/30 = 3000
// 分片数：JPEG 帧长 / 1400
const size_t maxPayload = kRtpMaxPayload;        // 1400
const size_t numFragments = (len + maxPayload - 1) / maxPayload;

for (size_t fragIdx = 0; fragIdx < numFragments; ++fragIdx) {
    bool isLast = (fragIdx == numFragments - 1);
    RTPHeader* rtp = ...;   // version=2, marker=isLast, PT=26
    rtp->sequence  = htons(ci->rtp_seq);
    rtp->timestamp = htonl(ts);
    rtp->ssrc      = htonl(ci->ssrc);
    // RFC 2435 JPEG 专有头：type=0, q=255(表在JPEG内), width/8, height/8
    RTPJPEGHeader* jh = ...;
    jh->frag_offset = offset;   // 24-bit 大端分片偏移
    memcpy(pkt + 12 + 8, jpeg + offset, fragLen);
    sendto(ci->rtp_sock_fd, pkt, ...);
    ci->rtp_seq++;
}
```

**分片逻辑**：
- 单帧 JPEG（~50KB）远超单个 UDP 包 MTU → 按 `kRtpMaxPayload=1400` 切分（MTU 1500 − IP 头 20 − UDP 头 8 = 1472，**留余量**避免 IP 分片）；
- **marker 位**：`isLast = (fragIdx == numFragments-1)`，**最后一包置 1**。接收端凭 marker 知道"这一帧结束了"，结合 seq 连续性重建帧；
- **frag_offset（24-bit）**：RFC 2435 要求，接收端按偏移把分片拼回原始 JPEG；
- **`type=0, q=255`**：量化表不随 RTP 单独传（`q=255`），而是**包含在 JPEG 数据内**——因为 MJPEG 摄像头直出的 JPEG 已内嵌量化表，无需 RFC 2435 的 Q-table 传参机制。

**每客户端独立 RTP 状态**（`ClientInfo`，`rtsp_server.h:211-217`）：`rtp_seq`/`rtp_ts`/`ssrc` 在 `handleSetup` 时用 `random()` 随机初始化（`546-549`），此后各自独立递增——**多个客户端互不干扰，seq 随机初值避免与旧会话混淆**。

### 3.2.3 RTCP SR（Sender Report，每 5s）

`rtcpsSendSR`（`872-908`）组 RTCP SR 包（`pkt_type=200`）：

```cpp
srPkt.sender.ntp_timestamp_msw/lsw = NTP 时间（getNTPTimestamp，2208988800 偏移）
srPkt.sender.rtp_timestamp         = ci->rtp_ts
srPkt.sender.sender_packet_count   = ci->packet_count
srPkt.sender.sender_octet_count    = ci->octet_count
```

发送时机：PLAY 时立即发一次（`623`）；之后 `checkRTCPSR`（`910-924`）在事件循环每轮（epoll_wait 超时 1s）检查，间隔 ≥5s 再发。

**RTCP 的作用**：让接收端（VLC/ffplay）知道 **"RTP 时间戳 ↔ NTP 真实时间" 的映射**，用于播放同步、延迟估算、统计丢包率。SR 里的 NTP 时间戳是 RTP 时间戳的"锚点"。

**为什么 5s**：RFC 3550 建议 RTCP 带宽约占会话带宽的 **5%**，且至少每 5 秒一次。SR 包仅 ~28 字节，5s 一次在 2MB/s 的 MJPEG 会话中占比可忽略（<0.01%），同时足够让播放器完成时间同步。**缩短到 1s 也行**（同步更快但带宽略增），**拉长到 30s**（省带宽但同步延迟大、个别播放器可能判定会话过期）。嵌入式低带宽下 5s 是"够用 + 不浪费"的折中。

### 3.2.4 UDP 发送策略

RTP 用 UDP 而非 TCP，`sendto()` 非阻塞发送：

**优点**：
- **低延迟**：无 TCP 的拥塞控制、重传、队头阻塞；丢包直接丢弃旧帧，实时性优先——视频流"丢一帧比等一帧好"；
- **无连接**：不需要每客户端建连状态机，`sendto` 带目标地址即发，适合"广播式"推送；
- **不阻塞**：UDP 发送缓冲满时返回 EAGAIN（当前代码甚至不处理，直接丢弃——对视频可接受）。

**缺点**：
- **丢包/乱序**：UDP 不保证可靠。RTP 头提供 seq 让接收端**检测丢包**、**按序重组**；
- **无拥塞控制**：高码率时可能加剧网络拥塞（嵌入式局域网场景通常可接受）；
- **无认证**：伪造源地址可以注入垃圾 RTP。

**如何应对 UDP 丢包**：① RTP 层靠 seq 检测 + marker 拼帧，缺包则整帧丢弃（MJPEG 帧内编码，丢一帧不影响后续帧）；② RTCP SR 提供统计供上层观察；③ 应用层可选的 NACK/重传（本实现未做——实时视频的取舍）。播放器侧 VLC/ffplay 对丢包有容错（显示上一帧或花屏一瞬）。

### 3.2.5 RTP 时间戳与 90kHz 时钟

```cpp
m_tsPerFrame = kRtpClockRate / kDefaultFPS;   // 90000 / 30 = 3000
```

**为什么 90kHz**：RFC 3551 规定 **JPEG（PT=26）的 RTP 时钟频率固定为 90000 Hz**。选择 90kHz 是因为它与常见帧率都是**整数倍关系**（90000 = 3000×30 = 3600×25 = 3750×24），每帧时间戳增量恰好是整数（30fps→3000），**避免浮点舍入、时间戳漂移**。这也是视频领域（H.264/H.265 同用 90kHz）的通行约定。

**时间戳语义**：每收到一帧 `rtp_ts += 3000`，表示"这一帧的采样时刻"（相对起始随机值）。播放器用 `rtp_ts` + RTCP SR 的映射决定**何时播放这一帧**——时间戳恒定递增是"视频匀速"的保证。

### 3.2.6 与 VLC/ffplay 的互操作性

信令流程严格按标准：`DESCRIBE → SETUP → PLAY`；响应含 `CSeq`、`Session`、`Transport`、`RTP-Info`（seq + rtptime 对齐 RTP 头）。`RTP-Info` 里的 seq/rtptime 与随后 RTP 包的 seq/timestamp 一致，VLC 才能正确解码。已知兼容性注意点（`docs/debug-summary.md` 有排障记录）：SDP 的 `c=` 行用 `0.0.0.0`、`a=control:track0` 的 URI 相对解析，某些播放器要求 `Content-Base` 与 DESCRIBE URI 一致（本实现已加 `Content-Base`）。

## 3.3 追问与应答

【面试官追问】"为什么 RTP 数据发送放在处理线程而不是 RTSP 线程？这样有什么好处和风险？"

> 【理想应答】`feedFrame` 由处理线程调用（`main.cpp:1035-1037` 附近），好处是**发送天然跟随采集节奏**——帧来了就发，不经过 RTSP 事件循环转发，少一次线程切换和队列拷贝，延迟最低。风险是**处理线程承担了发送负载**（分片组包 + sendto），若某个客户端网络慢，UDP 发送本身不阻塞（sendto 直接进内核缓冲），CPU 成本主要是组包。更深的隐患：`feedFrame` 锁外持有 `ClientInfo*`，若此刻 RTSP 线程 `disconnectClient` erase 了该客户端 → **悬垂指针风险**（当前 UDP sendto 只读 ci 字段，崩溃概率低但属于真实缺陷）。改进：引用计数或删除前标记。这个点要诚实说出"我看到了 UAF 风险及修复方向"。

【面试官追问】"SETUP 里 `client_port` 和 `server_port` 分别是什么？TCP 和 UDP 如何协同？"

> 【理想应答】`client_port=xxxx-yyyy` 是**客户端**（VLC）开放给服务器的 UDP 端口：xxxx 收 RTP、yyyy 收 RTCP。服务器在 SETUP 时创建**本地 UDP socket**（bind 随机端口），`getpeername` 拿客户端 IP，把 RTP/RTCP 包 `sendto` 到 client_port。**TCP（8554）只传信令**（DESCRIBE/SETUP/PLAY 文本协议），**UDP 传数据**（RTP 视频 + RTCP 报告）——"控制走可靠 TCP、数据走实时 UDP"是 RTSP 的标准分离设计。

【面试官追问】"如果播放器在 SETUP 后不发 PLAY 直接断开，会怎样？"

> 【理想应答】TCP 断开（EOF/EPOLLHUP）→ `disconnectClient`（`930-953`）：从 epoll 删除、关闭 TCP + RTP + RTCP 三个 socket、`m_clients.erase`。资源**完全释放**。状态停留 READY 的客户端不消耗数据面资源（feedFrame 只推给 PLAYING）。**缺漏**：RTSP 有 `Session: xxx;timeout=60` 会话超时机制，但当前实现**没有实现会话级超时回收**（只靠 TCP 断开兜底）——TCP 半开连接（客户端掉电未发 FIN）会残留 READY 状态的 entry。改进：加会话最后活跃时间 + 定时清理。

【面试官追问】"RFC 2435 的 `q` 字段为什么要设 255？"

> 【理想应答】RFC 2435 的 `q` 表示"量化表是否随 RTP 单独传输"：`q=0~127` 表示量化表以 type 方式内联在 JPEG 专有头后（每帧重复传，浪费带宽）；`q=255` 表示"量化表不在 RTP 中，接收端从 JPEG 数据自身解析"。本项目摄像头 MJPEG 直出的 JPEG **自带完整量化表**，所以 `q=255` 最省带宽且兼容标准播放器。

【面试官追问】"`kRtpMaxPayload=1400` 是怎么算出来的？为什么不能直接用 1500？"

> 【理想应答】以太网 MTU=1500，减去 IP 头 20 + UDP 头 8 = 1472 是"不触发 IP 分片"的最大 UDP payload。取 1400 是为**留余量**（VLAN 标签 4 字节、IP 选项、或隧道场景），避免"自以为没分片结果在中间路由器分片"——IP 分片会大幅降低可靠性（一片丢则整帧重组失败）。1400 是嵌入式推流的常见安全值。

---

# 第四部分 TCP 控制协议（control.cpp）

## 4.1 功能概述

面向"设备远程控制"的自定义二进制协议服务器：

- **epoll ET + 非阻塞**，单线程事件循环（`eventLoop`，`360-399`）；
- **协议帧**：`[magic:2][version:1][cmd:1][payload_len:2][payload:N][crc16:2]`；
- **命令**：拍照(0x01)、开始/停止录像(0x02/0x03)、设置分辨率(0x10)、设置格式(0x11)、查询状态(0x20)、心跳(0xFF)；
- **健壮性**：每客户端独立接收缓冲（粘包/拆包）、CRC-16/MODBUS 校验、心跳超时断开、命令函数表分发。

## 4.2 关键实现

### 4.2.1 协议帧结构与可扩展性

```cpp
#pragma pack(push, 1)
struct ProtoHeader {
    uint8_t  magic[2];       // 0xEB 0x90
    uint8_t  version;        // 0x01
    uint8_t  cmd;            // 命令类型
    uint16_t payload_len;    // 负载长度（网络字节序）
};
#pragma pack(pop)
```

**为什么这样设计**：
- **magic（魔数）**：快速同步 + 抗噪声——字节流里乱入的垃圾数据无法通过魔数校验，`tryParseFrame` 用它"找帧头"（`519-525`），找不到就丢缓冲；
- **version（版本）**：协议演进时**兼容旧客户端**——新版本可识别旧帧、返回"版本不支持"，不用改帧格式；
- **payload_len（长度）**：变长负载的基础，也是**拆包**的钥匙——知道帧总长 = 8 + payload_len；
- **cmd（命令码）+ 外部注册表**：命令码只需在枚举里加一个值 + `setCommandHandler` 注册回调，**协议本体零改动**；
- **CRC 兜底**：抗线路误码。

**响应帧**（`packResponse`，`113-163`）：`cmd | 0x80` 标记"这是响应"，额外加 1 字节 `status` 状态码（成功/未知命令/参数错误/CRC 错误/忙/不支持，`control.h:107-115`）。**请求/响应用同一帧格式**，客户端靠高位标志区分。

### 4.2.2 CRC-16/MODBUS 校验

```cpp
uint16_t crc16Modbus(const uint8_t* data, int len) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;  // 多项式 0x8005 反转
            else              crc >>= 1;
        }
    }
    return crc;
}
```

**为什么选 CRC 而不是简单校验和（sum/XOR）**：
- 校验和只能检出"和值不符"，对**数据字节交换、偶数个比特翻转、成片置零**等错误漏检率高；
- **CRC-16 检错能力**：所有单比特错、所有双比特错、所有奇数个比特错、长度 ≤16 bit 的所有突发错 **100% 检出**；更长突发错的漏检率仅 `2^-16 ≈ 0.0015%`。对"串口/网络误码"级别足够。

**CRC 计算覆盖范围**：`magic[2] + version[1] + cmd[1] + payload_len[2] + payload[N]`——**整帧除 CRC 自身外全部覆盖**（`tryParseFrame` 第 6 步：`crc16Modbus(data, frameTotal - 2)`，`579`）。

**嵌入式性能**：每字节 8 次移位异或。控制协议 payload 极小（状态查询 10 字节、设置分辨率 4 字节），一帧 CRC 计算微秒级，**开销可忽略**。若用于大 payload（>1KB）可换查表法（预生成 256 项表，每字节 1 次查表，快 ~8 倍）——但当前量级不需要。

### 4.2.3 TCP 粘包/拆包处理

TCP 是**字节流**，无帧边界——一次 `read` 可能收到 0.5 帧、2 帧、或一帧+半帧。解法：**每客户端独立 `recv_buf` 累积 + 按帧格式拆解**。

`tryParseFrame`（`510-608`）的拆帧流程：

```
① 长度不足 8（kMinFrameLen）→ 等更多数据
② 扫魔数：找不到 → 丢弃整个缓冲（防垃圾累积）；找到了但前面有垃圾 → 丢弃垃圾
③ 解析头：version/cmd/payload_len（网络字节序）
④ 超限检查：payload_len > 4096 → 丢弃整帧（防恶意超大帧吃内存）
⑤ 完整性检查：buf.size() < 8 + payload_len → 等更多数据
⑥ CRC 校验：不匹配 → 丢弃整帧
⑦ 分发命令 → 更新心跳 → erase 已处理帧 → 返回 true 继续拆下一帧
```

`handleClientData`（`466-504`）**循环 read 到 EAGAIN**（ET 语义），每次 read 后 `while (tryParseFrame(...))` 循环拆帧——**一次 read 拆出多帧**（粘包）与**多 次 read 拼一帧**（拆包）都被正确处理。

**为什么"找不到魔数就丢整个缓冲"**：正常帧必以 0xEB 0x90 开头；若缓冲里没有魔数，说明是纯噪声/错位流，留着只会让后续查找越来越慢，直接丢弃防止无界增长。

### 4.2.4 心跳超时机制

```cpp
static constexpr int kDefaultHeartbeatTimeout = 30;   // 超时 30s
static constexpr int kHeartbeatCheckInterval = 5;     // 检查间隔 5s
```

- **协议层**：客户端定期发 `CMD_HEARTBEAT`（0xFF，无负载）保活；
- **实现**：`ClientInfo.last_heartbeat` 记录最后活跃时间。注意关键细节（`595-602`）：**任何合法命令都会刷新 last_heartbeat**，不只是心跳命令——"任一帧到达即视为活着"；
- **检查**：`checkHeartbeats`（`718-738`）由 `epoll_wait` 超时（`kHeartbeatCheckInterval * 1000 = 5s`，`368-369`）周期性驱动，遍历超时客户端并断开；
- **为什么需要心跳**：检测**半开连接**——客户端断网/掉电时 TCP 不一定会发 FIN/RST，服务器无从得知，会一直占着 fd 和 `m_clients` 条目。心跳让"死连接"最多残留 30s 即被回收。

**为什么超时 30s、检查 5s**：30s 容忍网络抖动（客户端 10s 一次心跳也有余量）；5s 检查间隔使**回收延迟 ≤ 35s**，且 epoll_wait 超时不会频繁唤醒空转（5s 一次唤醒对 CPU 几乎无感）。

### 4.2.5 命令分发函数表

```cpp
std::map<uint8_t, CommandHandler> m_handlers;   // control.h:385
// 注册：
void setCommandHandler(uint8_t cmd, CommandHandler handler);   // 274-281

// 分发（dispatchCommand，614-647）：
auto it = m_handlers.find(cmd);
if (it != m_handlers.end())  handler(payload, payload_len, respBuf, &respLen);
else status = STATUS_UNKNOWN_CMD;
```

`CommandHandler = std::function<uint8_t(const uint8_t*, uint16_t, uint8_t*, uint16_t*)>`，main.cpp 注册示例（`725-856`）：

```cpp
controlSrv->setCommandHandler(CMD_CAPTURE, [capture, storage](...) {
    capture->captureJPEG(...);      // 调采集
    storage->savePhoto(...);        // 调存储
    return STATUS_OK;
});
```

**新增一个命令要改哪些代码**：① `Command` 枚举加一个值；② `main.cpp` 里 `setCommandHandler` 注册一个 lambda。**`ControlServer` 内部一行不改**——这符合**开闭原则**（对扩展开放、对修改关闭）：协议解析、粘包拆包、心跳、CRC 这些"框架"固定，命令是"插件"。

**与命令模式（Command Pattern）的关系**：这是命令模式的**轻量变体**——把"命令 → 行为"的映射从多分支 `if/else` 变成查表分发，效果等价于"将请求封装为可调用对象"。与标准命令模式的区别：没有显式的 `Command` 对象（无 undo/redo、无命令队列），用 `std::function` 闭包直接表达。若后续需要"命令日志/撤销/批量下发"，再升级为完整命令模式。

## 4.3 追问与应答

【面试官追问】"CRC 校验失败时，服务器应该回什么？客户端如何重试？"

> 【理想应答】当前实现：CRC 失败 → `LOG_WRN` + **丢弃整帧**（`582-587`），不返回任何响应。为什么**不回 CRC_ERROR**：CRC 已错，帧内容不可信，按错误帧构造的响应同样不可信，还可能造成"攻击者可注入响应"；且丢帧后客户端靠**自己的超时重试**（应用层请求-应答超时 → 重发命令）兜底，语义最干净。若一定要返回，也应**静默丢弃 + 让客户端超时重试**，而非回错误码。**协议里预留了 `STATUS_CRC_ERROR`**（`control.h:111`），可用于"客户端发来带校验字段的查询，服务器反馈'你上次的帧校验失败'"这类需要显式告知的场景。

【面试官追问】"CRC 通过但 payload 内容非法（如 SET_RESOLUTION 传 0x0000）怎么办？"

> 【理想应答】CRC 只保证**传输无错**，不保证**语义合法**。非法参数的处理链：① 命令 handler 内部校验并返回 `STATUS_BAD_PARAM`（0x02）——响应帧的 status 字段承载错误码；② 状态码体系（`STATUS_BAD_PARAM/STATUS_BUSY/STATUS_NOT_SUPPORTED`）让客户端能区分"参数错/忙/不支持"；③ 关键命令（如分辨率切换）还要在**采集侧**做二次防御（`CameraCapture` 查询 V4L2 能力列表，非法分辨率直接拒绝）。**协议分层**：magic/CRC 管传输层，status 管应用层，采集侧管硬件层——三层各司其职。

【面试官追问】"epoll 边缘触发（ET）为什么能保证指令的及时响应？漏读数据怎么办？"

> 【理想应答】**及时性**来自两点：① 边缘触发下只要有新数据到达，epoll_wait 立即返回，事件循环随即处理该 fd，无轮询延迟；② 非阻塞 + **读到 EAGAIN 才停**的循环确保内核缓冲被读尽，不会"读了部分就等下一次事件"。**漏读风险**正是 ET 的经典坑：如果一次事件只 read 一次就返回，而数据没读完，新数据到达时**不会再触发事件**（ET 只在状态变化时通知）→ 数据滞留。本项目通过 `while(true){read(); if(EAGAIN) break;}` 循环读尽来规避（`477-503`）。这是 ET 面试必考细节：**ET 必须循环读到 EAGAIN，LT 则不需要**。

【面试官追问】"控制服务是单线程事件循环，如果一个命令 handler 执行很慢（如拍照写 SD 卡），会怎样？"

> 【理想应答】**会阻塞整个事件循环**——所有客户端的后续命令都被拖住，这是单线程事件循环的固有特性。当前 handler（拍照/录像）都是毫秒级，可接受。**改进方向**：① 重操作丢给工作线程（handler 里 `std::async`/任务队列，立即返回"处理中"）；② 或改用每命令异步响应 + 超时。面试时讲"我知道单线程事件循环的阻塞点在哪、怎么隔离"即可。

【面试官追问】"`setStatusProvider` 的实现有什么巧妙之处？"

> 【理想应答】它不只存回调，还**内部自动注册了 `CMD_GET_STATUS` 的 handler**（`283-312`）：把 `StatusPayload` 字段（streaming/recording/width/height/format/fps）序列化为网络字节序的 10 字节负载。调用者只填业务字段、不碰网络字节序——**把"协议序列化"从业务回调里剥离**。这是"回调注入 + 框架代劳序列化"的组合，避免每个业务方重复写 htons 代码。

---

# 第五部分 面试攻防演练（综合追问）

【面试官追问】"MJPEG-over-HTTP、RTSP/RTP、私有 TCP 控制协议三者各自的适用场景？如果只保留一个，你保留哪个？"

> 【理想应答】① **MJPEG-over-HTTP**：浏览器零插件直看、调试/演示最方便，但每帧全量 JPEG、带宽大、无控制能力——适合"快速预览/内部调试"；② **RTSP/RTP**：标准播放器（VLC/ffplay）、低延迟流式、可扩展 H.264，是**产品化推流**的正道，但客户端需装播放器、信令复杂；③ **TCP 控制**：承载"摄像头"的设备语义（拍照/录像/参数），是**核心功能**，无可替代。
> **只保留一个**：若产品是"网络相机"，我保留 **TCP 控制**——因为"能拍照能录像"是相机存在的意义，而观看路径可以用最轻量的 HTTP（把 RTSP 降级为 `/stream`）替代；若产品是"监控流媒体服务器"，则保留 **RTSP**。**面试要点**：不是背出"都重要"，而是按产品定位做取舍推理。

【面试官追问】"i.MX6ULL 上同时开启三个服务，资源如何分配？哪个最可能成为瓶颈？如何量化？"

> 【理想应答】CPU 大头不在网络服务本身：MJPEG 直通路径下，网络侧开销 = 每帧 N 客户端 ×（memcpy + write/sendto）；若 YUYV 模式，**libjpeg 软件编码 ~25ms/帧**才是绝对大头（占满单核）。内存：三个服务稳态各几 MB（客户端线程栈 + 帧缓冲）。带宽：640x480 MJPEG ~50KB/帧 × fps × 客户端数，**10 客户端 30fps 可达 15MB/s**，超出百兆网口理论（~11MB/s）——**出站带宽最易先触顶**。
> **量化方法**：① `main.cpp` 的 `[PERF]` 插桩（每 5s 打 copy MB/s / fps / CPU / RSS）；② `top`/`pidstat` 看各线程 CPU；③ `sar -n DEV` 或 `ifconfig` 统计网口流量；④ 用 `v4l2-ctl` 先测采集端帧率上限（本项目实测摄像头仅 10fps——瓶颈在采集硬件而非网络）。**结论**：按"采集帧率 → 编码 → 拷贝 → 带宽 → 线程数"逐层排查，先用插桩定位再优化。

【面试官追问】"MJPEG 每客户端一线程，10 客户端线程数多少？如何避免线程创建/销毁开销？帧数据如何安全共享？"

> 【理想应答】线程数 = 1 accept + 10 = 11（见 2.3）。避免创建/销毁开销的方向：① **线程池**——预建固定线程 + 客户端 fd 队列，连接到来只投递 fd，避免每次 `pthread_create`（~几十 µs + 栈分配）；② **epoll 事件循环**——彻底消除每连接线程（前述演进方向）。帧数据安全共享：`m_frameMtx` 保护 `m_currentFrame`（写者 updateFrame、读者客户端线程都加锁），客户端线程只在锁内 `frame.assign` 拷出**自己的副本**，锁外 write——**"锁内拷、锁外发"**，多线程互不干扰；`m_frameIndex` 用 `std::atomic` 无锁判断新帧（`m_frameCV.wait_for` 的谓词）。

【面试官追问】"三个服务都无认证和加密。部署公网有哪些风险？怎么低成本加固？"

> 【理想应答】**风险**：① 任意人能通过 TCP 控制协议拍照/录像/改参数——**直接接管设备**；② HTTP/RTSP 流可被任意拉取（隐私泄露）；③ 无校验帧可被伪造注入（CRC 只是检错不是认证）；④ 端口暴露在公网易被扫描/DoS。**低成本加固（按嵌入式量级排序）**：① **防火墙**：`iptables` 只放行内网/白名单 IP，是最有效且零应用开销的；② **控制协议加握手 token**：TCP 连接建立后先交换预共享密钥（PSK）校验，失败即断开——只加一个命令，开销极小；③ HTTP 用 URL token 鉴权（`/stream?key=xxx`）；④ 连接数/频率限流防 DoS；⑤ 不上 TLS（i.MX6ULL 无硬件加速，TLS 握手耗 CPU 且流式每包加密开销大）——**"内网部署 + 防火墙 + 应用层 token"是嵌入式相机的现实方案**。

【面试官追问】"如果需求变更为支持 H.264 的 RTSP（RFC 6184），现有代码要改多大？"

> 【理想应答】**改动集中在两处，信令框架几乎不动**：
> ① **SDP**（`buildSDP`）：`m=video 0 RTP/AVP 26` → `96`；`a=rtpmap:26 JPEG/90000` → `a=rtpmap:96 H264/90000`；加 `a=fmtp:96 packetization-mode=1;profile-level-id=...`，SPS/PPS 通过 `sprop-parameter-sets` 或带内发送；
> ② **RTP 打包**（`rtpSendFrame`）：JPEG 专有头（8B）换成 H.264 NAL 头逻辑——单 NAL 直接放、大 NAL 用 **FU-A 分片**（1B FU indicator + 1B FU header）、聚合用 STAP-A；marker 语义仍是"访问单元（帧）结束"；seq/ts/ssrc 机制**完全复用**。
> **真正的瓶颈不在代码而在采集**：i.MX6ULL **无 VPU**，H.264 只能软件编码（x264），640x480 实时编码会吃满 CPU——所以"支持 H.264"的可行路径是外接 USB H.264 摄像头（UVC 直出 H.264）或换带 VPU 的芯片。**面试加分**：先指出硬件约束，再讲代码改动量，说明"信令层已抽象好，换编码器是换数据面"。

【面试官追问】"你的 RTSP 实现做过 VLC/ffplay 互操作测试吗？遇到过什么兼容性问题？"

> 【理想应答】README 明确 `ffplay rtsp://.../stream` / VLC 可播。实际兼容性坑（结合 `docs/debug-summary.md`）：① **SDP 的 `c=` 行与 `Content-Base`**：某些 VLC 版本对 `c=IN IP4 0.0.0.0` 的解析有差异，需要 `Content-Base` 与请求 URI 严格一致；② **`RTP-Info` 的 seq/rtptime 必须与 SETUP 后首包一致**，否则 VLC 会等待/丢帧；③ **Transport 响应格式**：必须回 `client_port=...-...;server_port=...-...;ssrc=...`，漏 server_port 某些播放器会 SETUP 失败；④ RTSP 响应行尾必须 `\r\n`（文本协议对行尾敏感）。**方法论**：每个方法用 Wireshark 抓包对照 RFC，双播放器（VLC + ffplay）交叉验证。

【面试官追问】"RTSP 里 `Session` 头和 `CSeq` 头的作用分别是什么？如果客户端不按 CSeq 递增发请求呢？"

> 【理想应答】`CSeq` 是请求-响应**配对序号**：响应必须回同样的 CSeq，客户端靠它匹配"哪个响应对应哪个请求"（RTSP 允许并发请求）。`Session` 标识一个 RTSP 会话（SETUP 创建、PLAY/TEARDOWN 携带），服务器用它区分不同播放会话。**不按序递增**：当前实现不强制校验 CSeq（解析后原样回显），只要求"SETUP 后同一会话"（`sessionVal` 比对，`330-338`）——宽松处理兼容性更好；严格服务器可拒绝乱序请求，但对单播放器场景没必要。

【面试官追问】"MJPEG 服务的 `/status` 返回 JSON 却没有用 JSON 库，为什么？"

> 【理想应答】手工 `snprintf` 拼 JSON（`sendStatusJSON`，`678-738`）：字段固定、格式简单、**零第三方依赖**——嵌入式交叉编译不引 JSON 库，二进制体积和崩溃面都小。这是"**够用即不引依赖**"原则：等 JSON 结构复杂到需要嵌套/数组/转义时，再引 `cJSON` 之类的单文件库也不迟。

---

# 第六部分 深度思考与横向关联（系统观）

## 6.1 模块解耦：网络模块如何获取帧数据？

**不通过回调、不通过共享队列，而是"处理线程直接调用服务的方法"**：

```cpp
// main.cpp:1026-1037（处理线程内，同一份 localFrame 喂三路）
if (hasHttpViewer)  mjpegServer->updateFrame(localFrame.data(), ...);
if (hasRtspViewer)  rtspServer->feedFrame(jpeg_out, jpeg_len, w, h);
if (g_recording)    g_storage->writeRecordFrame(localFrame.data(), size);
```

- **MJPEG 内部**是"**条件变量广播**"：`updateFrame` 写帧 + `notify_all`，多个客户端线程等待——这是经典的**生产者-消费者 + 广播（fan-out）**模型；
- **RTSP** 是"推入即发送"：`feedFrame` 同步遍历 PLAYING 客户端逐个 sendto——没有队列，同步扇出；
- **设计模式**：MJPEG 的条件变量广播是**观察者模式（Observer）的雏形**——帧是"事件"，客户端线程是"观察者"，`notify_all` 是"事件通知"。区别：观察者模式通常是一对多回调分发，这里用条件变量 + 每线程阻塞等待实现，效果等价但更省 CPU（线程休眠等待而非轮询）。

**优缺点**：✓ 广播模型天然支持多客户端同时消费最新帧；✓ 无中间队列、低延迟；✗ **丢弃旧帧**（每个客户端只拿"最新"帧，慢客户端永远追不上——对实时视频合理）；✗ 每客户端一帧拷贝放大内存带宽。

## 6.2 资源竞争：三服务同读一帧如何避免冲突？

- **同一份帧的"扇出"发生在处理线程内部、串行执行**（HTTP → RTSP → 录像顺序调用），所以**三服务之间不存在帧数据竞争**；
- 各服务内部用**独立锁**保护自己的状态：
  - MJPEG：`m_frameMtx`（当前帧）、`m_clientsMtx`（客户端表）、`m_qualityCacheMtx`（质量缓存）；
  - RTSP：`m_frameMtx`（最新帧）、`m_clientsMtx`（客户端表，feedFrame 收集 playingClients 后**锁外发送**）；
  - 控制：`m_clientsMtx`、`m_handlerMtx`；
- **没有用读写锁/原子引用计数**在服务间共享帧（那是 `include/common/frame_pool.h` 做的事——本项目帧池用于"采集→显示"链路，网络服务仍走拷贝）。锁粒度**粗但临界区小**（拷贝几十 KB 在锁内，ms 级），单核平台无多核并发放大，可接受。

## 6.3 设计模式评估

| 模式 | 现状 | 评估 |
|------|------|------|
| 生产者-消费者 | 采集/处理线程（产）→ 网络服务（消） | ✅ 处理线程串行扇出 + MJPEG 条件变量广播 |
| 观察者 | MJPEG `updateFrame`+`notify_all` 广播给客户端线程 | ✅ 雏形：帧=事件、客户端线程=观察者 |
| 命令模式 | 控制协议 `std::map<cmd, handler>` 函数表 | ✅ 轻量变体：查表分发代替多分支，开闭原则 |
| 状态模式 | RTSP `INIT→READY→PLAYING` enum + 状态校验 | ◐ 思想体现，但实现是 enum+if，非多态状态对象 |
| 策略 | `?quality` 质量策略、`setStreamInfo` 流参数 | ◐ 配置注入而非对象注入 |
| 模板方法 | RTSP 信令流程固定（DESCRIBE→SETUP→PLAY） | ◐ 隐式 |
| 单例/工厂 | 服务对象由 main 直接 new | — 嵌入式简洁优先 |

**若用观察者模式重构帧分发**：定义 `FrameObserver` 接口（`onFrame(shared_ptr<Frame>)`），采集侧维护 `vector<FrameObserver*>` 广播；HTTP 客户端、RTSP、录像各注册一个观察者。收益：新增消费方（如云推流）只需注册，不用改处理线程；代价：多一层间接调用 + 生命周期管理（观察者注册/注销）。**当前"main 显式串行扇出"在消费方固定（3 个）时更简单**——YAGNI，等消费方增多再重构。

## 6.4 系统级集成（systemd）

`configs/smartcam.service`：

```ini
ConditionPathExists=/dev/video0     # 摄像头未插入不启动，避免无限重启
Type=simple
ExecStart=/usr/local/bin/smartcam --device /dev/video0 \
    --http-port 8080 --control-port 9000 --rtsp-port 8554
Restart=on-failure                  # 意外退出自动重启
RestartSec=3
KillSignal=SIGTERM                  # 优雅停止：SIGTERM → 10s 超时 SIGKILL
TimeoutStopSec=10
ProtectSystem=full                  # 安全加固
ProtectHome=yes
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX AF_NETLINK
```

**崩溃恢复**：`Restart=on-failure` 让**整个进程**（含三个服务）崩溃后 3s 自动重启；`ConditionPathExists=/dev/video0` 防止"摄像头没插"时无限重启。**单个服务崩溃如何恢复**：三个服务都在一个进程内，**没有单服务独立重启能力**——这是"单进程多线程"架构的固有取舍（若按服务拆进程，可用 systemd 分别管理 + `Restart=` 各自恢复，代价是进程间通信复杂化）。

## 6.5 跨平台考量（条件编译 / PC Mock）

```cpp
// mjpeg_server.cpp:27-29
#ifdef HAS_LIBJPEG
#include <jpeglib.h>
#endif
```

- **`HAS_LIBJPEG`**：有 libjpeg-turbo 时启用 `?quality=N` 重编码；没有时**退化为纯直通**（客户端请求 quality 也被忽略，发原帧）——**同一份代码在"带 JPEG 库"与"不带"的平台上都能编译**；
- **`processor_neon.cpp`** 仅在 ARM 交叉编译时加入源文件列表（CMakeLists 按 `CMAKE_CROSSCOMPILING` 分支）；
- **PC Mock 模式**：无 `/dev/video0` 时 `main.cpp` 用 `MockCamera` 生成彩色测试条，网络服务照常启动——**网络层完全不感知相机是真是假**（只收 `uint8_t*`），这让 UI/网络调试可以在无硬件环境下进行；
- **条件编译原则**：功能降级（reencode 变直通）而非功能消失（服务仍可用），保证"最小编译配置也能跑"。

---

# 一句话总结

> "网络模块用**三种不同模型**支撑三种业务：MJPEG 用**每客户端一线程 + 条件变量广播**伺候持续推送型流媒体，RTSP 用 **epoll 单线程事件循环**处理请求-响应型信令、UDP sendto 低延迟发 RTP，控制协议用 **epoll ET + 每客户端接收缓冲 + CRC + 函数表**实现可靠可控的二进制命令通道。三者共享处理线程的同一份帧、互不抢锁，靠**'无人观看零开销'**优化与**'一次编码多方复用'**把 792MHz 单核的预算花在刀刃上。核心设计哲学是**'按业务形态选模型、按量级选技术'**——推送型不硬套 epoll，可靠控制不迷信 TCP 重传（UDP 够实时），安全防护按嵌入式现实选防火墙+token 而非 TLS。面试要敢说：每客户端一线程的上限在哪、UAF 风险在哪、怎么演进到 epoll/线程池/H.264。"

---

*本文档基于 SmartCam-Linux-on-imx6ull 项目源码，聚焦 `src/network/`。配合阅读：`docs/learn/03-mjpeg-stream-module-implementation.md`、`docs/learn/05-control-module-implementation.md`、`docs/learn/08-rtsp-module-implementation.md`（模块实现）、`docs/debug-summary.md`（网络排障）、`docs/interview/面试复习-main模块.md`（线程编排与数据流上游）、`docs/interview/面试复习-camera模块.md`（采集/处理线程下游）。*
