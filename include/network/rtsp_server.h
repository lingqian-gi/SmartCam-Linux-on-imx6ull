#ifndef SMART_CAM_RTSP_SERVER_H
#define SMART_CAM_RTSP_SERVER_H

/**
 * @file    rtsp_server.h
 * @brief   RTSP/RTP 实时流服务器
 *
 * 基于 RFC 2326 (RTSP 1.0)、RFC 3550 (RTP)、RFC 2435 (RTP JPEG) 实现。
 * 不依赖任何第三方库，纯 C++ + POSIX socket API。
 *
 * 支持的 RTSP 方法: OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN
 *
 * 用法:
 * @code
 *   RTSPServer server;
 *   server.setStreamInfo(640, 480, 30);      // 分辨率 + 帧率
 *   if (server.start(8554) == 0) {
 *       // 采集线程每获取一帧后调用
 *       server.feedFrame(jpeg_data, jpeg_len, 640, 480);
 *   }
 *   server.stop();
 * @endcode
 *
 * 播放:
 *   ffplay rtsp://192.168.1.100:8554/stream
 *   vlc    rtsp://192.168.1.100:8554/stream
 */

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <netinet/in.h>

// ============================================================
// 常量
// ============================================================

/// RTSP 默认端口
static constexpr int kRTSPDefaultPort = 8554;

/// 默认积压连接数
static constexpr int kRTSPBacklog = 8;

/// RTP 载荷类型 (PT=26 = JPEG)
static constexpr uint8_t kRtpPayloadTypeJPEG = 26;

/// RTP 时间戳时钟频率 (JPEG = 90,000 Hz, RFC 3551)
static constexpr uint32_t kRtpClockRate = 90000;

/// UDP MTU 安全值（以太网 1500 - IP头20 - UDP头8 = 1472，留余量）
static constexpr size_t kRtpMaxPayload = 1400;

/// RTCP SR 发送间隔 (秒)
static constexpr int kRTCPSRInterval = 5;

/// 默认帧率
static constexpr int kDefaultFPS = 30;

// ============================================================
// RTP 固定头 (RFC 3550 §5.1)
// ============================================================

#pragma pack(push, 1)
struct RTPHeader {
    /** @brief 第 1 个 32-bit 字 */
    uint8_t  cc       : 4;   // CSRC count
    uint8_t  extension : 1;   // 扩展头
    uint8_t  padding   : 1;   // 填充
    uint8_t  version   : 2;   // 协议版本 = 2
    /** @brief 第 1 个 32-bit 字 (续) */
    uint8_t  payload_type : 7; // 载荷类型
    uint8_t  marker        : 1; // 标记位 (帧结束 = 1)
    /** @brief 序列号 (网络字节序) */
    uint16_t sequence;
    /** @brief 时间戳 (网络字节序) */
    uint32_t timestamp;
    /** @brief 同步源标识 */
    uint32_t ssrc;
};
#pragma pack(pop)

// ============================================================
// RTP JPEG 专有头 (RFC 2435 §3.1)
// ============================================================

#pragma pack(push, 1)
struct RTPJPEGHeader {
    /** @brief 类型专用字段 */
    uint8_t  type_specific;    // 0 for baseline JPEG

    /** @brief 片段偏移 (24 bits, big-endian) */
    uint8_t  frag_offset[3];   // MSB first

    /** @brief 类型 + Q 因子 */
    uint8_t  type;              // 0 = table-spec in main JPEG header
    uint8_t  q;                 // 255 = tables not included (they're in JPEG)

    /** @brief 图像尺寸 (width/8, height/8) */
    uint8_t  width_div8;
    uint8_t  height_div8;
};
#pragma pack(pop)

// ============================================================
// RTCP (SR) 发送者报告 (RFC 3550 §6.4)
// ============================================================

#pragma pack(push, 1)
struct RTCPHeader {
    uint8_t  version   : 2;   // 2
    uint8_t  padding   : 1;
    uint8_t  rc        : 5;   // reception report count (0 for SR)
    uint8_t  pkt_type;        // 200 = SR
    uint16_t length;          // 长度 (网络字节序, 32-bit 字数 - 1)
};

struct RTCPSenderInfo {
    uint32_t ssrc;                         // 同步源
    uint32_t ntp_timestamp_msw;            // NTP 高 32 位
    uint32_t ntp_timestamp_lsw;            // NTP 低 32 位
    uint32_t rtp_timestamp;                // 对应的 RTP 时间戳
    uint32_t sender_packet_count;          // 累计发送包数
    uint32_t sender_octet_count;           // 累计发送字节数
};
#pragma pack(pop)

// ============================================================
// RTSPServer 类
// ============================================================

class RTSPServer {
public:
    RTSPServer();
    ~RTSPServer();

    // 禁用拷贝
    RTSPServer(const RTSPServer&) = delete;
    RTSPServer& operator=(const RTSPServer&) = delete;

    // ============================================================
    // 生命周期
    // ============================================================

    /**
     * @brief 设置流信息（分辨率 + 帧率），用于 SDP 和 RTP JPEG 头
     */
    void setStreamInfo(int width, int height, int fps = kDefaultFPS);

    /**
     * @brief 启动 RTSP 服务器（阻塞式事件循环，需在独立线程中调用）
     * @param port  监听端口 (默认 8554)
     * @return 0 成功，-1 失败
     */
    int start(int port = kRTSPDefaultPort);

    /**
     * @brief 停止服务器
     */
    void stop();

    /** @brief 是否正在运行 */
    bool isRunning() const { return m_running; }

    /** @brief 获取监听端口 */
    int port() const { return m_port; }

    /** @brief 获取当前客户端数量 */
    int clientCount() const;

    // ============================================================
    // 推帧接口（采集线程调用）
    // ============================================================

    /**
     * @brief 馈入一帧 JPEG 数据，推送到所有 PLAYING 客户端
     *
     * 线程安全，可从采集线程直接调用。
     * 内部对 JPEG 帧进行 RTP 分片并通过 UDP sendto() 发送。
     *
     * @param jpeg_data  JPEG 帧数据
     * @param len        数据长度
     * @param width      图像宽度 (用于 RTP JPEG 头)
     * @param height     图像高度 (用于 RTP JPEG 头)
     */
    void feedFrame(const uint8_t* jpeg_data, size_t len,
                   int width, int height);

private:
    // ============================================================
    // 客户端状态
    // ============================================================

    enum ClientState { INIT, READY, PLAYING };

    struct ClientInfo {
        int         tcp_fd;          // RTSP 控制连接
        int         rtp_sock_fd;     // UDP socket for sending RTP
        int         rtcp_sock_fd;    // UDP socket for sending RTCP
        struct sockaddr_in rtp_addr;  // 客户端 RTP 目标地址
        struct sockaddr_in rtcp_addr; // 客户端 RTCP 目标地址
        std::string  session_id;
        std::string  recv_buf;       // TCP 接收缓冲区
        ClientState  state = INIT;
        int          cseq = 0;       // 最后一个 CSeq

        // RTP 状态 (per-client)
        uint16_t     rtp_seq = 0;
        uint32_t     rtp_ts  = 0;
        uint32_t     ssrc   = 0;
        uint32_t     packet_count = 0;
        uint32_t     octet_count  = 0;

        // RTCP 计时
        std::chrono::steady_clock::time_point last_rtcp_sr;
    };

    // ============================================================
    // 网络
    // ============================================================

    int  createSocket(int port);
    void eventLoop();
    void acceptClient();
    void handleClientData(int client_fd);
    void disconnectClient(int client_fd);

    // ============================================================
    // RTSP 方法处理器
    // ============================================================

    void handleOptions(int client_fd, int cseq);
    void handleDescribe(int client_fd, int cseq, const std::string& uri);
    void handleSetup(int client_fd, int cseq, const std::string& uri,
                     const std::string& transport);
    void handlePlay(int client_fd, int cseq, const std::string& uri,
                    const std::string& session);
    void handleTeardown(int client_fd, int cseq, const std::string& session);

    // ============================================================
    // RTSP 请求解析
    // ============================================================

    bool parseRequest(const std::string& buf, std::string* method,
                      std::string* uri, int* cseq,
                      std::map<std::string, std::string>* headers);

    // ============================================================
    // RTP / RTCP
    // ============================================================

    /**
     * @brief 发送一帧 JPEG 到一个客户端（RTP 分片）
     */
    void rtpSendFrame(ClientInfo* ci, const uint8_t* jpeg, size_t len,
                      int width, int height);

    /**
     * @brief 发送单个 RTP 包
     */
    ssize_t rtpSendPacket(ClientInfo* ci, const uint8_t* payload, size_t plen,
                          uint32_t ts, bool marker);

    /**
     * @brief 发送 RTCP SR (Sender Report)
     */
    void rtcpsSendSR(ClientInfo* ci);

    /**
     * @brief 检查并定时发送 RTCP SR
     */
    void checkRTCPSR();

    // ============================================================
    // RTSP 响应发送
    // ============================================================

    void sendRTSPResponse(int client_fd, int cseq,
                          int status_code, const char* reason,
                          const std::map<std::string, std::string>& headers,
                          const std::string& body = "");

    // ============================================================
    // 工具
    // ============================================================

    std::string buildSDP(const std::string& server_ip);
    std::string generateSessionID();
    static uint64_t getNTPTimestamp();

    // ============================================================
    // 成员变量
    // ============================================================

    int  m_server_fd = -1;
    int  m_port      = 0;
    int  m_epoll_fd  = -1;
    std::atomic<bool> m_running{false};

    // 流参数
    int m_streamWidth  = 640;
    int m_streamHeight = 480;
    int m_streamFPS    = 30;
    uint32_t m_tsPerFrame = 3000;  // 90000 / fps

    // 客户端管理
    mutable std::mutex m_clientsMtx;
    std::map<int, ClientInfo> m_clients;

    // 当前帧（采集线程写入）
    std::mutex m_frameMtx;
    std::vector<uint8_t> m_latestJpeg;
    int m_latestWidth  = 0;
    int m_latestHeight = 0;
    std::atomic<bool> m_hasFrame{false};
};

#endif // SMART_CAM_RTSP_SERVER_H
