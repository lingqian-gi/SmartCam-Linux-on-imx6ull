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
    std::atomic<int>        targetFps{0};    // 用户设定的目标帧率（0=不限制，跟随硬件）

    // 帧处理线程同步
    std::mutex              procMtx;           // 处理线程专用锁
    std::condition_variable procCv;            // 新帧通知
    std::atomic<bool>       frameReady{false}; // 有新帧待处理
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
    std::thread*      processThread = nullptr;
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
        // 查询 V4L2 控制参数范围 & 注册相机控制回调
        // ============================================================
        {
            int min, max, step, def, val;

            // 亮度
            if (capture->queryControl(CameraCapture::V4L2_CID_BRIGHTNESS,
                                       min, max, step, def) == 0) {
                capture->getControl(CameraCapture::V4L2_CID_BRIGHTNESS, val);
                gui.setBrightnessRange(min, max, step, (val != 0 ? val : def));
                LOG_INF("Brightness: min=%d max=%d step=%d def=%d cur=%d",
                         min, max, step, def, val);
            }

            // 对比度
            if (capture->queryControl(CameraCapture::V4L2_CID_CONTRAST,
                                       min, max, step, def) == 0) {
                capture->getControl(CameraCapture::V4L2_CID_CONTRAST, val);
                gui.setContrastRange(min, max, step, (val != 0 ? val : def));
                LOG_INF("Contrast: min=%d max=%d step=%d def=%d cur=%d",
                         min, max, step, def, val);
            }

            // 白平衡色温
            if (capture->queryControl(CameraCapture::V4L2_CID_WHITE_BALANCE_TEMPERATURE,
                                       min, max, step, def) == 0) {
                capture->getControl(CameraCapture::V4L2_CID_WHITE_BALANCE_TEMPERATURE, val);
                gui.setWhiteBalanceRange(min, max, step, (val != 0 ? val : def));
                LOG_INF("WB Temp: min=%d max=%d step=%d def=%d cur=%d",
                         min, max, step, def, val);
            }

            // 自动白平衡
            if (capture->queryControl(CameraCapture::V4L2_CID_AUTO_WHITE_BALANCE,
                                       min, max, step, def) == 0) {
                capture->getControl(CameraCapture::V4L2_CID_AUTO_WHITE_BALANCE, val);
                gui.setAutoWhiteBalance(val != 0);
                LOG_INF("Auto WB: cur=%d", val);
            }

            // 自动曝光 → 查询并设置 GUI，同时强制手动模式防帧率下降
            {
                int expMin, expMax, expStep, expDef, expVal;
                if (capture->queryControl(CameraCapture::V4L2_CID_EXPOSURE_AUTO,
                                           expMin, expMax, expStep, expDef) == 0) {
                    capture->getControl(CameraCapture::V4L2_CID_EXPOSURE_AUTO, expVal);
                    LOG_INF("Auto Exposure: cur=%d (1=manual, 3=auto)", expVal);
                    // V4L2_EXPOSURE_MANUAL = 1，强制手动模式以保持帧率
                    if (expVal != 1) {
                        capture->setControl(
                            static_cast<int>(CameraCapture::V4L2_CID_EXPOSURE_AUTO), 1);
                        LOG_INF("Auto Exposure disabled (set to manual) to preserve framerate");
                    }
                    gui.setAutoExposure(false);  // 初始默认关闭自动曝光
                }

                // 曝光绝对值
                int absMin, absMax, absStep, absDef, absCur;
                if (capture->queryControl(CameraCapture::V4L2_CID_EXPOSURE_ABSOLUTE,
                                           absMin, absMax, absStep, absDef) == 0) {
                    capture->getControl(CameraCapture::V4L2_CID_EXPOSURE_ABSOLUTE, absCur);
                    int targetExposure = (absCur > 0) ? absCur : (absDef > 0 ? absDef : 312);
                    if (targetExposure < absMin) targetExposure = absMin;
                    if (targetExposure > absMax) targetExposure = absMax;
                    if (targetExposure > 300) targetExposure = 300;  // 30fps要求曝光<33ms
                    capture->setControl(
                        static_cast<int>(CameraCapture::V4L2_CID_EXPOSURE_ABSOLUTE),
                        targetExposure);
                    gui.setExposureRange(absMin, absMax, absStep, targetExposure);
                    LOG_INF("Exposure: range=[%d,%d] step=%d, set to %d",
                             absMin, absMax, absStep, targetExposure);
                }
            }

            // 帧率 — 查询 V4L2 支持的帧率范围
            {
                int curNum = 1, curDen = 30;
                capture->getFramerate(curNum, curDen);
                int currentFps = (curNum > 0) ? (curDen / curNum) : 30;

                // 尝试枚举设备支持的帧率
                std::vector<int> supportedFps;
                int enumRet = capture->enumFrameRates(
                    capture->getCurrentFormat(),
                    curRes.width, curRes.height,
                    supportedFps);

                if (enumRet == 0 && !supportedFps.empty()) {
                    int minFps = supportedFps.front();
                    int maxFps = supportedFps.back();

                    // 仅枚举到一个离散帧率时，minFps == maxFps，滑块无法滑动
                    // 回退到安全范围 1~60，允许用户尝试其他帧率
                    // （许多 UVC 摄像头虽只报告一个离散帧率，但 VIDIOC_S_PARM 仍可接受其他值）
                    if (minFps == maxFps) {
                        LOG_INF("Framerate: only one discrete rate (%d fps) enumerated, "
                                "falling back to safe range 1-60", minFps);
                        minFps = 1;
                        maxFps = 60;
                    }

                    // 确保当前帧率在范围内
                    if (currentFps < minFps) currentFps = minFps;
                    if (currentFps > maxFps) currentFps = maxFps;
                    gui.setFramerateRange(minFps, maxFps, currentFps);
                    LOG_INF("Framerate: supported=%zu rates, range=[%d, %d], current=%d",
                             supportedFps.size(), minFps, maxFps, currentFps);
                } else {
                    // 设备不支持枚举帧率，使用通用安全范围 1~60
                    gui.setFramerateRange(1, 60, currentFps);
                    LOG_INF("Framerate: enum not supported, using safe range 1-60, current=%d",
                             currentFps);
                }
            }

            // 注册统一回调：滑块变化 → V4L2 setControl
            gui.onCameraControlChanged([capture](int cid, int value) {
                int ret = capture->setControl(cid, value);
                if (ret < 0) {
                    LOG_WRN("setControl(cid=0x%08X, val=%d) failed (ret=%d)",
                             static_cast<uint32_t>(cid), value, ret);
                } else {
                    LOG_INF("Camera control: cid=0x%08X → %d",
                             static_cast<uint32_t>(cid), value);
                }
            });

            // 注册帧率变更回调：滑块变化 → 暂停采集 → 停止流 → 设置帧率 → 重启流 → 恢复采集
            // VIDIOC_S_PARM 在 STREAMON 期间返回 EBUSY，必须先停止流再设置帧率
            gui.onFramerateChanged([capture, rtspServer, &displayTimer](int fps) {
                if (fps <= 0) return;

                // 1. 暂停采集线程，防止 stopCapture 时采集线程还在使用 mmap 缓冲区
                g_state.paused = true;
                {
                    std::unique_lock<std::mutex> lk(g_state.pauseMtx);
                    g_state.pauseCv.wait_until(lk,
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(1100),
                        [] { return g_state.pausedAck.load(); });
                }

                // 2. 停止采集流 → 设置帧率 → 验证 → 重启采集流
                capture->stopCapture();
                int ret = capture->setFramerate(1, fps);
                if (ret < 0) {
                    LOG_WRN("setFramerate(%d) failed (ret=%d), will use software throttle",
                             fps, ret);
                }
                // 注意：即使 setFramerate 返回成功，驱动可能已调整帧率为硬件实际支持的值
                // （setFramerate 内部会输出调整日志）。软件节流会兜底保证目标帧率。
                capture->startCapture();

                // 3. 设置软件帧率节流目标（无论硬件是否真正生效，软件丢帧兜底）
                g_state.targetFps = fps;
                LOG_INF("Software framerate throttle set to %d fps", fps);

                // 4. 恢复采集线程
                g_state.paused = false;
                g_state.pauseCv.notify_one();

                // 5. 同步更新 RTSP 服务器的 SDP 和 RTP 时间戳
                if (rtspServer) {
                    Resolution res = capture->getCurrentResolution();
                    rtspServer->setStreamInfo(res.width, res.height, fps);
                    LOG_INF("RTSP stream info updated: %dx%d @ %dfps",
                             res.width, res.height, fps);
                }

                // 6. 更新显示定时器间隔
                if (displayTimer) {
                    int intervalMs = std::max(10, 1000 / fps);
                    displayTimer->setInterval(intervalMs);
                    LOG_INF("Display timer interval updated to %d ms (target %d fps)",
                             intervalMs, fps);
                }
            });
        }

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
        // 使用 V4L2 查询到的实际帧率，若无则默认 30
        int rtspFps = 30;
        {
            int num = 1, den = 30;
            if (capture->getFramerate(num, den) == 0 && num > 0) {
                rtspFps = den / num;
                if (rtspFps <= 0) rtspFps = 30;
            }
        }
        rtspServer->setStreamInfo(curRes.width, curRes.height,
                                  rtspFps);
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
        // ============================================================
        // 采集线程（仅做 getFrame → 拷贝 → putFrame，不阻塞在推流/录像上）
        // ============================================================
        captureThread = new std::thread([capture]() {
            FrameBuffer fb;
            // 帧率节流：记录上次输出帧的时间戳
            auto lastOutputTime = std::chrono::steady_clock::now();
            int  throttleFps    = g_state.targetFps.load();
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

                // 读取用户设定的目标帧率（可能随时变化）
                throttleFps = g_state.targetFps.load();

                if (capture->getFrame(&fb, 1000) < 0) {
                    if (!g_state.running) break;
                    continue;  // 超时重试
                }

                // ---- 帧率节流 ----
                if (throttleFps > 0) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsedMs = std::chrono::duration_cast<
                        std::chrono::milliseconds>(now - lastOutputTime).count();
                    auto minIntervalMs = 1000 / throttleFps;

                    if (elapsedMs < minIntervalMs) {
                        capture->putFrame(&fb);
                        continue;
                    }
                    lastOutputTime = now;
                }

                // 拷贝帧数据到共享缓冲区（V4L2 mmap 内存不能长期持有）
                {
                    std::lock_guard<std::mutex> lock(g_state.mtx);
                    g_state.frameData.assign(fb.data, fb.data + fb.length);
                    g_state.width  = fb.width;
                    g_state.height = fb.height;
                    g_state.format = fb.format;
                    g_state.fps    = capture->getCurrentFPS();
                }

                // 立即归还 V4L2 缓冲区，让硬件可以写入下一帧
                capture->putFrame(&fb);

                // 通知处理线程：有新帧可用
                {
                    std::lock_guard<std::mutex> lock(g_state.procMtx);
                    g_state.frameReady = true;
                }
                g_state.procCv.notify_one();
            }
        });

        // ============================================================
        // 处理线程（推流 MJPEG/RTSP + 录像，与采集解耦避免阻塞取帧）
        // ============================================================
        std::thread* processThread = new std::thread([mjpegServer, mjpegServerOk,
                                                       rtspServer]() {
            while (g_state.running) {
                // 等待采集线程通知新帧
                {
                    std::unique_lock<std::mutex> lk(g_state.procMtx);
                    g_state.procCv.wait(lk, [] {
                        return g_state.frameReady.load() || !g_state.running.load();
                    });
                    g_state.frameReady = false;
                }

                if (!g_state.running) break;

                // 从共享状态读取帧数据（独立锁，不阻塞采集线程）
                std::vector<uint8_t> localFrame;
                int localW = 0, localH = 0;
                PixelFormat localFmt = PixelFormat::FMT_RGB24;

                {
                    std::lock_guard<std::mutex> lock(g_state.mtx);
                    if (g_state.frameData.empty()) continue;
                    localFrame = g_state.frameData;  // 拷贝出来，快速释放锁
                    localW   = g_state.width;
                    localH   = g_state.height;
                    localFmt = g_state.format;
                }

                // YUYV → JPEG 编码（CPU 密集，不阻塞采集线程）
                bool needEncode = (localFmt == PixelFormat::FMT_YUYV) &&
                                  (mjpegServerOk || rtspServer);
                uint8_t*      jpeg_out = nullptr;
                unsigned long jpeg_len = 0;

                if (needEncode) {
#ifdef HAS_LIBJPEG
                    VideoProcessor::encodeYUYVtoJPEG(
                        localFrame.data(), localW, localH,
                        80, &jpeg_out, &jpeg_len);
#endif
                }

                // 推流到 MJPEG HTTP 服务器
                if (mjpegServerOk) {
                    if (localFmt == PixelFormat::FMT_MJPEG) {
                        mjpegServer->updateFrame(localFrame.data(),
                            static_cast<size_t>(localFrame.size()));
                    } else if (jpeg_out && jpeg_len > 0) {
                        mjpegServer->updateFrame(jpeg_out,
                            static_cast<size_t>(jpeg_len));
                    }
                }

                // 推流到 RTSP 服务器
                if (rtspServer) {
                    if (localFmt == PixelFormat::FMT_MJPEG) {
                        rtspServer->feedFrame(localFrame.data(),
                            static_cast<size_t>(localFrame.size()),
                            localW, localH);
                    } else if (jpeg_out && jpeg_len > 0) {
                        rtspServer->feedFrame(jpeg_out,
                            static_cast<size_t>(jpeg_len),
                            localW, localH);
                    }
                }

                if (jpeg_out) free(jpeg_out);

                // 录像写入（磁盘 I/O，不阻塞采集线程）
                if (g_recording && localFmt == PixelFormat::FMT_MJPEG && g_storage) {
                    g_storage->writeRecordFrame(localFrame.data(),
                        static_cast<int>(localFrame.size()));
                }
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
    g_state.procCv.notify_all();  // 唤醒处理线程使其退出

    if (captureThread && captureThread->joinable()) {
        captureThread->join();
        delete captureThread;
    }

    if (processThread && processThread->joinable()) {
        processThread->join();
        delete processThread;
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
