#ifndef SMART_CAM_CAMERA_CAPTURE_H
#define SMART_CAM_CAMERA_CAPTURE_H

/**
 * @file    capture.h
 * @brief   V4L2 视频采集引擎
 *
 * 封装 Linux V4L2 API 的完整采集流程，支持 YUYV / MJPEG 双格式。
 * 使用 mmap 零拷贝帧缓冲池（默认 4 个缓冲区轮转）。
 *
 * 典型用法:
 * @code
 *   CameraCapture cap;
 *   cap.init("/dev/video0");
 *   cap.enumFormats(...);            // 可选：枚举设备支持的格式
 *   cap.setFormat(640, 480, V4L2_PIX_FMT_MJPEG);
 *   cap.startCapture();
 *
 *   FrameBuffer fb;
 *   while (running) {
 *       if (cap.getFrame(&fb) == 0) {
 *           // 处理 fb.data, fb.length ...
 *           cap.putFrame(&fb);        // 归还缓冲区到队列
 *       }
 *   }
 *   cap.stopCapture();
 * @endcode
 */

#include <cstdint>
#include <vector>
#include <string>
#include <mutex>

#include "include/common/types.h"

// ============================================================
// V4L2 相关结构前向声明（避免引入 linux/videodev2.h 冲突）
// ============================================================
struct v4l2_capability;
struct v4l2_format;
struct v4l2_buffer;
struct v4l2_queryctrl;

/**
 * @brief V4L2 采集引擎类
 *
 * 非拷贝、非移动。建议作为单例或由主线程管理生命周期。
 */
class CameraCapture {
public:
    CameraCapture();
    ~CameraCapture();

    // 禁用拷贝
    CameraCapture(const CameraCapture&) = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;

    // ============================================================
    // 生命周期管理
    // ============================================================

    /**
     * @brief 打开 V4L2 设备
     * @param device  设备路径，如 "/dev/video0"
     * @return 0 成功，负值错误码
     */
    int init(const char* device = "/dev/video0");

    /**
     * @brief 关闭设备，释放所有资源
     */
    void release();

    // ============================================================
    // 设备查询
    // ============================================================

    /**
     * @brief 获取设备驱动信息
     */
    std::string getDriverInfo() const;

    /**
     * @brief 枚举设备支持的像素格式
     * @param formats  输出：支持的格式列表
     * @return 0 成功
     */
    int enumFormats(std::vector<uint32_t>& formats);

    /**
     * @brief 枚举支持的帧尺寸组合
     * @param pixfmt   像素格式（V4L2 FOURCC）
     * @param resolutions  输出：支持的 (width, height) 列表
     * @return 0 成功
     */
    int enumFrameSizes(uint32_t pixfmt, std::vector<std::pair<int,int>>& resolutions);

    // ============================================================
    // 格式 & 参数设置
    // ============================================================

    /**
     * @brief 设置采集格式
     * @param width   图像宽度
     * @param height  图像高度
     * @param pixfmt  V4L2 像素格式 (V4L2_PIX_FMT_YUYV / V4L2_PIX_FMT_MJPEG)
     * @return 0 成功
     */
    int setFormat(int width, int height, uint32_t pixfmt);

    /**
     * @brief 设置帧率
     * @param numerator   分子 (fps = numerator/denominator, 如 30/1 = 30fps)
     * @param denominator 分母
     * @return 0 成功，负值表示设备不支持
     */
    int setFramerate(int numerator, int denominator);

    /**
     * @brief 设置摄像头控制参数
     * @param cid    V4L2 控制 ID，如 V4L2_CID_BRIGHTNESS
     * @param value  控制值
     * @return 0 成功
     */
    int setControl(int cid, int value);

    /**
     * @brief 获取摄像头控制参数
     * @param cid    V4L2 控制 ID
     * @param value  输出：当前值
     * @return 0 成功
     */
    int getControl(int cid, int& value);

    /**
     * @brief 查询控制参数的范围
     * @param cid    V4L2 控制 ID
     * @param min    输出：最小值
     * @param max    输出：最大值
     * @param step   输出：步进
     * @param def    输出：默认值
     * @return 0 成功
     */
    int queryControl(int cid, int& min, int& max, int& step, int& def);

    // ============================================================
    // 采集控制
    // ============================================================

    /**
     * @brief 启动采集流
     * @return 0 成功
     */
    int startCapture();

    /**
     * @brief 停止采集流
     * @return 0 成功
     */
    int stopCapture();

    /**
     * @brief 阻塞获取一帧数据
     *
     * 调用 getFrame 后，必须调用 putFrame 归还缓冲区。
     * FrameBuffer.data 指向 mmap 内存，不应外部释放。
     *
     * @param buf  输出：帧数据（指向内部 mmap 内存）
     * @param timeout_ms  超时毫秒（<=0 表示阻塞等待）
     * @return 0 成功，-ETIMEDOUT 超时，负值错误
     */
    int getFrame(FrameBuffer* buf, int timeout_ms = -1);

    /**
     * @brief 归还缓冲区到 V4L2 队列（必须在 getFrame 后调用）
     * @param buf  要归还的帧（由 getFrame 填充，调用者不修改数据）
     * @return 0 成功
     */
    int putFrame(const FrameBuffer* buf);

    // ============================================================
    // 查询状态
    // ============================================================

    /** @brief 是否正在采集 */
    bool isStreaming() const { return m_streaming; }

    /** @brief 获取当前帧率（最近 N 帧平均） */
    double getCurrentFPS() const;

    /** @brief 获取当前分辨率 */
    Resolution getCurrentResolution() const;

    /** @brief 获取当前格式 */
    uint32_t getCurrentFormat() const { return m_pixfmt; }

    // ============================================================
    // 常量
    // ============================================================

    /** @brief 默认帧缓冲池大小 */
    static constexpr int kDefaultBufferCount = 4;

    /** @brief V4L2 像素格式常量（避免包含 linux/videodev2.h） */
    static constexpr uint32_t V4L2_PIX_FMT_YUYV  = 0x56595559;
    static constexpr uint32_t V4L2_PIX_FMT_MJPEG = 0x47504A4D;
    static constexpr uint32_t V4L2_PIX_FMT_RGB24 = 0x00000001;

private:
    // ============================================================
    // 内部实现
    // ============================================================

    /** @brief V4L2 mmap 缓冲区单元 */
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

    // ---- 设备 fd ----
    int  m_fd              = -1;
    bool m_streaming       = false;

    // ---- 格式 ----
    int  m_width           = 640;
    int  m_height          = 480;
    uint32_t m_pixfmt      = V4L2_PIX_FMT_MJPEG;

    // ---- 帧缓冲池 ----
    BufferUnit* m_buffers  = nullptr;
    int         m_nbuffers = 0;

    // ---- FPS 统计 ----
    mutable std::mutex m_fpsMtx;
    int  m_frameCount      = 0;
    double m_lastFpsTime   = 0.0;
    double m_currentFps    = 30.0;

    // ---- 线程安全 ----
    mutable std::mutex m_mtx;
};

#endif // SMART_CAM_CAMERA_CAPTURE_H
