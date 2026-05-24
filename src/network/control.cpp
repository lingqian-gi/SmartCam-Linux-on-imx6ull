/**
 * @file    control.cpp
 * @brief   TCP 私有二进制控制协议服务器实现
 *
 * 实现要点:
 *   1. epoll 边缘触发 (ET) 非阻塞 I/O
 *   2. 每客户端独立接收缓冲区，处理 TCP 粘包/拆包
 *   3. CRC-16/MODBUS 校验
 *   4. 命令处理函数表分发
 *   5. 心跳超时自动断开
 */

#include "include/network/control.h"
#include "include/common/logger.h"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>

// ============================================================
// CRC-16/MODBUS
//
// 多项式: 0x8005 (反转 = 0xA001)
// 初始值: 0xFFFF
// 参考: MODBUS over Serial Line Specification V1.02
// ============================================================

uint16_t crc16Modbus(const uint8_t* data, int len) {
    uint16_t crc = 0xFFFF;

    for (int i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x0001) {
                crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);
            } else {
                crc >>= 1;
            }
        }
    }

    return crc;
}

uint16_t calcFrameCRC(const ProtoHeader& header, const uint8_t* payload) {
    // 计算 CRC 覆盖范围: magic[2] + version[1] + cmd[1] + payload_len[2] + payload[N]
    uint16_t crc = 0xFFFF;

    // magic[2]
    crc ^= header.magic[0];
    for (int j = 0; j < 8; ++j) {
        if (crc & 0x0001) crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);
        else crc >>= 1;
    }
    crc ^= header.magic[1];
    for (int j = 0; j < 8; ++j) {
        if (crc & 0x0001) crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);
        else crc >>= 1;
    }

    // version[1]
    crc ^= header.version;
    for (int j = 0; j < 8; ++j) {
        if (crc & 0x0001) crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);
        else crc >>= 1;
    }

    // cmd[1]
    crc ^= header.cmd;
    for (int j = 0; j < 8; ++j) {
        if (crc & 0x0001) crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);
        else crc >>= 1;
    }

    // payload_len[2] (little-endian in memory → CRC 按字节序)
    uint8_t pl_lo = header.payload_len & 0xFF;
    uint8_t pl_hi = (header.payload_len >> 8) & 0xFF;
    crc ^= pl_lo;
    for (int j = 0; j < 8; ++j) {
        if (crc & 0x0001) crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);
        else crc >>= 1;
    }
    crc ^= pl_hi;
    for (int j = 0; j < 8; ++j) {
        if (crc & 0x0001) crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);
        else crc >>= 1;
    }

    // payload[N] (逐字节)
    uint16_t plen = ntohs(header.payload_len);
    for (uint16_t i = 0; i < plen && payload; ++i) {
        crc ^= payload[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x0001) crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);
            else crc >>= 1;
        }
    }

    return crc;
}

// ============================================================
// 帧序列化
// ============================================================

int packResponse(uint8_t cmd, uint8_t status,
                 const uint8_t* payload, uint16_t payload_len,
                 uint8_t* out_buf) {
    uint8_t* p = out_buf;

    // magic
    p[0] = kProtoMagic0;
    p[1] = kProtoMagic1;
    p += 2;

    // version
    *p++ = kProtoVersion;

    // cmd (原命令码 | 0x80)
    *p++ = static_cast<uint8_t>(cmd | kResponseFlag);

    // status
    *p++ = status;

    // payload_len (网络字节序)
    uint16_t net_payload_len = htons(payload_len);
    *p++ = net_payload_len & 0xFF;
    *p++ = (net_payload_len >> 8) & 0xFF;

    // payload
    if (payload && payload_len > 0) {
        memcpy(p, payload, payload_len);
        p += payload_len;
    }

    // 计算 CRC16（覆盖 magic → payload 末尾）
    // 需要构建一个临时 header 来调用 calcFrameCRC
    ProtoHeader hdr;
    hdr.magic[0]    = kProtoMagic0;
    hdr.magic[1]    = kProtoMagic1;
    hdr.version     = kProtoVersion;
    hdr.cmd         = static_cast<uint8_t>(cmd | kResponseFlag);
    hdr.payload_len = net_payload_len;

    // 注意: payload_len 在 hdr 中是网络字节序，但 calcFrameCRC 按内存字节序处理
    // 这里我们直接用 CRC 覆盖的范围计算
    // 覆盖: magic[2] + version[1] + cmd[1] + status[1] + payload_len[2] + payload[N]
    size_t crcLen = static_cast<size_t>(p - out_buf);
    uint16_t crc = crc16Modbus(out_buf, static_cast<int>(crcLen));
    uint16_t net_crc = htons(crc);

    *p++ = net_crc & 0xFF;
    *p++ = (net_crc >> 8) & 0xFF;

    return static_cast<int>(p - out_buf);
}

// ============================================================
// 构造 / 析构
// ============================================================

ControlServer::ControlServer()
    : m_server_fd(-1)
    , m_port(0)
    , m_epoll_fd(-1)
    , m_running(false)
    , m_heartbeatTimeout(kDefaultHeartbeatTimeout)
{
}

ControlServer::~ControlServer() {
    stop();
}

// ============================================================
// 启动 / 停止
// ============================================================

int ControlServer::start(int port) {
    if (m_running) {
        LOG_WRN("ControlServer already running on port %d", m_port);
        return 0;
    }

    m_port = port;

    // 创建监听 socket
    int fd = createSocket(port);
    if (fd < 0) return -1;
    m_server_fd = fd;

    // 创建 epoll 实例
    m_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (m_epoll_fd < 0) {
        LOG_ERR_("epoll_create1 failed: %s", strerror(errno));
        ::close(m_server_fd);
        m_server_fd = -1;
        return -1;
    }

    // 将 server fd 加入 epoll（边缘触发）
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = m_server_fd;
    if (epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_server_fd, &ev) < 0) {
        LOG_ERR_("epoll_ctl(server_fd) failed: %s", strerror(errno));
        ::close(m_epoll_fd);
        ::close(m_server_fd);
        m_epoll_fd = -1;
        m_server_fd = -1;
        return -1;
    }

    m_running = true;

    LOG_INF("ControlServer started on port %d (epoll ET)", port);
    LOG_INF("  → Connect: telnet <board-ip> %d  and send binary commands", port);

    // 进入事件循环（阻塞，直到 stop() 被调用）
    eventLoop();

    return 0;
}

void ControlServer::stop() {
    if (!m_running) return;

    LOG_INF("Stopping ControlServer (port %d)...", m_port);
    m_running = false;

    // 关闭 epoll fd 以中断 epoll_wait()
    if (m_epoll_fd >= 0) {
        ::close(m_epoll_fd);
        m_epoll_fd = -1;
    }

    // 关闭 server socket
    if (m_server_fd >= 0) {
        ::close(m_server_fd);
        m_server_fd = -1;
    }

    // 关闭所有客户端连接
    {
        std::lock_guard<std::mutex> lock(m_clientsMtx);
        for (auto& pair : m_clients) {
            if (pair.second.fd >= 0) {
                ::close(pair.second.fd);
            }
        }
        m_clients.clear();
    }

    LOG_INF("ControlServer stopped");
}

// ============================================================
// 公共接口
// ============================================================

int ControlServer::clientCount() const {
    std::lock_guard<std::mutex> lock(m_clientsMtx);
    return static_cast<int>(m_clients.size());
}

void ControlServer::setCommandHandler(uint8_t cmd, CommandHandler handler) {
    std::lock_guard<std::mutex> lock(m_handlerMtx);
    if (handler) {
        m_handlers[cmd] = std::move(handler);
    } else {
        m_handlers.erase(cmd);
    }
}

void ControlServer::setStatusProvider(std::function<void(StatusPayload&)> provider) {
    m_statusProvider = std::move(provider);
    // 同时注册到命令处理器表，以便 CMD_GET_STATUS 能触发
    if (m_statusProvider) {
        setCommandHandler(CMD_GET_STATUS,
            [this](const uint8_t* /*req*/, uint16_t /*req_len*/,
                   uint8_t* resp, uint16_t* resp_len) -> uint8_t {
                StatusPayload sp;
                memset(&sp, 0, sizeof(sp));
                m_statusProvider(sp);

                // 序列化 StatusPayload → 响应负载（网络字节序）
                resp[0]  = sp.streaming;
                resp[1]  = sp.recording;
                resp[2]  = sp.client_count;
                resp[3]  = sp.reserved;
                uint16_t netW = htons(sp.width);
                uint16_t netH = htons(sp.height);
                memcpy(resp + 4, &netW, 2);
                memcpy(resp + 6, &netH, 2);
                resp[8]  = sp.format;
                resp[9]  = sp.fps;
                *resp_len = sizeof(StatusPayload);
                return STATUS_OK;
            });
    } else {
        std::lock_guard<std::mutex> lock(m_handlerMtx);
        m_handlers.erase(CMD_GET_STATUS);
    }
}

// ============================================================
// socket 创建
// ============================================================

int ControlServer::createSocket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERR_("socket() failed: %s", strerror(errno));
        return -1;
    }

    // SO_REUSEADDR — 快速重启
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 设置为非阻塞
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port));

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERR_("bind(port=%d) failed: %s", port, strerror(errno));
        ::close(fd);
        return -1;
    }

    if (listen(fd, kDefaultBacklog) < 0) {
        LOG_ERR_("listen() failed: %s", strerror(errno));
        ::close(fd);
        return -1;
    }

    return fd;
}

// ============================================================
// 事件循环
// ============================================================

void ControlServer::eventLoop() {
    constexpr int kMaxEvents = 64;
    struct epoll_event events[kMaxEvents];

    LOG_INF("ControlServer event loop started");

    while (m_running) {
        // epoll_wait 超时设置为心跳检查间隔（毫秒）
        int nfds = epoll_wait(m_epoll_fd, events, kMaxEvents,
                               kHeartbeatCheckInterval * 1000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            LOG_ERR_("epoll_wait failed: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            if (fd == m_server_fd) {
                // 新连接
                acceptClient();
            } else if (events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
                // 客户端数据 或 对端关闭
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    disconnectClient(fd);
                } else {
                    handleClientData(fd);
                }
            }
        }

        // 心跳超时检查
        if (nfds == 0 || (nfds > 0)) {
            checkHeartbeats();
        }
    }

    LOG_INF("ControlServer event loop exited");
}

// ============================================================
// 接受连接
// ============================================================

void ControlServer::acceptClient() {
    while (true) {
        struct sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);

        int client_fd = accept(m_server_fd,
                               reinterpret_cast<struct sockaddr*>(&clientAddr),
                               &addrLen);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 边缘触发：所有连接已处理完毕
                break;
            }
            if (errno == EINTR) continue;
            LOG_ERR_("accept() failed: %s", strerror(errno));
            break;
        }

        // 设置客户端 socket 选项
        // TCP_NODELAY — 禁用 Nagle 算法，降低延迟
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        // 设为非阻塞
        int flags = fcntl(client_fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
        }

        // 加入 epoll（边缘触发）
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events  = EPOLLIN | EPOLLET;
        ev.data.fd = client_fd;
        if (epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            LOG_ERR_("epoll_ctl(client_fd=%d) failed: %s", client_fd, strerror(errno));
            ::close(client_fd);
            continue;
        }

        // 记录客户端
        {
            std::lock_guard<std::mutex> lock(m_clientsMtx);
            ClientInfo ci;
            ci.fd = client_fd;
            ci.last_heartbeat = std::chrono::steady_clock::now();
            ci.recv_buf.reserve(4096);
            m_clients[client_fd] = std::move(ci);
        }

        char ipStr[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        LOG_INF("Control client connected: %s:%d (total=%d)",
                ipStr, ntohs(clientAddr.sin_port), clientCount());
    }
}

// ============================================================
// 处理客户端数据
// ============================================================

void ControlServer::handleClientData(int client_fd) {
    ClientInfo* ci = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_clientsMtx);
        auto it = m_clients.find(client_fd);
        if (it == m_clients.end()) return;
        ci = &it->second;
    }

    // 边缘触发：循环读取直到 EAGAIN
    uint8_t readBuf[4096];
    while (true) {
        ssize_t n = read(client_fd, readBuf, sizeof(readBuf));
        if (n > 0) {
            // 追加到接收缓冲区
            ci->recv_buf.insert(ci->recv_buf.end(),
                                readBuf, readBuf + n);

            // 尝试解析帧（可能有多帧）
            while (tryParseFrame(client_fd, ci->recv_buf)) {
                // tryParseFrame 会修改 ci->recv_buf
            }
        } else if (n == 0) {
            // 对端正常关闭
            LOG_DBG("Control client fd=%d disconnected (EOF)", client_fd);
            disconnectClient(client_fd);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 边缘触发：所有数据已读完
                break;
            }
            if (errno == EINTR) continue;
            LOG_ERR_("read(client_fd=%d) failed: %s", client_fd, strerror(errno));
            disconnectClient(client_fd);
            return;
        }
    }
}

// ============================================================
// 帧解析
// ============================================================

bool ControlServer::tryParseFrame(int client_fd, std::vector<uint8_t>& buf) {
    if (buf.size() < static_cast<size_t>(kMinFrameLen)) {
        // 至少需要 8 字节才能形成最简帧
        return false;
    }

    // 1. 查找魔数 (0xEB 0x90)
    size_t magicPos = 0;
    bool found = false;
    for (size_t i = 0; i <= buf.size() - 2; ++i) {
        if (buf[i] == kProtoMagic0 && buf[i + 1] == kProtoMagic1) {
            magicPos = i;
            found = true;
            break;
        }
    }

    if (!found) {
        // 没有找到魔数，丢弃整个缓冲区（防止噪声数据积累）
        LOG_WRN("Control: no magic found in %zu bytes, discarding", buf.size());
        buf.clear();
        return false;
    }

    // 丢弃魔数之前的垃圾数据
    if (magicPos > 0) {
        LOG_DBG("Control: discarding %zu bytes before magic", magicPos);
        buf.erase(buf.begin(), buf.begin() + static_cast<long>(magicPos));
    }

    // 2. 检查是否有完整帧头
    if (buf.size() < static_cast<size_t>(kMinFrameLen)) {
        return false;  // 等待更多数据
    }

    // 3. 解析帧头
    const uint8_t* data = buf.data();
    ProtoHeader hdr;
    hdr.magic[0]    = data[0];
    hdr.magic[1]    = data[1];
    hdr.version     = data[2];
    hdr.cmd         = data[3];
    hdr.payload_len = static_cast<uint16_t>(data[4] | (data[5] << 8));  // 网络字节序

    uint16_t payload_len = ntohs(hdr.payload_len);

    // 4. 检查载荷是否超限
    if (payload_len > kMaxFrameLen - kMinFrameLen) {
        LOG_WRN("Control: payload too large (%u bytes), discarding frame", payload_len);
        buf.clear();
        return false;
    }

    // 5. 检查是否有完整帧（帧头 + 载荷 + CRC16）
    size_t frameTotal = kMinFrameLen + payload_len;
    if (buf.size() < frameTotal) {
        return false;  // 等待更多数据
    }

    // 6. 验证 CRC16
    uint16_t recv_crc = static_cast<uint16_t>(
        data[frameTotal - 2] | (data[frameTotal - 1] << 8));  // 网络字节序
    recv_crc = ntohs(recv_crc);

    // 计算 CRC（覆盖 magic → payload 末尾）
    uint16_t calc_crc;
    {
        // CRC 覆盖: magic[2] + version[1] + cmd[1] + payload_len[2] + payload[N]
        // 即 buf[0..frameTotal-3]（不含 CRC 自身）
        calc_crc = crc16Modbus(data, static_cast<int>(frameTotal - 2));
    }

    if (recv_crc != calc_crc) {
        LOG_WRN("Control: CRC mismatch (recv=0x%04X, calc=0x%04X), discarding frame",
                recv_crc, calc_crc);
        buf.clear();
        return false;
    }

    // 7. 提取载荷
    const uint8_t* payload = (payload_len > 0) ? (data + 6) : nullptr;

    // 8. 分发命令
    dispatchCommand(client_fd, hdr.cmd, payload, payload_len);

    // 9. 更新心跳时间
    {
        std::lock_guard<std::mutex> lock(m_clientsMtx);
        auto it = m_clients.find(client_fd);
        if (it != m_clients.end()) {
            it->second.last_heartbeat = std::chrono::steady_clock::now();
        }
    }

    // 10. 从缓冲区移除已处理的帧
    buf.erase(buf.begin(), buf.begin() + static_cast<long>(frameTotal));

    return true;
}

// ============================================================
// 命令分发
// ============================================================

void ControlServer::dispatchCommand(int client_fd, uint8_t cmd,
                                     const uint8_t* payload, uint16_t payload_len) {
    uint8_t status = STATUS_OK;
    uint8_t respBuf[4096];
    uint16_t respLen = 0;

    // 心跳命令由内置处理器处理（不占用外部注册表）
    if (cmd == CMD_HEARTBEAT) {
        status = handleHeartbeat(payload, payload_len, respBuf, &respLen);
    } else {
        // 从命令处理器表查找
        CommandHandler handler;
        {
            std::lock_guard<std::mutex> lock(m_handlerMtx);
            auto it = m_handlers.find(cmd);
            if (it != m_handlers.end()) {
                handler = it->second;
            }
        }

        if (handler) {
            status = handler(payload, payload_len, respBuf, &respLen);
        } else {
            LOG_WRN("Control: unknown command 0x%02X from fd=%d", cmd, client_fd);
            status = STATUS_UNKNOWN_CMD;
        }
    }

    // 发送响应
    if (!sendResponse(client_fd, cmd, status, respBuf, respLen)) {
        LOG_DBG("Control: failed to send response to fd=%d, disconnecting", client_fd);
        disconnectClient(client_fd);
    }
}

// ============================================================
// 发送响应
// ============================================================

bool ControlServer::sendResponse(int client_fd, uint8_t cmd, uint8_t status,
                                  const uint8_t* payload, uint16_t payload_len) {
    uint8_t outBuf[kMaxFrameLen];
    int frameLen = packResponse(cmd, status, payload, payload_len, outBuf);

    // 确保完整发送（处理 EAGAIN）
    ssize_t totalSent = 0;
    while (totalSent < frameLen) {
        ssize_t n = write(client_fd, outBuf + totalSent,
                          static_cast<size_t>(frameLen - totalSent));
        if (n > 0) {
            totalSent += n;
        } else if (n == 0) {
            return false;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 非阻塞模式下缓冲区满，稍等后重试
                usleep(1000);
                continue;
            }
            if (errno == EINTR) continue;
            return false;
        }
    }

    return true;
}

// ============================================================
// 内置心跳处理器
// ============================================================

uint8_t ControlServer::handleHeartbeat(const uint8_t* /*req*/, uint16_t /*req_len*/,
                                        uint8_t* /*resp*/, uint16_t* resp_len) {
    // 心跳帧：无负载，仅回显成功
    *resp_len = 0;
    return STATUS_OK;
}

// ============================================================
// 客户端断开
// ============================================================

void ControlServer::disconnectClient(int client_fd) {
    {
        std::lock_guard<std::mutex> lock(m_clientsMtx);
        auto it = m_clients.find(client_fd);
        if (it == m_clients.end()) return;

        // 从 epoll 移除
        epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);

        // 关闭 socket
        ::close(client_fd);
        m_clients.erase(it);
    }

    LOG_INF("Control client fd=%d disconnected (total=%d)",
            client_fd, clientCount());
}

// ============================================================
// 心跳超时检查
// ============================================================

void ControlServer::checkHeartbeats() {
    auto now = std::chrono::steady_clock::now();
    std::vector<int> toDisconnect;

    {
        std::lock_guard<std::mutex> lock(m_clientsMtx);
        for (const auto& pair : m_clients) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               now - pair.second.last_heartbeat).count();
            if (elapsed >= m_heartbeatTimeout) {
                toDisconnect.push_back(pair.first);
            }
        }
    }

    for (int fd : toDisconnect) {
        LOG_WRN("Control: client fd=%d heartbeat timeout (>=%ds), disconnecting",
                fd, m_heartbeatTimeout);
        disconnectClient(fd);
    }
}
