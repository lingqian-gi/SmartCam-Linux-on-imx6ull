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
#include <ifaddrs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>   // IFF_LOOPBACK

#include "include/display/gui.h"
#include "include/camera/capture.h"
#include "include/camera/processor.h"
#include "include/network/mjpeg_server.h"
#include "include/network/control.h"
#include "include/network/rtsp_server.h"
#include "include/storage/manager.h"
#include "include/common/logger.h"
#include "include/common/config.h"
#include "include/common/frame_pool.h"

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

// ============================================================
// 性能插桩：拷贝字节统计（A/B 对比帧池改造用，改造前后共用）
// ============================================================
// 统计一帧从摄像头到屏幕的全部 memcpy 字节数：
//   ① 采集线程   memcpy → g_state.frameData        (原始帧 JPEG/YUYV)
//   ② 处理线程   localFrame = g_state.frameData     (原始帧，推流/录像)
//   ③ setFrame   m_frameBuffer.assign               (原始帧，GUI 内部)
//   ④ 解码后     QImage(...).copy()                 (RGB24，解码结果上屏前拷贝)
//   ⑤ QPixmap::fromImage                            (RGB24，上屏必需拷贝)
// 每 5s 打印一次：[PERF] copy=xx.xMB/s frames=xxfps cpu=xx% rss=xxMB
// 改造前后跑同场景直接对比输出即可。
struct PerfStats {
    std::atomic<uint64_t> copyBytes{0};    // 累计拷贝字节数（①②③④）
    std::atomic<uint64_t> pixBytes{0};     // 累计上屏拷贝字节数（⑤，单独标注）
    std::atomic<uint64_t> frames{0};       // 累计处理帧数（采集线程 +1）
    std::atomic<uint64_t> cpuJiffies{0};   // 累计 CPU jiffies（utime+stime）
    // 上次采样快照（仅主线程 PERF 定时器读写）
    uint64_t              snapBytes = 0;
    uint64_t              snapPix   = 0;
    uint64_t              snapFrames= 0;
    uint64_t              snapCpu   = 0;
    double                snapTime  = 0.0;
};
static PerfStats g_perf;

// ============================================================
// RGB 显示帧池（帧池零拷贝路径）
// ============================================================
// 生产者：displayTimer（GUI 线程内解码 → 写入槽 → publish）
// 消费者：GUI refreshFrame（share → 浅引用 QImage 绘制 → release）
// 容量 2：GUI 持 1 槽 + 解码写 1 槽，天然双缓冲，无需锁。
FramePool* g_rgbPool = nullptr;

/** @brief 读取 /proc/self/stat 的 utime+stime（第 14+15 字段，单位 jiffies） */
static uint64_t readSelfCpuJiffies() {
    FILE* fp = fopen("/proc/self/stat", "r");
    if (!fp) return 0;
    char buf[512] = {0};
    if (!fgets(buf, sizeof(buf), fp)) { fclose(fp); return 0; }
    fclose(fp);
    // 跳过 "pid (comm) state ..."：comm 可能含空格/括号，从最后一个 ')' 后解析
    char* p = strrchr(buf, ')');
    if (!p) return 0;
    unsigned long long utime = 0, stime = 0;
    // ')' 后是 " state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt
    //   utime stime ..." → utime 是第 12 个字段（从 state 算起第 3 个数值）
    // 简化：sscanf 跳过 state(1) ppid(2) pgrp(3) session(4) tty(5) tpgid(6) flags(7)
    //        minflt(8) cminflt(9) majflt(10) cmajflt(11) utime(12) stime(13)
    int skipped = 0; char state = 0; unsigned long long tmp = 0;
    char* tok = p + 1;
    while (skipped < 12 && tok) {
        // 第 1 个 token 是 state（字符）
        if (skipped == 0) {
            while (*tok == ' ') ++tok;
            state = *tok;
            tok = strchr(tok, ' ') + 1;
            skipped = 1;
            continue;
        }
        while (*tok == ' ') ++tok;
        sscanf(tok, "%llu", &tmp);
        tok = strchr(tok, ' ') + 1;
        skipped++;
    }
    // skipped==12 时 tmp 即 utime；再读一个为 stime
    utime = tmp;
    while (*tok == ' ') ++tok;
    sscanf(tok, "%llu", &stime);
    return utime + stime;
}

/** @brief 读取 /proc/self/status 的 VmRSS（KB） */
static long readSelfRssKB() {
    FILE* fp = fopen("/proc/self/status", "r");
    if (!fp) return 0;
    char line[256];
    long rss = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &rss);
            break;
        }
    }
    fclose(fp);
    return rss;
}

// ============================================================
// 获取本机 IPv4 地址（用于在启动信息里展示浏览器/VLC 访问地址）
// ============================================================
// 遍历所有网卡，返回第一个"非 loopback、非 0.0.0.0"的 IPv4 地址。
// 若获取失败返回 "127.0.0.1"（此时提示用户自行查看 ip addr 配置）。
static std::string getLocalIPv4() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) < 0) {
        LOG_WRN("getifaddrs failed: %s", strerror(errno));
        return "127.0.0.1";
    }

    std::string result = "127.0.0.1";
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;                                   // 只看 IPv4
        if ((ifa->ifa_flags & IFF_LOOPBACK))
            continue;                                   // 跳过回环
        const auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        char ip[INET_ADDRSTRLEN] = {0};
        if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)))
            continue;
        // 跳过 0.0.0.0（地址未分配）
        if (strcmp(ip, "0.0.0.0") == 0)
            continue;
        result = ip;
        break;                                          // 取第一个有效地址
    }

    freeifaddrs(ifaddr);
    return result;
}


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
        // 初始化 RGB 显示帧池（容量 2：GUI 持 1 + 解码写 1）
        // ============================================================
        g_rgbPool = new FramePool(2);

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

        // 把实际生效的采集格式同步到设置面板的 Format 下拉框
        // （避免命令行 --fmt mjpeg 与 GUI 默认显示的 YUYV 不一致）
        gui.setCurrentFormat((curFmt == CameraCapture::V4L2_PIX_FMT_MJPEG)
                                 ? PixelFormat::FMT_MJPEG
                                 : PixelFormat::FMT_YUYV);

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

            // 自动曝光 → 仅查询并更新 GUI，不写硬件（保留摄像头自动曝光）。
            // ★ 修复：强制手动曝光(Exposure=300)会让该摄像头固件进入异常状态
            //   （输出黑帧且状态残留，退出程序后 v4l2-ctl 抓帧也黑）。
            //   v4l2-ctl 实证：不设曝光 → 正常大帧(~100KB)，设曝光300 → 黑帧(~6.7KB)。
            {
                int expMin, expMax, expStep, expDef, expVal;
                if (capture->queryControl(CameraCapture::V4L2_CID_EXPOSURE_AUTO,
                                           expMin, expMax, expStep, expDef) == 0) {
                    capture->getControl(CameraCapture::V4L2_CID_EXPOSURE_AUTO, expVal);
                    LOG_INF("Auto Exposure: cur=%d (1=manual, 3=auto), not forced",
                            expVal);
                    gui.setAutoExposure(expVal != 1);  // 反映当前自动/手动状态
                }

                // 曝光绝对值：只查询范围供 GUI 滑块显示，不写硬件
                int absMin, absMax, absStep, absDef, absCur;
                if (capture->queryControl(CameraCapture::V4L2_CID_EXPOSURE_ABSOLUTE,
                                           absMin, absMax, absStep, absDef) == 0) {
                    capture->getControl(CameraCapture::V4L2_CID_EXPOSURE_ABSOLUTE, absCur);
                    int def = (absCur > 0) ? absCur : (absDef > 0 ? absDef : absMin);
                    gui.setExposureRange(absMin, absMax, absStep, def);
                    LOG_INF("Exposure: not forced (auto kept), range=[%d,%d] cur=%d",
                            absMin, absMax, absCur);
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
            // 诊断：记录实际帧间隔用于定位硬件帧率
            auto  diagLastTime  = std::chrono::steady_clock::now();
            int   diagFrameCount = 0;
            double diagMinInterval = 9999.0, diagMaxInterval = 0.0;

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

                // 诊断：测量实际帧间隔（每 100 帧输出一次）
                {
                    auto now = std::chrono::steady_clock::now();
                    double interval = std::chrono::duration<double>(now - diagLastTime).count();
                    if (interval < diagMinInterval) diagMinInterval = interval;
                    if (interval > diagMaxInterval) diagMaxInterval = interval;
                    diagLastTime = now;
                    diagFrameCount++;

                    if (diagFrameCount % 100 == 0) {
                        double avgMs = 0;
                        {
                            std::lock_guard<std::mutex> lock(g_state.mtx);
                            avgMs = (g_state.fps > 0) ? 1000.0 / g_state.fps : 0;
                        }
                        LOG_INF("[FPS Diag] avg=%.1f fps (%.1f ms/frame), "
                                "raw interval: min=%.1f ms, max=%.1f ms, "
                                "frame size=%d bytes, throttle=%d fps",
                                (avgMs > 0 ? 1000.0/avgMs : 0), avgMs,
                                diagMinInterval * 1000.0, diagMaxInterval * 1000.0,
                                fb.length, throttleFps);
                        diagMinInterval = 9999.0;
                        diagMaxInterval = 0.0;
                    }
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
                // [PERF] ① 采集线程 → g_state.frameData 的 memcpy
                g_perf.copyBytes += fb.length;
                g_perf.frames++;   // 计为"处理帧数"（一帧的入口）

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

                // ---- 低风险优化：无人观看时跳过推流分发与深拷贝 ----
                // 若没有 HTTP 客户端、没有 RTSP 客户端、且未录像，则本帧无人消费，
                // 跳过深拷贝/编码/推流，给单核 CPU 减负（提高采集帧率）。
                const bool hasHttpViewer = mjpegServerOk &&
                                           mjpegServer->clientCount() > 0;
                const bool hasRtspViewer = rtspServer &&
                                           rtspServer->clientCount() > 0;
                if (!hasHttpViewer && !hasRtspViewer && !g_recording) {
                    continue;   // 无人消费，直接进入下一轮等待（frameReady 已清）
                }

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
                // [PERF] ② 处理线程 localFrame 深拷贝（推流/录像用）
                g_perf.copyBytes += localFrame.size();

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
                if (hasHttpViewer) {
                    if (localFmt == PixelFormat::FMT_MJPEG) {
                        mjpegServer->updateFrame(localFrame.data(),
                            static_cast<size_t>(localFrame.size()));
                    } else if (jpeg_out && jpeg_len > 0) {
                        mjpegServer->updateFrame(jpeg_out,
                            static_cast<size_t>(jpeg_len));
                    }
                }

                // 推流到 RTSP 服务器
                if (hasRtspViewer) {
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
        // 显示定时器（Qt 主线程，33ms ≈ 30fps）— 帧池零拷贝路径
        // ============================================================
        // 流程：借 rgb 槽 → 解码/转换入槽 → publish → share → setFrameShared
        // → requestRefresh（发布后立即上屏）
        // 消除了旧路径的 2 次 RGB24 深拷贝（setFrame 内 assign + QImage.copy()）。
        // 解码仍在 GUI 线程（单核板上线程无并行收益，见 docs 实施指南 §2.3）。
        //
        // 完全单驱动：CameraGUI 无内部刷新定时器，上屏统一走 requestRefresh()。
        // 本定时器在发布新帧后立即上屏——无任何相位延迟，且全项目只有一个驱动入口
        // （Mock 模式复用同一入口，由下方 Mock 分支的定时器驱动彩条）。
        displayTimer = new QTimer(&gui);
        displayTimer->setInterval(33);
        // 显示 FPS 统计：每 30 次实际渲染一帧算一次平均（反映真正显示速率）
        auto dispFpsLastTime = std::chrono::steady_clock::now();
        int  dispFpsCount    = 0;
        double dispFps       = 0.0;
        QObject::connect(displayTimer, &QTimer::timeout,
            [&gui, mjpegServer, &dispFpsLastTime, &dispFpsCount, &dispFps]() {
            // 1. 借 RGB 写槽（无空闲则丢帧，不阻塞）
            FrameSlot* slot = g_rgbPool->acquire();
            if (!slot) return;

            // 2. 取原始帧数据（短锁拷贝出共享区）
            std::vector<uint8_t> raw;
            int srcW = 0, srcH = 0;
            PixelFormat srcFmt = PixelFormat::FMT_RGB24;
            {
                std::lock_guard<std::mutex> lock(g_state.mtx);
                if (g_state.frameData.empty()) {
                    g_rgbPool->release(slot);
                    return;
                }
                raw    = g_state.frameData;   // 原始帧拷贝（JPEG ~0.1MB，唯一）
                srcW   = g_state.width;
                srcH   = g_state.height;
                srcFmt = g_state.format;
            }

            // 3. 解码/转换为 RGB24，直接写入池槽（消除 setFrame 的二次拷贝）
            slot->width  = srcW;
            slot->height = srcH;
            slot->format = PixelFormat::FMT_RGB24;
            if (srcFmt == PixelFormat::FMT_MJPEG) {
#ifdef HAS_LIBJPEG
                int dw = 0, dh = 0;
                if (VideoProcessor::decodeJPEGtoRGB(raw.data(), raw.size(),
                                                    slot->data, dw, dh)) {
                    slot->width  = dw;
                    slot->height = dh;
                } else {
                    g_rgbPool->release(slot);   // 坏帧丢帧
                    return;
                }
#endif
            } else if (srcFmt == PixelFormat::FMT_YUYV) {
                slot->data.resize(static_cast<size_t>(srcW) * srcH * 3);
                VideoProcessor::yuyvToRgb24(raw.data(), slot->data.data(), srcW, srcH);
            } else {   // FMT_RGB24：直拷
                slot->data = std::move(raw);
            }

            // [PERF] ③④ 已消除：解码直接写池槽（零拷贝），不再有 setFrame assign / QImage.copy()
            // [PERF] 本函数 raw = g_state.frameData 是一次原始帧拷贝（JPEG ~0.1MB），计入
            g_perf.copyBytes += raw.size();
            // [PERF] ⑤ 上屏拷贝：QImage 浅引用构造（不拷贝）→ setPixmap 时 QPixmap::fromImage
            //          做一次 RGB24 拷贝（上屏必需），计入 pixBytes
            g_perf.pixBytes += static_cast<uint64_t>(slot->width) *
                               slot->height * 3;

            // 4. 发布并交 GUI 共享（setFrameShared 内部持有引用，零拷贝上屏）
            slot->seq++;
            g_rgbPool->publish(slot);
            FrameSlot* displaySlot = g_rgbPool->share();
            if (displaySlot) {
                gui.setFrameShared(displaySlot);   // GUI 接管此引用
            }
            // 5. 发布后立即上屏（浅引用 setPixmap，近乎零开销）
            //    完全单驱动：requestRefresh 是全局唯一上屏入口，无相位延迟
            gui.requestRefresh();

            // 统计实际显示 FPS（每 30 次成功渲染一帧算一次平均）
            dispFpsCount++;
            if (dispFpsCount % 30 == 0) {
                auto now = std::chrono::steady_clock::now();
                double elapsed = std::chrono::duration<double>(now - dispFpsLastTime).count();
                if (elapsed > 0.0) dispFps = 30.0 / elapsed;
                dispFpsLastTime = now;
            }

            gui.setFPS(g_state.fps);             // 采集线程取帧速率
            gui.setDisplayFPS(dispFps);          // 实际显示速率
            gui.setClientCount(mjpegServer->clientCount());
        });
        displayTimer->start();

        // ============================================================
        // 性能统计定时器（每 5s 打印一次 [PERF] 行，A/B 对比用）
        // ============================================================
        QTimer* perfTimer = new QTimer(&gui);
        perfTimer->setInterval(5000);
        QObject::connect(perfTimer, &QTimer::timeout, []() {
            uint64_t bytes  = g_perf.copyBytes.load();
            uint64_t pix    = g_perf.pixBytes.load();
            uint64_t frames = g_perf.frames.load();
            uint64_t cpu    = g_perf.cpuJiffies.load();
            double now = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            double dt = now - g_perf.snapTime;
            if (dt <= 0.0) { g_perf.snapTime = now; return; }

            double copyMB = (bytes - g_perf.snapBytes) / dt / 1e6;
            double pixMB  = (pix   - g_perf.snapPix)   / dt / 1e6;
            double fps    = (frames - g_perf.snapFrames) / dt;
            double cpuPct = (cpu - g_perf.snapCpu) / dt / 100.0 * 100.0;  // jiffies=100Hz
            long   rssKB = readSelfRssKB();

            // 更新快照
            g_perf.snapBytes  = bytes;
            g_perf.snapPix    = pix;
            g_perf.snapFrames = frames;
            g_perf.snapCpu    = cpu;
            g_perf.snapTime   = now;

            // 采集线程记录 CPU 用 main 线程视角不准，改用 /proc/self 全进程统计：
            // 此处直接用 readSelfCpuJiffies 的差值（覆盖所有线程）
            static uint64_t lastSelfCpu = 0;
            uint64_t selfCpu = readSelfCpuJiffies();
            if (lastSelfCpu > 0) {
                double cpuPctAll = (selfCpu - lastSelfCpu) / dt / 100.0 * 100.0;
                LOG_INF("[PERF] copy=%.1fMB/s (+pix %.1f) frames=%.1ffps cpu=%.0f%% rss=%ldKB",
                        copyMB, pixMB, fps, cpuPctAll, rssKB);
            } else {
                LOG_INF("[PERF] copy=%.1fMB/s (+pix %.1f) frames=%.1ffps cpu=%.0f%% rss=%ldKB",
                        copyMB, pixMB, fps, cpuPct, rssKB);
            }
            lastSelfCpu = selfCpu;
        });
        perfTimer->start();

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

        // 查询本机 IP 并展示访问地址（浏览器 / VLC）
        const std::string devIp = getLocalIPv4();
        qInfo() << "浏览器打开: http://" << QString::fromStdString(devIp) << ":" << httpPort << "/";
        qInfo() << "VLC 播放:   rtsp://" << QString::fromStdString(devIp) << ":" << rtspPort << "/stream";
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

        // ---- Mock 显示驱动（完全单驱动：复用 requestRefresh 入口）----
        // CameraGUI 已无内部刷新定时器，此处用与真实模式相同的 requestRefresh()
        // 驱动 Mock 彩条滚动（refreshFrame 内 m_mockMode 分支），全项目单一驱动入口。
        displayTimer = new QTimer(&gui);
        displayTimer->setInterval(33);
        QObject::connect(displayTimer, &QTimer::timeout, [&gui]() {
            gui.requestRefresh();   // Mock 彩条滚动上屏
        });
        displayTimer->start();

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

    // g_rgbPool 不在此 delete：GUI(gui) 是栈对象，其 m_heldSlot 引用在函数返回后
    // 的析构中释放；若在此 delete 池，gui 析构时 g_rgbPool 已悬垂。
    // 池内存极小（2 槽 ≈ 1.84MB），进程退出时由 OS 回收。

    return ret;
}
