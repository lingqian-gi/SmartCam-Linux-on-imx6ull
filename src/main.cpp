/**
 * @file    main.cpp
 * @brief   SmartCam Linux 主入口
 *
 * 支持两种运行模式：
 *   1. 独立 GUI 测试模式（默认，无参数）：
 *      - 使用 MockCamera 生成彩色测试条
 *      - 适合 PC 端开发调试 UI 布局与交互
 *
 *   2. 真实相机模式（参数 --device /dev/video0）：
 *      - V4L2 采集 + GUI 实时预览
 *      - 多线程：采集线程拉帧 → 主线程 Qt 事件循环渲染
 *
 * 编译 & 运行：
 *   mkdir build && cd build
 *   cmake .. && make -j$(nproc)
 *   ./smartcam                    # Mock 模式
 *   ./smartcam --device /dev/video0       # 真实相机 (MJPEG)
 *   ./smartcam --device /dev/video0 --fmt yuyv  # 真实相机 (YUYV)
 *   # 开发板无 X server, 必须加 -platform linuxfb:
 *   ./smartcam --device /dev/video0 --fmt yuyv -platform linuxfb
 */

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <QDebug>
#include <QImage>
#include <cstdio>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <cstring>
#include <arpa/inet.h>

#include "include/display/gui.h"
#include "include/camera/capture.h"
#include "include/camera/processor.h"
#include "include/network/mjpeg_server.h"
#include "include/network/control.h"
#include "include/network/rtsp_server.h"
#include "include/storage/manager.h"
#include "include/common/logger.h"
#include "include/common/config.h"

// ============================================================
// 全局共享状态（采集线程 → GUI 线程 / 存储线程）
// ============================================================
struct CaptureState {
    std::mutex              mtx;
    std::vector<uint8_t>    frameData;   // 拷贝后的帧数据
    int                     width  = 0;
    int                     height = 0;
    PixelFormat             format = PixelFormat::FMT_RGB24;
    double                  fps    = 0.0;
    std::atomic<bool>       running{false};
    std::atomic<bool>       paused{false};   // 暂停采集（分辨率/格式切换等需要）
    std::mutex              pauseMtx;        // 暂停同步锁
    std::condition_variable pauseCv;         // 暂停同步条件变量
    std::atomic<bool>       pausedAck{false}; // 采集线程已确认暂停
};
static CaptureState g_state;

// 录像状态（由 main 线程设置，采集线程读取）
static std::atomic<bool> g_recording{false};

// 存储管理器（全局单例指针，线程安全）
static StorageManager* g_storage = nullptr;

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("SmartCam");
    app.setApplicationVersion("0.1.0");

    // ---- 命令行解析 ----
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("SmartCam Linux — 基于 iMX6ULL 的智能相机流媒体系统"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption configOpt(
        QStringLiteral("config"),
        QStringLiteral("配置文件路径 (默认 /etc/smartcam/smartcam.conf)"),
        QStringLiteral("config"),
        QStringLiteral("/etc/smartcam/smartcam.conf")
    );
    parser.addOption(configOpt);

    QCommandLineOption deviceOpt(
        QStringLiteral("device"),
        QStringLiteral("V4L2 设备路径, 例如 /dev/video0"),
        QStringLiteral("device"),
        QString()  // 默认空 → Mock 模式
    );
    parser.addOption(deviceOpt);

    QCommandLineOption portOpt(
        QStringLiteral("http-port"),
        QStringLiteral("MJPEG-over-HTTP 端口 (默认 8080)"),
        QStringLiteral("port"),
        ""  // 空 → 从配置文件读取
    );
    parser.addOption(portOpt);

    QCommandLineOption ctrlPortOpt(
        QStringLiteral("control-port"),
        QStringLiteral("TCP 控制协议端口 (默认 9000)"),
        QStringLiteral("port"),
        ""  // 空 → 从配置文件读取
    );
    parser.addOption(ctrlPortOpt);

    QCommandLineOption rtspPortOpt(
        QStringLiteral("rtsp-port"),
        QStringLiteral("RTSP 流媒体端口 (默认 8554)"),
        QStringLiteral("port"),
        ""  // 空 → 从配置文件读取
    );
    parser.addOption(rtspPortOpt);

    QCommandLineOption fmtOpt(
        QStringLiteral("fmt"),
        QStringLiteral("像素格式: yuyv | mjpeg (默认 mjpeg)"),
        QStringLiteral("fmt"),
        ""  // 空 → 从配置文件读取
    );
    parser.addOption(fmtOpt);

    parser.process(app);

    // ---- 加载配置文件（优先级: 命令行 --config > ~/.config > /etc） ----
    ConfigManager cfg;
    QString configPath;
    if (parser.isSet(configOpt)) {
        // 用户明确指定了 --config，直接使用
        configPath = parser.value(configOpt);
    } else {
        // 优先读用户级配置，不存在则用系统级
        const char* home = getenv("HOME");
        if (home) {
            std::string userCfg = std::string(home) + "/.config/smartcam/smartcam.conf";
            if (cfg.load(userCfg)) {
                configPath = QString::fromStdString(userCfg);
            }
        }
        if (configPath.isEmpty()) {
            configPath = QStringLiteral("/etc/smartcam/smartcam.conf");
        }
    }
    bool cfgLoaded = cfg.load(configPath.toStdString());
    if (cfgLoaded) {
        LOG_INF("Configuration loaded: %s", configPath.toStdString().c_str());
    } else {
        LOG_INF("No config file at %s, using defaults",
                configPath.isEmpty() ? "<none>" : configPath.toStdString().c_str());
    }

    // ---- 合并配置: 命令行 > 配置文件 > 硬编码默认值 ----
    QString device = parser.isSet(deviceOpt)
        ? parser.value(deviceOpt)
        : (cfgLoaded ? QString::fromStdString(cfg.getString("camera", "device")) : QString());

    int httpPort = parser.isSet(portOpt)
        ? parser.value(portOpt).toInt()
        : cfg.getInt("network", "http_port", 8080);

    int ctrlPort = parser.isSet(ctrlPortOpt)
        ? parser.value(ctrlPortOpt).toInt()
        : cfg.getInt("network", "control_port", 9000);

    int rtspPort = parser.isSet(rtspPortOpt)
        ? parser.value(rtspPortOpt).toInt()
        : cfg.getInt("network", "rtsp_port", 8554);

    QString fmtStr = parser.isSet(fmtOpt)
        ? parser.value(fmtOpt).toLower()
        : (cfgLoaded
            ? QString::fromStdString(cfg.getString("camera", "format", "mjpeg")).toLower()
            : QStringLiteral("mjpeg"));

    // 存储路径
    std::string photoDir = cfgLoaded
        ? cfg.getString("storage", "photo_dir", "/data/photos")
        : "/tmp/smartcam/photos";
    std::string videoDir = cfgLoaded
        ? cfg.getString("storage", "video_dir", "/data/videos")
        : "/tmp/smartcam/videos";

    // 提取基础存储路径（去掉尾部的 /photos 或 /videos）
    auto stripSuffix = [](std::string s, const std::string& suffix) -> std::string {
        if (s.size() > suffix.size() &&
            s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return s.substr(0, s.size() - suffix.size());
        }
        return s;
    };
    std::string basePath = stripSuffix(photoDir, "/photos");
    if (basePath.empty()) basePath = "/data";

    // 日志级别
    if (cfgLoaded) {
        std::string logLevel = cfg.getString("logging", "level", "info");
        if (logLevel == "debug")      Logger::instance()->setLevel(LogLevel::DEBUG);
        else if (logLevel == "warn")  Logger::instance()->setLevel(LogLevel::WARN);
        else if (logLevel == "error") Logger::instance()->setLevel(LogLevel::ERROR);
        else                          Logger::instance()->setLevel(LogLevel::INFO);

        if (cfg.getBool("logging", "use_syslog", false)) {
            Logger::instance()->setSyslogEnabled(true);
        }
    }

    // ---- 创建 & 显示 GUI ----
    CameraGUI gui;

    // ---- 初始化存储管理器 ----
    StorageManager storage(photoDir, videoDir);
    g_storage = &storage;

    // 绑定存储到相册组件
    gui.setGalleryStorage(&storage);

    // 同步当前存储路径到 GUI（Settings 下拉框显示当前路径）
    gui.setStoragePath(basePath);

    // 存储路径变更回调（重启后生效：更新配置并保存，下次启动使用新路径）
    gui.onStoragePathChanged([&cfg, &photoDir, &videoDir, &basePath](const std::string& path) {
        // 更新内存中的路径变量
        basePath = path;
        photoDir = path + "/photos";
        videoDir = path + "/videos";

        // 更新 StorageManager 的路径（即时生效）
        if (g_storage) {
            g_storage->setPhotoDir(photoDir);
            g_storage->setVideoDir(videoDir);
        }

        // 保存到配置文件（持久化：优先原路径，失败则写用户目录）
        cfg.setString("storage", "photo_dir", photoDir);
        cfg.setString("storage", "video_dir", videoDir);

        bool saved = cfg.save();  // 尝试写回原始路径（如 /etc/smartcam/smartcam.conf）
        if (!saved) {
            // 原始路径不可写（非 root 用户），写 ~/.config/smartcam/smartcam.conf
            const char* home = getenv("HOME");
            std::string userCfg = (home ? std::string(home) : "/home/debian")
                                  + "/.config/smartcam/smartcam.conf";
            saved = cfg.saveAs(userCfg);
            if (saved) {
                LOG_INF("Config saved to user path: %s", userCfg.c_str());
            }
        } else {
            LOG_INF("Storage path saved: %s", path.c_str());
        }

        if (!saved) {
            LOG_WRN("Failed to save config — storage path may not persist after reboot");
        }

        LOG_INF("Storage path changed: photos=%s  videos=%s",
                photoDir.c_str(), videoDir.c_str());
    });

    // ---- 真实相机模式 ----
    CameraCapture*    capture      = nullptr;
    std::thread*      captureThread = nullptr;
    QTimer*           displayTimer = nullptr;
    MJPEGStreamServer* mjpegServer = nullptr;
    ControlServer*    controlSrv   = nullptr;
    std::thread*      controlThread = nullptr;
    RTSPServer*       rtspServer   = nullptr;
    std::thread*      rtspThread   = nullptr;

    if (!device.isEmpty()) {
        // ============================================================
        // 初始化 V4L2 摄像头
        // ============================================================
        capture = new CameraCapture();
        if (capture->init(device.toStdString().c_str()) < 0) {
            LOG_ERR_("Failed to init camera device: %s",
                      device.toStdString().c_str());
            return 1;
        }

        // 打印驱动信息
        LOG_INF("%s", capture->getDriverInfo().c_str());

        // 枚举支持的格式
        std::vector<uint32_t> formats;
        capture->enumFormats(formats);
        bool hasYUYV  = false;
        bool hasMJPEG = false;
        for (auto f : formats) {
            if (f == CameraCapture::V4L2_PIX_FMT_YUYV)  hasYUYV  = true;
            if (f == CameraCapture::V4L2_PIX_FMT_MJPEG) hasMJPEG = true;
        }
        LOG_INF("Camera supports: YUYV=%s MJPEG=%s",
                 hasYUYV ? "YES" : "NO", hasMJPEG ? "YES" : "NO");

        // 选择格式
        uint32_t pixfmt;
        bool useYUYV = (fmtStr == "yuyv");
        if (useYUYV && hasYUYV) {
            pixfmt = CameraCapture::V4L2_PIX_FMT_YUYV;
        } else if (!useYUYV && hasMJPEG) {
            pixfmt = CameraCapture::V4L2_PIX_FMT_MJPEG;
        } else if (hasYUYV) {
            pixfmt = CameraCapture::V4L2_PIX_FMT_YUYV;
        } else if (hasMJPEG) {
            pixfmt = CameraCapture::V4L2_PIX_FMT_MJPEG;
        } else {
            LOG_ERR_("No supported pixel format found");
            delete capture;
            return 1;
        }

        // 设置 640x480
        if (capture->setFormat(640, 480, pixfmt) < 0) {
            LOG_ERR_("Failed to set format");
            delete capture;
            return 1;
        }

        Resolution curRes = capture->getCurrentResolution();
        uint32_t   curFmt = capture->getCurrentFormat();
        LOG_INF("Active format: %dx%d, fmt='%c%c%c%c'",
                 curRes.width, curRes.height,
                 (curFmt >> 0) & 0xFF, (curFmt >> 8) & 0xFF,
                 (curFmt >> 16) & 0xFF, (curFmt >> 24) & 0xFF);

        if (capture->startCapture() < 0) {
            LOG_ERR_("Failed to start capture");
            delete capture;
            return 1;
        }

        gui.setStreamingStatus(true);
        g_state.running = true;

        // ============================================================
        // 启动 MJPEG-over-HTTP 流媒体服务器
        //   - MJPEG 模式：摄像头硬件直出 JPEG，零拷贝推流
        //   - YUYV 模式：libjpeg-turbo 软件编码后推流
        // ============================================================
        mjpegServer = new MJPEGStreamServer();
        bool mjpegServerOk = false;
        if (mjpegServer->start(httpPort) == 0) {
            mjpegServerOk = true;
            LOG_INF("MJPEG stream server ready on port %d", httpPort);

            // 注册 /status 端点回调
            auto startTime = std::chrono::steady_clock::now();
            mjpegServer->setStatusProvider(
                [capture, mjpegServer = mjpegServer, startTime]() -> StreamStatus {
                    StreamStatus st;
                    st.streaming = capture->isStreaming();
                    st.recording = g_recording.load();
                    Resolution res = capture->getCurrentResolution();
                    st.width  = res.width;
                    st.height = res.height;
                    st.format = (capture->getCurrentFormat() ==
                                 CameraCapture::V4L2_PIX_FMT_MJPEG)
                                 ? "MJPEG" : "YUYV";
                    st.fps    = capture->getCurrentFPS();
                    st.client_count = mjpegServer->clientCount();
                    st.uptime_seconds = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - startTime
                        ).count());
                    return st;
                });
        } else {
            LOG_WRN("MJPEG stream server failed to start on port %d", httpPort);
        }

        // ============================================================
        // 启动 RTSP 流媒体服务器
        //   - MJPEG 模式：硬件直出 JPEG → RTP 分片发送
        //   - YUYV 模式：libjpeg-turbo 编码后 → RTP 分片发送
        // ============================================================
        rtspServer = new RTSPServer();
        rtspServer->setStreamInfo(curRes.width, curRes.height,
                                  30 /* fps */);
        rtspThread = new std::thread([rtspServer, rtspPort]() {
            LOG_INF("RTSP thread starting on port %d", rtspPort);
            rtspServer->start(rtspPort);
            LOG_INF("RTSP thread exited");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        LOG_INF("RTSP stream server ready on rtsp://<board-ip>:%d/stream", rtspPort);

        // ============================================================
        // 启动 TCP 私有控制协议服务器
        // ============================================================
        controlSrv = new ControlServer();

        // 注册 状态查询 处理器
        controlSrv->setStatusProvider([capture, mjpegServer = mjpegServer](
                                          StatusPayload& sp) {
            sp.streaming    = capture->isStreaming() ? 1 : 0;
            sp.recording    = g_recording.load() ? 1 : 0;
            sp.client_count = mjpegServer ? mjpegServer->clientCount() : 0;
            sp.reserved     = 0;

            Resolution res = capture->getCurrentResolution();
            sp.width  = static_cast<uint16_t>(res.width);
            sp.height = static_cast<uint16_t>(res.height);
            sp.format = (capture->getCurrentFormat() == CameraCapture::V4L2_PIX_FMT_MJPEG) ? 1 : 0;
            sp.fps    = static_cast<uint8_t>(capture->getCurrentFPS());
        });

        // 注册 拍照 处理器
        controlSrv->setCommandHandler(CMD_CAPTURE,
            [capture](const uint8_t* /*req*/, uint16_t /*req_len*/,
                      uint8_t* resp, uint16_t* resp_len) -> uint8_t {
                std::lock_guard<std::mutex> lock(g_state.mtx);
                if (g_state.frameData.empty() || !g_storage) {
                    return STATUS_BUSY;
                }

                std::string path;
                if (g_state.format == PixelFormat::FMT_MJPEG) {
                    path = g_storage->savePhoto(g_state.frameData.data(),
                                                static_cast<int>(g_state.frameData.size()));
                }
#ifdef HAS_LIBJPEG
                else if (g_state.format == PixelFormat::FMT_YUYV) {
                    uint8_t* jpeg_out = nullptr;
                    unsigned long jpeg_len = 0;
                    if (VideoProcessor::encodeYUYVtoJPEG(
                            g_state.frameData.data(),
                            g_state.width, g_state.height,
                            85, &jpeg_out, &jpeg_len) == 0) {
                        path = g_storage->savePhoto(jpeg_out,
                                                    static_cast<int>(jpeg_len));
                        free(jpeg_out);
                    }
                }
#endif
                if (path.empty()) {
                    return STATUS_INTERNAL_ERR;
                }

                // 响应负载 = 保存路径
                uint16_t plen = static_cast<uint16_t>(std::min(path.size(),
                                                       size_t(0xFFFF)));
                memcpy(resp, path.c_str(), plen);
                *resp_len = plen;
                return STATUS_OK;
            });

        // 注册 录像控制 处理器
        controlSrv->setCommandHandler(CMD_START_RECORD,
            [capture](const uint8_t* /*req*/, uint16_t /*req_len*/,
                      uint8_t* /*resp*/, uint16_t* resp_len) -> uint8_t {
                if (!g_storage || g_recording.load()) return STATUS_BUSY;

                std::lock_guard<std::mutex> lock(g_state.mtx);
                if (g_state.format != PixelFormat::FMT_MJPEG) {
                    return STATUS_NOT_SUPPORTED;
                }

                int w = g_state.width;
                int h = g_state.height;
                int fps = static_cast<int>(g_state.fps > 0 ? g_state.fps : 30.0);

                if (g_storage->startRecord(w, h, fps) == 0) {
                    g_recording = true;
                    *resp_len = 0;
                    return STATUS_OK;
                }
                return STATUS_INTERNAL_ERR;
            });

        controlSrv->setCommandHandler(CMD_STOP_RECORD,
            [](const uint8_t* /*req*/, uint16_t /*req_len*/,
               uint8_t* /*resp*/, uint16_t* resp_len) -> uint8_t {
                if (!g_recording.load()) return STATUS_OK;  // 已经没在录，也算成功

                g_recording = false;
                if (g_storage) g_storage->stopRecord();
                *resp_len = 0;
                return STATUS_OK;
            });

        // 注册 分辨率设置 处理器
        controlSrv->setCommandHandler(CMD_SET_RESOLUTION,
            [capture](const uint8_t* req, uint16_t req_len,
                      uint8_t* /*resp*/, uint16_t* resp_len) -> uint8_t {
                if (req_len < sizeof(ResolutionPayload)) return STATUS_BAD_PARAM;

                const auto* rp = reinterpret_cast<const ResolutionPayload*>(req);
                int w = static_cast<int>(ntohs(rp->width));
                int h = static_cast<int>(ntohs(rp->height));

                if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
                    return STATUS_BAD_PARAM;
                }

                if (capture->isStreaming()) {
                    capture->stopCapture();
                    capture->setFormat(w, h, capture->getCurrentFormat());
                    capture->startCapture();
                }
                *resp_len = 0;
                return STATUS_OK;
            });

        // 注册 格式切换 处理器
        controlSrv->setCommandHandler(CMD_SET_FORMAT,
            [capture](const uint8_t* req, uint16_t req_len,
                      uint8_t* /*resp*/, uint16_t* resp_len) -> uint8_t {
                if (req_len < sizeof(FormatPayload)) return STATUS_BAD_PARAM;

                const auto* fp = reinterpret_cast<const FormatPayload*>(req);
                uint32_t v4l2fmt = (fp->format == 1)
                    ? CameraCapture::V4L2_PIX_FMT_MJPEG
                    : CameraCapture::V4L2_PIX_FMT_YUYV;

                if (capture->isStreaming()) {
                    Resolution res = capture->getCurrentResolution();
                    capture->stopCapture();
                    capture->setFormat(res.width, res.height, v4l2fmt);
                    capture->startCapture();
                }
                *resp_len = 0;
                return STATUS_OK;
            });

        // 启动控制线程（ControlServer::start 内部是阻塞事件循环）
        controlThread = new std::thread([controlSrv, ctrlPort]() {
            LOG_INF("Control thread starting on port %d", ctrlPort);
            controlSrv->start(ctrlPort);
            LOG_INF("Control thread exited");
        });
        // 给控制线程一点时间启动
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // ============================================================
        // 启动采集线程（连续拉帧 → 拷贝到共享缓冲区 + 推流）
        // ============================================================
        captureThread = new std::thread([capture, mjpegServer, mjpegServerOk,
                                             rtspServer]() {
            FrameBuffer fb;

            while (g_state.running) {
                // 暂停期间等待恢复（分辨率/格式切换中）
                if (g_state.paused) {
                    g_state.pausedAck = true;
                    g_state.pauseCv.notify_one();
                    std::unique_lock<std::mutex> lk(g_state.pauseMtx);
                    g_state.pauseCv.wait(lk, [] { return !g_state.paused.load(); });
                    continue;
                }
                g_state.pausedAck = false;

                if (capture->getFrame(&fb, 1000) < 0) {
                    if (!g_state.running) break;
                    continue;  // 超时重试
                }

                {
                    std::lock_guard<std::mutex> lock(g_state.mtx);

                    // 拷贝帧数据到共享缓冲区（V4L2 mmap 内存不能长期持有）
                    g_state.frameData.assign(fb.data, fb.data + fb.length);
                    g_state.width  = fb.width;
                    g_state.height = fb.height;
                    g_state.format = fb.format;
                    g_state.fps    = capture->getCurrentFPS();
                }

                // === 推送帧到 HTTP / RTSP 流服务器 ===
                //
                // MJPEG 模式：摄像头硬件直出 JPEG — 零拷贝，直接推流
                // YUYV 模式：libjpeg-turbo 软编码为 JPEG 一次，复用给两个流
                //
                bool needEncode = (fb.format == PixelFormat::FMT_YUYV) &&
                                  (mjpegServerOk || rtspServer);
                uint8_t*      jpeg_out = nullptr;
                unsigned long jpeg_len = 0;

                if (needEncode) {
#ifdef HAS_LIBJPEG
                    VideoProcessor::encodeYUYVtoJPEG(
                        fb.data, fb.width, fb.height,
                        80, &jpeg_out, &jpeg_len);
#endif
                }

                if (mjpegServerOk) {
                    if (fb.format == PixelFormat::FMT_MJPEG) {
                        mjpegServer->updateFrame(fb.data,
                            static_cast<size_t>(fb.length));
                    } else if (jpeg_out && jpeg_len > 0) {
                        mjpegServer->updateFrame(jpeg_out,
                            static_cast<size_t>(jpeg_len));
                    }
                }

                if (rtspServer) {
                    if (fb.format == PixelFormat::FMT_MJPEG) {
                        rtspServer->feedFrame(fb.data,
                            static_cast<size_t>(fb.length),
                            fb.width, fb.height);
                    } else if (jpeg_out && jpeg_len > 0) {
                        rtspServer->feedFrame(jpeg_out,
                            static_cast<size_t>(jpeg_len),
                            fb.width, fb.height);
                    }
                }

                if (jpeg_out) free(jpeg_out);

                // 如果正在录像且格式为 MJPEG，写入帧到 AVI 文件
                if (g_recording && fb.format == PixelFormat::FMT_MJPEG && g_storage) {
                    g_storage->writeRecordFrame(fb.data, fb.length);
                }

                // 归还缓冲区到 V4L2 队列
                capture->putFrame(&fb);
            }
        });

        // ============================================================
        // 显示定时器（Qt 主线程，33ms ≈ 30fps）
        // ============================================================
        displayTimer = new QTimer(&gui);
        displayTimer->setInterval(33);
        QObject::connect(displayTimer, &QTimer::timeout, [&gui, mjpegServer]() {
            std::lock_guard<std::mutex> lock(g_state.mtx);
            if (g_state.frameData.empty()) return;

            // 直接使用 GUI 的 setFrame 接口
            // 内部 frameToQImage() 会处理 YUYV→RGB24 转换并深拷贝到 QImage
            gui.setFrame(g_state.frameData.data(),
                         static_cast<int>(g_state.frameData.size()),
                         g_state.width, g_state.height,
                         g_state.format);

            gui.setFPS(g_state.fps);
            gui.setClientCount(mjpegServer->clientCount());
        });
        displayTimer->start();

        // ============================================================
        // 连接回调：分辨率/格式变更 → 重新配置摄像头
        // ============================================================
        gui.onResolutionChanged([capture](int w, int h) {
            if (!capture->isStreaming()) return;

            // 1. 暂停采集线程，防止 stopCapture 时采集线程还在使用 mmap 缓冲区
            g_state.paused = true;
            // 等待采集线程确认暂停（getFrame 有 1s 超时，最多等 1.1s）
            {
                std::unique_lock<std::mutex> lk(g_state.pauseMtx);
                g_state.pauseCv.wait_until(lk,
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(1100),
                    [] { return g_state.pausedAck.load(); });
            }

            // 2. 安全停止采集、切换格式、重启
            capture->stopCapture();
            int ret = capture->setFormat(w, h, capture->getCurrentFormat());
            if (ret < 0) {
                LOG_ERR_("setFormat(%dx%d) failed (ret=%d), reverting to 640x480",
                          w, h, ret);
                capture->setFormat(640, 480, capture->getCurrentFormat());
            }
            capture->startCapture();

            // 3. 恢复采集线程
            g_state.paused = false;
            g_state.pauseCv.notify_one();
            LOG_INF("Resolution changed to %dx%d", w, h);
        });

        gui.onFormatChanged([capture, device](PixelFormat fmt) {
            if (!capture->isStreaming()) return;

            uint32_t v4l2fmt = (fmt == PixelFormat::FMT_YUYV)
                                   ? CameraCapture::V4L2_PIX_FMT_YUYV
                                   : CameraCapture::V4L2_PIX_FMT_MJPEG;

            // 暂停采集线程，防止竞态
            g_state.paused = true;
            {
                std::unique_lock<std::mutex> lk(g_state.pauseMtx);
                g_state.pauseCv.wait_until(lk,
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(1100),
                    [] { return g_state.pausedAck.load(); });
            }

            capture->stopCapture();
            int ret = capture->setFormat(640, 480, v4l2fmt);
            if (ret < 0) {
                LOG_ERR_("setFormat(640x480, %s) failed (ret=%d)",
                          (fmt == PixelFormat::FMT_YUYV) ? "YUYV" : "MJPEG", ret);
            }
            capture->startCapture();

            g_state.paused = false;
            g_state.pauseCv.notify_one();
            LOG_INF("Format changed to %s",
                     (fmt == PixelFormat::FMT_YUYV) ? "YUYV" : "MJPEG");
        });

        gui.onCaptureRequest([capture]() {
            // 从共享状态获取最新一帧并保存为 JPEG 照片
            std::lock_guard<std::mutex> lock(g_state.mtx);
            if (g_state.frameData.empty() || !g_storage) {
                LOG_WRN("Capture: no frame data available");
                return;
            }

            if (g_state.format == PixelFormat::FMT_MJPEG) {
                // MJPEG 模式：帧数据已经是 JPEG，直接保存
                std::string path = g_storage->savePhoto(
                    g_state.frameData.data(),
                    static_cast<int>(g_state.frameData.size()));
                LOG_INF("Photo captured (MJPEG): %s",
                         path.empty() ? "FAILED" : path.c_str());
            }
#ifdef HAS_LIBJPEG
            else if (g_state.format == PixelFormat::FMT_YUYV) {
                // YUV 模式：需要先编码为 JPEG
                uint8_t* jpeg_out = nullptr;
                unsigned long jpeg_len = 0;
                if (VideoProcessor::encodeYUYVtoJPEG(
                        g_state.frameData.data(),
                        g_state.width, g_state.height,
                        85, &jpeg_out, &jpeg_len) == 0) {
                    std::string path = g_storage->savePhoto(
                        jpeg_out, static_cast<int>(jpeg_len));
                    LOG_INF("Photo captured (YUV→JPEG): %s",
                             path.empty() ? "FAILED" : path.c_str());
                    free(jpeg_out);
                } else {
                    LOG_ERR_("Capture: YUYV→JPEG encoding failed");
                }
            }
#endif
            else {
                LOG_WRN("Capture: unsupported format for photo save");
            }
        });

        gui.onRecordToggle([capture, &gui](bool start) -> bool {
            if (!g_storage) return false;

            if (start) {
                // 开始录像：检查当前格式必须是 MJPEG
                std::lock_guard<std::mutex> lock(g_state.mtx);
                if (g_state.format != PixelFormat::FMT_MJPEG) {
                    LOG_WRN("Recording requires MJPEG mode (current format is YUYV)");
                    return false;  // 拒绝录制，GUI 按钮状态保持不变
                }
                int w = g_state.width;
                int h = g_state.height;
                int fps = static_cast<int>(g_state.fps > 0 ? g_state.fps : 30.0);

                if (g_storage->startRecord(w, h, fps) == 0) {
                    g_recording = true;
                    gui.setRecordingStatus(true);
                    LOG_INF("Recording started: %dx%d @ %dfps", w, h, fps);
                    return true;
                }
                LOG_ERR_("Recording start failed");
                return false;
            } else {
                g_recording = false;
                g_storage->stopRecord();
                gui.setRecordingStatus(false);
                LOG_INF("Recording stopped");
                return true;
            }
        });

        qInfo() << "==============================================";
        qInfo() << "SmartCam Linux — 真实相机模式";
        qInfo() << "配置:"  << (cfgLoaded ? configPath : "none (using defaults)");
        qInfo() << "设备:"  << device;
        qInfo() << "格式:"  << fmtStr;
        qInfo() << "HTTP 端口:" << httpPort << "  |  RTSP 端口:" << rtspPort;
        qInfo() << "控制端口:" << ctrlPort;
        qInfo() << "存储:" << QString::fromStdString(photoDir) << " / " << QString::fromStdString(videoDir);
        qInfo() << "流媒体:" << (mjpegServerOk ? "✅ 已启动" : "❌ 启动失败");
        qInfo() << "浏览器打开: http://<dev-ip>:" << httpPort << "/";
        qInfo() << "VLC 播放:   rtsp://<dev-ip>:" << rtspPort << "/stream";
        qInfo() << "==============================================";

    } else {
        // ============================================================
        // Mock 模式（无硬件，显示彩条）
        // ============================================================
        gui.onCaptureRequest([]() {
            qDebug() << "[Main] 拍照请求 (Mock)";
        });
        gui.onRecordToggle([](bool start) -> bool {
            qDebug() << "[Main] 录像切换:" << (start ? "开始" : "停止");
            return true;
        });
        gui.onResolutionChanged([](int w, int h) {
            qDebug() << "[Main] 分辨率变更:" << w << "x" << h;
        });
        gui.onFormatChanged([](PixelFormat fmt) {
            qDebug() << "[Main] 格式变更:" << static_cast<int>(fmt);
        });

        // 启动 TCP 控制服务器（Mock 模式：仅心跳 + 状态查询可用）
        controlSrv = new ControlServer();
        controlSrv->setStatusProvider([](StatusPayload& sp) {
            sp.streaming    = 0;
            sp.recording    = 0;
            sp.client_count = 0;
            sp.reserved     = 0;
            sp.width        = 640;
            sp.height       = 480;
            sp.format       = 1;  // MJPEG
            sp.fps          = 30;
        });
        controlSrv->setCommandHandler(CMD_CAPTURE,
            [](const uint8_t*, uint16_t, uint8_t*, uint16_t* rl) -> uint8_t {
                *rl = 0;
                return STATUS_NOT_SUPPORTED;
            });
        controlSrv->setCommandHandler(CMD_START_RECORD,
            [](const uint8_t*, uint16_t, uint8_t*, uint16_t* rl) -> uint8_t {
                *rl = 0;
                return STATUS_NOT_SUPPORTED;
            });
        controlSrv->setCommandHandler(CMD_STOP_RECORD,
            [](const uint8_t*, uint16_t, uint8_t*, uint16_t* rl) -> uint8_t {
                *rl = 0;
                return STATUS_NOT_SUPPORTED;
            });
        controlThread = new std::thread([controlSrv, ctrlPort]() {
            controlSrv->start(ctrlPort);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        qInfo() << "==============================================";
        qInfo() << "SmartCam Linux — Mock 模式";
        qInfo() << "配置:"  << (cfgLoaded ? configPath : "none (using defaults)");
        qInfo() << "控制端口:" << ctrlPort << " (Mock 模式下可用)";
        qInfo() << "传参 --device /dev/video0 切换到真实相机模式";
        qInfo() << "==============================================";
    }

    gui.show();

    // ---- Qt 事件循环 ----
    int ret = app.exec();

    // ---- 清理 ----
    g_state.running = false;

    if (captureThread && captureThread->joinable()) {
        captureThread->join();
        delete captureThread;
    }

    if (mjpegServer) {
        mjpegServer->stop();
        delete mjpegServer;
    }

    if (rtspServer) {
        rtspServer->stop();
    }
    if (rtspThread && rtspThread->joinable()) {
        rtspThread->join();
        delete rtspThread;
    }
    if (rtspServer) {
        delete rtspServer;
    }

    if (controlSrv) {
        controlSrv->stop();
    }
    if (controlThread && controlThread->joinable()) {
        controlThread->join();
        delete controlThread;
    }
    if (controlSrv) {
        delete controlSrv;
    }

    if (capture) {
        capture->release();
        delete capture;
    }

    return ret;
}
