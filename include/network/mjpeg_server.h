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
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

/**
 * @brief MJPEG-over-HTTP 流媒体服务器
 *
 * 架构:
 *   - 1 个 accept 线程：接受 TCP 连接
 *   - 每个客户端 1 个线程：发送 multipart 流
 *   - 采集线程调用 updateFrame() 推帧
 *   - 条件变量广播新帧到所有客户端线程
 */
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

private:
    /** @brief 客户端连接信息 */
    struct ClientInfo {
        int          fd;               // socket 文件描述符
        bool         active;           // 是否活跃
        uint64_t     lastSentIndex;    // 已发送的最新帧序号
    };

    // ---- 线程入口 ----
    void acceptLoop();
    void clientHandler(int client_fd);

    // ---- HTTP 处理 ----
    bool sendHttpHeader(int client_fd);
    bool sendMJPEGFrame(int client_fd, const uint8_t* jpeg, size_t len);
    bool sendIndexPage(int client_fd);
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

    // ---- 客户端管理 ----
    mutable std::mutex  m_clientsMtx;
    std::vector<ClientInfo> m_clients;
};

#endif // SMART_CAM_MJPEG_SERVER_H
