# src/camera/ 模块面试复习指南（视频采集与图像处理）

> 定位：以「技术面试官 + 应聘者」双视角，系统拆解 `src/camera/` 模块的代码实现。
> 阅读本文档前建议先自行通读 `src/camera/capture.cpp`、`src/camera/processor.cpp`、`src/camera/processor_neon.cpp` 及其头文件，并对照 `src/main.cpp` 中 300~990 行的调用场景。
>
> 组织方式：**模块概览（脑图）→ 分块详解（代码 + 坑点 + 面试追问）→ 综合思考（耦合/重构/设计模式）**。

---

## 目录

1. [第一部分 模块整体概览](#第一部分-模块整体概览)
   - 1.1 脑图式结构
   - 1.2 职责边界
   - 1.3 输入 / 输出
   - 1.4 依赖的外部库与平台
   - 1.4.1 前向声明与头文件依赖隔离（capture.h 设计细节）
   - 1.5 核心类 / 函数列表与调用关系
2. [第二部分 分块代码详解（含面试追问）](#第二部分-分块代码详解含面试追问)
   - 2.1 块一：设备初始化与能力查询
   - 2.1.1 ioctl 与 fcntl：驱动指令通道 vs fd 属性开关
   - 2.2 块二：格式 / 帧率 / 分辨率设置
   - 2.3 块三：mmap 帧缓冲池与流控制
   - 2.4 块四：帧捕获循环（getFrame / putFrame）
   - 2.5 块五：FPS 统计与帧率控制
   - 2.6 块六：颜色空间转换（YUYV → RGB，BT.601 定点）
   - 2.7 块七：NEON SIMD 加速
   - 2.8 块八：MJPEG 帧边界解析
   - 2.9 块九：JPEG 编码（libjpeg-turbo）
   - 2.10 块十：V4L2 相机控制（亮度/对比度/曝光/白平衡）
3. [第三部分 综合思考](#第三部分-综合思考)
   - 3.1 与推理引擎 / 网络传输 / 显示渲染的数据流耦合
   - 3.2 需求变更下的重构推演（USB→CSI、720p→4K、30→60fps）
   - 3.3 设计模式评估与改进建议
   - 3.4 面试「一句话总结」

---

# 第一部分 模块整体概览

## 1.1 脑图式结构

```
src/camera/ 视频采集与图像处理模块
│
├── CameraCapture（采集引擎，V4L2）          capture.h / capture.cpp
│   ├── 生命周期：init() / release()
│   ├── 设备查询：getDriverInfo() / enumFormats() / enumFrameSizes()
│   ├── 格式参数：setFormat() / setFramerate() / getFramerate()
│   │            enumFrameRates() / setControl() / getControl() / queryControl()
│   ├── 流控制：startCapture() / stopCapture() / isStreaming()
│   ├── 帧交互：getFrame() / putFrame()
│   ├── 状态查询：getCurrentFPS() / getCurrentResolution() / getCurrentFormat()
│   └── 内部实现：
│       openDevice → queryCapability → requestBuffers → mapBuffers
│       → queueAllBuffers → dequeueBuffer → unmapBuffers → updateFPS
│
├── VideoProcessor（图像处理工具，纯静态类）   processor.h / processor.cpp
│   ├── MJPEG 解析：isJPEGStart() / findJPEGFrame()
│   ├── 颜色转换：yuyvToRgb24() / yuyvToRgb565() / yuyvMacroPixelToRgb24()
│   └── JPEG 编码：encodeRGBtoJPEG() / encodeYUYVtoJPEG()
│
└── processor_neon.cpp（NEON SIMD 实现，非类成员）
    └── yuyv_to_rgb24_neon() —— YUYV→RGB24 向量化，16 像素/轮
```

**V4L2 完整采集流程（代码注释中明示的状态机）：**

```
open → querycap → s_fmt → reqbufs → querybuf → mmap → qbuf → streamon
→ [dqbuf → 处理 → qbuf] 循环 → streamoff → REQBUFS(0)
```

## 1.2 职责边界

| 本模块负责 | 本模块不负责 |
|-----------|-------------|
| 打开/枚举/配置 V4L2 摄像头设备 | 网络传输（HTTP/RTSP/TCP 在 `src/network/`） |
| 申请/映射/轮转 mmap 帧缓冲池 | 持久化存储（拍照/录像在 `src/storage/`） |
| 阻塞取帧并归还缓冲（getFrame/putFrame） | GUI 渲染（Qt 在 `src/display/`） |
| YUYV→RGB24/RGB565 颜色转换（含 NEON） | 线程编排与全局状态（`src/main.cpp` 的 `g_state`） |
| MJPEG 帧边界解析、JPEG 编码 | 业务逻辑（帧率节流、曝光联动等，均在 main.cpp 层） |
| V4L2 相机参数读写（亮度/曝光等） | 硬件 DMA 本身（内核 UVC/V4L2 驱动） |

**一句话**：camera 模块是"生产帧 + 加工帧"的供给侧，只暴露 `FrameBuffer` 给上层消费，不关心谁消费、怎么消费。这种"生产与消费解耦"是后面所有多线程设计的前提。

## 1.3 输入 / 输出

- **输入**：
  - 设备路径，如 `/dev/video0`（`init(const char* device)`）
  - 期望格式 `(width, height, pixfmt)`，如 `(640, 480, V4L2_PIX_FMT_MJPEG)`
  - V4L2 控件 ID 与目标值（`setControl(cid, value)`）
- **输出**：
  - `FrameBuffer`（`include/common/types.h`）：`data/length/width/height/format/index/timestamp`
  - 枚举结果：格式列表、分辨率列表、帧率列表、控件范围
  - 统计信息：`getCurrentFPS()` / `getCurrentResolution()` / `getCurrentFormat()`

## 1.4 依赖的外部库与平台

| 依赖 | 用途 | 说明 |
|------|------|------|
| Linux V4L2 内核接口（`linux/videodev2.h`） | 视频采集 | 标准框架，ioctl 驱动 |
| `libjpeg-turbo`（`jpeglib.h`） | JPEG 编码 | CMake 强制 `HAS_LIBJPEG`；ARM 交叉编译静态链接 |
| ARM NEON（`arm_neon.h`） | 颜色转换加速 | 由 `-mfpu=neon` 定义 `__ARM_NEON`，仅 ARM 交叉编译启用 |
| 标准 C++17 | 线程/容器/时间 | `std::mutex`、`std::vector`、`std::chrono` |

> 面试点：`capture.h` 用**前向声明**（`struct v4l2_capability;`）代替直接 include `linux/videodev2.h`，仅在头文件里保留 V4L2 常量（FOURCC、控件 ID）。好处是降低头文件耦合、避免 Qt/系统头冲突，代价是需要手工维护常量与内核头的一致性。这是一个值得在面试中主动讲出的"接口隔离"细节。

## 1.4.1 前向声明与头文件依赖隔离（capture.h 设计细节）

### 代码里实际写了什么

`capture.h` 第 40-43 行：

```cpp
// ============================================================
// V4L2 相关结构前向声明（避免引入 linux/videodev2.h 冲突）
// ============================================================
struct v4l2_capability;
struct v4l2_format;
struct v4l2_buffer;
struct v4l2_queryctrl;
```

这 4 行就是**前向声明（forward declaration）**。而 `capture.cpp` 第 26 行才是真正引入完整定义的地方：

```cpp
#include <linux/videodev2.h>
```

### 前向声明是什么

`struct v4l2_capability;` 这行告诉编译器：

> "存在一个叫 `v4l2_capability` 的结构体类型，但我现在不告诉你它里面有哪些成员。"

此时编译器知道这个**类型的名字**，但不知道它的**大小和内部布局**。这个"半知半解"就够用了——因为 `capture.h` 里只把 V4L2 结构体用作**指针 / 引用**：

```cpp
int queryCapability();                              // 内部用到 struct v4l2_capability
int dequeueBuffer(v4l2_buffer& buf, int timeout_ms); // 引用参数
```

指针和引用的大小是固定的（4/8 字节），编译器不需要知道结构体内部长什么样就能处理它们。**只有当你需要"按值"创建结构体变量、访问其成员、或计算 sizeof 时，才必须看到完整定义。**

### 为什么不直接 include `<linux/videodev2.h>` ？

1. **避免头文件"传染"（最核心）**：头文件会被很多 `.cpp` 间接 include。如果 `capture.h` 直接 include V4L2 头文件，那么每一个间接包含 `capture.h` 的文件都会被强制拽入 V4L2 内核头。而 V4L2 头文件依赖大量内核头（`asm/types.h`、`linux/ioctl.h` 等），还可能与其他库（Qt、OpenCV、libjpeg）的头文件发生**宏名冲突**——比如 `V4L2_PIX_FMT_*`、`V4L2_CID_*` 被重复定义。这正是注释里说的"避免引入冲突"。
2. **加快编译**：`linux/videodev2.h` 很大，include 一次就要重新解析全部内容。只在真正需要的 `capture.cpp` 里 include，其余文件免于重复解析。
3. **解耦**：`capture.h` 作为**对外接口**，不应暴露 V4L2 这个实现细节。只看到函数签名和常量就够了，调用者（如 `main.cpp`）完全不需要知道内部用了 V4L2 结构体。

### 常量为什么反而要"保留"在头文件里？

既然不用 V4L2 头文件，`V4L2_PIX_FMT_MJPEG`、`V4L2_CID_BRIGHTNESS` 这些宏就没地方来了。所以代码把它们**以常量形式复制进头文件**（第 224-235 行）：

```cpp
static constexpr uint32_t V4L2_PIX_FMT_YUYV  = 0x56595559;
static constexpr uint32_t V4L2_PIX_FMT_MJPEG = 0x47504A4D;
...
static constexpr uint32_t V4L2_CID_BRIGHTNESS = 0x00980900;
```

这些值是从 `linux/videodev2.h` 里"抄"过来的**数字常量**（FOURCC 码、控件 ID），本质是魔法数字，不需要任何结构体定义就能使用。放在头文件里，是为了让**调用者能直接用 `CameraCapture::V4L2_PIX_FMT_MJPEG` 这个有意义的名字**（见 `capture.h` 第 16 行的示例用法），而不必自己记十六进制数。

> 面试小坑：`static constexpr` 在 C++17 下是隐式 `inline` 的，不会有 ODR/链接问题；如果项目按 C++14 编译，类内 `static constexpr` 成员还需在 `.cpp` 里额外定义，否则可能链接报错。可作为追问点自查。

### 一句话总结

```
capture.h（对外接口）:  前向声明 4 个 V4L2 结构体（只提名字，不引定义）
                      + 复制 9 个 V4L2 数字常量（3 个 FOURCC + 6 个控件 ID）
capture.cpp（实现）:    #include <linux/videodev2.h>   ← 唯一真正看得到完整定义的地方
```

**收益**：`main.cpp`、`processor.h`、`types.h` 等文件都不用碰 V4L2 内核头文件 → 编译更快、无宏冲突、接口更干净。

### 面试追问自测

**Q1：前向声明什么时候会"不够用"？**
**A**：当需要按值使用结构体、访问其成员、`sizeof`、或把该类型传给需要完整定义的函数时。例如 `capture.cpp` 里 `struct v4l2_format fmt = {};` 必须出现在 include 之后。所以这个技巧只适用于"头文件里只用指针/引用"的场景。

**Q2：`struct v4l2_format;` 和 `struct v4l2_format {};` 的区别？**
**A**：前者是声明（不定义），后者是定义（空结构体，大小是 1）。用错会导致"对不完整类型的无效使用"编译错误。

**Q3：如果未来接口需要暴露 `struct v4l2_buffer` 的值类型返回值怎么办？**
**A**：那就不能再前向声明，必须在头文件里 include V4L2 头文件，或者改用不透明指针（Pimpl 惯用法）继续隔离——这也是一个可提出的重构方向。

## 1.5 核心类 / 函数列表与调用关系

```
main.cpp（真实相机模式）
  │
  ├─ new CameraCapture()
  │    init("/dev/video0")
  │    enumFormats → 检测 YUYV/MJPEG 支持
  │    setFormat(640,480, pixfmt)          ← 驱动可能调整实际分辨率，须回读
  │    queryControl/getControl             ← 亮度/对比度/白平衡/曝光
  │    setControl(V4L2_CID_EXPOSURE_AUTO, 1)  ← 强制手动曝光保帧率
  │    enumFrameRates/getFramerate         ← 决定 GUI 帧率滑块范围
  │    startCapture()                      ← 内部: reqbufs→querybuf→mmap→qbuf→streamon
  │
  ├─ 采集线程（std::thread）
  │    getFrame(&fb, 1000)                 ← select 超时 + DQBUF
  │    （软件帧率节流，未到时间直接 putFrame 丢弃）
  │    frameData.assign(fb.data, ...)      ← ★深拷贝，立即归还缓冲
  │    putFrame(&fb)                       ← 反查索引 + QBUF
  │    通知处理线程（条件变量 procCv）
  │
  ├─ 处理线程（std::thread）
  │    wait(procCv) → 从 g_state 拷贝本地帧
  │    YUYV 模式 → VideoProcessor::encodeYUYVtoJPEG()
  │    → mjpegServer->updateFrame() / rtspServer->feedFrame() / storage->writeRecordFrame()
  │
  └─ Qt 主线程（QTimer 33ms）
       gui.setFrame(data, len, w, h, format)  ← 内部再做 YUYV→RGB24 供 QImage
```

**关键设计信号**（背诵版）：
1. 采集线程路径上只有 `DQBUF → 拷贝 → QBUF` 三个 O(1) 操作，**CPU 密集/阻塞操作一律不在取帧路径上**。
2. V4L2 mmap 内存**不可长期持有**，必须尽快深拷贝后归还（4 缓冲池容易耗尽）。
3. 一次采集，四路消费（GUI/HTTP/RTSP/存储），编码结果共享，避免重复计算。

---

# 第二部分 分块代码详解（含面试追问）

## 2.1 块一：设备初始化与能力查询

### 代码讲解

`init()` 的核心是「先释放、后打开、再校验」的防御式逻辑：

```cpp
int CameraCapture::init(const char* device) {
    if (m_fd >= 0) {                    // 幂等性：重复 init 先释放旧资源
        LOG_WRN("Camera already initialized, releasing first");
        release();
    }
    int ret = openDevice(device);       // open(O_RDWR | O_NONBLOCK) 后清除 NONBLOCK
    if (ret < 0) return ret;
    ret = queryCapability();            // 校验 VIDEO_CAPTURE + STREAMING 能力位
    if (ret < 0) { close(m_fd); m_fd = -1; return ret; }
    return 0;
}
```

`openDevice()` 的一个细节值得注意：

```cpp
m_fd = open(device, O_RDWR | O_NONBLOCK, 0);   // ① 先以非阻塞打开（防御某些设备 open 阶段阻塞）
int flags = fcntl(m_fd, F_GETFL, 0);
if (flags >= 0) fcntl(m_fd, F_SETFL, flags & ~O_NONBLOCK);  // ② 清除 O_NONBLOCK → 转为阻塞（后续用 select 超时取帧）
```

**设计意图**：先以 `O_NONBLOCK` 打开，避免某些设备在初始化阶段（如 sensor 复位）阻塞 open 调用；随后立即清除 NONBLOCK，因为本模块采用 `select + DQBUF` 的方式管理阻塞与超时，而不是依赖 fd 的非阻塞标志。这是一种"防御式 + 职责分离"的写法。

`queryCapability()` 检查两个能力位：

```cpp
if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) return -ENODEV;  // 不是采集设备
if (!(cap.capabilities & V4L2_CAP_STREAMING))     return -ENOSYS;   // 不支持流式 I/O
```

### 潜在坑点

- **重复 init 泄漏**：如果 `init` 不做幂等处理，第二次调用会直接覆盖 `m_fd` 造成 fd 泄漏。这里通过"先 release"解决。
- **`queryCapability` 失败路径**：`init` 在 `queryCapability` 失败时手动 `close(m_fd); m_fd = -1;`，保证成员状态一致，防止析构时对已关闭 fd 二次操作。
- **错误码风格**：统一返回 `-errno`（负的错误码），调用方用 `< 0` 判断失败。这是嵌入式 C/C++ 常见约定，区别于 Qt 的 `bool` 或 C++ 异常。

### 面试追问与应答

**Q1：为什么 open 时用 `O_NONBLOCK`，紧接着又用 fcntl 把它关掉？**
**A**：两种目的。第一，某些 USB 摄像头驱动在 open 阶段可能阻塞（例如 sensor 尚未就绪、固件下载），以 NONBLOCK 打开可以避免 init 卡死，属于防御式编程。第二，我们后续的"带超时取帧"是通过 `select(fd, ..., timeout)` 实现的，fd 本身保持阻塞模式即可，两者不冲突。如果 fd 保持 NONBLOCK，`DQBUF` 在没有帧时会直接返回 `EAGAIN`，语义上反而要额外处理。

**Q2：`QUERYCAP` 为什么要校验 `V4L2_CAP_VIDEO_CAPTURE` 和 `V4L2_CAP_STREAMING` 两个能力位？**
**A**：`/dev/video*` 节点可能是 capture、output、overlay、radio 等不同类型。校验 capture 位是保证"它确实能采集视频"；校验 streaming 位是保证"它支持我们采用的 mmap 流式 I/O 路径"。如果只支持 read/write（`V4L2_CAP_READWRITE`），我们的缓冲池方案就要换一种实现，所以必须提前拒绝。

**Q3：错误码为什么设计成 `-errno` 而非 bool？**
**A**：调用方需要区分失败原因——设备不存在（`ENODEV`）、无权限（`EACCES`）、设备忙（`EBUSY`）处理策略完全不同。返回 `-errno` 既保留了 errno 的语义，又避免使用全局 `errno` 的线程不安全问题（每个线程各自有 errno，但显式返回值更清晰、可空指针传递）。代价是调用方要记得判断 `< 0`，这也是后续所有模块统一遵循的约定。

**Q4：`release()` 是线程安全的吗？如果在采集线程还在 `getFrame` 时调用会发生什么？**
**A**：`CameraCapture` 自身**不是线程安全的**，头文件注释明确"建议作为单例或由主线程管理生命周期"。如果采集线程正在 `getFrame`（阻塞在 select 或 DQBUF）时主线程调用 `stopCapture`，会出现对 `m_fd` 的并发 ioctl。这正是 main.cpp 中引入 `g_state.paused + pauseCv + pausedAck` 暂停机制的根因：**切分辨率/帧率前先让采集线程确认暂停**（getFrame 有 1s 超时，最多等 1.1s），确保 mmap 缓冲和 fd 无人使用后再安全 stop。这是"模块不保证线程安全 + 上层用协议保证安全"的典型配合。

---

## 2.1.1 ioctl 与 fcntl：驱动指令通道 vs fd 属性开关

> 本模块两个最常用的系统调用，面试高频辨析题。核心一句话：**`ioctl` 问"驱动怎么工作"（设备行为），`fcntl` 管"这个文件句柄怎么用"（fd 属性）**。

### 核心区别

| 维度 | `ioctl`（I/O Control） | `fcntl`（File Control） |
|------|------------------------|------------------------|
| 作用对象 | 设备本身（驱动行为） | 文件描述符（fd）的属性 |
| 适用范围 | 仅设备文件 `/dev/xxx`（字符/块设备） | **任意 fd**：普通文件、socket、管道、设备 |
| 本质 | 给驱动发**自定义命令**，每个驱动可定义海量命令 | 只有**有限的十几个标准命令**（`F_GETFL`/`F_SETFL`/`F_GETFD`/`F_SETLKW`...） |
| 第二参数含义 | 命令号（由内核宏编码，如 `VIDIOC_S_FMT`） | 标准命令常量（`F_xxx`） |
| 第三参数 | 通常是指向结构体的指针，由驱动解析 | 标志位、锁结构、fd 等，语义固定 |
| 返回 | 0 成功 / -1 失败（设 `errno`） | 视命令而定（如 `F_GETFL` 返回标志值） |

### `ioctl` 的使用（项目里的 V4L2 教科书案例）

**签名**：`int ioctl(int fd, unsigned long request, ...)`，第三个参数几乎总是指向结构体的指针。

项目 `capture.cpp` 的用法模式高度统一：**填结构体 → 传指针 → 查 `errno`**：

```cpp
// 1. 查询设备能力
struct v4l2_capability cap;
memset(&cap, 0, sizeof(cap));
if (ioctl(m_fd, VIDIOC_QUERYCAP, &cap) < 0) {
    return "QUERYCAP failed";
}

// 2. 设置格式
struct v4l2_format fmt;
fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
fmt.fmt.pix.width  = width;
fmt.fmt.pix.height = height;
fmt.fmt.pix.pixelformat = pixfmt;
if (ioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0) {
    LOG_ERR_("VIDIOC_S_FMT failed: %s", strerror(errno), ...);
}

// 3. 开始/停止流
enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
if (ioctl(m_fd, VIDIOC_STREAMON, &type) < 0) { ... }
```

要点：
- **命令号是内核写死的宏**（`VIDIOC_S_FMT`、`VIDIOC_QUERYCAP` 等），由 `linux/videodev2.h` 定义，你只能"选"，不能自定义。
- 命令号内部**编码了方向与大小**（`_IOR` 读 / `_IOW` 写 / `_IOWR` 读写），驱动靠它决定"从指针读入参数"还是"往指针写结果"。
- 所以同一个调用：`VIDIOC_S_CTRL` 是"把结构体写进内核"，`VIDIOC_G_CTRL` 是"从内核读出到结构体"。
- **失败一律返回 -1 并设置 `errno`**，必须查 `strerror(errno)`，这是驱动排障的第一信息来源。

### `fcntl` 的使用（项目里两种典型用法）

**签名**：`int fcntl(int fd, int cmd, ...)`。

**用法 1：读写 fd 状态标志（`F_GETFL` / `F_SETFL`）**——项目最常用，模式固定为 **GET 再 SET（读改写）**，避免覆盖其它标志：

```cpp
// capture.cpp：打开时先加 O_NONBLOCK，随后清掉转阻塞
int flags = fcntl(m_fd, F_GETFL, 0);
if (flags >= 0) {
    fcntl(m_fd, F_SETFL, flags & ~O_NONBLOCK);   // 清掉 NONBLOCK 位，再写回
}

// rtsp_server.cpp / mjpeg_server.cpp / control.cpp：置为非阻塞
int flags = fcntl(fd, F_GETFL, 0);
if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);   // 加上 NONBLOCK 位
```

注意三点：
- **必须先 `F_GETFL` 读回再改**：若直接 `F_SETFL` 传新值，会**覆盖原有标志**（如 `O_APPEND`、`O_RDWR`），这是经典 bug。
- 能改的只有**文件状态标志**中的一小部分（`O_NONBLOCK`、`O_APPEND`、`O_ASYNC`、`O_DIRECT` 等），`O_RDWR` 这类访问模式改不了。
- `F_SETFL` 可以作用在**任意 fd** 上（socket 设非阻塞就是这么做），这是它比 `ioctl` 通用之处。

**用法 2：fd 描述符标志 / 文件锁**（次要）：

```cpp
int fdflags = fcntl(fd, F_GETFD, 0);       // 如 FD_CLOEXEC 位
fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);  // exec 后自动关闭

struct flock lock = { F_WRLCK, SEEK_SET, 0, 0, 0 };
fcntl(fd, F_SETLKW, &lock);                // 文件锁（进程间互斥）
```

### 为什么本模块两种都要用？

回到 `capture.cpp` 的 open 序列，正好演示了两者的分工：

```cpp
m_fd = open(device, O_RDWR | O_NONBLOCK, 0);   // open 时初始为非阻塞
// 之后：
ioctl(m_fd, VIDIOC_QUERYCAP, &cap);   // ① 用 ioctl 问驱动"你是谁"（设备能力）
fcntl(m_fd, F_GETFL, ...);            // ② 用 fcntl 改 fd 的阻塞属性（与驱动无关）
ioctl(m_fd, VIDIOC_S_FMT, ...);       // ③ 用 ioctl 配置驱动采集格式
ioctl(m_fd, VIDIOC_REQBUFS, ...);     // ④ 用 ioctl 申请驱动侧 buffer
```

- **`fcntl` 管"fd 层"**：阻塞/非阻塞、append、close-on-exec、文件锁——这些是内核 **VFS 层**提供的，与具体设备无关，所以 socket、管道、设备都通用。
- **`ioctl` 管"驱动层"**：分辨率、帧率、曝光、申请 buffer、启停流——这些**只有具体驱动才懂**，通用内核无法定义，命令号由驱动头文件（`videodev2.h`）定义。

### 面试易混淆点

1. **`O_NONBLOCK` 用哪个设置？** 用 `fcntl`（fd 属性，通用）；`ioctl` 里没有"非阻塞"这个概念。
2. **`ioctl` 命令能自己发明吗？** 驱动开发者可以自定义，但用户态程序只能用驱动导出的宏（且要经 `_IOC` 宏校验方向/大小；命令未实现时返回 `ENOTTY`）。
3. **返回值陷阱**：`fcntl(F_GETFL)` 失败返回 -1；判断 `< 0` 即可（项目代码均如此）。
4. **改阻塞属性为什么不重新 open？** 因为 `open` 会重新初始化设备状态（V4L2 会重置 buffer），而 `fcntl` 是轻量原地修改，代价极小。

---

## 2.2 块二：格式 / 帧率 / 分辨率设置

### 代码讲解

`setFormat()` 最核心的经验是「**S_FMT 是建议值，驱动可能调整，必须回读**」：

```cpp
if (ioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0) { ... return -errno; }

// 读取实际设置的值（驱动可能调整了分辨率）
m_width  = static_cast<int>(fmt.fmt.pix.width);
m_height = static_cast<int>(fmt.fmt.pix.height);
m_pixfmt = fmt.fmt.pix.pixelformat;
// 检查 bytesperline，确认无 padding 问题
LOG_INF("Format set: %dx%d, ..., stride=%d", ..., fmt.fmt.pix.bytesperline);
```

`setFramerate()` 同样做"请求 vs 实际"比对并告警：

```cpp
if (ioctl(m_fd, VIDIOC_S_PARM, &parm) < 0) { ... return -errno; }
int reqFps = denominator / numerator;
int actFps = parm.parm.capture.timeperframe.denominator
           / parm.parm.capture.timeperframe.numerator;
if (actFps != reqFps)
    LOG_WRN("requested %d fps, driver adjusted to %d fps", reqFps, actFps);
```

`enumFrameRates()` 处理 `DISCRETE` 与 `STEPWISE` 两类帧率枚举，且 STEPWISE 限制最多 20 个离散值，最后排序 + 去重；枚举失败时回退到通用范围 1~120。

### 潜在坑点

- **STREAMON 期间调用 `S_FMT` 会返回 `EBUSY`**：代码用 `if (m_streaming) return -EBUSY;` 主动拦截，而 main.cpp 的帧率/分辨率变更回调则严格按 `暂停 → stopCapture → setFormat/S_PARM → startCapture → 恢复` 的序列执行。
- **驱动静默调整分辨率**：比如请求 320x240 驱动给 352x288（对齐/步进限制）。若用请求值分配缓冲区会越界，所以必须用回读值 `m_width/m_height`。
- **`bytesperline`（stride）**：某些设备行有 padding，`width*2 != bytesperline`。代码打日志"确认无 padding 问题"，实际处理 YUYV 时默认 stride 紧凑；若遇到带 padding 的设备需按 stride 逐行处理，这是本模块在通用性上的一个简化假设。
- **帧率联动**：S_PARM 的成功不代表驱动一定生效，所以 main.cpp 在帧率回调里**无论硬件是否成功都设置软件节流**（`g_state.targetFps`）兜底——"软件丢帧兜底"是工程上非常稳妥的做法。

### 面试追问与应答

**Q1：为什么 `setFormat` 之后必须重新读回 `fmt.fmt.pix.width/height/pixelformat`？**
**A**：V4L2 规范中 `VIDIOC_S_FMT` 是"尽力设置"：驱动可能把分辨率对齐到硬件步进（如 4 的倍数）、把像素格式改成它更擅长的相近格式（例如把 RGB32 换成 YUYV）。如果不回读，后续 mmap 长度、FrameBuffer 的 width/height/format 都会基于错误值，轻则花屏、重则缓冲区越界。回读是 V4L2 编程的**铁律**。

**Q2：为什么 `setFramerate` 的参数是 `numerator/denominator` 而不是直接一个 fps 整数？**
**A**：V4L2 的 `timeperframe` 语义是"每帧耗时 = numerator/denominator 秒"，用分数可以表达非整帧率，例如 29.97fps = 1001/30000 秒。直接传整数会丢失精度。内部换算成整数 fps（`denominator/numerator`）仅用于日志和 UI 展示。

**Q3：分辨率切换的完整正确流程是什么？为什么不能直接 S_FMT？**
**A**：正确序列：① 暂停采集线程（确保无人持有 mmap 缓冲/占用 fd）→ ② `STREAMOFF` → ③ `VIDIOC_S_FMT` 并回读实际值 → ④ 重新 `REQBUFS + mmap + QBUF + STREAMON` → ⑤ 恢复采集线程。因为缓冲区的 mmap 长度与分辨率强相关，只改 S_FMT 不重建缓冲池会导致内核写超出旧缓冲长度。本模块 `startCapture()` 每次都重建缓冲池，天然支持这个流程。

**Q4：帧率设置失败（驱动不支持）时，你的工程兜底方案是什么？**
**A**：双保险。第一，`setFramerate` 失败只告警不致命；第二，main.cpp 无条件把目标帧率写入 `g_state.targetFps`，采集线程在 getFrame 后按 `1000/throttleFps` 毫秒做软件节流——不到时间间隔的帧直接 `putFrame` 丢弃（这等价于"隔帧采样"）。这样即使硬件只支持固定帧率，上层也能以软件方式呈现目标帧率，代价是中间帧被丢弃而非重新编码。

---

## 2.3 块三：mmap 帧缓冲池与流控制

### 代码讲解

`startCapture()` 是状态机核心：申请缓冲 → mmap → 全部入队 → STREAMON，任何一步失败都要**回滚已分配资源**：

```cpp
int CameraCapture::startCapture() {
    if (m_streaming) { LOG_WRN("Already streaming"); return 0; }   // 幂等
    int ret = requestBuffers(kDefaultBufferCount);  // REQBUFS(4)
    if (ret < 0) return ret;
    ret = mapBuffers();                             // 逐个 QUERYBUF + mmap
    if (ret < 0) return ret;
    ret = queueAllBuffers();                        // 全部 QBUF
    if (ret < 0) { unmapBuffers(); return ret; }
    if (ioctl(m_fd, VIDIOC_STREAMON, &type) < 0) { unmapBuffers(); return -errno; }
    m_streaming = true; ...
}
```

`unmapBuffers()` 里藏着最容易忽略的一步——**`REQBUFS(0)` 释放驱动侧缓冲**：

```cpp
// 释放 V4L2 驱动侧缓冲区资源，否则后续 VIDIOC_S_FMT 会返回 EBUSY
if (m_fd >= 0) {
    struct v4l2_requestbuffers req; memset(&req, 0, sizeof(req));
    req.count = 0; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; req.memory = V4L2_MEMORY_MMAP;
    ioctl(m_fd, VIDIOC_REQBUFS, &req);
}
```

### 潜在坑点

- **缓冲数 < 2 直接失败**：`requestBuffers` 检查 `req.count < 2` 返回 `-ENOMEM`。因为至少需要"一帧被消费者持有、一帧在队列中"才能乒乓流转，单缓冲必然导致采集停顿或覆盖。
- **mmap 失败回滚**：`mapBuffers` 中任何一个 `mmap` 返回 `MAP_FAILED` 都会中断并返回，靠 `startCapture` 的失败路径调用 `unmapBuffers` 统一回滚（已映射的 munmap，未映射的跳过）。
- **`delete[] m_buffers` 与 fd 关闭顺序**：release 先 `unmapBuffers` 再 close fd，顺序不能反——先关 fd 会导致 munmap 时 fd 已失效（虽然 munmap 不依赖 fd，但语义上要先把映射拆除）。
- **`MAP_FAILED` 的判断**：`mmap` 失败返回 `(void*)-1`，代码里 `m_buffers[i].start == MAP_FAILED` 的判断不能省略，否则会把 -1 当有效地址，`unmapBuffers` 里 `munmap((void*)-1)` 会失败。

### 面试追问与应答

**Q1：为什么用 mmap 而不是 read() 读取帧数据？**
**A**：read() 路径是"内核驱动缓冲区 → 用户态临时缓冲 → 用户态拷贝"，至少两次内存拷贝；mmap 将驱动侧的 DMA 缓冲区直接映射进用户态虚拟地址空间，`DQBUF` 拿到的是**同一块物理内存**的指针，省去全部拷贝。对 640x480 的 MJPEG（约 30~100KB/帧）@30fps，每秒能省下大量内存带宽——在单核 792MHz 的 i.MX6ULL 上，这是能否跑满 30fps 的关键。

**Q2：为什么默认缓冲池是 4 个？改成 8 个或 2 个会怎样？**
**A**：缓冲池是"延迟吸收器"。取帧方处理一帧的时间如果大于摄像头产生一帧的间隔，队列就会向耗尽方向滑动。4 个缓冲允许消费者平均最多滞后 3 帧才开始丢帧；改成 8 个提高容错但多占约 4×帧大小内存（对 MJPEG 640x480 约多 200~400KB，可接受）；改成 2 个是最低要求，一旦某次处理抖动（如系统调度延迟）就立刻无缓冲可取、掉帧。本项目"采集线程只做 O(1) 拷贝、重活交给处理线程"的策略下，4 个绰绰有余。

**Q3：`REQBUFS(0)` 的作用是什么？不调用会怎样？**
**A**：`REQBUFS(0)` 告诉驱动释放所有已申请的缓冲区资源。如果不释放，驱动侧缓冲区仍被占用，**后续 `VIDIOC_S_FMT` 修改格式会返回 `EBUSY`**。所以 `unmapBuffers` 里必须补这一步，否则"停流 → 改分辨率 → 再启动"的重配置流程会神秘失败。这是 V4L2 编程的经典坑。

**Q4：`STREAMON` 失败时已经 mmap 的缓冲怎么处理？**
**A**：`startCapture` 的失败路径全部收敛到 `unmapBuffers()`：munmap 每个已映射区域 → `delete[] m_buffers` → `REQBUFS(0)` 释放驱动缓冲 → 置空指针。这样无论在哪一步失败，资源都不会泄漏，对象状态也能回到可重新 startCapture 的干净状态。**任何"分配了 N 个资源、中途失败"的流程都要有统一的回滚入口**，这是嵌入式资源管理的通用原则。

---

## 2.4 块四：帧捕获循环（getFrame / putFrame）

### 代码讲解

`getFrame()` = 带超时的 `DQBUF` + 索引校验 + 填充 FrameBuffer：

```cpp
int CameraCapture::getFrame(FrameBuffer* buf, int timeout_ms) {
    if (!m_streaming || m_fd < 0) return -EIO;
    struct v4l2_buffer vbuf; memset(&vbuf, 0, sizeof(vbuf));
    int ret = dequeueBuffer(vbuf, timeout_ms);      // select 超时 → DQBUF
    if (ret < 0) return ret;
    if (vbuf.index >= (unsigned)m_nbuffers || !m_buffers) {   // 索引越界防御
        LOG_ERR_("Invalid buffer index"); return -EINVAL;
    }
    buf->data     = (uint8_t*)m_buffers[vbuf.index].start;   // 零拷贝：直接指向 mmap
    buf->length   = vbuf.bytesused;                          // 驱动回填的实际字节数
    buf->width    = m_width; buf->height = m_height;
    buf->format   = (m_pixfmt == V4L2_PIX_FMT_YUYV) ? FMT_YUYV : FMT_MJPEG;
    buf->index    = m_frameCount++;
    buf->timestamp = std::chrono::steady_clock::now();
    updateFPS();
    return 0;
}
```

`putFrame()` 用**指针反查索引**归还缓冲：

```cpp
int idx = -1;
for (int i = 0; i < m_nbuffers; ++i)   // 遍历池，找 data 指针对应的槽位
    if (m_buffers[i].start == buf->data) { idx = i; break; }
if (idx < 0) { LOG_ERR_("buffer pointer not found in pool"); return -EINVAL; }
// QBUF(idx) 归还；m_buffers[idx].queued = true;
```

`dequeueBuffer()` 用 `select` 实现超时，避免 DQBUF 无限期阻塞：

```cpp
if (timeout_ms > 0) {
    FD_SET(m_fd, &fds); tv.tv_sec = timeout_ms/1000; tv.tv_usec = (timeout_ms%1000)*1000;
    int ret = select(m_fd + 1, &fds, nullptr, nullptr, &tv);
    if (ret < 0) return -errno;     // 注意：EINTR 未做重试
    if (ret == 0) return -ETIMEDOUT;
}
ioctl(m_fd, VIDIOC_DQBUF, &buf);
```

### 潜在坑点

- **配对契约**：`getFrame` 后**必须** `putFrame`，否则缓冲池 4 个槽位很快耗尽，`DQBUF` 永久阻塞。main.cpp 采集线程把深拷贝放在 getFrame 与 putFrame 之间，就是为满足"尽快归还"。
- **`putFrame` 用 O(n) 扫描反查索引**：正确但非最优。若 FrameBuffer 直接携带 `buffer_index` 字段（V4L2 的 `vbuf.index` 本来就有），可 O(1) 归还。当前实现多一次遍历，属于可优化点（面试主动提及加分）。
- **`select` 被信号中断（EINTR）**：当前直接返回 `-errno`，采集线程 `continue` 重试。若要求更稳可改为循环重试 select。这是健壮性可改进点。
- **`buf->length = vbuf.bytesused`**：使用驱动回填的实际字节数而非缓冲池长度。MJPEG 帧长度每帧不同，用池长度会导致多读垃圾数据；用 bytesused 才是真实有效数据。
- **外部不能 free `buf->data`**：它指向 mmap 内存，头文件注释明确"不应外部释放"。一旦外部 free 会破坏 V4L2 映射，这是契约问题，靠文档约束 + 上层深拷贝规避。

### 面试追问与应答

**Q1：getFrame/putFrame 为什么必须成对调用？如果某一帧被消费者遗忘会发生什么？**
**A**：缓冲池是固定 4 个槽位的"租借"模型：getFrame 借出一个槽位，putFrame 归还。遗忘归还 = 槽位永久流失，4 帧后所有缓冲都在消费者手里，驱动无处写入，`DQBUF` 永久阻塞，整个采集链冻结。因此设计上要求：**持有 mmap 缓冲的时间越短越好**。main.cpp 的做法是"拷贝完立刻归还"，把帧数据复制到普通堆内存 `g_state.frameData` 后再分发，彻底解除对 mmap 生命周期的手动管理。

**Q2：为什么 putFrame 要遍历缓冲池找索引，而不是直接记录？**
**A**：当前 FrameBuffer 结构体里没有记录 V4L2 buffer index，所以只能用 `data` 指针与池中 `start` 比对来反推。这是功能正确的，但存在两个隐患：一是 O(n) 开销，二是依赖指针唯一性。改进方案是给 FrameBuffer 增加 `pool_index` 字段（V4L2 `vbuf.index` 本就有值），putFrame 直接 `QBUF(vbuf.index)`，同时 `getFrame` 里顺带校验索引合法性。我在设计初版时用指针匹配是为了接口简洁，现在看属于"可以更好"的地方。

**Q3：select 超时 1 秒，超时后返回 -ETIMEDOUT，上层如何处理？**
**A**：main.cpp 采集线程对 `getFrame < 0` 的处理是 `continue` 重试（除非 `!running` 退出）。超时本身不代表摄像头故障——可能是驱动在重协商、或系统调度导致帧间隔被拉长。但连续长时间超时（比如几十秒）就应视为设备异常，可以升级处理（日志告警、重启采集）。当前代码是"静默重试 + 依赖上层 watchdog"，是一个可讨论的简化。

**Q4：这套"深拷贝 + 立刻归还"的设计和"引用计数持有 mmap"相比，优劣是什么？**
**A**：深拷贝代价是每帧一次内存拷贝（MJPEG 640x480 约 30~100KB，DDR3 上约几十微秒，可接受），但换来的是**无共享、无锁**——消费者拿到独立数据，彻底消除悬垂指针和数据竞争，这是嵌入式调试成本最低的方案。引用计数（如共享 ptr 持有 mmap 槽位）能省拷贝，但引入"槽位何时可回收"的复杂所有权逻辑，且 mmap 内存不可自由释放/拷贝。**在 792MHz 单核上，多一次内存拷贝换来的确定性和可调试性，性价比远高于省那次拷贝**。这是我明确做的工程取舍。

---

## 2.5 块五：FPS 统计与帧率控制

### 代码讲解

`updateFPS()` 采用"每 30 帧计算一次平均帧率"的滑动窗口：

```cpp
if (m_frameCount % 30 == 0) {
    double elapsed = now - m_lastFpsTime;
    if (elapsed > 0.0) m_currentFps = 30.0 / elapsed;
    m_lastFpsTime = now;
}
```

帧率控制（在 main.cpp 采集线程）分两层：
1. **硬件层**：`VIDIOC_S_PARM` 请求帧率（需先停流，见 2.2）。
2. **软件层兜底**：`g_state.targetFps` 生效，采集线程按 `1000/throttleFps` 毫秒节流，未到时间的帧直接 putFrame 丢弃。

```cpp
if (elapsedMs < minIntervalMs) { capture->putFrame(&fb); continue; }  // 丢帧
```

### 面试追问与应答

**Q1：为什么用"每 30 帧平均"而不是每秒瞬时测量？**
**A**：瞬时测量（比如每 1 秒统计帧数）受第一帧相位影响大、抖动明显；每 30 帧滑动窗口能平滑短时抖动，且无需维护 1 秒定时器，实现极简（两个成员变量）。缺点是最多滞后 30 帧才反映帧率变化，对"实时反馈"场景略钝，但对状态栏展示完全够用。GUI 侧的显示 FPS 也采用同样思路（每 30 次刷新算一次平均），两处保持一致。

**Q2：`std::chrono::steady_clock` 和 `system_clock` 的区别？为什么用 steady_clock？**
**A**：`steady_clock` 是单调时钟，**不受系统时间调整影响**（如 NTP 校时、手动改时间不会让它回跳）；`system_clock` 是墙上时钟，可能回拨。测 FPS、算时间差必须用单调时钟，否则 NTP 校时瞬间会算出负帧率或极大帧率。这是时间测量的正确姿势。

**Q3：软件节流丢帧时，实际是"隔几帧取一帧"吗？为什么不用降低硬件帧率？**
**A**：是的，等价于周期性抽样。用软件节流而非 S_PARM 是因为：① 很多 UVC 摄像头 `VIDIOC_S_PARM` 并不真正生效（只报告一个离散帧率，见 `enumFrameRates` 的回退逻辑）；② 反复 stop/start 流来改硬件帧率有额外开销和短暂断流，不适合频繁调节；③ 软件节流粒度细（精确到毫秒）、零状态切换。缺点是摄像头仍在满帧率采集，CPU/内存带宽没有省下——但对于推流场景，采集本身开销很小，收益主要在网络带宽。

**Q4：采集线程里 `lastOutputTime` 和 `throttleFps` 的初始化顺序，为什么每次循环都重新读 `targetFps`？**
**A**：`targetFps` 由 GUI 回调随时写入（`g_state.targetFps.store(fps)`），采集线程每次循环 `load()` 是为了**及时感知用户调节**，而不是启动时固化。`lastOutputTime` 初始化为线程启动时刻，保证第一帧不会被误判为"未到时间"而丢弃。这种"主线程写、采集线程读"的 `std::atomic<int>` 是嵌入式里最常见的轻量通信方式，无锁、无阻塞。

---

## 2.6 块六：颜色空间转换（YUYV → RGB24 / RGB565）

### 代码讲解

核心是 **BT.601 定点运算**，避免浮点：

```cpp
// YUYV 每 4 字节 = [Y0, U, Y1, V]，代表 2 个像素（U/V 共享）
int y0 = yuyv[si];  int u = yuyv[si+1] - 128;
int y1 = yuyv[si+2]; int v = yuyv[si+3] - 128;

int r0 = y0 + ((v * 359) >> 8);       // 1.402 ≈ 359/256
int g0 = y0 - ((u * 88) >> 8) - ((v * 183) >> 8);   // 0.344≈88/256, 0.714≈183/256
int b0 = y0 + ((u * 454) >> 8);       // 1.772 ≈ 454/256
```

`yuyvToRgb24()` 通过编译期宏分发到 NEON 版：

```cpp
#ifdef __ARM_NEON
    extern void yuyv_to_rgb24_neon(const uint8_t*, uint8_t*, int, int);
    yuyv_to_rgb24_neon(yuyv, rgb, w, h);
    return;
#endif
    // x86 / 无 NEON 退路: 标量 C++ 实现
```

`yuyvToRgb565()` 面向 16-bit LCD framebuffer（5-6-5 位布局，小端输出）。

### 潜在坑点

- **U/V 必须先减 128**：YUYV 的 U/V 是带偏置的（128 为中心），不偏移直接乘系数会整体偏色。
- **系数定点化有精度损失**：`359/256=1.4023`（理论 1.402），`454/256=1.7734`（理论 1.772），误差在肉眼不可见范围，但若做严格色彩还原（如打印）应提高定点精度或使用查表。
- **手写 clip 的开销**：标量版每个通道都做 `x<0?0:(x>255?255:x)` 分支，分支预测在乱序差异大的像素上会 miss。NEON 版用 `vqmovun`（饱和转换）免分支，这是加速之一。
- **奇数宽度**：YUYV 要求偶数宽度，`yuyvBufferSize` 里 `((w+1)&~1)*h*2` 做了向上对齐，但真正的采集侧由驱动保证偶数宽。

### 面试追问与应答

**Q1：为什么用定点数而不是浮点或查表？**
**A**：三层考虑。浮点在 i.MX6ULL 上虽然没有传统协处理器那么慢（Cortex-A7 有 VFPv4），但每像素多次乘加+类型转换仍有可观开销，且行为不可控；定点用整数乘移即可，**与 NEON 整数 SIMD 天然兼容**（这是关键，浮点 SIMD 在 ARMv7 上是可选特性，且精度行为不同）；查表虽然最快（一次索引+一次加法），但表大（Y、U、V 三维全表 >1MB 超内存预算，降维拆分表又复杂），且牺牲精度。定点是"精度、速度、实现复杂度、内存"四者的最优平衡。

**Q2：BT.601 和 BT.709 的区别？为什么选 BT.601？**
**A**：两者是 YUV↔RGB 的色域转换系数。BT.601（SDTV）和 BT.709（HDTV）的 R/G/B 系数不同（如 R 通道 1.402 vs 1.5748）。摄像头 YUYV 通常按 BT.601 采样（USB UVC 默认），且本项目输出目标是小屏与网络预览，色域偏差无感知。选 BT.601 是"跟随源头 + 目标场景不需要广色域"的正确选择。

**Q3：转换公式每像素都做 clip，能否预计算规避？**
**A**：clip 是必要的（定点乘加可能越界），但可以优化：NEON 用饱和指令（`vqmovun_s16` 自动钳制到 [0,255]），零分支；标量版可以预先把 Y/U/V 查表到"已展开的 RGB 系数表"来合并部分运算，但会牺牲内存。实际工程里 NEON 版已是主线，标量版只是 x86 调试兜底，性能不是主要矛盾。

**Q4：为什么 `yuyvToRgb565` 输出是小端（低字节在前）？**
**A**：RGB565 单像素 2 字节，小端 CPU 内存中低地址存低字节。LCD framebuffer 与 NEON 的 `vst1` 等指令都按内存小端写入，所以代码里 `rgb565[di++] = p0 & 0xFF; rgb565[di++] = (p0 >> 8) & 0xFF;` 先把低字节写前。如果画成大端，屏幕上红蓝会互换，这是个"看起来对、跑起来花"的经典 bug 源。

---

## 2.7 块七：NEON SIMD 加速

### 代码讲解

`processor_neon.cpp` 是实现 YUYV→RGB24 的 ARMv7 NEON 版本，核心是 **16 像素/轮**：

```
输入 32B YUYV（16 像素 = 8 宏像素）→ 输出 48B RGB24
Step1: vld2q_u8  → 将 Y 与 UV 去交织成两个寄存器（Y16, UV16）
Step2: vuzp      → 从 UV16 中分离 U8、V8 两个向量
Step3: vmovl     → 8bit 扩展 16bit（避免溢出）
Step4: BT.601 定点乘加（vdup 系数 + vmul + vshr 8）
Step5: vqmovun   → 16bit 饱和转换回 8bit（免手动 clip）
Step6: vst3_u8   → 交织写入 RGB24（R,G,B 三平面交错）
```

尾部处理：`for (; i + 1 < totalPixels; i += 2)` 用标量循环兜底，**保证任意宽度（含奇数/非 16 倍数）正确性**。

### 潜在坑点

- **`__ARM_NEON` 宏由 `-mfpu=neon` 触发**：CMake 只在 ARM 交叉编译时加 `-march=armv7-a -mfpu=neon -mfloat-abi=hard`；x86 构建下 `__ARM_NEON` 未定义，`processor.cpp` 里被 `#ifdef __ARM_NEON` 包裹的 extern 声明 + 调用分支不生效，走标量路径。注意 CMake 源列表始终包含 `processor_neon.cpp`，其向量路径只有在 ARM 交叉编译时才被真正启用。
- **尾部长度的算术 bug 风险**：`i + 15 < totalPixels` 保证向量路径一次 16 像素不越界；尾部循环 `i + 1 < totalPixels` 按宏像素（2 像素）步进。若宽度为奇数，最后一个像素的 U/V 会读取到下一行数据——YUYV 本身要求偶数宽，此边界由上游保证，但代码里未显式断言。
- **`vuzp_u8(uv_lo, uv_lo)` 的用法**：同一寄存器与自己 unzip，得到偶奇分离，再跨高低 64bit 合并出 8 个 U 与 8 个 V。这个"自 unzip"技巧是写出来易读、写错难调的典型，值得面试时展开。
- **NEON 结果与标量一致性**：两者系数、舍入方式（`+128>>8` vs 标量 `>>8`）必须一致，否则切换平台画面亮度/偏色有细微差异。`vHalf=128` 对应四舍五入，标量版无 `+128`，这里有**轻微不一致**（可讨论的精确实现差异）。

### 面试追问与应答

**Q1：NEON 为什么能加速？一次处理多少个像素？原理是什么？**
**A**：NEON 是 128 位 SIMD，`vld2q_u8` 一次加载 32 字节（去交织为 Y16 + UV16 两个 128 位寄存器）、`vst3_u8` 一次写 24 字节（8 像素 × 3 通道），主循环每轮处理 16 个像素。相比标量每轮 2 像素，指令吞吐提升约 8 倍（实测项目文档记录 ~8×）。加速来源有四点：① 一次加载/存储多个像素摊薄访存指令；② 乘加向量化；③ `vqmovun` 饱和转换免去分支 clip；④ 寄存器复用减少内存往返。

**Q2：为什么选择 `vld2/vuzp/vst3` 这套指令组合？**
**A**：YUYV 是交错布局 `[Y0,U,Y1,V,...]`，RGB24 是 `[R0,G0,B0,R1,...]`。`vld2`（de-interleave）把 Y 与 UV 通道拆开，`vuzp` 再把 U/V 拆开，`vst3`（interleave）把 R/G/B 重新交织写出——正好与两种内存布局一一对应。如果不拆开，就得手工移位拼接，指令数量翻倍。选择这套指令本质是"让 SIMD 的通道布局匹配像素格式布局"。

**Q3：宽度不是 16 的倍数时怎么保证正确？**
**A**：向量主循环按 16 像素步进，剩余不足 16 像素走标量兜底循环（`i+1<totalPixels`，每 2 像素一个宏像素）。这保证输出与输入一一对应、不越界。代价是尾部少量像素用标量慢一点，但占比通常 <5%，可忽略。**"SIMD 主循环 + 标量尾部"是所有向量化代码的通用收尾模式**。

**Q4：如何验证 NEON 版和标量版结果一致？**
**A**：项目里标准做法是：同一帧 YUYV 数据分别跑标量与 NEON，逐像素比对 RGB 差值，要求差值 ≤ 2（定点舍入差异允许 1~2），同时用若干已知色块（如纯红/纯绿/纯蓝/渐变灰）验证极值不溢出、无偏色。另外在 x86 上先验证标量逻辑正确，再在 ARM 上用 `perf` 验证加速比与数据一致。这是"算法改写必须做等价性验证"的工程纪律。

---

## 2.8 块八：MJPEG 帧边界解析

### 代码讲解

MJPEG 是连续的 JPEG 图片流，`findJPEGFrame()` 负责从流中截出完整一帧：

```cpp
// 正向找 SOI (0xFF 0xD8)
for (int i = 0; i < len - 1; ++i)
    if (data[i] == 0xFF && data[i+1] == 0xD8) { start = i; break; }
// 反向找 EOI (0xFF 0xD9)
for (int i = len - 1; i > start + 1; --i)
    if (data[i-1] == 0xFF && data[i] == 0xD9) { end = i; break; }
*jpeg_start = start;  *jpeg_len = end - start + 1;
```

### 潜在坑点

- **JPEG 数据内部可能出现 `0xFF 0xD8/0xD9` 吗？** JPEG 熵编码数据里，`0xFF` 后若跟 `00` 是填充字节（stuffing），正常的标记有独立语义。但在**未解码的原始流**中，熵编码段内 `0xFF` 后跟任意字节都可能出现，因此仅靠 `0xFFD8/0xFFD9` 定位**可能误判**。严格做法是逐段解析标记（SOF/SOS/DHT 等）确定图像数据长度。
- **多帧残留**：若缓冲里含多帧（比如 DQBUF 超长），本函数只返回第一帧边界，调用方需要 `memmove` 残余数据。V4L2 MJPEG 一个 buffer 通常恰好一帧，风险低。
- **找不到完整帧时返回 -1**，调用方应丢弃该缓冲继续 DQBUF，而不是把不完整帧交给解码器。

### 面试追问与应答

**Q1：这个函数的时间复杂度？为什么 EOI 从后往前找？**
**A**：O(len)，一次正向扫描找 SOI、一次反向扫描找 EOI。从后往前找 EOI 是因为帧尾就在缓冲末尾，反向扫描通常第一次迭代就能命中（`data[len-2]==0xFF && data[len-1]==0xD9`），平均开销极小。这是"利用已知布局做局部搜索"的工程技巧。

**Q2：JPEG 内数据里出现假的 0xFFD8/0xFFD9 会误判吗？如何处理？**
**A**：会。严格的 JPEG 解析必须按标记段推进：跳过 SOI、APPn（长度在段头 2 字节）、DQT/DHT/SOF/SOS，从 SOS 后的熵编码段长度反推图像数据结束位置，再验证 EOI。当前实现是"启发式"，在 V4L2 MJPEG（一个 buffer 一帧）场景足够可靠；若未来支持"一 buffer 多帧"或异常驱动，需要升级为完整标记解析。面试中能主动说出这个局限，比隐瞒要好得多。

**Q3：如果 `findJPEGFrame` 返回 -1（没找到完整帧），上层应该怎么办？**
**A**：把该缓冲直接 `putFrame` 归还（丢弃），不能把残缺数据送入解码/推流，否则下游 JPEG 解码器会花屏或崩溃。同时可计数统计坏帧率，若坏帧率异常升高说明摄像头输出异常或缓冲分配过小，需要告警。

---

## 2.9 块九：JPEG 编码（libjpeg-turbo）

### 代码讲解

`encodeRGBtoJPEG()` 用 `jpeg_mem_dest` 输出到内存而非文件：

```cpp
cinfo.err = jpeg_std_error(&jerr);
jpeg_create_compress(&cinfo);
jpeg_mem_dest(&cinfo, &buffer, &outlen);   // 输出到动态内存
cinfo.image_width = width; cinfo.image_height = height;
cinfo.input_components = 3; cinfo.in_color_space = JCS_RGB;
jpeg_set_defaults(&cinfo);
jpeg_set_quality(&cinfo, quality, TRUE);
jpeg_start_compress(&cinfo, TRUE);
// 逐行写入（一次一行，cache 友好，NEON 友好）
while (cinfo.next_scanline < cinfo.image_height) {
    row_pointer[0] = const_cast<JSAMPROW>(&rgb[cinfo.next_scanline * row_stride]);
    jpeg_write_scanlines(&cinfo, row_pointer, 1);
}
jpeg_finish_compress(&cinfo); jpeg_destroy_compress(&cinfo);
*jpeg_out = buffer;  // malloc'd 内存，调用者需 free
```

`encodeYUYVtoJPEG()` 先分配临时 RGB 缓冲，再调 `encodeRGBtoJPEG`。

### 潜在坑点

- **内存所有权**：`jpeg_mem_dest` 分配的 buffer 必须由**调用者 `free()`**。main.cpp 处理线程里 `if (jpeg_out) free(jpeg_out);` 在每次推流后释放，漏掉会造成每帧泄漏。
- **libjpeg 默认错误处理会 `exit()`**：坏帧/超大尺寸会让默认 error handler 直接终止进程。CMake 注释提到"自定义静默错误处理器抑制坏帧警告"，项目文档（难点八）明确这是为了防崩溃。
- **逐行写入 vs 整帧**：逐行写 `jpeg_write_scanlines(...,1)` 更省内存、对 cache 友好，是嵌入式推荐做法。
- **`quality` 参数**：影响码率与画质权衡，main.cpp 推流用 80、拍照用 85，符合"实时流求快、静态照求质"的场景差异。

### 面试追问与应答

**Q1：为什么选 libjpeg-turbo 而不是 libjpeg 或 ffmpeg？**
**A**：libjpeg-turbo 是 libjpeg 的 SIMD 加速版，在 ARM NEON 上有显著提速（DCT 和色度下采样均向量化），体积小、API 稳定、嵌入式移植成熟；纯 libjpeg 无 SIMD，ffmpeg 功能强但体积大、依赖重，对 512MB 内存、静态链接部署的板子不划算。且本项目只需"RGB→JPEG"单一能力，杀鸡不必用牛刀。

**Q2：`jpeg_mem_dest` 输出的内存为什么要调用者 free？如果一直不 free 呢？**
**A**：libjpeg 内部用 malloc 分配输出缓冲，`jpeg_destroy_compress` 不会释放它（所有权转移给调用者），文档约定调用者负责 free。每帧推流若都分配/释放一次，长期运行会因内存碎片或峰值上涨有风险——更好的做法是复用一块预分配缓冲（`jpeg_mem_dest` 支持传入已有 buffer），这是本项目"固定缓冲池、避免运行时大块分配"哲学下可以进一步强化的点。

**Q3：`quality` 取 80（推流）和 85（拍照）的依据是什么？**
**A**：JPEG 质量 75~85 是人眼感知的"甜点区"——高于 90 码率激增但感知提升趋缓，低于 70 出现可见块效应。推流场景码率即带宽成本（局域网内多客户端），取 80 兼顾画质与码率；拍照求画质细节，取 85 略高一档。另外 MJPEG 模式摄像头硬件直出的 JPEG 通常也是类似质量档，两路视觉一致性较好。

**Q4：为什么逐行写而不是一次性 `jpeg_write_scanlines` 全部行？**
**A**：逐行一次写一行，`row_pointer` 始终指向单行，避免分配整帧的 `JSAMPARRAY`（对 640x480 约 900KB），且每次 `jpeg_write_scanlines` 内部处理固定 8 行 MCU 高度，逐行调用只是把输入送进编码器缓冲，并不慢。对嵌入式内存受限场景，这是"内存换取无、速度无损"的标准写法。

---

## 2.10 块十：V4L2 相机控制（亮度/对比度/曝光/白平衡）

### 代码讲解

三层接口封装了完整的 V4L2 control 流程：

```cpp
int CameraCapture::queryControl(cid, min, max, step, def)  // QUERYCTRL 查范围
int CameraCapture::getControl(cid, value)                  // G_CTRL 读当前值
int CameraCapture::setControl(cid, value)                  // S_CTRL 写值
```

main.cpp 中的关键工程决策——**强制手动曝光保帧率**：

```cpp
// V4L2_EXPOSURE_MANUAL = 1，强制手动模式以保持帧率
if (expVal != 1) {
    capture->setControl(V4L2_CID_EXPOSURE_AUTO, 1);
    LOG_INF("Auto Exposure disabled to preserve framerate");
}
// 曝光绝对值限制：30fps 要求曝光 < 33ms
if (targetExposure > 300) targetExposure = 300;
```

### 潜在坑点

- **自动曝光 vs 帧率的耦合**：暗光下自动曝光会把曝光时间拉长到几十毫秒，导致帧率从 30fps 掉到 10fps 甚至更低——因为"一帧的曝光时间 > 帧间隔"。main.cpp 强制手动曝光并限幅 300（UVC 驱动下 V4L2 曝光绝对值通常以 100µs 为单位，300 ≈ 30ms，小于 30fps 的 33ms 帧间隔，保证曝光完成且有富余）。这是"帧率需求反推曝光上限"的典型工程权衡。
- **控件互锁**：如自动白平衡开启时，手动色温 `WHITE_BALANCE_TEMPERATURE` 可能被驱动忽略。GUI 层需要互锁逻辑（main.cpp 注释提及"互锁逻辑"）。
- **`setControl` 不校验返回值范围**：只做 ioctl 层面的失败处理；若驱动对超范围值静默钳制，日志里的 name 是"unknown"（QUERYCTRL 失败时）。

### 面试追问与应答

**Q1：自动曝光为什么会导致帧率下降？你的解决思路是什么？**
**A**：曝光时间是传感器"开门"时长，直接决定每帧的采集耗时。自动曝光在暗光下会把曝光时间拉到最大（可能 100ms+），于是帧间隔被曝光时间下限锁定，帧率自然掉到 <10fps。解决思路是"应用层对帧率有硬需求时，剥夺自动曝光的调节权"：启动时强制 `EXPOSURE_AUTO=1`（手动），并把绝对曝光值限幅在"目标帧间隔以内"（30fps → 曝光 ≤ 33ms 的量级）。这是"控制环路决策权在应用层"的设计——相机参数服务于业务目标（帧率/画质），而不是相反。

**Q2：`queryControl` 返回的范围怎么用？为什么 GUI 要设置滑块范围？**
**A**：不同摄像头驱动对同一控件（如亮度）的范围/步进/默认值完全不同（有的 0~255，有的 -64~64）。启动时 QUERYCTRL 拿到真实范围传给 GUI 设置滑块，才能保证"滑块的每个位置都是驱动接受的有效值"，避免设置无效值或滑块不可操作。这就是"设备驱动是事实来源，UI 是映射层"的原则。

**Q3：GUI 里自动白平衡和手动色温为什么需要互锁？**
**A**：V4L2 驱动的语义是：自动白平衡开启时，驱动根据场景自动调整增益和色温，手动 `WHITE_BALANCE_TEMPERATURE` 的写入会被忽略或被自动值覆盖。若不互锁，用户会看到"滑了没反应"，体验与状态都错乱。互锁方案：开启 AWB 时禁用色温滑块（GUI），关闭 AWB 时才允许手动色温，同时把当前状态回读同步到 UI。**凡是"硬件模式互斥"的控制项，UI 都要有对应的联动**。

**Q4：`setControl` 返回成功是否代表值真的生效了？**
**A**：不一定。`VIDIOC_S_CTRL` 成功只表示驱动接受了请求，但驱动可能：钳制到范围外相邻值、因当前模式忽略该控件（如自动模式下手动值无效）、延迟生效。严格验证应 `G_CTRL` 回读比对。本项目对曝光控件在初始化时做了"查询范围 → 读当前值 → 限幅 → 设置"的流程，并未严格回读比对；其余控件靠 GUI 滑块范围约束，属于性价比合适的信任边界。

---

# 第三部分 综合思考

## 3.1 与推理引擎 / 网络传输 / 显示渲染的数据流耦合与接口设计

> 说明：本项目当前没有独立的推理引擎模块（i.MX6ULL 无 NPU，设计约束里排除了推理），但"如果把 camera 模块接到推理/网络/显示"的耦合分析是面试高频题，以下用现有模块映射到通用框架。

### 当前数据流（真实代码）

```
CameraCapture（mmap 帧）                    ← 生产
    │  getFrame → 深拷贝 → putFrame
    ▼
g_state.frameData（mutex 保护 + 条件变量）   ← 分发枢纽
    ├─ Qt 主线程  → gui.setFrame() → QImage → QLabel      （显示渲染）
    ├─ 处理线程   → encodeYUYVtoJPEG → mjpegServer / rtspServer（网络传输）
    └─ 处理线程   → storage->writeRecordFrame              （存储）
```

### 耦合设计要点（面试可讲）

| 接口点 | 设计 | 解耦价值 |
|--------|------|---------|
| 模块间传递单元 | 统一的 `FrameBuffer`（`data/length/width/height/format/index/timestamp`） | 生产/消费双方只需认识一种结构 |
| 生产-消费同步 | 深拷贝 + mutex + 条件变量 | 消费者拿到独立数据，无悬垂指针 |
| 取帧路径 | 采集线程只做 O(1) 操作 | 慢消费者（网络拥塞、磁盘 IO）不阻塞取帧 |
| 格式路由 | `PixelFormat` 枚举 + 运行时分支（MJPEG 直通 / YUYV 先编码） | 编码策略可替换，消费方无感知 |
| 控制反哺 | 回调（`gui.onXxxChanged`）→ capture 重配置 | UI 与采集解耦，通过协议联动 |

**扩展想象**：如果接入推理引擎（如 MobileNet SSD），自然的接法是**在采集线程之后、推流之前**插入一个推理消费者——它可以作为"第四路消费者"订阅同一份 `frameData`，或作为处理线程内的一步（先推理再推流）。由于本架构已经是"一帧多消费者"模式，新增一路消费只需在 `g_state` 分发处加一个订阅方，这正是观察者模式的雏形带来的扩展性。

## 3.2 需求变更下的重构推演

### 场景 A：USB 相机 → CSI 接口（如 OV5640 接 IMX6ULL CSI）

**变化点**：
- V4L2 节点可能从 `/dev/video0`（UVC）变为 CSI 子设备 + ISP 管线（`/dev/v4l-subdev*`），需要配置 sensor 输出格式、ISP 的 scaler/色彩矩阵。
- 格式链从"UVC 直出 MJPEG"变为"raw Bayer → ISP → YUYV"，MJPEG 硬件直出路径**消失**，必须走软件编码（本项目已支持 YUYV→JPEG，天然兼容）。
- 帧格式可能从 YUYV 变成 raw Bayer（`V4L2_PIX_FMT_SRGGB8` 等），需要新增 demosaic 步骤。

**重构方案**：
1. **抽象相机源接口**（`ICameraSource`）：`init/enumFormats/setFormat/start/getFrame/putFrame/stop`，`CameraCapture` 与 `CsiCameraCapture` 各自实现，main.cpp 用**工厂**按设备路径或配置选择实现。这是把"变化点"隔离的关键。
2. **格式处理管线化**：新增 demosaic（Bayer→RGB）步骤，用策略模式注入 `VideoProcessor` 前，形成 `采集 → [demosaic →] 转换/编码 → 分发` 的可组合管线。
3. **能力探测**：启动时枚举 CSI 支持的格式/分辨率（本模块已有 `enumFormats/enumFrameSizes`），回退到 640x480 YUYV 保证兜底可用。

**追问：CSI 的增益是什么？** sensor 直连 SoC，延迟低、可控制曝光/增益/ISP 参数，不占 USB 带宽；代价是集成复杂、需处理 subdev 配置与 ISP 调优。

### 场景 B：720p → 4K（分辨率升级）

**变化点**：像素量 ×9（720p 约 92 万 → 4K 约 829 万），内存带宽、编码 CPU、缓冲池内存全部放大约 9 倍。

**重构方案**：
1. **内存**：缓冲池 4 个 × 单帧大小暴增（YUYV 4K 一帧约 16MB，4 个 = 64MB，超出 i.MX6ULL 预算）→ 必须改 MJPEG 直出（每帧压缩后几百 KB）+ 减少缓冲数 + 分片处理。
2. **性能**：4K YUYV→RGB 的 NEON 吞吐也要 ×9，单核 792MHz 不可行 → 要么降级预览分辨率（取缩略预览 + 存全分辨率），要么换带 VPU/H.264 的 SoC。**这正是"分辨率目标反推硬件选型"的决策逻辑**。
3. **接口**：`FrameBuffer.width/height` 是 int，天然支持 3840x2160，结构体无需改；但 `encodeYUYVtoJPEG` 的临时 RGB 缓冲（w*h*3 ≈ 24MB）要改成按行流式编码，避免峰值内存爆掉。

### 场景 C：30fps → 60fps

**变化点**：帧间隔 33ms → 16.6ms，所有处理链路的每帧预算减半。

**重构方案**：
1. **曝光**：强制曝光上限从"≤33ms"改为"≤16ms"（`targetExposure > 150` 钳制），否则自动曝光会再次拉掉帧率。
2. **缓冲池**：60fps 下处理抖动更容易耗尽 4 缓冲 → 缓冲数升到 6~8，或进一步压缩取帧路径延迟。
3. **编码**：YUYV 模式 60fps 软编码 CPU 预算翻倍（每帧 25ms → 不够）→ 只能强制 MJPEG 模式（硬件直出，<1ms）。
4. **网络**：60fps 的 MJPEG 码率近似翻倍，局域网内确认带宽余量；RTSP 时间戳/帧率参数同步更新（main.cpp 已有 `setStreamInfo` 联动）。
5. **软件节流**：`1000/60 ≈ 16ms` 间隔，节流判断改为更高精度时间戳（当前用毫秒，60fps 下 16ms 粒度勉强够，可用微秒级 `steady_clock`）。

**核心洞察**：**帧率/分辨率/格式三个维度不是独立参数，而是 CPU/内存/带宽/曝光时间的联合约束**。重构的本质是"先看资源预算，再定技术路径"，这是嵌入式面试官最想听到的思维方式。

## 3.3 设计模式评估与改进建议

### 现状分析

| 模式 | 现状 | 评估 |
|------|------|------|
| 单例 | `CameraCapture` 按"单实例"使用（全局 `g_state` 也类似） | 合理但非显式单例，靠使用约定 |
| 观察者 | 帧分发 = 采集线程 → 多消费者（GUI/HTTP/RTSP/存储），用"共享状态 + 条件变量"实现 | 是**隐式观察者**：没有注册/注销列表，消费者通过轮询共享帧获得数据 |
| 工厂 | 无；main.cpp 硬编码 new `CameraCapture` | 新增相机源（CSI）时需重构 |
| 策略 | 格式处理用 `if/else`（MJPEG 直通 vs YUYV 编码） | 是**隐式策略**，扩展新格式要改分支 |
| 职责链 | 处理线程内"编码→HTTP→RTSP→存储"线性链 | 近似职责链/流水线，但顺序硬编码 |

### 改进建议（面试给出"批判 + 方案"）

1. **引入 `ICameraSource` 工厂**（应对 USB→CSI 扩展）：
   ```cpp
   // 工厂：按 device 前缀或配置创建具体实现
   ICameraSource* createCameraSource(const std::string& dev) {
       if (dev.rfind("/dev/v4l-subdev", 0) == 0) return new CsiCameraCapture(dev);
       return new CameraCapture(dev);   // UVC
   }
   ```

2. **把隐式观察者显式化**：设计 `FrameSink` 接口（`onFrame(const FrameBuffer&)`），`FrameDistributor` 持有 `std::vector<FrameSink*>` 并广播。好处：消费者可动态注册/注销（如某客户端断开就不推送），新增消费方不改采集代码。代价：需要解决"多消费者共享 mmap 帧"的拷贝问题——当前深拷贝策略下，每个 sink 都要一份拷贝，可改为"生产者做一次拷贝，多个消费者引用 + 引用计数"。

3. **策略模式封装格式路径**：定义 `FrameEncoder` 接口（`encode(const FrameBuffer&, uint8_t** out)`），MJPEG（零编码直通）与 YUYV（libjpeg）各为一种策略，用 `pixfmt` 路由。消除 `if (localFmt == MJPEG) ... else ...` 的散落分支。

4. **`CameraCapture` 类职责拆分**（当前偏重）：设备查询/格式/缓冲池/流控制/FPS 统计集于一身的 ~760 行类。可拆为 `V4l2Device`（open/query/format/ctrl）+ `BufferPool`（reqbufs/mmap/qbuf/dqbuf）+ `CaptureEngine`（编排 + FPS）。增强可测试性（每个子类可独立单测）。

5. **RAII 化缓冲管理**：`BufferUnit* m_buffers` 用 `new[]/delete[]` 手动管理，可改为持有 `BufferUnit` 的 `std::vector` 或自定义 RAII 包装，异常安全更好。

**设计模式总评**：当前代码"模式正确但隐式"——观察者、策略都以条件分支和共享状态的形式存在，功能完备、改动最少，但扩展性受限。**在嵌入式项目中，"先做对，再抽象"是可辩护的顺序；但面试时主动提出这些抽象方向，能体现架构意识**。要权衡的是：抽象层会带来间接调用开销（对热路径的每帧处理需谨慎，帧分发路径保持浅层）。

## 3.4 面试「一句话总结」

> "camera 模块是整个系统的数据源头，我围绕三个原则设计它：**零拷贝**（V4L2 mmap 让 DMA 帧直达用户态）、**快进快出**（采集线程只做取帧-拷贝-归还三个 O(1) 操作，所有 CPU 重活在处理线程）、**生产消费解耦**（深拷贝 + 条件变量分发，一帧多路消费）。在这个基础上，用定点运算和 NEON 把颜色转换压到 ~5ms/帧，用 MJPEG 硬件直出实现推流零编码路径，最终让 792MHz 单核稳定跑 30fps、内存仅 8MB。如果需求变化，我会先做资源预算分析（CPU/内存/带宽/曝光时间），再决定是调参、换算法还是换硬件——因为分辨率、帧率、格式从来不是独立的参数。"

---

## 附：速查表（面试前 5 分钟过一遍）

| 主题 | 一句话答案 |
|------|-----------|
| 为什么 mmap | 内核 DMA 缓冲直接映射用户态，省两次拷贝 |
| 为什么 4 缓冲 | 延迟吸收器，平衡容错与内存 |
| 为什么必须回读 S_FMT 结果 | 驱动可能调整分辨率/格式，不回读会越界 |
| 为什么 REQBUFS(0) | 释放驱动缓冲，否则 S_FMT 返回 EBUSY |
| 为什么 getFrame/putFrame 成对 | 缓冲池租借模型，漏还 = 缓冲耗尽 = 采集冻结 |
| 为什么深拷贝再归还 | mmap 内存不能长期持有，深拷贝换确定性 |
| 为什么定点不浮点 | 无浮点开销，且与 NEON 整数 SIMD 兼容 |
| 为什么 NEON | 16 像素/轮 + vqmovun 免分支，实测 ~8× |
| 为什么强制手动曝光 | 自动曝光在暗光下拉长曝光 → 帧率暴跌 |
| 为什么软件节流兜底 | 很多 UVC 的 S_PARM 不生效，软件隔帧采样最可靠 |
| MJPEG vs YUYV | MJPEG 零编码但画质固定；YUYV 可软件处理但吃 CPU |
| 换 CSI 怎么改 | 抽象 ICameraSource 工厂 + demosaic 管线 |
| 升 4K 怎么改 | 内存/带宽/编码全爆表 → 预算分析反推硬件选型 |
| 30→60fps 改什么 | 曝光上限减半、缓冲池扩容、强制 MJPEG、节流精度提高 |
