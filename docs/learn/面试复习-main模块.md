# src/main.cpp 模块面试复习指南（程序入口与线程编排）

> 定位：以「技术面试官 + 应聘者」双视角，系统拆解 `src/main.cpp` 的代码实现。
> 阅读本文档前建议先自行通读 `src/main.cpp`（约 1450 行），并对照 `include/common/`、`include/display/gui.h`、`include/common/frame_pool.h`。
>
> 组织方式：**模块概览（脑图）→ 全局状态与线程编排 → 三个核心函数深度解析（readSelfCpuJiffies / readSelfRssKB / [PERF] 插桩）→ 面试速查**。

---

## 目录

1. [第一部分 模块整体概览](#第一部分-模块整体概览)
   - 1.1 脑图式结构
   - 1.2 职责边界
   - 1.3 全局状态设计
     - 1.3.4 unique_lock vs lock_guard（锁管理器对比）
   - 1.4 代码结构与关系全景图
   - 1.5 V4L2 缓冲与 RGB 帧池的关系
   - 1.6 FramePool 四方法详解（acquire / share / release / publish）
   - 1.7 C++ 内存序（memory_order）详解
2. [第二部分 线程编排与配置解析](#第二部分-线程编排与配置解析)
   - 2.1 六线程模型
   - 2.2 配置三级优先级
   - 2.3 命令行解析详解（QCommandLineParser）
   - 2.4 回调注入（业务编排）
   - 2.5 三个相机回调的区别（onCameraControlChanged / onResolutionChanged / onFormatChanged）
   - 2.6 onResolutionChanged 深入：paused vs stopCapture 与双线程握手
   - 2.7 wait_until 详解：原型、参数与使用
3. [第三部分 核心函数深度解析](#第三部分-核心函数深度解析)
   - 3.1 readSelfCpuJiffies() —— 进程 CPU 时间读取
   - 3.2 readSelfRssKB() —— 进程内存占用读取
   - 3.3 [PERF] 性能插桩机制
4. [第四部分 面试速查表](#第四部分-面试速查表)

---

# 第一部分 模块整体概览

## 1.1 脑图式结构

```
src/main.cpp  程序入口：全局状态 + 线程编排 + 回调注入 + 性能插桩
│
├── 全局共享状态
│   ├── CaptureState g_state        ← 采集→GUI/处理 的中转站（mutex + 条件变量）
│   ├── std::atomic<bool> g_recording  ← 录像标志（main 写，采集/处理读）
│   ├── StorageManager* g_storage  ← 存储管理器单例指针
│   ├── PerfStats g_perf            ← 性能插桩计数器（atomic）
│   └── FramePool* g_rgbPool       ← RGB 显示帧池（帧池零拷贝，容量 2）
│
├── 工具函数
│   ├── readSelfCpuJiffies()        ← 读 /proc/self/stat，全进程 CPU 时间
│   ├── readSelfRssKB()             ← 读 /proc/self/status，进程 RSS 内存
│   └── getLocalIPv4()              ← 遍历网卡拿本机 IP（启动信息展示）
│
├── main()
│   ├── 命令行解析（QCommandLineParser）
│   ├── 配置加载（ConfigManager，三级优先级）
│   ├── GUI 创建 + 存储绑定 + 回调注入
│   ├── 真实相机模式（--device 非空）
│   │   ├── V4L2 摄像头初始化 + 控制参数查询
│   │   ├── MJPEG HTTP / RTSP / TCP 控制 三服务启动
│   │   ├── 采集线程 + 处理线程 + displayTimer + perfTimer
│   │   └── 显示帧池零拷贝路径
│   ├── Mock 模式（无 --device）→ 彩条 + 控制服务
│   └── Qt 事件循环 + 资源清理
└── 线程模型：Qt 主线程 + 采集 + 处理 + RTSP + 控制（+ HTTP 客户端动态线程）
```

## 1.2 职责边界

| 本模块负责 | 本模块不负责 |
|-----------|-------------|
| 线程编排：创建/join 采集、处理、RTSP、控制线程 | V4L2 ioctl 细节（`src/camera/`） |
| 全局共享状态定义（`g_state` / `g_recording` / `g_rgbPool`） | 网络协议实现（`src/network/`） |
| 配置三级优先级合并（命令行 > 配置文件 > 默认值） | GUI 控件逻辑（`src/display/`） |
| 回调注入（GUI 事件 → 业务动作的 lambda 桥接） | 文件持久化（`src/storage/`） |
| 性能插桩（`[PERF]` / `[FPS Diag]`） | 协议编解码（各服务内部） |
| 进程资源读取（CPU jiffies / RSS） | 硬件 DMA（内核驱动） |

**一句话**：main.cpp 是"**胶水层**"——它不实现具体功能，而是把 camera/display/network/storage 四个模块编排成一个可运行的系统，并承担"线程怎么分、状态怎么传、事件怎么接"的全局决策。

## 1.3 全局状态设计

### 1.3.1 CaptureState（`main.cpp:56-75`）——帧数据中转站

```cpp
struct CaptureState {
    std::mutex              mtx;            // 帧数据锁
    std::vector<uint8_t>    frameData;      // 拷贝后的最新帧
    int                     width = 0, height = 0;
    PixelFormat             format = FMT_RGB24;
    double                  fps = 0.0;
    std::atomic<bool>       running{false};  // 线程退出标志
    std::atomic<bool>       paused{false};   // 暂停采集（切分辨率/格式）
    std::mutex              pauseMtx;        // 暂停同步锁
    std::condition_variable pauseCv;         // 暂停同步条件变量
    std::atomic<bool>       pausedAck{false};// 采集线程已确认暂停
    std::atomic<int>        targetFps{0};    // 用户目标帧率（0=不限制）
    std::mutex              procMtx;         // 处理线程专用锁
    std::condition_variable procCv;          // 新帧通知
    std::atomic<bool>       frameReady{false}; // 有新帧待处理
};
static CaptureState g_state;
```

**设计要点**：
- **`frameData` 是"最新一帧"而非队列**：采集线程每帧覆盖写，消费方（GUI/处理线程）主动取最新——天然防堆积，中间帧被覆盖丢弃（拉模式哲学，见 display 篇）
- **两把锁分工**：`mtx` 保护帧数据本身；`procMtx` 专门服务处理线程的条件等待，避免"等帧"和"读帧"互相阻塞
- **暂停握手**：`paused/pauseMtx/pauseCv/pausedAck` 实现"切分辨率/帧率前先让采集线程确认停手"的双向握手（`main.cpp:613-620`）

### 1.3.2 为什么用 `std::atomic` 而非 mutex？

`running/paused/targetFps/frameReady` 都是**单标志位**，读多写少、无复合操作——`std::atomic` 提供无锁的读改写，避免每次访问都要拿 mutex 的开销。**规则**：复合读改写（如"取帧→拷贝→归还"）用 mutex；单一标志位用 atomic。

### 1.3.3 RGB 显示帧池（`main.cpp:114`）

```cpp
// 容量 2：GUI 持 1 槽 + 解码写 1 槽，天然双缓冲，无需锁。
FramePool* g_rgbPool = nullptr;
```

`FramePool` 是 header-only 的帧池（`include/common/frame_pool.h`），用**引用计数 + 原子指针**实现无锁双缓冲。main.cpp 负责创建（`new FramePool(2)`），displayTimer 做生产者（解码入槽 → publish），GUI 做消费者（share → 浅引用上屏）。

### 1.3.4 unique_lock vs lock_guard（锁管理器对比）

> 面试高频：为什么同一个项目里 `g_state.pauseMtx` 用 unique_lock、`g_state.mtx` 用 lock_guard？核心一句话——**lock_guard 是"最小可用"的 RAII（不能中途解锁），unique_lock 是"功能完整"的 RAII（加解锁时机完全可控，且是条件变量 wait 的唯一合法搭配）**。

**两处实际用法**：

```cpp
// 用法 1：unique_lock（main.cpp:1209，暂停握手）
{
    std::unique_lock<std::mutex> lk(g_state.pauseMtx);
    g_state.pauseCv.wait_until(lk,
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1100),
        [] { return g_state.pausedAck.load(); });
}

// 用法 2：lock_guard（main.cpp:944，帧数据保护）
{
    std::lock_guard<std::mutex> lock(g_state.mtx);
    g_state.frameData.assign(fb.data, fb.data + fb.length);
    ...
}
```

**详细对比**：

| 维度 | `lock_guard` | `unique_lock` |
|------|-------------|---------------|
| RAII 自动管理 | ✅ 构造加锁、析构解锁 | ✅ 同样自动 |
| 手动解锁 | ❌ 不能 | ✅ `lk.unlock()` |
| 重新加锁 | ❌ 不能 | ✅ `lk.lock()` |
| 延迟加锁 | ❌ 构造即锁 | ✅ `defer_lock` 先构造后加锁 |
| 尝试加锁 | ❌ 不能 | ✅ `try_lock()` 非阻塞尝试 |
| 配合条件变量 | ❌ 不行 | ✅ **必须**（`wait` 需要它） |
| 移动语义 | ❌ 不能移动 | ✅ 可移动（转移锁所有权） |
| 内部开销 | 轻（无额外状态） | 稍重（维护一个标志位） |

**为什么条件变量 `wait` 必须用 `unique_lock`？**（最关键的考点）

`wait` 内部要做三件事：
1. 检查条件（`pausedAck` 是否已置位）
2. **不满足 → 原子地"解锁 + 睡眠"**（两步合并，避免丢失唤醒）
3. 被唤醒 → **重新加锁**，再检查条件

问题来了：`wait` 需要在睡眠期间**解锁**、醒来后**重新加锁**——这要求锁对象支持"手动解锁 + 重新加锁"。而 **`lock_guard` 没有 `unlock()`/`lock()` 接口**，所以编译器直接拒绝：

```cpp
std::lock_guard<std::mutex> lk(mtx);
cv.wait(lk, ...);   // ❌ 编译错误：lock_guard 不能被 wait 使用
```

- **为什么不能锁着等？** 如果 `wait` 不解锁就睡眠，持有锁的线程一直占着 `g_state.pauseMtx`——主线程 `notify_one()` 时需要拿同一把锁通知，被卡死 → **死锁**。所以 `wait` 必须能在睡眠前释放锁。
- **为什么"解锁+睡眠"要原子？** 如果先解锁、再睡眠，中间有空隙：主线程可能在这个空隙里 `notify_one()`，而睡眠线程还没开始等——**唤醒信号丢失**，线程永远等不到。`wait` 内部把"解锁+睡眠"做成原子操作，从根上避免这个经典 bug。

**结合项目理解"为什么这么选"**：

- **场景 A：`pauseCv.wait_until`（必须 unique_lock）**——等待期间必须**释放锁**（否则 notify 方卡死）、等待结束必须**重新持锁**（继续访问 g_state）。只有 `unique_lock` 能满足 → **没得选**。
- **场景 B：`g_state.mtx` 保护帧数据（lock_guard 足够）**——进临界区 → 改数据 → 出临界区，**全程不需要中途解锁**。`lock_guard` 语义正好："进了就别走，走了就别回"，更轻量、意图更明确。

**工程原则**：**能用 `lock_guard` 就不用 `unique_lock`**——`lock_guard` 意图清晰（纯临界区）、开销小；只有真正需要"解锁/重锁/条件等待"时才升级到 `unique_lock`。这是 Google C++ Style 等规范的常见建议。

**补充：unique_lock 的 defer_lock / try_lock 用法**（项目没用，面试常追问）：

```cpp
std::unique_lock<std::mutex> lk(mtx, std::defer_lock);  // 延迟加锁：先构造空锁
if (needLock) lk.lock();                                // 条件满足才加锁

std::unique_lock<std::mutex> lk(mtx, std::try_to_lock); // 尝试加锁（非阻塞）
if (lk.owns_lock()) { /* 拿到锁 */ } else { /* 没拿到，去做别的事 */ }

lk.unlock();     // 手动解锁后做事（不占锁）
// ... 长时间计算，不阻塞别人 ...
lk.lock();       // 需要时再锁
```

**面试一句话**：
> "lock_guard 和 unique_lock 都是 RAII 锁管理器，区别在**可控性**：lock_guard 构造加锁、析构解锁，**不能中途解锁/重锁**，适合'纯临界区'（如保护 g_state.frameData）；unique_lock 加解锁时机完全可控（可延迟加锁、手动解锁、重新加锁、可移动），**是条件变量 wait 的唯一合法搭配**——因为 wait 需要在睡眠前原子地'解锁+睡眠'、唤醒后'重新加锁'，lock_guard 没有这些接口。工程上能用 lock_guard 就不用 unique_lock（意图清晰、开销小），只有条件等待等场景才升级。项目里 pauseCv.wait_until 用 unique_lock（要等待+重锁），g_state.mtx 保护帧数据用 lock_guard（纯临界区）——正好是两种 RAII 的典型分工。"

## 1.4 代码结构与关系全景图

> 6 张图从不同视角看 main.cpp 的内部组织与对外关系：**文件布局 → 执行流程 → 全局状态读写 → 线程数据流 → 回调注入 → 模块依赖**。面试前把这 6 张图过一遍，就能对 main.cpp 形成完整的空间认知。

### 1.4.1 图 1：整体布局图（文件级结构）

```
src/main.cpp（约 1450 行）
│
├─ ① 头文件区 (24-51 行)         ← 引入 Qt / 各模块头 / 系统头
│
├─ ② 全局共享状态 (53-114 行)
│    ├─ CaptureState g_state      ← 帧中转站（锁+条件变量）
│    ├─ g_recording               ← 录像标志
│    ├─ g_storage                 ← 存储管理器指针
│    ├─ PerfStats g_perf          ← 性能插桩计数
│    └─ FramePool* g_rgbPool      ← RGB 显示帧池
│
├─ ③ 工具函数区 (116-201 行)
│    ├─ readSelfCpuJiffies()      ← 读 /proc/self/stat CPU 时间
│    ├─ readSelfRssKB()           ← 读 /proc/self/status 内存
│    └─ getLocalIPv4()            ← 获取本机 IP
│
└─ ④ main() 主体 (204-1453 行)
     ├─ 4.1 初始化 (205-347)      ← Qt应用/命令行/配置
     ├─ 4.2 GUI+存储 (348-409)    ← 创建 GUI、注入回调
     ├─ 4.3 真实相机模式 (411-1347) ← 主业务路径
     ├─ 4.4 Mock 模式 (1348-1405) ← 无硬件调试
     ├─ 4.5 事件循环 (1416-1419)  ← app.exec()
     └─ 4.6 清理 (1420-1453)      ← join线程/释放资源
```

**关键点**：四个"区"从上到下职责递进——**状态 → 工具 → 流程编排**，工具函数只依赖系统头，不依赖业务模块。

### 1.4.2 图 2：main() 执行流程时序图（最核心）

```
main(argc, argv)
  │
  ├─① QApplication app(...)             ← 初始化 Qt（消费平台参数如 -platform linuxfb）
  │
  ├─② QCommandLineParser 解析           ← 6 个选项
  │
  ├─③ ConfigManager 加载三级配置         ← 命令行 > 配置 > 默认
  │
  ├─④ CameraGUI gui（栈对象）           ← GUI 贯穿整个生命周期
  │    └─ setGalleryStorage / onStoragePathChanged（回调注入）
  │
  ├─ 判断 device 是否为空？
  │   │
  │   ├── 空 → ═══ Mock 模式 (4.4) ═══
  │   │     ├─ 注入空回调（qDebug 打印）
  │   │     ├─ ControlServer 仅心跳/状态
  │   │     └─ Mock 驱动定时器 → requestRefresh → 彩条
  │   │
  │   └── 非空 → ═══ 真实相机模式 (4.3) ═══
  │         ├─ new FramePool(2)                    ← 帧池
  │         ├─ CameraCapture 初始化                 ← V4L2
  │         │    ├─ enumFormats → 选格式
  │         │    ├─ setFormat(640,480)
  │         │    └─ queryControl（亮度/对比度/白平衡/曝光）
  │         ├─ MJPEGStreamServer.start()           ← HTTP 8080
  │         ├─ RTSPServer.start() → RTSP线程        ← RTSP 8554
  │         ├─ ControlServer.start() → 控制线程     ← TCP 9000
  │         ├─ 采集线程 start                       ← 拉帧
  │         ├─ 处理线程 start                       ← 编码/推流/录像
  │         ├─ displayTimer(33ms)                  ← 解码上屏
  │         └─ perfTimer(5s)                       ← 性能插桩
  │
  ├─⑤ gui.show()
  │
  ├─⑥ app.exec()                     ← Qt 事件循环（阻塞直到退出）
  │
  └─⑦ 清理：running=false → notify_all → join 各线程 → 释放
```

**关键设计**：`gui` 是**栈对象**（`main.cpp:349`），生命周期 = 整个 app；而 `capture`/`mjpegServer` 等是**堆对象 + 线程**，在真实模式下创建、退出时清理——这决定了"GUI 常驻、业务资源按需"的编排哲学。

### 1.4.3 图 3：全局状态关系图（谁读谁写）

```
                    ┌─────────────────────────────┐
                    │  g_state (CaptureState)     │
                    │  mtx / frameData / width... │
                    └───┬──────────┬──────────┬───┘
            写:采集线程 │          │ 读:GUI   │ 读:处理线程
                │       │          │          │
    采集线程: getFrame → 拷贝 → putFrame     │          │
               └→ notify (procCv) ──────┘          │
                                      displayTimer 读→ 解码
                                      (g_rgbPool→setFrameShared→requestRefresh)
                                                      │
                                                      └→ 编码/推流/录像
┌──────────────┐   ┌────────────────┐   ┌─────────────────┐
│ g_recording  │   │   g_perf       │   │  g_rgbPool      │
│ (atomic bool)│   │ (atomic计数)   │   │ (FramePool*)    │
│ 写:GUI/TCP   │   │ 写:3线程累加   │   │ 生产:displayTimer│
│ 读:采集/处理 │   │ 读:perfTimer   │   │ 消费:GUI(share) │
└──────────────┘   └────────────────┘   └─────────────────┘
```

**读-写矩阵速记**：`g_state` 一写多读（采集写、GUI/处理读）；`g_perf` 多写单读（3 线程累加、perfTimer 读）；`g_rgbPool` 生产-消费（displayTimer 产、GUI 消）。

### 1.4.4 图 4：线程与数据流关系图（6 线程 + 数据流向）

```
┌─────────────┐     ┌──────────────┐     ┌─────────────┐     ┌─────────────┐
│ 采集线程     │     │ 处理线程      │     │ displayTimer│     │  GUI线程     │
│ (873)       │     │ (970)        │     │ (1070)      │     │ (Qt主线程)   │
│             │     │              │     │             │     │             │
│ getFrame    │     │ wait(procCv) │     │ acquire槽   │     │ 浅引用       │
│   │         │     │   │          │     │   │         │     │ setPixmap    │
│ 拷贝         │     │ 深拷贝       │     │ 解码入槽     │     │   ▲         │
│   │         │     │   │          │     │   │         │     │   │         │
│ putFrame    │     │ YUYV→JPEG    │     │ publish     │     │   │         │
│   │         │     │   │          │     │   │         │     │   │         │
│ notify ─────┼──►  │ HTTP/RTSP/   │     │ share       │     │   │         │
│ (procCv)    │     │ 录像写盘      │     │   │         │     │   │         │
└─────┬───────┘     └──────┬───────┘     │ setFrame    │     │   │         │
      │                    │             │ Shared      │     │   │         │
      │  g_state.frameData │             │   │         │     │   │         │
      └────────────────────┴─────────────┴───┴─────► ──┴─────┘   │
                                                                  │
┌─────────────┐  ┌──────────────┐  ┌──────────────┐  ┌─────────────┐
│ RTSP线程     │  │ 控制线程      │  │ MJPEG-accept │  │ perfTimer   │
│ (712)       │  │ (859)        │  │ (服务内部)    │  │ (1158)      │
│ epoll+分片   │  │ epoll+CRC    │  │ 客户端线程×N  │  │ 读g_perf    │
└─────────────┘  └──────────────┘  └──────────────┘  └─────────────┘
```

**核心关系**：
- 采集线程是**唯一生产者**，通过 `g_state`（帧数据）+ `procCv`（通知）供给 3 路消费者
- 处理线程吃**原始帧**做编码/推流/录像；displayTimer 吃**原始帧**做解码显示（走帧池，零拷贝）
- RTSP/控制/HTTP 都是**网络服务线程**，只消费各自服务器的内部状态，不碰 g_state

### 1.4.5 图 5：回调注入关系图（GUI 事件 → 业务动作）

```
                    CameraGUI (display模块)
                    │
  GUI 事件（按钮/滑块/下拉框）
  │  拍照 / 录像 / 分辨率 / 格式 / 存储路径 / 相机控制 / 帧率
  │
  ▼ 调用注入的 std::function 回调
  ┌────────────────────────────────────────────────┐
  │ main.cpp 中注入的 lambda（捕获业务对象指针）     │
  │                                                │
  │ onCaptureRequest      → 存 JPEG（g_state+存储） │
  │ onRecordToggle        → startRecord/stopRecord │
  │ onResolutionChanged   → 暂停→停流→S_FMT→重启    │
  │ onFormatChanged       → 暂停→停流→setFormat→重启│
  │ onStoragePathChanged  → 更新路径+写配置         │
  │ onCameraControlChanged→ capture->setControl(cid)│
  │ onFramerateChanged    → S_PARM+软件节流+displayTimer│
  └────────────────────────────────────────────────┘
```

**关系本质**：main.cpp 是**桥**——GUI 只发出"我想做什么"，main.cpp 决定"怎么做"（操作 capture/storage/rtspServer）。这样 display 模块零依赖 camera/network，可独立测试（Mock 模式注入空 lambda）。

### 1.4.6 图 6：依赖关系图（模块间耦合）

```
main.cpp 依赖（编译期 include）
│
├── include/display/gui.h        ← GUI（注入回调接口）
├── include/camera/capture.h     ← 采集（V4L2）
├── include/camera/processor.h   ← 图像处理（解码/编码）
├── include/network/mjpeg_server.h ← HTTP 流
├── include/network/control.h    ← TCP 控制
├── include/network/rtsp_server.h ← RTSP 流
├── include/storage/manager.h    ← 存储
├── include/common/*.h           ← types/config/logger/frame_pool
│
└── 反向依赖（谁依赖 main.cpp）：
     ← 无。main.cpp 是最顶层，不暴露接口给任何模块
     ← 但 GUI 通过 std::function 回调反向"调用"main 注入的逻辑
        （依赖倒置，运行时依赖而非编译期依赖）
```

**依赖方向总结**：main.cpp 处于依赖图**最顶端**，编译期单向依赖所有模块；运行时通过回调形成"逻辑反向"（GUI 调 main 注入的逻辑），但无编译期反向依赖——这是依赖倒置的体现。

### 1.4.7 一句话架构（面试速记）

> main.cpp 是"**胶水 + 编排**"：用栈对象 `gui` 贯穿生命周期，用堆对象 + 线程按需创建业务资源；通过 `g_state` 共享帧数据、`std::function` 回调解耦控制、`FramePool` 实现显示零拷贝、`g_perf` 做性能插桩——五个全局状态 + 五条线程 + 一套回调，把 camera/display/network/storage 串成完整系统。

## 1.5 V4L2 缓冲与 RGB 帧池的关系

> 面试高频追问：**"V4L2 不是已经零拷贝了吗，为什么还要 g_state 拷贝和 FramePool？两者到底什么关系？"** 这一节把两套缓冲体系彻底讲透。

### 1.5.1 核心结论：两套独立、分工明确的内存

| | V4L2 内核缓冲区 | RGB 显示帧池（FramePool） |
|---|---|---|
| **内存归属** | **内核**（驱动侧分配） | **用户态**（应用 `std::vector`） |
| **分配方式** | `VIDIOC_REQBUFS` → 内核 DMA 缓冲区 | `new FramePool(2)` → 预分配 2 个槽 |
| **数据内容** | 原始帧（**JPEG/YUYV** 压缩/原始数据） | **RGB24** 解码后的显示数据 |
| **数据来源** | 摄像头 DMA 硬件直写 | `displayTimer` 解码（libjpeg/NEON） |
| **所有权模型** | 租借（getFrame/putFrame 成对） | 共享（acquire/share/release + 引用计数） |
| **交互协议** | DQBUF/QBUF | publish/share/release |

**它们之间不直接相连**，中间隔着 `g_state.frameData` 这个"拷贝中转站"。完整链路：

```
V4L2 mmap 缓冲 ──①拷贝──► g_state.frameData ──②解码──► FramePool 槽 ──③浅引用──► GUI 上屏
(内核,JPEG/YUYV)      (用户态,原始帧)      (用户态,RGB24)      (QImage 零拷贝)
```

### 1.5.2 第一层：V4L2 内核缓冲区（`capture.cpp`）

**申请**（`capture.cpp:581-604`）：

```cpp
req.count  = kDefaultBufferCount;        // 请求 4 个
req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
req.memory = V4L2_MEMORY_MMAP;           // mmap 方式
ioctl(m_fd, VIDIOC_REQBUFS, &req);       // 内核分配 DMA 缓冲
if (req.count < 2) return -ENOMEM;       // 至少 2 个才能轮转
m_nbuffers = req.count;                  // 回读实际分配数（双向参数）
```

`VIDIOC_REQBUFS` 让**内核驱动**分配 4 个 DMA 缓冲区（物理连续、页对齐，供 DMA 引擎直写）。`count` 是"请求/返回双向"——驱动可能只给 3 个，必须回读 `req.count`。

**映射**（`capture.cpp:619-624`）：

```cpp
m_buffers[i].start = mmap(nullptr, buf.length,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED,   // 灵魂：CPU 和 DMA 共享同一物理内存
                           m_fd, buf.m.offset);
```

`MAP_SHARED` 是零拷贝的关键——同一块物理内存，DMA 硬件写入、CPU 应用读取。`buf.length` 是按"最坏情况帧"分配的天花板（MJPEG 640x480 可能 256KB），实际有效数据看每帧 `bytesused`。

**租借协议**（getFrame/putFrame）：

```cpp
// getFrame: DQBUF 借出，data 直接指向 mmap 内存（零拷贝指针）
buf->data   = (uint8_t*)m_buffers[vbuf.index].start;
buf->length = vbuf.bytesused;                     // 实际帧长（每帧不同）

// putFrame: 用 data 指针遍历反推槽位 → QBUF 归还
for (int i = 0; i < m_nbuffers; ++i)
    if (m_buffers[i].start == buf->data) { idx = i; break; }
ioctl(m_fd, VIDIOC_QBUF, &vbuf);                  // 归还给驱动
```

**租借模型**：只有 4 个槽，必须尽快归还，否则驱动无缓冲可用、`DQBUF` 永久阻塞——这是采集线程"拷贝完立即归还"的根本原因。

### 1.5.3 第二层：RGB 显示帧池（`frame_pool.h` + `main.cpp`）

**创建**（`main.cpp:415`）：

```cpp
g_rgbPool = new FramePool(2);   // 容量 2：GUI 持 1 槽 + 解码写 1 槽，天然双缓冲
```

**槽结构**（`frame_pool.h:43-50`）：

```cpp
struct FrameSlot {
    std::vector<uint8_t> data;   // RGB24 像素数据（预分配）
    std::atomic<int>     refs;   // 引用计数：0=空闲, >0=被持有
    uint64_t             seq;    // 帧序号
    int width, height;
    PixelFormat format;          // 固定 FMT_RGB24
};
```

**核心 API**（引用计数 + 原子指针）：

```cpp
FrameSlot* acquire() {           // 借空闲槽（refs 0→1），无空闲返回 nullptr
    for (auto& s : m_slots) {
        int expected = 0;
        if (s->refs.compare_exchange_strong(expected, 1)) return s.get();
    }
    return nullptr;              // 池满 → 丢帧，不阻塞
}
FrameSlot* share() {             // 取当前槽共享引用（refs+1）
    FrameSlot* cur = m_current.load(std::memory_order_acquire);
    if (cur) cur->refs.fetch_add(1, std::memory_order_relaxed);
    return cur;
}
void release(FrameSlot* s) {     // 归还引用（refs-1）
    if (!s) return;
    s->refs.fetch_sub(1, std::memory_order_release);
}
void publish(FrameSlot* s) {     // 发布为"当前"（原子替换指针）
    std::atomic_thread_fence(std::memory_order_release);
    FrameSlot* old = m_current.exchange(s, std::memory_order_acq_rel);
    if (old) release(old);       // 旧 current 槽归零后可复用
}
```

**共享模型**：不搬数据，多个消费者通过 `share()/release()` 共享同一块 RGB 内存，引用计数保证"最后用完的人释放"。核心不变量：**`acquire` 只借空闲槽（refs==0）**，天然读写分离、无锁。

### 1.5.4 关键：两者如何通过 `g_state` 衔接（`main.cpp`）

**环节 ①：采集线程拷贝**（`main.cpp:942-956`）：

```cpp
{
    std::lock_guard<std::mutex> lock(g_state.mtx);
    g_state.frameData.assign(fb.data, fb.data + fb.length);  // V4L2 mmap → g_state
    g_state.width  = fb.width;
    ...
}
capture->putFrame(&fb);   // 立即归还 V4L2 缓冲！
```

采集线程拿到 V4L2 的 mmap 指针后，**必须深拷贝**到 `g_state.frameData` 才能 `putFrame` 归还——因为 mmap 内存会被硬件覆盖。这一步是"V4L2 缓冲 → 应用自有内存"的**所有权转移**。

**环节 ②：displayTimer 借槽解码**（`main.cpp:1079-1131`）：

```cpp
FrameSlot* slot = g_rgbPool->acquire();   // 借 RGB 写槽（无空闲则丢帧）
if (!slot) return;

std::vector<uint8_t> raw;
{
    std::lock_guard<std::mutex> lock(g_state.mtx);
    raw = g_state.frameData;              // g_state → raw（短锁拷贝，临时）
    ...
}

// 解码直接写入池槽！（关键：输出端就是显示端要读的缓冲）
if (srcFmt == FMT_MJPEG)
    VideoProcessor::decodeJPEGtoRGB(raw.data(), raw.size(), slot->data, dw, dh);
else if (srcFmt == FMT_YUYV)
    VideoProcessor::yuyvToRgb24(raw.data(), slot->data.data(), srcW, srcH);

slot->seq++;
g_rgbPool->publish(slot);                 // 原子发布为"当前"
FrameSlot* displaySlot = g_rgbPool->share();
if (displaySlot) gui.setFrameShared(displaySlot);  // GUI 接管引用
gui.requestRefresh();                     // 立即上屏
```

**关系点**：
- `raw` 是一次必要的原始帧拷贝（JPEG ~0.1MB）——`g_state` 会被采集线程随时覆盖，必须短锁拷出
- `decodeJPEGtoRGB` 的 `slot->data` 是**引用参数**——解码结果**直接写进池槽**。这是帧池零拷贝的核心：**解码输出端 = 显示端读取的缓冲，中间不再有第二次拷贝**

**环节 ③：GUI 浅引用上屏**（`gui.cpp` refreshFrame）：

```cpp
if (m_heldSlot) {
    // QImage 浅引用：不 .copy()，直接指向池槽内存
    img = QImage(m_currentFrame.data, w, h, w * 3, QImage::Format_RGB888);
}
if (!img.isNull()) m_videoDisplay->setPixmap(QPixmap::fromImage(img));
```

`m_heldSlot` 持有池槽引用（refs≥1），保证 `slot->data` 生命周期有效，QImage 才能安全浅引用——**数据从头到尾只在池槽里存在一份**。

### 1.5.5 为什么需要"两套缓冲 + 一个中转"（对比总结）

```
        V4L2 内核缓冲（4个）            g_state（1份）           FramePool 槽（2个）
        ┌────────────────┐            ┌──────────────┐        ┌────────────────┐
 硬件   │ JPEG/YUYV 帧    │  ①拷贝     │ 原始帧副本    │  ②解码  │ RGB24 像素     │ ③浅引用
 ──DMA─►│ (驱动管理)     │ ────────►  │ (应用自有)   │ ──────► │ (应用共享)     │ ───────► GUI
        └────────────────┘            └──────────────┘        └────────────────┘
         getFrame/putFrame            mutex 保护              acquire/publish/share/release
         (租借:借出归还)              (所有权转移)            (共享:引用计数)
```

| 问题 | 为什么需要 V4L2 缓冲 | 为什么需要 FramePool |
|------|---------------------|---------------------|
| **数据格式不同** | V4L2 存压缩帧（JPEG/YUYV），体积小 | FramePool 存解码后 RGB24（0.92MB），可显示 |
| **所有权模型不同** | 驱动租借，必须尽快归还（4 槽） | 应用共享，多个消费者引用计数 |
| **生命周期不同** | 硬件随时覆盖，不能长期持有 | 引用计数保证"最后用完才释放" |
| **消费方式不同** | 一帧多路消费（推流/录像/显示）都从 `g_state` 取 | 显示链路专用，避免每路都解码 |

**核心结论**：V4L2 缓冲是"**原始帧的暂存区**"（驱动管理、租借、快速周转）；FramePool 是"**显示帧的共享区**"（应用管理、引用计数、零拷贝）。`g_state` 是两者之间的**所有权过渡带**——把"驱动借给我们的内存"变成"应用自己的内存"，才能安全地分发给多路消费者。

### 1.5.6 性能视角（面试加分）

```
优化前（旧 setFrame 路径）：V4L2 → g_state → setFrame.assign → QImage.copy() = RGB24 拷 2 次
优化后（帧池路径）：       V4L2 → g_state → 解码直写池槽 → QImage 浅引用 = RGB24 拷 0 次
实测：显示链路拷贝 10.0 → 0.5 MB/s（-95%）
```

V4L2 缓冲到 `g_state` 的**原始帧拷贝省不掉**（JPEG 小，~0.1MB，换所有权确定性）；但 **RGB24 的两次深拷贝被帧池完全消除**——这正是两套系统"分工"的意义：**V4L2 管"怎么拿帧"，FramePool 管"怎么零拷贝分发"**。

### 1.5.7 面试追问与应答

**Q1：V4L2 不是已经零拷贝了吗，为什么采集线程还要深拷贝？**
**A**：V4L2 的零拷贝指的是"内核 DMA 缓冲 → 用户态"不需要 read() 那样的搬运（mmap 直接映射）。但 V4L2 缓冲池只有 4 槽、硬件会随时覆盖，且 mmap 内存不能跨线程安全共享——所以采集线程必须 `assign()` 深拷贝到 `g_state` 再归还，用一次 memcpy（几十微秒）换"所有权确定 + 无锁多路消费"。

**Q2：FramePool 省掉的到底是什么拷贝？**
**A**：省的是**显示链路的 RGB24 深拷贝**。旧路径 `setFrame` 的 `assign` + `QImage.copy()` 每帧拷 2 次 0.92MB；帧池路径解码结果直接写进池槽、GUI 浅引用上屏，RGB24 全程 0 次深拷贝。V4L2 → g_state 的**原始帧拷贝（JPEG 小）省不掉**，但它是"数据所有权过渡"的必要代价。

**Q3：为什么 FramePool 容量是 2 而不是更多？**
**A**：容量 2 正好满足"GUI 持 1 槽（正在显示）+ 解码写 1 槽（下一帧）"的双缓冲需求——天然读写分离、无需锁。更多槽位对显示链路没有收益（显示是"最新帧优先"，中间帧被覆盖丢弃），反而多占内存（每槽 0.92MB）。

**Q4：两者各自解决什么问题？一句话？**
**A**：V4L2 缓冲解决"**怎么高效地从硬件拿帧**"（mmap 零拷贝 + 租借周转）；FramePool 解决"**怎么高效地把帧分发给显示**"（引用计数共享 + 零拷贝上屏）。`g_state` 是连接两者的"所有权过渡带"。

### 1.5.8 数据旅程：拷贝与引用完整清单

> 面试高频："摄像头数据从硬件到各消费端，到底发生了几次拷贝、几次引用？"这一节给出完整清单，并**特别澄清"显示链路零拷贝"的准确边界**（易错点，见 1.5.8.3）。

#### ① 总体结论

从摄像头到各消费端，数据共发生 **4 次深拷贝（memcpy）+ 多次引用（零拷贝共享）**。核心规律：**拷贝都在"线程边界 + 所有权转移"处，引用都在"同一份数据被多端共享"处**。

#### ② 全景图（一次采集，多路分发）

```
摄像头 DMA
   │
   ▼
① V4L2 mmap 缓冲区（内核, JPEG/YUYV）          ← 引用#0：DMA 直写，getFrame 拿到指针（零拷贝）
   │
   │  拷贝#1（采集线程, 必须）
   ▼
② g_state.frameData（用户态, 原始帧）
   │
   ├─┬──────────────────────────────────────────────┐
   │ │                                              │
   │ 拷贝#2（处理线程）                             │  拷贝#4（displayTimer）
   ▼                                              │  ▼
③ localFrame（处理线程本地）                      │  ⑤ raw（GUI 线程临时）
   │                                              │  │
   ├─ MJPEG 直通（引用，无拷贝，见下）              │  解码直写（无拷贝）
   ├─ YUYV 模式：编码为 JPEG（编码自身开销）         │  ▼
   │                                              │  ⑥ FramePool 槽（RGB24）
   ▼                                              │  │
  ④ 各服务器内部                                    │  share（引用#1）
   │                                              │  ▼
   ├─ HTTP: m_currentFrame.assign（拷贝#3a）       │  GUI m_heldSlot 持有（引用#2）
   ├─ RTSP: m_latestJpeg.assign（拷贝#3b）         │  │
   ├─ 录像: fwrite 直写磁盘（无内存拷贝）           │  QImage 浅引用（引用#3）
   └─ RTP 分片: sendto 直发（无拷贝）               │  │
                                                  ▼
                                               ⑦ QPixmap::fromImage（上屏拷贝#5）
```

#### ③ 逐个环节明细

**引用环节（零拷贝，不搬数据）**：

| 环节 | 类型 | 代码 | 说明 |
|------|------|------|------|
| ① DMA → mmap | **引用#0** | `buf->data = m_buffers[vbuf.index].start`（capture.cpp:470） | DMA 硬件直写，getFrame 直接拿指针，**无拷贝** |
| ⑥ 池槽 → GUI | **引用#1** | `g_rgbPool->share()` → `setFrameShared` | 引用计数共享，GUI 持 refs |
| GUI 持有槽 | **引用#2** | `m_heldSlot = slot` | 生命周期由引用计数保证 |
| 槽 → QImage | **引用#3** | `QImage(m_currentFrame.data, ...)` 不 `.copy()` | 浅引用池槽内存 |

**拷贝环节（4 次深拷贝 + 1 次上屏拷贝）**：

| 环节 | 类型 | 代码 | 为什么必须 |
|------|------|------|-----------|
| ② mmap → g_state | **拷贝#1** | `g_state.frameData.assign(fb.data, ...)`（main.cpp:945） | V4L2 缓冲会被硬件覆盖，必须深拷贝后才 `putFrame` 归还 |
| ③ g_state → localFrame | **拷贝#2** | `localFrame = g_state.frameData`（main.cpp:1003） | 处理线程要锁外做重活，必须拷出后快速释放锁 |
| ④a → HTTP | **拷贝#3a** | `m_currentFrame.assign(data, data+len)`（mjpeg_server.cpp:255） | 服务器要等所有客户端就绪，需存副本 |
| ④b → RTSP | **拷贝#3b** | `m_latestJpeg.assign(...)`（rtsp_server.cpp:762） | 同上，供新客户端 join 用 |
| ⑤ g_state → raw | **拷贝#4** | `raw = g_state.frameData`（main.cpp:1087） | displayTimer 短锁拷贝，避免持锁解码 |
| ⑦ 上屏 | **拷贝#5** | `QPixmap::fromImage(img)`（gui.cpp） | linuxfb 上屏物理必需 |

**无拷贝环节（直接透传）**：

| 环节 | 代码 | 说明 |
|------|------|------|
| HTTP 直发 | `write(client_fd, jpeg, len)` | 从 `m_currentFrame` 直接发给客户端 |
| RTSP RTP 分片 | `rtpSendFrame(ci, jpeg_data, len, ...)` → `sendto` | 直接从传入指针分片发送 |
| 录像写盘 | `fwrite(jpeg_data, 1, len, m_recordFile)`（manager.cpp） | 直接从 `localFrame` 写磁盘 |

#### ④ 为什么拷贝都在"线程边界 + 所有权转移"处？

```
采集线程 → g_state     ：所有权转移（mmap 借来的内存必须归还）
g_state → 处理线程      ：锁竞争（锁内只拷 1ms，锁外做 25ms 编码）
g_state → displayTimer ：锁竞争（短锁拷贝，锁外解码）
服务器内部 assign      ：多客户端等待（存副本供 join）
```

**每条拷贝都有一个明确的"为什么不能省"的理由**——不是冗余拷贝，而是并发/所有权模型的必要代价。

#### ⑤ ⚠️ 重点澄清："显示链路零拷贝"的准确边界（易错点）

> **误区**："帧池让显示链路零拷贝"——这句话**不严谨**。因为显示链路还有 `g_state → raw` 这次 JPEG 拷贝。

**准确说法**：帧池消除的是 **RGB24 数据的 2 次深拷贝**，而不是"整个显示链路没有任何拷贝"。

关键：**要区分"谁被拷了"**。显示链路里有两种数据：

| 数据 | 旧路径 | 帧池路径 | 帧池是否改变 |
|------|--------|---------|-------------|
| **JPEG 原始帧**（g_state → raw） | 拷贝 1 次 | 拷贝 1 次 | **不变**（都是 1 次） |
| **RGB24**（解码结果） | setFrame.assign + QImage.copy() = **拷 2 次** | 解码直写池槽 + QImage 浅引用 = **拷 0 次** | **省掉 2 次** |

```
旧路径：g_state → raw（JPEG拷）→ 解码 → RGB24拷1(setFrame.assign) → RGB24拷2(QImage.copy()) → 上屏
帧池：  g_state → raw（JPEG拷）→ 解码直写池槽 → share引用 → QImage浅引用 → 上屏
         │                                                               │
         └─ 这1次拷贝在优化前后都存在                                └─ 省掉的2次
```

**为什么 raw 的 JPEG 拷贝"消不掉"**：
1. `g_state.frameData` 是**采集线程随时覆盖**的最新帧（`g_state.mtx` 保护）
2. 解码要花 25ms，**不可能持锁 25ms**（会把采集线程卡死）
3. 所以必须短锁拷出 raw，锁外解码

**两个独立维度的优化**：
- **raw 拷贝** = 线程安全/锁竞争的代价（不可省）
- **RGB 拷贝** = 数据搬运的冗余（帧池省掉了）

**代码证据**（[PERF] 注释，main.cpp）：

```cpp
// [PERF] ③④ 已消除：解码直接写池槽（零拷贝），不再有 setFrame assign / QImage.copy()
// [PERF] 本函数 raw = g_state.frameData 是一次原始帧拷贝（JPEG ~0.1MB），计入
g_perf.copyBytes += raw.size();   // ← raw 拷贝仍在统计
```

`copyBytes` 统计里 **raw 那次拷贝一直算着**，只有"③④ RGB 两次拷贝"被标记为"已消除"。

**准确表述（面试直接用）**：
> "帧池优化 = **显示链路的 RGB24 零拷贝**（解码直写池槽 + QImage 浅引用，RGB24 全程 0 次深拷贝）；但显示链路整体仍有 **1 次 JPEG 原始帧拷贝**（g_state → raw），它是线程安全/锁竞争的必然代价，不属于帧池优化目标，优化前后都存在。"

#### ⑥ 一次采集的总账（面试手算）

**MJPEG 模式，无 HTTP/RTSP 客户端，有显示**：

```
mmap 指针 → g_state（拷贝#1：JPEG ~0.1MB）
g_state → localFrame（拷贝#2）
g_state → raw（拷贝#4，仅显示时）
= 每帧 3 次深拷贝（JPEG 量级）+ 显示 RGB24 零拷贝（引用）
```

有网络客户端时再加 HTTP/RTSP 各 1 次 assign（拷贝#3a/3b）。

**一句话总结**：
> "摄像头数据从硬件到各消费端共发生 **4 次深拷贝 + 多次零拷贝引用**：mmap 拿到 DMA 指针是引用（零拷贝），随后采集线程为归还缓冲深拷贝到 g_state（拷贝#1）、处理线程为快速释放锁拷到 localFrame（拷贝#2）、displayTimer 短锁拷到 raw（拷贝#4）、HTTP/RTSP 服务器为多客户端 join 各存一份副本（拷贝#3a/3b），上屏还有一次物理必需的 QPixmap 拷贝。**引用集中在帧池显示链路**——解码直写池槽、share 引用、QImage 浅引用，RGB24 全程零深拷贝。核心规律：**拷贝发生在'线程边界 + 所有权转移'处，是并发模型的必要代价；引用发生在'同一数据多端共享'处，是零拷贝优化的主战场**。注意'显示链路零拷贝'是**限定 RGB24 数据**的说法，JPEG raw 的 1 次拷贝仍存在（1.5.8 ⑤）。"

## 1.6 FramePool 四方法详解（acquire / share / release / publish）

> 面试高频：四个方法怎么配合？为什么不用锁？核心一句话——**这是"数据共享"而非"数据搬移"**：不拷贝帧数据，而是通过引用计数让多线程共享同一块内存。四方法是这套共享机制的四把钥匙。

### 1.6.1 先建立心智模型：核心不变量

关键数据结构（`frame_pool.h:43-50`）：

```cpp
struct FrameSlot {
    std::vector<uint8_t> data;      // 帧数据（RGB24）
    std::atomic<int>     refs{0};   // ★ 引用计数：0=空闲, >0=被持有
    uint64_t             seq{0};    // 帧序号
    int width, height;
    PixelFormat format;
};

class FramePool {
    std::vector<std::unique_ptr<FrameSlot>> m_slots;   // 2 个槽
    std::atomic<FrameSlot*> m_current{nullptr};        // ★ 当前发布槽指针
};
```

**核心不变量**（整个设计的灵魂）：
> **`acquire` 只借空闲槽（refs==0）** —— 由此天然实现读写分离、无锁并发。

### 1.6.2 四个方法逐一拆解

**① acquire() —— 生产者"借"一个空闲槽来写**（`frame_pool.h:76-83`）：

```cpp
FrameSlot* acquire() {
    for (auto& s : m_slots) {              // 遍历所有槽
        int expected = 0;
        if (s->refs.compare_exchange_strong(expected, 1))  // CAS：期望 refs==0，成功则置 1
            return s.get();                // 借到槽，返回指针
    }
    return nullptr;                        // 全被占用 → 池满，返回空
}
```

- 用 **CAS（Compare-And-Swap）** 找第一个 `refs==0`（空闲）的槽
- CAS 成功：`refs` 从 0 变成 1，返回该槽 → **该槽被本线程独占写入**
- 全占用：返回 `nullptr`（调用方丢帧，不阻塞）
- **作用**：生产者写入前调用，**只借 refs==0 的槽**——保证写槽时绝无消费者在读它（读写分离的根源）

**② publish() —— 生产者把写好的槽"发布"为当前**（`frame_pool.h:121-127`）：

```cpp
void publish(FrameSlot* s) {
    std::atomic_thread_fence(std::memory_order_release);  // 先确保 data 写完整
    FrameSlot* old = m_current.exchange(s, std::memory_order_acq_rel);  // 原子替换 current
    if (old)
        release(old);   // 释放旧 current 的"池持有引用"（refs 1→0），旧槽可复用
}
```

- **release fence**：保证 `s->data` 的写入（解码结果）**先于** `m_current` 指针的可见——消费者看到新指针时，数据一定完整
- **`m_current.exchange(s)`**：原子地把 `m_current` 从旧槽指向新槽，**返回旧槽指针**
- **释放旧槽的池持有引用**：`release(old)` 使旧槽 refs 1→0，归零后可被 `acquire` 复用
- **作用**：把"刚写好的槽"变成"消费者可见的当前帧"。**publish 不释放 s 的引用**——新槽以 refs==1 持续被池持有，保证发布期间不被生产者重写（详见 1.6.4）

**③ share() —— 消费者"共享"当前槽的引用**（`frame_pool.h:92-97`）：

```cpp
FrameSlot* share() {
    FrameSlot* cur = m_current.load(std::memory_order_acquire);  // 读当前槽指针
    if (cur)
        cur->refs.fetch_add(1, std::memory_order_relaxed);        // refs+1
    return cur;
}
```

- `m_current.load(acquire)`：取当前槽指针（acquire 与 publish 的 release fence 配对，保证能看到完整数据）
- `refs.fetch_add(1)`：**引用计数 +1**，表示"又一个消费者持有这个槽"
- **作用**：消费者（GUI）读帧前调用，**refs 1→2**——即使 publish 换帧释放旧槽，这个消费者的引用还在，数据不会丢

**④ release() —— 消费者用完归还引用**（`frame_pool.h:102-105`）：

```cpp
void release(FrameSlot* s) {
    if (!s) return;
    s->refs.fetch_sub(1, std::memory_order_release);  // refs-1
}
```

- **作用**：消费者用完归还。当**最后一个** release 使 refs 归 0 时，槽回到空闲态，重新可被 `acquire` 借出复用

### 1.6.3 四方法如何配合（生命周期全景）

用 RGB 显示池（容量 2）的完整一帧流程演示：

```
时间 →  生产者(displayTimer)                   消费者(GUI)
─────────────────────────────────────────────────────────────────
 ①    slot = acquire()          → refs: 0→1（独占写入权）
 ②    解码 → slot->data         → 写 RGB24（refs==1，无人读它）
 ③    publish(slot)             → m_current 指向它；refs 保持 1（池持有）
                                  └ 旧 current 被 release → 0（可复用）
 ④                              → displaySlot = share()
                                     m_current.load() → refs: 1→2
 ⑤                              → setFrameShared(displaySlot)
                                     → 消费数据（refs==2：池1份+GUI1份）
 ⑥   （下一帧）acquire 找 refs==0 → 旧槽已是0 → 借到写新帧
 ⑦                              → GUI 换帧时 release(displaySlot)
                                     → refs: 2→1（池仍持有，current 槽不被重写）
```

**引用计数状态机**：

```
refs=0 ──acquire──► refs=1（生产者独占写入）
refs=1 ──publish──► refs=1（池持有 current，等待消费者）
refs=1 ──share───► refs=2（池 + 1个消费者）
refs=2 ──release──► refs=1（消费者归还）
refs=1 ──publish换帧──► release(old) → refs=0（旧槽可复用）
refs=0 ──acquire──► ...（循环）
```

### 1.6.4 深入：publish 的"引用转移"语义（易错点详解）

> ⚠️ **上一节状态机图最容易误导的地方**：`refs=1 ──publish──► refs=1` 和 `refs=1 ──publish换帧──► refs=0` **不是同一个槽的连续状态，而是两个不同的槽**。publish 每次同时影响两个槽。

**核心：refs 只计数，不区分"这 1 个引用是谁的"**。同样 `refs==1`，有三种不同含义：

| refs==1 时 | 这 1 个引用是谁的 | 代码中发生的事 |
|-----------|------------------|---------------|
| 刚 `acquire()` 完 | **生产者**（有写入权） | `refs 0→1`，生产者独占写 |
| 刚 `publish()` 完 | **池**（m_current 指向它） | 引用从"生产者"**转移**给"池" |
| 消费者 `share()` 后 release 一次 | **池**（消费者已归还） | 消费者用完，池仍持有 |

**关键**：publish 不改变 refs 的数值，它改变的是**这 1 个引用属于谁**——从"生产者的写入权"变成"池的 current 持有权"。这就是"refs=1 → publish → refs=1"看起来没变、但语义完全变了的原因。

**逐行走代码**（关键在 `exchange` 的返回值）：

```cpp
void publish(FrameSlot* s) {
    std::atomic_thread_fence(std::memory_order_release);   // ① 数据写完整
    FrameSlot* old = m_current.exchange(s, std::memory_order_acq_rel);  // ② ★
    if (old)
        release(old);                                      // ③ 释放旧槽
}
```

`m_current.exchange(s)` 是**原子交换**：把 `m_current` 指向新槽 `s`，**同时返回交换前指向的旧槽 `old`**。这一步同时完成两件事：

```
调用前：m_current → old（旧槽，refs=1，池持有）
        s（新槽，refs=1，生产者写入权）

exchange(s) 后：m_current → s（新槽接管 current 地位）
               返回值 = old

然后：release(old) → 旧槽 refs 1→0
```

**完整两帧流程**（跟着 refs 走一遍，池容量 2，槽 A、B）：

```
步骤    代码                     槽A.refs    槽B.refs    谁持有槽A的引用
─────────────────────────────────────────────────────────────────────
1      slot = acquire()          0→1        0          [生产者]
2      解码写入 slot->data       1          0          [生产者]
3      publish(slot)            1（不变）    0          [池]  ← 引用从生产者转移给池
       m_current: nullptr → A

4      displaySlot = share()     1→2        0          [池 + 消费者]
5      GUI 换帧后 release()      2→1        0          [池]（消费者归还）

6      slot = acquire()          1          0→1        [池(A)]、[生产者(B)]
       ★ acquire 只借 refs==0 的槽 → 借到 B（A 是 refs==1，借不到 → 不会被重写）
7      解码写入 B                1          1          [池(A)]、[生产者(B)]
8      publish(B)：
       m_current.exchange(B)      → m_current: A→B，返回值 = A（旧槽）
       release(A)                1→0        1          A 归零，B 由生产者转池
```

**第 8 步才是"refs=1 → release → refs=0"的真相**：旧槽 A 在"别人（B）发布"时被 `exchange` 的返回值带出来，然后 `release(old)` 释放**池持有 A 的那 1 个引用**，A 才从 1 变 0。

**为什么要有这层"引用转移"**：

| 问题 | 机制 |
|------|------|
| 新槽发布后，为什么不能马上被生产者重写？ | 因为它的 refs 还是 1（池持有）→ `acquire` 借不到它 |
| 旧槽被换下后，为什么可以立刻复用？ | 因为 `release(old)` 让它 refs 归 0 → `acquire` 借得到它 |

**逻辑闭环**：
- `acquire` 只借 refs==0 → 池持有的 current 槽（refs==1）永远不会被生产者重写
- 换帧时池"放弃"旧槽（release→0）→ 旧槽回到可复用池
- 如果消费者 `share()` 过旧槽（refs 变 2），换帧时 release 后是 2→1，**仍 >0** → 消费者还在读，不能复用，等消费者 release 才归 0

### 1.6.5 为什么不直接用锁？（无锁设计的关键）

**整个池没有一把 mutex**，靠两个原子机制保证安全：

| 机制 | 保证什么 |
|------|---------|
| **`acquire` 只借 refs==0** | 生产者写槽时，绝无消费者在读该槽（读写分离） |
| **引用计数 `share`/`release`** | 消费者持有时数据不会丢；最后一个 release 才复用 |
| **`m_current` 原子指针 + 内存序** | publish 的 release fence 与 share 的 acquire load 配对，保证"看到指针 = 看到完整数据" |
| **CAS（`compare_exchange_strong`）** | 多个生产者同时 acquire 时，只有一个能成功借到同一槽 |

**为什么能无锁**：因为"借槽写"和"读 current"操作的是**不同的槽**——生产者写 refs==1 的槽，消费者读 refs>0 的已发布槽，两者永不冲突。这就是 `main.cpp:114` 注释说的"容量 2：GUI 持 1 槽 + 解码写 1 槽，天然双缓冲，无需锁"。

### 1.6.6 实际调用点（main.cpp + gui.cpp）

```cpp
// main.cpp displayTimer（生产者路径）
FrameSlot* slot = g_rgbPool->acquire();     // ① 借槽
if (!slot) return;                          //   池满丢帧，不阻塞
decodeJPEGtoRGB(raw.data(), raw.size(), slot->data, dw, dh);  // ② 解码入槽
slot->seq++;
g_rgbPool->publish(slot);                   // ③ 发布
FrameSlot* displaySlot = g_rgbPool->share();// ④ 共享引用（生产者自己 share 给 GUI）
if (displaySlot) gui.setFrameShared(displaySlot);  // ⑤ GUI 接管
gui.requestRefresh();

// gui.cpp setFrameShared（消费者路径）
void CameraGUI::setFrameShared(FrameSlot* slot) {
    if (m_heldSlot) {
        if (g_rgbPool) g_rgbPool->release(m_heldSlot);  // ⑥ 释放上一帧
        m_heldSlot = nullptr;
    }
    m_heldSlot = slot;    // 持有新帧引用（refs 保持 2：池1 + GUI1）
    m_currentFrame.data = slot->data.data();   // 零拷贝指向共享内存
    ...
}

// gui.cpp ~CameraGUI / 换帧时
g_rgbPool->release(m_heldSlot);   // ⑦ GUI 归还引用
```

### 1.6.7 面试速记表

| 方法 | 谁调用 | refs 变化 | 作用 | 对应生活比喻 |
|------|--------|-----------|------|-------------|
| `acquire` | 生产者 | 0→1 | 借空闲槽独占写入 | 借空房间写文章 |
| `publish` | 生产者 | 保持 1（换帧时旧槽 1→0） | 把写好的槽发布为当前 | 贴到公告栏 |
| `share` | 消费者 | 1→2 | 共享当前槽引用 | 抄一份（不搬走） |
| `release` | 消费者 | 2→1→0 | 归还引用，最后一人释放 | 用完了交还钥匙 |

**面试一句话**：
> "acquire 是生产者借空闲槽（refs 0→1，CAS 保证独占）；publish 把写好的槽发布为当前（release fence 保证数据先于指针可见，换帧时旧槽释放）；share 是消费者取当前槽的共享引用（refs+1，保证数据不丢）；release 是归还引用（refs-1，最后一个 release 使槽回到可复用）。四者通过 refs 引用计数 + m_current 原子指针 + 'acquire 只借空闲槽'的不变量，实现**无锁双缓冲**——写槽和读槽永不冲突，这是帧池零拷贝能安全运行的核心。"

## 1.7 C++ 内存序（memory_order）详解

> 面试深挖：帧池"无锁为什么安全"的底层根基。核心一句话——**memory_order 是告诉编译器和 CPU "内存操作可见性的顺序约束"**，release/acquire 配对是理解帧池安全性的关键。

### 1.7.1 为什么需要内存序？

现代 CPU 有多级缓存 + 乱序执行，**每个线程看到的"内存写入顺序"可能不一样**：

```cpp
// 线程 A（生产者）
slot->data = 解码结果;      // ① 写数据
m_current.store(slot);      // ② 发布指针

// 线程 B（消费者）
FrameSlot* s = m_current.load();  // ③ 读指针
use(s->data);                     // ④ 用数据
```

**问题**：如果 ② 先于 ① 被线程 B 看到（CPU 重排/缓存延迟），B 会拿到新指针、却读到**旧数据**——数据竞争 bug。**内存序就是给编译器/CPU 的指令：怎么安排这些可见性**。

### 1.7.2 三种内存序在本项目的实际代码

`frame_pool.h` 三个方法各用一种，正好是经典的"release/acquire 配对"：

```cpp
// ① release：用于 publish —— "写发布"
void publish(FrameSlot* s) {
    std::atomic_thread_fence(std::memory_order_release);   // 数据写完整，再发布指针
    FrameSlot* old = m_current.exchange(s, std::memory_order_acq_rel);
    if (old) release(old);
}

// ② acquire：用于 share —— "读获取"
FrameSlot* share() {
    FrameSlot* cur = m_current.load(std::memory_order_acquire);  // 看到指针 = 数据完整
    if (cur) cur->refs.fetch_add(1, std::memory_order_relaxed);
    return cur;
}

// ③ acq_rel：用于 publish 里的 exchange —— 同时读旧值 + 写新值
FrameSlot* old = m_current.exchange(s, std::memory_order_acq_rel);
```

### 1.7.3 逐个解释

**① `memory_order_release`（释放语义）—— 用于"写发布"**

- 含义：在 release **之前**的所有内存写操作，都不会被重排到 release 之后；且这些写对"配对的 acquire 读者"**可见**
- 通俗理解：release 像"关闸门"——闸门放下前所有货（数据）都**先装好、再放行**。读者拿到放行信号（acquire）时，货一定齐了
- 类比：在黑板上写完答案（写 data），然后**拍手示意**（release）——拍手保证"答案先写完，才拍手"

**② `memory_order_acquire`（获取语义）—— 用于"读获取"**

- 含义：在 acquire **之后**的所有内存读操作，都不会被重排到 acquire 之前；且能看到配对的 release 之前写入的所有数据
- 通俗理解：acquire 像"开门闸"——开门后后续读才能进行；开门时，release 方写的所有东西都**可见了**
- 类比：别人拍手（release）后你才转身看黑板（acquire）——转身保证"看到的一定是拍手时已写好的完整答案"

**③ `memory_order_acq_rel`（获取-释放语义）—— 用于"读改写"（RMW）**

- 含义：**同时具备 acquire 和 release 两种语义**——既保证之前的写对别人可见（release 部分），又能看到别人的最新写（acquire 部分）。**只用于读-改-写操作**（exchange / fetch_add / compare_exchange 等）
- 为什么 `exchange` 需要 acq_rel：
  - **读旧值 `old`**（acquire 部分）：要看到别的线程对 `m_current` 的最新写
  - **写新值 `s`**（release 部分）：要让自己之前的写对读者可见
  - `exchange` 是"读旧 + 写新"组合操作，所以用 acq_rel

### 1.7.4 内存序强度对照表

| 内存序 | 本质 | 本项目用途 | 强度 |
|--------|------|-----------|------|
| `relaxed` | 无任何顺序保证，只保证原子性 | `fetch_add` 计数器（refs 增减） | 最弱 |
| `release` | 之前的写对 acquire 读者可见 | publish 发布指针前 | 中 |
| `acquire` | 之后能读到 release 方写的数据 | share 读指针时 | 中 |
| `acq_rel` | 两者兼有（只用于 RMW） | exchange 交换指针时 | 强 |
| `seq_cst` | 全局一致顺序（默认） | 未用（太贵） | 最强 |

### 1.7.5 release/acquire 配对的"契约"（帧池安全的核心）

**release 和 acquire 必须成对使用，才能建立"同步关系"**：

```
线程 A（publish）                          线程 B（share）
─────────────────                        ─────────────────
写入 data（解码结果）                       │
     │                                    │
     │  release fence                     │
     ▼                                    │
exchange(m_current, acq_rel)  ──同步──►  load(m_current, acquire)
                                           │
                                           保证：看到新指针时，data 一定完整
```

**同步关系**：在**同一个原子变量**（`m_current`）上，线程 A 做 release 写、线程 B 做 acquire 读（读到 A 写的值），则 A 在 release 之前的所有写（`data`），B 在 acquire 之后一定能看到。

**这正是帧池零拷贝安全的核心**：消费者 `share()` 拿到 `m_current` 指针的那一刻，`acquire` 保证生产者 `publish()` 在 `release` 之前写进 `slot->data` 的解码结果**一定可见**——不会出现"拿到新指针、读到旧数据"。

### 1.7.6 为什么 `refs` 用 `relaxed`，release() 里却用 release？

```cpp
cur->refs.fetch_add(1, std::memory_order_relaxed);   // share 里
s->refs.fetch_sub(1, std::memory_order_release);     // release() 里
```

**refs 计数器本身不关心顺序**：
- `fetch_add(1)` 只是"数加 1"，不依赖别的数据——`relaxed` 足够
- 但 `release()` 里的 `fetch_sub(1)` 用了 `release`——因为"归还引用"意味着"我读完了 data"，要保证**读 data 的操作先于 refs 减 1**，否则别的线程可能误以为该槽没人用了而复用它，导致读一半数据

**memory_order 的选择标准**：**只有跨线程传递"数据依赖"时才需要 acquire/release；纯计数器（不依赖数据内容）用 relaxed 即可**。

### 1.7.7 面试一句话总结

> "memory_order 是告诉编译器和 CPU '内存操作可见性的顺序约束'。release 表示'我之前的写，对配对的 acquire 读者可见'（先写数据、再发指针）；acquire 表示'我能看到配对的 release 写方的所有数据'（看到指针 = 数据完整）；acq_rel 用于 exchange 这类读改写，两者兼有。本项目帧池正是靠 'publish 的 release + share 的 acquire' 在同一个 m_current 变量上配对，保证消费者拿到指针时解码数据一定完整——这是无锁零拷贝安全的根基；而 refs 计数器只用 relaxed（不传递数据依赖），release() 里用 release 是为了防止'数据还没读完就被复用'。"

---

# 第二部分 线程编排与配置解析

## 2.1 六线程模型

| 线程 | 创建位置 | 职责 | 生命周期 |
|------|---------|------|---------|
| Qt 主线程 | `main` 进程本身 | GUI + displayTimer(33ms 解码上屏) + perfTimer(5s) | 整个 app |
| 采集线程 | `main.cpp:873` | `getFrame → 拷贝到 g_state → putFrame → notify`，O(1) 轻活 | `g_state.running=false` 退出 |
| 处理线程 | `main.cpp:970` | 等新帧 → YUYV 编码 → HTTP/RTSP 推流 → 录像写盘 | 同上 |
| RTSP 线程 | `main.cpp:712` | `rtspServer->start()` 阻塞事件循环（epoll） | `rtspServer->stop()` 退出 |
| 控制线程 | `main.cpp:859` | `controlSrv->start()` 阻塞事件循环（epoll） | `controlSrv->stop()` 退出 |
| HTTP 客户端线程 | MJPEG 服务器内部 | 每接入一个客户端 detach 一个 handler 线程 | 客户端断开 |

**关键设计**：
- **采集线程只做轻活**：取帧/拷贝/归还三个操作，把"编码、推流、写盘"等重活全部下沉到处理线程——避免慢客户端/磁盘 IO 阻塞取帧导致 4 缓冲耗尽（V4L2 缓冲池租借模型）
- **RTSP/控制各自独立线程**：互不阻塞，都是 `epoll` 单线程事件循环
- **退出协调**：`g_state.running=false` + `procCv.notify_all()` 唤醒处理线程 → 各线程 join → 资源清理

## 2.2 配置三级优先级

```
命令行 > 配置文件 > 硬编码默认值
```

```cpp
QString device = parser.isSet(deviceOpt)
    ? parser.value(deviceOpt)                       // ① 命令行最高
    : (cfgLoaded ? QString::fromStdString(cfg.getString("camera", "device")) : QString());
                                                    // ② 配置文件
                                                    // ③ 无配置→默认（空=Mock 模式）
```

配置文件查找路径：`--config` 显式指定 > `~/.config/smartcam/smartcam.conf` > `/etc/smartcam/smartcam.conf`。

**面试点**：这种三级优先级是嵌入式系统的标配——**命令行给调试者临时覆盖能力，配置文件给运维默认值，硬编码给"没配置文件也能跑"的兜底**。

## 2.3 命令行解析详解（QCommandLineParser）

### 2.3.1 整体流程三步走

```
第 1 步（204-214 行）：准备
    QApplication app(argc, argv)          ← Qt 应用初始化（消费 Qt 自带参数）
    parser 基础设置（描述/帮助/版本）

第 2 步（216-262 行）：定义选项
    QCommandLineOption 构造 6 个自定义选项 → parser.addOption()

第 3 步（264-314 行）：解析并取值
    parser.process(app)                   ← 正式解析
    parser.isSet() / parser.value()       ← 读取用户传入的值
    → 与配置文件、默认值做三级合并
```

### 2.3.2 核心类与函数原型

**QApplication 构造函数**：

```cpp
QApplication app(int &argc, char **argv);   // argc 必须是非 const 引用！
```

- 初始化 Qt 资源与平台插件（linuxfb / xcb 等），任何 Qt GUI 程序的第一步
- **解析并移除 Qt 平台自身认识的参数**（如 `-platform linuxfb`、`-style`），剩余参数才交给应用
- `argc` 非 const 引用——Qt 会从 `argc/argv` 中移除它消费掉的参数
- `setApplicationName` / `setApplicationVersion` 供 `--version` 输出（`SmartCam 0.1.0`）

**QCommandLineParser 常用方法**：

```cpp
void setApplicationDescription(const QString &description);  // 帮助里的描述
void addHelpOption();                                         // 注册 --help / -h / -?
void addVersionOption();                                      // 注册 --version / -v
void addOption(const QCommandLineOption &option);
bool process(const QStringList &arguments);   // 或 process(app) / process(*QCoreApplication::instance())
bool isSet(const QCommandLineOption &option) const;   // 用户是否传了该选项
QString value(const QCommandLineOption &option) const;  // 取选项值
```

**QCommandLineOption 构造函数**（本项目用的是 4 参版本）：

```cpp
QCommandLineOption(
    const QString &name,          // 长选项名：--name
    const QString &description,   // 帮助文本
    const QString &valueName,     // 值名（帮助里显示，可选）
    const QString &defaultValue   // 默认值（可选）
);
```

> 等价写法：`QCommandLineOption({QStringList() << "d" << "device", "描述"})` 可同时给短选项（`-d`）和长选项（`--device`）。本项目**只用长选项**，没有短选项别名。

### 2.3.3 六个选项逐个拆解

```cpp
QCommandLineOption configOpt(
    QStringLiteral("config"),
    QStringLiteral("配置文件路径 (默认 /etc/smartcam/smartcam.conf)"),
    QStringLiteral("config"),
    QStringLiteral("/etc/smartcam/smartcam.conf")   // ← 有实际默认值
);
QCommandLineOption deviceOpt(
    QStringLiteral("device"),
    QStringLiteral("V4L2 设备路径, 例如 /dev/video0"),
    QStringLiteral("device"),
    QString()                                        // ← 空 → 运行时决定 Mock
);
QCommandLineOption portOpt(
    QStringLiteral("http-port"),
    QStringLiteral("MJPEG-over-HTTP 端口 (默认 8080)"),
    QStringLiteral("port"),
    ""                                               // ← 空 → 从配置文件读取
);
// ctrlPortOpt(control-port) / rtspPortOpt(rtsp-port) / fmtOpt(fmt) 同 portOpt 模式
```

| 变量 | 选项名 | 值名 | 默认值 | 作用 |
|------|--------|------|--------|------|
| `configOpt` | `--config` | `config` | `/etc/smartcam/smartcam.conf` | 配置文件路径 |
| `deviceOpt` | `--device` | `device` | **空字符串** | V4L2 设备；空=Mock 模式 |
| `portOpt` | `--http-port` | `port` | **空字符串** | HTTP 端口（空→读配置） |
| `ctrlPortOpt` | `--control-port` | `port` | **空字符串** | 控制端口 |
| `rtspPortOpt` | `--rtsp-port` | `port` | **空字符串** | RTSP 端口 |
| `fmtOpt` | `--fmt` | `fmt` | **空字符串** | 像素格式 yuyv/mjpeg |

**三种默认值策略（设计亮点）**：

| 策略 | 选项 | 默认值 | 意图 |
|------|------|--------|------|
| A：有实际默认值 | `configOpt` | `/etc/smartcam/smartcam.conf` | 路径有明确系统约定 |
| B：空 + 运行时决定 | `deviceOpt` | 空字符串 | 无配置时进入 Mock 模式 |
| C：空 + 交给配置文件 | 4 个端口/格式 | 空字符串 | **让配置文件能生效**（哨兵值技巧） |

> **为什么端口默认值留空而不直接写 8080？** 配置优先级是"命令行 > 配置文件 > 默认值"。若构造时填 8080，`parser.value(portOpt)` 永远非空，代码无法区分"用户没传"和"用户传了 8080"——配置文件里的端口会被永久遮蔽。**留空字符串作"哨兵值"，让 `isSet()` 成为唯一判断依据**，才能干净实现三级优先级。

### 2.3.4 触发解析与取值

```cpp
parser.process(app);    // 等价 process(QCoreApplication::arguments())，自动跳过 argv[0]
```

`--help` / `--version` 由内置选项处理：解析到即打印并 `exit(0)`，不走到后面。

**取值三连**：

```cpp
// ① 配置文件路径（第 2 层优先级）
QString configPath;
if (parser.isSet(configOpt)) {
    configPath = parser.value(configOpt);       // --config 显式指定最高
} else {
    const char* home = getenv("HOME");
    if (home) {
        std::string userCfg = std::string(home) + "/.config/smartcam/smartcam.conf";
        if (cfg.load(userCfg)) configPath = QString::fromStdString(userCfg);  // 用户级
    }
    if (configPath.isEmpty())
        configPath = QStringLiteral("/etc/smartcam/smartcam.conf");            // 系统级
}
```

**查找顺序**：`--config` 显式指定 → `~/.config/smartcam/smartcam.conf`（存在才用）→ `/etc/smartcam/smartcam.conf`。注意这里**先 `cfg.load()` 探测用户级配置是否存在**（load 失败返回 false），存在才用，否则落到系统级。

**三级合并模式（以端口为例）**：

```cpp
int httpPort = parser.isSet(portOpt)
    ? parser.value(portOpt).toInt()               // ① 命令行
    : cfg.getInt("network", "http_port", 8080);   // ②③ 配置 > 默认
```

`cfg.getInt(section, key, defaultValue)` 本身是"配置 > 默认"合并（无 key 时返回 8080），外层 `isSet` 判断"命令行是否覆盖"。

**大小写归一化（fmt）**：

```cpp
QString fmtStr = parser.isSet(fmtOpt)
    ? parser.value(fmtOpt).toLower()              // ← .toLower() 统一小写
    : (cfgLoaded ? QString::fromStdString(cfg.getString("camera", "format", "mjpeg")).toLower()
                 : QStringLiteral("mjpeg"));
```

用户传 `--fmt MJPEG` 和 `--fmt mjpeg` 效果一样，避免大小写敏感导致格式匹配失败。

### 2.3.5 实际使用示例

```bash
# ① 什么都不传 → Mock 模式（device 空）+ 配置默认端口
./smartcam

# ② 真实相机，全部默认
./smartcam --device /dev/video0

# ③ 指定端口（命令行覆盖配置）
./smartcam --device /dev/video0 --http-port 9090 --rtsp-port 9554

# ④ 指定格式 + 指定配置文件
./smartcam --config /home/root/myconfig.conf --device /dev/video0 --fmt yuyv

# ⑤ 帮助 / 版本
./smartcam --help
./smartcam --version
```

`--help` 输出大致为：

```
Usage: ./smartcam [options]
SmartCam Linux — 基于 iMX6ULL 的智能相机流媒体系统

Options:
  -h, --help                    Displays help on commandline options.
  -v, --version                 Displays version information.
  --config <config>             配置文件路径 (默认 /etc/smartcam/smartcam.conf)
  --device <device>             V4L2 设备路径, 例如 /dev/video0
  --http-port <port>            MJPEG-over-HTTP 端口 (默认 8080)
  --control-port <port>         TCP 控制协议端口 (默认 9000)
  --rtsp-port <port>            RTSP 流媒体端口 (默认 8554)
  --fmt <fmt>                   像素格式: yuyv | mjpeg (默认 mjpeg)
```

### 2.3.6 面试可讲的点

1. **`argc` 非 const 引用**：`QApplication` 会消费 Qt 平台参数（如 `-platform linuxfb`），签名是 `int &argc`——与标准 `main(int argc, char* argv[])` 的差异点。
2. **默认值留空 = 哨兵值技巧**：端口默认值故意留 `""`，让 `isSet()` 成为"用户是否显式传入"的唯一判断依据，从而干净实现三级优先级。
3. **`parser.process(app)` vs `process(args)`**：`process(app)` 等价于 `process(QCoreApplication::arguments())`，自动跳过 `argv[0]`。
4. **`--help`/`--version` 由 Qt 内置**：`addHelpOption()` / `addVersionOption()` 一行注册，解析到即自动打印退出。
5. **`toLower()` 归一化**：格式参数统一小写，避免大小写敏感 bug。
6. **isSet + value 的配合**：`isSet` 判断"传没传"，`value` 取"传的值"——**注意即使有默认值，isSet 仍为 false**，这正是三级合并能工作的前提。

## 2.4 回调注入（业务编排）

main.cpp 通过 `gui.onXxxChanged(lambda)` 把 GUI 事件桥接为业务动作，lambda 捕获 `capture`/`rtspServer` 等指针：

```cpp
gui.onResolutionChanged([capture](int w, int h) {
    g_state.paused = true;                    // ① 暂停采集线程
    // ② 等采集线程确认暂停（getFrame 1s 超时，最多等 1.1s）
    capture->stopCapture();                   // ③ 安全停止
    capture->setFormat(w, h, ...);            // ④ 改格式
    capture->startCapture();                  // ⑤ 重启
    g_state.paused = false;                   // ⑥ 恢复
});
```

**为什么要"暂停→停流→改→重启"**：V4L2 的 `S_FMT` 在 STREAMON 下返回 `EBUSY`，且改格式时 mmap 缓冲正在被采集线程使用——必须先让采集线程确认暂停再动。

**面试点**：回调注入 = **依赖倒置**——display 模块不依赖 camera 类，只依赖 main.cpp 注入的 `std::function`。这使 GUI 可独立测试（Mock 模式），也把"业务编排"集中在 main.cpp 一处。

## 2.5 三个相机回调的区别（onCameraControlChanged / onResolutionChanged / onFormatChanged）

> 面试高频辨析题：这三个回调都跟"改相机"有关，但**底层走完全不同的 V4L2 路径**。核心一句话：`onCameraControlChanged` 是"拧旋钮"（写寄存器，不断流）；`onResolutionChanged` / `onFormatChanged` 是"换镜头配置"（重建缓冲池，必须断流）。

### 2.5.1 核心区别表

| 回调 | 改什么 | 底层 V4L2 操作 | 是否要重启采集流 |
|------|--------|---------------|-----------------|
| `onCameraControlChanged` | 传感器**图像参数**（亮度/对比度/白平衡） | `VIDIOC_S_CTRL` | **不用**，即时生效 |
| `onResolutionChanged` | **分辨率**（宽×高） | `VIDIOC_S_FMT` | **要**，停流→改→重启 |
| `onFormatChanged` | **像素格式**（YUYV/MJPEG） | `VIDIOC_S_FMT` | **要**，停流→改→重启 |

### 2.5.2 逐个拆解（结合代码）

**① onCameraControlChanged —— 调图像参数，即时生效**（`main.cpp:597-606`）：

```cpp
gui.onCameraControlChanged([capture](int cid, int value) {
    int ret = capture->setControl(cid, value);      // 直接写寄存器
    if (ret < 0) {
        LOG_WRN("setControl(cid=0x%08X, val=%d) failed (ret=%d)",
                 static_cast<uint32_t>(cid), value, ret);
    } else {
        LOG_INF("Camera control: cid=0x%08X → %d", ...);
    }
});
```

- **入参**：`(int cid, int value)` —— cid 是 V4L2 控件 ID（亮度 `0x00980900`、对比度 `0x00980901`、白平衡等）
- **调用点**：GUI 亮度/对比度/白平衡**滑块拖动时**（`gui.cpp` 的 `onBrightnessChanged` 等）
- **底层**：`capture->setControl(cid, value)` → `VIDIOC_S_CTRL` → 直接写传感器寄存器
- **关键**：**不需要重启采集流**——改的是 sensor 内部增益/偏移寄存器，DMA 管线不受影响，下一帧立即生效

**② onResolutionChanged —— 改分辨率，必须重建缓冲**（`main.cpp:1202-1229`）：

```cpp
gui.onResolutionChanged([capture](int w, int h) {
    if (!capture->isStreaming()) return;

    g_state.paused = true;                       // 1. 暂停采集线程
    {   // 等采集线程确认暂停（getFrame 1s 超时，最多等 1.1s）
        std::unique_lock<std::mutex> lk(g_state.pauseMtx);
        g_state.pauseCv.wait_until(lk, now+1100ms,
            [] { return g_state.pausedAck.load(); });
    }
    capture->stopCapture();                      // 2. 安全停止
    int ret = capture->setFormat(w, h, capture->getCurrentFormat());  // 只改 size
    if (ret < 0) { capture->setFormat(640, 480, ...); }   // 失败回退
    capture->startCapture();                     // 3. 重启
    g_state.paused = false;                      // 4. 恢复
    g_state.pauseCv.notify_one();
});
```

- **入参**：`(int w, int h)` —— 目标宽高
- **调用点**：设置面板分辨率下拉框（`onResolutionComboChanged`）
- **底层**：`VIDIOC_S_FMT`（只改 size，格式保持当前）
- **关键**：**必须走完整流程**——分辨率变了，mmap 缓冲池大小必须重建（`startCapture` 内部重新 REQBUFS + mmap）。不做 3 步会出问题：不暂停 → stopCapture 时采集线程还在用 mmap 缓冲 → 竞态；不重建缓冲 → 帧大小与旧缓冲不匹配 → 越界/花屏

**③ onFormatChanged —— 改像素格式，同样必须重启**（`main.cpp:1231-1259`）：

```cpp
gui.onFormatChanged([capture, device](PixelFormat fmt) {
    if (!capture->isStreaming()) return;

    uint32_t v4l2fmt = (fmt == PixelFormat::FMT_YUYV)
                           ? CameraCapture::V4L2_PIX_FMT_YUYV
                           : CameraCapture::V4L2_PIX_FMT_MJPEG;

    g_state.paused = true;                       // 暂停采集线程（同 onResolutionChanged）
    { ... 同样的暂停握手 ... }
    capture->stopCapture();
    int ret = capture->setFormat(640, 480, v4l2fmt);   // 只改 pixelformat
    if (ret < 0) { LOG_ERR_(...); }
    capture->startCapture();
    g_state.paused = false;
    g_state.pauseCv.notify_one();
});
```

- **入参**：`(PixelFormat fmt)` —— 枚举类型（`FMT_YUYV` / `FMT_MJPEG`）
- **调用点**：设置面板格式下拉框
- **底层**：`VIDIOC_S_FMT`（只改 pixelformat，分辨率固定 640x480）
- **关键**：**同样必须重启**——YUYV 帧 `w*h*2` 字节、MJPEG 压缩变长，**缓冲池大小/布局完全不同**，必须 REQBUFS 重来

### 2.5.3 为什么"改参数"不用重启，而"改格式/分辨率"必须重启？

| 维度 | onCameraControlChanged | onResolutionChanged / onFormatChanged |
|------|----------------------|--------------------------------------|
| **改的是** | sensor 内部寄存器（增益/偏移） | 缓冲池的**布局参数**（大小/格式） |
| **底层 ioctl** | `VIDIOC_S_CTRL`（写寄存器） | `VIDIOC_S_FMT`（重建格式+缓冲） |
| **缓冲池是否受影响** | 否，DMA 管线不变 | **是**，帧大小/格式全变 |
| **STREAMON 下能否调用** | 可以（即时生效） | **不行**，`S_FMT` 在 STREAMON 下返回 `EBUSY` |
| **代码流程** | 一行 `setControl` | 暂停→停流→S_FMT→重启→恢复 |

**两个底层原因**：
1. **`VIDIOC_S_FMT` 在 STREAMON 状态返回 `EBUSY`**——必须 `STREAMOFF` 后才能改格式/分辨率（V4L2 规范强制）
2. **格式/分辨率变化 → 缓冲区不匹配**——YUYV 640x480 帧 614KB、MJPEG 压缩后几十 KB，旧 mmap 缓冲装不下新格式；必须 REQBUFS 重新按新参数分配

而 `VIDIOC_S_CTRL` 只是写一个寄存器值，DMA 管线不用重启，所以**可以随时调、即时生效**。

### 2.5.4 共同点（为什么都叫"回调"）

三者都是 main.cpp **注入到 GUI 的 `std::function` lambda**（依赖倒置）：
- GUI 侧只"发事件"（滑块动了、下拉框变了）
- main.cpp 的 lambda 决定"怎么做"（操作 capture 对象）
- 好处：display 模块零依赖 camera 类，Mock 模式可注入空 lambda

```cpp
// Mock 模式（main.cpp:1359-1364）——三个都注入空实现
gui.onResolutionChanged([](int w, int h) { qDebug() << "分辨率变更"; });
gui.onFormatChanged([](PixelFormat fmt) { qDebug() << "格式变更"; });
```

### 2.5.5 面试速记表

| | onCameraControlChanged | onResolutionChanged | onFormatChanged |
|---|---|---|---|
| 参数类型 | `(int cid, int value)` | `(int w, int h)` | `(PixelFormat fmt)` |
| 改什么 | 亮度/对比度/白平衡 | 宽×高 | YUYV/MJPEG |
| 底层 | `VIDIOC_S_CTRL` | `VIDIOC_S_FMT` | `VIDIOC_S_FMT` |
| 重启流 | ❌ 即时生效 | ✅ 必须 | ✅ 必须 |
| 失败回退 | 无 | 640x480 | 无（保持原格式） |
| 触发控件 | 滑块 | 分辨率下拉框 | 格式下拉框 |

**面试一句话**：三者都是 main.cpp 注入给 GUI 的回调，区别在**改的对象**：`onCameraControlChanged` 调 sensor 图像参数（`VIDIOC_S_CTRL` 写寄存器），**即时生效、无需断流**；`onResolutionChanged` 和 `onFormatChanged` 都改**缓冲池布局**（`VIDIOC_S_FMT`），因为 S_FMT 在 STREAMON 下返回 EBUSY、且帧大小变了缓冲必须重建，所以**都要走"暂停→停流→改→重启→恢复"完整流程**，区别只是前者只改 size、后者只改 pixelformat。

## 2.6 onResolutionChanged 深入：paused vs stopCapture 与双线程握手

> 面试深挖："为什么改个分辨率要这么麻烦？`paused=true` 和 `stopCapture()` 有什么区别？"这一节把 onResolutionChanged 的完整实现思路讲透。

### 2.6.1 paused vs stopCapture：应用层协调 vs 驱动层停流

| | `g_state.paused = true` | `capture->stopCapture()` |
|---|---|---|
| **操作对象** | 应用层的**标志位**（`std::atomic<bool>`） | V4L2 **内核驱动**（`VIDIOC_STREAMOFF`） |
| **本质** | 通知采集线程"你先别拉帧了" | 让驱动**停止 DMA 采集** + 释放 mmap 缓冲 |
| **谁执行** | 只是写一个变量 | 执行 `ioctl` 系统调用 + `unmapBuffers` |
| **是否阻塞** | 不阻塞（原子写） | 阻塞（ioctl + munmap） |
| **影响范围** | 只影响采集线程的**循环逻辑** | 影响**整个硬件采集链路** |

```cpp
// ① paused：应用层协调标志
g_state.paused = true;     // 采集线程看到它 → 停止取帧，进入等待

// ② stopCapture：V4L2 驱动级停流
int CameraCapture::stopCapture() {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(m_fd, VIDIOC_STREAMOFF, &type);   // 停 DMA
    m_streaming = false;
    unmapBuffers();                          // munmap + REQBUFS(0) 释放缓冲
    ...
}
```

**为什么要两个？** 解决的是**两个不同层面的问题**：
- `stopCapture()` 停的是硬件流，但**采集线程可能正卡在 `getFrame()`（select 等待 DQBUF）里**，如果直接停流，采集线程还在用 mmap 缓冲 → 竞态/崩溃
- 所以必须先 `paused=true` **让采集线程主动退出取帧循环**，等它确认（`pausedAck`）后才安全地 `stopCapture()`

**类比**：`paused=true` 是"让工人先放下手里的活"，`stopCapture()` 是"关掉机器"。必须先让工人停手，才能安全关机器。

### 2.6.2 完整代码（`main.cpp:1202-1229`）

```cpp
gui.onResolutionChanged([capture](int w, int h) {
    if (!capture->isStreaming()) return;                       // ① 防御

    // 1. 暂停采集线程，防止 stopCapture 时采集线程还在使用 mmap 缓冲区
    g_state.paused = true;                                     // ② 置暂停标志
    // 等待采集线程确认暂停（getFrame 有 1s 超时，最多等 1.1s）
    {
        std::unique_lock<std::mutex> lk(g_state.pauseMtx);     // ③ 条件变量等待
        g_state.pauseCv.wait_until(lk,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(1100),
            [] { return g_state.pausedAck.load(); });
    }

    // 2. 安全停止采集、切换格式、重启
    capture->stopCapture();                                    // ④ 停流
    int ret = capture->setFormat(w, h, capture->getCurrentFormat());  // ⑤ 改分辨率
    if (ret < 0) {                                             // ⑥ 失败回退
        LOG_ERR_("setFormat(%dx%d) failed (ret=%d), reverting to 640x480",
                  w, h, ret);
        capture->setFormat(640, 480, capture->getCurrentFormat());
    }
    capture->startCapture();                                   // ⑦ 重启

    // 3. 恢复采集线程
    g_state.paused = false;                                    // ⑧ 解除暂停
    g_state.pauseCv.notify_one();                              // ⑨ 唤醒采集线程
    LOG_INF("Resolution changed to %dx%d", w, h);
});
```

### 2.6.3 实现思路：为什么是"暂停 → 停流 → 改 → 重启 → 恢复"五步？

**核心矛盾**：V4L2 的 `S_FMT` 有两个硬性限制：
1. **STREAMON 状态下调用 `S_FMT` 返回 `EBUSY`**——必须先 STREAMOFF
2. **改分辨率后 mmap 缓冲必须重建**——帧大小变了，旧缓冲装不下

而采集线程一直在跑（拉帧/归还缓冲），**直接停流会和采集线程打架**。所以必须设计一套"双线程协作"的握手协议。

### 2.6.4 逐步拆解（重点）

**① 防御检查**：`if (!capture->isStreaming()) return;` —— 摄像头未启动时 `stopCapture`/`setFormat` 无意义甚至出错，提前返回。

**② 置暂停标志**：`g_state.paused = true` —— 告诉采集线程"下个循环别再拉帧了"。采集线程在循环开头检查：

```cpp
while (g_state.running) {
    if (g_state.paused) {                    // ← 看到暂停标志
        g_state.pausedAck = true;            //   确认：我停了
        g_state.pauseCv.notify_one();        //   通知主线程
        std::unique_lock<std::mutex> lk(g_state.pauseMtx);
        g_state.pauseCv.wait(lk, [] { return !g_state.paused.load(); });  // 挂起等待
        continue;
    }
    g_state.pausedAck = false;
    ...
}
```

注意：`paused` 是 `std::atomic<bool>`——因为它是**跨线程共享**（主线程写、采集线程读），用 atomic 避免数据竞争。

**③ 等待采集线程确认**：`wait_until` —— 为什么必须等？
- 置 `paused=true` 只是"请求"，采集线程可能**正卡在 `getFrame()` 的 select 等待里**（最长 1s 超时）
- 不等确认就 `stopCapture()` → 采集线程可能正在 `putFrame()` 使用 mmap 缓冲，此时 `unmapBuffers()` 释放缓冲 → **use-after-free / 崩溃**
- `wait_until` 语义：条件满足立即返回；不满足则释放 `pauseMtx` 睡眠（最多 1.1s）；超时也返回（**兜底**，不能无限等）

**④ 安全停流**：`capture->stopCapture()` —— 此时采集线程已确认暂停（或超时兜底），**没人再用 mmap 缓冲**，可安全 `STREAMOFF` + `unmapBuffers`。

**⑤ 改分辨率**：`capture->setFormat(w, h, capture->getCurrentFormat())` —— 只改 `width/height`，**保持当前像素格式**。内部 `VIDIOC_S_FMT`（此时已 STREAMOFF 不会 EBUSY）→ **回读驱动实际接受的值**（驱动可能调整分辨率，必须写回 `m_width/m_height`）。

**⑥ 失败回退**：`if (ret < 0) capture->setFormat(640, 480, ...)` —— 请求分辨率驱动不支持时回退到**已知可靠的默认值**，保证系统继续工作而不是崩掉。

**⑦ 重启采集**：`capture->startCapture()` —— 内部重新执行 `REQBUFS → QUERYBUF → mmap → QBUF → STREAMON`，**按新分辨率重建缓冲池**。这就是"改分辨率必须重建缓冲"的落地。

**⑧⑨ 恢复采集线程**：`paused=false`（撤销标志）+ `notify_one()`（唤醒挂起的采集线程继续拉帧）。

### 2.6.5 完整时序图（双线程协作）

```
主线程 (onResolutionChanged)              采集线程
──────────────────────                  ─────────────────
 ① isStreaming() 检查
 ② paused = true        ──────────►     循环开头看到 paused
 ③ wait_until(1100ms)                   pausedAck = true
    等待...                             notify_one()  ← 被唤醒
                                         wait() 挂起等待恢复
 ④ stopCapture()                        （挂起中，不碰缓冲）
 ⑤ setFormat(w,h)                       
 ⑥ (失败则回退 640x480)
 ⑦ startCapture()                       
 ⑧ paused = false                       
 ⑨ notify_one()       ──────────►      wait 返回，继续拉帧
```

### 2.6.6 面试要点总结

| 步骤 | 解决什么问题 |
|------|-------------|
| ① 防御检查 | 避免无效操作 |
| ② paused=true | 请求采集线程让路（应用层协调） |
| ③ wait_until | 等采集线程确认，防 use-after-free |
| ④ stopCapture | STREAMOFF + 释放缓冲（驱动层） |
| ⑤ setFormat | S_FMT 改分辨率（必须停流后） |
| ⑥ 失败回退 | 不支持的格式回退 640x480 |
| ⑦ startCapture | 按新分辨率重建缓冲池 |
| ⑧⑨ 恢复 | 撤销暂停 + 唤醒采集线程 |

**面试一句话**：
> "onResolutionChanged 的核心是**双线程握手协议**：因为 V4L2 的 S_FMT 在 STREAMON 下返回 EBUSY、且改分辨率后 mmap 缓冲必须重建，所以先 `paused=true` 请求采集线程让路，用 `wait_until` 等它确认暂停（防采集线程还在用缓冲时被释放），再安全地 `stopCapture → setFormat → startCapture`，最后 `paused=false + notify_one` 唤醒采集线程继续拉帧。paused 是应用层协调标志（原子变量），stopCapture 是驱动层停流（ioctl），两者解决不同层面的问题，必须配合使用。"

## 2.7 wait_until 详解：原型、参数与使用

> 面试深挖：2.6 里那个"最多等 1.1s"是怎么实现的？这一节把 `std::condition_variable::wait_until` 的原型、参数、两个重载、返回值、易错点讲透。

### 2.7.1 项目里的实际使用

```cpp
// main.cpp:1210-1212（onResolutionChanged 里）
std::unique_lock<std::mutex> lk(g_state.pauseMtx);
g_state.pauseCv.wait_until(lk,
    std::chrono::steady_clock::now() + std::chrono::milliseconds(1100),
    [] { return g_state.pausedAck.load(); });
```

### 2.7.2 原型（`std::condition_variable::wait_until`）

```cpp
// 重载 1：无谓词版（不带条件判断）
template<class Clock, class Duration>
cv_status wait_until(std::unique_lock<std::mutex>& lock,
                     const std::chrono::time_point<Clock, Duration>& abs_time);

// 重载 2：带谓词版（推荐，本项目用的这个）
template<class Clock, class Duration, class Predicate>
bool wait_until(std::unique_lock<std::mutex>& lock,
                const std::chrono::time_point<Clock, Duration>& abs_time,
                Predicate pred);
```

### 2.7.3 参数逐个说明

| 参数 | 类型 | 作用 |
|------|------|------|
| `lock` | `std::unique_lock<std::mutex>&` | **必须持有**的锁（wait 期间会释放它，唤醒后重新持有）。为什么必须 unique_lock？因为 wait 需要"解锁+睡眠+重新加锁"，lock_guard 做不到 |
| `abs_time` | `time_point<Clock, Duration>` | **绝对时间点**（注意不是相对时长！），到这个时刻为止等待。本项目：`steady_clock::now() + 1100ms` |
| `pred` | `Predicate`（可调用对象） | 条件谓词，返回 bool。**只要 pred 为 false 就继续等，为 true 立即返回** |

### 2.7.4 两个重载的区别

**重载 1（无谓词）—— 裸等待，需自己循环**：

```cpp
cv_status st = cv.wait_until(lk, abs_time);
if (st == std::cv_status::timeout) {
    // 超时了
} else {
    // 被 notify 唤醒（注意：可能是"假唤醒"）
}
```

缺点：被唤醒后**不检查条件**，遇到"假唤醒"（spurious wakeup）会继续往下走——所以**必须手动循环检查条件**：

```cpp
while (!g_state.pausedAck.load()) {
    if (cv.wait_until(lk, abs_time) == std::cv_status::timeout)
        break;   // 超时，退出
}
```

**重载 2（带谓词）—— 内部自动循环（推荐）**：

```cpp
bool ok = cv.wait_until(lk, abs_time, [] { return g_state.pausedAck.load(); });
```

内部等价于：

```cpp
while (!pred()) {                        // 谓词不满足就一直等
    if (wait_until(lock, abs_time) == std::cv_status::timeout) {
        return pred();                   // 超时：再查一次谓词决定返回值
    }
}
return true;                             // 谓词满足
```

好处：
1. **自动处理假唤醒**——谓词不满足就继续等，不会误判
2. **超时返回"谓词最终结果"**——不是盲目的"超时=true"，而是告诉你"等到超时时条件到底满足没"
3. 语义清晰，一行搞定

### 2.7.5 返回值

| 重载 | 返回值 | 含义 |
|------|--------|------|
| 重载 1 | `cv_status::timeout` / `cv_status::no_timeout` | 单纯告诉你"是否超时" |
| 重载 2 | `bool` | **谓词的最终结果**：true=条件满足返回；false=超时且条件仍未满足 |

### 2.7.6 三个关键概念（面试必问）

**① 绝对时间 vs 相对时长（最易错点）**

`wait_until` 用**绝对时间点**，`wait_for` 用**相对时长**：

```cpp
// wait_until：绝对时间（本项目用法）
cv.wait_until(lk, steady_clock::now() + 1100ms, pred);

// wait_for：相对时长（等效写法）
cv.wait_for(lk, 1100ms, pred);
```

**为什么推荐 wait_until？** 如果用 `wait_for` 配合循环，每次重算"现在+1100ms"，会导致**总等待时间被拉长**（多次重算累计）。`wait_until` 的绝对时间点在循环里**固定不变**，保证"最多等 1.1s"的承诺严格成立。本项目用 `wait_until` 正是这个原因——它要严格保证"最多等 1.1s"（对应采集线程 getFrame 的 1s 超时兜底）。

**② 假唤醒（spurious wakeup）**

条件变量**可能被莫名唤醒**（无 notify、超时未到）。带谓词版自动规避——谓词不满足就继续等，所以**永远用带谓词版本**。

**③ 为什么等待时必须持锁？**

谓词 `pausedAck.load()` 访问的是共享状态，必须有锁保护（与其他写 `pausedAck` 的线程互斥）。wait 在检查谓词前**假设你已经持锁**，检查时持锁、睡眠时释放、唤醒后重新持锁再检查——这就是 unique_lock 的用武之地。

### 2.7.7 完整正确用法模板

```cpp
// ① 持锁
std::unique_lock<std::mutex> lk(mtx);
// ② 带谓词等待（绝对时间，自动处理假唤醒）
bool done = cv.wait_until(lk,
    std::chrono::steady_clock::now() + std::chrono::milliseconds(1100),
    [] { return g_state.pausedAck.load(); });
// ③ 判断结果
if (done) {
    // 条件满足（采集线程已确认暂停）
} else {
    // 超时兜底（即使没确认，也不能无限等）
}
```

### 2.7.8 面试一句话总结

> "`wait_until` 是条件变量的带超时等待函数，原型接收三个参数：已持有的 `unique_lock`（wait 期间释放、唤醒后重新持有）、**绝对时间点**（不是相对时长，保证循环里总等待时间固定）、条件谓词（返回 bool）。带谓词版本内部自动循环处理假唤醒，超时返回谓词的最终结果。项目里用它实现'最多等 1.1s 采集线程确认暂停'——绝对时间确保严格超时，谓词确保只被'真的暂停了'唤醒，unique_lock 确保睡眠期间不占锁（否则 notify 方会死锁）。"

---

# 第三部分 核心函数深度解析

> 这一部分是 main.cpp 里三个**容易被忽略但面试价值高**的函数/机制：两个进程资源读取器（`readSelfCpuJiffies` / `readSelfRssKB`）和一个性能插桩体系（`[PERF]`）。它们共同构成"性能排查地基"。

## 3.1 readSelfCpuJiffies() —— 进程 CPU 时间读取（`main.cpp:116-152`）

### 作用

**一句话**：读取当前进程（smartcam 自己）从启动到此刻累计消耗的 **CPU 时间**（用户态 utime + 内核态 stime，单位 jiffies），供 `[PERF]` 插桩计算"进程整体 CPU 占用率"。

**为什么需要它**：程序有 5~6 个线程，CPU 被所有线程共享消耗。如果只统计 main 线程视角，CPU% 会严重失真。`/proc/self/stat` 记录的是**整个进程所有线程**的累计 CPU 时间，差值除以时间间隔才是准的全进程 CPU%。

### 完整代码

```cpp
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
    int skipped = 0; char state = 0; unsigned long long tmp = 0;
    char* tok = p + 1;
    while (skipped < 12 && tok) {
        if (skipped == 0) {                    // 第 1 个 token 是 state（字符）
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
    utime = tmp;                               // skipped==12 时 tmp 即 utime
    while (*tok == ' ') ++tok;
    sscanf(tok, "%llu", &stime);               // 再读一个为 stime
    return utime + stime;
}
```

### 逐段拆解

**① 读文件（118-125 行）**
`/proc/self/stat` 是 Linux 提供的进程信息文件，**一行**、空格分隔，格式：
```
1234 (smartcam) S 1 1234 1234 0 -1 4194560 168 ... 9 5 0 0 20 0 ...
│    │        │
│    └── comm（进程名，可能含空格和括号！）
└── pid
```
**经典坑**：第 2 字段 `comm` 用 `(...)` 括起来且可能含右括号，所以**不能**从第 1 个 `)` 开始解析，必须用 `strrchr`（`r`=reverse）找**最后一个** `)`——最后一个 `)` 一定是 comm 的结束符。

**② 字段定位（127-130 行）**
`/proc/self/stat` 字段序号从 3 开始（`man proc`）。CPU 时间相关：
- **第 14 字段 `utime`**：用户态 CPU 时间（jiffies）
- **第 15 字段 `stime`**：内核态 CPU 时间（jiffies）

`)` 之后紧跟 `state`（第 3 字段），所以从 state 数：`state(3) ppid(4) pgrp(5) session(6) tty(7) tpgid(8) flags(9) minflt(10) cminflt(11) majflt(12) cmajflt(13) utime(14) stime(15)`——**跳过 12 个 token**，第 12 个是 utime，再往后一个是 stime。

**③ 逐 token 解析（131-150 行）**
- 首 token `state` 是**字符**（如 `S`），特殊处理：`state = *tok`
- 后续 11 个 token 都是数字，`sscanf(tok, "%llu", &tmp)` 读取，`strchr(tok,' ')+1` 跳空格
- 循环结束时 `tmp` = utime；退出后**再读一个** = stime
- 返回 `utime + stime`

### 调用方（`main.cpp:1185-1195`）

```cpp
static uint64_t lastSelfCpu = 0;
uint64_t selfCpu = readSelfCpuJiffies();
if (lastSelfCpu > 0) {
    double cpuPctAll = (selfCpu - lastSelfCpu) / dt / 100.0 * 100.0;
    //                 Δjiffies  / 秒 / 100Hz × 100
    LOG_INF("[PERF] ... cpu=%.0f%% ...", cpuPctAll, ...);
}
lastSelfCpu = selfCpu;   // 第一次 lastSelfCpu==0，只记录不输出
```

`lastSelfCpu` 是 `static`，首次调用为 0 走 else 分支只记录，避免假数据。jiffies 单位 100Hz = 1 jiffy = 10ms。

### 面试追问

**Q1：为什么不用 `getrusage()` 而要读 `/proc/self/stat`？**
**A**：两者都能拿全进程 CPU 时间。选 `/proc` 是因为嵌入式 Linux 上最通用、无依赖、可读性好；且 `/proc/self/stat` 一行能同时取到 utime/stime，配合 `strrchr` 解析也顺带展示了"手写 proc 解析"的能力。`getrusage` 的 `RUSAGE_SELF` 也是选项，但 `/proc` 更直观。

**Q2：为什么返回累计值而不是瞬时值？**
**A**：配合调用方做**差值**——每 5s 取一次，`(本次 - 上次) / 间隔` 得到这 5s 的平均 CPU%，避免瞬时采样的抖动。这是"滑窗差值法"，也是性能分析看趋势不看瞬时的原则。

## 3.2 readSelfRssKB() —— 进程内存占用读取（`main.cpp:154-168`）

### 作用

**一句话**：读取 `/proc/self/status` 文件中的 `VmRSS:` 字段（进程当前常驻物理内存，单位 KB），供 `[PERF]` 插桩展示 `rss=xxxKB`。

### 完整代码

```cpp
/** @brief 读取 /proc/self/status 的 VmRSS（KB） */
static long readSelfRssKB() {
    FILE* fp = fopen("/proc/self/status", "r");
    if (!fp) return 0;
    char line[256];
    long rss = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {  // 行首匹配 "VmRSS:"
            sscanf(line + 6, "%ld", &rss);      // 跳过 "VmRSS:" 后解析数字
            break;
        }
    }
    fclose(fp);
    return rss;
}
```

### 为什么是 RSS 而非 VSZ

| 指标 | 含义 | 为什么不用 |
|------|------|-----------|
| **RSS** | 常驻物理内存 | ✅ 最接近"这个进程吃了多少内存" |
| VSZ | 虚拟地址空间大小 | ❌ 含 mmap 文件、未触碰的堆，虚高（可能几个 GB） |
| PSS | 按共享比例分摊 | 更准但 `/proc/self/status` 非主字段，解释成本高 |

对嵌入式（512MB 板子，可用 <200MB），**RSS 是判断"会不会 OOM"的最直接指标**。

### 与 readSelfCpuJiffies() 的对比

| 维度 | readSelfCpuJiffies() | readSelfRssKB() |
|------|----------------------|-----------------|
| 源文件 | `/proc/self/stat`（**一行**，50+ 字段） | `/proc/self/status`（**多行**，`key: value`） |
| 解析方式 | 手写 token 遍历，跳 12 字段 | **逐行 `fgets`** + `strncmp` 前缀匹配 |
| 坑点 | `comm` 含括号 → `strrchr` 找最后 `)` | 无格式坑 |

**为什么两种解析策略？** `/proc/self/stat` 是扁平一行、全靠位置 → 只能按字段序号跳；`/proc/self/status` 是每行 `Key: Value` → 前缀匹配更直观、对字段顺序不敏感。**针对文件结构选解析策略**。

### 面试追问

**Q1：`strncmp(line, "VmRSS:", 6)` 为什么用 `strncmp` 不用 `strcmp`？**
**A**：`line` 是整行 `"VmRSS:\t1234\n"`，`strcmp` 会比较到结尾必然不等；`strncmp` 只比较前 6 个字符的前缀，恰好匹配 key。这是"前缀匹配"的经典用法。

**Q2：找不到 `VmRSS:` 行会怎样？**
**A**：返回 0（`rss` 初值），perf 日志显示 0，不崩溃。理论正常系统必有此字段，属防御性兜底。

## 3.3 [PERF] 性能插桩机制（`main.cpp:83-106` + `1155-1197`）

### 作用

**一句话**：每 5s 打印一行带 `[PERF]` 前缀的日志，量化"帧搬运字节数 / 处理帧率 / CPU 占用 / 内存占用"四个指标，用于**改造前后 A/B 对比**（尤其帧池零拷贝优化）和**瓶颈定位**。

这正是支撑文档里"帧池零拷贝把拷贝降 95% 但帧率没变 → 最终定位摄像头硬件 10fps"那场排查的核心工具。

### 架构：生产者-采样器模式

```
┌─ 生产端（多个线程，各自原子累加）─────────────┐
│  采集线程  → g_perf.copyBytes += fb.length     │  [PERF] ①
│  处理线程  → g_perf.copyBytes += localFrame    │  [PERF] ②
│  displayTimer → g_perf.copyBytes += raw.size   │  [PERF] ③ (原始帧)
│  displayTimer → g_perf.pixBytes += w*h*3       │  [PERF] ⑤ (上屏RGB)
│  采集线程  → g_perf.frames++                   │  帧数入口
└──────────────────────┬────────────────────────┘
                       │ 每帧累加（无锁原子）
┌──────────────────────▼────────────────────────┐
│  采样端：perfTimer（Qt 主线程，每 5s 一次）      │
│  读累计值 → 减上次快照 → 除以时间间隔 → 打印     │
└───────────────────────────────────────────────┘
```

**为什么用 `std::atomic`**：采集/处理/GUI 三个线程并发累加同一计数器，原子保证无锁线程安全，不阻塞任何取帧/推流路径。

### 数据结构（`PerfStats`，`main.cpp:94-106`）

```cpp
struct PerfStats {
    // ── 累加区：多线程写，原子 ──
    std::atomic<uint64_t> copyBytes;   // 帧数据搬运字节（①②③）
    std::atomic<uint64_t> pixBytes;    // 上屏 RGB 拷贝（⑤，单独算）
    std::atomic<uint64_t> frames;      // 处理帧数
    std::atomic<uint64_t> cpuJiffies;  // CPU jiffies（后改用 /proc/self）
    // ── 快照区：仅主线程 PERF 定时器读写（非原子，无竞争）──
    uint64_t snapBytes, snapPix, snapFrames, snapCpu;
    double   snapTime;
};
static PerfStats g_perf;
```

**设计要点**：**累加区原子、快照区非原子**——快照只在主线程采样器读写，无竞争；累加区被多线程写，必须原子。这是"按访问模式决定同步策略"的体现。

### 各统计点的精确含义（理解关键）

| 标记 | 位置 | 统计什么 | 目的 |
|------|------|---------|------|
| ① | 采集线程 `:952` | `fb.length`（原始帧） | 采集 → `g_state` 的深拷贝 |
| ② | 处理线程 `:1009` | `localFrame.size()` | 处理线程取帧的深拷贝 |
| ③④ | displayTimer `:1123` | `raw.size()`（原始帧） | **③④ 已消除**——帧池改造后不再有 setFrame assign + QImage.copy() |
| ⑤ | displayTimer `:1126` | `slot->width*height*3`（RGB24） | QPixmap::fromImage 上屏必需拷贝，**单独计入 pixBytes** |
| frames | 采集线程 `:953` | 每帧 +1 | 处理帧率入口 |

**关键洞察**：`copyBytes` 和 `pixBytes` **分开统计**——帧池零拷贝的目标是消掉 ③④ 两次 RGB 深拷贝，改造后 `copyBytes` 只含原始帧（JPEG ~0.1MB），`pixBytes` 是物理上屏必需拷贝。**把"可优化的拷贝"和"物理必需的拷贝"分开，才能看到优化到底省了什么**。

### 采样与计算（perfTimer，`:1160-1196`）

```cpp
double dt = now - g_perf.snapTime;                     // 距上次采样秒数
if (dt <= 0.0) { g_perf.snapTime = now; return; }      // 防御：首次

double copyMB = (bytes - g_perf.snapBytes) / dt / 1e6;    // MB/s
double pixMB  = (pix   - g_perf.snapPix)   / dt / 1e6;
double fps    = (frames - g_perf.snapFrames) / dt;         // 帧率
long   rssKB  = readSelfRssKB();
```

**滑窗差值法**：每 5s 算一次"这 5 秒的平均值"，平滑抖动；代价是最多滞后 5s 反映变化。性能分析要**看趋势不看瞬时**。

### CPU% 的两个口径（一个演进点）

代码里其实有**两套** CPU 统计：
1. `g_perf.cpuJiffies`（`:1164`）：有定义，但...
2. **实际打印用 `readSelfCpuJiffies()`**（`:1185-1195`）：

```cpp
// 采集线程记录 CPU 用 main 线程视角不准，改用 /proc/self 全进程统计：
static uint64_t lastSelfCpu = 0;
uint64_t selfCpu = readSelfCpuJiffies();
if (lastSelfCpu > 0) {
    double cpuPctAll = (selfCpu - lastSelfCpu) / dt / 100.0 * 100.0;  // 全进程
    LOG_INF("[PERF] copy=%.1fMB/s (+pix %.1f) frames=%.1ffps cpu=%.0f%% rss=%ldKB",
            copyMB, pixMB, fps, cpuPctAll, rssKB);
} else {
    LOG_INF("[PERF] copy=%.1fMB/s (+pix %.1f) frames=%.1ffps cpu=%.0f%% rss=%ldKB",
            copyMB, pixMB, fps, cpuPct, rssKB);   // 首次退化路径
}
lastSelfCpu = selfCpu;
```

**演进原因**（注释明示）：早期想在采集线程手动累加 jiffies，但**单线程视角不准**——CPU 被所有线程共享。改用 `/proc/self` 一次读全进程 utime+stime，差值即全进程 CPU%。`g_perf.cpuJiffies` 成遗留字段（首次采样退化路径仍用）。

### 输出示例与解读

```
[PERF] copy=0.5MB/s (+pix 3.3) frames=10.0fps cpu=97% rss=8.2MB
```

| 字段 | 含义 | 排查价值 |
|------|------|---------|
| `copy=0.5MB/s` | 帧搬运带宽 | 帧池优化后 10.0→0.5（-95%）看这里 |
| `(+pix 3.3)` | 上屏 RGB 拷贝带宽 | 物理必需拷贝，单独标注 |
| `frames=10.0fps` | 处理帧率 | 与目标 30fps 对比 |
| `cpu=97%` | 全进程 CPU 占用 | 判断瓶颈在不在 CPU |
| `rss=8.2MB` | 常驻内存 | 内存预算验证 |

### 这套机制如何支撑那场"瓶颈排查"（面试重点）

文档 3.2 场景 D 的完整推理链，每一步都有 `[PERF]` 数据支撑：

1. `copy=10.0MB/s` → 判断拷贝是最大优化项 → 做帧池零拷贝
2. 帧池后 `copy=0.5MB/s`（-95%）**但 frames 仍 10fps、cpu 仍 99%** → 排除拷贝是瓶颈
3. 插桩显示解码 15.6ms、渲染 9ms、处理线程已跳过 → 排除应用层任务
4. **去掉渲染后 cpu=67%（有 33% 空闲）但帧率仍 10** → CPU 有空闲却提不上去 → 瓶颈在供给端
5. `v4l2-ctl` 直测 → 摄像头硬件实际输出 10fps（能力列表声称 30fps）

**核心方法论**：`[PERF]` 提供**可量化、可对照**的证据——每次优化跑同场景对比输出行，数值变没变一目了然，避免靠直觉猜瓶颈。

### 面试追问

**Q1：为什么 copyBytes 和 pixBytes 分开统计？**
**A**：帧池零拷贝的目标是消掉显示链路的 RGB 深拷贝（旧 setFrame assign + QImage.copy() 两次）。分开统计后，`copyBytes` 只剩物理必需的原始帧搬运（JPEG），`pixBytes` 是上屏必需的 QPixmap 转换——改造效果直接看 copyBytes 从 10.0 掉到 0.5 就是证据。若不分开，无法区分"优化掉的"和"省不掉的"。

**Q2：为什么用滑窗差值而不是每秒瞬时采样？**
**A**：瞬时采样受两帧之间的相位影响大、抖动明显；5s 滑窗给出平滑的平均值，适合看趋势。代价是反馈滞后，对"实时调参"不够敏感——但这正是性能分析要的：稳定性优先于实时性。

**Q3：CPU% 为什么不用 g_perf.cpuJiffies 而是另读 /proc/self？**
**A**：这是代码演进留下的"历史注脚"。早期想在工作线程里手动累加 CPU 时间，但单线程视角无法代表全进程（smartcam 有 5 个线程，CPU 被共享消耗）。`/proc/self/stat` 一次读全进程所有线程的累计 CPU 时间，差值才是准的全进程 CPU%。能主动讲出这段演进，说明你真的理解为什么"线程视角的 CPU 统计会失真"。

**Q4：[PERF] 对实际优化决策有什么帮助？**
**A**：它让"优化是否有用"变成可验证的事实。帧池零拷贝做完，跑同一场景，copy 从 10.0 掉到 0.5，但 frames 纹丝不动——这个"优化了却没效果"的反直觉结果，正是靠 [PERF] 暴露出来的，从而推动继续排查（最终定位硬件 10fps）。**没有插桩，就只能靠猜**。

---

# 第四部分 面试速查表

| 主题 | 一句话答案 |
|------|-----------|
| main.cpp 定位 | 胶水层：线程编排 + 全局状态 + 回调注入，不实现具体功能 |
| 几线程 | Qt 主线程 + 采集 + 处理 + RTSP + 控制（+ HTTP 客户端动态线程） |
| 采集线程为什么只做轻活 | 慢客户端/磁盘 IO 不阻塞取帧，防 V4L2 4 缓冲耗尽 |
| 为什么 atomic 不用 mutex | 单标志位读多写少，atomic 无锁开销；复合读改写才用 mutex |
| 配置三级优先级 | 命令行 > 配置文件 > 硬编码默认值 |
| 命令行用什么解析 | QCommandLineParser + QCommandLineOption，process(app) 触发解析 |
| 为什么端口默认值留空 | 哨兵值技巧：让 isSet() 成为唯一判断，配置文件端口才不会被遮蔽 |
| 怎么给短选项 -d | QCommandLineOption({QStringList() << "d" << "device", ...}) 传 QStringList |
| --help/--version 哪来的 | addHelpOption()/addVersionOption() 内置，process 时自动打印退出 |
| 回调注入是什么 | std::function 依赖倒置，GUI 不依赖具体模块，可独立测试 |
| 为什么切分辨率要暂停 | S_FMT 在 STREAMON 下 EBUSY + mmap 缓冲使用中，需双向握手 |
| readSelfCpuJiffies 读什么 | /proc/self/stat 的 utime+stime（全进程 CPU，jiffies） |
| 为什么 strrchr 找最后 `)` | comm 字段可能含括号，最后一个 `)` 才是 comm 结束符 |
| readSelfRssKB 读什么 | /proc/self/status 的 VmRSS（进程常驻物理内存 KB） |
| 为什么选 RSS 不选 VSZ | RSS 是真实物理内存，VSZ 虚高，嵌入式判断 OOM 看 RSS |
| 两种 proc 解析为何不同 | stat 扁平一行按位置跳字段；status 按行前缀匹配 key:value |
| [PERF] 是什么 | 每 5s 打印 copy/pix/frames/cpu/rss 五维，A/B 对比优化效果 |
| 为什么 copy/pix 分开 | 区分"可优化的拷贝"与"物理必需的拷贝"，看优化到底省了什么 |
| CPU% 为什么用 /proc/self | 单线程视角失真（多线程共享 CPU），全进程差值才准 |
| 滑窗差值法 | 每 5s 取累计值差/时间间隔 → 平均值，看趋势不看瞬时 |
| [PERF] 最大价值 | 让"优化是否有用"可量化对照，避免靠直觉猜瓶颈 |
| 那场排查的结论 | 帧池降拷贝 95% 但帧率不变 → 逐步排除 → v4l2-ctl 直测确认硬件 10fps |

---

*本文档基于 SmartCam-Linux-on-imx6ull 项目源码（commit: main 分支），聚焦 `src/main.cpp` 的全局状态、线程编排、配置解析与三个核心函数（readSelfCpuJiffies / readSelfRssKB / [PERF] 插桩）。配合阅读：`docs/learn/面试复习-camera模块.md`（采集/处理线程下游）、`docs/learn/面试复习-display模块.md`（displayTimer 上游）、`docs/debug-summary.md` #27（瓶颈排查实录）。*
