# SmartCam-Linux-on-imx6ull — 逐文件源代码精读

> **目标平台**: i.MX6ULL (Cortex-A7, armv7-a) + 7寸LCD触摸屏 (800x480)  
> **核心技术栈**: C++17 + Qt5 Widgets/linuxfb + V4L2 + libjpeg-turbo + POSIX sockets  
> **整体架构**: V4L2摄像头采集 → 格式转换/编码 → GUI实时预览 + MJPEG/RTSP推流 + 拍照/录像存储 + TCP私有控制协议

---

## 目录

1. [构建系统 — CMakeLists.txt](#1-cmakeliststxt)
2. [公共基础模块 — include/common/](#2-公共基础模块)
   - [types.h](#21-typesh)
   - [logger.h](#22-loggerh)
   - [config.h](#23-configh)
   - [ringbuf.h](#24-ringbufh)
3. [摄像头模块 — include/camera/ & src/camera/](#3-摄像头模块)
   - [capture.h](#31-captureh)
   - [capture.cpp](#32-capturecpp)
   - [processor.h](#33-processorh)
   - [processor.cpp](#34-processorcpp)
   - [processor_neon.cpp](#35-processor_neoncpp)
4. [显示模块 — include/display/ & src/display/](#4-显示模块)
   - [gui.h](#41-guih)
   - [gui.cpp](#42-guicpp)
   - [gallery.h / gallery.cpp](#43-galleryh--gallerycpp)
   - [video_player.h / video_player.cpp](#44-video_playerh--video_playercpp)
5. [网络模块 — include/network/ & src/network/](#5-网络模块)
   - [mjpeg_server.h / .cpp](#51-mjpeg_serverh--cpp)
   - [control.h / control.cpp](#52-controlh--controlcpp)
   - [rtsp_server.h / rtsp_server.cpp](#53-rtsp_serverh--rtsp_servercpp)
6. [存储模块 — include/storage/ & src/storage/](#6-存储模块)
   - [manager.h / manager.cpp](#6-storage-h--managercpp)
7. [主入口 — src/main.cpp](#7-maincpp)

---

## 1. CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.10)
project(SmartCam VERSION 0.1.0 LANGUAGES CXX C)

# ============================================================
# 编译选项
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 根据平台自动选择 flags
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    # ARM 交叉编译 (iMX6ULL Cortex-A7)
    add_compile_options(-march=armv7-a -mfpu=neon -mfloat-abi=hard -O2)
    message(STATUS "✓ ARM 交叉编译模式 (Cortex-A7 NEON)")
else()
    # x86 PC 开发调试
    add_compile_options(-O2 -g)
    message(STATUS "✓ x86 本地编译模式")
endif()

# ============================================================
# 依赖
# ============================================================

# Qt5 (Widgets)
find_package(Qt5 REQUIRED COMPONENTS Widgets)

# libjpeg-turbo (MJPEG 解码, 自定义错误处理器抑制坏帧警告)
find_package(JPEG REQUIRED)

# ARM 交叉编译时强制静态链接 libjpeg，避免开发板缺少 .so 库
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    find_library(JPEG_STATIC_LIB NAMES libjpeg.a libjpeg-turbo.a
                 PATHS ${CMAKE_FIND_ROOT_PATH}/usr/lib/arm-linux-gnueabihf
                       ${CMAKE_FIND_ROOT_PATH}/usr/lib/arm-linux-gnueabi
                       ${CMAKE_FIND_ROOT_PATH}/usr/lib
                       /usr/lib/arm-linux-gnueabihf
                       /usr/lib/arm-linux-gnueabi
                       ${JPEG_ROOT}
                       ${JPEG_LIBRARY_DIRS})
    if(JPEG_STATIC_LIB)
        set(JPEG_LIBRARIES ${JPEG_STATIC_LIB})
        message(STATUS "✓ ARM 静态链接 libjpeg: ${JPEG_STATIC_LIB}")
    else()
        message(WARNING "⚠ 未找到静态 libjpeg.a，回退到动态链接 (可能缺少 .so)")
    endif()
endif()

# ============================================================
# 源文件 (按模块组织)
# ============================================================

set(CAMERA_SOURCES
    src/camera/capture.cpp
    src/camera/processor.cpp
    src/camera/processor_neon.cpp
    include/camera/capture.h
    include/camera/processor.h
)

set(DISPLAY_SOURCES
    src/display/gui.cpp
    include/display/gui.h    # 需要 MOC 处理 (含 Q_OBJECT)
    src/display/gallery.cpp
    include/display/gallery.h  # 需要 MOC 处理 (含 Q_OBJECT)
    src/display/video_player.cpp
    include/display/video_player.h  # 需要 MOC 处理 (含 Q_OBJECT)
)

set(NETWORK_SOURCES
    src/network/mjpeg_server.cpp
    include/network/mjpeg_server.h
    src/network/control.cpp
    include/network/control.h
    src/network/rtsp_server.cpp
    include/network/rtsp_server.h
)

set(STORAGE_SOURCES
    src/storage/manager.cpp
    include/storage/manager.h
)

set(MAIN_SOURCES
    src/main.cpp
)

set(ALL_SOURCES
    ${CAMERA_SOURCES}
    ${DISPLAY_SOURCES}
    ${NETWORK_SOURCES}
    ${STORAGE_SOURCES}
    ${MAIN_SOURCES}
)

# 启用 Qt 元对象编译器自动处理
set(CMAKE_AUTOMOC ON)

# ============================================================
# 可执行文件
# ============================================================

add_executable(smartcam ${ALL_SOURCES})

# ARM 交叉编译时静态链接 C/C++ 标准库，消除 glibc/libstdc++ 版本不匹配问题
# （开发板可能跑 Debian oldstable，glibc 比 Docker 里的新很多）
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    target_link_options(smartcam PRIVATE -static-libstdc++ -static-libgcc)
    message(STATUS "✓ ARM 静态链接 libstdc++ / libgcc")
endif()

target_include_directories(smartcam PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(smartcam PRIVATE
    Qt5::Widgets
    pthread
)

# libjpeg-turbo (MJPEG 解码)
target_compile_definitions(smartcam PRIVATE HAS_LIBJPEG)
target_include_directories(smartcam PRIVATE ${JPEG_INCLUDE_DIRS})
target_link_libraries(smartcam PRIVATE ${JPEG_LIBRARIES})
message(STATUS "✓ libjpeg-turbo 已启用 (libs=${JPEG_LIBRARIES})")

# ============================================================
# 安装 (部署到开发板)
# ============================================================

install(TARGETS smartcam
    RUNTIME DESTINATION /usr/local/bin
)

install(FILES configs/smartcam.service
    DESTINATION /etc/systemd/system
    OPTIONAL
)

install(FILES configs/smartcam.conf
    DESTINATION /etc/smartcam
    OPTIONAL
)

# ============================================================
# 打印构建摘要
# ============================================================

message(STATUS "")
message(STATUS "SmartCam 构建配置:")
message(STATUS "  Qt 版本:     ${Qt5_VERSION}")
message(STATUS "  C++ 标准:    C++${CMAKE_CXX_STANDARD}")
message(STATUS "  源文件数:    ${CMAKE_PROJECT_NAME}")
message(STATUS "")
message(STATUS "使用方法:")
message(STATUS "  cd build && cmake .. && make -j\$(nproc)")
message(STATUS "  ./smartcam                                    # Mock 模式 (PC 测试)")
message(STATUS "  ./smartcam --device /dev/video0                # 真实相机 (需 V4L2)")
message(STATUS "  ./smartcam --device /dev/video0 --fmt yuyv     # YUYV 模式")
message(STATUS "  # 开发板无 X server，linuxfb 后端 + 修复触摸权限:")
message(STATUS "  export QT_QPA_FB_HIDECURSOR=1")
message(STATUS "  # linuxfb 自动检测触摸设备，如无响应则 chmod 或加入 input 组")
message(STATUS "  sudo chmod 666 /dev/input/event2")
message(STATUS "  ./smartcam --device /dev/video0 --fmt yuyv -platform linuxfb")
message(STATUS "")
```

### 逐段精读

**1. 项目基础配置 (行 1-7)**
```cmake
cmake_minimum_required(VERSION 3.10)
project(SmartCam VERSION 0.1.0 LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 17)
```
- 要求 CMake ≥ 3.10（Ubuntu 18.04 自带 3.10，兼容性很好）
- 启用两种语言：`CXX` 用于主代码，`C` 用于 libjpeg C API 的 `extern "C"` 包裹
- C++17 标准，`std::optional`、结构化绑定、`if constexpr` 等特性随即可用

**2. 平台自适应编译选项 (行 9-20)**
```cmake
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    add_compile_options(-march=armv7-a -mfpu=neon -mfloat-abi=hard -O2)
```
- **关键设计**：同一份 CMakeLists 支持 PC（x86）和 ARM 板端编译
- 条件判断用 `CMAKE_CROSSCOMPILING`：交叉编译时加 `-mfpu=neon` 启用 NEON SIMD（`processor_neon.cpp` 依赖此标志）
- `-mfloat-abi=hard` 是关键——i.MX6ULL Cortex-A7 有硬件浮点单元（VFPv4），hard ABI 用 FPU 寄存器传浮点参数，性能远超 soft float
- PC 端：`-O2 -g` 保留调试符号

**3. 依赖查找 (行 24-58)**
```cmake
find_package(Qt5 REQUIRED COMPONENTS Widgets)
find_package(JPEG REQUIRED)
```
- `Qt5::Widgets` → 提供 QApplication/QWidget/QPushButton/QTimer 等
- `JPEG` → CMake 内置的 FindJPEG 模块，自动找 libjpeg 或 libjpeg-turbo

**ARM 静态链接逻辑 (行 32-53)** 是最精巧的部分：
```cmake
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    find_library(JPEG_STATIC_LIB NAMES libjpeg.a libjpeg-turbo.a ...)
```
- 嵌入式开发板可能没有 `libjpeg.so` 或版本不匹配，所以**优先找 `.a` 静态库直接链接到可执行文件**
- 搜索路径包括 sysroot 内的标准位置（交叉编译 Docker 环境在 `/workspace/npi-sysroot/` 中）

**4. 源文件分组 (行 62-112)**
```cmake
set(CAMERA_SOURCES      src/camera/capture.cpp src/camera/processor.cpp ...)
set(DISPLAY_SOURCES     src/display/gui.cpp include/display/gui.h ...)
set(NETWORK_SOURCES     src/network/mjpeg_server.cpp ...)
set(STORAGE_SOURCES     src/storage/manager.cpp ...)
set(MAIN_SOURCES        src/main.cpp)
```
- **模块化分组**：摄像头/显示/网络/存储 四大模块 + 主入口
- 注意 `DISPLAY_SOURCES` 和 `NETWORK_SOURCES` 中都包含了 `.h` 文件——这是给 MOC (Qt Meta-Object Compiler) 用的
- `include/display/gui.h` 等头文件因为包含 `Q_OBJECT` 宏，必须经 MOC 处理生成 `moc_*.cpp`

**5. 最终目标 (行 114-140)**
```cmake
set(CMAKE_AUTOMOC ON)
add_executable(smartcam ${ALL_SOURCES})
target_link_libraries(smartcam PRIVATE Qt5::Widgets pthread ${JPEG_LIBRARIES})
target_compile_definitions(smartcam PRIVATE HAS_LIBJPEG)
```
- `CMAKE_AUTOMOC ON`：CMake 自动扫描源文件中的 `Q_OBJECT`，调用 moc 生成元对象代码
- 链接库：Qt5::Widgets（GUI）、pthread（多线程）、libjpeg（MJPEG 编解码）
- `HAS_LIBJPEG` 宏：所有 `.cpp` 中 `#ifdef HAS_LIBJPEG` 的条件编译依据

**6. 安装规则 (行 144-156)**
- 部署到开发板：可执行文件 → `/usr/local/bin`，systemd 服务 → `/etc/systemd/system`，配置 → `/etc/smartcam`

---

## 2. 公共基础模块

### 2.1 types.h

```cpp
#ifndef SMART_CAM_COMMON_TYPES_H
#define SMART_CAM_COMMON_TYPES_H

#include <cstdint>
#include <cstddef>
#include <chrono>
#include <QMetaType>

/**
 * @brief 像素格式枚举
 */
enum class PixelFormat : uint32_t {
    FMT_YUYV   = 0x56595559,   // V4L2_PIX_FMT_YUYV  (YUYV 4:2:2)
    FMT_MJPEG  = 0x47504A4D,   // V4L2_PIX_FMT_MJPEG
    FMT_RGB24  = 0x01010101,   // 内部格式: RGB 24bit
    FMT_RGB565 = 0x01010102,   // 内部格式: RGB 16bit (565)
};

/**
 * @brief 分辨率
 */
struct Resolution {
    int width;
    int height;

    bool operator==(const Resolution& o) const {
        return width == o.width && height == o.height;
    }
    bool operator!=(const Resolution& o) const { return !(*this == o); }
};

// 常用分辨率
inline constexpr Resolution RES_640x480  { 640,  480 };
inline constexpr Resolution RES_320x240  { 320,  240 };
inline constexpr Resolution RES_1280x720 { 1280, 720 };

// 注册到 Qt 元对象系统 (用于 QVariant 存储)
Q_DECLARE_METATYPE(Resolution)

/**
 * @brief 帧缓冲区 — 在模块间传递帧数据
 */
struct FrameBuffer {
    uint8_t*  data     = nullptr;   // 帧数据指针
    int       length   = 0;         // 数据长度（字节）
    int       width    = 0;
    int       height   = 0;
    PixelFormat format = PixelFormat::FMT_RGB24;
    int       index    = 0;         // 帧序号（递增）
    std::chrono::steady_clock::time_point timestamp;

    FrameBuffer() : timestamp(std::chrono::steady_clock::now()) {}
};

/**
 * @brief 相机状态
 */
struct CameraStatus {
    bool     streaming    = false;
    bool     recording    = false;
    int      fps          = 0;
    Resolution resolution = RES_640x480;
    PixelFormat format    = PixelFormat::FMT_YUYV;
    int      client_count = 0;      // 网络客户端数
};

#endif // SMART_CAM_COMMON_TYPES_H
```

#### 逐段精读

**PixelFormat 枚举 (行 13-18)**
```cpp
enum class PixelFormat : uint32_t {
    FMT_YUYV   = 0x56595559,   // 'Y','U','Y','V' = YUYV (小端)
    FMT_MJPEG  = 0x47504A4D,   // 'M','J','P','G'
    FMT_RGB24  = 0x01010101,
    FMT_RGB565 = 0x01010102,
};
```
- 使用 `enum class` (C++11) 保证类型安全，避免隐式转换为 int
- `FMT_YUYV` 和 `FMT_MJPEG` 的值与 V4L2 的 FOURCC 码**完全一致**（Linux 内核的 `v4l2_fourcc('Y','U','Y','V')` 在小端序机器上得到 `0x56595559`）
- `FMT_RGB24` 和 `FMT_RGB565` 是**内部自定义**格式，V4L2 不直接支持，用于 GUI 渲染中间缓冲

**FrameBuffer (行 38-49)**
```cpp
struct FrameBuffer {
    uint8_t*  data     = nullptr;
    int       length   = 0;
    ...
};
```
- 这是**整个系统的核心数据传输结构体**，所有模块（采集/显示/推流/存储）都通过它传递帧数据
- `data` 指针**不拥有内存**：在采集线程中指向 V4L2 mmap 映射的内核缓冲区（零拷贝），在 GUI 中指向深拷贝后的内部 `m_frameBuffer` 向量
- `timestamp` 用 `steady_clock`（单调递增时钟，不受系统时间调整影响，适合测量时间间隔）

**Q_DECLARE_METATYPE(Resolution) (行 33)**
```cpp
Q_DECLARE_METATYPE(Resolution)
```
- 关键宏：让 Qt 的 `QVariant` 系统能够存储/传递自定义的 `Resolution` 类型
- 配合 `QVariant::fromValue(Resolution{640,480})` 和 `QVariant::value<Resolution>()` 使用
- 在 `gui.cpp` 的 `QComboBox::setItemData()` 中实际用到

---

### 2.2 logger.h

```cpp
#ifndef SMART_CAM_COMMON_LOGGER_H
#define SMART_CAM_COMMON_LOGGER_H

#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <cstring>
#include <mutex>
#include <string>

// syslog 仅 Linux 可用
#ifndef _WIN32
#include <syslog.h>
#endif

/**
 * @brief 日志级别
 */
enum class LogLevel {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3,
    NONE  = 4   // 关闭所有日志
};

/**
 * @brief 日志管理器（单例）
 */
class Logger {
public:
    static Logger* instance() {
        static Logger inst;
        return &inst;
    }

    // 禁用拷贝
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void setLevel(LogLevel level) { m_level = level; }
    LogLevel level() const { return m_level; }

    void setSyslogEnabled(bool enabled) {
#ifndef _WIN32
        m_useSyslog = enabled;
#endif
    }

    void setTimestampEnabled(bool enabled) { m_showTimestamp = enabled; }

    /**
     * @brief 核心日志方法
     */
    void log(LogLevel level, const char* file, int line,
             const char* func, const char* fmt, ...) {
        if (level < m_level) return;

        std::lock_guard<std::mutex> lock(m_mtx);

        // 格式化的消息
        char msg[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);

        output(level, file, line, func, msg);
    }

private:
    Logger()
        : m_level(LogLevel::DEBUG)
        , m_useSyslog(false)
        , m_showTimestamp(true)
    {
    }

    void output(LogLevel level, const char* file, int line,
                const char* func, const char* msg) {
        const char* levelStr = "";
        const char* color    = "";

        switch (level) {
        case LogLevel::DEBUG: levelStr = "DEBUG"; color = "\033[36m"; break;  // 青
        case LogLevel::INFO:  levelStr = "INFO";  color = "\033[32m"; break;  // 绿
        case LogLevel::WARN:  levelStr = "WARN";  color = "\033[33m"; break;  // 黄
        case LogLevel::ERROR: levelStr = "ERROR"; color = "\033[31m"; break;  // 红
        default: break;
        }

        // 简易文件名（去掉路径）
        const char* shortFile = file;
        const char* slash = strrchr(file, '/');
        if (slash) shortFile = slash + 1;

        // 时间戳
        char timebuf[32] = {0};
        if (m_showTimestamp) {
            time_t now = time(nullptr);
            struct tm* tm_info = localtime(&now);
            strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info);
        }

        // 控制台输出（带颜色）
        if (m_showTimestamp) {
            fprintf(stdout, "%s %s[%s]%s %s:%d (%s) %s\n",
                    timebuf, color, levelStr, "\033[0m",
                    shortFile, line, func, msg);
        } else {
            fprintf(stdout, "%s[%s]%s %s:%d (%s) %s\n",
                    color, levelStr, "\033[0m",
                    shortFile, line, func, msg);
        }
        fflush(stdout);

        // syslog 输出
#ifndef _WIN32
        if (m_useSyslog) {
            static bool syslogOpened = false;
            if (!syslogOpened) {
                openlog("smartcam", LOG_PID | LOG_NDELAY, LOG_USER);
                syslogOpened = true;
            }
            int prio = LOG_INFO;
            switch (level) {
            case LogLevel::DEBUG: prio = LOG_DEBUG;   break;
            case LogLevel::INFO:  prio = LOG_INFO;     break;
            case LogLevel::WARN:  prio = LOG_WARNING;  break;
            case LogLevel::ERROR: prio = LOG_ERR;      break;
            default: break;
            }
            syslog(prio, "%s:%d (%s) %s", shortFile, line, func, msg);
        }
#endif
    }

    LogLevel  m_level;
    bool      m_useSyslog;
    bool      m_showTimestamp;
    std::mutex m_mtx;
};

// ============================================================
// 便捷宏 — 避免与 syslog.h 的常量宏冲突
// ============================================================

#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARNING
#undef LOG_ERR

#define LOG_DBG(fmt, ...) \
    Logger::instance()->log(LogLevel::DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_INF(fmt, ...) \
    Logger::instance()->log(LogLevel::INFO,  __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_WRN(fmt, ...) \
    Logger::instance()->log(LogLevel::WARN,  __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_ERR_(fmt, ...) \
    Logger::instance()->log(LogLevel::ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#endif // SMART_CAM_COMMON_LOGGER_H
```

#### 逐段精读

**单例模式 (行 38-40)**
```cpp
static Logger* instance() {
    static Logger inst;     // C++11 保证线程安全的静态局部变量初始化
    return &inst;
}
```
- **Meyers' Singleton**：C++11 标准保证多线程环境下 `static` 局部变量只初始化一次，无需手动加锁

**核心日志方法 `log()` (行 55-69)**
```cpp
void log(LogLevel level, const char* file, int line, const char* func, const char* fmt, ...) {
    if (level < m_level) return;   // 级别过滤

    std::lock_guard<std::mutex> lock(m_mtx);  // 线程安全

    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    output(level, file, line, func, msg);
}
```
- `va_list` / `va_start` / `vsnprintf`：C 风格可变参数处理（与宏 `__VA_ARGS__` 配合）
- 缓冲区 1024 字节（对日志场景够用，超长会被截断——`vsnprintf` 不会溢出）

**彩色控制台输出 (行 78-97)**
```cpp
case LogLevel::ERROR: levelStr = "ERROR"; color = "\033[31m"; break;  // 红
```
- ANSI 转义序列：`\033[31m` = 红色前景，`\033[0m` = 重置
- 对 linuxfb 环境（无终端）无影响，PC 调试时直观

**syslog 集成 (行 100-118)**
```cpp
if (m_useSyslog) {
    static bool syslogOpened = false;
    if (!syslogOpened) {
        openlog("smartcam", LOG_PID | LOG_NDELAY, LOG_USER);
        syslogOpened = true;
    }
```
- 部署到开发板后，日志写入系统日志，可通过 `journalctl` 查看
- `static bool syslogOpened` 保证 `openlog()` 只调用一次
- `LOG_NDELAY`：不延迟连接（立即写入）

**宏冲突处理 (行 123-126)**
```cpp
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARNING
#undef LOG_ERR
```
- `<syslog.h>` 定义了 `LOG_DEBUG`、`LOG_ERR` 等宏（整数值 7、3 等），会和自定义的日志宏冲突
- 先 `#undef` 清除系统定义，再重新定义为我们自己的版本
- 自定义宏叫 `LOG_ERR_`（多一个下划线）避开 syslog 的 `LOG_ERR`（3）

---

### 2.3 config.h

```cpp
#ifndef SMART_CAM_COMMON_CONFIG_H
#define SMART_CAM_COMMON_CONFIG_H

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>

class ConfigManager {
public:
    ConfigManager() = default;

    /**
     * @brief 从文件加载配置
     * @return true 加载成功，false 文件不存在或读取失败
     */
    bool load(const std::string& path) {
        m_path = path;
        m_data.clear();

        std::ifstream file(path);
        if (!file.is_open()) {
            return false;  // 文件不存在不是致命错误
        }

        std::string line;
        std::string currentSection;

        while (std::getline(file, line)) {
            // 去除行首尾空白
            line = trim(line);

            // 跳过空行和注释
            if (line.empty() || line[0] == '#' || line[0] == ';') {
                continue;
            }

            // 检测 section: [camera]
            if (line[0] == '[' && line.back() == ']') {
                currentSection = line.substr(1, line.size() - 2);
                currentSection = trim(currentSection);
                continue;
            }

            // 解析 key = value
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key   = trim(line.substr(0, eq));
            std::string value = trim(line.substr(eq + 1));

            // 去掉行尾注释（# 之后的内容，但不是值的一部分）
            size_t hash = value.find('#');
            if (hash != std::string::npos && hash > 0 && value[hash - 1] == ' ') {
                value = trim(value.substr(0, hash));
            }

            if (!key.empty() && !currentSection.empty()) {
                m_data[currentSection][key] = value;
            }
        }

        return true;
    }

    /** @brief 获取配置文件路径 */
    const std::string& path() const { return m_path; }
    bool hasSection(const std::string& section) const {
        return m_data.find(section) != m_data.end();
    }

    // ============================================================
    // 类型安全取值
    // ============================================================

    std::string getString(const std::string& section,
                          const std::string& key,
                          const std::string& defaultValue = "") const {
        auto secIt = m_data.find(section);
        if (secIt == m_data.end()) return defaultValue;

        auto keyIt = secIt->second.find(key);
        if (keyIt == secIt->second.end()) return defaultValue;

        return keyIt->second;
    }

    int getInt(const std::string& section,
               const std::string& key,
               int defaultValue = 0) const {
        std::string val = getString(section, key);
        if (val.empty()) return defaultValue;
        return std::atoi(val.c_str());
    }

    bool getBool(const std::string& section,
                 const std::string& key,
                 bool defaultValue = false) const {
        std::string val = getString(section, key);
        if (val.empty()) return defaultValue;

        std::transform(val.begin(), val.end(), val.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        return (val == "true" || val == "yes" || val == "1" || val == "on");
    }

    bool hasKey(const std::string& section, const std::string& key) const {
        auto secIt = m_data.find(section);
        if (secIt == m_data.end()) return false;
        return secIt->second.find(key) != secIt->second.end();
    }

    void setString(const std::string& section,
                   const std::string& key,
                   const std::string& value) {
        m_data[section][key] = value;
    }

    bool save() const {
        if (m_path.empty()) return false;

        std::ofstream file(m_path);
        if (!file.is_open()) return false;

        for (const auto& sec : m_data) {
            file << "[" << sec.first << "]\n";
            for (const auto& kv : sec.second) {
                file << kv.first << " = " << kv.second << "\n";
            }
            file << "\n";
        }

        return true;
    }

    bool saveAs(const std::string& path) const {
        // 确保父目录存在
        size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0) {
            mkdirParents(path.substr(0, slash));
        }

        std::ofstream file(path);
        if (!file.is_open()) return false;

        for (const auto& sec : m_data) {
            file << "[" << sec.first << "]\n";
            for (const auto& kv : sec.second) {
                file << kv.first << " = " << kv.second << "\n";
            }
            file << "\n";
        }

        return true;
    }

private:
    static std::string trim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    /** @brief 递归创建目录 (类似 mkdir -p) */
    static void mkdirParents(const std::string& path) {
        if (path.empty() || path == "/") return;
        std::string parent = path;
        size_t slash = parent.rfind('/');
        if (slash != std::string::npos && slash > 0) {
            mkdirParents(parent.substr(0, slash));
        }
        mkdir(path.c_str(), 0755);  // 忽略 EEXIST 错误
    }

    std::string m_path;
    std::map<std::string, std::map<std::string, std::string>> m_data;
};

#endif // SMART_CAM_COMMON_CONFIG_H
```

#### 逐段精读

**存储结构 (末尾)**
```cpp
std::map<std::string, std::map<std::string, std::string>> m_data;
//   section名           key名       value字符串
// 例如: m_data["camera"]["device"] = "/dev/video0"
```
- 双层 `std::map` 完美映射 INI 文件的 `[section]` → `key=value` 结构
- 全部存字符串，不预设类型。`getInt()` / `getBool()` 在取值时转换

**`load()` 解析逻辑 (行 22-84)**
```cpp
while (std::getline(file, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;  // 跳过注释
    if (line[0] == '[' && line.back() == ']') {                       // section
        currentSection = line.substr(1, line.size() - 2);
    }
    size_t eq = line.find('=');                                        // key=value
    if (eq == std::string::npos) continue;
    std::string key = trim(line.substr(0, eq));
    std::string value = trim(line.substr(eq + 1));
    m_data[currentSection][key] = value;
}
```
- 状态机解析：维护 `currentSection`，遇到 `[...]` 切换，遇到 `key=value` 存入
- 支持行尾注释（`value # this is a comment`），但前提是 `#` 前有空格——避免误删值中本身包含 `#` 的情况

**`getBool()` 的宽松解析**
```cpp
return (val == "true" || val == "yes" || val == "1" || val == "on");
```
- 接受四种真值写法，大小写不敏感（已预处理 `tolower`）

**`saveAs()` 的目录创建**
```cpp
bool saveAs(const std::string& path) const {
    size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        mkdirParents(path.substr(0, slash));  // mkdir -p
    }
```
- 写 `~/.config/smartcam/smartcam.conf` 时，`~/.config/smartcam` 目录可能不存在，先递归创建
- `mkdirParents` 递归调用自己创建父目录链

---

### 2.4 ringbuf.h

```cpp
#ifndef SMART_CAM_COMMON_RINGBUF_H
#define SMART_CAM_COMMON_RINGBUF_H

#include <cstddef>
#include <mutex>
#include <cstring>
#include <cstdint>

template<typename T>
class RingBuffer {
public:
    explicit RingBuffer(int capacity)
        : m_capacity(capacity), m_head(0), m_tail(0), m_size(0)
    {
        m_buffer = new T[static_cast<size_t>(capacity)];
    }

    ~RingBuffer() {
        delete[] m_buffer;
        m_buffer = nullptr;
    }

    // 禁用拷贝
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // 允许移动
    RingBuffer(RingBuffer&& other) noexcept
        : m_buffer(other.m_buffer), m_capacity(other.m_capacity),
          m_head(other.m_head), m_tail(other.m_tail), m_size(other.m_size)
    {
        other.m_buffer = nullptr;
        other.m_capacity = 0;
        other.m_head = other.m_tail = other.m_size = 0;
    }

    /**
     * @brief 入队（队列满时返回 false）
     */
    bool push(const T& item) {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_size >= m_capacity) return false;
        m_buffer[m_tail] = item;
        m_tail = (m_tail + 1) % m_capacity;   // 写指针模运算前进
        m_size++;
        return true;
    }

    /**
     * @brief 出队（队列空时返回 false）
     */
    bool pop(T& item) {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_size <= 0) return false;
        item = m_buffer[m_head];
        m_head = (m_head + 1) % m_capacity;   // 读指针模运算前进
        m_size--;
        return true;
    }

    /**
     * @brief 入队，若满则覆盖最旧数据
     */
    bool pushOverwrite(const T& item) {
        bool overwritten = false;
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_size >= m_capacity) {
            m_buffer[m_tail] = item;
            m_tail = (m_tail + 1) % m_capacity;
            m_head = m_tail;     // 读写指针重合 → 假满
            overwritten = true;
        } else {
            m_buffer[m_tail] = item;
            m_tail = (m_tail + 1) % m_capacity;
            m_size++;
        }
        return overwritten;
    }

    bool peek(T& item) const {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_size <= 0) return false;
        item = m_buffer[m_head];
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_head = m_tail = m_size = 0;
    }

    int size() const {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_size;
    }
    int capacity() const { return m_capacity; }
    bool empty() const {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_size <= 0;
    }
    bool full() const {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_size >= m_capacity;
    }

private:
    T*           m_buffer;
    int          m_capacity;
    int          m_head;     // 读指针
    int          m_tail;     // 写指针
    int          m_size;     // 当前元素数
    mutable std::mutex m_mtx;
};

#endif // SMART_CAM_COMMON_RINGBUF_H
```

#### 逐段精读

**环形缓冲区核心算法 (行 55-61)**
```cpp
bool push(const T& item) {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_size >= m_capacity) return false;   // 满则拒绝
    m_buffer[m_tail] = item;
    m_tail = (m_tail + 1) % m_capacity;        // 模运算保证循环
    m_size++;
    return true;
}
```
- **经典三指针环形队列**：`head`(读) / `tail`(写) / `size`(计数)
- `m_tail = (m_tail + 1) % m_capacity` 是环形缓冲区最关键的模运算——tail 到达数组末尾后回绕到索引 0
- 用 `m_size` 而非 `head==tail` 来区分空/满状态（空：`size==0`，满：`size==capacity`）

**pushOverwrite 的覆盖语义**
```cpp
bool pushOverwrite(const T& item) {
    if (m_size >= m_capacity) {
        m_buffer[m_tail] = item;
        m_tail = (m_tail + 1) % m_capacity;
        m_head = m_tail;   // 读写指针重合！
    }
```
- 当缓冲区满时，覆盖最旧的数据：`m_head = m_tail` 使读写指针重合，`pop` 会从 `head` 读最新数据
- 适合**只关注最新帧**的场景（丢掉旧帧不怕）

**移动构造 (行 27-35)**
```cpp
RingBuffer(RingBuffer&& other) noexcept
    : m_buffer(other.m_buffer), ... {
    other.m_buffer = nullptr;
}
```
- 支持移动语义（C++11 右值引用），允许 `RingBuffer buf = std::move(other)` 高效转移所有权
- 禁止拷贝（`delete`），避免意外复制数组内存

---

## 3. 摄像头模块

### 3.1 capture.h

```cpp
#ifndef SMART_CAM_CAMERA_CAPTURE_H
#define SMART_CAM_CAMERA_CAPTURE_H

#include <cstdint>
#include <vector>
#include <string>
#include <mutex>
#include "include/common/types.h"

// V4L2 前向声明（避免引入 linux/videodev2.h 冲突）
struct v4l2_capability;
struct v4l2_format;
struct v4l2_buffer;
struct v4l2_queryctrl;

class CameraCapture {
public:
    CameraCapture();
    ~CameraCapture();

    CameraCapture(const CameraCapture&) = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;

    // ============================================================
    // 生命周期
    // ============================================================
    int init(const char* device = "/dev/video0");
    void release();

    // ============================================================
    // 设备查询
    // ============================================================
    std::string getDriverInfo() const;
    int enumFormats(std::vector<uint32_t>& formats);
    int enumFrameSizes(uint32_t pixfmt, std::vector<std::pair<int,int>>& resolutions);

    // ============================================================
    // 格式 & 参数设置
    // ============================================================
    int setFormat(int width, int height, uint32_t pixfmt);
    int setFramerate(int numerator, int denominator);
    int getFramerate(int& numerator, int& denominator);
    int enumFrameRates(uint32_t pixfmt, int width, int height,
                       std::vector<int>& frameRates);
    int setControl(int cid, int value);
    int getControl(int cid, int& value);
    int queryControl(int cid, int& min, int& max, int& step, int& def);

    // ============================================================
    // 采集控制
    // ============================================================
    int startCapture();
    int stopCapture();
    int getFrame(FrameBuffer* buf, int timeout_ms = -1);
    int putFrame(const FrameBuffer* buf);

    // ============================================================
    // 状态查询
    // ============================================================
    bool isStreaming() const { return m_streaming; }
    double getCurrentFPS() const;
    Resolution getCurrentResolution() const;
    uint32_t getCurrentFormat() const { return m_pixfmt; }

    // ============================================================
    // 常量（避免 #include <linux/videodev2.h> 与 Qt MOC 冲突）
    // ============================================================
    static constexpr int kDefaultBufferCount = 4;

    static constexpr uint32_t V4L2_PIX_FMT_YUYV  = 0x56595559;
    static constexpr uint32_t V4L2_PIX_FMT_MJPEG = 0x47504A4D;
    static constexpr uint32_t V4L2_PIX_FMT_RGB24 = 0x00000001;

    static constexpr uint32_t V4L2_CID_BRIGHTNESS               = 0x00980900;
    static constexpr uint32_t V4L2_CID_CONTRAST                 = 0x00980901;
    static constexpr uint32_t V4L2_CID_AUTO_WHITE_BALANCE       = 0x0098090C;
    static constexpr uint32_t V4L2_CID_WHITE_BALANCE_TEMPERATURE = 0x0098090A;
    static constexpr uint32_t V4L2_CID_EXPOSURE_AUTO            = 0x009a0901;
    static constexpr uint32_t V4L2_CID_EXPOSURE_ABSOLUTE        = 0x009a0902;

private:
    struct BufferUnit {
        void*  start;       // mmap 映射地址
        size_t length;      // 缓冲区长度
        int    index;       // 缓冲区索引
        bool   queued;      // 是否在 V4L2 队列中
        FrameBuffer toFrameBuffer(int w, int h, uint32_t fmt);
    };

    int openDevice(const char* device);
    int queryCapability();
    int requestBuffers(int count);
    int mapBuffers();
    int unmapBuffers();
    int queueAllBuffers();
    int dequeueBuffer(v4l2_buffer& buf, int timeout_ms);
    void updateFPS();

    int  m_fd              = -1;
    bool m_streaming       = false;
    int  m_width           = 640;
    int  m_height          = 480;
    uint32_t m_pixfmt      = V4L2_PIX_FMT_MJPEG;

    BufferUnit* m_buffers  = nullptr;
    int         m_nbuffers = 0;

    mutable std::mutex m_fpsMtx;
    int  m_frameCount      = 0;
    double m_lastFpsTime   = 0.0;
    double m_currentFps    = 30.0;

    mutable std::mutex m_mtx;
};

#endif // SMART_CAM_CAMERA_CAPTURE_H
```

#### 逐段精读

**为什么用前向声明而非 `#include <linux/videodev2.h>`？(行 11-14)**
```cpp
struct v4l2_capability;
struct v4l2_format;
struct v4l2_buffer;
struct v4l2_queryctrl;
```
- 关键！`<linux/videodev2.h>` 定义了 `V4L2_PIX_FMT_YUYV` 等宏，可能和 Qt 头文件产生命名冲突
- 因为 Qt 的 MOC 编译器需要解析所有 `#include`——如果 `.h` 中包含了内核头文件，MOC 处理时可能报错
- 做法：**头文件只前向声明，实现文件 `.cpp` 中才 `#include <linux/videodev2.h>`**

**V4L2 常量硬编码 (行 80-92)**
```cpp
static constexpr uint32_t V4L2_PIX_FMT_YUYV  = 0x56595559;
static constexpr uint32_t V4L2_CID_BRIGHTNESS = 0x00980900;
```
- 这些值来自内核头文件：`V4L2_CID_BRIGHTNESS` = `(V4L2_CID_BASE+0)`, `V4L2_CID_BASE` = `0x00980900`
- 硬编码在类中避免了在头文件 `#include` 内核头文件的需求
- `static constexpr` 在编译期求值，零运行时开销

**BufferUnit 结构 (行 96-102)**
```cpp
struct BufferUnit {
    void*  start;     // mmap 返回的虚拟地址
    size_t length;    // V4L2 驱动分配的缓冲区大小
    int    index;     // 0..3
    bool   queued;    // 当前是否在驱动的输入队列中
};
```
- `start` 直接映射到内核 DMA 缓冲区，硬件把图像写入这里
- `queued` 标志用于 putFrame 时反查索引（从 `data` 指针找到对应 `index`）

**核心 API 设计**
```cpp
int getFrame(FrameBuffer* buf, int timeout_ms = -1);
int putFrame(const FrameBuffer* buf);
```
- `getFrame` / `putFrame` 是**配对调用**的——类似于 RAII 的 acquire/release
- `timeout_ms` 默认 -1 为无限阻塞，正数为毫秒超时（底层用 `select()` 实现）

---

### 3.2 capture.cpp

```cpp
/**
 * @file    capture.cpp
 * @brief   V4L2 视频采集引擎实现
 *
 * 完整 V4L2 采集流程:
 *   open → querycap → s_fmt → reqbufs → querybuf → mmap → qbuf
 *   → streamon → [dqbuf → process → qbuf] ... → streamoff
 *
 * 支持 YUYV / MJPEG 双格式，mmap 零拷贝帧缓冲池。
 */

#include "include/camera/capture.h"
#include "include/common/logger.h"

#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <algorithm>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <fcntl.h>

// V4L2 相关头文件（仅在 .cpp 中包含，避免与 Qt MOC 冲突）
#include <linux/videodev2.h>

// ============================================================
// 构造 / 析构
// ============================================================

CameraCapture::CameraCapture()
    : m_fd(-1)
    , m_streaming(false)
    , m_width(640)
    , m_height(480)
    , m_pixfmt(V4L2_PIX_FMT_MJPEG)
    , m_buffers(nullptr)
    , m_nbuffers(0)
    , m_frameCount(0)
    , m_lastFpsTime(0.0)
    , m_currentFps(30.0)
{
}

CameraCapture::~CameraCapture() {
    release();
}
```

```cpp
// ============================================================
// 初始化 / 释放
// ============================================================

int CameraCapture::init(const char* device) {
    if (m_fd >= 0) {
        LOG_WRN("Camera already initialized, releasing first");
        release();
    }

    int ret = openDevice(device);
    if (ret < 0) return ret;

    ret = queryCapability();
    if (ret < 0) {
        close(m_fd);
        m_fd = -1;
        return ret;
    }

    LOG_INF("Camera initialized: fd=%d, device=%s", m_fd, device);
    return 0;
}

void CameraCapture::release() {
    if (m_streaming) {
        stopCapture();     // STREAMOFF + 释放 mmap
    }
    unmapBuffers();
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
        LOG_INF("Camera device closed");
    }
}
```

#### 逐段详解

**`init()` 的初始化流程 (行 54-72)**

这是打开 V4L2 设备的入口函数，做了两件事：

1. **防御性检查**：`if (m_fd >= 0)` — 如果之前已经 open 过（比如热重启摄像头），先 `release()` 清理
2. **两阶段初始化**：
   - `openDevice(device)` → `open()` 系统调用获取文件描述符
   - `queryCapability()` → `VIDIOC_QUERYCAP` ioctl 查询设备能力

```cpp
ret = queryCapability();
if (ret < 0) {
    close(m_fd);      // 出错则回滚：关闭已打开的 fd
    m_fd = -1;
}
```
- 错误处理采用**手动回滚**模式：每步失败都撤销之前成功的操作

```cpp
// ============================================================
// 设备查询
// ============================================================

int CameraCapture::enumFormats(std::vector<uint32_t>& formats) {
    if (m_fd < 0) return -ENODEV;

    formats.clear();
    struct v4l2_fmtdesc fmtdesc;
    memset(&fmtdesc, 0, sizeof(fmtdesc));
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for (int i = 0;; ++i) {
        fmtdesc.index = i;
        if (ioctl(m_fd, VIDIOC_ENUM_FMT, &fmtdesc) < 0) {
            break;  // 枚举结束 — 驱动返回 EINVAL/ENOENT
        }
        formats.push_back(fmtdesc.pixelformat);
        LOG_DBG("  Format[%d]: '%c%c%c%c' = %s",
                  i,
                  (fmtdesc.pixelformat >> 0) & 0xFF,
                  (fmtdesc.pixelformat >> 8) & 0xFF,
                  (fmtdesc.pixelformat >> 16) & 0xFF,
                  (fmtdesc.pixelformat >> 24) & 0xFF,
                  reinterpret_cast<char*>(fmtdesc.description));
    }

    return formats.empty() ? -ENOENT : 0;
}
```

**枚举像素格式的 V4L2 协议细节 (行 112-136)**

```cpp
for (int i = 0;; ++i) {
    fmtdesc.index = i;
    if (ioctl(m_fd, VIDIOC_ENUM_FMT, &fmtdesc) < 0) {
        break;  // 驱动返回错误表示枚举结束
    }
    formats.push_back(fmtdesc.pixelformat);
```
- **V4L2 枚举范式**：从 `index=0` 开始递增循环调用 `VIDIOC_ENUM_FMT`，直到 `ioctl` 返回 <0（通常是 `EINVAL` 表示没有更多格式）
- 驱动每次调用填充 `fmtdesc.pixelformat`（四字符码如 `MJPG`）和 `fmtdesc.description`（人类可读名如 `"MJPEG"`）

**FOURCC 打印技巧**：
```cpp
(fmtdesc.pixelformat >> 0) & 0xFF,   // 提取第0字节 → 打印为字符
(fmtdesc.pixelformat >> 8) & 0xFF,   // 提取第1字节
```
- 把 `0x47504A4D` 拆成四个字节打印，输出 `'M','J','P','G'`

```cpp
// ============================================================
// 格式 & 参数设置
// ============================================================

int CameraCapture::setFormat(int width, int height, uint32_t pixfmt) {
    if (m_fd < 0) return -ENODEV;
    if (m_streaming) {
        LOG_WRN("setFormat called while streaming, stop first");
        return -EBUSY;    // 流传输中不能改格式
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = static_cast<__u32>(width);
    fmt.fmt.pix.height      = static_cast<__u32>(height);
    fmt.fmt.pix.pixelformat = pixfmt;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;

    if (ioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERR_("VIDIOC_S_FMT failed: %s (w=%d h=%d fmt=0x%08X)",
                  strerror(errno), width, height, pixfmt);
        return -errno;
    }

    // ★ 读取实际设置的值（驱动可能调整了分辨率）
    m_width  = static_cast<int>(fmt.fmt.pix.width);
    m_height = static_cast<int>(fmt.fmt.pix.height);
    m_pixfmt = fmt.fmt.pix.pixelformat;

    // bytesperline 用于检查 stride，确认无 padding 问题
    LOG_INF("Format set: %dx%d, fmt='%c%c%c%c', stride=%d",
             m_width, m_height,
             (m_pixfmt >> 0) & 0xFF, (m_pixfmt >> 8) & 0xFF,
             (m_pixfmt >> 16) & 0xFF, (m_pixfmt >> 24) & 0xFF,
             fmt.fmt.pix.bytesperline);

    return 0;
}
```

**`setFormat()` — 设置图像格式的核心 (行 176-210)**

1. **前置条件检查**：`if (m_streaming) return -EBUSY` — V4L2 规范要求必须在 `STREAMON` 之前设置格式，流传输中改格式会返回 `EBUSY`

2. **填充 `v4l2_format` 结构**：
   ```cpp
   fmt.fmt.pix.field = V4L2_FIELD_ANY;   // 不指定场序，让驱动选择
   ```
   - `V4L2_FIELD_ANY` 告诉驱动："随便选你支持的场序"（对 USB 摄像头通常是 `V4L2_FIELD_NONE`）

3. **关键点 —— 驱动可能调整参数**：
   ```cpp
   // 读取实际设置的值（驱动可能调整了分辨率）
   m_width  = static_cast<int>(fmt.fmt.pix.width);
   m_height = static_cast<int>(fmt.fmt.pix.height);
   ```
   - `VIDIOC_S_FMT` 调用后，驱动可能**修改**请求的分辨率/格式为它实际支持的最接近值
   - 必须**写回**驱动的返回值，不能用自己请求的值

```cpp
int CameraCapture::setFramerate(int numerator, int denominator) {
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = static_cast<__u32>(numerator);
    parm.parm.capture.timeperframe.denominator = static_cast<__u32>(denominator);
    // V4L2 的帧率用 "每帧间隔" (timeperframe) 表示，而非 fps
    // fps = denominator / numerator，例如 30fps = 1/30 → numerator=1, denominator=30

    if (ioctl(m_fd, VIDIOC_S_PARM, &parm) < 0) {
        LOG_WRN("VIDIOC_S_PARM (fps) not supported: %s", strerror(errno));
        return -errno;
    }

    int actFps = (parm.parm.capture.timeperframe.numerator > 0)
                     ? (denominator / numerator)
                     : 0;
    if (actFps != denominator / numerator) {
        LOG_WRN("requested %d fps → driver adjusted to %d fps", ...);
    }
    return 0;
}
```

**帧率的 V4L2 表示方式 (行 212-240)**

V4L2 用 `timeperframe`（每帧时间间隔）表示帧率，而非直接使用 fps 值：
- `numerator=1, denominator=30` 表示每帧 1/30 秒 = 30fps
- `numerator=1, denominator=15` 表示每帧 1/15 秒 = 15fps

同样，`VIDIOC_S_PARM` 后驱动可能调整值，需要读取返回结构体。

```cpp
// ============================================================
// 采集控制
// ============================================================

int CameraCapture::startCapture() {
    if (m_fd < 0) return -ENODEV;
    if (m_streaming) {
        LOG_WRN("Already streaming");
        return 0;
    }

    // 步骤 1: REQBUFS
    int ret = requestBuffers(kDefaultBufferCount);
    if (ret < 0) return ret;

    // 步骤 2: QUERYBUF + mmap
    ret = mapBuffers();
    if (ret < 0) return ret;

    // 步骤 3: QBUF（全部入队）
    ret = queueAllBuffers();
    if (ret < 0) {
        unmapBuffers();
        return ret;
    }

    // 步骤 4: STREAMON
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(m_fd, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERR_("VIDIOC_STREAMON failed: %s", strerror(errno));
        unmapBuffers();
        return -errno;
    }

    m_streaming = true;
    m_frameCount = 0;
    m_lastFpsTime = 0.0;

    LOG_INF("Capture started: %dx%d, %d buffers",
             m_width, m_height, m_nbuffers);
    return 0;
}
```

**`startCapture()` — V4L2 采集启动流水线 (行 400-437)**

这是整个 V4L2 协议的核心流程，分 4 个步骤：

| 步骤 | ioctl | 作用 |
|------|-------|------|
| 1 | `VIDIOC_REQBUFS` | 请求内核分配 N 个 DMA 缓冲区 |
| 2 | `VIDIOC_QUERYBUF` + `mmap()` | 查询每个缓冲区的物理偏移，mmap 到用户空间虚拟地址 |
| 3 | `VIDIOC_QBUF` × N | 将所有空缓冲区放入驱动的**输入队列**（驱动往里填数据） |
| 4 | `VIDIOC_STREAMON` | 启动硬件采集，驱动开始填充输入队列中的缓冲区 |

每一步失败都做回滚（释放已分配资源）。

```cpp
int CameraCapture::getFrame(FrameBuffer* buf, int timeout_ms) {
    if (!m_streaming || m_fd < 0) return -EIO;

    struct v4l2_buffer vbuf;
    memset(&vbuf, 0, sizeof(vbuf));

    int ret = dequeueBuffer(vbuf, timeout_ms);   // DQBUF
    if (ret < 0) return ret;

    // 校验缓冲区索引有效性
    if (vbuf.index >= static_cast<__u32>(m_nbuffers) || !m_buffers) {
        LOG_ERR_("Invalid buffer index: %u (nbufs=%d)", vbuf.index, m_nbuffers);
        return -EINVAL;
    }

    // ★ 填充 FrameBuffer — 零拷贝！
    buf->data     = static_cast<uint8_t*>(m_buffers[vbuf.index].start);
    buf->length   = static_cast<int>(vbuf.bytesused);
    buf->width    = m_width;
    buf->height   = m_height;
    buf->format   = (m_pixfmt == V4L2_PIX_FMT_YUYV)
                        ? PixelFormat::FMT_YUYV
                        : PixelFormat::FMT_MJPEG;
    buf->index    = m_frameCount++;
    buf->timestamp = std::chrono::steady_clock::now();

    updateFPS();
    return 0;
}
```

**`getFrame()` — 零拷贝帧获取 (行 454-484)**

```cpp
buf->data = static_cast<uint8_t*>(m_buffers[vbuf.index].start);
```
- **零拷贝核心**：`buf->data` 直接指向 mmap 映射的内核缓冲区虚拟地址，硬件 DMA 已经把图像数据写入这里
- **不需要 `memcpy`** — 采集线程直接读取这块内存
- `vbuf.bytesused` 是驱动填充的实际数据大小（MJPEG 模式下每帧大小可变）

**⚠️ 重要约束**：`getFrame` 返回后，调用者**必须尽快**调用 `putFrame()` 归还缓冲区，否则 V4L2 驱动会耗尽输入队列导致丢帧。

```cpp
int CameraCapture::putFrame(const FrameBuffer* buf) {
    // 从 data 指针反推缓冲区索引
    int idx = -1;
    for (int i = 0; i < m_nbuffers; ++i) {
        if (m_buffers[i].start == buf->data) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        LOG_ERR_("putFrame: buffer pointer not found in pool");
        return -EINVAL;
    }

    struct v4l2_buffer vbuf;
    memset(&vbuf, 0, sizeof(vbuf));
    vbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vbuf.memory = V4L2_MEMORY_MMAP;
    vbuf.index  = static_cast<__u32>(idx);

    if (ioctl(m_fd, VIDIOC_QBUF, &vbuf) < 0) {  // 归还到驱动输入队列
        LOG_ERR_("VIDIOC_QBUF[%d] failed: %s", idx, strerror(errno));
        return -errno;
    }

    m_buffers[idx].queued = true;
    return 0;
}
```

**`putFrame()` — 通过指针反查索引 (行 486-516)**

巧妙的设计：
```cpp
for (int i = 0; i < m_nbuffers; ++i) {
    if (m_buffers[i].start == buf->data) { idx = i; break; }
}
```
- 调用者传入的 `buf`（由 `getFrame` 返回）并没有携带索引——索引由 mmap 地址反推
- 比较的是虚拟地址 (`==`)，因为 `mmap()` 返回的地址是唯一的

```cpp
// ============================================================
// select + DQBUF 实现带超时的帧等待
// ============================================================

int CameraCapture::dequeueBuffer(struct v4l2_buffer& buf, int timeout_ms) {
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    // 使用 select 实现超时
    if (timeout_ms > 0) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_fd, &fds);

        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int ret = select(m_fd + 1, &fds, nullptr, nullptr, &tv);
        if (ret < 0) {
            return -errno;
        }
        if (ret == 0) {
            return -ETIMEDOUT;   // 超时——没有帧可用
        }
    }

    if (ioctl(m_fd, VIDIOC_DQBUF, &buf) < 0) {
        LOG_ERR_("VIDIOC_DQBUF failed: %s", strerror(errno));
        return -errno;
    }

    if (buf.index < static_cast<__u32>(m_nbuffers)) {
        m_buffers[buf.index].queued = false;
    }

    return 0;
}
```

**`dequeueBuffer()` — select + DQBUF 组合 (行 691-724)**

```cpp
int ret = select(m_fd + 1, &fds, nullptr, nullptr, &tv);
```
- V4L2 设备 fd 是一个**可读的**文件描述符——当驱动有帧准备好的时候，fd 变成可读
- `select` 等待 fd 变为可读（有帧），或者超时
- 这样 `VIDIOC_DQBUF` 几乎不会阻塞，因为 select 已经保证了数据可用

```cpp
// ============================================================
// mmap 缓冲区映射
// ============================================================

int CameraCapture::mapBuffers() {
    for (int i = 0; i < m_nbuffers; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = static_cast<__u32>(i);

        if (ioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            LOG_ERR_("VIDIOC_QUERYBUF[%d] failed: %s", i, strerror(errno));
            return -errno;
        }

        m_buffers[i].start = mmap(nullptr,          // 让内核选虚拟地址
                                   buf.length,       // 缓冲区大小
                                   PROT_READ | PROT_WRITE,  // 可读写
                                   MAP_SHARED,       // 共享映射——CPU 和 DMA 都能访问
                                   m_fd,              // 设备文件描述符
                                   buf.m.offset);     // 缓冲区的物理偏移
        if (m_buffers[i].start == MAP_FAILED) {
            LOG_ERR_("mmap[%d] failed: %s", i, strerror(errno));
            return -errno;
        }

        m_buffers[i].length = static_cast<size_t>(buf.length);
    }
    return 0;
}
```

**mmap 零拷贝原理 (行 606-641)**

```cpp
m_buffers[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                           MAP_SHARED, m_fd, buf.m.offset);
```

关键参数：
- `MAP_SHARED`：CPU 和 DMA 硬件共享同一块物理内存。USB 摄像头的 DMA 控制器把图像数据写入这块物理内存，CPU 直接通过 `mmap` 返回的虚拟地址读取，**中间没有任何拷贝**
- `buf.m.offset`：驱动分配的各缓冲区物理偏移量，`mmap` 通过 fd + offset 定位到特定的物理页面
- `PROT_READ | PROT_WRITE`：允许读（采集端读帧数据）和写（DMA 写入）

**对比**：如果不用 mmap 而用 `VIDIOC_DQBUF` + `read()` 方式，每次取帧要多一次内核→用户空间的拷贝。mmap 模式下 640×480 MJPEG 帧（约 80KB）的延迟可以降低 1-2ms。

```cpp
int CameraCapture::unmapBuffers() {
    if (!m_buffers) return 0;

    for (int i = 0; i < m_nbuffers; ++i) {
        if (m_buffers[i].start && m_buffers[i].start != MAP_FAILED) {
            munmap(m_buffers[i].start, m_buffers[i].length);
        }
    }

    delete[] m_buffers;
    m_buffers  = nullptr;
    m_nbuffers = 0;

    // ★ 重要：释放 V4L2 驱动侧缓冲区资源
    if (m_fd >= 0) {
        struct v4l2_requestbuffers req;
        memset(&req, 0, sizeof(req));
        req.count  = 0;                // count=0 释放所有内核缓冲区
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (ioctl(m_fd, VIDIOC_REQBUFS, &req) < 0) {
            LOG_WRN("VIDIOC_REQBUFS(0) failed: %s", strerror(errno));
        }
    }

    return 0;
}
```

**`unmapBuffers()` — 关键清理逻辑 (行 643-671)**

```cpp
req.count = 0;   // VIDIOC_REQBUFS with count=0 释放内核 DMA 缓冲区
```

这是容易被忽略但非常重要的步骤：
- `munmap` 只是解除用户空间的映射，**不会**释放内核的 DMA 缓冲区
- 必须用 `VIDIOC_REQBUFS` + `count=0` **显式释放**内核资源
- 如果不做这一步，后续调用 `VIDIOC_S_FMT` 会返回 `EBUSY`（驱动认为还有活跃缓冲区）

```cpp
// ============================================================
// FPS 统计
// ============================================================

void CameraCapture::updateFPS() {
    std::lock_guard<std::mutex> lock(m_fpsMtx);

    double now = std::chrono::duration<double>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count();

    if (m_lastFpsTime == 0.0) {
        m_lastFpsTime = now;
        return;
    }

    // 每 30 帧计算一次平均 FPS
    if (m_frameCount % 30 == 0) {
        double elapsed = now - m_lastFpsTime;
        if (elapsed > 0.0) {
            m_currentFps = 30.0 / elapsed;   // FPS = 帧数 / 时间差
        }
        m_lastFpsTime = now;
    }
}
```

**`updateFPS()` — 滑动窗口 FPS 统计 (行 730-750)**

```cpp
if (m_frameCount % 30 == 0) {
    m_currentFps = 30.0 / elapsed;
}
```
- 每 30 帧计算一次平均 FPS，避免每帧计算导致的抖动
- 计算公式：FPS = 帧数 / 时间差 = 30 / (now - m_lastFpsTime)
- 用 `steady_clock` 而非 `system_clock`，不受 NTP 时间调整影响

---

### 3.3 processor.h

```cpp
#ifndef SMART_CAM_CAMERA_PROCESSOR_H
#define SMART_CAM_CAMERA_PROCESSOR_H

#include <cstdint>
#include <vector>

/**
 * @brief 视频处理工具类（纯静态方法）
 */
class VideoProcessor {
public:
    // ============================================================
    // MJPEG 帧解析
    // ============================================================
    static int findJPEGFrame(const uint8_t* data, int len,
                             int* jpeg_start, int* jpeg_len);
    static bool isJPEGStart(const uint8_t* data, int len);

    // ============================================================
    // 颜色空间转换（BT.601 定点运算）
    // ============================================================
    static void yuyvToRgb24(const uint8_t* yuyv, uint8_t* rgb, int w, int h);
#ifdef __ARM_NEON
    static void yuyvToRgb24Neon(const uint8_t* yuyv, uint8_t* rgb, int w, int h);
#endif
    static void yuyvToRgb565(const uint8_t* yuyv, uint8_t* rgb565, int w, int h);
    static void yuyvMacroPixelToRgb24(const uint8_t yuyv[4], uint8_t rgb[6]);

    // ============================================================
    // JPEG 编码（使用 libjpeg-turbo）
    // ============================================================
    static int encodeRGBtoJPEG(const uint8_t* rgb, int width, int height,
                               int quality, uint8_t** jpeg_out,
                               unsigned long* jpeg_len);
    static int encodeYUYVtoJPEG(const uint8_t* yuyv, int width, int height,
                                int quality, uint8_t** jpeg_out,
                                unsigned long* jpeg_len);

    static inline int rgb24BufferSize(int w, int h) { return w * h * 3; }
    static inline int rgb565BufferSize(int w, int h) { return w * h * 2; }
    static inline int yuyvBufferSize(int w, int h) {
        return ((w + 1) & ~1) * h * 2;  // YUYV 要求偶数宽度，向上对齐
    }

private:
    VideoProcessor() = delete;  // 纯静态类，禁止实例化
};

#endif // SMART_CAM_CAMERA_PROCESSOR_H
```

#### 逐段精读

**纯静态工具类设计**
```cpp
class VideoProcessor {
private:
    VideoProcessor() = delete;   // 禁止实例化
};
```
- 所有方法都是 `static`，构造函数 `= delete`，这是 C++ 中实现"纯工具类"（类似 Java 的 utility class）的标准写法
- 无状态，无锁，天然线程安全

**YUYV 缓冲区对齐**
```cpp
static inline int yuyvBufferSize(int w, int h) {
    return ((w + 1) & ~1) * h * 2;
}
```
- YUYV 4:2:2 格式每对像素（宏像素 = Y0+U+Y1+V = 4字节）共享一对 UV 分量 → 宽度必须为偶数
- `(w + 1) & ~1` 将奇数宽度向上对齐到偶数

---

### 3.4 processor.cpp

```cpp
#include "include/camera/processor.h"
#include "include/common/logger.h"

#include <cstring>
#include <cstdlib>

#ifdef HAS_LIBJPEG
#include <jpeglib.h>
#endif

// ============================================================
// MJPEG 帧解析
// ============================================================

bool VideoProcessor::isJPEGStart(const uint8_t* data, int len) {
    return (len >= 2 && data[0] == 0xFF && data[1] == 0xD8);
}

int VideoProcessor::findJPEGFrame(const uint8_t* data, int len,
                                   int* jpeg_start, int* jpeg_len) {
    if (!data || len < 2 || !jpeg_start || !jpeg_len) {
        return -1;
    }

    // 查找 SOI 标记 (0xFF 0xD8)
    int start = -1;
    for (int i = 0; i < len - 1; ++i) {
        if (data[i] == 0xFF && data[i + 1] == 0xD8) {
            start = i;
            break;
        }
    }
    if (start < 0) return -1;

    // 从 SOI 向后查找 EOI 标记 (0xFF 0xD9)
    int end = -1;
    for (int i = len - 1; i > start + 1; --i) {
        if (data[i - 1] == 0xFF && data[i] == 0xD9) {
            end = i;
            break;
        }
    }
    if (end < 0) return -1;

    *jpeg_start = start;
    *jpeg_len   = end - start + 1;
    return 0;
}
```

**JPEG 帧边界检测原理**

JPEG 文件格式的核心标记：
- **SOI** (Start of Image): `0xFF 0xD8` — 帧开始
- **EOI** (End of Image): `0xFF 0xD9` — 帧结束
- MJPEG 是连续多张 JPEG 图片的流，需要在 V4L2 原始数据中查找 SOI/EOI 边界

```cpp
// 从前向后找 SOI
for (int i = 0; i < len - 1; ++i) {
    if (data[i] == 0xFF && data[i + 1] == 0xD8) { start = i; break; }
}
// 从后向前找 EOI
for (int i = len - 1; i > start + 1; --i) {
    if (data[i - 1] == 0xFF && data[i] == 0xD9) { end = i; break; }
}
```
- `0xFF` 标记广泛存在于 JPEG 数据中（如量化表、霍夫曼表），但**只有 0xFF + 特定标记字节**才是真正的帧边界
- 从后向前找 EOI 比从前向后快，因为 EOI 通常在帧末尾

```cpp
// ============================================================
// YUYV → RGB24 颜色空间转换（BT.601 定点运算）
// ============================================================

void VideoProcessor::yuyvToRgb24(const uint8_t* yuyv, uint8_t* rgb,
                                  int w, int h) {
#ifdef __ARM_NEON
    // ARM 平台: 使用 NEON SIMD 加速
    extern void yuyv_to_rgb24_neon(const uint8_t*, uint8_t*, int, int);
    yuyv_to_rgb24_neon(yuyv, rgb, w, h);
    return;
#endif

    // x86 / 无 NEON 退路: 标量 C++ 实现
    const int pixels = w * h;
    int di = 0;
    for (int i = 0; i < pixels; i += 2) {
        int si = i * 2;
        int y0 = yuyv[si];
        int u  = yuyv[si + 1] - 128;    // U 去偏移（-128~127 → 0~255）
        int y1 = yuyv[si + 2];
        int v  = yuyv[si + 3] - 128;    // V 去偏移

        auto clip = [](int x) -> uint8_t {
            return static_cast<uint8_t>(x < 0 ? 0 : (x > 255 ? 255 : x));
        };

        // BT.601 定点运算: YUV → RGB
        // R = Y + 1.402   × (V-128)    → Y + (V × 359) >> 8
        // G = Y - 0.34414 × (U-128) - 0.71414 × (V-128) → Y - (U×88 + V×183) >> 8
        // B = Y + 1.772   × (U-128)    → Y + (U × 454) >> 8

        int r0 = y0 + ((v * 359) >> 8);
        int g0 = y0 - ((u * 88) >> 8) - ((v * 183) >> 8);
        int b0 = y0 + ((u * 454) >> 8);

        int r1 = y1 + ((v * 359) >> 8);
        int g1 = y1 - ((u * 88) >> 8) - ((v * 183) >> 8);
        int b1 = y1 + ((u * 454) >> 8);

        rgb[di++] = clip(r0); rgb[di++] = clip(g0); rgb[di++] = clip(b0);
        rgb[di++] = clip(r1); rgb[di++] = clip(g1); rgb[di++] = clip(b1);
    }
}
```

**BT.601 颜色空间转换的定点优化**

标准 BT.601 公式使用浮点数：
```
R = Y + 1.402   × (V - 128)
G = Y - 0.34414 × (U - 128) - 0.71414 × (V - 128)
B = Y + 1.772   × (U - 128)
```

浮点运算在 Cortex-A7 上**非常慢**（没有硬件 FPU 的 SIMD 加速）。这里使用定点近似：
```
1.402  ≈ 359 / 256    →  移8位定点 (Q8)
0.344  ≈ 88 / 256
0.714  ≈ 183 / 256
1.772  ≈ 454 / 256
```

实现为：
```cpp
int r0 = y0 + ((v * 359) >> 8);
//        ↑      ↑相乘后右移8位 = 除以256
```
- 全程整数运算，零浮点指令
- 右移 8 位替代除法 `÷ 256`（编译器优化为单周期移位指令）
- `clip()` 函数模拟饱和截断到 `[0, 255]`

**宏像素处理**：
```cpp
for (int i = 0; i < pixels; i += 2) {   // 每次处理 2 个像素
    int si = i * 2;
    int y0 = yuyv[si];    // Y0
    int u  = yuyv[si+1];  // U  (Y0和Y1共用)
    int y1 = yuyv[si+2];  // Y1
    int v  = yuyv[si+3];  // V  (Y0和Y1共用)
```
YUYV 4:2:2 格式：每 4 字节 = `[Y0, U, Y1, V]` 对应 2 个像素。Y 全分辨率，UV 水平半分辨率。

```cpp
// ============================================================
// JPEG 编码（libjpeg-turbo）
// ============================================================

int VideoProcessor::encodeRGBtoJPEG(const uint8_t* rgb, int width, int height,
                                    int quality, uint8_t** jpeg_out,
                                    unsigned long* jpeg_len) {
#ifdef HAS_LIBJPEG
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    // 输出到内存（而非文件）
    unsigned char* buffer = nullptr;
    unsigned long  outlen = 0;
    jpeg_mem_dest(&cinfo, &buffer, &outlen);   // ★ 内存输出

    cinfo.image_width      = static_cast<JDIMENSION>(width);
    cinfo.image_height     = static_cast<JDIMENSION>(height);
    cinfo.input_components = 3;                 // RGB = 3 通道
    cinfo.in_color_space   = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    // 逐行写入——ARM NEON 友好（逐行处理而非整帧一次性）
    JSAMPROW row_pointer[1];
    int row_stride = width * 3;
    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = const_cast<JSAMPROW>(
            &rgb[cinfo.next_scanline * row_stride]);
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    *jpeg_out = buffer;     // malloc'd 内存，调用者需 free
    *jpeg_len = outlen;

    LOG_DBG("JPEG encoded: %dx%d quality=%d → %lu bytes",
              width, height, quality, outlen);
    return 0;

#else
    *jpeg_out = nullptr;
    *jpeg_len = 0;
    LOG_WRN("JPEG encoding not available — compile with -DHAS_LIBJPEG");
    return -1;
#endif
}

int VideoProcessor::encodeYUYVtoJPEG(const uint8_t* yuyv, int width, int height,
                                     int quality, uint8_t** jpeg_out,
                                     unsigned long* jpeg_len) {
    // YUYV → RGB24 临时缓冲
    std::vector<uint8_t> rgb(static_cast<size_t>(width * height * 3));
    yuyvToRgb24(yuyv, rgb.data(), width, height);

    return encodeRGBtoJPEG(rgb.data(), width, height, quality, jpeg_out, jpeg_len);
}
```

**libjpeg-turbo 内存编码 (行 175-220)**

```cpp
jpeg_mem_dest(&cinfo, &buffer, &outlen);
```
- libjpeg 默认输出到文件，`jpeg_mem_dest` 重定向输出到动态分配的内存缓冲区
- `buffer` 由 libjpeg 内部 `malloc`，调用者负责 `free`
- 嵌入式场景下无需中间文件，直接用于网络推流

```cpp
while (cinfo.next_scanline < cinfo.image_height) {
    row_pointer[0] = const_cast<JSAMPROW>(&rgb[...]);
    jpeg_write_scanlines(&cinfo, row_pointer, 1);  // 逐行写入
}
```
- 逐行编码而非一次整帧：降低内存峰值（因为 `jpeg_mem_dest` 的输出是逐步增长的）

```cpp
int VideoProcessor::encodeYUYVtoJPEG(...) {
    std::vector<uint8_t> rgb(width * height * 3);
    yuyvToRgb24(yuyv, rgb.data(), width, height);
    return encodeRGBtoJPEG(rgb.data(), ...);
}
```
- YUYV→JPEG 是两步操作：先 YUYV→RGB24，再 RGB24→JPEG
- 利用纯静态方法链式组合

---

### 3.5 processor_neon.cpp

```cpp
/**
 * @file    processor_neon.cpp
 * @brief   YUYV→RGB24 的 ARM NEON SIMD 加速实现
 *
 * 目标平台: Cortex-A7 (armv7-a), i.MX6ULL
 * 编译条件: __ARM_NEON 已定义 (由 -mfpu=neon 自动开启)
 *
 * 加速原理:
 *   - YUYV 4:2:2 每 4 字节 = [Y0,U,Y1,V] = 2 像素，U/V 共用
 *   - NEON 128-bit 寄存器一次加载 16 字节 = 4 个宏像素 = 8 真实像素
 *   - BT.601 系数使用定点 Q8 运算 (乘加 → 右移 8 位)
 *   - vqmovun 内置饱和转换，无需手动 clamp
 *
 * 轮次处理:
 *   主循环: 一次处理 8 宏像素 = 16 像素 = 32B YUYV 输入 → 48B RGB 输出
 *   尾部:   < 16 像素的残量退化为标量 C++ 循环
 */

#include <arm_neon.h>
#include <cstdint>
#include <cstddef>

void yuyv_to_rgb24_neon(const uint8_t* yuyv, uint8_t* rgb,
                         int width, int height) {
    const int totalPixels = width * height;
    const uint8_t* src = yuyv;
    uint8_t*       dst = rgb;

    // ── BT.601 定点系数 (Q8) ──
    const int16x8_t vRcoeff   = vdupq_n_s16(359);
    const int16x8_t vGcoeff_U = vdupq_n_s16(88);
    const int16x8_t vGcoeff_V = vdupq_n_s16(183);
    const int16x8_t vBcoeff   = vdupq_n_s16(454);

    const int16x8_t v128 = vdupq_n_s16(128);
    const int16x8_t vHalf = vdupq_n_s16(128);  // 四舍五入

    // ── 主循环: 每次处理 16 像素 ──
    int i = 0;
    for (; i + 15 < totalPixels; i += 16, src += 32) {
        // Step 1: 加载并去交织 YUYV
        uint8x16x2_t yu = vld2q_u8(src);      // ★ NEON 去交织指令
        uint8x16_t  y16   = yu.val[0];         // 16 个 Y 值
        uint8x16_t  uv16  = yu.val[1];         // 8 对 UV 值

        // Step 2: 从 uv16 中分离 U 和 V，扩展到 16-bit 并去偏移
        uint8x8_t u8 = /* ... 通过 vuzp 去交织 ... */;
        uint8x8_t v8 = /* ... */;
        int16x8_t u = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(u8)), v128);
        int16x8_t v = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(v8)), v128);

        // Step 3: Y 扩展到 16-bit
        int16x8_t y_lo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(y16)));
        int16x8_t y_hi = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(y16)));

        // Step 4: BT.601 矩阵乘法（NEON 向量化：每次 8 个像素并行）
        int16x8_t r_lo = vaddq_s16(y_lo,
            vshrq_n_s16(vaddq_s16(vmulq_s16(v, vRcoeff), vHalf), 8));
        int16x8_t g_lo = vsubq_s16(y_lo,
            vshrq_n_s16(vaddq_s16(
                vaddq_s16(vmulq_s16(u, vGcoeff_U),
                          vmulq_s16(v, vGcoeff_V)), vHalf), 8));
        int16x8_t b_lo = vaddq_s16(y_lo,
            vshrq_n_s16(vaddq_s16(vmulq_s16(u, vBcoeff), vHalf), 8));
        // ... 高 8 像素同理 ...

        // Step 5: 饱和转换 int16 → uint8 (自动 clip 到 [0,255])
        uint8x8_t r8_lo = vqmovun_s16(r_lo);   // ★ 内置饱和转换，无需循环 clip
        uint8x8_t g8_lo = vqmovun_s16(g_lo);
        uint8x8_t b8_lo = vqmovun_s16(b_lo);

        // Step 6: 交织 R,G,B → interleaved RGB24 并写入
        uint8x8x3_t rgb_lo;
        rgb_lo.val[0] = r8_lo;
        rgb_lo.val[1] = g8_lo;
        rgb_lo.val[2] = b8_lo;
        vst3_u8(dst, rgb_lo);       // ★ NEON 去交织存储指令
        dst += 24;                   // 8 像素 × 3 = 24 字节
        // ... 高 8 像素同理 ...
    }

    // ── 尾部: 剩余 < 16 像素，退化为标量循环 ──
    for (; i + 1 < totalPixels; i += 2) {
        // 与 processor.cpp 中同样的 BT.601 标量实现
        // ...
    }
}
```

**逐段详解 — NEON SIMD 加速核心**

**NEON 128-bit 寄存器能力**：
- Cortex-A7 NEON 有 16 个 128-bit 寄存器 (Q0~Q15)
- 一次可以处理 16×8-bit = 8×16-bit = 4×32-bit 数据
- YUYV 转换：每 4 字节 → 6 字节 RGB，NEON 一次处理 32 字节 YUYV → 48 字节 RGB

**Step 1: vld2q_u8 去交织加载 (行 50-52)**
```cpp
uint8x16x2_t yu = vld2q_u8(src);
```
这是最核心的指令！`vld2q_u8` 从内存加载 32 字节，并自动将偶数字节和奇数字节分到两个独立的 128-bit 寄存器：
- `yu.val[0]` = [Y0, Y1, Y2, ..., Y15]（16 个 Y）
- `yu.val[1]` = [U0, V0, U1, V1, ..., U7, V7]（8 对 UV）

一个指令完成了手动循环中需要 16 次字节操作的工作！

**Step 2: UV 分离 (行 54-76)**
```cpp
uint8x8x2_t uv_lo_sep = vuzp_u8(uv_lo, uv_lo);
```
`vuzp` (unzip) 指令将 [U0,V0,U2,V2,...] 分离为 U=[U0,U2,...] 和 V=[V0,V2,...]，一次处理 8 对 UV。

**Step 4: BT.601 向量化计算 (行 88-108)**
```cpp
int16x8_t r_lo = vaddq_s16(y_lo,
    vshrq_n_s16(vaddq_s16(vmulq_s16(v, vRcoeff), vHalf), 8));
```
这个单行 NEON 内联汇编并行计算了 **8 个像素的 R 值**：
- `vmulq_s16(v, vRcoeff)` → 8 个 V×(359) 同时计算
- `vaddq_s16(..., vHalf)` → 加 128（用于四舍五入到最近整数）
- `vshrq_n_s16(..., 8)` → 右移 8 位实现除以 256
- `vaddq_s16(y_lo, ...)` → 加上亮度 Y

每一步都是 **8 路 SIMD 并行**！

**Step 5: vqmovun_s16 饱和转换**
```cpp
uint8x8_t r8_lo = vqmovun_s16(r_lo);
```
`vqmovun_s16` 是带饱和的运动窄化指令：
- 输入：8 个 16-bit 有符号整数
- 输出：8 个 8-bit 无符号整数
- 自动 clamp：<0 → 0，>255 → 255
- **无需手动循环做 clip()**

**Step 6: vst3_u8 交织存储**
```cpp
vst3_u8(dst, rgb_lo);
```
`vst3` 将 R、G、B 三个寄存器按 R₀,G₀,B₀,R₁,G₁,B₁,... 的方式交织写入内存——生成正确的 RGB24 interleaved 格式。

**性能对比**：
- 标量版本：640×480 ≈ 307,200 像素，每像素约 6 次乘法+3 次加减 → ~2.8M 次运算
- NEON 版本：约 307,200 / 16 × 1 轮 NEON 循环 = ~19,200 次循环，每次循环内 8 路并行
- 理论加速比约 **6-8×**

---

## 4. 显示模块

### 4.1 gui.h

```cpp
#ifndef SMART_CAM_DISPLAY_GUI_H
#define SMART_CAM_DISPLAY_GUI_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QTimer>
#include <QStackedWidget>
#include <QDialog>
#include <QSlider>
#include <QCheckBox>
#include <cstdint>

#include "include/common/types.h"
#include "include/display/gallery.h"

class CameraGUI : public QWidget {
    Q_OBJECT

public:
    explicit CameraGUI(QWidget* parent = nullptr);
    ~CameraGUI() override;

    // 帧数据输入
    void setFrame(const uint8_t* data, int len, int w, int h, PixelFormat fmt);

    // 状态更新
    void setFPS(double fps);           // 硬件帧率 (HW_FPS)
    void setDisplayFPS(double fps);     // 软件/显示帧率 (SW_FPS)
    void setClientCount(int count);
    void setRecordingStatus(bool recording);
    void setStreamingStatus(bool streaming);
    void setStoragePath(const std::string& path);

    // 回调注册 (callbacks injected by main.cpp)
    using CallbackVoid   = std::function<void()>;
    using CallbackIntInt = std::function<void(int, int)>;
    using CallbackFormat = std::function<void(PixelFormat)>;
    using CallbackString = std::function<void(const std::string&)>;
    using CallbackCameraControl = std::function<void(int cid, int value)>;
    using CallbackFramerate = std::function<void(int fps)>;

    void onCaptureRequest(CallbackVoid cb);
    void onRecordToggle(std::function<bool(bool)> cb);   // 返回 true=成功
    void onResolutionChanged(CallbackIntInt cb);
    void onFormatChanged(CallbackFormat cb);
    void onStoragePathChanged(CallbackString cb);
    void onCameraControlChanged(CallbackCameraControl cb);
    void onFramerateChanged(CallbackFramerate cb);

    // 相机控制参数范围设置（由 main.cpp 在 V4L2 查询后初始化）
    void setBrightnessRange(int min, int max, int step, int value);
    void setContrastRange(int min, int max, int step, int value);
    void setWhiteBalanceRange(int min, int max, int step, int value);
    void setAutoWhiteBalance(bool enabled);
    void setExposureRange(int min, int max, int step, int value);
    void setAutoExposure(bool enabled);
    void setFramerateRange(int minFps, int maxFps, int currentFps);

    void setGalleryStorage(StorageManager* storage);
    void showGallery();
    void showLivePreview();

signals:
    void captureClicked();
    void recordToggled(bool start);
    void resolutionChanged(int w, int h);
    void formatChanged(PixelFormat fmt);

private slots:
    void refreshFrame();
    void onCapture();
    void onRecord();
    void onSettings();
    void onGallery();
    void onBackFromGallery();
    void onBrightnessChanged(int value);
    void onContrastChanged(int value);
    void onAutoWbChanged(int state);
    void onWbChanged(int value);
    void onAutoExposureChanged(int state);
    void onExposureChanged(int value);
    void onFramerateSliderChanged(int value);
    void onFramerateDebounced();
    void onResetDefaults();

private:
    void buildUI();
    void buildSettingsDialog();
    void connectSignals();
    void enterMockMode();
    QImage frameToQImage(const uint8_t* data, int len, int w, int h, PixelFormat fmt);

    QStackedWidget* m_mainStack;
    QLabel*         m_videoDisplay;
    QPushButton*    m_btnCapture;
    QPushButton*    m_btnRecord;
    QPushButton*    m_btnSettings;
    QPushButton*    m_btnGallery;
    PhotoGallery*   m_gallery;
    QLabel*         m_labelFPS;
    QDialog*        m_settingsDialog;
    QSlider*        m_brightnessSlider;
    QSlider*        m_contrastSlider;
    QSlider*        m_wbSlider;
    QCheckBox*      m_autoWbCheckBox;
    QSlider*        m_exposureSlider;
    QCheckBox*      m_autoExposureCheckBox;
    QSlider*        m_framerateSlider;
    QTimer*         m_framerateDebounceTimer;

    FrameBuffer  m_currentFrame;
    std::vector<uint8_t> m_frameBuffer;  // 内部深拷贝缓冲
    bool         m_isRecording = false;
    bool         m_mockMode    = false;

    struct ControlInfo { int min, max, step, def, current; };
    ControlInfo m_brightnessInfo, m_contrastInfo, m_wbInfo, m_exposureInfo, m_framerateInfo;

    CallbackVoid        m_onCapture;
    CallbackBool        m_onRecordToggle;
    CallbackIntInt      m_onResolutionChanged;
    CallbackFormat      m_onFormatChanged;
    CallbackString      m_onStoragePathChanged;
    CallbackCameraControl m_onCameraControl;
    CallbackFramerate    m_onFramerate;
};

// YUYV → RGB24 内联转换 (头文件中定义，供 gui.cpp 使用)
inline void yuyv_to_rgb24(const uint8_t* yuyv, uint8_t* rgb, int w, int h) {
#ifdef __ARM_NEON
    extern void yuyv_to_rgb24_neon(const uint8_t*, uint8_t*, int, int);
    yuyv_to_rgb24_neon(yuyv, rgb, w, h);
    return;
#endif
    // 标量退路
    const int pixels = w * h;
    int di = 0;
    for (int i = 0; i < pixels; i += 2) {
        int si = i * 2;
        int y0 = yuyv[si], u = yuyv[si+1] - 128, y1 = yuyv[si+2], v = yuyv[si+3] - 128;
        auto clip = [](int x) -> uint8_t { return x < 0 ? 0 : (x > 255 ? 255 : x); };
        rgb[di++] = clip(y0 + ((v * 359) >> 8));
        rgb[di++] = clip(y0 - ((u * 88) >> 8) - ((v * 183) >> 8));
        rgb[di++] = clip(y0 + ((u * 454) >> 8));
        rgb[di++] = clip(y1 + ((v * 359) >> 8));
        rgb[di++] = clip(y1 - ((u * 88) >> 8) - ((v * 183) >> 8));
        rgb[di++] = clip(y1 + ((u * 454) >> 8));
    }
}

#endif
```

#### 逐段精读

**回调注入模式 (行 60-72)**
```cpp
using CallbackVoid   = std::function<void()>;
void onCaptureRequest(CallbackVoid cb) { m_onCapture = std::move(cb); }
```

GUI 类**不直接操作** CameraCapture/MJPEGServer/StorageManager，而是通过回调函数解耦：
- `main.cpp` 在构造 GUI 后，调用 `gui.onCameraControlChanged([capture](...) {...})` 注入 lambda
- 当用户拖动亮度滑块时，GUI 调用 `m_onCameraControl(V4L2_CID_BRIGHTNESS, value)`
- 这种模式使得 GUI 可以独立测试（Mock 模式）+ 真机运行时由 main.cpp 注入真实逻辑

**inner 帧缓冲 — 避免悬垂指针**
```cpp
std::vector<uint8_t> m_frameBuffer;  // 内部深拷贝缓冲
```
- `setFrame()` 把采集线程的共享数据**深拷贝**到 `m_frameBuffer`，然后 `m_currentFrame.data` 指向它
- 这是因为采集线程可能在 Qt 主线程渲染 QImage 之前就覆盖了原来的 mmap 内存

**`setFrame()` → `refreshFrame()` → `frameToQImage()` 的渲染流水线**：
```
采集线程: setFrame() → 深拷贝到 m_frameBuffer
QTimer:   refreshFrame() → frameToQImage() → QImage → QPixmap → QLabel::setPixmap()
```

---

### 4.2 gui.cpp

由于文件太大（约 800 行），这里重点解析几个核心函数：

```cpp
#include "include/display/gui.h"
#include "include/camera/capture.h"
#include <QApplication>
#include <QScreen>
#include <QPainter>
#include <QImage>
#include <QFont>
#include <QDateTime>
#include <QStackedWidget>
#include <QScrollArea>
#include <QDebug>
#include <cmath>
#include <cstring>
#include <algorithm>
```

**MJPEG 解码的静默错误处理**

```cpp
#ifdef HAS_LIBJPEG
#include <jpeglib.h>
#include <setjmp.h>

struct jpegErrorMgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

// ★ 自定义错误处理器：坏帧时静默跳过，不打印 libjpeg 的 stderr 警告
static void jpegSilentErrorExit(j_common_ptr cinfo) {
    jpegErrorMgr* myerr = reinterpret_cast<jpegErrorMgr*>(cinfo->err);
    longjmp(myerr->setjmp_buffer, 1);   // 跳回 setjmp 点
}

static void jpegSilentOutputMessage(j_common_ptr) {
    /* 完全静默 — 不输出任何警告 */
}

static bool decodeMjpegToRgb(const uint8_t* jpeg_data, size_t jpeg_len,
                              std::vector<uint8_t>& rgb, int& out_w, int& out_h) {
    struct jpeg_decompress_struct cinfo;
    jpegErrorMgr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit       = jpegSilentErrorExit;     // 替换为静默版本
    jerr.pub.output_message   = jpegSilentOutputMessage; // 替换为静默版本

    if (setjmp(jerr.setjmp_buffer)) {    // ★ setjmp 捕获 longjmp
        jpeg_destroy_decompress(&cinfo);
        return false;                    // 解码失败，返回 false（不闪退）
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_len);
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);

    out_w = cinfo.output_width;
    out_h = cinfo.output_height;
    rgb.resize(out_w * out_h * 3);

    while (cinfo.output_scanline < static_cast<JDIMENSION>(out_h)) {
        JSAMPROW row = rgb.data() + cinfo.output_scanline * out_w * 3;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}
#endif
```

**`setjmp`/`longjmp` 的错误处理模式**：

这是 libjpeg 书籍中的经典用法：
1. 用户定义 `jpegSilentErrorExit`，遇到错误时 `longjmp` 回 `setjmp` 点
2. 标准 libjpeg 会在 stderr 输出 "Corrupt JPEG data: ..." 等警告
3. `jpegSilentOutputMessage` 覆盖 `output_message` 回调 **完全静默**这些警告
4. 对嵌入式的实时视频流，坏帧是正常的（网络丢包、DMA 不完整）——用户的视频预览页不应该被日志刷屏

**Settings 对话框构建**

```cpp
void CameraGUI::buildSettingsDialog() {
    m_settingsDialog = new QDialog(this);
    m_settingsDialog->setMinimumSize(640, 440);
    m_settingsDialog->setModal(true);  // 阻塞主事件循环

    // 深色主题样式
    m_settingsDialog->setStyleSheet(
        "QDialog { background-color: #0F1117; }"
        "QLabel { color: #E6EDF3; font-size: 13px; }"
        "QGroupBox { color: #E6EDF3; font-size: 14px; font-weight: bold; "
        "  border: 1px solid #30363D; border-radius: 8px; ... }");

    // 三个分组框: Video Settings / Camera Controls / 底部按钮
    auto* videoGroup = new QGroupBox("Video Settings", m_settingsDialog);
    auto* camGroup   = new QGroupBox("Camera Controls", m_settingsDialog);
    // ...
}
```

**渲染流水线核心 — `frameToQImage()`**

```cpp
QImage CameraGUI::frameToQImage(const uint8_t* data, int len, int w, int h, PixelFormat fmt) {
    switch (fmt) {
    case PixelFormat::FMT_RGB24:
        return QImage(data, w, h, w * 3, QImage::Format_RGB888).copy();
        // ↑ .copy() 强制深拷贝——QImage 不拥有 data 指针！

    case PixelFormat::FMT_YUYV: {
        std::vector<uint8_t> rgb(w * h * 3);
        yuyv_to_rgb24(data, rgb.data(), w, h);   // 内联函数 → NEON or 标量
        return QImage(rgb.data(), w, h, w * 3, QImage::Format_RGB888).copy();
    }

    case PixelFormat::FMT_MJPEG:
#ifdef HAS_LIBJPEG
        std::vector<uint8_t> rgb;
        int dw = 0, dh = 0;
        if (decodeMjpegToRgb(data, len, rgb, dw, dh)) {
            return QImage(rgb.data(), dw, dh, dw * 3, QImage::Format_RGB888).copy();
        }
#endif
        // 退路: Qt 内置解码器
        QImage img;
        if (img.loadFromData(data, len, "JPEG")) {
            return img.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        return {};  // 解码失败，空 QImage
    }
}
```

**`.copy()` 为什么重要**：
```cpp
return QImage(data, rgb.data(), w, h, w * 3, Format_RGB888).copy();
```
- `QImage(const uchar*, ...)` **不拷贝数据**——它只存储一个指向传入 `data` 的指针
- 如果没有 `.copy()`，`rgb` 向量析构后 QImage 的指针就悬垂了
- `.copy()` 强制深拷贝——QImage 自己 `malloc` 一块内存，拷贝像素数据

**Mock 模式 — 彩色测试条生成**

```cpp
void CameraGUI::enterMockMode() {
    m_mockMode = true;
    // 预生成彩色竖条测试图
    static constexpr uint8_t Colors[][3] = {
        {255,255,255}, {255,255,0}, {0,255,255},
        {0,255,0}, {255,0,255}, {255,0,0},
        {0,0,255}, {0,0,0},
    };
    constexpr int NumColors = sizeof(Colors) / sizeof(Colors[0]);
    int barWidth = w / NumColors;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int colorIdx = (x / barWidth) % NumColors;
            float factor = 0.6f + 0.4f * y / h;  // 纵向渐变
            int idx = (y * w + x) * 3;
            buf[idx]   = Colors[colorIdx][0] * factor;
            buf[idx+1] = Colors[colorIdx][1] * factor;
            buf[idx+2] = Colors[colorIdx][2] * factor;
        }
    }
}
```

### 4.3 gallery.h / gallery.cpp

**关键数据结构**

```cpp
class PhotoGallery : public QWidget {
    Q_OBJECT
    // 两种视图
    QStackedWidget* m_stack;   // [0]=网格缩略图, [1]=全屏查看
    // 缩略图生成
    bool createThumbnail(const std::string& jpegPath, int thumbW, int thumbH, QPixmap& out);
    bool createVideoThumbnail(const std::string& aviPath, int thumbW, int thumbH, QPixmap& out);
    // 触摸滑动
    bool eventFilter(QObject* obj, QEvent* event) override;
};
```

**gallery.cpp 核心逻辑 — JPEG 缩放解码 (libjpeg scale_num/scale_denom)**

```cpp
bool PhotoGallery::createThumbnail(const std::string& jpegPath,
                                    int thumbW, int thumbH, QPixmap& out) {
#ifdef HAS_LIBJPEG
    // ★ libjpeg 内置缩放解码——不解码全尺寸再缩小，直接在 DCT 域缩放
    int scaleDenom = 1;
    if (cinfo.image_width > thumbW * 8)  scaleDenom = 8;  // 1/8 采样
    else if (cinfo.image_width > thumbW * 4)  scaleDenom = 4;  // 1/4
    else if (cinfo.image_width > thumbW * 2)  scaleDenom = 2;  // 1/2
    cinfo.scale_num   = 1;
    cinfo.scale_denom = scaleDenom;     // 1/8 缩放 → DCT 域直接降采样
```
- 如果原图 1920×1080，缩略图 170×120：`scale_denom=8` → 解码输出 240×135 → 再 Qt scale 到 170×120
- DCT 域缩放远快于全尺寸解码+缩小（省去了 7/8 的 IDCT 计算）

**视频缩略图提取**

```cpp
bool PhotoGallery::createVideoThumbnail(const std::string& aviPath,
                                         int thumbW, int thumbH, QPixmap& out) {
    std::vector<uint8_t> jpegData;
    if (!StorageManager::extractAviThumbnail(aviPath, jpegData))  // 从 AVI 提取第一帧
        return false;
    return createThumbnailFromJpegData(jpegData, thumbW, thumbH, out);
}
```
- 视频的封面来自 AVI 文件中的第一帧 MJPEG 数据
- `extractAviThumbnail` 解析 RIFF 容器 → 找 `movi` LIST → 读取第一个 `00dc` chunk → 得到 JPEG 字节流

### 4.4 video_player.h / video_player.cpp

**轻量 AVI 播放器架构**

```cpp
class VideoPlayer : public QWidget {
    Q_OBJECT
    FILE*        m_file;
    QTimer*      m_timer;
    double       m_fps;
    int          m_totalFrames;
    int          m_currentFrame;
    std::vector<AviIndexEntry> m_index;   // idx1 索引表
    bool m_playing;
};
```

**播放逻辑**

```cpp
void VideoPlayer::onTimerTick() {
    m_currentFrame++;
    if (m_currentFrame >= m_totalFrames) {
        m_currentFrame = m_totalFrames - 1;
        pause();
        emit playbackFinished();    // 播放完毕 → gallery 收到信号
        return;
    }
    // seek → 读 00dc chunk → 解码 JPEG → QLabel::setPixmap
    std::vector<uint8_t> jpegData;
    readFrameJpeg(jpegData);
    decodeAndDisplay(jpegData);
}
```

**AVI 文件解析**

```cpp
bool VideoPlayer::parseAviHeader() {
    // 1. 读 RIFF + "AVI " head
    // 2. 解析 LIST hdrl → 找 avih chunk → 提取宽/高/总帧数/fps
    // 3. 跳过 hdrl → 定位 LIST movi → 记录 m_moviDataOffset
    // 4. 解析 idx1 索引表 → 填充 m_index (每帧偏移+长度)
```

## 5. 网络模块

### 5.1 mjpeg_server.h / .cpp

```cpp
class MJPEGStreamServer {
    // producer-consumer 模型
    std::mutex              m_frameMtx;
    std::condition_variable m_frameCV;
    std::vector<uint8_t>    m_currentFrame;
    std::atomic<uint64_t>   m_frameIndex{0};

    // accept 线程（1个）
    std::thread* m_acceptThread;
    // 客户端列表
    struct ClientInfo {
        int fd; bool active; uint64_t lastSentIndex;
        int quality = 100;  // 100=直通，<100则重编码
    };
    std::vector<ClientInfo> m_clients;
};
```

**架构设计**：
```
采集线程: updateFrame(jpeg) → 存储到 m_currentFrame → notify_all

accept 线程: accept() → 创建客户端线程 (detach)

客户端线程:
  读取 HTTP 请求 → 解析路径:
    GET /          → sendIndexPage() (HTML 页面)
    GET /stream    → sendHttpHeader() + 循环 sendMJPEGFrame()
    GET /snapshot  → sendSnapshot() (单帧 JPEG)
    GET /status    → sendStatusJSON()

  流发送循环 (每客户端独立):
    while (running) {
        wait(frameCV, 有新帧)
        发送 multipart header + JPEG 数据
    }
```

**HTTP multipart/x-mixed-replace 协议**

```cpp
bool MJPEGStreamServer::sendMJPEGFrame(int client_fd, const uint8_t* jpeg, size_t len) {
    char header[256];
    snprintf(header, sizeof(header),
        "--%s\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: %zu\r\n"
        "\r\n",
        kBoundary, len);
    write(client_fd, header, ...);
    write(client_fd, jpeg, len);
    write(client_fd, "\r\n", 2);
}
```

multipart 格式：
```
--SmartCamFrame\r\n
Content-Type: image/jpeg\r\n
Content-Length: 54789\r\n
\r\n
[JPEG BINARY DATA 54789 bytes]\r\n
--SmartCamFrame\r\n
Content-Type: image/jpeg\r\n
Content-Length: 55123\r\n
\r\n
[JPEG BINARY DATA 55123 bytes]\r\n
...
```

浏览器解析到 `Content-Type: multipart/x-mixed-replace` 后，会自动把每个 part 作为独立帧渲染，形成视频流效果。

**按 quality 重编码缓存**

```cpp
void MJPEGStreamServer::updateFrame(const uint8_t* data, size_t len) {
    // 对每个需要的 quality<100 做重编码（同一帧只做一次）
    for (const auto& [quality, _] : needed) {
        uint8_t* reJpeg = nullptr;
        reencodeJpegQuality(data, len, &reJpeg, &reLen, quality);
        m_qualityCache[quality].assign(reJpeg, reJpeg + reLen);
    }
    m_frameCV.notify_all();
}
```
- 多个客户端请求相同 quality → **只重编码一次**，共享缓存
- `quality=100` → 直通原数据，零开销

### 5.2 control.h / control.cpp

**私有二进制协议帧格式**

```
Byte:  0    1    2    3    4  5    6..N-3      N-2  N-1
Field: [EB] [90] [1] [CMD] [PLEN_HI PLEN_LO] [PAYLOAD] [CRC16_HI CRC16_LO]
```

```cpp
#pragma pack(push, 1)
struct ProtoHeader {
    uint8_t  magic[2];       // 0xEB 0x90
    uint8_t  version;        // 0x01
    uint8_t  cmd;            // 命令类型
    uint16_t payload_len;    // 负载长度（网络字节序）
};
#pragma pack(pop)
```

**epoll 边缘触发事件循环**

```cpp
void ControlServer::eventLoop() {
    struct epoll_event events[64];
    while (m_running) {
        int nfds = epoll_wait(m_epoll_fd, events, 64, kHeartbeatCheckInterval * 1000);
        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == m_server_fd) {
                acceptClient();   // 新连接
            } else if (events[i].events & EPOLLIN) {
                handleClientData(fd);  // 读数据 → 解析帧
            }
        }
        checkHeartbeats();  // 周期性心跳超时检查
    }
}
```

**TCP 粘包处理**

```cpp
bool ControlServer::tryParseFrame(int client_fd, std::vector<uint8_t>& buf) {
    // 1. 查找魔数 0xEB 0x90
    // 2. 检查是否有完整帧头 (≥8字节)
    // 3. 解析 payload_len → 计算完整帧长
    // 4. 等待足够数据 → 验证 CRC16
    // 5. 分发命令 → 移除已处理帧
}
```

关键点——**如果一次 TCP recv 收到多个帧**：
- `tryParseFrame` 在 `while` 循环中反复调用，每成功解析一帧就从 `buf` 首部移除
- 如果 `buf` 中数据不够完整帧，返回 `false`，等待下次 `EPOLLIN` 追加数据

**CRC-16/MODBUS**

```cpp
uint16_t crc16Modbus(const uint8_t* data, int len) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}
```

标准 MODBUS CRC-16 算法，多项式 `0x8005`（反转 `0xA001`），初值 `0xFFFF`。

**命令分发表**

```cpp
std::map<uint8_t, CommandHandler> m_handlers;
// CMD_CAPTURE        → 拍照回调
// CMD_START_RECORD   → 开始录像回调
// CMD_STOP_RECORD    → 停止录像回调
// CMD_SET_RESOLUTION → 设置分辨率回调
// CMD_SET_FORMAT     → 切换格式回调
// CMD_GET_STATUS     → 查询状态回调
// CMD_HEARTBEAT      → 内置心跳处理器
```

### 5.3 rtsp_server.h / rtsp_server.cpp

**RTSP 协议交互流程**

```
Client                 Server (SmartCam)
  |                   |
  |--OPTIONS--------->|  支持哪些方法？
  |<--200 OK----------|  Public: OPTIONS,DESCRIBE,SETUP,PLAY,TEARDOWN
  |                   |
  |--DESCRIBE-------->|  请描述流信息
  |<--200 OK SDP------|  返回 SDP (描述分辨率/帧率/编解码)
  |                   |
  |--SETUP----------->|  建立 RTP/RTCP 通道 (Transport: RTP/AVP;unicast;client_port=5000-5001)
  |<--200 OK (Session)|  返回 session ID + server RTP/RTCP 端口
  |                   |
  |--PLAY------------>|  开始播放
  |<--200 OK----------|  开始推送 RTP 流
  |                   |
  |  [RTP UDP 数据...]|
  |  [RTCP SR ...]    |  每 5s 一次
  |                   |
  |--TEARDOWN-------->|  断开连接
  |<--200 OK----------|
```

**epoll + TCP 控制连接处理**

```cpp
void RTSPServer::eventLoop() {
    struct epoll_event events[64];
    while (m_running) {
        int nfds = epoll_wait(m_epoll_fd, events, 64, 1000);  // 1s 超时
        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == m_server_fd) acceptClient();
            else handleClientData(fd);  // 解析 RTSP 请求文本
        }
        checkRTCPSR();  // 定时发送 RTCP Sender Report
    }
}
```

**RTSP 文本请求解析**

```
DESCRIBE rtsp://192.168.1.100:8554/stream RTSP/1.0\r\n
CSeq: 1\r\n
Accept: application/sdp\r\n
\r\n
```

```cpp
bool RTSPServer::parseRequest(const std::string& buf,
                               std::string* method, std::string* uri,
                               int* cseq,
                               std::map<std::string, std::string>* headers) {
    std::istringstream stream(buf);
    std::string line;
    std::getline(stream, line);  // 请求行: "DESCRIBE rtsp://..."
    // 提取 method, uri
    while (std::getline(stream, line)) {
        if (line.empty() || line == "\r") break;  // 空行=头结束
        size_t colon = line.find(':');
        std::string key = line.substr(0, colon);  // "CSeq"
        std::string val = line.substr(colon + 1); // "1"
        (*headers)[key] = val;
    }
}
```

**RTP JPEG 分片发送 (RFC 2435)**

```cpp
void RTSPServer::rtpSendFrame(ClientInfo* ci, const uint8_t* jpeg, size_t len,
                               int width, int height) {
    const size_t maxPayload = kRtpMaxPayload;  // 1400 bytes
    const size_t numFragments = (len + maxPayload - 1) / maxPayload;
    // 以太网 MTU 1500 - IP头20 - UDP头8 = 1472，留余量取 1400

    for (size_t fragIdx = 0; fragIdx < numFragments; ++fragIdx) {
        // 组装 RTP 包: [RTP固定头(12)] + [JPEG专有头(8)] + [JPEG数据片段]
        RTPHeader* rtp = ...;
        rtp->marker = (fragIdx == numFragments - 1) ? 1 : 0;
                      // ★ 最后一包的 marker 位=1，表示帧结束

        RTPJPEGHeader* jpegHdr = ...;
        jpegHdr->frag_offset[0/1/2] = offset;  // 24-bit 片段偏移
        jpegHdr->type = 0;     // tables in main JPEG header
        jpegHdr->q = 255;      // Q factor 255 = 量化表不包含在 RTP 头中
        jpegHdr->width_div8  = width / 8;
        jpegHdr->height_div8 = height / 8;

        sendto(ci->rtp_sock_fd, pkt.data(), pkt.size(), 0,
               &ci->rtp_addr, sizeof(ci->rtp_addr));
    }
}
```

**每客户端独立 RTP 状态**
```cpp
struct ClientInfo {
    uint16_t rtp_seq = 0;    // 每帧递增
    uint32_t rtp_ts  = 0;    // 时间戳, 每帧增加 tsPerFrame
    uint32_t ssrc    = 0;    // 随机生成的同步源 ID
    uint32_t packet_count, octet_count;  // 用于 RTCP SR
};
```

**RTCP Sender Report**
```cpp
void RTSPServer::rtcpsSendSR(ClientInfo* ci) {
    uint64_t ntp = getNTPTimestamp();
    // 发送 SR 包：包含 NTP 时间戳 + RTP 时间戳 + 累计包数/字节数
    // 客户端 (VLC/ffplay) 用 RTCP SR 做音视频同步和网络抖动计算
}
```

**SDP 生成**
```cpp
std::string RTSPServer::buildSDP(const std::string& server_ip) {
    sdp << "v=0\r\n";
    sdp << "o=- " << time(nullptr) << " 1 IN IP4 " << server_ip << "\r\n";
    sdp << "s=SmartCam Live Stream\r\n";
    sdp << "m=video 0 RTP/AVP 26\r\n";      // payload type 26 = JPEG
    sdp << "a=rtpmap:26 JPEG/90000\r\n";     // 时钟频率 90kHz
    sdp << "a=fmtp:26 width=" << m_streamWidth
        << ";height=" << m_streamHeight << "\r\n";
    sdp << "a=framerate:" << m_streamFPS << ".0\r\n";
}
```

---

## 6. 存储模块

### manager.h / manager.cpp

**AVI 容器格式结构体**

```cpp
#pragma pack(push, 1)   // ★ 关键：禁止编译器对齐填充
struct RiffChunk {
    uint32_t fourcc;   // "RIFF", "LIST", "avih", "movi", "00dc", "idx1"
    uint32_t size;     // 后续数据大小（不含 fourcc + size 自身）
};

struct AviMainHeader {
    uint32_t dwMicroSecPerFrame;  // 每帧间隔 (微秒) = 1000000 / fps
    uint32_t dwWidth, dwHeight;
    uint32_t dwTotalFrames;      // 0（录制中）→ 录制结束后回填
    uint32_t dwStreams;          // 1 = 只有视频流
    uint32_t dwFlags;            // 0x10 = 包含 idx1 索引
    // ...
};

struct AviIndexEntry {
    uint32_t ckid;          // "00dc" = 0x63643030
    uint32_t dwFlags;       // 0x10 = 关键帧
    uint32_t dwChunkOffset; // 相对 movi 数据区的偏移
    uint32_t dwChunkLength; // 帧数据大小
};
#pragma pack(pop)
```

**`#pragma pack(push, 1)` — 二进制兼容性的关键**：
- 默认情况下，编译器在结构体中插入填充字节以满足对齐要求（如 `uint32_t` 需要 4 字节对齐）
- 如果不关闭填充，`sizeof(AviMainHeader)` 可能比 RIFF 标准多出几个字节
- 结果：Windows Media Player/VLC 无法正确解析 AVI 文件

**录像 = 边写边回填**

```cpp
int StorageManager::startRecord(int width, int height, int fps) {
    // 1. 创建文件，写入 AVI 头（其中 dwTotalFrames=0，留给 finalizeAvi 回填）
    writeAviHeader();   // 记录 m_avihFramesOffset, m_strhLengthOffset 等

    // 2. 写入 LIST movi 头
    writeFourCC(fp, "LIST");
    writeU32(fp, 0);  // movi size = 0，回填
    writeFourCC(fp, "movi");
}

int StorageManager::writeRecordFrame(const uint8_t* jpeg_data, int len) {
    // 写入 "00dc" chunk (压缩视频数据)
    writeFourCC(m_recordFile, "00dc");
    writeU32(m_recordFile, len);       // JPEG 帧大小
    fwrite(jpeg_data, 1, len, m_recordFile);
    if (len % 2 != 0) { fwrite("\0", 1, 1, ...); }  // WORD 对齐！

    // 记录帧索引项（相对 movi 数据区的偏移）
    FrameIndex idx;
    idx.offset = ftell(...) - m_moviDataOffset;
    idx.length = len;
    m_frameIndexList.push_back(idx);
}
```

**finalizeAvi() — 录制结束后的回填操作**

```cpp
int StorageManager::finalizeAvi() {
    // 1. 回填 movi LIST size
    fseek(fp, moviSizePos, SEEK_SET);
    writeU32(fp, actualMoviSize);

    // 2. 写入 idx1 索引块
    writeFourCC(fp, "idx1");
    writeU32(fp, indexDataSize);
    for (auto& entry : m_frameIndexList) {
        AviIndexEntry idxEntry = { 0x63643030, 0x10, entry.offset, entry.length };
        fwrite(&idxEntry, sizeof(idxEntry), 1, fp);
    }

    // 3. 回填 avih.dwTotalFrames 和 strh.dwLength
    fseek(fp, m_avihFramesOffset, SEEK_SET);
    writeU32(fp, actualFrameCount);

    // 4. 回填 RIFF 文件总大小
    fseek(fp, m_rifSizeOffset, SEEK_SET);
    writeU32(fp, actualFileSize - 8);  // RIFF size = 文件大小 - 8
}
```

**WORD 对齐 (行 245-249)**
```cpp
if (len % 2 != 0) {
    uint8_t pad = 0;
    fwrite(&pad, 1, 1, m_recordFile);
}
```
RIFF 规范要求所有 chunk 数据必须 16-bit 对齐。奇数长度 JPEG 需要补 1 字节 0。

**JPEG 头解析读取宽高 — 不完整解码**

```cpp
bool StorageManager::readJpegSize(const std::string& path, int& w, int& h) {
    FILE* fp = fopen(path.c_str(), "rb");
    uint8_t buf[4096];           // 只读前 4KB
    fread(buf, 1, sizeof(buf), fp);
    fclose(fp);

    for (size_t i = 2; i < n - 8; i++) {
        if (buf[i] == 0xFF) {
            uint8_t marker = buf[i + 1];
            if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
                // SOF0/SOF1: 段中包含高度和宽度
                h = (buf[i+5] << 8) | buf[i+6];
                w = (buf[i+7] << 8) | buf[i+8];
                return true;
            }
            // 跳过变长段
            uint16_t segLen = (buf[i+2] << 8) | buf[i+3];
            i += segLen + 1;
        }
    }
}
```
- 只读取 JPEG 文件的 SOF 标记段——不需要完整 JPEG 解压缩
- 扫描标记段：`0xFF + 标记字节 + 段长度 + 段数据`
- SOF (Start of Frame) 标记 `0xC0/0xC1/0xC2` 包含图像高度（2字节）和宽度（2字节）

---

## 7. main.cpp

```cpp
/**
 * @brief SmartCam Linux 主入口
 *
 * 支持两种运行模式:
 *   1. Mock 模式: --device 未指定 → 彩色测试条，用于 PC 端 UI 开发
 *   2. 真实相机模式: --device /dev/video0 → V4L2 采集 + 全部功能
 */

#include <QApplication>
#include <QCommandLineParser>
// ...

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // ---- 1. 命令行解析 ----
    QCommandLineParser parser;
    // --config, --device, --http-port, --control-port, --rtsp-port, --fmt

    // ---- 2. 加载配置文件（优先级: 命令行 > ~/.config > /etc）----
    ConfigManager cfg;
    cfg.load("/etc/smartcam/smartcam.conf");
    // 配置合并: 命令行 > 配置文件 > 硬编码默认值

    // ---- 3. 创建 GUI ----
    CameraGUI gui;

    if (!device.isEmpty()) {
        // ============================================================
        // 真实相机模式
        // ============================================================

        // 3a. 初始化 V4L2 摄像头
        CameraCapture* capture = new CameraCapture();
        capture->init(device);
        capture->setFormat(640, 480, pixfmt);
        capture->startCapture();

        // 3b. 查询 V4L2 控制参数范围 → 设置 GUI 滑块
        capture->queryControl(V4L2_CID_BRIGHTNESS, min, max, step, def);
        gui.setBrightnessRange(min, max, step, currentValue);

        // 3c. 启动 MJPEG HTTP 服务器
        MJPEGStreamServer* mjpegServer = new MJPEGStreamServer();
        mjpegServer->start(httpPort);

        // 3d. 启动 RTSP 服务器线程
        RTSPServer* rtspServer = new RTSPServer();
        std::thread* rtspThread = new std::thread([rtspServer, rtspPort]() {
            rtspServer->start(rtspPort);
        });

        // 3e. 启动 TCP 控制服务器线程
        ControlServer* controlSrv = new ControlServer();
        std::thread* controlThread = new std::thread([controlSrv, ctrlPort]() {
            controlSrv->start(ctrlPort);
        });

        // 3f. 启动采集线程
        std::thread* captureThread = new std::thread([capture]() {
            FrameBuffer fb;
            while (g_state.running) {
                capture->getFrame(&fb, 1000);        // DQBUF
                {
                    std::lock_guard lock(g_state.mtx);
                    g_state.frameData.assign(fb.data, fb.data + fb.length);
                }
                capture->putFrame(&fb);               // QBUF
                g_state.procCv.notify_one();          // 通知处理线程
            }
        });

        // 3g. 启动处理线程 (推流 + 录像)
        std::thread* processThread = new std::thread([...]() {
            while (g_state.running) {
                g_state.procCv.wait(...);             // 等待采集线程通知
                // MJEPG HTTP 推流
                mjpegServer->updateFrame(jpeg, len);
                // RTSP 推流
                rtspServer->feedFrame(jpeg, len, w, h);
                // 录像写入
                if (recording) g_storage->writeRecordFrame(jpeg, len);
            }
        });

        // 3h. 显示定时器 (Qt 主线程)
        QTimer* displayTimer = new QTimer(&gui);
        displayTimer->setInterval(33);  // 30fps
        QObject::connect(displayTimer, &QTimer::timeout, [&gui, mjpegServer]() {
            gui.setFrame(g_state.frameData.data(), ...);
            gui.setFPS(g_state.fps);           // HW_FPS
            gui.setDisplayFPS(dispCurrentFps);  // SW_FPS
        });
    } else {
        // Mock 模式
        // ...
    }

    gui.show();
    int ret = app.exec();   // Qt 事件循环

    // ---- 清理 ----
    g_state.running = false;
    g_state.procCv.notify_all();
    captureThread->join();
    processThread->join();
    // ...
}
```

**主程序的线程架构**

```
┌────────────────────────────────────────────────────────────┐
│                      main 线程 (Qt 事件循环)                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│  │  30fps   │  │ 按钮事件  │  │ Settings │  │  相册     │   │
│  │  QTimer  │  │ 处理     │  │  对话框  │  │  浏览     │   │
│  └────┬─────┘  └──────────┘  └──────────┘  └──────────┘   │
│       │ setFrame / setFPS / setDisplayFPS                   │
│       ▼                                                    │
│  ┌─────────────────────────────────────────┐               │
│  │  CameraGUI: m_frameBuffer (深拷贝)       │               │
│  │  → QImage → QPixmap → QLabel::setPixmap │               │
│  └─────────────────────────────────────────┘               │
└────────────────────────────────────────────────────────────┘
           ▲
           │ g_state.frameData (mutex 保护)
           │
┌──────────┴─────────────────────────────────────────────────┐
│                    采集线程 (captureThread)                  │
│  getFrame() → 拷贝到 g_state → putFrame() → notify处理线程   │
└────────────────────────────┬────────────────────────────────┘
                             │ procCv.notify
┌────────────────────────────▼────────────────────────────────┐
│                    处理线程 (processThread)                  │
│  YUYV→JPEG 编码 (CPU密集)                                   │
│  → mjpegServer->updateFrame()  (HTTP MJPEG 推流)           │
│  → rtspServer->feedFrame()    (RTSP/RTP 推流)              │
│  → storage->writeRecordFrame() (AVI 录像)                  │
└─────────────────────────────────────────────────────────────┘

┌────────────────────────────┐  ┌─────────────────────────────┐
│  MJPEG accept 线程         │  │  RTSP 事件循环线程           │
│  → 每个客户端独立线程        │  │  → epoll 单线程事件循环      │
│  → wait(condition_variable) │  │  → RTP UDP sendto()        │
└────────────────────────────┘  └─────────────────────────────┘

┌────────────────────────────┐
│  ControlServer 线程         │
│  → epoll ET 事件循环         │
│  → 二进制协议帧解析          │
└────────────────────────────┘
```

**暂停/恢复协调机制**

当用户切换分辨率或帧率时，需要暂停采集线程防止竞态：

```cpp
// main 线程: 设置暂停标志
g_state.paused = true;
g_state.pauseCv.wait_until(lk, timeout, []{ return g_state.pausedAck; });

// 采集线程: 响应暂停
if (g_state.paused) {
    g_state.pausedAck = true;
    g_state.pauseCv.notify_one();
    g_state.pauseCv.wait(lk, []{ return !g_state.paused; });
}
```

这是一个**双向握手**机制：
1. main 线程设置 `paused=true`，然后等待采集线程的 `pausedAck`
2. 采集线程在 `getFrame` 循环入口检测到 `paused`，设置 `pausedAck=true` 并进入等待
3. main 线程确认采集线程已停止后，安全地 `stopCapture()` → 切换参数 → `startCapture()`
4. main 线程解除暂停，采集线程恢复运行

**帧率节流 (软件丢帧)**

```cpp
// 采集线程中
if (throttleFps > 0) {
    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = (now - lastOutputTime).count();
    auto minIntervalMs = 1000 / throttleFps;
    if (elapsedMs < minIntervalMs) {
        capture->putFrame(&fb);   // 归还但不处理
        continue;                 // 跳过此帧
    }
}
```

- V4L2 的 `VIDIOC_S_PARM` 在某些摄像头驱动上不支持
- 软件节流兜底：如果距离上次输出不足 `1000/fps` 毫秒，直接归还缓冲区跳过此帧

---

## 附录：数据流总览

```
V4L2 Camera (/dev/video0)
    │
    │ mmap DMA buffer (零拷贝)
    ▼
┌─────────────────────┐
│  CameraCapture      │  采集线程
│  getFrame / putFrame│
└────────┬────────────┘
         │ FrameBuffer (YUYV or MJPEG)
         ▼
┌─────────────────────┐
│  g_state.frameData  │  共享内存 (mutex protected)
│  深拷贝             │
└──────┬──────┬───────┘
       │      │       │
       ▼      ▼       ▼
   ┌─────┐ ┌─────┐ ┌──────────────┐
   │ GUI │ │HTTP │ │RTSP          │
   │qtfb │ │MJPEG│ │RTP/UDP       │
   │     │ │     │ │RFC 2435      │
   └─────┘ └─────┘ └──────────────┘
       │      │       │
       ▼      ▼       ▼
   7" LCD  Browser   VLC/ffplay
   Touch     |       |
             |       |
       ┌─────┴───────┴────┐
       │  TCP Control     │
       │  二进制协议        │
       │  远程拍照/录像/控制 │
       └──────────────────┘
                    │
       ┌────────────┴────┐
       │  StorageManager │
       │  JPEG / AVI     │
       │  SD card / eMMC │
       └─────────────────┘
```

---

*本文档基于 SmartCam-Linux-on-imx6ull 项目源码（commit: main branch），逐文件进行代码级解读，涵盖架构设计、核心算法、线程模型、网络协议和二进制格式等全部底层实现细节。*
