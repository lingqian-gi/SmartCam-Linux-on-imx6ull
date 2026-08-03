# src/display/ 模块面试复习指南（显示与交互）

> 定位：以「技术面试官 + 应聘者」双视角，系统拆解 `src/display/` 模块的代码实现。
> 阅读本文档前建议先自行通读 `src/display/` 全部源码（`gui.cpp` / `gallery.cpp` / `video_player.cpp` 及头文件），并对照 `src/main.cpp` 中 displayTimer 与回调注入部分（约 880~1130 行）。
>
> 组织方式：**模块概览（脑图）→ 分块详解（代码 + 坑点 + 面试追问）→ 综合思考（耦合/重构/设计模式）**。

---

## 目录

1. [第一部分 模块整体概览](#第一部分-模块整体概览)
   - 1.1 脑图式结构
   - 1.2 职责边界
   - 1.3 输入 / 输出
   - 1.4 依赖的外部库与平台
   - 1.5 核心类 / 函数列表与调用关系
   - 1.6 Qt 线程模型与两个定时器的关系
2. [第二部分 分块代码详解（含面试追问）](#第二部分-分块代码详解含面试追问)
   - 2.1 块一：帧渲染管线（帧池零拷贝路径 + 旧路径兜底）
   - 2.2 块二：帧率控制（displayTimer / m_refreshTimer 双定时器与双 FPS）
   - 2.3 块三：回调注入（松耦合的关键）
   - 2.4 块四：页面导航（QStackedWidget 三层嵌套）
   - 2.5 块五：触摸交互（eventFilter / 滑动翻页 / 滑块防抖）
   - 2.6 块六：相册与缩略图（scale_denom 缩放解码）
   - 2.7 块七：OSD 叠加与 QPainter
   - 2.8 块八：VideoPlayer（轻量 AVI 播放器）
   - 2.9 块九：Mock 模式
3. [第三部分 综合思考](#第三部分-综合思考)
   - 3.1 与 camera / network / storage 的数据流耦合
   - 3.2 需求变更下的重构推演（720p、60fps、换渲染后端）
   - 3.3 设计模式评估与改进建议
   - 3.4 面试「一句话总结」
4. [附：速查表](#附display-模块速查表面试前-5-分钟过一遍)
5. [补充：面试高频追问深度展开](#补充面试高频追问深度展开)

---

# 第一部分 模块整体概览

## 1.1 脑图式结构

```
src/display/ 显示与交互模块
│
├── CameraGUI（主界面）                  gui.h / gui.cpp (~1140 行)
│   ├── 帧渲染：setFrameShared() 零拷贝路径（m_heldSlot + QImage 浅引用）
│   │            + frameToQImage() 兜底路径（Mock/无槽时）+ refreshFrame() 主渲染
│   │            （setFrame() 已废弃——定义仍在但无任何调用点，为死代码）
│   ├── 帧池交互：extern FramePool* g_rgbPool；setFrameShared 持有槽引用，refreshFrame 浅引用上屏
│   ├── 帧率控制：m_refreshTimer (GUI 内) + displayTimer (main 注入) / 双 FPS
│   ├── 回调注入：onCaptureRequest / onRecordToggle / onResolutionChanged
│   │            onFormatChanged / onStoragePathChanged / onCameraControlChanged
│   │            onFramerateChanged
│   ├── 设置弹窗：亮度/对比度/白平衡/曝光/帧率滑块 + Reset Defaults
│   ├── 页面导航：QStackedWidget [0]实时预览 / [1]相册
│   ├── 状态栏：Cap FPS / Disp FPS / LIVE|IDLE / Clients / REC
│   └── Mock 模式：enterMockMode() 8 色彩条 + 滚动
│
├── PhotoGallery（相册）                 gallery.h / gallery.cpp (~1015 行)
│   ├── 缩略图网格：3 列 + 按日期分组 + 滚动区域
│   ├── 全屏查看：照片 / 视频自动切换（QStackedWidget 媒体栈）
│   ├── 多选模式：Select / Select All / Deselect All / Delete(N)
│   ├── 缩略图生成：createThumbnail / createVideoThumbnail（scale_denom 缩放解码）
│   ├── 触摸滑动翻页：eventFilter + 60px 阈值
│   └── 存储空间状态栏：已用/总空间 + <5% 红色警报
│
└── VideoPlayer（轻量 AVI 播放器）       video_player.h / video_player.cpp (~500 行)
    ├── AVI 解析：RIFF → hdrl/avih → movi → idx1 索引表
    ├── 播放：QTimer(1000/fps) 逐帧 seek + fread + libjpeg 解码
    ├── 控制：播放/暂停/进度条拖拽 seek / 帧号与剩余时间
    └── 无 ffmpeg/gstreamer/vlc，纯手写 + 复用 libjpeg-turbo
```

## 1.2 职责边界

| 本模块负责 | 本模块不负责 |
|-----------|-------------|
| 视频帧渲染上屏（解码/转换/缩放/显示） | 视频采集（V4L2 在 `src/camera/`） |
| 触摸/按钮交互与手势识别 | 网络传输（HTTP/RTSP/TCP 在 `src/network/`） |
| 相册浏览、全屏查看、删除确认 | 文件持久化与磁盘管理（`src/storage/`） |
| 设置面板与相机参数 UI | 帧数据生产（`g_state` 由采集线程填充） |
| 轻量 AVI 播放（解码渲染） | 线程编排与全局状态（`src/main.cpp`） |
| Mock 模式（无硬件 UI 调试） | V4L2 控制 ioctl 本身（经回调间接驱动） |

**一句话**：display 模块是"消费帧 + 呈现交互"的需求侧，通过回调把"业务动作"委托给 main.cpp，自己只关心两件事——**把帧画对、把触摸翻译对**。它不知道 camera 怎么采帧，camera 也不知道屏幕怎么渲染。

## 1.3 输入 / 输出

- **输入**：
  - 帧数据：`setFrameShared(FrameSlot*)` 从帧池拉取共享槽（RGB24，零拷贝）；兜底路径 `frameToQImage` 处理多格式（RGB24/RGB565/YUYV/MJPEG，Mock/无槽时用）。`setFrame(data, len, w, h, fmt)` 已废弃（无调用点，死代码）
  - 状态更新：`setFPS` / `setDisplayFPS` / `setClientCount` / `setRecordingStatus` / `setStreamingStatus`
  - 相机控制范围：`setBrightnessRange` / `setContrastRange` / `setWhiteBalanceRange` / `setExposureRange` / `setFramerateRange`
  - 存储绑定：`setGalleryStorage(StorageManager*)`
- **输出**：
  - 业务回调：拍照、录像 toggle、分辨率/格式/存储路径变更、相机控制（cid,value）、帧率变更
  - Qt 信号：`captureClicked` / `recordToggled` / `resolutionChanged` / `formatChanged` / `backToLive`
  - 用户可见：预览画面、状态栏、相册、设置面板

## 1.4 依赖的外部库与平台

| 依赖 | 用途 | 说明 |
|------|------|------|
| Qt5 Widgets（`QWidget`/`QLabel`/`QTimer`/`QPainter`） | GUI 框架 | CMake `AUTOMOC` 处理 `Q_OBJECT`；linuxfb 后端 |
| `linuxfb` 平台插件（`QT_QPA_PLATFORM`） | 帧缓冲渲染 | 内部 mmap `/dev/fb0`，内置 evdev 触摸 |
| `libjpeg-turbo`（`jpeglib.h`） | MJPEG 解码 / 缩略图 | `HAS_LIBJPEG` 条件编译；自定义错误处理器防坏帧崩溃 |
| `arm_neon.h`（`__ARM_NEON`） | YUYV→RGB 加速 | 仅 ARM 交叉编译启用，x86 退化标量 |
| `VideoProcessor`（`src/camera/processor.h`） | YUYV→RGB24 颜色转换 / JPEG→RGB 解码 | `gui.cpp` include 复用，避免维护第二份转换代码；含 NEON 加速与 `decodeJPEGtoRGB` |
| `FramePool` / `FrameSlot`（`include/common/frame_pool.h`） | 帧池零拷贝显示链路 | `extern FramePool* g_rgbPool`；`setFrameShared` 持有槽引用，`refreshFrame` QImage 浅引用 |
| StorageManager（`src/storage/`） | 相册数据源 | `listPhotos` / `listVideos` / `deletePhoto` / `extractAviThumbnail` |
| 标准 C++17 | 容器/函数 | `std::function` 回调、`std::vector` |

> 面试点：display 模块**对 camera/network 的控制完全靠回调**（依赖倒置），但对 StorageManager 直接持有指针（相册需要它列文件）；此外 `gui.cpp` include 了 `camera/processor.h` 复用颜色转换（`VideoProcessor::yuyvToRgb24`）。**注意**：历史上 `gui.h` 曾内联一份 `yuyv_to_rgb24`/`yuyv_to_rgb565`，与 `processor.cpp` 重复，后已删除、统一到 `VideoProcessor`（详见 3.1）。

## 1.5 核心类 / 函数列表与调用关系

```
main.cpp（Qt 主线程）
  │
  ├─ CameraGUI gui;                        ← 栈对象，生命周期 = 整个 app
  │    gui.setGalleryStorage(&storage)     ← 绑定相册数据源
  │    gui.onStoragePathChanged(...)       ← 注入回调（业务逻辑全在 lambda 里）
  │    gui.onCameraControlChanged(...)     ← V4L2 cid/value 透传
  │    gui.onFramerateChanged(...)         ← 停流→S_PARM→重启 + 更新 displayTimer
  │    gui.onResolutionChanged(...)        ← 暂停→停流→setFormat→重启
  │    gui.onFormatChanged(...)
  │    gui.onCaptureRequest(...)           ← 存 JPEG（MJPEG 直存 / YUYV 先编码）
  │    gui.onRecordToggle(...)             ← 校验 MJPEG 才允许录像
  │
  ├─ displayTimer (QTimer 33ms)            ← main 侧：借槽→解码入槽→publish→share→setFrameShared + FPS 更新
  │
  └─ gui.show() → app.exec()               ← 进入 Qt 事件循环
        │
        └─ CameraGUI::m_refreshTimer (QTimer 33ms)   ← GUI 侧：refreshFrame → QImage 浅引用 → setPixmap
```

**两个定时器串联的完整链路**（这是理解显示线程模型的关键，见 1.6）：

```
displayTimer.timeout (main, 锁内)         m_refreshTimer.timeout (GUI 线程)
  g_state → 借rgb池槽 → 解码/转换入槽   ───►   refreshFrame
  → publish → share → setFrameShared           → m_heldSlot 存在: QImage 浅引用（零拷贝）
  （解码在 displayTimer 内，GUI 主线程）        → 否则: frameToQImage（旧路径兜底）
                                                → setPixmap → paintEvent → fb0
```

## 1.6 Qt 线程模型与两个定时器的关系

**全局只有 Qt 主线程一个 GUI 线程**（采集线程、处理线程、控制线程、RTSP 线程是 `std::thread`，不碰 Qt 对象）。所有控件操作必须在 GUI 线程，跨线程访问 Qt 对象是未定义行为。

项目里有**两个 33ms 定时器**，职责不同：

| 定时器 | 所在 | 回调 | 干的事 |
|--------|------|------|--------|
| `displayTimer` | `main.cpp` | lambda | 借 rgb 池槽 → 短锁拷贝 raw → **解码/转换入槽**（`decodeJPEGtoRGB`/`yuyvToRgb24`）→ `publish` → `share` → `setFrameShared` + 更新 FPS/客户端数 |
| `m_refreshTimer` | `gui.cpp` | `refreshFrame` | 若持有 `m_heldSlot` 用 QImage 浅引用（零拷贝），否则回退 `frameToQImage` → `setPixmap` |

> **注意**：帧池改造后，**解码（JPEG→RGB）发生在 `displayTimer` 的 timeout 内**（即 GUI 主线程，`main.cpp` 的 displayTimer 回调中），不再在 `m_refreshTimer` 的 `frameToQImage` 里。两个定时器仍在 GUI 主线程顺序执行，但 `m_refreshTimer` 变成了纯"浅引用上屏"，几乎不耗时。

**为什么拆成两个而不是一个？**
1. `displayTimer` 要访问 `g_state`（main.cpp 的全局）和 `g_rgbPool`（帧池），属于"模块边界的数据搬运 + 解码"；`m_refreshTimer` 只碰 GUI 内部状态，属于"渲染"。
2. 分离让 `CameraGUI` 类可以**独立测试**（PC Mock 模式只有 `m_refreshTimer` 在跑，不依赖 `g_state`）。
3. 帧池改造后，解码集中在 `displayTimer`，`m_refreshTimer` 退化为主持"浅引用上屏"；两者仍在 GUI 主线程顺序执行。
4. 代价是两个定时器**相位不同步**，帧会多约 0~33ms 的随机延迟——这是"解耦 vs 延迟"的权衡，面试能主动指出说明理解深。

【面试官追问】"这两个定时器都在 GUI 线程，会不会互相抢时间导致帧率减半？"

> 【理想应答】不会减半。两者都是 33ms 独立触发，Qt 事件循环按到期先后逐个执行 timeout 槽，每个槽的执行时间远小于 33ms（拷贝 ~1ms、解码 ~10-25ms），所以平均刷新率约 30fps。真正的风险是**解码耗时接近 33ms 时**：如果某帧解码超时，事件循环会积压，后续 timeout 被推迟，表现为"偶发跳帧"。这就是"解码应该移出 GUI 线程"的动机来源。

【面试官追问】"为什么跨线程不能用信号直接驱动 QLabel？"

> 【理想应答】Qt 信号跨线程默认用 `Qt::AutoConnection`，会依据"发射线程 ≠ 接收对象所在线程"自动转成 `QueuedConnection`，把调用包装成事件投递到 GUI 线程队列——本质是安全的，但有一个关键前提：**事件是否堆积取决于"生产速率 vs 消费速率"**。跨线程事件投递就是一个生产者-消费者队列（类似 M/M/1 排队模型，平均延迟 \(W = 1/(\mu - \lambda)\)），只有**采集帧率（λ）超过 GUI 线程的处理能力（μ，受单帧解码耗时约束）**时，队列才会无限堆积、延迟不断增长；**慢速采集（λ < μ）下队列是稳定的，不会堆积**。真正触发堆积的是"快速采集 + 慢显示"：例如采集 60fps、GUI 解码一帧 25ms（μ≈40fps），λ > μ，每帧一次事件投递就越积越多。所以本项目刻意不用"采集线程 → 每帧 emit 信号"，而是用定时器**拉模式**主动取最新帧：无论采集多快，每次定时器触发只取"最新"一帧、中间帧被覆盖丢弃，**延迟有界（最多一个周期）**，从根源上规避了 λ > μ 时的无限堆积；即使慢速采集下推模式本来也不会堆积，拉模式仍带来**节拍固定**（显示帧率钉在定时器周期，不受采集波动影响、无抖动）和**省去每帧元对象序列化/事件循环调度开销**的收益。拉模式 = 主动轮询，推模式 = 事件驱动，本项目选前者保实时性。

---

# 第二部分 分块代码详解（含面试追问）

## 2.1 块一：帧渲染管线（帧池零拷贝路径 + 旧路径兜底）

### 代码讲解

**代码意图**：把共享状态里的一帧安全地变成屏幕上的像素，核心是**帧池零拷贝**。

**帧池路径（当前真实相机模式主路径）**——`setFrameShared`（`gui.cpp:489`），零拷贝持有共享槽：

```cpp
void CameraGUI::setFrameShared(FrameSlot* slot) {
    if (!slot) return;
    // 1. 释放上一帧持有的槽引用（若有）
    if (m_heldSlot) {
        if (g_rgbPool) g_rgbPool->release(m_heldSlot);
        m_heldSlot = nullptr;
    }
    // 2. 持有新帧槽引用（slot 的 refs 已由 share() +1，GUI 接管）
    m_heldSlot = slot;
    // 3. m_currentFrame 直接指向共享数据（零拷贝）
    m_currentFrame.data   = slot->data.data();
    m_currentFrame.length = (int)slot->data.size();
    m_currentFrame.width  = slot->width;
    m_currentFrame.height = slot->height;
    m_currentFrame.format = PixelFormat::FMT_RGB24;   // 显示槽固定 RGB24
    m_currentFrame.index++;
    m_mockMode = false;
}
```

`refreshFrame`（`gui.cpp:276`）中，持有槽时用 **QImage 浅引用（不 `.copy()`）**：

```cpp
QImage img;
if (m_heldSlot) {
    // ---- 零拷贝路径：QImage 浅引用共享槽（不 .copy()）----
    // m_heldSlot 保证数据生命周期有效；QImage 是临时对象，作用域结束即毁
    const int w = m_currentFrame.width;
    const int h = m_currentFrame.height;
    img = QImage(m_currentFrame.data, w, h, w * 3, QImage::Format_RGB888);
} else {
    img = frameToQImage(m_currentFrame.data, ...);   // 旧路径兜底（Mock/无槽）
}
if (!img.isNull()) m_videoDisplay->setPixmap(QPixmap::fromImage(img));
```

**兜底路径说明**——真正的兜底是 `frameToQImage`（`gui.cpp:1121`），按格式分派，仅当**无 `m_heldSlot`** 时被 `refreshFrame` 调用（如 Mock 模式没有帧池槽）。

> ⚠️ **注意（代码现状）**：`setFrame`（`gui.cpp:473`）虽然定义仍在、深拷贝到 `m_frameBuffer`，但**全代码库已无任何调用点，是死代码**——真实相机走 `setFrameShared`，Mock 模式走 `refreshFrame` 的 else 分支调 `frameToQImage`，都不经 `setFrame`。其 `m_frameBuffer` 成员也仅被它使用（遗留）。
>
> ```cpp
> void CameraGUI::setFrame(const uint8_t* data, int len, int w, int h, PixelFormat fmt) {
>     m_frameBuffer.assign(data, data + len);   // 深拷贝，避免悬垂（死代码，无人调用）
>     m_currentFrame.data = m_frameBuffer.data();
>     ...
> }
> ```

```cpp
case PixelFormat::FMT_YUYV: {
    std::vector<uint8_t> rgb(w * h * 3);
    VideoProcessor::yuyvToRgb24(data, rgb.data(), w, h);   // 复用 camera 模块，含 NEON 加速
    return QImage(rgb.data(), w, h, w * 3, QImage::Format_RGB888).copy();
}
case PixelFormat::FMT_MJPEG: {
    // libjpeg 解码（旧路径）；帧池路径已在 displayTimer 用 VideoProcessor::decodeJPEGtoRGB 入槽
    ...
}
```

**三个关键设计（帧池路径）**：
1. **解码发生在 displayTimer，而非 refreshFrame**：`displayTimer` 里 `decodeJPEGtoRGB`/`yuyvToRgb24` 直接把结果写入池槽，`m_refreshTimer` 的 `refreshFrame` 只做浅引用上屏——解码结果与显示端共享同一块池内存，**省掉 setFrame 的 assign 和 QImage.copy() 两次 RGB 深拷贝**。
2. **QImage 浅引用（不 `.copy()`）是安全的**：因为 `m_heldSlot` 持有池槽引用（refs≥1），保证 `slot->data` 生命周期有效；QImage 是临时对象，作用域结束即毁，不跨帧持有 → 无悬垂。
3. **生命周期不变量**：GUI 始终持有且仅持有一份 `m_heldSlot`；`setFrameShared` 在换帧时才 release 旧槽（归零后复用），解码方不会回收正在显示的槽。

### 潜在坑点

- **双悬垂风险（遗留 setFrame 路径，死代码）**：遗留 `setFrame` 必须深拷贝（`g_state.frameData` 会被采集线程 realloc）；旧 `QImage` 构造后必须 `.copy()`。**帧池路径消除了这两处**——数据生命周期由 `m_heldSlot` 引用计数保证。
- **`m_heldSlot` 泄漏风险**：若 `setFrameShared` 换帧时忘了 release 旧槽、或析构时没释放 `m_heldSlot`，槽永不归还 → 池满持续丢帧。`CameraGUI::~CameraGUI` 里判空释放 `m_heldSlot` 正是防泄漏（`gui.cpp:113`）。
- **`setScaledContents(true)` 的性能**：QLabel 每帧绘制时都对 pixmap 做一次缩放插值，640x480 可接受，但升 720p 后 CPU 开销翻倍——应缓存缩放结果。**实测它在 linuxfb 上会额外触发 `QColorProfile::fromSRgb`（Qt 颜色管理）成为 CPU 热点**，关闭后该热点从 48% 降到 27%（见 §2.2 坑点）。
- **坏帧静默跳过 vs 画面停顿**：解码失败 `release(slot)` 丢帧，`refreshFrame` 里 `if (!img.isNull())` 跳过 setPixmap，画面保持上一帧。好处是不花屏，坏处是坏帧过多时画面"冻住"却无提示——需要一个坏帧计数器报警。

### 面试追问与应答

**Q1：帧池路径和旧 setFrame 深拷贝路径的本质区别？为什么能零拷贝？**
**A**：旧路径（`setFrame` 现已无调用点，为死代码，仅作历史对比）：`setFrame` 把帧深拷贝进 `m_frameBuffer`（assign），`frameToQImage` 解码后再 `.copy()` 一次，RGB24 每帧拷 2 遍。帧池路径：`displayTimer` 解码结果**直接写入池槽**（`slot->data`），`setFrameShared` 让 `m_currentFrame` 零拷贝指向共享内存，`refreshFrame` 用 QImage 浅引用上屏——RGB24 全程 0 次深拷贝。安全靠 `m_heldSlot` 引用计数保证槽生命周期，实测显示链路拷贝从 10.0 → 0.5 MB/s（-95%）。

**Q2：MJPEG 每帧解码耗时约 25ms，decode 在哪个线程？会不会卡 UI？**
**A**：解码发生在 Qt 主线程的 `displayTimer` 的 timeout 槽（帧池路径下不再是 `frameToQImage`）。一次解码 25ms 会阻塞事件循环。**单核 i.MX6ULL 上把解码移出 GUI 线程反而更卡**（线程无法并行，多引入拷贝+切换开销，已回退），所以解码留在 GUI 主线程是当前妥协。**重要修正（实测）**：帧率卡 10fps 的根因**不是解码，而是摄像头硬件实际输出 10fps**——`v4l2-ctl` 直测确认（尽管能力列表声称 30fps）。低分辨率显示解码能省 CPU，但**无法突破硬件帧率上限**（详见 §2.2 坑点"判定帧率瓶颈的方法论"）。

**Q3：为什么 `setScaledContents(true)` + `setPixmap`，而不是先 `img.scaled(w,h)` 再设置？**
**A**：`setScaledContents(true)` 让 QLabel 在绘制时拉伸 pixmap，等价于先缩放但**延迟到 paintEvent**、避免多一次中间缓冲；缺点是每次绘制都重缩放。对轻微缩放可接受，升 720p 后应缓存缩放结果。

**Q4：`frameToQImage` 为什么是 `switch` 而不是多态？加一种新格式要改哪？**
**A**：当前 4 种格式（RGB24/RGB565/YUYV/MJPEG）分支清晰、无抽象成本，且帧池路径下 `frameToQImage` 只是无槽时的兜底。但加新格式（如 NV12、H.264 解码帧）要改 switch + 头文件枚举。可演化为"`FormatConverter` 策略表"——`std::unordered_map<PixelFormat, std::function<QImage(...)>>`，新增格式注册即可，符合开闭原则（见 3.3）。

## 2.2 块二：帧率控制（displayTimer / m_refreshTimer 双定时器与双 FPS）

### 代码讲解

`main.cpp`（显示定时器，拉模式 + 帧池路径）：

```cpp
displayTimer = new QTimer(&gui);
displayTimer->setInterval(33);                    // 33ms ≈ 30fps
QObject::connect(displayTimer, &QTimer::timeout, [&gui, mjpegServer]() {
    // 1. 借 RGB 写槽（无空闲则丢帧，不阻塞）
    FrameSlot* slot = g_rgbPool->acquire();
    if (!slot) return;
    // 2. 短锁拷贝出原始帧
    std::vector<uint8_t> raw;  int srcW, srcH;  PixelFormat srcFmt;
    {
        std::lock_guard<std::mutex> lock(g_state.mtx);
        if (g_state.frameData.empty()) { g_rgbPool->release(slot); return; }
        raw = g_state.frameData;                    // 原始帧拷贝（JPEG ~0.1MB，唯一）
        srcW = g_state.width; srcH = g_state.height; srcFmt = g_state.format;
    }
    // 3. 解码/转换直接写入池槽（消除二次拷贝）
    slot->width = srcW; slot->height = srcH; slot->format = FMT_RGB24;
    if (srcFmt == FMT_MJPEG) {
        if (!VideoProcessor::decodeJPEGtoRGB(raw.data(), raw.size(), slot->data, dw, dh))
            { g_rgbPool->release(slot); return; }   // 坏帧丢帧
    } else if (srcFmt == FMT_YUYV) {
        slot->data.resize(srcW * srcH * 3);
        VideoProcessor::yuyvToRgb24(raw.data(), slot->data.data(), srcW, srcH);
    }
    // 4. 发布并交 GUI 共享（setFrameShared 持有引用，零拷贝上屏）
    slot->seq++;
    g_rgbPool->publish(slot);
    if (FrameSlot* ds = g_rgbPool->share())
        gui.setFrameShared(ds);                     // GUI 接管引用

    // 统计实际显示 FPS（每 30 次成功渲染一帧算平均）
    dispFpsCount++;
    if (dispFpsCount % 30 == 0) {
        auto now = steady_clock::now();
        double elapsed = duration<double>(now - dispFpsLastTime).count();
        if (elapsed > 0.0) dispFps = 30.0 / elapsed;
        dispFpsLastTime = now;
    }
    gui.setFPS(g_state.fps);        // Cap FPS（采集线程取帧速率）
    gui.setDisplayFPS(dispFps);     // Disp FPS（实际显示速率）
    gui.setClientCount(mjpegServer->clientCount());
});
displayTimer->start();
```

**核心设计：拉模式 + 覆盖旧帧 + 帧池双缓冲，天然防堆积。** 采集线程每帧都写 `g_state`，但 GUI 每 33ms 只取一次——两帧之间的中间帧被"覆盖丢弃"。比"采集线程每帧直接推送刷新（emit 信号）"好在：

- **不存在队列，就没有"堆积"这一说**：`g_state` 是"最新一帧"共享状态而非事件队列，无论采集多快（λ 多大）、显示多慢（μ 多小），都没有东西排队等待，天然规避了 λ > μ 时事件队列无限堆积的问题；
- **帧池无锁双缓冲**：生产者（displayTimer 写槽）与消费者（refreshFrame 读 `m_current` 槽）通过引用计数分离，`acquire` 只借空闲槽（refs==0），写槽时不会有消费者读它——**读写分离不靠锁**；
- 显示帧率稳定在定时器周期，不受采集波动影响（推模式下显示节拍跟随采集、有抖动）；
- 没有跨线程事件投递，无元对象序列化与事件循环调度开销、无事件队列延迟漂移。

**双 FPS 设计（Cap FPS / Disp FPS）**：
- `setFPS` 显示 **Cap FPS**（采集线程取帧速率，来自 V4L2 `updateFPS()` 每 30 帧测量）；
- `setDisplayFPS` 显示 **Disp FPS**（实际显示速率，displayTimer 内统计"成功渲染一帧"每 30 次平均）。
- 两个数对比能直接暴露"采集 30、显示 15"的瓶颈——**Cap FPS 高但 Disp FPS 低，说明解码/渲染跟不上**（如 720p 解码慢）。这是排查显示性能的关键仪表。

**帧率联动**：用户拖帧率滑块，回调里 `displayTimer->setInterval(1000/fps)` 同步更新显示节奏，让 UI 跟随配置。

### 潜在坑点

- **`g_state.fps` 是采集线程的 FPS，不是显示 FPS**：两个数字不同步是常态，若面试官问"为什么 Disp FPS 比 Cap FPS 低"，要能回答"解码耗时吃掉显示预算"（分辨率高→解码慢→Disp 下降，但 Cap 由采集线程独立测量，仍可能保持较高）。
- **⚠️ Disp FPS 可能高于 Cap FPS（统计口径差异，非 bug 也非"显示快于采集"）**：这两个 FPS 统计的不是同一个对象。Cap FPS 统计"采集到的**不同帧**的速率"（`getFrame` 取到新帧才计数，真实受 V4L2 输出 + 单核 CPU 限制）；Disp FPS 统计"displayTimer **渲染动作**的次数"，**未做新帧去重**——只要 `g_state.frameData` 非空就渲染并计入，重复渲染同一帧也算。当采集只有 ~10fps（100ms/帧）而 displayTimer 按 33ms 周期触发时，一个采集帧间隔内 displayTimer 会渲染约 3 次（其中 2 次是重复渲染同一帧），Disp FPS 被"重复渲染计数"灌水，长期稳定地虚高于 Cap FPS。**修复方向**：在 `g_state` 加 `frameSeq` 帧序号，displayTimer 只在帧序号变化时渲染并计数，则 Disp FPS 恒 ≤ Cap FPS。
- **双定时器相位不同步**：`displayTimer` 拷贝的帧与 `m_refreshTimer` 渲染的帧可能相差 0~33ms，画面延迟略大但稳定。若要降低延迟，可合并为单定时器。
- **⚠️ linuxfb 渲染热点 `QColorProfile::fromSRgb`（实测占 ~48% CPU）**：Qt linuxfb 平台在每次 pixmap 绘制/缩放时做颜色空间管理，perf 采样显示该函数是单核 CPU 的最大热点。**关闭 `setScaledContents` 只能部分缓解（48%→27%），无法完全消除**——它与 QImage 格式无关（即使改成 RGB565 也一样，因为根因是 linuxfb 插件的固有绘制管线，而非格式不匹配）。这是嵌入式 linuxfb 渲染的固有代价。
- **⚠️ 判定帧率瓶颈的方法论（关键经验）**：本次排查 Cap FPS 恒为 10fps，发现"CPU 从 100% 降到 67%（有 33% 空闲）但帧率仍 10"——**这说明瓶颈不在 CPU，而在供给端（摄像头硬件）**。判断瓶颈要先问"CPU 有空闲时帧率提得上去吗"：提不上去 → 瓶颈在硬件/驱动，改应用代码无效。**最终用 `v4l2-ctl` 直测确认摄像头实际输出 10fps**（尽管 `--list-formats-ext` 声称支持 30fps，能力列表 ≠ 实际输出）。
- **帧率滑块范围受限**：`setFramerateRange` 钳制到 1~120fps，且 `displayTimer->setInterval` 用 `std::max(10, 1000/fps)` 保底 10ms——显示定时器最快 100fps，避免 setInterval(0) 导致忙等。

### 面试追问与应答

**Q1：为什么用 QTimer 33ms 而不是在采集线程里直接调 setFrame？**
**A**：① **线程安全**：Qt 控件只能在 GUI 线程操作，跨线程改 QLabel 必须通过 queued connection 或定时器转发；QTimer 天然运行在 GUI 线程。② **节流防堆积**：每帧跨线程投递时，事件队列是否堆积取决于"采集速率 vs GUI 消费速率"——**只有采集快于 GUI 处理能力时**（如 60fps 采集、解码 25ms/帧）队列才会无限堆积、延迟越来越大；慢速采集（λ < μ）下队列稳定、不会堆积，但推模式仍有显示节拍随采集波动、每次投递带元对象序列化开销的缺点。拉模式固定 33ms 取最新帧，无论采集多快都天然丢弃中间帧、延迟有界且节拍稳定。这是生产者-消费者模型里"**推模式保每帧、拉模式保实时性**"的经典取舍。

**Q2：锁 `g_state.mtx` 期间如果 displayTimer 内部做解码（25ms），采集线程会卡住吗？**
**A**：不会卡 25ms。帧池路径下，`displayTimer` 拿锁只做**短锁拷贝**——`raw = g_state.frameData`（原始帧，JPEG ~0.1MB），**解码（`decodeJPEGtoRGB`）发生在锁外**，直接写入池槽。也就是说**锁内只拷贝、锁外解码**，锁持有时间就是一次原始帧 memcpy（~1ms 级），采集线程最多等 1ms。这个"锁内轻活、锁外重活"的划分是本题的得分点（和 camera 篇"拷贝 #2 快速释放锁"同一哲学）。

**Q3：Disp FPS 统计是怎么算的？有什么缺点？**
**A**：`main.cpp` displayTimer 回调内，每 30 次"**成功渲染一帧**"（走到 `setFrameShared` 之后）用 `30 / elapsed` 算平均。优点：统计的是实际渲染的帧，而非定时器触发次数——解码慢导致丢帧时（`acquire` 失败提前 return 的路径不计入），Disp FPS 会如实下降。缺点：30 帧窗口在低帧率下更新慢（10fps 下约 3s 才更新一次）。更细的做法可改用滑动指数平均（EMA），响应更快。

## 2.3 块三：回调注入（松耦合的关键）

### 代码讲解

`gui.h:63-77` 定义回调类型：

```cpp
using CallbackVoid   = std::function<void()>;
using CallbackBool   = std::function<bool(bool)>;      // 返回 true=成功，用于"拒绝"语义
using CallbackIntInt = std::function<void(int, int)>;  // (w, h)
using CallbackFormat = std::function<void(PixelFormat)>;
using CallbackCameraControl = std::function<void(int cid, int value)>;
using CallbackFramerate = std::function<void(int fps)>;
```

注入接口（`gui.cpp:527`）：

```cpp
void CameraGUI::onCaptureRequest(CallbackVoid cb)       { m_onCapture = std::move(cb); }
void CameraGUI::onRecordToggle(std::function<bool(bool)> cb) { m_onRecordToggle = std::move(cb); }
void CameraGUI::onResolutionChanged(CallbackIntInt cb)  { m_onResolutionChanged = std::move(cb); }
```

**"回调可拒绝"模式**（录像按钮）：`onRecord` 先问回调，回调返回 false 就不切换 UI 状态：

```cpp
void CameraGUI::onRecord() {
    bool shouldRecord = !m_isRecording;
    if (m_onRecordToggle) {
        if (!m_onRecordToggle(shouldRecord)) {          // 回调拒绝
            qDebug() << "[GUI] Record denied (format not supported)";
            return;                                      // UI 状态不变
        }
    }
    m_isRecording = shouldRecord;                        // 通过才改 UI
    ...
}
```

**回调 → 业务的全链路示例**（分辨率切换，`main.cpp:932`）：

```cpp
gui.onResolutionChanged([capture](int w, int h) {
    if (!capture->isStreaming()) return;
    g_state.paused = true;                     // ① 暂停采集线程
    // ② 等采集线程确认暂停（getFrame 1s 超时，最多等 1.1s）
    ...
    capture->stopCapture();                    // ③ 安全停止
    int ret = capture->setFormat(w, h, capture->getCurrentFormat());
    if (ret < 0) { ... capture->setFormat(640, 480, ...); }  // 失败回退
    capture->startCapture();                   // ④ 重启
    g_state.paused = false;                    // ⑤ 恢复
});
```

为什么"暂停 → 停止 → 设置 → 重启"四步必须做：V4L2 的 `S_FMT` 在 STREAMON 状态下返回 EBUSY，且停止时必须确保没有线程还在 `dqbuf`/`putFrame` 使用 mmap 缓冲——这就是 `g_state.paused` + `pauseCv` + `pausedAck` 这套暂停握手的作用。

### 潜在坑点

- **回调在 GUI 线程执行，可能阻塞 UI**：分辨率切换回调里 `wait_until` 最多等 1.1s，期间 UI 冻结。这是"回调同步执行"的代价，模态操作可接受，但长耗时回调应异步化。
- **单回调覆盖语义**：后 `onXxx` 覆盖前注册，多个控制端（GUI + 网络）同时改参数会互相覆盖——目前只有 GUI 一个控制端，安全；多端场景需广播。
- **lambda 捕获的生命周期**：回调捕获 `capture`/`rtspServer` 裸指针，若这些对象在 GUI 析构前被 delete，回调会变悬垂。main.cpp 中这些对象存活整个 app 生命周期，故安全，但属于隐式契约。

### 面试追问与应答

**Q1：这个回调机制和你自己类里的信号（captureClicked 等）是什么关系？为什么不直接用信号？**
**A**：双通道。信号面向"外部想监听 GUI 事件"（测试代码 connect 感知点击），回调面向"GUI 需要让业务模块执行动作"。`std::function` 回调比跨对象 connect 更轻量、不引入 Q_OBJECT 元对象开销，且 main.cpp 注入 lambda 可直接捕获局部变量。缺点是无类型外约束、单注册——但恰好符合"每个业务动作只有一个执行者"。

**Q2：如果要把 GUI 从 Qt 换成别的，这些回调还能用吗？**
**A**：能用一半。`std::function` 回调是纯 C++，**业务侧（main.cpp）注入代码几乎不用改**；要换的是 display 内部（定时器、帧格式、触摸）。结论："**display 对业务的接口是干净的，业务对 display 的适配是 Qt 绑定的**"——合理分层，业务不该知道渲染细节。

**Q3：回调会被覆盖吗？为什么不支持多个监听者？**
**A**：会，后注册覆盖先注册。有意简化——每个业务动作全局只有一个执行者，不需要发布-订阅广播。若未来要支持多客户端控制面板（GUI + Web 同时改参数），可升级为 `std::vector<CallbackXxx>` 广播或直接用 Qt 信号（本身支持多 connect）。

## 2.4 块四：页面导航（QStackedWidget 三层嵌套）

### 代码讲解

**代码意图**：800x480 单屏空间有限，用页面栈切换"实时预览 / 相册网格 / 全屏 / 视频播放"，避免同时绘制多个页面浪费内存。

三层嵌套的 QStackedWidget：

```
QStackedWidget (gui.m_mainStack)
  ├─ [0] 实时预览容器 (m_liveViewContainer)
  └─ [1] PhotoGallery (m_gallery)
        └─ QStackedWidget (gallery.m_stack)
             ├─ [0] 缩略图网格视图 (m_galleryView)
             └─ [1] 全屏视图 (m_fullView)
                   └─ QStackedWidget (gallery.m_fullMediaStack)
                        ├─ [0] 照片显示 QLabel (m_fullPhotoDisplay)
                        └─ [1] VideoPlayer (m_videoPlayer)
```

导航逻辑：
- `onGallery()`：仅当 `m_mainStack->currentIndex() == 0` 才 `showGallery()`（防重复进入），切到相册时**隐藏 Capture/Record 按钮**（`gui.cpp:450`）。
- `onBackFromGallery()`：相册的 `backToLive` 信号 → `showLivePreview()` 切回 index 0，恢复按钮。
- gallery 内部：网格 ↔ 全屏；全屏内：照片 ↔ 视频播放器。

**为什么用 QStackedWidget 而不是换窗口**：
1. 单屏无窗口管理器，弹新窗口不可行；
2. 所有页面**常驻内存**（提前构建），切换 O(1)、无重建延迟——代价是所有页面（含相册控件）的初始构建内存常驻，约多占 1~2MB。
3. `QStackedWidget` 只显示当前页，**非当前页不绘制、不派发事件**，CPU 上零开销。

### 潜在坑点

- **`setGalleryStorage` 的重建逻辑**：`gui.cpp:433` 先 `delete m_gallery` 再 `new PhotoGallery(storage, this)` 替换 index 1——因为 storage 绑定时机晚于构造（main.cpp 初始化完 storage 才调用）。若多次调用会反复删建，但 main.cpp 只调用一次，安全。
- **页面切换时未停视频**：`onBackFromGallery`/`onPrevPhoto`/`onNextPhoto` 都先 `stopVideoPlayback()`，防止视频 QTimer 继续跑（后台解码浪费 CPU + 状态错乱）。面试可主动提这一点。
- **按钮可见性与页面状态耦合**：进相册要 hide Capture/Record，返回要 show——用 `currentIndex` 驱动两处 UI 状态，逻辑分散；更优做法是用"页面枚举 + 状态机"统一管理。

### 面试追问与应答

**Q1：为什么不用 QTabWidget 或 QDialog 做相册？**
**A**：QTabWidget 会显示标签页头，在 800x480 屏上浪费垂直空间且不符合相机 UI 习惯；QDialog 是模态弹窗，会阻塞主窗口事件循环，导致视频预览冻结。QStackedWidget 全屏切换、无模态、无标签头，最适合单屏设备。

**Q2：QStackedWidget 非当前页会消耗资源吗？**
**A**：控件对象和布局常驻内存（构建时全部创建），但**非当前页不参与绘制和事件分发**，CPU 零开销；内存开销取决于页面复杂度（相册的缩略图控件是主要部分）。这是"用内存换切换速度"的典型嵌入式取舍。

**Q3：页面切换时为什么要先停视频？**
**A**：VideoPlayer 用 QTimer 驱动逐帧解码，若切回预览时视频还在播，QTimer 会继续在后台解码（浪费 CPU）且状态错乱（回到相册时可能还停在播放中）。`stopVideoPlayback()` 在 `updateFullscreenDisplay`/`onBackToGallery`/`onPrevPhoto`/`onNextPhoto` 里统一调用，保证"切页 = 停播"的确定性。

## 2.5 块五：触摸交互（eventFilter / 滑动翻页 / 滑块防抖）

### 代码讲解

**代码意图**：把触摸手势翻译成业务动作，且不让高频手势打爆业务。

**触摸事件完整链路**（背下来）：

```
gt9147 电容屏 → 内核驱动 → input subsystem (evdev)
  → /dev/input/eventX → linuxfb 平台插件读 fd → Qt 合成 QMouseEvent
  → QApplication::notify → 分发到目标 QWidget → slot/eventFilter
```

应用层**不需要也不应该**直接读 `/dev/input`，除非要做 Qt 之外的特殊手势。

**滑动翻页**（`gallery.cpp:997`）通过 `eventFilter` 手写手势识别：

```cpp
bool PhotoGallery::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_fullView && m_stack->currentIndex() == 1) {
        if (event->type() == QEvent::MouseButtonPress) {
            m_touchStartX = static_cast<QMouseEvent*>(event)->x();
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            int dx = static_cast<QMouseEvent*>(event)->x() - m_touchStartX;
            if (std::abs(dx) > 60) {          // 滑动阈值 60px
                if (dx < 0) onNextPhoto();
                else        onPrevPhoto();
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}
```

要点：**只在按下/抬起两个点采样位移**，不做逐帧跟踪——嵌入式触摸识别"最简可用"策略；60px 阈值过滤手指抖动。

**滑块防抖**（帧率滑块，`gui.cpp:959`）：

```cpp
void CameraGUI::onFramerateSliderChanged(int value) {
    m_framerateValue->setText(QString("%1 fps").arg(value));  // 视觉立即反馈
    m_framerateDebounceTimer->start(300);                      // 300ms 防抖
}
void CameraGUI::onFramerateDebounced() {
    int value = m_framerateSlider->value();
    if (m_onFramerate) m_onFramerate(value);                   // 实际执行
}
```

**为什么必须防抖**：帧率变更回调里要做"暂停采集 → 停流 → `VIDIOC_S_PARM` → 重启流"（约几 ms 到几十 ms），而滑块拖动每秒触发几十次 `valueChanged`。不防抖会把采集线程反复打断，画面冻结。用 300ms 单发定时器合并高频事件，让回调只在用户停手后执行一次——这就是"**合并最后状态**"的防抖语义。

### 潜在坑点

- **`eventFilter` 返回 false 的作用**：返回 false 表示"不吞掉事件"，让事件继续沿 Qt 事件传播链走。若返回 true 会吞掉点击，导致按钮等子控件收不到事件。
- **滑动阈值与速度方向**：只看位移不看速度，慢速拖动翻页、快速轻扫也翻页，行为一致；但"按下后滑出又滑回"（dx<60px）不会触发——符合预期。
- **`setMouseTracking(true)`**：`m_fullView->setMouseTracking(true)` 让无按键移动也产生事件，这里配合按下/抬起逻辑无副作用；但若在视频区开 mouse tracking 会带来额外事件开销。

### 面试追问与应答

**Q1：触摸坐标和画面显示偏移了，可能是什么原因？如何校准？**
**A**：三类原因。① **tslib 校准缺失**：linuxfb 走 evdev 直连时，Qt 假设触摸板坐标已归一化；若驱动上报原始坐标没做转换，会出现线性偏移/镜像/旋转。解决：校准 `QT_QPA_EVDEV_*` 环境变量，或用 tslib 校准矩阵。② **分辨率不匹配**：触摸板上报分辨率（absinfo）与实际屏分辨率不一致，Qt 按上报范围归一化到屏尺寸导致偏差——检查 `/sys/class/input/eventX/device/` 的 abs 参数。③ **屏的镜像/翻转**：电容屏与面板装配角度导致，需在设备树配置 `touchscreen-inverted-x/y` 或 swap-xy。排查顺序：先 `evtest` 看原始坐标是否符合物理位置，再检查 Qt 环境变量与校准。

**Q2：QMessageBox::exec() 是阻塞的，触摸删除确认时 UI 会卡吗？**
**A**：会阻塞 Qt 事件循环，但这是**有意的模态**——删除是危险操作，模态确认要求用户先决策再继续。真正的问题是 `exec()` 打开期间视频预览停止刷新（GUI 线程被占），画面冻结几百 ms。改进：确认框用非阻塞 `open()` + 信号回调，或先暂停预览定时器、确认完再恢复。当前选简单，可接受。

**Q3：为什么滑动翻页不用 QSwipeGesture？**
**A**：QGesture 框架在嵌入式 linuxfb 上依赖平台手势支持，稳定性差；手写 eventFilter 按下/抬起两点判断，逻辑透明、零依赖、可精确控制阈值。对"水平翻页"这种单轴手势，自写比框架更可控。

## 2.6 块六：相册与缩略图（scale_denom 缩放解码）

### 代码讲解

**代码意图**：在低配平台上快速显示大量照片，核心是"解码时直接缩到目标尺寸"。

**列表刷新**（`gallery.cpp:54` `refresh()`）：`listPhotos` + `listVideos` 合并 → 按时间戳倒序 → 按日期重建分组 → `loadVisibleThumbnails()`。

**scale_denom 缩放解码**（`gallery.cpp:261`）——libjpeg 的核心优化：

```cpp
int scaleDenom = 1;
if (cinfo.image_width > thumbW * 8)  scaleDenom = 8;    // 大图直接 1/8 解码
else if (cinfo.image_width > thumbW * 4) scaleDenom = 4;
else if (cinfo.image_width > thumbW * 2) scaleDenom = 2;
cinfo.scale_num = 1;
cinfo.scale_denom = scaleDenom;
jpeg_start_decompress(&cinfo);
```

**原理**：JPEG 的 DCT 系数本身就是 8x8 块，libjpeg 的 `scale_denom=2/4/8` 让**逆 DCT 阶段直接跳过部分高频系数**，解码器只输出 1/2、1/4、1/8 尺寸。相比"先全尺寸解码再 `QImage.scaled()`"，内存占用降为 1/N²，CPU 大幅下降——"170px 缩略图实测 ~15ms"的来源。

**视频缩略图**：`extractAviThumbnail` 解析 RIFF 头定位第一帧 `00dc` chunk，取 JPEG 数据复用同一解码路径（`createVideoThumbnail`），视觉上与照片一致。

**多选删除**：`QSet<int> m_selectedIndices` 存选中索引，`onDeleteSelected` 遍历删除 + 收集失败项 + 弹窗汇报 + `refresh()` 重建列表。

### 潜在坑点

- **"伪懒加载"的代价**：`loadVisibleThumbnails` 一次性为所有照片建控件 + 解码缩略图，100 张 ≈ 1.5s 卡顿。真懒加载（滚动到哪建哪）是优化方向——可结合 QScrollArea 的滚动事件按需创建。
- **每张缩略图一个 `QPushButton` + `QCheckBox` + `QLabel`**：控件数量 = 照片数 × 3，200 张照片 = 600 个控件，Qt 控件对象内存开销大。更优：用 `QListView`/`QGraphicsView` 的 delegate 自绘，或复用控件池。
- **删除后索引漂移**：`onDeletePhoto` 删除后 `refresh()` 重建 `m_flatPhotos`，原 `m_currentIndex` 可能越界——代码用 `newIdx >= size` 钳制回退，处理正确。
- **坏图占位**：缩略图解码失败显示 "?"（照片）/ "▶"（视频），不崩溃。

### 面试追问与应答

**Q1：为什么不直接用 QImage(path) 让 Qt 自己解？**
**A**：① **性能**：Qt 内置 JPEG 解码默认全尺寸解码，100 张 1280x720 的内存/时间开销是 scale_denom 方案的十几倍；libjpeg-turbo 缩放解码是嵌入式缩略图标配。② **可控性**：libjpeg 可预分配 buffer、自定义错误处理（坏图不崩），`QImage(path)` 遇畸形 JPEG 可能直接失败或打警告。同一套"libjpeg + 定点运算"优化哲学贯穿整个项目。

**Q2：scale_denom 能缩小到什么程度？跟 scale_num 什么关系？**
**A**：libjpeg 官方支持 1/2、1/4、1/8（scale_denom=2,4,8），scale_num 默认为 1。这是 DCT 块的整分频，无损额外解码开销。想要任意缩放（如 1/3）得自己先 DCT 域缩再二次缩放。本项目 170px 目标对 640/1280 宽图用 2 或 4 够用。

**Q3：200 张照片的相册，内存怎么评估？**
**A**：每张缩略图 QPixmap = 170x120x4 ≈ 82KB（ARGB32），200 张 ≈ 16MB 峰值——明显偏大。优化：① 用 scale_denom 解码到更小再放大；② 用 `QListView` delegate 按需绘制（只有可见项创建 pixmap）；③ 复用单个 pixmap 缓冲。README 里"相册峰值内存 ~2.5MB"是 6 张可见缩略图的场景——说明当前实现**实际上只建可见部分才能达标**，一次性建全部会爆。

## 2.7 块七：OSD 叠加与 QPainter

### 代码讲解

**代码意图**：在画面上叠加文字/图形信息（FPS、模式、框选），选对绘制层次。

项目里有两类"叠加"：

**① 状态栏信息**（FPS / Clients / REC / LIVE）用**独立的 QLabel 放在布局里**，不画在视频上（`gui.cpp:162-186`）。这是最廉价的"OSD"——不需要和视频帧合成，只是独立的 UI 元素。

**② Mock 模式下在视频 pixmap 上直接画文本**（`gui.cpp:309-328`）：

```cpp
QPixmap pix = m_videoDisplay->pixmap(...);
QPainter p(&pix);
p.setPen(QColor(255, 255, 255, 180));
p.setFont(QFont("monospace", 11));
p.drawText(8, 22, QString("Mock Mode | Frame: %1 | %2x%3")...);
p.end();
m_videoDisplay->setPixmap(pix);
```

**QPainter 绘制时机**：在 `setPixmap` 之前对同一份 pixmap 画文字，然后一次 setPixmap——避免"先设置再画"的二次重绘。

### 潜在坑点

- **QPainter 画在 QPixmap 上是"改像素"**：每次 `refreshFrame` 都要先取回 pixmap → 画文字 → setPixmap，等于把 OSD 光栅化和视频合成耦合，33ms 一次。状态栏用独立 QLabel 则只有文本变化才重绘。
- **linuxfb 无合成器**：半透明 OSD（`QColor(...,180)` alpha）需要软件 alpha 混合，性能差。这也是状态栏用"不透明色块 QLabel"而非半透明悬浮层的原因。
- **字体渲染成本**：`QFont("monospace", 11)` 每次绘制都重新布局文字，若每帧画会浪费 CPU——OSD 文字应只在内容变化时更新。

### 面试追问与应答

**Q1：在视频上叠加 OSD，QPainter 直接画 vs 分层控件（child QLabel），哪个性能好？为什么？**
**A**：**分层控件性能好**。视频帧是**每 33ms 全帧更新**的对象，OSD 是**低频不变**的对象。OSD 直接画进视频 pixmap，则每次视频刷新都要把 OSD 文字重新栅格化；而独立 child QLabel 只需在 OSD 内容变化时重绘那一小块（脏矩形），视频刷新时 OSD 控件不重绘。当前代码状态栏用独立 QLabel（正确），Mock 水印画进 pixmap（调试用，可接受）。若做框选矩形，也应用独立覆盖控件或 QGraphicsItem，而不是每帧合成进图像。

**Q2：透明 OSD 在 linuxfb 上有什么坑？**
**A**：linuxfb 的 `WA_TranslucentBackground`/半透明窗口支持很弱（无合成器），alpha 混色通常需软件合成，性能差甚至不支持。稳妥做法：不透明背景 + 小面积，或把 OSD 合成进视频帧（软件 alpha）。本项目状态栏用"有背景色块 QLabel"正是回避合成短板。

**Q3：为什么帧号/FPS 这种"必须实时变"的信息不用 OSD 画进视频？**
**A**：其实状态栏已经做到了"独立控件 + 实时刷新"——`setFPS`/`setDisplayFPS` 只改 QLabel 文本，Qt 只重绘该标签区域，不动视频区。视频区保持纯图像，绘制路径最简。这是"**把变的东西和不变的东西分层**"的原则。

## 2.8 块八：VideoPlayer（轻量 AVI 播放器）

### 代码讲解

**代码意图**：录制的是自产 AVI（RIFF + MJPEG 帧），没必要引入 ffmpeg——手写一个"只会放自家格式"的播放器，零外部解码依赖。

**AVI 解析**（`video_player.cpp:293` `parseAviHeader`）三步定位：

```
① RIFF 头校验: "RIFF" + size + "AVI "
② hdrl LIST → 扫到 avih 块 → dwWidth/dwHeight/dwTotalFrames/dwMicroSecPerFrame
   (fps = 1e6 / dwMicroSecPerFrame)
③ 跳到 movi 数据区 → 越过数据 → 读 idx1 索引表 → m_index (每帧偏移+长度)
```

**逐帧播放**（`onTimerTick`，`video_player.cpp:251`）：

```cpp
m_currentFrame++;
if (m_currentFrame >= m_totalFrames) { pause(); emit playbackFinished(); return; }
std::vector<uint8_t> jpegData;
if (!readFrameJpeg(jpegData)) { emit playbackError(...); pause(); return; }
decodeAndDisplay(jpegData);     // libjpeg 解码 → QPixmap → setPixmap
```

`decodeAndDisplay` 复用 scale_denom 缩放解码（按显示区大小选 1/2/4）——视频 640x480 播放在 ~360px 区域时直接 1/2 解码，省一半 CPU。

**进度条拖拽 seek**：`sliderReleased` → `seekToFrame(target)` → 读帧解码显示 → 恢复定时器。`m_sliderDragging` 标志在拖拽期间暂停定时器刷新，避免边拖边播跳帧。

### 潜在坑点

- **帧索引的内存放大**：`m_index` 每帧一条 `AviIndexEntry`，120 帧很小；但若 AVI 是超长录像，idx1 可能数千条——解析时 `numEntries = idx1Size / sizeof(AviIndexEntry)` 直接 resize，需防御异常 idx1Size。
- **`fread` 字节序**：RIFF 全程小端，`dwMicroSecPerFrame` 等直接按主机序读——i.MX6ULL 是 ARM 小端，PC x86 也是小端，恰好一致；若移植到大端平台需翻转。这是"无抽象、直接读"的隐患。
- **`dataSize > 100MB` 防御**：`readFrameJpeg` 上限 100MB 防恶意/损坏文件的超大分配。
- **播放结束语义**：到最后一帧 `pause()` 保持画面 + emit `playbackFinished`，gallery 里 `onVideoPlaybackFinished` 只打日志保持暂停——用户可 Prev/Next 继续。
- **解码仍在 GUI 线程**：和 MJPEG 预览同样的短板——25ms 解码会阻塞事件循环，播放时按钮交互可能卡顿。低分辨率 AVI 可接受。

### 面试追问与应答

**Q1：为什么不用 ffmpeg/gstreamer 播放 AVI？**
**A**：本项目录制的 AVI 是自产格式（MJPEG + RIFF），结构已知且简单；引入 ffmpeg 在 i.MX6ULL 上会显著增加二进制体积（几十 MB）和交叉编译负担。手写解析器只支持自家格式，代码可控、体积小（~500 行）。**工程权衡**：在自己能完全控制编解码格式的前提下，不需要通用解码器；若未来要播 H.264/其他来源视频，就必须引入或移植 ffmpeg。

**Q2：AVI 播放的帧率是怎么控制的？会不会快放/慢放？**
**A**：QTimer 间隔 = `1000/m_fps`（fps 从 avih 的 `dwMicroSecPerFrame` 算出）。解码耗时不计入间隔，所以如果单帧解码超过间隔，实际会变慢（跳帧）。想精确同步需"解码时间戳 + 补偿"（解码快的早等、慢的延迟减少），当前实现是"尽力而为"的定时器节流。

**Q3：seek 是怎么做到的？为什么能 O(1)？**
**A**：AVI 的 idx1 索引表记录了每帧在 movi 区的字节偏移，`seekToFrame` 直接用 `fseek(m_file, m_moviDataOffset + entry.dwChunkOffset, SEEK_SET)` 定位——**随机访问 O(1)**。这也是为什么录制端要写 idx1（见 storage 篇）：索引让播放器不必从头扫描到目标帧。

## 2.9 块九：Mock 模式

### 代码讲解

**代码意图**：无硬件也能开发/演示 UI，把 UI 开发从硬件依赖中解放出来。

`enterMockMode()`（`gui.cpp:1038`）：生成 8 色彩条 + 纵向渐变，`refreshFrame` 里每帧把彩条**水平位移 2 像素**模拟流动（`gui.cpp:282`），并叠加帧号水印。

```cpp
int offset = (m_mockFrameIndex * 2) % w;      // 每帧移动 2 像素
// 滚动实现：从 srcX = (x + w - offset) % w 取像素写回
```

**与真实模式的关系**：`setFrame` 会置 `m_mockMode = false`，从 Mock 平滑切回真实帧；`onResolutionComboChanged` 在 Mock 模式会重新 `enterMockMode()` 按新分辨率重建测试图。

### 潜在坑点

- **Mock 循环逐像素滚动是 O(w×h) 的 CPU 重活**：640x480 全帧逐字节滚动在 PC 上无感，但若是真机误入 Mock 模式会吃 CPU——实际只有无摄像头时才进 Mock。
- **Mock buffer 与 m_currentFrame 共享指针**：`m_currentFrame.data` 被设为 `m_mockBuffer.data()`，真实模式 `setFrame` 后 `m_mockMode=false` 且 data 改指 `m_frameBuffer`，指针归属切换需保证生命周期（两个成员都是类成员，安全）。

### 面试追问与应答

**Q1：Mock 模式的价值到底是什么？你实际用它做了什么？**
**A**：① **开发解耦**：PC 上无 V4L2、无 `/dev/fb0`，但 `scripts/build.sh` PC 模式能直接跑 GUI——UI 布局、信号槽、设置弹窗、相册交互全部 PC 调试，把"硬件适配"和"UI 开发"两条线分开，硬件未就绪时软件先行。② **演示/CI**：无摄像头也能演示整个界面流。③ **测试**：为 UI 逻辑提供确定性输入。

**Q2：Mock 模式对硬件联调有什么帮助？**
**A**：能预先验证 UI 的全部交互路径（切分辨率、切格式、开设置、进相册），联调时把故障范围收敛到"采集/解码"而非"UI 逻辑"。本质上是一种**软件在环（SIL）测试**。

---

# 第三部分 综合思考

## 3.1 与 camera / network / storage 的数据流耦合

**现状评估**：

| 耦合对象 | 方式 | 耦合度 | 说明 |
|----------|------|--------|------|
| camera | `std::function` 回调（控制） | **松** | GUI 不知道 CameraCapture 类的存在 |
| camera | `setFrameShared(FrameSlot*)` / `setFrame(...)`（数据） | **中** | main.cpp 在中间适配 + 帧池解码；GUI 认"帧槽契约" |
| camera | `VideoProcessor::yuyvToRgb24` / `decodeJPEGtoRGB`（编译期） | **中** | `gui.cpp` include `processor.h` 复用颜色转换与解码（纯静态工具类，无 V4L2 依赖） |
| common | `FramePool` / `g_rgbPool`（帧池） | **中** | GUI 通过 `extern FramePool* g_rgbPool` release 槽引用；`setFrameShared` 持有 `m_heldSlot` |
| network | `setClientCount(int)` 单向状态 | **松** | 只收状态，不发指令 |
| storage | 直接持有 `StorageManager*` | **紧** | 相册直接调用 `listPhotos`/`deletePhoto` 等 |

**为什么 storage 是紧耦合而 camera 是松耦合？**
- camera 控制频率低、语义简单（拍照/切分辨率），用回调最轻；
- 相册需要**大量双向数据交互**（列目录、删文件、读缩略图、存空间查询），十几个方法如果全走回调会非常啰嗦，直接持有指针更务实。

**编译期依赖说明（重要演进）**：早期版本 `gui.h` 内联了一份 `yuyv_to_rgb24`/`yuyv_to_rgb565`（BT.601 定点），与 `processor.cpp` 完全重复。重构后已删除，`gui.cpp` 统一调用 `VideoProcessor::yuyvToRgb24`（同样含 NEON 分流）与 `decodeJPEGtoRGB`（帧池显示解码）。收益：颜色转换与解码只维护一份实现，消除"两处系数不一致"的隐患；代价：display 对 camera 头文件多了一条编译期依赖（`processor.h` 是纯静态工具类，不含 V4L2 结构体，实际耦合很弱）。`gui.cpp` 本就要 include `capture.h` 用 `V4L2_CID_*` 常量，故该依赖并非新增方向。

**帧池依赖说明**：帧池改造后，`gui.cpp` 顶部 `extern FramePool* g_rgbPool` 引用 main.cpp 定义的全局帧池，`setFrameShared` 内部持有/释放 `FrameSlot` 引用。GUI 不再深拷贝帧数据，改为"共享引用 + 生命周期由引用计数保证"——这是对 camera 数据耦合方式的一次重要演进（从"拷贝契约"到"共享契约"）。

**改进方向**：相册对 storage 的依赖可以抽象为 `MediaProvider` 接口（`list/delete/extractThumbnail`），便于单元测试注入假存储；camera 侧保持回调即可。颜色转换若想彻底解耦，可将 `VideoProcessor` 从 `camera/` 目录上移到 `common/`（它本质是通用图像工具，不依赖 V4L2）——这是合理的下一步重构。**松紧结合**是合理的——不同接口不同耦合策略。

## 3.2 需求变更下的重构推演

### 场景 A：预览升到 1280x720（720p）

**变化点**：每帧 RGB24 从 0.92MB → 2.76MB（×3），解码/缩放/拷贝全部 ×3。

**重构方案**：
1. **低分辨率显示解码（第一优先级，已实测验证方向）**：720p MJPEG 单帧全尺寸解码 ~50-80ms，主线程必卡。**注意：不能靠"解码移出 GUI 线程"**——实测单核 i.MX6ULL 上独立解码线程反而更卡（线程无法并行，多引入拷贝+切换开销，已回退）。正确方向是**用 `scale_denom` 只解码显示所需尺寸**（如 800x480），解码 25ms→~8ms，推流仍用原始 720p MJPEG 零编码。帧池已为 720p RGB 的零拷贝搬运铺好路。
2. **缓存缩放结果**：`setScaledContents` 每帧重缩放 720p→800x480 太贵，先 `img.scaled` 一次缓存，只在分辨率变化时重算。
3. **拷贝裁剪**：显示只需 800x480，采集是 1280x720——可在解码时用 scale_denom 直接缩到显示尺寸，省掉一次全尺寸 RGB 拷贝（帧池槽可直接装缩小后的 RGB）。
4. **内存**：帧池已消除显示链路的 RGB 深拷贝（`setFrameShared` + 浅引用）；`g_state.frameData`（JPEG）+ 帧池 RGB 槽（2 槽 × 2.76MB）≈ 6MB，需评估是否缩 RGB 槽尺寸。

**核心洞察**：升分辨率不是"参数改大"，而是每帧预算 ×3 的连锁反应——先动"最贵的路径"（解码/缩放），再谈显示。**帧池零拷贝已经消除了"搬运"这个维度，剩下要攻的是"解码计算"这个维度**。

### 场景 B：显示从 Qt 换成 SDL / 裸 framebuffer

**变化点**：渲染后端整个替换。

**重构方案**：
1. 业务侧（main.cpp）：**几乎不动**——回调是纯 C++，`displayTimer` 也可保留（QTimer 换成 std::thread + sleep 或 timerfd）。
2. display 内部：定义 `IDisplaySink` 接口（`present(FrameBuffer)` / `onCapture` 等回调注册），Qt 实现只是一个适配器。SDL 实现：`SDL_CreateWindow` + `SDL_UpdateTexture` + `SDL_RenderCopy`；裸 FB 实现：mmap `/dev/fb0` + RGB565 转换 + 自写命中检测。
3. **触摸**：Qt 有 QMouseEvent，裸方案要自己读 `/dev/input` + 手势识别——这是最大的重写成本。
4. **结论**：回调边界保住了"业务逻辑复用"，损失的是 Qt 全家桶（布局/字体/控件）。这也反证了**当初若抽象 `IDisplaySink`，现在换后端只是加一个实现类**。

### 场景 C：30fps → 60fps 显示

**变化点**：显示周期 33ms → 16.6ms，每帧显示预算减半。

**重构方案**：
1. 定时器间隔改 16ms，但 **解码/拷贝必须跟上**——720p 下 60fps 软解码不可行，需 scale_denom 低分辨率解码或 PXP；
2. 双定时器相位不同步在 16ms 周期下更明显，建议合并为单定时器（采集→渲染一个循环）；帧池 `acquire` 池满丢帧提供了天然反压，即使解码跟不上也不阻塞采集；
3. 节流：`displayTimer->setInterval(std::max(10, 1000/fps))` 已有 10ms 保底，60fps=16ms 可行；
4. 注意 LCD 面板本身是否支持 60fps 刷新（linuxfb 下 `FBIO_WAITFORVSYNC`）。

## 3.3 设计模式评估与改进建议

| 模式 | 现状 | 评估 |
|------|------|------|
| 观察者 | Qt 信号槽（`clicked`→slot） | 标准观察者，Qt 原生实现 |
| 依赖倒置 | `std::function` 回调注入 | 优质实践：display 依赖抽象不依赖实现 |
| 适配器/策略 | 无 `IDisplaySink` 抽象 | 换渲染后端需全重写，建议引入 |
| 生产者-消费者 | 采集线程 → g_state → displayTimer → 帧池槽 | 显式实现：共享状态 + 锁 + 拉模式 + 帧池引用计数双缓冲 |
| 状态 | 录像按钮 toggle（`m_isRecording`） | 隐式状态，靠标志位 |
| 策略 | `frameToQImage` 按格式 switch | 隐式策略，新格式要改 switch |
| 模板方法 | `refreshFrame` 固定流程 | 隐式模板方法 |

**改进建议（面试给出"批判 + 方案"）**：

1. **引入 `IDisplaySink`**：`virtual void present(const FrameBuffer&) = 0;`。Qt/SDL/裸 FB 各一个实现类，"显示后端"变成可插拔策略。
2. **低分辨率显示解码（实测验证方向，替代"解码移出 GUI 线程"）**：早期以为"解码移出 GUI 线程"能提速，实测单核 i.MX6ULL 上独立解码线程**反而更卡**（线程无法并行 + 多引入拷贝/切换开销，已回退）。正确方向是**用 `scale_denom` 只解码显示所需尺寸**（显示解码 25ms→~8ms），推流仍用原始 MJPEG 零编码。**单核上"少计算 + 少拷贝"比"多线程"更有效**。
3. **`FormatConverter` 策略表**：`std::unordered_map<PixelFormat, Converter>` 注册新格式，消除 switch 分支。
4. **`GestureRecognizer` 抽象**：滑动手势识别从 gallery 内抽离，便于扩展双击/长按。
5. **状态机替代标志位**：`m_isRecording`/`m_selectMode` 等 bool 可演化为枚举状态机，防非法状态组合。
6. **缩略图控件复用池**：用 `QListView` delegate 或控件池替代"每图 3 控件"，降内存。

**权衡**：当前代码"功能完备、耦合最小化"，且已用帧池消除显示链路深拷贝、验证了瓶颈；但抽象不足导致换后端成本高、解码仍占用 GUI 主线程。面试能说出"回调边界已解耦、渲染后端缺适配器抽象、单核上瓶颈在解码、方向是低分辨率解码而非多线程"就是很好的系统观。

## 3.4 面试「一句话总结」

> "display 模块是系统的'人机界面'，我围绕三个原则设计：**解耦**（业务通过 `std::function` 回调注入，display 不知道 camera/network 的实现，帧数据走 `setFrameShared(FrameSlot*)` 共享契约，换渲染后端只需换适配层）、**节流防堆积 + 帧池零拷贝**（采集线程随意推、GUI 用 33ms QTimer 拉最新帧，锁内只拷贝、解码在锁外；解码结果写入帧池槽、GUI 用 QImage 浅引用上屏，消除显示链路 RGB 深拷贝，实测拷贝 10→0.5 MB/s）、**低配适配**（MJPEG 用 libjpeg 缩放解码 `scale_denom` 与自定义错误处理器，YUYV 用 NEON 转换，相册缩略图只解码可见尺寸，OSD 用独立控件避免与高频视频帧耦合，AVI 播放手写解析器免 ffmpeg）。触摸走 linuxfb+evdev，Qt 内部 mmap `/dev/fb0` 但应用代码不碰它。
>
> 我做了一个很有价值的量化分析：帧池把拷贝降了 95%，但帧率没变；我又排除了解码、渲染，甚至把渲染全去掉、CPU 降到 67% 帧率还是 10fps——**最终用 `v4l2-ctl` 直测确认瓶颈是摄像头硬件实际输出 10fps**（不是拷贝、不是解码），而且单核上独立解码线程反而更卡（已回退）。这段经历让我明白：**单核性能排查要先确认硬件供给能力，再优化应用层**。如果让我重构，我会用低分辨率显示解码（换 30fps 摄像头后 CPU 成为瓶颈时）、抽象 `IDisplaySink` 渲染后端接口、把相机参数变更收敛成独立的 CameraController，让显示面更纯粹。"

---

# 附：display 模块速查表（面试前 5 分钟过一遍）

| 主题 | 一句话答案 |
|------|-----------|
| 为什么选 Qt | 嵌入式 GUI 成熟框架，linuxfb 后端直接 mmap /dev/fb0，免写按钮/布局/字体 |
| Qt 怎么上屏 | QPainter → backing store（软件双缓冲）→ mmap framebuffer → LCD 控制器 |
| 触摸怎么来 | gt9147 → evdev → linuxfb 插件 → QMouseEvent，应用不直接读 /dev/input |
| 为什么不直接用信号连接 | 回调更轻量、可捕获局部变量、业务解耦；信号用于外部监听 |
| 怎么防 UI 堆积 | QTimer 拉模式，33ms 取最新帧，锁内只拷贝、锁外解码 |
| 两个定时器是什么 | displayTimer（main 借槽+解码入槽+publish+setFrameShared）+ m_refreshTimer（GUI 浅引用上屏） |
| 为什么 setFrame 要深拷贝 | g_state.frameData 在采集线程可能 realloc，浅存会悬垂（旧路径） |
| 帧池路径为什么不拷 | setFrameShared 持有槽引用 + QImage 浅引用，RGB24 零深拷贝（拷贝 10→0.5MB/s） |
| 为什么 QImage 要 .copy() | QImage 浅引用外部缓冲，setPixmap 生命周期更长（仅旧 frameToQImage 路径） |
| 怎么防 MJPEG 坏帧崩程序 | libjpeg 自定义 error_exit + longjmp，坏帧跳过不退出 |
| 显示解码在哪个线程 | 单核上解码留在 GUI 主线程（displayTimer）；独立解码线程实测更卡已回退 |
| 真正瓶颈是啥 | 实测为**摄像头硬件实际输出 10fps**（v4l2-ctl 直测）；解码/渲染只是 CPU 占用，不是帧率瓶颈 |
| 判定瓶颈方法论 | CPU 有空闲但帧率提不上去 → 瓶颈在供给端（硬件）；能力列表 ≠ 实际输出，须 v4l2-ctl 直测 |
| 怎么加速缩略图 | libjpeg scale_denom=2/4/8，逆 DCT 直接跳高频，省内存省 CPU |
| 滑块拖动为什么防抖 | 帧率变更要停流重启（几十 ms），不防抖会反复打断采集 |
| OSD 用什么画 | 独立 QLabel/控件最省（脏矩形局部重绘），别每帧合成进视频 |
| linuxfb 透明窗口 | 无合成器，半透明支持弱，用不透明块 + 小面积 |
| 触摸偏移怎么查 | evtest 看原始坐标 → 检查 absinfo 分辨率 → 设备树镜像/swap 配置 |
| 花屏/撕裂排查 | 静态 vs 动态二分 → 格式匹配 → vsync → PXP 叠加 |
| 页面切换怎么做 | QStackedWidget 三层嵌套（预览↔相册↔全屏↔视频），非当前页不绘制 |
| 切页前先做什么 | stopVideoPlayback()——防后台 QTimer 解码浪费 CPU |
| AVI 播放为什么不用 ffmpeg | 自产格式简单 + 免体积负担；idx1 索引让 seek O(1) |
| 开机自启 | systemd service：ConditionPathExists=/dev/video0 + Restart=on-failure |
| 换 SDL/裸 FB 要改多少 | 业务侧几乎不动（回调是纯 C++），display 内部全重写 |
| 最大性能短板 | 摄像头硬件 10fps（改代码无法突破）；linuxfb 渲染热点 QColorProfile::fromSRgb 占 ~48% CPU |
| 帧池零拷贝是啥 | displayTimer 解码直写池槽 + setFrameShared 浅引用，显示链路拷贝 -95% |
| 未用 PXP | 诚实点：NEON 已达标；PXP 适合 YUV→RGB/叠加/缩放，不加速 JPEG |

---

# 补充：面试高频追问深度展开

## 补充 1：Qt 信号槽的三种连接类型（Direct / Queued / Auto）

**考点**：display 模块到处用 connect，面试官很可能追问底层机制。

| 类型 | 触发方式 | 适用 |
|------|----------|------|
| `Qt::DirectConnection` | 发射线程**同步**调用槽 | 同线程对象间 |
| `Qt::QueuedConnection` | 打包成事件投递到接收者线程队列，**异步** | 跨线程安全通信 |
| `Qt::AutoConnection`（默认） | 同线程=Direct，跨线程=Queued | 大多数场景 |

**关键**：跨线程信号默认 Queued，意味着槽在接收对象所在线程的**下一个事件循环**执行——不是立刻执行。所以"采集线程 emit 帧到达"不会立刻刷新 UI，这也是本项目改用拉模式的深层原因之一。

**追问自测**：
- Q1：Direct 连接在发射线程执行槽，如果槽很重会怎样？→ 发射方阻塞，可能卡住采集线程。
- Q2：Queued 连接的参数怎么传递？→ 必须是 Qt 元类型（Q_DECLARE_METATYPE），跨线程传 `Resolution` 等自定义结构需注册。
- Q3：为什么本项目 `captureClicked` 等信号在同线程（都是 GUI 线程 emit + slot）？→ 默认 Auto=Direct，同步执行，无排队延迟。
- Q4：跨线程每帧 emit 一个帧事件，事件队列会无限堆积吗？→ 不一定。堆积的边界条件是**生产速率 λ > 消费速率 μ**（采集帧率超过 GUI 线程处理事件的能力，类似 M/M/1 排队模型）。慢速采集（λ < μ）下队列稳定、延迟有界，**不会堆积**；只有"快速采集 + 慢显示"（如 60fps 采集、GUI 解码 25ms/帧，μ≈40fps）时 λ > μ，队列才会无限增长。这正是"拉模式防堆积"的真正适用范围——它通过"只取最新帧、覆盖中间帧"把延迟钉在有界区间，无论 λ 多大都不堆。

## 补充 2：QImage / QPixmap / QLabel 的内存与性能差异

**考点**：`frameToQImage` 返回 QImage、`setPixmap` 接收 QPixmap，为什么要 fromImage 转换？

| 类 | 存储 | 特点 |
|----|------|------|
| QImage | 内存缓冲（像素在进程内存） | 适合像素级读写/编码；可深拷贝 `.copy()` |
| QPixmap | 平台缓冲（linuxfb=内存映射） | 绘制优化；QPainter 直接画；显示专用 |
| QLabel | 控件 | 持有一个 pixmap 并绘制 |

- `QPixmap::fromImage(img)` 会**拷贝像素**（或共享隐式数据，取决于 Qt 版本与格式）。
- 为什么不能直接 `setPixmap(QPixmap::fromImage(img))` 后就丢 img？→ fromImage 后 img 生命周期可结束（拷贝已完成），但为保险 `.copy()` 深拷贝。
- 性能要点：频繁从 QImage→QPixmap 是拷贝热点；若格式可对齐（RGB565↔Format_RGB16），linuxfb 上可减少转换。

**追问自测**：
- Q1：QImage 的 `.copy()` 什么时候必须调？→ 外部缓冲（`rgb.data()`）生命周期短于 QImage 时。
- Q2：为什么 Mock 模式直接用 RGB24 QImage 而不转 RGB565？→ PC 显示是 RGB888，真机 linuxfb 才可能 565——用 RGB24 通用。

## 补充 3：内存占用预算（面试手算）

在 512MB i.MX6ULL 上，display 相关的常驻内存：

| 项 | 大小 | 说明 |
|----|------|------|
| linuxfb backing store | 800x480x2 ≈ 0.8MB | RGB565 离屏缓冲 |
| 视频帧 RGB24 缓冲 | 640x480x3 ≈ 0.92MB | **帧池改造后**：2 个 RGB 池槽 ≈ 1.84MB 预分配（固定、无每帧 realloc）；显示链路不再有 `m_frameBuffer` assign + QImage.copy() 的临时拷贝 |
| 原始帧共享 | g_state.frameData（JPEG ~0.1MB） | displayTimer 短锁拷贝的 raw 临时缓冲 |
| 缩略图（可见 6 张） | 170x120x4x6 ≈ 0.5MB | ARGB32 |
| 相册控件对象 | ~1-2MB | 200 张照片×3 控件 |
| 总 display 占用 | ~5-6MB | 不含 Qt 库本体与系统缓冲；帧池预分配 +0.3MB |

README 说"运行内存（推流）~8MB"——display 占了其中一半多。帧池把"动态分配的 RGB 拷贝"变成"固定 2 槽的常驻内存"，**消除了每帧 realloc 抖动**，内存曲线更平稳。面试能背出这些数字，比空谈"内存优化"有说服力得多。

## 补充 4：花屏 / 撕裂排查完整思路（高频实战题）

**排查路径（按成本从低到高）**：

| 步骤 | 操作 | 判据 |
|------|------|------|
| ① 静态 vs 动态二分 | 暂停采集（Mock 模式）看静态图是否花 | 静态也花 → 渲染/LCD；只有动时花 → 帧同步 |
| ② 帧缓冲格式匹配 | 确认 LCD 是 RGB565（`FBIOPUT_VSCREENINFO`），UI 是否按 RGB888 写 | 格式错会整体偏色/错位 |
| ③ 撕裂 | 快速移动画面看水平错位带 | 是 → 单缓冲刷新竞态 |
| ④ 坏帧 | MJPEG 流丢帧/半帧被显示 | 解码层做 SOI-EOI 完整性校验 |
| ⑤ DMA/内存 | 帧缓冲地址对齐、被其他进程映射 | mmap 冲突 |

**撕裂的本质与解法**：linuxfb 单缓冲时，LCD 控制器在扫描中间改写帧缓冲 → 水平撕裂线。解法：① Qt backing store（软件双缓冲，当前已用）；② `FBIO_WAITFORVSYNC` 等 vsync 再 flush；③ 上 PXP 硬件叠加/缩放（i.MX6ULL 有 PXP，**本项目未用**——诚实点 + 可扩展点）。

**PXP 追问应答**：诚实回答项目未用 PXP。原因：PXP 主要价值是 YUV→RGB 转换、旋转/缩放/叠加，需初始化 `imx_pxp` 驱动 + DMA 同步，本项目靠 NEON 软转（~5ms）已达标，引入复杂度收益不成比例。若用，最佳落点：① 显示前 YUYV→RGB 交 PXP（释放 CPU 给编码）；② OSD 两层合成；③ 大分辨率缩放。注意 PXP 不加速 JPEG 解码。"知道有硬件、知道为什么不用、知道怎么用"三层次。

---

# 补充：CameraControl 与 Format 的区别

> 定位：display 模块中两个容易混淆的回调/概念。**CameraControl 调 sensor 图像参数（亮度/白平衡/曝光）**，**Format 改像素格式（YUYV/MJPEG）**——两者底层走完全不同的 V4L2 ioctl 路径，GUI 侧对应不同的回调与流程。

## 1. 一句话区别

- **CameraControl（相机控制）** = 调整**传感器图像参数**（亮度/对比度/白平衡/曝光），通过 V4L2 控件（`V4L2_CID_*`）设置，**改完即时生效，无需重启采集流**。
- **Format（像素格式）** = 决定摄像头**输出的数据编码**（YUYV 原始 / MJPEG 压缩），通过 `VIDIOC_S_FMT` 设置，**改完必须停流重启**。

## 2. 代码层面的对应关系

| 维度 | CameraControl | Format |
|------|---------------|--------|
| 回调类型 | `CallbackCameraControl = std::function<void(int cid, int value)>` | `CallbackFormat = std::function<void(PixelFormat)>` |
| GUI 注入 | `onCameraControlChanged(...)` | `onFormatChanged(...)` |
| 底层 V4L2 ioctl | `VIDIOC_S_CTRL` / `VIDIOC_G_CTRL` / `VIDIOC_QUERYCTRL` | `VIDIOC_S_FMT` / `VIDIOC_ENUM_FMT` |
| 具体项 | 亮度、对比度、白平衡、色温、曝光 | YUYV、MJPEG |
| 修改后动作 | 直接 `setControl` 即时生效 | 暂停采集 → 停流 → `setFormat` → 重启流 |

## 3. GUI 中的调用差异

**CameraControl**（`gui.cpp` 滑块拖动时直接回调）：

```cpp
void CameraGUI::onBrightnessChanged(int value) {
    m_brightnessValue->setText(QString::number(value));
    if (m_onCameraControl) {
        m_onCameraControl(static_cast<int>(CameraCapture::V4L2_CID_BRIGHTNESS), value);
    }
}
```

main.cpp 收到后就是一行 `capture->setControl(cid, value)`，驱动实时调整，**采集流不受影响**：

```cpp
gui.onCameraControlChanged([capture](int cid, int value) {
    capture->setControl(cid, value);   // 即改即生效
});
```

**Format**（下拉框切换时需要完整重启流程，`main.cpp:961`）：

```cpp
gui.onFormatChanged([capture, device](PixelFormat fmt) {
    uint32_t v4l2fmt = (fmt == PixelFormat::FMT_YUYV)
                           ? CameraCapture::V4L2_PIX_FMT_YUYV
                           : CameraCapture::V4L2_PIX_FMT_MJPEG;
    g_state.paused = true;              // ① 暂停采集线程
    // ② 等采集线程确认暂停
    capture->stopCapture();             // ③ 停流
    capture->setFormat(640, 480, v4l2fmt);  // ④ 改格式
    capture->startCapture();            // ⑤ 重启
    g_state.paused = false;
});
```

## 4. 为什么 Format 切换要重启而 Control 不用？

1. **Format 影响缓冲区分配**：YUYV 帧是 `w*h*2` 字节、MJPEG 帧是压缩后变长字节，缓冲池大小/格式不同，必须先 `REQBUFS` 重来；
2. **V4L2 规范**：`VIDIOC_S_FMT` 在 STREAMON 状态下返回 `EBUSY`，必须先 `STREAMOFF`；
3. **CameraControl 是"参数寄存器"**：直接写 sensor 内部寄存器（增益、曝光时间），DMA 管线无需重启。

## 5. 一个容易混淆的点

`V4L2_CID_EXPOSURE_*`（曝光）虽然名叫"Control"，但调整曝光会影响帧率（曝光时间长了帧率掉），所以 GUI 里曝光滑块**不走防抖、直接回调**——而 FrameRate 变更却要走"停流重启"（因为 `VIDIOC_S_PARM` 也是流状态敏感）。这说明 V4L2 里"控件"和"流参数"是两套体系，不能只看名字。

## 6. 面试一句话总结

> "CameraControl 走 `VIDIOC_S_CTRL`，是调 sensor 的图像参数（亮度/白平衡/曝光），写寄存器即时生效、不断流；Format 走 `VIDIOC_S_FMT`，是改像素格式（YUYV/MJPEG），因为要重新分配缓冲池且 S_FMT 在 STREAMON 下会 EBUSY，所以必须先停流再重启。GUI 层对应两个回调：`onCameraControlChanged(cid, value)` 即时透传，`onFormatChanged(fmt)` 触发完整的暂停→停流→设置→重启握手。"

---

# 补充：MJPEG 直出模式下的解码路径（推流/录像 vs 本地显示）

> 定位：display 模块的 MJPEG 解码（`decodeMjpegToRgb`）常被面试官追问"MJPEG 直出是不是就完全不用解码了？"——答案要按**数据链路**区分。

## 1. 一句话回答

**MJPEG 直出模式下，推流（HTTP/RTSP）和录像完全不需要解码（JPEG 字节流原样转发/写盘，零拷贝零处理）；但本地屏幕显示必须解码**——屏幕是像素设备，JPEG 是压缩流，`displayTimer` 内 `VideoProcessor::decodeJPEGtoRGB` 必然做 JPEG→RGB（libjpeg-turbo，~25ms），结果直写帧池槽（旧路径 `frameToQImage` 的 `FMT_MJPEG` 分支仍有 `decodeMjpegToRgb`，仅作无 `m_heldSlot` 时兜底）。

## 2. 两条链路的代码证据

**链路 A：采集 → 处理线程 → 推流/录像（无需解码）**

`main.cpp` 处理线程（835-879 行）的核心判断：

```cpp
// YUYV → JPEG 编码（CPU 密集，不阻塞采集线程）
bool needEncode = (localFmt == PixelFormat::FMT_YUYV) && (mjpegServerOk || rtspServer);
...
if (needEncode) { VideoProcessor::encodeYUYVtoJPEG(...); }  // 仅 YUYV 才编码

// 推流到 MJPEG HTTP 服务器
if (mjpegServerOk) {
    if (localFmt == PixelFormat::FMT_MJPEG) {
        mjpegServer->updateFrame(localFrame.data(), ...);    // MJPEG 直通，不解码
    }
}
...
// 录像写入（仅 MJPEG 模式支持）
if (g_recording && localFmt == PixelFormat::FMT_MJPEG && g_storage) {
    g_storage->writeRecordFrame(localFrame.data(), ...);     // JPEG 直接进 AVI
}
```

- HTTP：`multipart/x-mixed-replace` 直接把 JPEG 原样发给浏览器；
- RTSP：RTP 按 RFC 2435 把 JPEG 分片，**无需解码也无需再编码**；
- 录像：JPEG 帧直接写进 AVI 的 `00dc` chunk（且**录像只支持 MJPEG 模式**，YUYV 不录像）。

**链路 B：采集 → GUI 显示（必须解码）**

`main.cpp` `displayTimer` 内用 `VideoProcessor::decodeJPEGtoRGB`（帧池路径，解码结果直写池槽）：

```cpp
// displayTimer timeout 内
if (srcFmt == PixelFormat::FMT_MJPEG) {
    if (!VideoProcessor::decodeJPEGtoRGB(raw.data(), raw.size(), slot->data, dw, dh))
        { g_rgbPool->release(slot); return; }   // 坏帧丢帧
    slot->width = dw; slot->height = dh;
}
g_rgbPool->publish(slot);                       // 发布 → GUI setFrameShared 浅引用上屏
```

（旧路径 `frameToQImage` 的 `FMT_MJPEG` 分支仍有 `decodeMjpegToRgb`，仅作为无 `m_heldSlot` 时的兜底。）

## 3. 两种模式的真实开销对比

| 环节 | MJPEG 模式 | YUYV 模式 |
|------|-----------|-----------|
| 处理线程（推流/录像用） | **0**（直通） | 编码 JPEG ~25ms/帧 |
| GUI 显示 | 解码 JPEG ~25ms/帧 | YUV→RGB ~5ms/帧 |
| 录像 | 支持（直写） | 不支持 |

**反直觉点**：MJPEG 模式下本地显示的解码（~25ms）反而比 YUYV 模式的显示转换（~5ms）更贵。MJPEG 直出的优势**不在省解码**——解码一样存在——而在**省掉了处理线程那 25ms 的编码**，且推流/存储零拷贝。README 说"MJPEG 硬件输出 <1ms、零 CPU 编码开销"指的是**推流路径**，不是显示路径。

## 4. 面试追问与应答

**Q1：MJPEG 直出是不是完全不用 CPU 处理了？**
**A**：不是。网络推流和录像确实零处理（JPEG 直通），但本地预览每帧仍要 libjpeg 解码一次（~25ms）才能上屏。CPU 开销从"编码"转移成了"显示解码"，总量仍然比 YUYV（编码 25ms + 显示转换 5ms）少，但绝不是零。

**Q2：为什么 YUYV 模式不能录像？**
**A**：AVI 容器里存的是 MJPEG 帧（`00dc` 压缩 chunk）。YUYV 是未压缩原始格式，要录像就得先编码成 JPEG，而 YUYV 模式下的 JPEG 编码结果只用于推流，录像代码干脆只在 `FMT_MJPEG` 时写盘（`main.cpp:876`）——既避免重复编码，也保证 AVI 格式统一。

**Q3：既然显示解码 25ms 在 GUI 线程，MJPEG 模式为什么还能跑 30fps？**
**A**：640x480 的 MJPEG 帧通常小于 100KB，解码耗时往往低于 25ms 的理论值，33ms 定时器有富余；且坏帧被静默跳过。**但实测 CPU 99% 打满、帧率卡 10fps——最终 v4l2-ctl 确认根因是摄像头硬件实际输出 10fps**（解码/渲染只是 CPU 占用，不是帧率上限，见 §3.2 场景 D）。升 720p 解码会超 33ms，应走"低分辨率显示解码"（scale_denom 只解显示尺寸）省 CPU，而非"解码移出 GUI 线程"（见 3.2 场景 A）。

**Q4：如果让处理线程解码成 RGB 再推给 GUI，能省掉显示解码吗？**
**A**：能省掉 GUI 线程的解码，但**总量不省**——解码还是要做一次，只是从 GUI 线程挪到处理线程（还多一次 RGB 数据的跨线程拷贝）。**且实测单核 i.MX6ULL 上独立解码线程反而更卡**（线程无法并行，多引入拷贝+切换开销，曾实现后回退）。真正的瓶颈是"解码计算"本身，不是"在哪个线程"。注意推流路径要的是 JPEG，处理线程解码成 RGB 反而要再编码回去。**正确方向是**：推流保持 JPEG 直通，显示路径用 `scale_denom` 低分辨率解码（25ms→~8ms），既降计算又不引入线程切换开销。
