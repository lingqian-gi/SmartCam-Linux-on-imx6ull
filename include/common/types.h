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
