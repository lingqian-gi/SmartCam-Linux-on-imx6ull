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

// V4L2 相关头文件
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
        stopCapture();
    }

    unmapBuffers();

    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
        LOG_INF("Camera device closed");
    }
}

// ============================================================
// 设备查询
// ============================================================

std::string CameraCapture::getDriverInfo() const {
    if (m_fd < 0) return "Not initialized";

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(m_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        return "QUERYCAP failed";
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "Driver: %s | Card: %s | Bus: %s | Version: %u.%u.%u",
             reinterpret_cast<char*>(cap.driver),
             reinterpret_cast<char*>(cap.card),
             reinterpret_cast<char*>(cap.bus_info),
             (cap.version >> 16) & 0xFF,
             (cap.version >> 8) & 0xFF,
             cap.version & 0xFF);
    return std::string(buf);
}

int CameraCapture::enumFormats(std::vector<uint32_t>& formats) {
    if (m_fd < 0) return -ENODEV;

    formats.clear();
    struct v4l2_fmtdesc fmtdesc;
    memset(&fmtdesc, 0, sizeof(fmtdesc));
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for (int i = 0;; ++i) {
        fmtdesc.index = i;
        if (ioctl(m_fd, VIDIOC_ENUM_FMT, &fmtdesc) < 0) {
            break;  // 枚举结束
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

int CameraCapture::enumFrameSizes(uint32_t pixfmt,
                                  std::vector<std::pair<int,int>>& resolutions) {
    if (m_fd < 0) return -ENODEV;

    resolutions.clear();
    struct v4l2_frmsizeenum frmsize;
    memset(&frmsize, 0, sizeof(frmsize));
    frmsize.pixel_format = pixfmt;

    for (int i = 0;; ++i) {
        frmsize.index = i;
        if (ioctl(m_fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) < 0) {
            break;
        }

        if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            resolutions.emplace_back(frmsize.discrete.width,
                                     frmsize.discrete.height);
            LOG_DBG("  Size: %dx%d", frmsize.discrete.width, frmsize.discrete.height);
        } else if (frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
            // 步进范围，简单起见只列出最小值
            resolutions.emplace_back(frmsize.stepwise.min_width,
                                     frmsize.stepwise.min_height);
            resolutions.emplace_back(frmsize.stepwise.max_width,
                                     frmsize.stepwise.max_height);
            break;
        } else {
            break;
        }
    }

    return resolutions.empty() ? -ENOENT : 0;
}

// ============================================================
// 格式 & 参数设置
// ============================================================

int CameraCapture::setFormat(int width, int height, uint32_t pixfmt) {
    if (m_fd < 0) return -ENODEV;
    if (m_streaming) {
        LOG_WRN("setFormat called while streaming, stop first");
        return -EBUSY;
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

    // 读取实际设置的值（驱动可能调整了分辨率）
    m_width  = static_cast<int>(fmt.fmt.pix.width);
    m_height = static_cast<int>(fmt.fmt.pix.height);
    m_pixfmt = fmt.fmt.pix.pixelformat;

    // 检查 bytesperline，确认无 padding 问题
    LOG_INF("Format set: %dx%d, fmt='%c%c%c%c', stride=%d",
             m_width, m_height,
             (m_pixfmt >> 0) & 0xFF, (m_pixfmt >> 8) & 0xFF,
             (m_pixfmt >> 16) & 0xFF, (m_pixfmt >> 24) & 0xFF,
             fmt.fmt.pix.bytesperline);

    return 0;
}

int CameraCapture::setFramerate(int numerator, int denominator) {
    if (m_fd < 0) return -ENODEV;

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = static_cast<__u32>(numerator);
    parm.parm.capture.timeperframe.denominator = static_cast<__u32>(denominator);

    if (ioctl(m_fd, VIDIOC_S_PARM, &parm) < 0) {
        LOG_WRN("VIDIOC_S_PARM (fps) not supported: %s", strerror(errno));
        return -errno;
    }

    LOG_INF("Framerate set: %d/%d fps",
             parm.parm.capture.timeperframe.denominator,
             parm.parm.capture.timeperframe.numerator);
    return 0;
}

int CameraCapture::getFramerate(int& numerator, int& denominator) {
    if (m_fd < 0) return -ENODEV;

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(m_fd, VIDIOC_G_PARM, &parm) < 0) {
        LOG_WRN("VIDIOC_G_PARM failed: %s", strerror(errno));
        return -errno;
    }

    numerator   = static_cast<int>(parm.parm.capture.timeperframe.numerator);
    denominator = static_cast<int>(parm.parm.capture.timeperframe.denominator);
    return 0;
}

int CameraCapture::enumFrameRates(uint32_t pixfmt, int width, int height,
                                   std::vector<int>& frameRates) {
    if (m_fd < 0) return -ENODEV;

    frameRates.clear();
    struct v4l2_frmivalenum frmival;
    memset(&frmival, 0, sizeof(frmival));
    frmival.pixel_format = pixfmt;
    frmival.width        = static_cast<__u32>(width);
    frmival.height       = static_cast<__u32>(height);

    for (int i = 0;; ++i) {
        frmival.index = static_cast<__u32>(i);
        if (ioctl(m_fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) < 0) {
            break;  // 枚举结束
        }

        if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            int fps = static_cast<int>(frmival.discrete.denominator) /
                      static_cast<int>(frmival.discrete.numerator);
            if (fps > 0) {
                frameRates.push_back(fps);
                LOG_DBG("  FrameInterval[%d]: %d/%d = %d fps",
                         i, frmival.discrete.numerator,
                         frmival.discrete.denominator, fps);
            }
        } else if (frmival.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
            // 步进式帧间隔：stepwise.min = 最短帧间隔(最高fps),
            //              stepwise.max = 最长帧间隔(最低fps)
            int highFps = static_cast<int>(frmival.stepwise.min.denominator) /
                          static_cast<int>(frmival.stepwise.min.numerator);
            int lowFps  = static_cast<int>(frmival.stepwise.max.denominator) /
                          static_cast<int>(frmival.stepwise.max.numerator);
            int stepFps = static_cast<int>(frmival.stepwise.step.denominator) /
                          static_cast<int>(frmival.stepwise.step.numerator);
            if (stepFps <= 0) stepFps = 1;
            if (lowFps <= 0)  lowFps  = 1;
            if (highFps <= 0) highFps = 1;
            // 确保 lowFps <= highFps
            if (lowFps > highFps) std::swap(lowFps, highFps);

            // 限制最多 20 个离散值，避免过多
            int count = 0;
            for (int f = lowFps; f <= highFps && count < 20; f += stepFps) {
                if (f > 0) {
                    frameRates.push_back(f);
                    count++;
                }
            }
            LOG_DBG("  FrameInterval stepwise: low=%d high=%d step=%d fps",
                     lowFps, highFps, stepFps);
            break;
        } else {
            break;
        }
    }

    // 如果设备不支持枚举帧率，返回通用范围 1~120
    if (frameRates.empty()) {
        LOG_INF("No frame intervals enumerated, using default range 1-120");
        for (int f = 1; f <= 120; f += 1) {
            frameRates.push_back(f);
        }
        return -ENOENT;
    }

    // 排序（升序）
    std::sort(frameRates.begin(), frameRates.end());
    // 去重
    frameRates.erase(std::unique(frameRates.begin(), frameRates.end()),
                     frameRates.end());

    return 0;
}

int CameraCapture::setControl(int cid, int value) {
    if (m_fd < 0) return -ENODEV;

    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id    = static_cast<__u32>(cid);
    ctrl.value = static_cast<__s32>(value);

    if (ioctl(m_fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        LOG_ERR_("VIDIOC_S_CTRL (cid=0x%08X, val=%d) failed: %s",
                  cid, value, strerror(errno));
        return -errno;
    }

    char name[32] = "unknown";
    struct v4l2_queryctrl qctrl;
    memset(&qctrl, 0, sizeof(qctrl));
    qctrl.id = static_cast<__u32>(cid);
    if (ioctl(m_fd, VIDIOC_QUERYCTRL, &qctrl) == 0) {
        snprintf(name, sizeof(name), "%s", reinterpret_cast<char*>(qctrl.name));
    }

    LOG_DBG("Control set: %s = %d", name, value);
    return 0;
}

int CameraCapture::getControl(int cid, int& value) {
    if (m_fd < 0) return -ENODEV;

    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = static_cast<__u32>(cid);

    if (ioctl(m_fd, VIDIOC_G_CTRL, &ctrl) < 0) {
        return -errno;
    }

    value = static_cast<int>(ctrl.value);
    return 0;
}

int CameraCapture::queryControl(int cid, int& min, int& max, int& step, int& def) {
    if (m_fd < 0) return -ENODEV;

    struct v4l2_queryctrl qctrl;
    memset(&qctrl, 0, sizeof(qctrl));
    qctrl.id = static_cast<__u32>(cid);

    if (ioctl(m_fd, VIDIOC_QUERYCTRL, &qctrl) < 0) {
        return -errno;
    }

    min  = static_cast<int>(qctrl.minimum);
    max  = static_cast<int>(qctrl.maximum);
    step = static_cast<int>(qctrl.step);
    def  = static_cast<int>(qctrl.default_value);

    LOG_DBG("Control '%s': range=[%d, %d], step=%d, default=%d",
              reinterpret_cast<char*>(qctrl.name), min, max, step, def);
    return 0;
}

// ============================================================
// 采集控制
// ============================================================

int CameraCapture::startCapture() {
    if (m_fd < 0) return -ENODEV;
    if (m_streaming) {
        LOG_WRN("Already streaming");
        return 0;
    }

    // 请求缓冲区
    int ret = requestBuffers(kDefaultBufferCount);
    if (ret < 0) return ret;

    // mmap 映射
    ret = mapBuffers();
    if (ret < 0) return ret;

    // 所有缓冲区入队
    ret = queueAllBuffers();
    if (ret < 0) {
        unmapBuffers();
        return ret;
    }

    // 开始流
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

int CameraCapture::stopCapture() {
    if (!m_streaming || m_fd < 0) return 0;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(m_fd, VIDIOC_STREAMOFF, &type) < 0) {
        LOG_WRN("VIDIOC_STREAMOFF failed: %s", strerror(errno));
    }

    m_streaming = false;
    unmapBuffers();

    LOG_INF("Capture stopped");
    return 0;
}

int CameraCapture::getFrame(FrameBuffer* buf, int timeout_ms) {
    if (!m_streaming || m_fd < 0) return -EIO;

    struct v4l2_buffer vbuf;
    memset(&vbuf, 0, sizeof(vbuf));

    int ret = dequeueBuffer(vbuf, timeout_ms);
    if (ret < 0) return ret;

    // 校验缓冲区索引
    if (vbuf.index >= static_cast<__u32>(m_nbuffers) || !m_buffers) {
        LOG_ERR_("Invalid buffer index: %u (nbufs=%d)", vbuf.index, m_nbuffers);
        return -EINVAL;
    }

    // 填充 FrameBuffer
    buf->data     = static_cast<uint8_t*>(m_buffers[vbuf.index].start);
    buf->length   = static_cast<int>(vbuf.bytesused);
    buf->width    = m_width;
    buf->height   = m_height;
    buf->format   = (m_pixfmt == V4L2_PIX_FMT_YUYV)
                        ? PixelFormat::FMT_YUYV
                        : PixelFormat::FMT_MJPEG;
    buf->index    = m_frameCount++;
    buf->timestamp = std::chrono::steady_clock::now();

    // FPS 统计
    updateFPS();

    return 0;
}

int CameraCapture::putFrame(const FrameBuffer* buf) {
    if (!m_streaming || m_fd < 0) return -EIO;
    if (!buf || !buf->data) return -EINVAL;

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

    if (ioctl(m_fd, VIDIOC_QBUF, &vbuf) < 0) {
        LOG_ERR_("VIDIOC_QBUF (index=%d) failed: %s", idx, strerror(errno));
        return -errno;
    }

    m_buffers[idx].queued = true;
    return 0;
}

double CameraCapture::getCurrentFPS() const {
    std::lock_guard<std::mutex> lock(m_fpsMtx);
    return m_currentFps;
}

Resolution CameraCapture::getCurrentResolution() const {
    std::lock_guard<std::mutex> lock(m_mtx);
    return {m_width, m_height};
}

// ============================================================
// 内部实现
// ============================================================

int CameraCapture::openDevice(const char* device) {
    m_fd = open(device, O_RDWR | O_NONBLOCK, 0);
    if (m_fd < 0) {
        LOG_ERR_("Cannot open '%s': %s", device, strerror(errno));
        return -errno;
    }

    // 暂时设置为阻塞（后续用 select/poll）
    int flags = fcntl(m_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(m_fd, F_SETFL, flags & ~O_NONBLOCK);
    }

    LOG_INF("Device opened: %s, fd=%d", device, m_fd);
    return 0;
}

int CameraCapture::queryCapability() {
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));

    if (ioctl(m_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        LOG_ERR_("VIDIOC_QUERYCAP failed: %s", strerror(errno));
        return -errno;
    }

    // 检查是否是 video capture 设备
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        LOG_ERR_("Device is not a video capture device");
        return -ENODEV;
    }

    // 检查是否支持 streaming I/O
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        LOG_ERR_("Device does not support streaming I/O");
        return -ENOSYS;
    }

    LOG_INF("Driver: %s | Card: %s | Bus: %s | ver=%u.%u.%u",
             reinterpret_cast<char*>(cap.driver),
             reinterpret_cast<char*>(cap.card),
             reinterpret_cast<char*>(cap.bus_info),
             (cap.version >> 16) & 0xFF,
             (cap.version >> 8) & 0xFF,
             cap.version & 0xFF);

    return 0;
}

int CameraCapture::requestBuffers(int count) {
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = static_cast<__u32>(count);
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(m_fd, VIDIOC_REQBUFS, &req) < 0) {
        LOG_ERR_("VIDIOC_REQBUFS (%d buffers) failed: %s", count, strerror(errno));
        return -errno;
    }

    if (req.count < 2) {
        LOG_ERR_("Insufficient buffer memory: only %u buffers", req.count);
        return -ENOMEM;
    }

    m_nbuffers = static_cast<int>(req.count);
    m_buffers  = new BufferUnit[static_cast<size_t>(m_nbuffers)];
    memset(m_buffers, 0, sizeof(BufferUnit) * static_cast<size_t>(m_nbuffers));

    LOG_INF("Requested %d V4L2 buffers (got %d)", count, m_nbuffers);
    return 0;
}

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

        m_buffers[i].start = mmap(nullptr,
                                   buf.length,
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED,
                                   m_fd,
                                   buf.m.offset);
        if (m_buffers[i].start == MAP_FAILED) {
            LOG_ERR_("mmap[%d] failed: %s (length=%u, offset=%u)",
                      i, strerror(errno), buf.length, buf.m.offset);
            m_buffers[i].start = nullptr;
            return -errno;
        }

        m_buffers[i].length = static_cast<size_t>(buf.length);
        m_buffers[i].index  = i;
        m_buffers[i].queued = false;

        LOG_DBG("  Buffer[%d]: mapped at %p, length=%zu", i,
                  m_buffers[i].start, m_buffers[i].length);
    }

    return 0;
}

int CameraCapture::unmapBuffers() {
    if (!m_buffers) return 0;

    for (int i = 0; i < m_nbuffers; ++i) {
        if (m_buffers[i].start && m_buffers[i].start != MAP_FAILED) {
            munmap(m_buffers[i].start, m_buffers[i].length);
            m_buffers[i].start = nullptr;
        }
    }

    delete[] m_buffers;
    m_buffers  = nullptr;
    m_nbuffers = 0;

    // 释放 V4L2 驱动侧缓冲区资源，否则后续 VIDIOC_S_FMT 会返回 EBUSY
    if (m_fd >= 0) {
        struct v4l2_requestbuffers req;
        memset(&req, 0, sizeof(req));
        req.count  = 0;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (ioctl(m_fd, VIDIOC_REQBUFS, &req) < 0) {
            LOG_WRN("VIDIOC_REQBUFS(0) failed: %s", strerror(errno));
        }
    }

    return 0;
}

int CameraCapture::queueAllBuffers() {
    for (int i = 0; i < m_nbuffers; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = static_cast<__u32>(i);

        if (ioctl(m_fd, VIDIOC_QBUF, &buf) < 0) {
            LOG_ERR_("VIDIOC_QBUF[%d] failed: %s", i, strerror(errno));
            return -errno;
        }
        m_buffers[i].queued = true;
    }

    return 0;
}

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
            return -ETIMEDOUT;  // 超时
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

// ============================================================
// 辅助
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
            m_currentFps = 30.0 / elapsed;
        }
        m_lastFpsTime = now;
    }
}

FrameBuffer CameraCapture::BufferUnit::toFrameBuffer(int w, int h, uint32_t fmt) {
    FrameBuffer fb;
    fb.data   = static_cast<uint8_t*>(start);
    fb.length = static_cast<int>(length);
    fb.width  = w;
    fb.height = h;
    fb.format = (fmt == V4L2_PIX_FMT_YUYV) ? PixelFormat::FMT_YUYV
                                           : PixelFormat::FMT_MJPEG;
    return fb;
}
