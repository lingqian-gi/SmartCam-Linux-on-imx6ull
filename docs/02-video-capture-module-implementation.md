# 视频采集模块 — 实现记录

> **编号**：MOD-02
> **创建日期**：2026-05-22
> **状态**：✅ 已实现，编译通过
> **依赖**：V4L2 (Linux 内核)、C++17、Qt5 Widgets、pthread

---

## 一、模块概述

基于文档 [3.1 视频采集模块](../求职项目-智能相机流媒体系统.md) 和 [3.2 视频处理模块](../求职项目-智能相机流媒体系统.md) 的设计，实现完整的 V4L2 视频采集引擎和图像处理工具类，适配野火 iMX6ULL Pro + USB 摄像头 (YUV/MJPEG)。

### 本模块在项目中的位置

```
SmartCam 四线程架构
                    ┌──────────────┐
                    │   main.cpp   │
                    │  (主线程)     │
                    └──────┬───────┘
                           │
          ┌────────────────┼────────────────┐
          │                │                │
          ▼                ▼                ▼
   ┌────────────┐  ┌────────────┐  ┌──────────────┐
   │ 采集线程    │  │ 显示线程    │  │ 控制/传输线程  │
   │ (Capture)  │  │ (GUI)      │  │ (未来模块)    │
   └─────┬──────┘  └─────┬──────┘  └──────────────┘
         │               │
         │  拷贝帧 →      │
         └───────────────┘
```

### 核心设计理念

- **V4L2 mmap 零拷贝**：采集到的帧数据通过 mmap 直接映射到用户空间，避免内核态→用户态的数据拷贝
- **YUV/MJPEG 双模式**：本地预览用 YUYV（低延迟），网络串流用 MJPEG（已压缩，零 CPU 编码开销）
- **采集线程分离**：阻塞式 DQBUF 放在独立线程中，不阻塞 Qt 主事件循环
- **帧数据拷贝**：V4L2 mmap 内存必须在归还队列前拷贝出来，因此采集线程将帧数据拷贝到共享缓冲区，GUI 定时器读取

### 功能清单

| 功能 | 状态 |
|------|------|
| V4L2 设备打开/关闭 (VIDIOC_QUERYCAP) | ✅ |
| mmap 帧缓冲池 (默认 4 缓冲区轮转) | ✅ |
| YUYV 4:2:2 格式采集 (V4L2_PIX_FMT_YUYV) | ✅ |
| MJPEG 格式采集 (V4L2_PIX_FMT_MJPEG) | ✅ |
| 枚举支持格式 (VIDIOC_ENUM_FMT) | ✅ |
| 枚举支持分辨率 (VIDIOC_ENUM_FRAMESIZES) | ✅ |
| 摄像头控制参数 (亮度/对比度/白平衡) | ✅ |
| 帧率设置 (VIDIOC_S_PARM) | ✅ |
| select 超时机制 (避免 DQBUF 永久阻塞) | ✅ |
| 实时 FPS 统计 (每 30 帧滚动平均) | ✅ |
| YUYV → RGB24 颜色空间转换 (BT.601 定点) | ✅ |
| YUYV → RGB565 颜色空间转换 (16-bit LCD) | ✅ |
| MJPEG 帧边界查找 (SOI/EOI 标记定位) | ✅ |
| JPEG 编码 (libjpeg-turbo, 可选) | ✅ 条件编译 |
| 线程安全日志系统 (彩色控制台 + syslog) | ✅ |
| 环形缓冲区模板 (pushOverwrite 覆盖策略) | ✅ |
| 采集线程 → GUI 显示集成 | ✅ |
| Mock 模式 (无硬件可运行) | ✅ 此前模块 |

---

## 二、文件清单

### 2.1 新建文件（6 个）

```
SmartCam-Linux-on-imx6ull/
├── include/
│   ├── camera/
│   │   ├── capture.h              # CameraCapture 类声明 (~130 行)
│   │   └── processor.h            # VideoProcessor 类声明 (~100 行)
│   └── common/
│       ├── logger.h               # 日志系统 (~200 行, header-only)
│       └── ringbuf.h              # 环形缓冲区模板 (~110 行, header-only)
├── src/
│   ├── camera/
│   │   ├── capture.cpp            # V4L2 采集实现 (~420 行)
│   │   └── processor.cpp          # 图像处理实现 (~170 行)
└── docs/
    └── 02-video-capture-module-implementation.md  # 本文档
```

### 2.2 修改文件（2 个）

```
SmartCam-Linux-on-imx6ull/
├── CMakeLists.txt                 # 添加 camera 源文件 + pthread 链接
└── src/main.cpp                   # 集成 CameraCapture 采集线程 + GUI 显示
```

### 2.3 完整规模统计

| 模块 | 头文件 | 源文件 | 合计行数 |
|------|--------|--------|----------|
| CameraCapture | capture.h ~130 | capture.cpp ~420 | ~550 |
| VideoProcessor | processor.h ~100 | processor.cpp ~170 | ~270 |
| Logger (header-only) | logger.h ~200 | - | ~200 |
| RingBuffer (header-only) | ringbuf.h ~110 | - | ~110 |
| **相机模块总规模** | **~540** | **~590** | **~1130** |

---

## 三、关键设计决策

### 3.1 为何使用 `getFrame()` + `putFrame()` 配对模式而不是回调？

许多 V4L2 示例采用在采集循环中直接处理帧（如 `process_frame()` 回调），但这种方式耦合度高。本模块采用显式的 get/put 配对：

```
采集线程:
  while (running) {
      getFrame(&fb)      // 阻塞取帧 → 返回 mmap 指针
      // ... 拷贝 fb.data 到共享缓冲区 ...
      putFrame(&fb)      // 归还缓冲区到 V4L2 队列
  }
```

**优点**：
- 调用者自行决定何时拷贝、拷贝到哪里
- 可以方便地将帧数据分发到多个消费者（GUI + 网络）
- 帧数据的生命周期由调用者掌控，避免回调中的竞态条件

### 3.2 为何不用 `select()` 超时替代 `O_NONBLOCK` + 忙等？

V4L2 DQBUF 在阻塞模式下可能永久阻塞（摄像头断开等异常）。本模块在 `dequeueBuffer()` 中使用 `select()` 实现可超时的阻塞：

```cpp
// capture.cpp dequeueBuffer():
fd_set fds;
FD_SET(m_fd, &fds);
struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
int ret = select(m_fd + 1, &fds, nullptr, nullptr, &tv);
if (ret == 0) return -ETIMEDOUT;  // 超时 → 采集线程可检查 running 标志
```

这样既保持了阻塞模式的高效（有数据时立即返回），又保证了线程可在超时后优雅退出。

### 3.3 为何帧率统计使用 30 帧滚动平均而非瞬时值？

`updateFPS()` 每 30 帧计算一次滚动平均：

```cpp
if (m_frameCount % 30 == 0) {
    double elapsed = now - m_lastFpsTime;
    if (elapsed > 0.0) m_currentFps = 30.0 / elapsed;
    m_lastFpsTime = now;
}
```

瞬时帧率（每相邻两帧的时间差倒数）噪声大，单核 Cortex-A7 上调度抖动可达 5-10ms。30 帧滚动平均能平滑显示稳定帧率，适合给 GUI 状态栏显示。

### 3.4 日志宏为何命名为 `LOG_DBG` / `LOG_INF` / `LOG_WRN` / `LOG_ERR_`？

Linux `syslog.h` 定义了名为 `LOG_DEBUG`、`LOG_INFO`、`LOG_ERR` 等整数常量宏。我们的日志宏如果也命名为 `LOG_DEBUG()` 会与 syslog 冲突导致重定义警告。

| syslog 常量 (冲突) | 本模块宏 (无冲突) |
|---------------------|-------------------|
| `LOG_DEBUG` (int:7) | `LOG_DBG` |
| `LOG_INFO` (int:6)  | `LOG_INF` |
| `LOG_WARNING` (int:4) | `LOG_WRN` |
| `LOG_ERR` (int:3)   | `LOG_ERR_` |

同时在 logger.h 末尾用 `#undef` 取消 syslog 的宏定义，避免后续代码误用。

### 3.5 JPEG 编码为何使用条件编译？

iMX6ULL 上 MJPEG 模式零 CPU 开销是最优选，JPEG 软编码（YUV→JPEG 通过 libjpeg-turbo）仅在以下场景需要：

1. YUV 模式下需要网络传输
2. 拍照保存（从 YUYV 帧写入 JPEG 文件）

因此 `VideoProcessor::encodeRGBtoJPEG()` 使用 `#ifdef HAS_LIBJPEG` 条件编译，默认不启用。在 CMakeLists.txt 中取消注释相应行即可开启：

```cmake
find_package(JPEG QUIET)
if(JPEG_FOUND)
    target_compile_definitions(smartcam PRIVATE HAS_LIBJPEG)
    target_link_libraries(smartcam PRIVATE JPEG::JPEG)
endif()
```

### 3.6 为何采集线程拷贝帧数据到共享缓冲区？

V4L2 mmap 映射的内存由内核管理，当调用 `putFrame()`（`VIDIOC_QBUF`）后，该缓冲区会被内核重新填充下一帧数据。因此必须在 `putFrame()` **之前**将数据拷贝出来。

```
正确的生命周期:
  getFrame → 得到 mmap 指针 → [拷贝数据到安全内存] → putFrame
                                                     ↑
                                        拷贝必须在此前完成

错误的生命周期:
  getFrame → 得到 mmap 指针 → putFrame → 后续再读数据
                                        ↑
                            数据已被新帧覆盖！
```

本模块的实现中，采集线程完成拷贝后立即 `putFrame()`，最大限度减少缓冲区在用户态的滞留时间。

---

## 四、类接口文档

### 4.1 CameraCapture (include/camera/capture.h)

```cpp
class CameraCapture {
public:
    CameraCapture();
    ~CameraCapture();

    // ---- 生命周期 ----
    int init(const char* device = "/dev/video0");
    void release();

    // ---- 设备查询 ----
    std::string getDriverInfo() const;
    int enumFormats(std::vector<uint32_t>& formats);
    int enumFrameSizes(uint32_t pixfmt, std::vector<std::pair<int,int>>& resolutions);

    // ---- 格式 & 参数 ----
    int setFormat(int width, int height, uint32_t pixfmt);      // YUYV / MJPEG
    int setFramerate(int numerator, int denominator);
    int setControl(int cid, int value);          // 亮度/对比度等
    int getControl(int cid, int& value);
    int queryControl(int cid, int& min, int& max, int& step, int& def);

    // ---- 采集控制 ----
    int startCapture();
    int stopCapture();
    int getFrame(FrameBuffer* buf, int timeout_ms = -1);   // 阻塞取帧
    int putFrame(const FrameBuffer* buf);                   // 归还缓冲区

    // ---- 状态查询 ----
    bool isStreaming() const;
    double getCurrentFPS() const;
    Resolution getCurrentResolution() const;
    uint32_t getCurrentFormat() const;

    // ---- 常量 ----
    static constexpr int kDefaultBufferCount = 4;
    static constexpr uint32_t V4L2_PIX_FMT_YUYV  = 0x56595559;
    static constexpr uint32_t V4L2_PIX_FMT_MJPEG = 0x47504A4D;
};
```

**典型用法：**

```cpp
CameraCapture cap;
cap.init("/dev/video0");
cap.enumFormats(formats);            // 可选：枚举设备支持的格式
cap.setFormat(640, 480, CameraCapture::V4L2_PIX_FMT_MJPEG);
cap.startCapture();

FrameBuffer fb;
while (running) {
    if (cap.getFrame(&fb) == 0) {
        // 处理 fb.data, fb.length ...
        // 必须在 putFrame 前完成数据拷贝
        cap.putFrame(&fb);
    }
}
cap.stopCapture();
```

### 4.2 VideoProcessor (include/camera/processor.h)

```cpp
class VideoProcessor {
public:
    // ---- MJPEG 帧解析 ----
    static int  findJPEGFrame(const uint8_t* data, int len,
                              int* jpeg_start, int* jpeg_len);
    static bool isJPEGStart(const uint8_t* data, int len);

    // ---- 颜色空间转换 ----
    static void yuyvToRgb24(const uint8_t* yuyv, uint8_t* rgb, int w, int h);
    static void yuyvToRgb565(const uint8_t* yuyv, uint8_t* rgb565, int w, int h);
    static void yuyvMacroPixelToRgb24(const uint8_t yuyv[4], uint8_t rgb[6]);

    // ---- JPEG 编码 (需 HAS_LIBJPEG) ----
    static int encodeRGBtoJPEG(const uint8_t* rgb, int width, int height,
                               int quality, uint8_t** jpeg_out, unsigned long* jpeg_len);
    static int encodeYUYVtoJPEG(const uint8_t* yuyv, int width, int height,
                                int quality, uint8_t** jpeg_out, unsigned long* jpeg_len);

    // ---- 工具方法 ----
    static inline int rgb24BufferSize(int w, int h)   { return w * h * 3; }
    static inline int rgb565BufferSize(int w, int h)  { return w * h * 2; }
    static inline int yuyvBufferSize(int w, int h)    { return ((w + 1) & ~1) * h * 2; }
};
```

### 4.3 Logger (include/common/logger.h, header-only)

```cpp
// 使用便捷宏（推荐）
LOG_DBG("format=%dx%d", w, h);
LOG_INF("Camera initialized, device=%s", device);
LOG_WRN("setFormat called while streaming");
LOG_ERR_("VIDIOC_DQBUF failed: %s", strerror(errno));

// Logger 单例配置
Logger::instance()->setLevel(LogLevel::DEBUG);     // 日志级别
Logger::instance()->setSyslogEnabled(true);         // syslog 输出
Logger::instance()->setTimestampEnabled(false);     // 关闭时间戳
```

**输出示例：**
```
22:15:33 [INFO] capture.cpp:138 (setFormat) Format set: 640x480, fmt='YUYV', stride=1280
22:15:33 [INFO] capture.cpp:318 (startCapture) Capture started: 640x480, 4 buffers
22:15:33 [DEBG] capture.cpp:278 (mapBuffers)   Buffer[0]: mapped at 0xb6f5a000, length=614400
```

### 4.4 RingBuffer (include/common/ringbuf.h, header-only)

```cpp
template<typename T>
class RingBuffer {
public:
    explicit RingBuffer(int capacity);
    ~RingBuffer();

    bool push(const T& item);           // 入队（满时返回 false）
    bool pop(T& item);                  // 出队（空时返回 false）
    bool pushOverwrite(const T& item);  // 入队，满则覆盖最旧数据
    bool peek(T& item) const;           // 查看队头（不移除）
    void clear();                       // 清空

    int  size()     const;
    int  capacity() const;
    bool empty()    const;
    bool full()     const;
};
```

**覆盖策略说明：**

`pushOverwrite()` 在队列满时，将新数据直接写入 tail 位置，然后将 head 前移一位（"头追尾"），丢弃最旧的数据。这一策略是流媒体应用的关键——网络慢的客户端收到的是最新画面，而非排队滞后的旧帧。

---

## 五、V4L2 采集完整流程

```
                 ┌─────────────────────────────────┐
                 │    CameraCapture::init("/dev/video0")  │
                 └────────────────┬────────────────┘
                                  │
                                  ▼
          ┌──────────────────────────────────────────┐
          │  1. open("/dev/video0", O_RDWR)          │
          │     → 获取文件描述符 m_fd                │
          └────────────────┬─────────────────────────┘
                           │
                           ▼
          ┌──────────────────────────────────────────┐
          │  2. IOCTL: VIDIOC_QUERYCAP               │
          │     → 验证 V4L2_CAP_VIDEO_CAPTURE        │
          │     → 验证 V4L2_CAP_STREAMING            │
          │     → 读取 driver/card/bus_info          │
          └────────────────┬─────────────────────────┘
                           │
                           ▼
          ┌──────────────────────────────────────────┐
          │  3. CameraCapture::setFormat(w, h, fmt)  │
          │     → IOCTL: VIDIOC_S_FMT                │
          │     → 驱动可能调整分辨率，读取实际值      │
          └────────────────┬─────────────────────────┘
                           │
                           ▼
          ┌──────────────────────────────────────────┐
          │  4. CameraCapture::startCapture()        │
          │                                          │
          │  a. IOCTL: VIDIOC_REQBUFS (count=4)      │
          │     → 请求 4 个内核缓冲区                │
          │                                          │
          │  b. 循环: VIDIOC_QUERYBUF + mmap         │
          │     → 将内核缓冲区映射到用户空间          │
          │                                          │
          │  c. 循环: VIDIOC_QBUF                    │
          │     → 所有缓冲区入队                      │
          │                                          │
          │  d. IOCTL: VIDIOC_STREAMON               │
          │     → 启动数据流                         │
          └────────────────┬─────────────────────────┘
                           │
                           ▼
          ┌──────────────────────────────────────────┐
          │  5. 采集循环 (独立线程)                  │
          │                                          │
          │  while (running) {                       │
          │    a. select(fd, timeout=1000ms)         │
          │       → 等数据就绪或超时                 │
          │                                          │
          │    b. IOCTL: VIDIOC_DQBUF                │
          │       → 取出一帧 (mmap 指针)             │
          │       → 填充 FrameBuffer                 │
          │                                          │
          │    c. 拷贝帧数据到共享缓冲区              │
          │       (必须在 d 之前完成)                 │
          │                                          │
          │    d. IOCTL: VIDIOC_QBUF                 │
          │       → 归还缓冲区到队列                  │
          │  }                                       │
          └────────────────┬─────────────────────────┘
                           │
                           ▼
          ┌──────────────────────────────────────────┐
          │  6. CameraCapture::stopCapture()         │
          │     → IOCTL: VIDIOC_STREAMOFF            │
          │     → munmap 所有缓冲区                  │
          │     → delete[] m_buffers                 │
          └────────────────┬─────────────────────────┘
                           │
                           ▼
          ┌──────────────────────────────────────────┐
          │  7. CameraCapture::release()             │
          │     → close(m_fd)                        │
          └──────────────────────────────────────────┘
```

### 5.1 数据结构：V4L2 mmap 缓冲池

```cpp
struct BufferUnit {
    void*  start;     // mmap 映射地址（指向内核 DMA 缓冲区）
    size_t length;    // 映射长度
    int    index;     // 索引 (0 ~ n_buffers-1)
    bool   queued;    // 是否在 V4L2 队列中
};
```

4 个缓冲区的轮转示意图：

```
采集线程:  DQBUF(0) → 处理(0) → QBUF(0) → DQBUF(1) → 处理(1) → QBUF(1) ...
             │                    │          │                    │
  mmap内存:  buf[0]  ← 数据中    buf[1]    buf[2]    ← 数据中   buf[3]
            └──────── 正在处理 ────┘        └──────── 正在处理 ────┘
             (用户态)                        (用户态)
                      ┌───────────────────────┐
                      │  内核 UVCH 驱动        │
                      │  ┌─buf[0]─┐ ┌─buf[1]─┐ │
                      │  └────────┘ └────────┘ │
                      │  ┌─buf[2]─┐ ┌─buf[3]─┐ │
                      │  └────────┘ └────────┘ │
                      └───────────────────────┘
```

---

## 六、颜色空间转换原理

### 6.1 YUYV 4:2:2 打包格式

```
每个宏像素 = 4 字节 = 2 个像素:
  Byte:   [0]    [1]    [2]    [3]
         Y0      U      Y1      V
         ↑            ↑
       像素0亮度    像素1亮度
         └──── 共享 UV ────┘

实际像素排列 (640x480):
  Y0  U0  Y1  V0   Y2  U1  Y3  V1   Y4  U2  Y5  V2 ...
  └──── 像素0+1 ──┘ └──── 像素2+3 ──┘ └──── 像素4+5 ──┘
```

### 6.2 BT.601 转换公式（定点运算）

```
标准 BT.601 (浮点):
  R = Y + 1.402   * (V - 128)
  G = Y - 0.34414 * (U - 128) - 0.71414 * (V - 128)
  B = Y + 1.772   * (U - 128)

本模块定点运算 (避免浮点, ARM NEON 友好):
  R = Y + (V-128) * 359 >> 8    (1.402 ≈ 359/256)
  G = Y - (U-128) * 88  >> 8    (0.344 ≈  88/256)
       - (V-128) * 183 >> 8     (0.714 ≈ 183/256)
  B = Y + (U-128) * 454 >> 8    (1.772 ≈ 454/256)
```

### 6.3 性能估算（iMX6ULL Cortex-A7 @ 792MHz）

| 操作 | 640x480 | 1280x720 | 备注 |
|------|---------|----------|------|
| YUYV→RGB24 | ~5ms | ~12ms | 纯 CPU, 2 次乘加/像素 |
| YUYV→RGB565 | ~3ms | ~8ms | 16-bit 半宽输出 |
| libjpeg-turbo 编码 | ~25ms | ~60ms | 需 NEON 加速 |
| MJPEG 硬件输出 | ~0ms | ~0ms | 摄像头处理, 零 CPU |

---

## 七、main.cpp 集成详解

### 7.1 多线程数据流

```
采集线程 (std::thread)                    Qt 主线程 (QTimer 33ms)
┌─────────────────────┐                  ┌────────────────────────┐
│ while (running) {   │                  │ QTimer::timeout() {    │
│   getFrame(&fb)     │                  │   lock(mtx)            │
│   lock(mtx)         │                  │   gui.setFrame(...)    │
│   copy to shared    │  ──── mutex ──▶  │   gui.setFPS(...)      │
│   unlock(mtx)       │                  │   unlock(mtx)          │
│   putFrame(&fb)     │                  │ }                      │
│ }                    │                  │                        │
└─────────────────────┘                  └────────────────────────┘
                          共享缓冲区
                    ┌──────────────────┐
                    │ g_state.frameData │  std::vector<uint8_t>
                    │ g_state.width     │  int
                    │ g_state.height    │  int
                    │ g_state.format    │  PixelFormat
                    │ g_state.fps       │  double
                    └──────────────────┘
```

### 7.2 采集线程关键代码

```cpp
// src/main.cpp — 采集线程 lambda
captureThread = new std::thread([capture]() {
    FrameBuffer fb;
    while (g_state.running) {
        // 1. 阻塞取帧 (1s 超时)
        if (capture->getFrame(&fb, 1000) < 0) {
            if (!g_state.running) break;
            continue;  // 超时重试
        }

        {
            // 2. 拷贝到共享缓冲区（在 putFrame 之前！）
            std::lock_guard<std::mutex> lock(g_state.mtx);
            g_state.frameData.assign(fb.data, fb.data + fb.length);
            g_state.width  = fb.width;
            g_state.height = fb.height;
            g_state.format = fb.format;
            g_state.fps    = capture->getCurrentFPS();
        }

        // 3. 归还缓冲区（此时 mmap 内存可被内核重用）
        capture->putFrame(&fb);
    }
});
```

### 7.3 显示定时器关键代码

```cpp
// src/main.cpp — 显示定时器
displayTimer = new QTimer(&gui);
displayTimer->setInterval(33);  // ≈30fps
QObject::connect(displayTimer, &QTimer::timeout, [&gui]() {
    std::lock_guard<std::mutex> lock(g_state.mtx);
    if (g_state.frameData.empty()) return;

    // setFrame 存储指针, frameToQImage 内部深拷贝到 QImage
    gui.setFrame(g_state.frameData.data(),
                 static_cast<int>(g_state.frameData.size()),
                 g_state.width, g_state.height,
                 g_state.format);
    gui.setFPS(g_state.fps);
});
```

### 7.4 格式/分辨率切换回调查看

```cpp
// 分辨率切换 → 停止 → 重新配置 → 启动
gui.onResolutionChanged([capture](int w, int h) {
    if (capture->isStreaming()) {
        capture->stopCapture();
        capture->setFormat(w, h, capture->getCurrentFormat());
        capture->startCapture();
    }
});

// 格式切换 → 同上
gui.onFormatChanged([capture](PixelFormat fmt) {
    uint32_t v4l2fmt = (fmt == PixelFormat::FMT_YUYV)
        ? CameraCapture::V4L2_PIX_FMT_YUYV
        : CameraCapture::V4L2_PIX_FMT_MJPEG;
    if (capture->isStreaming()) {
        capture->stopCapture();
        capture->setFormat(640, 480, v4l2fmt);
        capture->startCapture();
    }
});
```

---

## 八、CMakeLists.txt 修改说明

### 8.1 修改内容

**新增 camera 源文件列表：**

```cmake
# 在 DISPLAY_SOURCES 之前新增
set(CAMERA_SOURCES
    src/camera/capture.cpp
    src/camera/processor.cpp
    include/camera/capture.h
    include/camera/processor.h
)

set(ALL_SOURCES
    ${CAMERA_SOURCES}    # 新增
    ${DISPLAY_SOURCES}
    ${MAIN_SOURCES}
)
```

**新增 pthread 链接：**

```cmake
target_link_libraries(smartcam PRIVATE
    Qt5::Widgets
    pthread                          # 新增: std::thread 需要
)
```

**新增可选 libjpeg 支持（注释状态）：**

```cmake
# find_package(JPEG QUIET)
# if(JPEG_FOUND)
#     target_compile_definitions(smartcam PRIVATE HAS_LIBJPEG)
#     target_link_libraries(smartcam PRIVATE JPEG::JPEG)
# endif()
```

### 8.2 修改对比

| 修改项 | 修改前 | 修改后 |
|--------|--------|--------|
| 源文件数 | 2 个 (gui.cpp, main.cpp) | 5 个 (capture.cpp, processor.cpp, gui.cpp, main.cpp, + MOC) |
| 链接库 | Qt5::Widgets | Qt5::Widgets + pthread |
| 构建摘要 | 显示 2 个源文件 | 显示 5 个源文件 |

---

## 九、构建与运行

### 9.1 PC 本地编译（开发调试）

```bash
cd SmartCam-Linux-on-imx6ull
mkdir -p build && cd build
cmake .. && make -j$(nproc)

# Mock 模式（无摄像头，显示彩条）
./smartcam

# 帮助信息
./smartcam --help
```

### 9.2 ARM 交叉编译（iMX6ULL）

```bash
# 使用项目自带的构建脚本
./scripts/build.sh arm

# 部署到开发板
scp build/arm/smartcam root@192.168.1.100:/usr/local/bin/
```

### 9.3 硬件运行（连摄像头）

开发板上无 X server，必须使用 `-platform linuxfb` 指定 Framebuffer 后端：

```bash
# 开发板上执行
# YUYV 模式（本地预览）
./smartcam --device /dev/video0 --fmt yuyv -platform linuxfb

# MJPEG 模式（零 CPU 采集，用于网络传输）
./smartcam --device /dev/video0 --fmt mjpeg -platform linuxfb

# 或设置环境变量
export QT_QPA_PLATFORM=linuxfb
./smartcam --device /dev/video0 --fmt yuyv
```

### 9.4 编译验证记录

```
日期: 2026-05-22
CMake: 3.25.1
编译器: GCC 11.4.0 (x86_64)
Qt: 5.15.3
结果: ✅ 0 error, 0 warning, 产物 ~2MB
Mock 模式: ✅ 正常启动, 彩条测试图输出正确
Help 参数: ✅ 显示完整命令行说明
```

---

## 十、与其他模块的集成点

```
                    ┌──────────────┐
                    │   main.cpp   │
                    └──────┬───────┘
                           │
          ┌────────────────┼────────────────┐
          │                │                │
          ▼                ▼                ▼
   ┌────────────┐  ┌────────────┐  ┌──────────────┐
   │ Camera     │  │ CameraGUI  │  │ MJPEGStream  │
   │ Capture    │  │ (显示模块)  │  │ Server(未来)  │
   └─────┬──────┘  └─────┬──────┘  └──────┬───────┘
         │               │                │
         │  getFrame()   │  setFrame()    │  updateFrame()
         │  putFrame()   │  setFPS()      │  (MJPEG 直通)
         ▼               ▼                ▼
   ┌──────────────────────────────────────────────┐
   │       共享帧缓冲区 (mutex 保护)              │
   │  g_state.frameData / width / height / format │
   └──────────────────────────────────────────────┘
```

### 与未来模块的回调接口

| main.cpp 回调 | 目标模块 | 说明 |
|---------------|----------|------|
| `onCaptureRequest()` | StorageManager | 拍照 → savePhoto() |
| `onRecordToggle()` | StorageManager | 录像 → startRecord()/stopRecord() |
| `onResolutionChanged()` | CameraCapture | 切换分辨率 → setFormat() + restart |
| `onFormatChanged()` | CameraCapture | 切换格式 → setFormat() + restart |

### 与 CPU 占用相关的性能关注点

| 场景 | 瓶颈 | 预期指标 |
|------|------|----------|
| YUYV 采集 + GUI 显示 | YUYV→RGB24 转换 | CPU ~15-20% @ 640x480 |
| MJPEG 采集 | 基本无 CPU 开销 | CPU < 5% |
| YUYV 采集 + JPEG 软编码 | libjpeg-turbo 编码 | CPU ~50% @ 640x480 |
| MJPEG 采集 + HTTP 流 | 内存拷贝 + socket 发送 | CPU ~10% |

---

## 十一、技术要点总结

| 技术点 | 实现方式 | 面试可讲 |
|--------|----------|----------|
| V4L2 完整流程 | open→s_fmt→reqbufs→mmap→qbuf→streamon→[dqbuf→qbuf]→streamoff | ✅ 核心技术栈 |
| mmap 零拷贝 | 内核 DMA 缓冲区直接映射到用户空间，避免 read() 拷贝 | ✅ 性能优化 |
| 双模式切换 | setFormat() 运行时切换 YUYV/MJPEG，需停止→配置→启动 | ✅ 设计灵活性 |
| select 超时 | 阻塞 DQBUF + select 超时，兼顾效率和可中断性 | ✅ 健壮性 |
| 滚动 FPS 统计 | 每 30 帧平均，避免单帧抖动导致显示不稳定 | ✅ 工程经验 |
| BT.601 定点运算 | 浮点系数转为整数乘加 >> 8，ARM 无 FPU 可用 | ✅ 嵌入式优化 |
| 帧拷贝策略 | getFrame→拷贝→putFrame 三步，明确的数据生命周期 | ✅ 内存管理 |
| 采集线程分离 | 独立 std::thread 拉帧，QTimer 33ms 渲染，互不阻塞 | ✅ 多线程架构 |
| 生产消费者模式 | mutex + vector 共享缓冲区，采集线程写 GUI 线程读 | ✅ 线程同步 |
| 条件编译 libjpeg | HAS_LIBJPEG 开关，默认不依赖第三方库 | ✅ 工程化管理 |

### 面试可追问的要点

**Q1: V4L2 的三种 I/O 方式有什么区别？为什么选 mmap？**

> V4L2 支持三种方式：read/write（每次调用拷贝数据）、mmap（内核 DMA 缓冲区直接映射）、userptr（用户分配缓冲区）。mmap 是最佳平衡——零拷贝、无需分配大块连续物理内存、驱动支持最好。

**Q2: 如果 DQBUF 超时了，说明什么？怎么处理？**

> 超时可能原因：1) 摄像头 USB 断开 2) 驱动卡死 3) 帧率太低。采集线程中超时后会检查 `running` 标志决定是否退出，不会永久阻塞。配合心跳检测可以检测到摄像头异常。

**Q3: 为什么 putFrame 要通过 data 指针反推索引，而不是让调用者传 index？**

> 设计原则：调用者不应该关心内部缓冲区索引。`FrameBuffer.data` 是 mmap 地址，用地址查找索引是封装细节，对外隐藏了 V4L2 缓冲池的实现。

**Q4: 为什么日志宏用 `LOG_ERR_` 而不是 `LOG_ERROR`？**

> 因为 `syslog.h` 定义了 `LOG_ERR`（整数常量 3），我们的日志宏也是函数式宏，会发生重定义。加下划线后缀避免冲突。这是跨平台开发中的常见注意事项。

---

## 十二、后续 TODO

- [ ] 集成 MJPEG-over-HTTP 流媒体服务器 → 采集线程通过 FrameQueue 分发帧
- [ ] 集成 StorageManager → onCaptureRequest/onRecordToggle 实际生效
- [ ] YUYV→RGB NEON 汇编优化（预期提升 30-50%）
- [ ] VideoProcessor 增加图像缩放功能（用于分辨率降采样）
- [ ] 添加 V4L2 设备热插拔检测（udev 监控）
- [ ] 添加 `CameraStatus` 结构体通过控制协议返回
- [ ] 单元测试：`tests/test_capture.cpp`（Mock V4L2 设备）
- [ ] 文档：`docs/api.md` Doxygen 生成类接口文档

---

## 十三、变更记录

| 日期 | 变更内容 |
|------|----------|
| 2026-05-22 | 初始实现：CameraCapture 类 (V4L2)、VideoProcessor 类、日志系统、环形缓冲区、main.cpp 集成、CMake 构建、编译通过 |

