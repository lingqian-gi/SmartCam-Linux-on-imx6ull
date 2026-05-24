/**
 * @file    rtsp_server.cpp
 * @brief   RTSP/RTP 实时流服务器实现
 *
 * 实现要点:
 *   1. epoll ET + 非阻塞 TCP 处理 RTSP 控制连接
 *   2. UDP sendto() 发送 RTP 数据 (低延迟, 不阻塞)
 *   3. RFC 2435 JPEG 载荷格式 (RTP 分片, marker 位标记帧结束)
 *   4. RFC 3550 RTCP SR (Sender Report, 每 5s 一次)
 *   5. 每个客户端独立 RTP 状态机 (seq / ts / ssrc)
 *
 * 参考:
 *   - RFC 2326: RTSP 1.0
 *   - RFC 3550: RTP / RTCP
 *   - RFC 2435: RTP Payload Format for JPEG
 */

#include "include/network/rtsp_server.h"
#include "include/common/logger.h"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sstream>
#include <algorithm>
#include <random>

// ============================================================
// 构造 / 析构
// ============================================================

RTSPServer::RTSPServer()
    : m_server_fd(-1), m_port(0), m_epoll_fd(-1), m_running(false)
{
    m_tsPerFrame = kRtpClockRate / kDefaultFPS;
}

RTSPServer::~RTSPServer() {
    stop();
}

// ============================================================
// 流信息
// ============================================================

void RTSPServer::setStreamInfo(int width, int height, int fps) {
    m_streamWidth  = width;
    m_streamHeight = height;
    m_streamFPS    = fps > 0 ? fps : kDefaultFPS;
    m_tsPerFrame   = kRtpClockRate / static_cast<uint32_t>(m_streamFPS);
}

// ============================================================
// 启动 / 停止
// ============================================================

int RTSPServer::start(int port) {
    if (m_running) {
        LOG_WRN("RTSPServer already running on port %d", m_port);
        return 0;
    }

    m_port = port;

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

    // 将 server fd 加入 epoll (边缘触发)
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = m_server_fd;
    if (epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_server_fd, &ev) < 0) {
        LOG_ERR_("epoll_ctl(server_fd) failed: %s", strerror(errno));
        ::close(m_epoll_fd);
        ::close(m_server_fd);
        m_epoll_fd  = -1;
        m_server_fd = -1;
        return -1;
    }

    m_running = true;

    char ipStr[INET_ADDRSTRLEN] = {0};
    {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0) {
            inet_ntop(AF_INET, &addr.sin_addr, ipStr, sizeof(ipStr));
        }
    }
    LOG_INF("RTSP Server started on rtsp://%s:%d/stream", ipStr, port);
    LOG_INF("  → Play: ffplay rtsp://<board-ip>:%d/stream", port);

    // 进入事件循环 (阻塞, 直到 stop() 被调用)
    eventLoop();

    return 0;
}

void RTSPServer::stop() {
    if (!m_running) return;

    LOG_INF("Stopping RTSP Server (port %d)...", m_port);
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
            if (pair.second.tcp_fd >= 0) {
                ::close(pair.second.tcp_fd);
            }
            if (pair.second.rtp_sock_fd >= 0) {
                ::close(pair.second.rtp_sock_fd);
            }
            if (pair.second.rtcp_sock_fd >= 0) {
                ::close(pair.second.rtcp_sock_fd);
            }
        }
        m_clients.clear();
    }

    LOG_INF("RTSP Server stopped");
}

// ============================================================
// socket 创建
// ============================================================

int RTSPServer::createSocket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERR_("socket() failed: %s", strerror(errno));
        return -1;
    }

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

    if (listen(fd, kRTSPBacklog) < 0) {
        LOG_ERR_("listen() failed: %s", strerror(errno));
        ::close(fd);
        return -1;
    }

    return fd;
}

// ============================================================
// 事件循环
// ============================================================

void RTSPServer::eventLoop() {
    constexpr int kMaxEvents = 64;
    struct epoll_event events[kMaxEvents];

    LOG_INF("RTSP event loop started (epoll ET)");

    while (m_running) {
        // epoll_wait 超时设为 1 秒（用于定时 RTCP SR 检查）
        int nfds = epoll_wait(m_epoll_fd, events, kMaxEvents, 1000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            LOG_ERR_("epoll_wait failed: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            if (fd == m_server_fd) {
                acceptClient();
            } else if (events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    disconnectClient(fd);
                } else {
                    handleClientData(fd);
                }
            }
        }

        // RTCP SR 定时检查 (每 5s)
        checkRTCPSR();
    }

    LOG_INF("RTSP event loop exited");
}

// ============================================================
// accept
// ============================================================

void RTSPServer::acceptClient() {
    while (true) {
        struct sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);

        int client_fd = accept(m_server_fd,
                               reinterpret_cast<struct sockaddr*>(&clientAddr),
                               &addrLen);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            LOG_ERR_("accept() failed: %s", strerror(errno));
            break;
        }

        // TCP_NODELAY
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        // 非阻塞
        int flags = fcntl(client_fd, F_GETFL, 0);
        if (flags >= 0) fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        // 加入 epoll
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events  = EPOLLIN | EPOLLET;
        ev.data.fd = client_fd;
        if (epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            LOG_ERR_("epoll_ctl(client=%d) failed: %s", client_fd, strerror(errno));
            ::close(client_fd);
            continue;
        }

        // 记录客户端 (INIT 状态)
        {
            std::lock_guard<std::mutex> lock(m_clientsMtx);
            ClientInfo ci;
            ci.tcp_fd   = client_fd;
            ci.state    = INIT;
            m_clients[client_fd] = std::move(ci);
        }

        char ipStr[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        LOG_INF("RTSP client connected: %s:%d (total=%d)",
                ipStr, ntohs(clientAddr.sin_port), clientCount());
    }
}

// ============================================================
// 处理客户端数据 (边缘触发 → 循环读取直到 EAGAIN)
// ============================================================

void RTSPServer::handleClientData(int client_fd) {
    ClientInfo* ci = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_clientsMtx);
        auto it = m_clients.find(client_fd);
        if (it == m_clients.end()) return;
        ci = &it->second;
    }

    char readBuf[4096];
    while (true) {
        ssize_t n = read(client_fd, readBuf, sizeof(readBuf) - 1);
        if (n > 0) {
            readBuf[n] = '\0';
            ci->recv_buf.append(readBuf, static_cast<size_t>(n));

            // 尝试解析完整的 RTSP 请求（以 \r\n\r\n 结束）
            size_t endPos;
            while ((endPos = ci->recv_buf.find("\r\n\r\n")) != std::string::npos) {
                std::string request = ci->recv_buf.substr(0, endPos + 4);
                ci->recv_buf.erase(0, endPos + 4);

                // 解析请求行 + 头
                std::string method, uri;
                int cseq = -1;
                std::map<std::string, std::string> headers;

                if (!parseRequest(request, &method, &uri, &cseq, &headers)) {
                    LOG_WRN("RTSP: malformed request from fd=%d", client_fd);
                    sendRTSPResponse(client_fd, cseq >= 0 ? cseq : 0, 400,
                                     "Bad Request", {}, "");
                    continue;
                }

                // 如果客户端已经 SETUP, 必须是同一 session
                auto sessionIt = headers.find("Session");
                std::string sessionVal;
                if (sessionIt != headers.end()) {
                    size_t semi = sessionIt->second.find(';');
                    sessionVal = (semi != std::string::npos)
                        ? sessionIt->second.substr(0, semi)
                        : sessionIt->second;
                }

                // 路由到对应处理器
                if (method == "OPTIONS") {
                    handleOptions(client_fd, cseq);
                } else if (method == "DESCRIBE") {
                    handleDescribe(client_fd, cseq, uri);
                } else if (method == "SETUP") {
                    auto transIt = headers.find("Transport");
                    std::string transport = transIt != headers.end()
                        ? transIt->second : "";
                    handleSetup(client_fd, cseq, uri, transport);
                } else if (method == "PLAY") {
                    handlePlay(client_fd, cseq, uri, sessionVal);
                } else if (method == "TEARDOWN") {
                    handleTeardown(client_fd, cseq, sessionVal);
                } else {
                    LOG_WRN("RTSP: unknown method '%s' from fd=%d",
                            method.c_str(), client_fd);
                    sendRTSPResponse(client_fd, cseq, 405,
                                     "Method Not Allowed", {}, "");
                }
            }
        } else if (n == 0) {
            LOG_DBG("RTSP client fd=%d disconnected (EOF)", client_fd);
            disconnectClient(client_fd);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            LOG_ERR_("read(client=%d) failed: %s", client_fd, strerror(errno));
            disconnectClient(client_fd);
            return;
        }
    }
}

// ============================================================
// OPTIONS
// ============================================================

void RTSPServer::handleOptions(int client_fd, int cseq) {
    LOG_DBG("RTSP: OPTIONS from fd=%d, cseq=%d", client_fd, cseq);
    sendRTSPResponse(client_fd, cseq, 200, "OK",
                     {{"Public", "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN"}}, "");
}

// ============================================================
// DESCRIBE
// ============================================================

void RTSPServer::handleDescribe(int client_fd, int cseq,
                                 const std::string& /*uri*/) {
    LOG_DBG("RTSP: DESCRIBE from fd=%d", client_fd);

    // 获取服务器 IP
    std::string serverIp = "0.0.0.0";
    {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        if (getsockname(client_fd, reinterpret_cast<struct sockaddr*>(&addr),
                        &len) == 0) {
            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr.sin_addr, ipStr, sizeof(ipStr));
            serverIp = ipStr;
        }
    }

    std::string sdp = buildSDP(serverIp);

    sendRTSPResponse(client_fd, cseq, 200, "OK",
                     {
                         {"Content-Type",   "application/sdp"},
                         {"Content-Length", std::to_string(sdp.size())},
                         {"Content-Base",   "rtsp://" + serverIp + ":" +
                           std::to_string(m_port) + "/stream"}
                     }, sdp);
}

// ============================================================
// SETUP (建立 RTP/RTCP 传输通道)
// ============================================================

void RTSPServer::handleSetup(int client_fd, int cseq,
                              const std::string& /*uri*/,
                              const std::string& transport) {
    LOG_DBG("RTSP: SETUP from fd=%d, Transport=%s", client_fd, transport.c_str());

    // 解析 Transport 头: RTP/AVP;unicast;client_port=xxxx-yyyy
    int clientRtpPort  = 0;
    int clientRtcpPort = 0;

    {
        auto parsePort = [](const std::string& s, const std::string& key) -> int {
            size_t pos = s.find(key);
            if (pos == std::string::npos) return 0;
            pos += key.size();
            while (pos < s.size() && !isdigit(s[pos])) pos++;
            return atoi(s.c_str() + pos);
        };
        clientRtpPort  = parsePort(transport, "client_port=");
        clientRtcpPort = parsePort(transport, "server_port=");
        // 通常只有一个 client_port 范围, 如 client_port=5000-5001
        // 但格式多变，简化处理
        if (clientRtpPort == 0) {
            // 尝试从 client_port=xxxx-yyyy 解析
            size_t cp = transport.find("client_port=");
            if (cp != std::string::npos) {
                const char* p = transport.c_str() + cp + 12;
                char* end = nullptr;
                clientRtpPort = static_cast<int>(strtol(p, &end, 10));
                if (end && *end == '-') {
                    clientRtcpPort = static_cast<int>(strtol(end + 1, nullptr, 10));
                } else {
                    // 可能是单个端口; RTCP = RTP + 1
                    clientRtcpPort = clientRtpPort + 1;
                }
            }
        }
    }

    if (clientRtpPort <= 0 || clientRtpPort > 65535) {
        LOG_WRN("RTSP: SETUP bad transport '%s'", transport.c_str());
        sendRTSPResponse(client_fd, cseq, 461, "Unsupported Transport", {}, "");
        return;
    }
    if (clientRtcpPort <= 0) clientRtcpPort = clientRtpPort + 1;

    LOG_INF("RTSP: SETUP → RTP port=%d, RTCP port=%d",
            clientRtpPort, clientRtcpPort);

    // 获取客户端 IP (从已建立的 TCP 连接)
    struct sockaddr_in peerAddr;
    socklen_t peerLen = sizeof(peerAddr);
    getpeername(client_fd, reinterpret_cast<struct sockaddr*>(&peerAddr), &peerLen);

    // 创建 UDP socket for RTP
    int rtp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (rtp_sock < 0) {
        LOG_ERR_("RTP socket() failed: %s", strerror(errno));
        sendRTSPResponse(client_fd, cseq, 500, "Internal Server Error", {}, "");
        return;
    }

    // 创建 UDP socket for RTCP
    int rtcp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (rtcp_sock < 0) {
        ::close(rtp_sock);
        LOG_ERR_("RTCP socket() failed: %s", strerror(errno));
        sendRTSPResponse(client_fd, cseq, 500, "Internal Server Error", {}, "");
        return;
    }

    // 获取本地端口
    struct sockaddr_in localRtp, localRtcp;
    socklen_t len = sizeof(localRtp);
    memset(&localRtp, 0, sizeof(localRtp));
    localRtp.sin_family = AF_INET;
    localRtp.sin_addr.s_addr = INADDR_ANY;
    localRtp.sin_port = 0;
    bind(rtp_sock, reinterpret_cast<struct sockaddr*>(&localRtp), sizeof(localRtp));
    getsockname(rtp_sock, reinterpret_cast<struct sockaddr*>(&localRtp), &len);

    len = sizeof(localRtcp);
    memset(&localRtcp, 0, sizeof(localRtcp));
    localRtcp.sin_family = AF_INET;
    localRtcp.sin_addr.s_addr = INADDR_ANY;
    localRtcp.sin_port = 0;
    bind(rtcp_sock, reinterpret_cast<struct sockaddr*>(&localRtcp), sizeof(localRtcp));
    getsockname(rtcp_sock, reinterpret_cast<struct sockaddr*>(&localRtcp), &len);

    // 填充客户端 RTP/RTCP 地址
    struct sockaddr_in rtpTarget;
    memset(&rtpTarget, 0, sizeof(rtpTarget));
    rtpTarget.sin_family      = AF_INET;
    rtpTarget.sin_addr.s_addr = peerAddr.sin_addr.s_addr;
    rtpTarget.sin_port        = htons(static_cast<uint16_t>(clientRtpPort));

    struct sockaddr_in rtcpTarget;
    memset(&rtcpTarget, 0, sizeof(rtcpTarget));
    rtcpTarget.sin_family      = AF_INET;
    rtcpTarget.sin_addr.s_addr = peerAddr.sin_addr.s_addr;
    rtcpTarget.sin_port        = htons(static_cast<uint16_t>(clientRtcpPort));

    // 生成 session ID + SSRC
    std::string sessionId = generateSessionID();

    {
        std::lock_guard<std::mutex> lock(m_clientsMtx);
        auto it = m_clients.find(client_fd);
        if (it == m_clients.end()) {
            ::close(rtp_sock);
            ::close(rtcp_sock);
            return;
        }
        ClientInfo& ci = it->second;

        // 关闭旧的 RTP/RTCP sockets (如果重复 SETUP)
        if (ci.rtp_sock_fd >= 0)  ::close(ci.rtp_sock_fd);
        if (ci.rtcp_sock_fd >= 0) ::close(ci.rtcp_sock_fd);

        ci.rtp_sock_fd  = rtp_sock;
        ci.rtcp_sock_fd = rtcp_sock;
        ci.rtp_addr     = rtpTarget;
        ci.rtcp_addr    = rtcpTarget;
        ci.session_id   = sessionId;
        ci.state        = READY;

        // 初始化 RTP 状态
        ci.rtp_seq = random() & 0xFFFF;
        ci.rtp_ts  = random() & 0xFFFFFFFF;
        ci.ssrc    = random() & 0xFFFFFFFF;
        ci.packet_count = 0;
        ci.octet_count  = 0;
        ci.last_rtcp_sr = std::chrono::steady_clock::now();
    }

    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peerAddr.sin_addr, ipStr, sizeof(ipStr));

    int svrRtpPort  = ntohs(localRtp.sin_port);
    int svrRtcpPort = ntohs(localRtcp.sin_port);

    sendRTSPResponse(client_fd, cseq, 200, "OK",
                     {
                         {"Session", sessionId + ";timeout=60"},
                         {"Transport",
                          "RTP/AVP;unicast;client_port=" +
                          std::to_string(clientRtpPort) + "-" +
                          std::to_string(clientRtcpPort) +
                          ";server_port=" +
                          std::to_string(svrRtpPort) + "-" +
                          std::to_string(svrRtcpPort) +
                          ";ssrc=" + std::to_string(static_cast<uint32_t>(
                              htonl(static_cast<uint32_t>(random()))))}
                     }, "");

    LOG_INF("RTSP: SETUP complete → session=%s, ssrc=0x%08X",
            sessionId.c_str(), static_cast<unsigned>(random() & 0xFFFFFFFF));
}

// ============================================================
// PLAY
// ============================================================

void RTSPServer::handlePlay(int client_fd, int cseq,
                             const std::string& /*uri*/,
                             const std::string& session) {
    LOG_DBG("RTSP: PLAY from fd=%d, Session=%s", client_fd, session.c_str());

    ClientInfo* ci = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_clientsMtx);
        auto it = m_clients.find(client_fd);
        if (it == m_clients.end() || it->second.session_id != session) {
            sendRTSPResponse(client_fd, cseq, 454, "Session Not Found", {}, "");
            return;
        }
        ci = &it->second;
    }

    if (ci->state != READY && ci->state != PLAYING) {
        sendRTSPResponse(client_fd, cseq, 455,
                         "Method Not Valid in This State", {}, "");
        return;
    }

    ci->state = PLAYING;

    // RTP-Info 头
    uint32_t ssrc_nbo = htonl(ci->ssrc);
    sendRTSPResponse(client_fd, cseq, 200, "OK",
                     {
                         {"Session", ci->session_id + ";timeout=60"},
                         {"Range", "npt=0.000-"},
                         {"RTP-Info",
                          "url=rtsp://" + std::string("0.0.0.0") + ":" +
                          std::to_string(m_port) + "/stream;seq=" +
                          std::to_string(ci->rtp_seq) + ";rtptime=" +
                          std::to_string(ci->rtp_ts)}
                     }, "");

    LOG_INF("RTSP: PLAY → client fd=%d now PLAYING", client_fd);

    // 立即发送第一个 RTCP SR
    rtcpsSendSR(ci);
}

// ============================================================
// TEARDOWN
// ============================================================

void RTSPServer::handleTeardown(int client_fd, int cseq,
                                 const std::string& /*session*/) {
    LOG_DBG("RTSP: TEARDOWN from fd=%d", client_fd);
    sendRTSPResponse(client_fd, cseq, 200, "OK", {}, "");
    disconnectClient(client_fd);
}

// ============================================================
// RTSP 请求解析
// ============================================================

bool RTSPServer::parseRequest(const std::string& buf,
                               std::string* method,
                               std::string* uri,
                               int* cseq,
                               std::map<std::string, std::string>* headers) {
    if (buf.empty()) return false;

    std::istringstream stream(buf);
    std::string line;

    // 请求行: METHOD URI RTSP/1.0
    if (!std::getline(stream, line)) return false;
    // 去掉行尾 \r
    if (!line.empty() && line.back() == '\r') line.pop_back();

    std::istringstream reqLine(line);
    if (!(reqLine >> *method)) return false;
    if (!(reqLine >> *uri)) return false;

    // 解析头
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;

        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key   = line.substr(0, colon);
        std::string value = line.substr(colon + 1);

        // trim value
        size_t start = value.find_first_not_of(" \t");
        size_t end   = value.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos) {
            value = value.substr(start, end - start + 1);
        }

        // 标准化 key 为首字母大写其余小写
        for (auto& c : key) c = static_cast<char>(tolower(c));
        if (!key.empty()) key[0] = static_cast<char>(toupper(key[0]));

        // CSeq 特殊处理
        if (key == "Cseq") {
            *cseq = atoi(value.c_str());
        }

        (*headers)[key] = value;
    }

    return !method->empty() && !uri->empty();
}

// ============================================================
// RTSP 响应发送
// ============================================================

void RTSPServer::sendRTSPResponse(
    int client_fd, int cseq, int status_code, const char* reason,
    const std::map<std::string, std::string>& headers,
    const std::string& body) {

    std::ostringstream resp;

    // 状态行
    resp << "RTSP/1.0 " << status_code << " " << reason << "\r\n";
    resp << "CSeq: " << cseq << "\r\n";
    resp << "Server: SmartCam/1.0\r\n";

    // 自定义头
    for (const auto& h : headers) {
        if (h.first != "Cseq") {  // CSeq 已经写了
            resp << h.first << ": " << h.second << "\r\n";
        }
    }

    // Content-Length (如果有 body)
    if (!body.empty()) {
        resp << "Content-Length: " << body.size() << "\r\n";
    }

    resp << "\r\n";

    if (!body.empty()) {
        resp << body;
    }

    std::string respStr = resp.str();

    // 确保完整发送
    ssize_t totalSent = 0;
    while (totalSent < static_cast<ssize_t>(respStr.size())) {
        ssize_t n = write(client_fd, respStr.data() + totalSent,
                          respStr.size() - static_cast<size_t>(totalSent));
        if (n > 0) {
            totalSent += n;
        } else if (n == 0) {
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            if (errno == EINTR) continue;
            LOG_ERR_("write(RTSP response, fd=%d) failed: %s",
                     client_fd, strerror(errno));
            return;
        }
    }
}

// ============================================================
// feedFrame (采集线程调用)
// ============================================================

void RTSPServer::feedFrame(const uint8_t* jpeg_data, size_t len,
                            int width, int height) {
    if (!m_running) return;

    // 存储最新帧（供后续客户端使用）
    {
        std::lock_guard<std::mutex> lock(m_frameMtx);
        m_latestJpeg.assign(jpeg_data, jpeg_data + len);
        m_latestWidth  = width;
        m_latestHeight = height;
        m_hasFrame     = true;
    }

    // 推送到所有 PLAYING 客户端
    std::vector<ClientInfo*> playingClients;
    {
        std::lock_guard<std::mutex> lock(m_clientsMtx);
        for (auto& pair : m_clients) {
            if (pair.second.state == PLAYING) {
                playingClients.push_back(&pair.second);
            }
        }
    }

    // 在锁外发送 RTP (避免阻塞)
    for (auto* ci : playingClients) {
        rtpSendFrame(ci, jpeg_data, len, width, height);
    }
}

// ============================================================
// RTP 帧发送 + 分片 (RFC 2435)
// ============================================================

void RTSPServer::rtpSendFrame(ClientInfo* ci, const uint8_t* jpeg, size_t len,
                               int width, int height) {
    // 更新 RTP 时间戳
    ci->rtp_ts += m_tsPerFrame;

    // 计算 JPEG 分片数量
    const size_t maxPayload = kRtpMaxPayload;
    const size_t numFragments = (len + maxPayload - 1) / maxPayload;
    if (numFragments == 0) return;

    uint32_t ts = ci->rtp_ts;

    for (size_t fragIdx = 0; fragIdx < numFragments; ++fragIdx) {
        size_t offset = fragIdx * maxPayload;
        size_t remaining = len - offset;
        size_t fragLen = (remaining < maxPayload) ? remaining : maxPayload;
        bool isLast = (fragIdx == numFragments - 1);

        // ============================================================
        // 组装 RTP 包: [RTP固定头(12)] + [JPEG专有头(8)] + [JPEG数据]
        // ============================================================

        size_t pktSize = sizeof(RTPHeader) + sizeof(RTPJPEGHeader) + fragLen;
        std::vector<uint8_t> pkt(pktSize);

        // --- RTP 固定头 ---
        RTPHeader* rtp = reinterpret_cast<RTPHeader*>(pkt.data());
        memset(rtp, 0, sizeof(RTPHeader));
        rtp->version      = 2;
        rtp->padding      = 0;
        rtp->extension    = 0;
        rtp->cc           = 0;
        rtp->marker       = isLast ? 1 : 0;          // 最后一包标记帧结束
        rtp->payload_type = kRtpPayloadTypeJPEG;      // 26
        rtp->sequence     = htons(ci->rtp_seq);
        rtp->timestamp    = htonl(ts);
        rtp->ssrc         = htonl(ci->ssrc);

        // --- RTP JPEG 专有头 (RFC 2435) ---
        RTPJPEGHeader* jpegHdr = reinterpret_cast<RTPJPEGHeader*>(
            pkt.data() + sizeof(RTPHeader));
        jpegHdr->type_specific = 0;
        // Fragment offset: 24 bits, big-endian
        uint32_t fragOff = static_cast<uint32_t>(offset);
        jpegHdr->frag_offset[0] = (fragOff >> 16) & 0xFF;
        jpegHdr->frag_offset[1] = (fragOff >> 8) & 0xFF;
        jpegHdr->frag_offset[2] = fragOff & 0xFF;
        jpegHdr->type        = 0;            // tables in main JPEG header
        jpegHdr->q            = 255;          // Q factor 255 = tables not included
        jpegHdr->width_div8   = static_cast<uint8_t>(width / 8);
        jpegHdr->height_div8  = static_cast<uint8_t>(height / 8);

        // --- JPEG 载荷 ---
        memcpy(pkt.data() + sizeof(RTPHeader) + sizeof(RTPJPEGHeader),
               jpeg + offset, fragLen);

        // --- 发送 ---
        ssize_t sent = sendto(ci->rtp_sock_fd, pkt.data(), pkt.size(), 0,
                              reinterpret_cast<struct sockaddr*>(&ci->rtp_addr),
                              sizeof(ci->rtp_addr));
        if (sent > 0) {
            ci->packet_count++;
            ci->octet_count += static_cast<uint32_t>(sent);
        } else if (sent < 0) {
            // UDP sendto 通常不会失败 (本地发送)
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                LOG_WRN("RTP sendto() failed: %s (fd=%d, seq=%u)",
                        strerror(errno), ci->tcp_fd, ci->rtp_seq);
            }
        }

        // 递增序列号
        ci->rtp_seq++;
    }

    LOG_DBG("RTP: sent frame → %zu fragments, ts=%u, ssrc=0x%08X",
            numFragments, ts, ci->ssrc);
}

// ============================================================
// RTCP SR (Sender Report)
// ============================================================

void RTSPServer::rtcpsSendSR(ClientInfo* ci) {
    if (ci->rtcp_sock_fd < 0) return;

    // NTP 时间戳 (64-bit)
    uint64_t ntp = getNTPTimestamp();
    uint32_t ntp_msw = static_cast<uint32_t>((ntp >> 32) & 0xFFFFFFFF);
    uint32_t ntp_lsw = static_cast<uint32_t>(ntp & 0xFFFFFFFF);

    // 组装 RTCP SR 包
    struct {
        RTCPHeader       hdr;
        RTCPSenderInfo   sender;
    } srPkt;

    memset(&srPkt, 0, sizeof(srPkt));

    // RTCP Header
    srPkt.hdr.version  = 2;
    srPkt.hdr.padding  = 0;
    srPkt.hdr.rc       = 0;    // SR 中 = 0
    srPkt.hdr.pkt_type = 200;  // SR
    srPkt.hdr.length   = htons(6);  // 6 个 32-bit 字 (hdr + sender info)

    // Sender Info
    srPkt.sender.ssrc                = htonl(ci->ssrc);
    srPkt.sender.ntp_timestamp_msw   = htonl(ntp_msw);
    srPkt.sender.ntp_timestamp_lsw   = htonl(ntp_lsw);
    srPkt.sender.rtp_timestamp       = htonl(ci->rtp_ts);
    srPkt.sender.sender_packet_count = htonl(ci->packet_count);
    srPkt.sender.sender_octet_count  = htonl(ci->octet_count);

    sendto(ci->rtcp_sock_fd, &srPkt, sizeof(srPkt), 0,
           reinterpret_cast<struct sockaddr*>(&ci->rtcp_addr),
           sizeof(ci->rtcp_addr));

    ci->last_rtcp_sr = std::chrono::steady_clock::now();
}

void RTSPServer::checkRTCPSR() {
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(m_clientsMtx);
    for (auto& pair : m_clients) {
        ClientInfo& ci = pair.second;
        if (ci.state != PLAYING) continue;

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                           now - ci.last_rtcp_sr).count();
        if (elapsed >= kRTCPSRInterval) {
            rtcpsSendSR(&ci);
        }
    }
}

// ============================================================
// 客户端断开
// ============================================================

void RTSPServer::disconnectClient(int client_fd) {
    {
        std::lock_guard<std::mutex> lock(m_clientsMtx);
        auto it = m_clients.find(client_fd);
        if (it == m_clients.end()) return;

        // 从 epoll 移除
        epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);

        // 关闭所有 socket
        ::close(client_fd);                 // TCP
        if (it->second.rtp_sock_fd >= 0) {
            ::close(it->second.rtp_sock_fd); // RTP UDP
        }
        if (it->second.rtcp_sock_fd >= 0) {
            ::close(it->second.rtcp_sock_fd); // RTCP UDP
        }

        m_clients.erase(it);
    }

    LOG_INF("RTSP client fd=%d disconnected (total=%d)",
            client_fd, clientCount());
}

// ============================================================
// 查询
// ============================================================

int RTSPServer::clientCount() const {
    std::lock_guard<std::mutex> lock(m_clientsMtx);
    return static_cast<int>(m_clients.size());
}

// ============================================================
// SDP 生成
// ============================================================

std::string RTSPServer::buildSDP(const std::string& server_ip) {
    std::ostringstream sdp;

    sdp << "v=0\r\n";
    sdp << "o=- " << time(nullptr) << " 1 IN IP4 " << server_ip << "\r\n";
    sdp << "s=SmartCam Live Stream\r\n";
    sdp << "i=SmartCam MJPEG RTSP Stream\r\n";
    sdp << "c=IN IP4 0.0.0.0\r\n";
    sdp << "t=0 0\r\n";
    sdp << "a=control:*\r\n";
    sdp << "a=range:npt=0-\r\n";
    sdp << "m=video 0 RTP/AVP 26\r\n";
    sdp << "a=control:track0\r\n";
    sdp << "a=rtpmap:26 JPEG/90000\r\n";
    sdp << "a=fmtp:26 width=" << m_streamWidth
        << ";height=" << m_streamHeight << "\r\n";
    sdp << "a=framerate:" << m_streamFPS << ".0\r\n";

    return sdp.str();
}

// ============================================================
// 工具函数
// ============================================================

std::string RTSPServer::generateSessionID() {
    static std::mutex mtx;
    static int counter = 0;

    std::lock_guard<std::mutex> lock(mtx);
    counter++;

    char buf[32];
    snprintf(buf, sizeof(buf), "%010d%06d",
             static_cast<int>(time(nullptr)), counter);
    return std::string(buf);
}

uint64_t RTSPServer::getNTPTimestamp() {
    // NTP epoch = Jan 1, 1900
    // Unix epoch = Jan 1, 1970
    // offset = 2208988800 seconds
    constexpr uint64_t kNTPEpochOffset = 2208988800ULL;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    uint64_t ntpSec  = static_cast<uint64_t>(ts.tv_sec) + kNTPEpochOffset;
    // 将纳秒转换为 NTP 小数部分 (2^32 * ns / 1e9)
    uint64_t ntpFrac = static_cast<uint64_t>(ts.tv_nsec) * 4294967296ULL / 1000000000ULL;

    return (ntpSec << 32) | ntpFrac;
}
