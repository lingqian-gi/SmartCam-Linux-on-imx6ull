/**
 * @file    processor.cpp
 * @brief   VideoProcessor 工具类实现
 *
 * 提供静态方法用于：
 *   - MJPEG 帧边界查找
 *   - YUYV→RGB24 / RGB565 颜色空间转换（BT.601 定点运算）
 *   - JPEG 编码（libjpeg-turbo）
 */

#include "include/camera/processor.h"
#include "include/common/logger.h"

#include <cstring>
#include <cstdlib>

// libjpeg-turbo — 必须在文件作用域包含（含 extern "C" 包裹）
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

// ============================================================
// 颜色空间转换
// ============================================================

void VideoProcessor::yuyvToRgb24(const uint8_t* yuyv, uint8_t* rgb,
                                  int w, int h) {
    const int pixels = w * h;
    int di = 0;
    for (int i = 0; i < pixels; i += 2) {
        int si = i * 2;
        int y0 = yuyv[si];
        int u  = yuyv[si + 1] - 128;
        int y1 = yuyv[si + 2];
        int v  = yuyv[si + 3] - 128;

        // BT.601 定点运算 → RGB
        auto clip = [](int x) -> uint8_t {
            return static_cast<uint8_t>(x < 0 ? 0 : (x > 255 ? 255 : x));
        };

        int r0 = y0 + ((v * 359) >> 8);
        int g0 = y0 - ((u * 88) >> 8) - ((v * 183) >> 8);
        int b0 = y0 + ((u * 454) >> 8);

        int r1 = y1 + ((v * 359) >> 8);
        int g1 = y1 - ((u * 88) >> 8) - ((v * 183) >> 8);
        int b1 = y1 + ((u * 454) >> 8);

        rgb[di++] = clip(r0);
        rgb[di++] = clip(g0);
        rgb[di++] = clip(b0);
        rgb[di++] = clip(r1);
        rgb[di++] = clip(g1);
        rgb[di++] = clip(b1);
    }
}

void VideoProcessor::yuyvToRgb565(const uint8_t* yuyv, uint8_t* rgb565,
                                   int w, int h) {
    const int pixels = w * h;
    int di = 0;
    for (int i = 0; i < pixels; i += 2) {
        int si = i * 2;
        int y0 = yuyv[si];
        int u  = yuyv[si + 1] - 128;
        int y1 = yuyv[si + 2];
        int v  = yuyv[si + 3] - 128;

        auto clip = [](int x) -> int { return x < 0 ? 0 : (x > 255 ? 255 : x); };

        auto to565 = [&](int y, int u2, int v2) -> uint16_t {
            int r = y + ((v2 * 359) >> 8);
            int g = y - ((u2 * 88) >> 8) - ((v2 * 183) >> 8);
            int b = y + ((u2 * 454) >> 8);
            return static_cast<uint16_t>(
                ((clip(r) & 0xF8) << 8) |
                ((clip(g) & 0xFC) << 3) |
                ((clip(b) & 0xF8) >> 3));
        };

        uint16_t p0 = to565(y0, u, v);
        uint16_t p1 = to565(y1, u, v);
        rgb565[di++] =  p0        & 0xFF;
        rgb565[di++] = (p0 >> 8)  & 0xFF;
        rgb565[di++] =  p1        & 0xFF;
        rgb565[di++] = (p1 >> 8)  & 0xFF;
    }
}

void VideoProcessor::yuyvMacroPixelToRgb24(const uint8_t yuyv[4],
                                            uint8_t rgb[6]) {
    int y0 = yuyv[0];
    int u  = yuyv[1] - 128;
    int y1 = yuyv[2];
    int v  = yuyv[3] - 128;

    auto clip = [](int x) -> uint8_t {
        return static_cast<uint8_t>(x < 0 ? 0 : (x > 255 ? 255 : x));
    };

    rgb[0] = clip(y0 + ((v * 359) >> 8));
    rgb[1] = clip(y0 - ((u * 88) >> 8) - ((v * 183) >> 8));
    rgb[2] = clip(y0 + ((u * 454) >> 8));
    rgb[3] = clip(y1 + ((v * 359) >> 8));
    rgb[4] = clip(y1 - ((u * 88) >> 8) - ((v * 183) >> 8));
    rgb[5] = clip(y1 + ((u * 454) >> 8));
}

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
    jpeg_mem_dest(&cinfo, &buffer, &outlen);

    cinfo.image_width      = static_cast<JDIMENSION>(width);
    cinfo.image_height     = static_cast<JDIMENSION>(height);
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    // 逐行写入——ARM NEON 友好
    JSAMPROW row_pointer[1];
    int row_stride = width * 3;
    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = const_cast<JSAMPROW>(
            &rgb[cinfo.next_scanline * row_stride]);
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    // 转移所有权给调用者
    *jpeg_out = buffer;   // malloc'd 内存，调用者需 free
    *jpeg_len = static_cast<int>(outlen);

    LOG_DBG("JPEG encoded: %dx%d quality=%d → %lu bytes",
              width, height, quality, outlen);
    return 0;

#else
    (void)rgb;
    (void)width;
    (void)height;
    (void)quality;
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
