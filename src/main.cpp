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
#include <vector>
#include <cstring>

#include "include/display/gui.h"
#include "include/camera/capture.h"
#include "include/common/logger.h"

// ============================================================
// 全局共享状态（采集线程 → GUI 线程）
// ============================================================
struct CaptureState {
    std::mutex              mtx;
    std::vector<uint8_t>    frameData;   // 拷贝后的帧数据
    int                     width  = 0;
    int                     height = 0;
    PixelFormat             format = PixelFormat::FMT_RGB24;
    double                  fps    = 0.0;
    std::atomic<bool>       running{false};
};
static CaptureState g_state;

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
        "8080"
    );
    parser.addOption(portOpt);

    QCommandLineOption fmtOpt(
        QStringLiteral("fmt"),
        QStringLiteral("像素格式: yuyv | mjpeg (默认 yuyv)"),
        QStringLiteral("fmt"),
        "yuyv"
    );
    parser.addOption(fmtOpt);

    parser.process(app);

    QString device  = parser.value(deviceOpt);
    int     httpPort = parser.value(portOpt).toInt();
    QString fmtStr  = parser.value(fmtOpt).toLower();

    // ---- 创建 & 显示 GUI ----
    CameraGUI gui;

    // ---- 真实相机模式 ----
    CameraCapture*    capture      = nullptr;
    std::thread*      captureThread = nullptr;
    QTimer*           displayTimer = nullptr;

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
        // 启动采集线程（连续拉帧 → 拷贝到共享缓冲区）
        // ============================================================
        captureThread = new std::thread([capture]() {
            FrameBuffer fb;

            while (g_state.running) {
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

                // 归还缓冲区到 V4L2 队列
                capture->putFrame(&fb);
            }
        });

        // ============================================================
        // 显示定时器（Qt 主线程，33ms ≈ 30fps）
        // ============================================================
        displayTimer = new QTimer(&gui);
        displayTimer->setInterval(33);
        QObject::connect(displayTimer, &QTimer::timeout, [&gui]() {
            std::lock_guard<std::mutex> lock(g_state.mtx);
            if (g_state.frameData.empty()) return;

            // 直接使用 GUI 的 setFrame 接口
            // 内部 frameToQImage() 会处理 YUYV→RGB24 转换并深拷贝到 QImage
            gui.setFrame(g_state.frameData.data(),
                         static_cast<int>(g_state.frameData.size()),
                         g_state.width, g_state.height,
                         g_state.format);

            gui.setFPS(g_state.fps);
        });
        displayTimer->start();

        // ============================================================
        // 连接回调：分辨率/格式变更 → 重新配置摄像头
        // ============================================================
        gui.onResolutionChanged([capture](int w, int h) {
            if (capture->isStreaming()) {
                capture->stopCapture();
                capture->setFormat(w, h, capture->getCurrentFormat());
                capture->startCapture();
                LOG_INF("Resolution changed to %dx%d", w, h);
            }
        });

        gui.onFormatChanged([capture, device](PixelFormat fmt) {
            uint32_t v4l2fmt = (fmt == PixelFormat::FMT_YUYV)
                                   ? CameraCapture::V4L2_PIX_FMT_YUYV
                                   : CameraCapture::V4L2_PIX_FMT_MJPEG;
            if (capture->isStreaming()) {
                capture->stopCapture();
                capture->setFormat(640, 480, v4l2fmt);
                capture->startCapture();
                LOG_INF("Format changed to %s",
                         (fmt == PixelFormat::FMT_YUYV) ? "YUYV" : "MJPEG");
            }
        });

        gui.onCaptureRequest([capture]() {
            LOG_INF("Capture request — will be handled by StorageManager later");
            // TODO: 集成 StorageManager::savePhoto()
        });

        gui.onRecordToggle([capture](bool start) {
            LOG_INF("Record %s — will be handled by StorageManager later",
                     start ? "start" : "stop");
            // TODO: 集成 StorageManager::startRecord() / stopRecord()
        });

        qInfo() << "==============================================";
        qInfo() << "SmartCam Linux — 真实相机模式";
        qInfo() << "设备:" << device;
        qInfo() << "格式:" << fmtStr;
        qInfo() << "HTTP 端口:" << httpPort;
        qInfo() << "==============================================";

    } else {
        // ============================================================
        // Mock 模式（无硬件，显示彩条）
        // ============================================================
        gui.onCaptureRequest([]() {
            qDebug() << "[Main] 拍照请求 (Mock)";
        });
        gui.onRecordToggle([](bool start) {
            qDebug() << "[Main] 录像切换:" << (start ? "开始" : "停止");
        });
        gui.onResolutionChanged([](int w, int h) {
            qDebug() << "[Main] 分辨率变更:" << w << "x" << h;
        });
        gui.onFormatChanged([](PixelFormat fmt) {
            qDebug() << "[Main] 格式变更:" << static_cast<int>(fmt);
        });

        qInfo() << "==============================================";
        qInfo() << "SmartCam Linux — Mock 模式";
        qInfo() << "运行在模拟环境中, 显示彩色测试条";
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

    if (capture) {
        capture->release();
        delete capture;
    }

    return ret;
}
