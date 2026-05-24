/**
 * @file    mjpeg_server.cpp
 * @brief   MJPEG-over-HTTP 流媒体服务器实现
 *
 * 实现要点:
 *   1. 使用 multipart/x-mixed-replace MIME 类型实现无限推送
 *   2. accept 线程 + 每客户端一线程模型
 *   3. 条件变量广播新帧到所有活跃客户端
 *   4. 支持 GET / → HTML 页面, GET /stream → MJPEG 流
 *   5. 客户端断开自动清理（延迟清理避免线程竞争）
 */

#include "include/network/mjpeg_server.h"
#include "include/common/logger.h"

#include <cstring>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

// ============================================================
// multipart 边界字符串
// ============================================================
static constexpr const char* kBoundary  = "SmartCamFrame";
static constexpr const char* kCRLF      = "\r\n";

// ============================================================
// 构造 / 析构
// ============================================================

MJPEGStreamServer::MJPEGStreamServer()
    : m_server_fd(-1)
    , m_port(0)
    , m_running(false)
    , m_acceptThread(nullptr)
{
}

MJPEGStreamServer::~MJPEGStreamServer() {
    stop();
}

// ============================================================
// 启动 / 停止
// ============================================================

int MJPEGStreamServer::start(int port) {
    if (m_running) {
        LOG_WRN("MJPEG server already running on port %d", m_port);
        return 0;
    }

    m_port = port;

    // ---- 创建 TCP socket ----
    m_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_server_fd < 0) {
        LOG_ERR_("socket() failed: %s", strerror(errno));
        return -1;
    }

    // SO_REUSEADDR — 允许快速重启
    int reuse = 1;
    setsockopt(m_server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // ---- bind ----
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port));

    if (bind(m_server_fd, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) < 0) {
        LOG_ERR_("bind(port=%d) failed: %s", port, strerror(errno));
        ::close(m_server_fd);
        m_server_fd = -1;
        return -1;
    }

    // ---- listen ----
    if (listen(m_server_fd, 8) < 0) {
        LOG_ERR_("listen() failed: %s", strerror(errno));
        ::close(m_server_fd);
        m_server_fd = -1;
        return -1;
    }

    // ---- 启动 accept 线程 ----
    m_running = true;
    m_acceptThread = new std::thread(&MJPEGStreamServer::acceptLoop, this);

    LOG_INF("MJPEG-over-HTTP server started on port %d", port);
    LOG_INF("  → Open http://<board-ip>:%d/ in browser", port);
    LOG_INF("  → Or use <img src=\"http://<board-ip>:%d/stream\">", port);
    return 0;
}

void MJPEGStreamServer::stop() {
    if (!m_running) return;

    LOG_INF("Stopping MJPEG server (port %d)...", m_port);
    m_running = false;

    // 1. 关闭 server socket → accept() 立即返回
    if (m_server_fd >= 0) {
        ::close(m_server_fd);
        m_server_fd = -1;
    }

    // 2. 通知所有客户端线程退出
    m_frameCV.notify_all();

    // 3. 等待 accept 线程结束
    if (m_acceptThread && m_acceptThread->joinable()) {
        m_acceptThread->join();
        delete m_acceptThread;
        m_acceptThread = nullptr;
    }

    // 4. 关闭所有客户端连接（线程已 detached，自动退出）
    {
        std::lock_guard<std::mutex> lock(m_clientsMtx);
        for (auto& c : m_clients) {
            if (c.fd >= 0) {
                ::close(c.fd);
                c.fd = -1;
            }
        }
        m_clients.clear();
    }

    LOG_INF("MJPEG server stopped");
}

// ============================================================
// 公共接口
// ============================================================

int MJPEGStreamServer::clientCount() const {
    std::lock_guard<std::mutex> lock(m_clientsMtx);
    int count = 0;
    for (const auto& c : m_clients) {
        if (c.active) ++count;
    }
    return count;
}

void MJPEGStreamServer::updateFrame(const uint8_t* data, size_t len) {
    if (!m_running || !data || len == 0) return;

    {
        std::lock_guard<std::mutex> lock(m_frameMtx);
        m_currentFrame.assign(data, data + len);
        m_frameIndex++;
    }

    // 广播新帧到所有等待的客户端线程
    m_frameCV.notify_all();
}

void MJPEGStreamServer::setStatusProvider(StreamStatusProvider provider) {
    m_statusProvider = std::move(provider);
}

// ============================================================
// 接受线程
// ============================================================

void MJPEGStreamServer::acceptLoop() {
    LOG_INF("Accept thread started");

    while (m_running) {
        struct sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);

        int client_fd = accept(m_server_fd,
                               reinterpret_cast<struct sockaddr*>(&clientAddr),
                               &addrLen);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (!m_running) break;
            LOG_ERR_("accept() failed: %s", strerror(errno));
            continue;
        }

        // 获取客户端 IP 用于日志
        char clientIP[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, sizeof(clientIP));
        LOG_INF("Client connected: %s:%d (total=%d)",
                clientIP, ntohs(clientAddr.sin_port), clientCount() + 1);

        // 启动客户端处理线程
        addClient(client_fd);
    }

    LOG_INF("Accept thread exited");
}

// ============================================================
// 客户端处理线程
// ============================================================

void MJPEGStreamServer::clientHandler(int client_fd) {
    // ---- 1. 读取 HTTP 请求 ----
    char reqBuf[4096];
    if (!readHttpRequest(client_fd, reqBuf, sizeof(reqBuf))) {
        LOG_DBG("Client disconnected before sending request");
        removeClient(client_fd);
        return;
    }

    // ---- 2. 解析请求路径 ----
    bool wantStream   = false;
    bool wantSnapshot = false;
    bool wantStatus   = false;
    const char* line = reqBuf;

    // 第一行格式: GET /path HTTP/1.1
    if (std::strncmp(line, "GET ", 4) == 0) {
        const char* pathStart = line + 4;
        const char* pathEnd   = std::strchr(pathStart, ' ');
        if (pathEnd) {
            size_t pathLen = static_cast<size_t>(pathEnd - pathStart);

            if (pathLen == 1 && pathStart[0] == '/') {
                // GET / → 返回 HTML 页面
            } else if (pathLen == 7 &&
                       std::strncmp(pathStart, "/stream", 7) == 0) {
                wantStream = true;
            } else if (pathLen == 9 &&
                       std::strncmp(pathStart, "/snapshot", 9) == 0) {
                wantSnapshot = true;
            } else if (pathLen == 7 &&
                       std::strncmp(pathStart, "/status", 7) == 0) {
                wantStatus = true;
            }
            // 其他路径 → sendIndexPage (当作 404 展示)
        }
    }

    // ---- 3. 发送响应 ----
    if (wantSnapshot) {
        // 返回单帧 JPEG 快照
        if (!sendSnapshot(client_fd)) {
            LOG_DBG("Failed to send snapshot");
        }
        removeClient(client_fd);
        return;
    }

    if (wantStatus) {
        // 返回 JSON 设备状态
        if (!sendStatusJSON(client_fd)) {
            LOG_DBG("Failed to send status JSON");
        }
        removeClient(client_fd);
        return;
    }

    if (wantStream) {
        // 发送 MJPEG 流
        if (!sendHttpHeader(client_fd)) {
            LOG_DBG("Failed to send HTTP header to client");
            removeClient(client_fd);
            return;
        }

        // 获取当前客户端信息来更新 lastSentIndex
        {
            std::lock_guard<std::mutex> lock(m_clientsMtx);
            for (auto& c : m_clients) {
                if (c.fd == client_fd) {
                    c.lastSentIndex = m_frameIndex.load();
                    break;
                }
            }
        }

        // 流发送循环
        uint64_t lastIndex = 0;
        while (m_running) {
            std::vector<uint8_t> frame;
            uint64_t currentIndex;

            {
                std::unique_lock<std::mutex> lock(m_frameMtx);

                // 等待新帧（超时 1s 以便检查 m_running）
                if (!m_frameCV.wait_for(lock, std::chrono::seconds(1),
                        [this, &lastIndex = lastIndex]() {
                            return !m_running ||
                                   m_frameIndex.load() != lastIndex;
                        })) {
                    // 超时，检查是否需要退出
                    if (!m_running) break;
                    continue;
                }

                if (!m_running) break;

                // 有新帧，拷贝出来（在锁外发送）
                frame.assign(m_currentFrame.begin(), m_currentFrame.end());
                currentIndex = m_frameIndex.load();
            }

            lastIndex = currentIndex;

            if (frame.empty()) continue;

            if (!sendMJPEGFrame(client_fd, frame.data(), frame.size())) {
                LOG_DBG("Client disconnected (send failed)");
                break;
            }
        }
    } else {
        // 返回 HTML 页面或 404
        if (!sendIndexPage(client_fd)) {
            LOG_DBG("Failed to send index page");
        }
    }

    // ---- 4. 清理（removeClient 已包含 close） ----
    removeClient(client_fd);
    LOG_INF("Client disconnected (total=%d)", clientCount());
}

// ============================================================
// HTTP 请求读取
// ============================================================

bool MJPEGStreamServer::readHttpRequest(int client_fd, char* buf,
                                         size_t bufsize) {
    // 使用 select 超时读取，避免永久阻塞
    fd_set readfds;
    struct timeval tv;
    tv.tv_sec  = 5;
    tv.tv_usec = 0;

    FD_ZERO(&readfds);
    FD_SET(client_fd, &readfds);

    int ret = select(client_fd + 1, &readfds, nullptr, nullptr, &tv);
    if (ret <= 0) return false;

    ssize_t n = read(client_fd, buf, bufsize - 1);
    if (n <= 0) return false;

    buf[n] = '\0';
    return true;
}

// ============================================================
// 发送 HTTP 响应头（multipart）
// ============================================================

bool MJPEGStreamServer::sendHttpHeader(int client_fd) {
    char header[512];
    int len = snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=%s\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Server: SmartCam/0.1\r\n"
        "\r\n",
        kBoundary);

    if (len <= 0) return false;

    ssize_t sent = write(client_fd, header, static_cast<size_t>(len));
    return (sent == len);
}

// ============================================================
// 发送一帧 MJPEG（multipart 格式）
// ============================================================

bool MJPEGStreamServer::sendMJPEGFrame(int client_fd,
                                        const uint8_t* jpeg,
                                        size_t len) {
    // 使用 writev 或多次 write 发送 multipart 头部 + 数据
    // 头部格式:
    // --boundary\r\n
    // Content-Type: image/jpeg\r\n
    // Content-Length: N\r\n
    // \r\n
    // [JPEG DATA]\r\n

    char header[256];
    int headerLen = snprintf(header, sizeof(header),
        "--%s\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: %zu\r\n"
        "\r\n",
        kBoundary, len);

    if (headerLen <= 0) return false;

    // 发送 header
    ssize_t sent = write(client_fd, header, static_cast<size_t>(headerLen));
    if (sent != headerLen) return false;

    // 发送 JPEG 数据
    const uint8_t* ptr = jpeg;
    size_t remaining = len;
    while (remaining > 0) {
        sent = write(client_fd, ptr, remaining);
        if (sent <= 0) return false;
        ptr += sent;
        remaining -= static_cast<size_t>(sent);
    }

    // 发送尾部 CRLF（每个 part 结束后需要）
    sent = write(client_fd, kCRLF, 2);
    if (sent != 2) return false;

    return true;
}

// ============================================================
// 发送单帧 JPEG 快照（GET /snapshot）
// ============================================================

bool MJPEGStreamServer::sendSnapshot(int client_fd) {
    std::vector<uint8_t> frame;

    {
        std::lock_guard<std::mutex> lock(m_frameMtx);
        if (m_currentFrame.empty()) {
            // 尚无帧数据，返回 503 Service Unavailable
            const char* noContent =
                "HTTP/1.0 503 Service Unavailable\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n"
                "\r\n"
                "No frame available yet.\r\n";
            size_t l = std::strlen(noContent);
            ssize_t ignored = write(client_fd, noContent, l);
            (void)ignored;
            return true;
        }
        frame = m_currentFrame;
    }

    // 构造 HTTP 响应头
    char header[256];
    int headerLen = snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Server: SmartCam/0.1\r\n"
        "\r\n",
        frame.size());

    if (headerLen <= 0) return false;

    // 发送 header
    ssize_t sent = write(client_fd, header, static_cast<size_t>(headerLen));
    if (sent != headerLen) return false;

    // 发送 JPEG 数据
    const uint8_t* ptr = frame.data();
    size_t remaining = frame.size();
    while (remaining > 0) {
        sent = write(client_fd, ptr, remaining);
        if (sent <= 0) return false;
        ptr += sent;
        remaining -= static_cast<size_t>(sent);
    }

    LOG_DBG("Snapshot sent: %zu bytes", frame.size());
    return true;
}

// ============================================================
// 发送 JSON 设备状态（GET /status）
// ============================================================

bool MJPEGStreamServer::sendStatusJSON(int client_fd) {
    // 构建 JSON 状态
    std::string json;

    if (m_statusProvider) {
        StreamStatus st = m_statusProvider();

        char buf[512];
        // 手工拼 JSON（零依赖，适合嵌入式）
        int len = snprintf(buf, sizeof(buf),
            "{\r\n"
            "  \"streaming\": %s,\r\n"
            "  \"recording\": %s,\r\n"
            "  \"width\": %d,\r\n"
            "  \"height\": %d,\r\n"
            "  \"format\": \"%s\",\r\n"
            "  \"fps\": %.1f,\r\n"
            "  \"clients\": %d,\r\n"
            "  \"uptime_seconds\": %d\r\n"
            "}\r\n",
            st.streaming  ? "true" : "false",
            st.recording  ? "true" : "false",
            st.width,
            st.height,
            st.format.c_str(),
            st.fps,
            st.client_count,
            st.uptime_seconds);

        if (len > 0 && static_cast<size_t>(len) < sizeof(buf)) {
            json.assign(buf, static_cast<size_t>(len));
        }
    }

    if (json.empty()) {
        json = "{ \"error\": \"status not available\" }\r\n";
    }

    // 构造 HTTP 响应
    char header[256];
    int headerLen = snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Server: SmartCam/0.1\r\n"
        "\r\n",
        json.size());

    if (headerLen <= 0) return false;

    // 发送 header
    ssize_t sent = write(client_fd, header, static_cast<size_t>(headerLen));
    if (sent != headerLen) return false;

    // 发送 JSON body
    sent = write(client_fd, json.data(), json.size());
    return (sent == static_cast<ssize_t>(json.size()));
}

// ============================================================
// 发送 HTML 索引页
// ============================================================

bool MJPEGStreamServer::sendIndexPage(int client_fd) {
    const char* html =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "<meta charset='utf-8'>\n"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>\n"
        "<title>SmartCam — 实时画面</title>\n"
        "<style>\n"
        "  * { margin: 0; padding: 0; box-sizing: border-box; }\n"
        "  body {\n"
        "    background: #0a0a1a;\n"
        "    color: #e0e0e0;\n"
        "    font-family: -apple-system, 'Segoe UI', sans-serif;\n"
        "    display: flex;\n"
        "    flex-direction: column;\n"
        "    align-items: center;\n"
        "    min-height: 100vh;\n"
        "    padding: 20px;\n"
        "  }\n"
        "  h1 {\n"
        "    font-size: 24px;\n"
        "    margin: 10px 0;\n"
        "    color: #0f3460;\n"
        "    letter-spacing: 2px;\n"
        "  }\n"
        "  .viewer {\n"
        "    background: #1a1a2e;\n"
        "    border: 2px solid #0f3460;\n"
        "    border-radius: 8px;\n"
        "    padding: 4px;\n"
        "    max-width: 100%;\n"
        "  }\n"
        "  .viewer img {\n"
        "    display: block;\n"
        "    max-width: 100%;\n"
        "    height: auto;\n"
        "    border-radius: 4px;\n"
        "  }\n"
        "  .status {\n"
        "    margin-top: 12px;\n"
        "    font-size: 14px;\n"
        "    color: #888;\n"
        "  }\n"
        "  .status .dot {\n"
        "    display: inline-block;\n"
        "    width: 10px;\n"
        "    height: 10px;\n"
        "    border-radius: 50%;\n"
        "    background: #2ecc71;\n"
        "    margin-right: 6px;\n"
        "    animation: pulse 1.5s ease-in-out infinite;\n"
        "  }\n"
        "  .api-links {\n"
        "    margin-top: 16px;\n"
        "    font-size: 12px;\n"
        "    color: #555;\n"
        "  }\n"
        "  .api-links a {\n"
        "    color: #0f3460;\n"
        "    text-decoration: none;\n"
        "    margin: 0 6px;\n"
        "  }\n"
        "  .api-links a:hover { text-decoration: underline; }\n"
        "  @keyframes pulse {\n"
        "    0%, 100% { opacity: 1; }\n"
        "    50% { opacity: 0.4; }\n"
        "  }\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<h1>SmartCam</h1>\n"
        "<div class='viewer'>\n"
        "  <img src='/stream' alt='实时画面加载中...'>\n"
        "</div>\n"
        "<div class='status'>\n"
        "  <span class='dot'></span>LIVE\n"
        "</div>\n"
        "<div class='api-links'>\n"
        "  API: <a href='/snapshot'>snapshot</a>"
        " | <a href='/status'>status</a>\n"
        "</div>\n"
        "</body>\n"
        "</html>\n";

    size_t len = std::strlen(html);
    ssize_t sent = write(client_fd, html, len);
    return (sent == static_cast<ssize_t>(len));
}

// ============================================================
// 客户端管理
// ============================================================

void MJPEGStreamServer::addClient(int client_fd) {
    // 设置为非阻塞（可选，帮助检测断开）
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    // 启动处理线程（detach，由客户端自行退出）
    std::thread t(&MJPEGStreamServer::clientHandler, this, client_fd);
    t.detach();

    std::lock_guard<std::mutex> lock(m_clientsMtx);
    ClientInfo info;
    info.fd     = client_fd;
    info.active = true;
    info.lastSentIndex = m_frameIndex.load();
    m_clients.push_back(info);
}

void MJPEGStreamServer::removeClient(int fd) {
    std::lock_guard<std::mutex> lock(m_clientsMtx);
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->fd == fd) {
            if (it->fd >= 0) ::close(it->fd);
            m_clients.erase(it);
            break;
        }
    }
}

void MJPEGStreamServer::cleanupDisconnected() {
    // 标记客户端不活跃后延迟清理（当前简化：在移除时立即清理）
}
