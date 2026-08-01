# SmartCam PXP 硬件加速 — 实现计划

> 版本: v1.0 | 日期: 2026-08-02 | 预计工期: 3~5 天
> 目标: 用 i.MX6ULL 的 PXP 硬件引擎替代 CPU（NEON/标量）完成显示路径的 YUYV→RGB 颜色转换，释放 CPU 给编码，并为未来 720p/60fps 预留加速能力。
> 说明: 本项目当前**未使用 PXP**（NEON 软转 ~5ms 已达标），本计划是"如果需求变化（升分辨率/CPU 吃满）就启用"的预案 + 可落地实施方案。

---

## 一、背景与目标

### 1.1 现状

当前显示路径的颜色转换完全由 CPU 承担：

```
V4L2 mmap → g_state.frameData（YUYV 深拷贝）
  → GUI m_refreshTimer → frameToQImage → VideoProcessor::yuyvToRgb24（NEON ~5ms / 标量）
  → QImage → QPixmap → QLabel → linuxfb → /dev/fb0
```

**瓶颈**：YUYV→RGB 转换 + `setScaledContents` 缩放都在 Qt 主线程，每帧吃 CPU ~5ms+。MJPEG 模式下 YUYV 转换不触发（硬件直出 JPEG），但 YUYV 模式、以及未来 720p 时转换量 ×3，CPU 会吃紧。

### 1.2 目标

| 目标 | 度量 |
|------|------|
| P1: YUYV→RGB 转换交给 PXP，CPU 零占用 | 转换阶段 CPU 占用从 ~5ms/帧 降到 ~0 |
| P2: 上层（GUI）无感知切换 | `frameToQImage` 接口不变，内部实现替换 |
| P3: PXP 不可用时自动回退 | NEON/标量软转作为 fallback，不阻塞启动 |
| P4（可选）: PXP 叠加 OSD 层 | 为未来画中画/框选预留 |

### 1.3 非本期目标

- ~~PXP 直接输出到 LCD（深集成）~~：当前用 Qt linuxfb 渲染，改直连 LCD 需放弃 Qt 渲染，改动量过大，列为 Phase 2 单独评估。
- ~~PXP 加速 JPEG 编解码~~：PXP 不具备该能力（无 VPU），推流路径不受益。

---

## 二、技术选型：PXP 如何接入 Linux

i.MX6ULL 的 PXP 在 Linux 内核中以 **V4L2 memory-to-memory（M2M）设备**暴露（驱动 `imx-pxp`，设备节点 `/dev/videoX`，通常 `/dev/video1`，`/dev/video0` 是摄像头）。

### 2.1 为什么选 V4L2 M2M 而不是 DRM/KMS

| 方案 | 说明 | 选型 |
|------|------|------|
| **V4L2 M2M**（imx-pxp 驱动） | PXP 作为 buffer-to-buffer 转换器，输入 YUYV 输出 RGB | ✅ 首选：项目已是 V4L2 专家，改动最小，linuxfb 显示链路不变 |
| DRM/KMS plane | PXP 作为显示平面，内核做格式转换+合成 | ❌ 需放弃 linuxfb 改 DRM 后端，且 Qt linuxfb 不支持 |
| 直接 ioctl 操作 PXP 寄存器 | 绕过内核驱动 | ❌ 需写自定义内核驱动，风险高，不标准 |

### 2.2 V4L2 M2M 标准流程（PXP 用法）

```
open("/dev/video1")
  │ VIDIOC_QUERYCAP（检查 V4L2_CAP_VIDEO_M2M）
  ├─ 输出队列(OUTPUT) VIDIOC_S_FMT：源格式 YUYV 640x480
  ├─ 采集队列(CAPTURE) VIDIOC_S_FMT：目标格式 RGB565/RGB888
  ├─ 两个队列各 VIDIOC_REQBUFS(4) + mmap
  ├─ 缩放：VIDIOC_S_SELECTION(CAPTURE, compose 矩形)
  ├─ VIDIOC_STREAMON（两个队列）
  │
  │ 每帧转换：QBUF(源) + QBUF(目标) → DQBUF(目标，完成) → 拿 RGB 结果
  │
  └─ VIDIOC_STREAMOFF
```

**关键点**：PXP 转换是**异步**的——提交任务后由驱动硬件执行，`DQBUF` 返回才代表转换完成。所以应用需要"转换任务队列 + 完成通知"，不能同步等。

---

## 三、硬件 / 内核前置条件（必须先确认）

| 项 | 检查方式 | 若缺失 |
|----|----------|--------|
| 内核开启 PXP 驱动 | `zcat /proc/config.gz \| grep IMX_PXP` 或 `/boot/config` | 需重新编译内核（`CONFIG_VIDEO_IMX_PXP=y`） |
| 设备树启用 pxp 节点 | `ls /dev/video*`，多出的 videoX 即 PXP | 需在设备树加 `&pxp { status = "okay"; }` |
| 确认 PXP 设备号 | `cat /sys/class/video4linux/video1/name` | 输出应为 `imx-pxp` |
| libcamera/v4l2 工具（调试用） | `v4l2-ctl --list-devices` | 仅调试需要，非运行必需 |

**风险提示**：野火 i.MX6ULL 出厂 BSP 的 PXP 驱动不一定默认开启。**Phase 0 必须先做环境探测**，确认 `/dev/video1` 存在且 name 为 `imx-pxp`，否则后续全部落空。

---

## 四、模块设计

### 4.1 新增类：`PxpConverter`（`include/camera/pxp_converter.h` + `src/camera/pxp_converter.cpp`）

```cpp
/**
 * @brief PXP 硬件颜色转换器（V4L2 M2M 封装）
 *
 * 职责：把 YUYV 帧异步转换为 RGB888/RGB565 帧，替代 CPU 软转。
 * 线程安全：转换提交与结果取回支持跨线程（内部用互斥锁）。
 * 失败策略：init 失败 → isAvailable() 返回 false，调用方回退软转。
 */
class PxpConverter {
public:
    PxpConverter();
    ~PxpConverter();

    // ---- 生命周期 ----
    int init(const char* dev = "/dev/video1");  // 打开 M2M 设备，配置格式
    void shutdown();                             // 停止流、释放缓冲

    // ---- 能力查询 ----
    bool isAvailable() const;                    // PXP 是否可用
    static bool probe(const char* dev = "/dev/video1");  // 探测设备（不占资源）

    // ---- 转换任务 ----
    /**
     * @brief 提交一个 YUYV→RGB 转换任务（异步）
     * @param src   YUYV 源数据（调用方持有，直到 onDone 回调）
     * @param dst   输出 RGB 缓冲（由本类内部缓冲池提供）
     * @param w/h   宽高（YUYV 偶数宽）
     * @param fmt   目标格式：RGB888 / RGB565
     * @param onDone 完成回调（转换结果就绪时调用，携带 dst 指针）
     * @return 0 提交成功，负值失败
     */
    int convert(const uint8_t* src, int w, int h,
                PixelFormat outFmt,
                std::function<void(const uint8_t* rgb, int len)> onDone);

    // ---- 内部 ----
private:
    int  m_fd = -1;
    struct V4l2Buf { void* start; size_t len; };
    std::vector<V4l2Buf> m_srcBufs;   // 源队列（mmap）
    std::vector<V4l2Buf> m_dstBufs;   // 目标队列（mmap）
    int m_nbufs = 0;
    int m_width = 0, m_height = 0;
    PixelFormat m_outFmt = PixelFormat::FMT_RGB888;
    std::mutex m_mtx;                 // 保护任务提交
};
```

### 4.2 集成点：替换 `frameToQImage` 的 YUYV 分支

`src/display/gui.cpp` 当前：

```cpp
case PixelFormat::FMT_YUYV: {
    std::vector<uint8_t> rgb(w * h * 3);
    VideoProcessor::yuyvToRgb24(data, rgb.data(), w, h);
    return QImage(rgb.data(), w, h, w * 3, QImage::Format_RGB888).copy();
}
```

改后（PXP 可用走硬件，否则回退软转）：

```cpp
case PixelFormat::FMT_YUYV: {
    // PXP 加速路径：提交异步转换，完成回调里刷新画面
    if (g_pxp && g_pxp->isAvailable()) {
        g_pxp->convert(data, w, h, PixelFormat::FMT_RGB888,
            [this, w, h](const uint8_t* rgb, int len) {
                // 回调在 GUI 线程执行（转换线程投递），setPixmap 更新画面
                QImage img(rgb, w, h, w * 3, QImage::Format_RGB888).copy();
                m_videoDisplay->setPixmap(QPixmap::fromImage(img));
            });
        return {};   // 当前帧渲染交给回调
    }
    // 回退：NEON/标量软转（现状路径）
    std::vector<uint8_t> rgb(w * h * 3);
    VideoProcessor::yuyvToRgb24(data, rgb.data(), w, h);
    return QImage(rgb.data(), w, h, w * 3, QImage::Format_RGB888).copy();
}
```

### 4.3 线程模型

```
采集线程                  PXP 转换线程（新增 std::thread）
  getFrame ──→ convert() ──→ QBUF 提交 ──→ PXP 硬件
                                            │
  GUI 线程 ←──── onDone 回调（Qt::QueuedConnection 投递）
  setPixmap
```

**线程设计要点**：
1. PXP 转换是异步的，DQBUF 阻塞等待完成——所以需要**独立转换线程**做 `DQBUF`，不能占 GUI 线程；
2. 完成回调要**投递回 GUI 线程**（用 Qt 信号 + QueuedConnection，或 QMetaObject::invokeMethod），Qt 控件只能在 GUI 线程操作；
3. 源缓冲 `src` 的持有权：提交后 PXP 正在读，必须在回调前保持源数据存活——由调用方（g_state）保证 + 拷贝进 src 队列缓冲。

### 4.4 缓冲策略（关键权衡）

| 方案 | 说明 | 选型 |
|------|------|------|
| PXP 内部 mmap 缓冲池（4 源 + 4 目标） | 每次转换把 YUYV 拷进源缓冲，结果在目标缓冲 | ✅ 标准做法，生命周期可控 |
| 直接引用 g_state（零拷贝源） | 源缓冲 DMA 指向 g_state | ❌ 需 PXP 支持 USERPTR，且源内存被采集线程覆盖，同步复杂 |

**内存开销**：源缓冲 4×614KB + 目标缓冲 4×922KB(RGB888) ≈ **6MB**，在 512MB 上可接受；若目标用 RGB565（显示是 565）则 4×614KB，共 ~5MB。

---

## 五、实现步骤（分阶段）

### Phase 0：环境探测（0.5 天）

- [ ] 在开发板执行探测命令，确认 PXP 设备存在：
  ```bash
  ls -la /dev/video*
  cat /sys/class/video4linux/video1/name   # 期望输出 imx-pxp
  v4l2-ctl --list-devices
  ```
- [ ] 若不存在：查内核配置 `grep IMX_PXP /boot/config*`；缺则重编内核（CONFIG_VIDEO_IMX_PXP=y）+ 设备树 `&pxp { status="okay"; }`。
- [ ] 用 `v4l2-ctl` 手工验证 PXP 转换能力（YUYV→RGB565 缩放），确认驱动可用。
- **退出条件**：PXP 设备可用；否则本计划终止并记录原因。

### Phase 1：`PxpConverter` 最小实现（1.5 天）

- [ ] 新建 `include/camera/pxp_converter.h` + `src/camera/pxp_converter.cpp`；
- [ ] 实现 `init/probe/shutdown`：打开设备、QUERYCAP、双队列 S_FMT、REQBUFS+mmap、STREAMON；
- [ ] 实现 `convert`：QBUF 源+目标 → 返回；内部转换线程 DQBUF 完成 → 调 onDone；
- [ ] 编译通过（PC 模式用 stub 占位返回 false，ARM 模式真实编译）。

### Phase 2：GUI 集成 + 回退（1 天）

- [ ] `main.cpp` 创建全局 `PxpConverter* g_pxp`，`init` 失败不阻断启动（isAvailable=false）；
- [ ] 修改 `frameToQImage` YUYV 分支：PXP 可用走硬件，否则回退软转；
- [ ] 完成回调投递 GUI 线程（Qt signal + QueuedConnection）；
- [ ] Mock 模式不受影响（不触发 YUYV 转换）。

### Phase 3：验证与调优（1 天）

- [ ] 开发板实测：YUYV 模式下 PXP vs NEON 的 CPU 占用对比（`top`/`mpstat`）；
- [ ] 画面正确性：色彩一致（BT.601 系数相同）、无花屏、无撕裂；
- [ ] 帧率稳定性：连续运行 30 分钟，无 DQBUF 超时堆积；
- [ ] 回退验证：临时改设备路径测试 fallback 路径；
- [ ] 补充文档（PXP 模块实现记录）。

### Phase 4（可选）：OSD 叠加

- [ ] 利用 PXP 的双输入 alpha 合成，把状态栏/框选合成到视频层；
- [ ] 需重新评估显示架构（linuxfb 下 PXP 合成结果如何进 Qt）。

---

## 六、风险与对策

| 风险 | 影响 | 对策 |
|------|------|------|
| PXP 驱动未开启 / 设备不存在 | Phase 0 即终止 | 探测先行；设备树/内核配置文档化 |
| PXP 转换结果与 NEON 色彩不一致 | 显示偏色 | 统一 BT.601 系数；测试图对比 |
| 异步转换延迟波动 | 画面延迟 | 完成回调投递 GUI 线程；必要时限速 |
| 转换线程 DQBUF 阻塞 | 线程泄漏 | 超时 + 错误处理（同 getFrame 的 select 超时模式） |
| 内存增加 ~6MB | 内存紧张 | 目标用 RGB565 减半；确认 512MB 预算 |
| Qt linuxfb 与 PXP 结果衔接 | 集成复杂度 | Phase 2 先做 buffer-to-buffer 浅集成，不做直连 |

---

## 七、测试方案

| 测试 | 方法 | 通过标准 |
|------|------|----------|
| 设备探测 | 板端脚本 | PXP 设备存在、name=imx-pxp |
| 单元（转换正确性） | 固定 YUYV 测试图 → PXP vs NEON 输出逐像素比对 | 差异 < 1%（量化误差） |
| 集成（显示） | YUYV 模式运行，肉眼 + 截图 | 色彩正常、无花屏/撕裂 |
| 性能 | `mpstat` 对比转换 CPU 占用 | PXP 路径 CPU < 1% |
| 稳定性 | 30 分钟连续运行 | 无 DQBUF 超时、无内存增长 |
| 回退 | 指定错误设备路径启动 | 自动走软转，功能正常 |

---

## 八、代码变更清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/camera/pxp_converter.h` | **新增** | `PxpConverter` 类声明 |
| `src/camera/pxp_converter.cpp` | **新增** | V4L2 M2M 封装实现 |
| `include/display/gui.h` | 修改 | `frameToQImage` 签名不变（内部变） |
| `src/display/gui.cpp` | 修改 | YUYV 分支走 PXP + 回退 |
| `src/main.cpp` | 修改 | 创建 `g_pxp`、初始化、析构 |
| `CMakeLists.txt` | 修改 | 新增 `pxp_converter.cpp` |
| `configs/smartcam.conf` | 修改（可选） | `[camera] pxp_device = /dev/video1`、`use_pxp = true` |
| `docs/` | 新增 | `plan-pxp-acceleration.md`（本文档）+ 后续实现记录 |

---

## 八·五、代码草案（预计新增 ~550 行）

> 本节给出各阶段的关键代码草案，供评审与预估。总行数：`pxp_converter.h` ~90 + `pxp_converter.cpp` ~340 + GUI 集成 ~40 + main.cpp ~40 + CMake/配置 ~40 ≈ **550 行**。
> ⚠️ 代码为**草案**，未编译验证，V4L2 ioctl 细节（如 `VIDIOC_S_SELECTION` 缩放）需按实际驱动调整。

### 8.5.1 `include/camera/pxp_converter.h`（新增，~90 行）

```cpp
#ifndef SMART_CAM_CAMERA_PXP_CONVERTER_H
#define SMART_CAM_CAMERA_PXP_CONVERTER_H

#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>

#include "include/common/types.h"

/**
 * @brief PXP 硬件颜色转换器（V4L2 M2M 封装）
 *
 * 把 YUYV 帧异步转换为 RGB888/RGB565，替代 CPU 软转。
 * 线程安全：转换提交与结果取回支持跨线程。
 * 失败策略：init 失败 → isAvailable() 返回 false，调用方回退软转。
 */
class PxpConverter {
public:
    PxpConverter();
    ~PxpConverter();

    // 禁用拷贝
    PxpConverter(const PxpConverter&) = delete;
    PxpConverter& operator=(const PxpConverter&) = delete;

    /** @brief 打开 M2M 设备并配置双队列格式 */
    int init(const char* dev = "/dev/video1");

    /** @brief 停止流、释放缓冲、退出转换线程 */
    void shutdown();

    /** @brief PXP 是否可用（init 成功且设备正常） */
    bool isAvailable() const { return m_available; }

    /** @brief 探测设备是否可用（不长期占用资源） */
    static bool probe(const char* dev = "/dev/video1");

    /**
     * @brief 提交 YUYV→RGB 转换任务（异步）
     * @param src    YUYV 源数据（拷贝进内部缓冲，调用方可立即释放）
     * @param w/h    宽高（YUYV 偶数宽）
     * @param outFmt 目标格式 FMT_RGB888 / FMT_RGB565
     * @param onDone 完成回调（在 GUI 线程调用，携带结果指针）
     * @return 0 成功，负值失败
     */
    int convert(const uint8_t* src, int w, int h,
                PixelFormat outFmt,
                std::function<void(const uint8_t* rgb, int len)> onDone);

private:
    struct V4l2Buf {
        void*  start = nullptr;
        size_t length = 0;
        bool   queued = false;
    };

    int openDevice(const char* dev);
    int setupFormats(int w, int h, uint32_t srcFmt, uint32_t dstFmt);
    int requestAndMapBuffers();
    void converterThreadLoop();      // 独立转换线程：DQBUF 完成 → onDone
    void cleanup();

    // ---- 设备状态 ----
    int  m_fd = -1;
    bool m_available = false;
    bool m_streaming = false;

    // ---- 格式 ----
    int    m_width  = 0;
    int    m_height = 0;
    PixelFormat m_outFmt = PixelFormat::FMT_RGB888;
    uint32_t m_srcFourcc = 0;   // V4L2_PIX_FMT_YUYV
    uint32_t m_dstFourcc = 0;   // V4L2_PIX_FMT_RGB24 / RGB565

    // ---- 缓冲池（源 + 目标各 N 个） ----
    static constexpr int kBufCount = 4;
    std::vector<V4l2Buf> m_srcBufs;
    std::vector<V4l2Buf> m_dstBufs;

    // ---- 任务与线程 ----
    std::thread        m_convThread;
    std::atomic<bool>  m_running{false};
    std::atomic<bool>  m_hasTask{false};
    std::mutex         m_mtx;             // 保护队列/状态
    std::condition_variable m_taskCv;     // 有任务通知
};

#endif // SMART_CAM_CAMERA_PXP_CONVERTER_H
```

### 8.5.2 `src/camera/pxp_converter.cpp`（新增，~340 行，核心骨架）

```cpp
#include "include/camera/pxp_converter.h"
#include "include/common/logger.h"

#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

// V4L2 FOURCC（避免依赖 capture.h 常量，独立维护）
#ifndef V4L2_PIX_FMT_YUYV
#define V4L2_PIX_FMT_YUYV  0x56595559
#endif
#ifndef V4L2_PIX_FMT_RGB24
#define V4L2_PIX_FMT_RGB24 0x18
#endif
#ifndef V4L2_PIX_FMT_RGB565
#define V4L2_PIX_FMT_RGB565 0x21
#endif

PxpConverter::PxpConverter() = default;

PxpConverter::~PxpConverter() { shutdown(); }

bool PxpConverter::probe(const char* dev) {
    int fd = open(dev, O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) return false;
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    bool ok = (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
              && (cap.capabilities & V4L2_CAP_VIDEO_M2M);
    close(fd);
    return ok;
}

int PxpConverter::init(const char* dev) {
    if (m_available) return 0;

    m_fd = open(dev, O_RDWR | O_NONBLOCK, 0);
    if (m_fd < 0) {
        LOG_WRN("PXP: cannot open %s: %s", dev, strerror(errno));
        return -errno;
    }

    // 清 O_NONBLOCK → 阻塞模式（与 capture 一致）
    int flags = fcntl(m_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(m_fd, F_SETFL, flags & ~O_NONBLOCK);

    // 默认 640x480 YUYV→RGB888；运行时可用 convert 改尺寸
    m_width = 640; m_height = 480;
    m_outFmt = PixelFormat::FMT_RGB888;
    m_srcFourcc = V4L2_PIX_FMT_YUYV;
    m_dstFourcc = V4L2_PIX_FMT_RGB24;

    if (setupFormats(m_width, m_height, m_srcFourcc, m_dstFourcc) < 0 ||
        requestAndMapBuffers() < 0) {
        cleanup();
        return -1;
    }

    // 启动转换线程
    m_running = true;
    m_convThread = std::thread(&PxpConverter::converterThreadLoop, this);

    m_available = true;
    LOG_INF("PXP: initialized (%s), %dx%d YUYV→RGB", dev, m_width, m_height);
    return 0;
}

int PxpConverter::setupFormats(int w, int h, uint32_t srcFmt, uint32_t dstFmt) {
    // ---- 输出队列(OUTPUT)：源格式 ----
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width  = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = srcFmt;
    fmt.fmt.pix.field  = V4L2_FIELD_ANY;
    if (ioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERR_("PXP: OUTPUT S_FMT failed: %s", strerror(errno));
        return -errno;
    }

    // ---- 采集队列(CAPTURE)：目标格式 ----
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width  = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = dstFmt;
    fmt.fmt.pix.field  = V4L2_FIELD_ANY;
    if (ioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERR_("PXP: CAPTURE S_FMT failed: %s", strerror(errno));
        return -errno;
    }
    return 0;
}

int PxpConverter::requestAndMapBuffers() {
    // ---- 源队列 ----
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = kBufCount;
    req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(m_fd, VIDIOC_REQBUFS, &req) < 0) {
        LOG_ERR_("PXP: OUTPUT REQBUFS failed: %s", strerror(errno));
        return -errno;
    }
    m_srcBufs.resize(req.count);
    for (unsigned i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT; buf.memory = V4L2_MEMORY_MMAP; buf.index = i;
        if (ioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0) return -errno;
        m_srcBufs[i].start = mmap(nullptr, buf.length, PROT_READ|PROT_WRITE,
                                  MAP_SHARED, m_fd, buf.m.offset);
        m_srcBufs[i].length = buf.length;
        // QBUF 入队
        ioctl(m_fd, VIDIOC_QBUF, &buf);
    }

    // ---- 目标队列（同源队列，type=CAPTURE）----
    memset(&req, 0, sizeof(req));
    req.count  = kBufCount;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(m_fd, VIDIOC_REQBUFS, &req) < 0) {
        LOG_ERR_("PXP: CAPTURE REQBUFS failed: %s", strerror(errno));
        return -errno;
    }
    m_dstBufs.resize(req.count);
    for (unsigned i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buf.memory = V4L2_MEMORY_MMAP; buf.index = i;
        if (ioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0) return -errno;
        m_dstBufs[i].start = mmap(nullptr, buf.length, PROT_READ|PROT_WRITE,
                                  MAP_SHARED, m_fd, buf.m.offset);
        m_dstBufs[i].length = buf.length;
        ioctl(m_fd, VIDIOC_QBUF, &buf);
    }

    // ---- STREAMON 两个队列 ----
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(m_fd, VIDIOC_STREAMON, &t);
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(m_fd, VIDIOC_STREAMON, &t);
    m_streaming = true;
    return 0;
}

int PxpConverter::convert(const uint8_t* src, int w, int h,
                          PixelFormat outFmt,
                          std::function<void(const uint8_t*, int)> onDone) {
    if (!m_available || !src) return -EIO;

    // 简化：队列满时丢弃该帧（最新帧策略，与显示节流一致）
    std::lock_guard<std::mutex> lock(m_mtx);

    // 找一个空闲源缓冲（queued=false）
    int freeIdx = -1;
    for (int i = 0; i < (int)m_srcBufs.size(); ++i) {
        if (!m_srcBufs[i].queued) { freeIdx = i; break; }
    }
    if (freeIdx < 0) return -EAGAIN;   // 队列满，丢弃

    // 拷贝源数据进源缓冲（PXP 正在读时保证存活）
    size_t srcLen = (size_t)w * h * 2;
    if (srcLen > m_srcBufs[freeIdx].length) return -EINVAL;
    memcpy(m_srcBufs[freeIdx].start, src, srcLen);
    m_srcBufs[freeIdx].queued = true;

    // 找一个空闲目标缓冲
    int dstIdx = -1;
    for (int i = 0; i < (int)m_dstBufs.size(); ++i) {
        if (!m_dstBufs[i].queued) { dstIdx = i; break; }
    }
    if (dstIdx < 0) { m_srcBufs[freeIdx].queued = false; return -EAGAIN; }

    // QBUF 源 + 目标 → PXP 开始转换
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT; buf.memory = V4L2_MEMORY_MMAP;
    buf.index = freeIdx; buf.bytesused = (__u32)srcLen;
    if (ioctl(m_fd, VIDIOC_QBUF, &buf) < 0) {
        m_srcBufs[freeIdx].queued = false;
        return -errno;
    }
    // 注意：完整实现需把任务(freeIdx, dstIdx, onDone)入队，由转换线程配对 DQBUF
    // 此处简化：目标 DQBUF 由 converterThreadLoop 完成
    // 实际提交目标 QBUF + 记录回调的完整逻辑略（草案）

    return 0;
}

void PxpConverter::converterThreadLoop() {
    while (m_running) {
        // 阻塞 DQBUF 目标队列（PXP 完成信号）
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(m_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EINTR) continue;
            LOG_WRN("PXP: DQBUF failed: %s", strerror(errno));
            break;
        }
        // 完成 → 把结果投递回 GUI 线程（草案：直接调用，实际需 QueuedConnection）
        if (m_dstBufs[buf.index].queued) {
            // 通知 onDone（简化：由调用方在 GUI 线程注册的完成处理函数执行）
            // 这里标记完成并归还
            m_dstBufs[buf.index].queued = false;
            ioctl(m_fd, VIDIOC_QBUF, &buf);   // 重新入队
        }
    }
}

void PxpConverter::shutdown() {
    if (m_running) {
        m_running = false;
        if (m_convThread.joinable()) m_convThread.join();
    }
    cleanup();
}

void PxpConverter::cleanup() {
    if (m_streaming && m_fd >= 0) {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ioctl(m_fd, VIDIOC_STREAMOFF, &t);
        t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(m_fd, VIDIOC_STREAMOFF, &t);
        m_streaming = false;
    }
    for (auto& b : m_srcBufs) if (b.start) munmap(b.start, b.length);
    for (auto& b : m_dstBufs) if (b.start) munmap(b.start, b.length);
    m_srcBufs.clear(); m_dstBufs.clear();
    if (m_fd >= 0) { close(m_fd); m_fd = -1; }
    m_available = false;
}
```

> **草案说明**：上述 `convert`/`converterThreadLoop` 为**简化骨架**——完整实现需要"任务队列"（记录每次转换的 src/dst 索引 + 回调），由转换线程把 `DQBUF` 的目标索引与提交时的回调配对。建议参考本项目 `capture.cpp` 的 `getFrame/putFrame` 配对模式 + 一个 `std::deque<Task>`。草案行数已含此逻辑的实现空间（~50 行）。

### 8.5.3 `src/display/gui.cpp` 修改（~25 行，PXP 优先 + 软转回退）

```cpp
// 顶部：声明全局 PXP 指针（main.cpp 定义）
extern PxpConverter* g_pxp;

// frameToQImage 的 YUYV 分支改为：
case PixelFormat::FMT_YUYV: {
    // PXP 加速路径：提交异步转换，完成回调里刷新画面
    if (g_pxp && g_pxp->isAvailable()) {
        // 目标格式跟随显示后端：linuxfb RGB565 或 PC RGB888
        PixelFormat outFmt = PixelFormat::FMT_RGB888;
        g_pxp->convert(data, w, h, outFmt, [this, w, h](const uint8_t* rgb, int len) {
            // 投递到 GUI 线程（PXP 转换线程完成，不能直接碰 QLabel）
            QMetaObject::invokeMethod(this, [this, w, h, rgb, len]() {
                QImage img(rgb, w, h, w * 3, QImage::Format_RGB888).copy();
                m_videoDisplay->setPixmap(QPixmap::fromImage(img));
            }, Qt::QueuedConnection);
        });
        return {};   // 当前帧由回调渲染
    }
    // 回退：NEON/标量软转（现状路径）
    std::vector<uint8_t> rgb(w * h * 3);
    VideoProcessor::yuyvToRgb24(data, rgb.data(), w, h);
    return QImage(rgb.data(), w, h, w * 3, QImage::Format_RGB888).copy();
}
```

> **注意**：PXP 转换是**异步**的，`frameToQImage` 返回后 RGB 才就绪。这要求 `frameToQImage` 调用处（`refreshFrame`）能接受"本帧由回调渲染"——需小幅调整 `refreshFrame` 的返回值语义（当前 `if (!img.isNull())` 判断）。这是集成时最需要注意的行为变化。

### 8.5.4 `src/main.cpp` 修改（~40 行，全局 PXP + 生命周期）

```cpp
// 全局 PXP 实例（在 CameraGUI gui 之前定义）
PxpConverter* g_pxp = nullptr;

// main() 中、真实相机模式初始化后：
g_pxp = new PxpConverter();
if (g_pxp->init("/dev/video1") < 0) {
    LOG_WRN("PXP unavailable, falling back to software conversion");
    delete g_pxp;
    g_pxp = nullptr;   // isAvailable()=false，GUI 走软转
} else {
    LOG_INF("PXP hardware conversion enabled");
}

// app.exec() 返回后（main 收尾）：
if (g_pxp) { g_pxp->shutdown(); delete g_pxp; g_pxp = nullptr; }
```

### 8.5.5 `CMakeLists.txt` + `configs/smartcam.conf` 修改（~17 行）

```cmake
# CMakeLists.txt 的 CAMERA_SOURCES 追加：
set(CAMERA_SOURCES
    src/camera/capture.cpp
    src/camera/processor.cpp
    src/camera/pxp_converter.cpp          # ← 新增
    include/camera/capture.h
    include/camera/processor.h
    include/camera/pxp_converter.h       # ← 新增
)
```

```ini
# smartcam.conf 新增 [camera] 配置项（可选）：
# pxp_device = /dev/video1   # PXP M2M 设备（留空=禁用）
# use_pxp    = true          # 显式开关
```

### 8.5.6 行数汇总

| 文件 | 预估行数 |
|------|----------|
| `pxp_converter.h` | ~90 |
| `pxp_converter.cpp` | ~340 |
| `gui.cpp` 修改 | ~25（+ gui.h 1 行 extern） |
| `main.cpp` 修改 | ~40 |
| `CMakeLists.txt` + config | ~17 |
| **合计** | **~510~550 行** |

> 与"八、代码变更清单"一致：**新增 2 个文件（~430 行）+ 修改 4 个文件（~110 行）**。

---

## 九、工期估算

| 阶段 | 内容 | 工期 |
|------|------|------|
| Phase 0 | 环境探测 + 内核/设备树确认 | 0.5 天 |
| Phase 1 | PxpConverter 最小实现 | 1.5 天 |
| Phase 2 | GUI 集成 + 软转回退 | 1 天 |
| Phase 3 | 验证 + 调优 + 文档 | 1 天 |
| Phase 4（可选） | OSD 叠加 | 2 天 |
| **合计** | | **3~5 天**（不含 Phase 4） |

---

## 十、结论

> PXP 加速是一个"锦上添花 + 为未来铺路"的优化：当前 NEON 软转（~5ms）在 640x480@30fps 下已达标，PXP 的核心价值是**释放 CPU**（从 ~5ms 降到 ~0）和**为 720p/60fps 预留能力**。接入方式选 V4L2 M2M（imx-pxp 驱动），与项目现有 V4L2 技术栈一致，改动集中在新增 `PxpConverter` 类 + 替换 `frameToQImage` 的 YUYV 分支。**最大前置风险是板子内核是否已开启 PXP 驱动**，因此 Phase 0 环境探测必须先行。若探测失败，本计划自然终止，系统继续走 NEON 软转，功能不受影响。
