#ifndef SMART_CAM_CAMERA_PROCESSOR_H
#define SMART_CAM_CAMERA_PROCESSOR_H

/**
 * @file    processor.h
 * @brief   VideoProcessor 工具类 — 图像格式转换、MJPEG 解析、JPEG 编码
 *
 * 提供纯静态方法，供采集/显示/传输模块调用。
 *
 * 主要功能:
 *   1. MJPEG 帧解析 — 从 MJPEG 流中提取完整 JPEG 帧
 *   2. 颜色空间转换 — YUYV→RGB24 / YUYV→RGB565 (BT.601 定点运算)
 *   3. JPEG 编码 — YUV → JPEG (使用 libjpeg-turbo，可选 NEON 加速)
 *   4. JPEG 帧大小查找 — 从 MJPEG 流中快速定位 JPEG 帧边界
 */

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

    /**
     * @brief 从 MJPEG 流中查找并提取完整 JPEG 帧
     *
     * MJPEG 是连续的多张 JPEG 图片，每张以 0xFF 0xD8 开头，0xFF 0xD9 结尾。
     * 此函数在输入数据中查找完整的 JPEG 帧边界。
     *
     * @param data       MJPEG 流数据
     * @param len        数据长度
     * @param jpeg_start 输出：JPEG 帧在 data 中的起始偏移
     * @param jpeg_len   输出：JPEG 帧长度
     * @return 0 找到完整帧，-1 未找到
     */
    static int findJPEGFrame(const uint8_t* data, int len,
                             int* jpeg_start, int* jpeg_len);

    /**
     * @brief 检查数据是否以 JPEG SOI 标记 (0xFF 0xD8) 开头
     */
    static bool isJPEGStart(const uint8_t* data, int len);

    // ============================================================
    // 颜色空间转换（BT.601 定点运算）
    // ============================================================

    /**
     * @brief YUYV 4:2:2 → RGB24
     *
     * 转换公式 (BT.601):
     *   R = Y + 1.402   * (V - 128)
     *   G = Y - 0.34414 * (U - 128) - 0.71414 * (V - 128)
     *   B = Y + 1.772   * (U - 128)
     *
     * 使用定点运算: 1.402 ≈ 359/256, 0.344 ≈ 88/256, 0.714 ≈ 183/256, 1.772 ≈ 454/256
     *
     * @param yuyv  输入 YUYV 数据
     * @param rgb   输出 RGB24 数据 (w*h*3 字节，由调用者分配)
     * @param w     图像宽度
     * @param h     图像高度
     */
    static void yuyvToRgb24(const uint8_t* yuyv, uint8_t* rgb, int w, int h);

    /**
     * @brief YUYV 4:2:2 → RGB565（适配 16-bit LCD framebuffer）
     *
     * 输出为小端字节序，每像素 2 字节:
     *   bits 15-11: R (5), bits 10-5: G (6), bits 4-0: B (5)
     *
     * @param yuyv   输入 YUYV 数据
     * @param rgb565 输出 RGB565 数据 (w*h*2 字节)
     * @param w      图像宽度
     * @param h      图像高度
     */
    static void yuyvToRgb565(const uint8_t* yuyv, uint8_t* rgb565, int w, int h);

    /**
     * @brief 将单个 YUYV 宏像素 (4字节) 转换为 RGB24 (6字节)
     * @param yuyv  [y0, u, y1, v]
     * @param rgb   [r0,g0,b0, r1,g1,b1]
     */
    static void yuyvMacroPixelToRgb24(const uint8_t yuyv[4], uint8_t rgb[6]);

    // ============================================================
    // JPEG 编码（使用 libjpeg-turbo）
    // ============================================================

    /**
     * @brief RGB24 → JPEG 编码
     *
     * 编译时需 link libjpeg (-ljpeg)。
     * 若未定义 HAS_LIBJPEG，返回 -1。
     *
     * @param rgb       RGB24 像素数据 (w*h*3 字节)
     * @param width     图像宽度
     * @param height    图像高度
     * @param quality   JPEG 质量 (1-100)，推荐 80
     * @param jpeg_out  输出：编码后的 JPEG 数据（自动分配，调用者 free）
     * @param jpeg_len  输出：JPEG 数据长度
     * @return 0 成功，-1 未编译 libjpeg 支持，-2 编码失败
     */
    static int encodeRGBtoJPEG(const uint8_t* rgb, int width, int height,
                               int quality, uint8_t** jpeg_out,
                               unsigned long* jpeg_len);

    /**
     * @brief YUYV → RGB24 → JPEG 编码（一步完成）
     * @return 0 成功，负值错误码
     */
    static int encodeYUYVtoJPEG(const uint8_t* yuyv, int width, int height,
                                int quality, uint8_t** jpeg_out,
                                unsigned long* jpeg_len);

    // ============================================================
    // 工具方法
    // ============================================================

    /**
     * @brief 计算 RGB24 图像所需缓冲区大小
     */
    static inline int rgb24BufferSize(int w, int h) { return w * h * 3; }

    /**
     * @brief 计算 RGB565 图像所需缓冲区大小
     */
    static inline int rgb565BufferSize(int w, int h) { return w * h * 2; }

    /**
     * @brief 计算 YUYV 图像所需缓冲区大小
     * YUYV 每像素 2 字节，但必须是偶数宽度
     */
    static inline int yuyvBufferSize(int w, int h) {
        return ((w + 1) & ~1) * h * 2;
    }

private:
    VideoProcessor() = delete;  // 纯静态类，禁止实例化
};

#endif // SMART_CAM_CAMERA_PROCESSOR_H
