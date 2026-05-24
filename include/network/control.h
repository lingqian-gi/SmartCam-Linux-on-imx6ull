#ifndef SMART_CAM_NETWORK_CONTROL_H
#define SMART_CAM_NETWORK_CONTROL_H

/**
 * @file    control.h
 * @brief   TCP 私有二进制控制协议服务器
 *
 * 基于设计文档 §3.5 实现，使用自定义紧凑二进制协议，
 * 支持远程拍照、录像控制、参数设置、状态查询和心跳保活。
 *
 * 协议帧格式:
 *   [magic:2][version:1][cmd:1][payload_len:2][payload:N][crc16:2]
 *
 * 架构:
 *   - epoll 边缘触发，非阻塞 I/O
 *   - 每客户端独立接收缓冲区（处理 TCP 粘包/拆包）
 *   - 命令处理函数表（类似虚函数表，扩展友好）
 *   - 心跳超时断开（默认 30s）
 *
 * 典型用法:
 * @code
 *   ControlServer ctrl;
 *   ctrl.setStatusProvider([&](StatusPayload& s) {
 *       s.streaming = capture->isStreaming();
 *       s.recording = storage->isRecording();
 *       s.width  = 640;
 *       s.height = 480;
 *       s.format = 1;   // MJPEG
 *       s.fps    = 30;
 *       s.client_count = mjpegServer->clientCount();
 *   });
 *   ctrl.setCommandHandler(CMD_CAPTURE, [&](auto req, auto reqLen, auto* resp, auto* respLen) {
 *       storage->savePhoto(jpeg_data, len);
 *       return STATUS_OK;
 *   });
 *   ctrl.start(9000);
 *   // ... 在另一个线程运行 ...
 *   ctrl.stop();
 * @endcode
 */

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

// ============================================================
// 协议常量
// ============================================================

/// 魔数（帧头标识）
static constexpr uint8_t kProtoMagic0 = 0xEB;
static constexpr uint8_t kProtoMagic1 = 0x90;

/// 协议版本
static constexpr uint8_t kProtoVersion = 0x01;

/// 最小帧长度: magic[2] + version[1] + cmd[1] + payload_len[2] + crc16[2] = 8
static constexpr int kMinFrameLen = 8;

/// 最大帧长度 (含 payload): 8 + 4096
static constexpr int kMaxFrameLen = kMinFrameLen + 4096;

/// 响应标志位 (cmd | 0x80 表示响应帧)
static constexpr uint8_t kResponseFlag = 0x80;

/// 默认心跳超时 (秒)
static constexpr int kDefaultHeartbeatTimeout = 30;

/// 心跳检查间隔 (秒)
static constexpr int kHeartbeatCheckInterval = 5;

/// 默认监听端口
static constexpr int kDefaultControlPort = 9000;

/// 默认积压连接数
static constexpr int kDefaultBacklog = 8;

// ============================================================
// 命令码
// ============================================================

enum Command : uint8_t {
    CMD_CAPTURE         = 0x01,   // 拍照
    CMD_START_RECORD    = 0x02,   // 开始录像
    CMD_STOP_RECORD     = 0x03,   // 停止录像
    CMD_SET_RESOLUTION  = 0x10,   // 设置分辨率 (payload: uint16_t w, uint16_t h)
    CMD_SET_FORMAT      = 0x11,   // 设置格式 (payload: uint8_t 0=YUYV, 1=MJPEG)
    CMD_GET_STATUS      = 0x20,   // 查询状态
    CMD_HEARTBEAT       = 0xFF,   // 心跳
};

/// 响应帧中的 cmd 字段 = 原命令码 | kResponseFlag
inline constexpr uint8_t responseCmd(uint8_t cmd) {
    return static_cast<uint8_t>(cmd | kResponseFlag);
}

// ============================================================
// 状态码
// ============================================================

enum StatusCode : uint8_t {
    STATUS_OK            = 0x00,   // 成功
    STATUS_UNKNOWN_CMD   = 0x01,   // 未知命令
    STATUS_BAD_PARAM     = 0x02,   // 参数错误
    STATUS_CRC_ERROR     = 0x03,   // CRC 校验失败
    STATUS_INTERNAL_ERR  = 0x04,   // 内部错误
    STATUS_BUSY          = 0x05,   // 设备忙
    STATUS_NOT_SUPPORTED = 0x06,   // 不支持的操作
};

// ============================================================
// 协议帧结构体（紧凑打包，无对齐填充）
// ============================================================

#pragma pack(push, 1)

/**
 * @brief 协议帧头（不含可变长 payload）
 */
struct ProtoHeader {
    uint8_t  magic[2];       // 魔数：0xEB 0x90
    uint8_t  version;        // 协议版本：0x01
    uint8_t  cmd;            // 命令类型
    uint16_t payload_len;    // 负载长度（网络字节序）
};

/**
 * @brief 请求帧（用于解析接收到的数据）
 */
struct ProtoFrame {
    ProtoHeader header;
    uint8_t     payload[0];  // 可变长负载
};

/**
 * @brief 响应帧头
 */
struct ProtoResponseHeader {
    uint8_t  magic[2];       // 0xEB 0x90
    uint8_t  version;        // 0x01
    uint8_t  cmd;            // 原命令码 | 0x80
    uint8_t  status;         // 状态码
    uint16_t payload_len;    // 负载长度（网络字节序）
    // uint8_t payload[];
    // uint16_t crc16;
};

// ============================================================
// 命令负载结构体
// ============================================================

/**
 * @brief CMD_GET_STATUS 响应负载
 */
struct StatusPayload {
    uint8_t  streaming;       // 0=idle, 1=streaming
    uint8_t  recording;       // 0=not recording, 1=recording
    uint8_t  client_count;    // HTTP 客户端数
    uint8_t  reserved;
    uint16_t width;           // 当前分辨率宽 (网络字节序)
    uint16_t height;          // 当前分辨率高 (网络字节序)
    uint8_t  format;          // 0=YUYV, 1=MJPEG
    uint8_t  fps;             // 当前帧率 (取整)
};

static_assert(sizeof(StatusPayload) == 10, "StatusPayload must be 10 bytes");

/**
 * @brief CMD_SET_RESOLUTION 请求负载
 */
struct ResolutionPayload {
    uint16_t width;           // 网络字节序
    uint16_t height;          // 网络字节序
};

static_assert(sizeof(ResolutionPayload) == 4, "ResolutionPayload must be 4 bytes");

/**
 * @brief CMD_SET_FORMAT 请求负载
 */
struct FormatPayload {
    uint8_t format;           // 0=YUYV, 1=MJPEG
};

static_assert(sizeof(FormatPayload) == 1, "FormatPayload must be 1 byte");

#pragma pack(pop)

// ============================================================
// 命令处理器类型
// ============================================================

/**
 * @brief 命令处理回调
 *
 * 由 ControlServer 在收到命令时调用（在控制线程中执行）。
 * 调用者需自行保证线程安全。
 *
 * @param req_payload   请求负载数据指针
 * @param req_len       请求负载长度
 * @param resp_payload  输出：响应负载缓冲区（由 ControlServer 分配，至少 4096 字节）
 * @param resp_len      输出：响应负载实际长度
 * @return 状态码 (STATUS_OK 表示成功)
 */
using CommandHandler = std::function<uint8_t(
    const uint8_t* req_payload, uint16_t req_len,
    uint8_t* resp_payload, uint16_t* resp_len)>;

// ============================================================
// CRC16 工具
// ============================================================

/**
 * @brief 计算 CRC-16/MODBUS 校验值
 * @param data  数据指针
 * @param len   数据长度
 * @return CRC16 值（主机字节序）
 */
uint16_t crc16Modbus(const uint8_t* data, int len);

/**
 * @brief 计算协议帧的 CRC16（覆盖 magic → payload 末尾）
 * @param header  帧头
 * @param payload 负载数据（可为 nullptr 若 payload_len==0）
 * @return CRC16 值（主机字节序）
 */
uint16_t calcFrameCRC(const ProtoHeader& header, const uint8_t* payload);

// ============================================================
// 帧序列化工具
// ============================================================

/**
 * @brief 将响应帧序列化到缓冲区
 * @param cmd         原命令码
 * @param status      状态码
 * @param payload     负载数据 (可为 nullptr)
 * @param payload_len 负载长度
 * @param out_buf     输出缓冲区（至少 8 + payload_len 字节）
 * @return 序列化后的总长度
 */
int packResponse(uint8_t cmd, uint8_t status,
                 const uint8_t* payload, uint16_t payload_len,
                 uint8_t* out_buf);

// ============================================================
// ControlServer 类
// ============================================================

/**
 * @brief TCP 私有控制协议服务器
 *
 * 非拷贝、非移动。生命周期由创建者管理。
 *
 * 内部线程模型:
 *   - 1 个主控线程: epoll_wait 驱动的事件循环
 *     - 接受新连接
 *     - 读取客户端数据、解析帧、分发命令
 *     - 周期性心跳超时检查
 */
class ControlServer {
public:
    ControlServer();
    ~ControlServer();

    // 禁用拷贝
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // ============================================================
    // 生命周期
    // ============================================================

    /**
     * @brief 启动服务器（阻塞式事件循环，需在独立线程中调用）
     * @param port  监听端口（默认 9000）
     * @return 0 成功，-1 失败
     */
    int start(int port = kDefaultControlPort);

    /**
     * @brief 停止服务器（线程安全，可从任意线程调用）
     */
    void stop();

    /** @brief 是否正在运行 */
    bool isRunning() const { return m_running; }

    /** @brief 获取监听端口 */
    int port() const { return m_port; }

    /** @brief 获取当前客户端数量 */
    int clientCount() const;

    // ============================================================
    // 命令处理器注册
    // ============================================================

    /**
     * @brief 注册命令处理器
     * @param cmd     命令码
     * @param handler 处理回调（在控制线程中被调用）
     */
    void setCommandHandler(uint8_t cmd, CommandHandler handler);

    /**
     * @brief 设置状态查询回调（CMD_GET_STATUS 的便捷接口）
     *
     * 回调应填充 StatusPayload 结构的所有字段。
     * 内部会自动将 StatusPayload 序列化为响应负载。
     *
     * @param provider  状态提供回调
     */
    void setStatusProvider(std::function<void(StatusPayload&)> provider);

    /** @brief 设置心跳超时时间（秒，默认 30） */
    void setHeartbeatTimeout(int seconds) { m_heartbeatTimeout = seconds; }

    /** @brief 获取心跳超时时间 */
    int heartbeatTimeout() const { return m_heartbeatTimeout; }

private:
    // ============================================================
    // 网络
    // ============================================================

    int createSocket(int port);
    void eventLoop();
    void acceptClient();
    void handleClientData(int client_fd);
    void disconnectClient(int client_fd);
    void checkHeartbeats();

    // ============================================================
    // 协议处理
    // ============================================================

    /**
     * @brief 尝试从客户端接收缓冲区中解析一帧
     * @param client_fd  客户端 fd
     * @param buf        缓冲区（可能被修改以移除已处理数据）
     * @return true 解析并处理了一帧，false 无完整帧
     */
    bool tryParseFrame(int client_fd, std::vector<uint8_t>& buf);

    /**
     * @brief 分发命令并发送响应
     */
    void dispatchCommand(int client_fd, uint8_t cmd,
                         const uint8_t* payload, uint16_t payload_len);

    /**
     * @brief 发送响应帧
     */
    bool sendResponse(int client_fd, uint8_t cmd, uint8_t status,
                      const uint8_t* payload, uint16_t payload_len);

    // ============================================================
    // 内置命令处理器
    // ============================================================

    uint8_t handleHeartbeat(const uint8_t* req, uint16_t req_len,
                            uint8_t* resp, uint16_t* resp_len);

    // ============================================================
    // 成员变量
    // ============================================================

    int  m_server_fd = -1;
    int  m_port      = 0;
    int  m_epoll_fd  = -1;
    std::atomic<bool> m_running{false};

    // 心跳超时 (秒)
    int m_heartbeatTimeout = kDefaultHeartbeatTimeout;

    // 命令处理器表
    mutable std::mutex m_handlerMtx;
    std::map<uint8_t, CommandHandler> m_handlers;

    // 状态提供回调
    std::function<void(StatusPayload&)> m_statusProvider;

    // 客户端信息
    struct ClientInfo {
        int  fd;
        std::vector<uint8_t> recv_buf;   // 接收缓冲区（处理 TCP 粘包/拆包）
        std::chrono::steady_clock::time_point last_heartbeat;
    };

    mutable std::mutex m_clientsMtx;
    std::map<int, ClientInfo> m_clients;
};

#endif // SMART_CAM_NETWORK_CONTROL_H
