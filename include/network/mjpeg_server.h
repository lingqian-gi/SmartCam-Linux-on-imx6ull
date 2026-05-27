#ifndef SMART_CAM_MJPEG_SERVER_H
#define SMART_CAM_MJPEG_SERVER_H

/**
 * @file    mjpeg_server.h
 * @brief   MJPEG-over-HTTP 流媒体服务器
 *
 * 使用 HTTP multipart/x-mixed-replace 协议，浏览器可直接通过
 * <img src="http://ip:port/stream"> 或打开 http://ip:port/ 观看实时画面。
 *
 * 用法:
 * @code
 *   MJPEGStreamServer server;
 *   if (server.start(8080) == 0) {
 *       // 采集线程每获取一帧后调用
 *       server.updateFrame(jpeg_data, jpeg_len);
 *   }
 *   server.stop();
 * @endcode
 */

#include <cstdint>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <string>
#include <functional>

/**
 * @brief MJPEG-over-HTTP 流媒体服务器
 *
 * 架构:
 *   - 1 个 accept 线程：接受 TCP 连接
 *   - 每个客户端 1 个线程：发送 multipart 流 / snapshot / status
 *   - 采集线程调用 updateFrame() 推帧
 *   - 条件变量广播新帧到所有客户端线程
 *
 * 路由:
 *   GET /           → HTML 页面
 *   GET /stream     → MJPEG 无限流 (multipart/x-mixed-replace)
 *   GET /snapshot   → 单帧 JPEG 快照 (image/jpeg)
 *   GET /status     → JSON 设备状态 (application/json)
 */

/**
 * @brief 状态提供回调类型（用于 GET /status）
 *
 * 回调应在被调用时填充以下字段:
 *   - streaming:     是否正在推流
 *   - recording:     是否正在录像
 *   - width/height:  当前分辨率
 *   - format:        像素格式字符串 ("YUYV" / "MJPEG")
 *   - fps:           当前帧率
 *   - client_count:  HTTP 在线客户端数
 *   - uptime_seconds: 程序运行时间（秒）
 */
struct StreamStatus {
    bool        streaming      = false;
    bool        recording      = false;
    int         width          = 0;
    int         height         = 0;
    std::string format         = "N/A";
    double      fps            = 0.0;
    int         client_count   = 0;
    int         uptime_seconds = 0;
};

using StreamStatusProvider = std::function<StreamStatus()>;

class MJPEGStreamServer {
public:
    MJPEGStreamServer();
    ~MJPEGStreamServer();

    // 禁用拷贝
    MJPEGStreamServer(const MJPEGStreamServer&) = delete;
    MJPEGStreamServer& operator=(const MJPEGStreamServer&) = delete;

    /**
     * @brief 启动 HTTP 服务器
     * @param port  监听端口（默认 8080）
     * @return 0 成功，-1 失败
     */
    int start(int port = 8080);

    /**
     * @brief 停止服务器，断开所有客户端
     */
    void stop();

    /** @brief 是否正在运行 */
    bool isRunning() const { return m_running; }

    /** @brief 获取监听端口 */
    int port() const { return m_port; }

    /** @brief 获取当前客户端数量 */
    int clientCount() const;

    /**
     * @brief 更新最新一帧 JPEG 数据（采集线程调用，线程安全）
     * @param data  JPEG 帧数据（拷贝到内部缓冲区）
     * @param len   数据长度（字节）
     */
    void updateFrame(const uint8_t* data, size_t len);

    /**
     * @brief 设置 GET /status 的状态提供回调
     *
     * 回调会在每次 /status 请求时被调用（在客户端线程中）。
     * 调用者需自行保证回调中访问的数据的线程安全。
     *
     * @param provider  状态提供回调
     */
    void setStatusProvider(StreamStatusProvider provider);

private:
    /** @brief 客户端连接信息 */
    struct ClientInfo {
        int          fd;               // socket 文件描述符
        bool         active;           // 是否活跃
        uint64_t     lastSentIndex;    // 已发送的最新帧序号
        int          quality = 100;    // JPEG 质量 (1-100), 100=直通不重编码
    };

    // ---- 线程入口 ----
    void acceptLoop();
    void clientHandler(int client_fd);

    // ---- HTTP 处理 ----
    bool sendHttpHeader(int client_fd);
    bool sendMJPEGFrame(int client_fd, const uint8_t* jpeg, size_t len);
    bool sendIndexPage(int client_fd);
    bool sendSnapshot(int client_fd);
    bool sendStatusJSON(int client_fd);
    bool readHttpRequest(int client_fd, char* buf, size_t bufsize);

    // ---- 客户端管理 ----
    void addClient(int client_fd);
    void removeClient(int fd);
    void cleanupDisconnected();

    // ---- 网络 ----
    int                m_server_fd;
    int                m_port;
    std::atomic<bool>  m_running;

    // ---- 接收线程 ----
    std::thread*       m_acceptThread;

    // ---- 最新帧（生产者-消费者模型） ----
    std::mutex              m_frameMtx;
    std::condition_variable m_frameCV;
    std::vector<uint8_t>    m_currentFrame;
    std::atomic<uint64_t>   m_frameIndex{0};

    // ---- 质量缓存: quality → 重编码后的 JPEG（按需生成，多客户端共享） ----
    std::mutex                       m_qualityCacheMtx;
    std::map<int, std::vector<uint8_t>> m_qualityCache;  // quality → JPEG bytes

    // ---- 客户端管理 ----
    mutable std::mutex  m_clientsMtx;
    std::vector<ClientInfo> m_clients;

    // ---- 状态提供回调 ----
    StreamStatusProvider m_statusProvider;
};

#endif // SMART_CAM_MJPEG_SERVER_H
