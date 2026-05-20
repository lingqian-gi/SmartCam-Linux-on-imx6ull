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
 *      - 连接 V4L2 摄像头采集 → GUI 预览
 *      - 需接入真实硬件
 *
 * 编译 & 运行：
 *   mkdir build && cd build
 *   cmake .. && make -j$(nproc)
 *   ./smartcam                    # Mock 模式
 *   ./smartcam --device /dev/video0    # 真实相机（需 iMX6ULL）
 */

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <cstdio>

#include "include/display/gui.h"

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

    parser.process(app);

    QString device = parser.value(deviceOpt);
    int httpPort   = parser.value(portOpt).toInt();

    // ---- 创建 & 显示 GUI ----
    CameraGUI gui;

    // 连接回调 — 在真实设备模式下注入实际逻辑
    gui.onCaptureRequest([]() {
        qDebug() << "[Main] 拍照请求";
        // TODO: 调用 StorageManager::savePhoto()
    });

    gui.onRecordToggle([](bool start) {
        qDebug() << "[Main] 录像切换:" << (start ? "开始" : "停止");
        // TODO: 调用 StorageManager::startRecord() / stopRecord()
    });

    gui.onResolutionChanged([](int w, int h) {
        qDebug() << "[Main] 分辨率变更:" << w << "x" << h;
        // TODO: 调用 CameraCapture::setFormat(w, h, ...)
    });

    gui.onFormatChanged([](PixelFormat fmt) {
        qDebug() << "[Main] 格式变更:" << static_cast<int>(fmt);
        // TODO: 调用 CameraCapture::setFormat(..., fmt)
    });

    gui.show();

    if (device.isEmpty()) {
        qInfo() << "==============================================";
        qInfo() << "SmartCam Linux — Mock 模式";
        qInfo() << "运行在模拟环境中, 显示彩色测试条";
        qInfo() << "传参 --device /dev/video0 切换到真实相机模式";
        qInfo() << "==============================================";
    } else {
        qInfo() << "==============================================";
        qInfo() << "SmartCam Linux — 真实相机模式";
        qInfo() << "设备:" << device;
        qInfo() << "HTTP 端口:" << httpPort;
        qInfo() << "==============================================";
        // TODO: 创建 CameraCapture, 启动采集线程, 连接帧到 GUI
        // 在后续模块集成时实现
    }

    return app.exec();
}
