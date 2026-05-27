# 帧率控制 — 运行时可调帧率

> 发布日期: 2026-05-27  
> 关联模块: CameraCapture, CameraGUI, RTSPServer, main

---

## 1. 功能概述

为 SmartCam 增加**运行时动态调整帧率**的功能。在设置弹窗的 "Camera Controls" 分组中新增 Framerate 滑块，用户拖动即可实时调整 V4L2 摄像头采集帧率，同时自动同步 RTSP 流媒体服务器和 GUI 显示定时器。

### 1.1 核心功能

| 功能 | 说明 |
|------|------|
| 帧率滑块 | 横向滑块，范围由 V4L2 设备支持的帧率区间自动确定 |
| 范围自动探测 | 通过 `VIDIOC_ENUM_FRAMEINTERVALS` 枚举设备支持的帧率；若不支持则回退 1~60fps |
| V4L2 硬件写入 | 滑块变化 → `VIDIOC_S_PARM` 设置 `timeperframe` 参数，下一帧生效 |
| RTSP 自动同步 | 帧率变更后调用 `rtspServer->setStreamInfo()` 更新 SDP 中的 `a=framerate` 和 RTP 时间戳间隔 |
| GUI 定时器联动 | 帧率变更后动态调整 `displayTimer->setInterval()`，避免低帧率时 CPU 空转、高帧率时刷新不足 |
| 重置默认值 | Reset Defaults 按钮同时恢复帧率到 V4L2 驱动默认值 |
| 范围限制 | 硬性上下限 1~120fps；设备枚举范围或安全回退范围（1~60fps） |

### 1.2 设计理念

- **非固定帧率**：项目中 FPS 原本有三层含义（采集动态测量、GUI 刷新固定 33ms、RTSP 硬编码 30），本次实现将它们统一到用户可控的帧率参数上。
- **安全范围约束**：最大 120fps 避免嵌入式硬件负载过高；最小 1fps 保证系统不会挂死。
- **无需重启**：通过 `VIDIOC_S_PARM` 直接写入 V4L2 驱动，采集线程不需要停止/重启，下一帧即按新帧率产出。

---

## 2. 交互流程

```
初始化阶段（main.cpp）
    │
    ├─ capture->getFramerate(curNum, curDen)
    │    └─ VIDIOC_G_PARM → 读取当前帧率
    │
    ├─ capture->enumFrameRates(pixfmt, w, h, supportedFps)
    │    ├─ 成功 → minFps=vector.front(), maxFps=vector.back()
    │    └─ 失败 → minFps=1, maxFps=60 (安全回退)
    │
    └─ gui.setFramerateRange(minFps, maxFps, currentFps)
         └─ 滑块范围 = [minFps, maxFps]

运行时阶段
    │
用户拖动 Framerate 滑块
    │
    ▼
CameraGUI::onFramerateSliderChanged(value)        [gui.cpp]
    │
    ├─ m_framerateValue->setText("XX fps")         ← 更新右侧标签
    ├─ m_framerateInfo.current = value              ← 记录当前值
    └─ m_onFramerate(value)                         ← 触发回调
        │
        ▼
lambda [capture, rtspServer, &displayTimer](int fps)   [main.cpp]
    │
    ├─ (1) capture->setFramerate(1, fps)
    │    └─ VIDIOC_S_PARM 设置 timeperframe
    │
    ├─ (2) rtspServer->setStreamInfo(w, h, fps)
    │    └─ 更新 SDP a=framerate + RTP m_tsPerFrame
    │
    └─ (3) displayTimer->setInterval(1000/fps)
         └─ clamp 到 [10ms, 100ms] → [100fps, 10fps]
```

---

## 3. 修改文件清单

| 文件 | 修改类型 | 说明 |
|------|----------|------|
| `include/camera/capture.h` | **修改** | 新增 `getFramerate()` 和 `enumFrameRates()` 方法声明 |
| `src/camera/capture.cpp` | **修改** | 实现 `getFramerate()` 和 `enumFrameRates()`；新增 `#include <algorithm>` |
| `include/display/gui.h` | **修改** | 新增帧率 UI 控件声明、回调类型、范围设置接口、slot 声明 |
| `src/display/gui.cpp` | **修改** | 在设置对话框新增帧率滑块行；实现范围设置方法、槽函数、更新重置逻辑 |
| `src/main.cpp` | **修改** | 新增帧率查询逻辑 + 帧率回调注册；RTSP 初始化使用实际帧率 |

---

## 4. 详细代码变更

### 4.1 `include/camera/capture.h` — 新增 2 个 API 声明

**位置**: 在 `setFramerate()` 声明之后（CameraCapture 类的 public 区）。

```cpp
/**
 * @brief 获取当前帧率设置
 * @param numerator   输出：分子
 * @param denominator 输出：分母
 * @return 0 成功，负值表示设备不支持
 */
int getFramerate(int& numerator, int& denominator);

/**
 * @brief 枚举当前格式/分辨率下支持的帧率
 * @param pixfmt       像素格式 (V4L2 FOURCC)
 * @param width        宽度
 * @param height       高度
 * @param frameRates   输出：支持的帧率列表 (fps 整数值)
 * @return 0 成功，负值无可用帧率
 */
int enumFrameRates(uint32_t pixfmt, int width, int height,
                   std::vector<int>& frameRates);
```

**说明**:
- `getFramerate()`: 通过 `VIDIOC_G_PARM` 读取 V4L2 驱动的 `timeperframe` 参数，返回分数字段 `numerator / denominator`（即每帧间隔 = numerator/denominator 秒，帧率 = denominator/numerator）。
- `enumFrameRates()`: 通过 `VIDIOC_ENUM_FRAMEINTERVALS` 枚举当前格式和分辨率下所有支持的帧间隔。输出为整型 fps 的 `std::vector<int>` 列表（已排序去重）。

---

### 4.2 `src/camera/capture.cpp` — 实现 2 个新 API

#### 4.2.1 新增 `#include <algorithm>`（第 21 行）

```cpp
#include <algorithm>
```

**说明**: `enumFrameRates()` 中对结果列表使用了 `std::sort()` 和 `std::unique()` 进行排序去重。

#### 4.2.2 `getFramerate()` 实现

```cpp
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
```

**机制**: V4L2 `VIDIOC_G_PARM` — 读取当前流参数，其中 `timeperframe` 表示每帧间隔时间。例如 `timeperframe = {1, 30}` 表示每帧 1/30 秒，即 30fps。

#### 4.2.3 `enumFrameRates()` 实现

```cpp
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
            // 离散帧率：如 5, 10, 15, 20, 25, 30 fps
            int fps = static_cast<int>(frmival.discrete.denominator) /
                      static_cast<int>(frmival.discrete.numerator);
            if (fps > 0) {
                frameRates.push_back(fps);
            }
        } else if (frmival.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
            // 步进帧率：如 1~30fps，步长 1
            // 展开为离散列表（最多 20 个避免过多）
            int minFps = static_cast<int>(frmival.stepwise.min.denominator) /
                         static_cast<int>(frmival.stepwise.min.numerator);
            int maxFps = static_cast<int>(frmival.stepwise.max.denominator) /
                         static_cast<int>(frmival.stepwise.max.numerator);
            int stepFps = static_cast<int>(frmival.stepwise.step.denominator) /
                          static_cast<int>(frmival.stepwise.step.numerator);
            if (stepFps <= 0) stepFps = 1;

            int count = 0;
            for (int f = minFps; f <= maxFps && count < 20; f += stepFps) {
                if (f > 0) {
                    frameRates.push_back(f);
                    count++;
                }
            }
            break;
        } else {
            break;
        }
    }

    // 设备不支持枚举帧率 → 回退为通用范围 1~120
    if (frameRates.empty()) {
        LOG_INF("No frame intervals enumerated, using default range 1-120");
        for (int f = 1; f <= 120; f += 1) {
            frameRates.push_back(f);
        }
        return -ENOENT;
    }

    // 排序去重
    std::sort(frameRates.begin(), frameRates.end());
    frameRates.erase(std::unique(frameRates.begin(), frameRates.end()),
                     frameRates.end());

    return 0;
}
```

**V4L2 帧间隔枚举类型**:

| `frmival.type` | 含义 | 处理方式 |
|----------------|------|----------|
| `V4L2_FRMIVAL_TYPE_DISCRETE` | 离散值（如 5, 10, 15, 30 fps） | 逐个添加 |
| `V4L2_FRMIVAL_TYPE_STEPWISE` | 步进范围（如 1~30, step=1） | 展开为离散列表（最多 20 个） |
| 枚举失败 / 空 | 设备不支持 | 返回 `-ENOENT`，调用方使用安全回退范围 |

---

### 4.3 `include/display/gui.h` — 扩展 GUI 类

#### 4.3.1 新增回调类型（第 68 行）

```cpp
using CallbackFramerate = std::function<void(int fps)>;
```

#### 4.3.2 新增公共方法（第 76、85 行）

```cpp
void onFramerateChanged(CallbackFramerate cb);                // 回调注册
void setFramerateRange(int minFps, int maxFps, int currentFps); // 范围设置
```

#### 4.3.3 新增 private slot（第 114 行）

```cpp
void onFramerateSliderChanged(int value);   // 帧率滑块槽函数
```

> **命名说明**: 由于 `onFramerateChanged` 已用作回调注册方法名（与 `CallbakFramerate` 绑定），slot 必须使用不同名字避免 Qt `connect` 模板推导歧义。命名为 `onFramerateSliderChanged` 以明确其与滑块控件的绑定关系。

#### 4.3.4 新增私有成员变量

**UI 控件**（第 157~158 行）:
```cpp
QSlider*     m_framerateSlider  = nullptr;   // 帧率横向滑块
QLabel*      m_framerateValue   = nullptr;   // 帧率数值标签 "XX fps"
```

**数据成员**（第 177~178、187 行）:
```cpp
ControlInfo m_framerateInfo;             // min/max/step/def/current (复用 ControlInfo 结构体)
int         m_framerateDefault = 30;      // V4L2 驱动默认帧率
CallbackFramerate m_onFramerate;          // 帧率变更回调
```

---

### 4.4 `src/display/gui.cpp` — 帧率 UI 实现

#### 4.4.1 `buildSettingsDialog()` 中新增帧率行（在相机控制分组末尾）

**位置**: 在 "WB Temp" 行之后、"mainLayout->addWidget(camGroup)" 之前。

```cpp
// 帧率
auto* fpsRow = new QHBoxLayout();
auto* fpsLabel = new QLabel(QStringLiteral("Framerate:"), camGroup);
fpsLabel->setFixedWidth(100);
m_framerateSlider = new QSlider(Qt::Horizontal, camGroup);
m_framerateSlider->setRange(1, 120);          // 初始占位范围
m_framerateSlider->setValue(30);              // 初始占位值
m_framerateSlider->setSingleStep(1);          // 步长 1
m_framerateSlider->setPageStep(5);            // 翻页步长 5
m_framerateSlider->setStyleSheet(sliderStyle);
m_framerateValue = new QLabel(QStringLiteral("30 fps"), camGroup);
m_framerateValue->setFixedWidth(60);
m_framerateValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
fpsRow->addWidget(fpsLabel);
fpsRow->addWidget(m_framerateSlider, 1);
fpsRow->addWidget(m_framerateValue);
camLayout->addLayout(fpsRow);
```

**布局**: 与其他相机控制行格式一致 — 100px 固定宽标签 + 自适应滑块 + 60px 数值标签（显示 "XX fps"）。

#### 4.4.2 `connectSignals()` 中新增信号连接

```cpp
connect(m_framerateSlider, QOverload<int>::of(&QSlider::valueChanged),
        this, &CameraGUI::onFramerateSliderChanged);
```

**说明**: `QSlider::valueChanged` 有 `int` 和 `QString` 两个重载，必须使用 `QOverload<int>::of()` 消歧义。

#### 4.4.3 `setFramerateRange()` 范围设置方法

```cpp
void CameraGUI::setFramerateRange(int minFps, int maxFps, int currentFps) {
    // 限制合理范围：最小 1fps，最大 120fps
    minFps = std::max(1, minFps);
    maxFps = std::min(120, maxFps);
    if (maxFps < minFps) maxFps = minFps;
    currentFps = std::max(minFps, std::min(maxFps, currentFps));

    m_framerateInfo = {minFps, maxFps, 1, currentFps, currentFps};
    m_framerateDefault = currentFps;

    m_framerateSlider->blockSignals(true);
    m_framerateSlider->setRange(minFps, maxFps);
    m_framerateSlider->setSingleStep(1);
    m_framerateSlider->setPageStep(5);
    m_framerateSlider->setValue(currentFps);
    m_framerateSlider->blockSignals(false);
    m_framerateValue->setText(QString("%1 fps").arg(currentFps));
}
```

**安全边界**:
| 场景 | 最终范围 |
|------|----------|
| V4L2 枚举成功且支持 5~30fps | `[5, 30]` |
| V4L2 枚举失败 | `[1, 60]`（安全回退） |
| V4L2 返回异常大值如 min=1 max=1000 | `[1, 120]`（硬上限截断） |
| `currentFps` 超出范围 | clamp 到 `[minFps, maxFps]` |

#### 4.4.4 `onFramerateSliderChanged()` 槽函数

```cpp
void CameraGUI::onFramerateSliderChanged(int value) {
    m_framerateValue->setText(QString("%1 fps").arg(value));
    m_framerateInfo.current = value;
    if (m_onFramerate) {
        m_onFramerate(value);
    }
    qDebug() << "[GUI] Framerate changed:" << value;
}
```

#### 4.4.5 `onResetDefaults()` 中新增帧率恢复逻辑

```cpp
// 恢复帧率
m_framerateSlider->setValue(m_framerateInfo.def);
m_framerateValue->setText(QString("%1 fps").arg(m_framerateInfo.def));
m_framerateInfo.current = m_framerateInfo.def;
if (m_onFramerate) {
    m_onFramerate(m_framerateInfo.def);
}
```

#### 4.4.6 回调注册方法

```cpp
void CameraGUI::onFramerateChanged(CallbackFramerate cb) {
    m_onFramerate = std::move(cb);
}
```

---

### 4.5 `src/main.cpp` — 帧率查询与回调集成

#### 4.5.1 初始化阶段：查询 V4L2 帧率范围

**位置**: 在自动白平衡查询之后、`onCameraControlChanged` 回调注册之前。

```cpp
// 帧率 — 查询 V4L2 支持的帧率范围
{
    int curNum = 1, curDen = 30;
    capture->getFramerate(curNum, curDen);
    int currentFps = (curNum > 0) ? (curDen / curNum) : 30;

    // 尝试枚举设备支持的帧率
    std::vector<int> supportedFps;
    int enumRet = capture->enumFrameRates(
        capture->getCurrentFormat(),
        curRes.width, curRes.height,
        supportedFps);

    if (enumRet == 0 && !supportedFps.empty()) {
        int minFps = supportedFps.front();
        int maxFps = supportedFps.back();
        // 确保当前帧率在范围内
        if (currentFps < minFps) currentFps = minFps;
        if (currentFps > maxFps) currentFps = maxFps;
        gui.setFramerateRange(minFps, maxFps, currentFps);
        LOG_INF("Framerate: supported=%zu rates, range=[%d, %d], current=%d",
                 supportedFps.size(), minFps, maxFps, currentFps);
    } else {
        // 设备不支持枚举帧率，使用通用安全范围 1~60
        gui.setFramerateRange(1, 60, currentFps);
        LOG_INF("Framerate: enum not supported, using safe range 1-60, current=%d",
                 currentFps);
    }
}
```

**流程**:
1. `getFramerate()` → 读取当前 V4L2 timeperframe，计算 currentFps
2. `enumFrameRates(formats, w, h)` → 枚举设备支持的帧率列表
3. 根据枚举结果设置滑块范围
4. 若 currentFps 不在枚举范围内，clamp 到边界

#### 4.5.2 RTSP 初始化时使用实际帧率（替代硬编码 30）

**修改前**:
```cpp
rtspServer->setStreamInfo(curRes.width, curRes.height,
                          30 /* fps */);
```

**修改后**:
```cpp
// 使用 V4L2 查询到的实际帧率，若无则默认 30
int rtspFps = 30;
{
    int num = 1, den = 30;
    if (capture->getFramerate(num, den) == 0 && num > 0) {
        rtspFps = den / num;
        if (rtspFps <= 0) rtspFps = 30;
    }
}
rtspServer->setStreamInfo(curRes.width, curRes.height,
                          rtspFps);
```

**说明**: RTSP 服务器不再硬编码 `fps=30`，而是从 V4L2 驱动读取实际帧率，保证 SDP 中 `a=framerate` 和 RTP `m_tsPerFrame` 与实际采集帧率一致。

#### 4.5.3 帧率变更回调注册

```cpp
// 注册帧率变更回调：滑块变化 → V4L2 setFramerate + 更新 RTSP + 更新显示定时器
gui.onFramerateChanged([capture, rtspServer, &displayTimer](int fps) {
    if (fps <= 0) return;

    int ret = capture->setFramerate(1, fps);
    if (ret < 0) {
        LOG_WRN("setFramerate(%d) failed (ret=%d)", fps, ret);
    } else {
        LOG_INF("Framerate changed to %d fps", fps);
    }

    // 同步更新 RTSP 服务器的 SDP 和 RTP 时间戳
    if (rtspServer) {
        Resolution res = capture->getCurrentResolution();
        rtspServer->setStreamInfo(res.width, res.height, fps);
        LOG_INF("RTSP stream info updated: %dx%d @ %dfps",
                 res.width, res.height, fps);
    }

    // 更新显示定时器间隔，至少 10ms，最高 100ms (10fps)
    if (displayTimer) {
        int intervalMs = std::max(10, std::min(100, 1000 / fps));
        displayTimer->setInterval(intervalMs);
        LOG_INF("Display timer interval updated to %d ms (target %d fps)",
                 intervalMs, fps);
    }
});
```

**回调触发时执行三步操作**:

| 步骤 | 操作 | V4L2/API | 效果 |
|------|------|----------|------|
| ① | 写入硬件帧率 | `capture->setFramerate(1, fps)` → `VIDIOC_S_PARM` | 摄像头驱动按新帧率输出帧 |
| ② | 更新 RTSP 参数 | `rtspServer->setStreamInfo(w, h, fps)` | SDP `a=framerate` + RTP `m_tsPerFrame = 90000/fps` |
| ③ | 更新显示定时器 | `displayTimer->setInterval(1000/fps)` | GUI 刷新频率匹配采集帧率 |

**displayTimer 间隔范围**:
| 帧率 | 计算间隔 | 实际间隔 | 说明 |
|------|----------|----------|------|
| 5fps | 200ms | **100ms** | 被上限 clamp |
| 10fps | 100ms | 100ms | 上限边界 |
| 30fps | 33ms | 33ms | 正常 |
| 60fps | 16ms | 16ms | 正常 |
| 120fps | 8ms | **10ms** | 被下限 clamp |

**lambda 捕获说明**: `displayTimer` 通过 **引用捕获** `[&displayTimer]`——因为此回调注册于 `displayTimer = new QTimer(&gui)` 创建之前，lambda 定义时 `displayTimer` 为 `nullptr`。引用捕获确保执行时访问到已初始化后的指针。回调执行时 `displayTimer` 已创建并启动。

---

## 5. 架构数据流

```
                     main.cpp (初始化阶段)
┌──────────────────────────────────────────────────────────────┐
│  capture->getFramerate()    ──► VIDIOC_G_PARM  → currentFps │
│  capture->enumFrameRates()  ──► VIDIOC_ENUM_FRAMEINTERVALS  │
│                                      → [minFps, maxFps]      │
│                                                              │
│  gui.setFramerateRange(min, max, cur) ──► 滑块范围设置        │
│  rtspServer->setStreamInfo(w, h, actualFps)                  │
│                                                              │
│  gui.onFramerateChanged(lambda) ──► 注册回调                  │
└──────────────────────────────────────────────────────────────┘


                     main.cpp (运行阶段)
┌──────────────────────────────────────────────────────────────┐
│                        帧率回调触发                            │
│                            │                                 │
│          ┌─────────────────┼─────────────────┐               │
│          ▼                 ▼                  ▼               │
│   CameraCapture      RTSPServer         displayTimer         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐       │
│  │VIDIOC_S_PARM │  │setStreamInfo │  │setInterval   │       │
│  │ timeperframe │  │ SDP + RTP    │  │ 1000/fps ms  │       │
│  │ = 1/fps      │  │ m_tsPerFrame │  │ clamp[10,100]│       │
│  └──────────────┘  └──────────────┘  └──────────────┘       │
└──────────────────────────────────────────────────────────────┘


                     CameraGUI 内部
┌──────────────────────────────────────────────────────────────┐
│  m_framerateSlider ──valueChanged(int)──►                     │
│    onFramerateSliderChanged(value)                            │
│      ├─ m_framerateValue->setText("XX fps")                  │
│      ├─ m_framerateInfo.current = value                      │
│      └─ m_onFramerate(value)  ──────► main.cpp lambda        │
│                                                              │
│  m_btnResetDefaults ──clicked()──► onResetDefaults()         │
│    └─ m_framerateSlider->setValue(m_framerateInfo.def)       │
│       └─ 触发 onFramerateSliderChanged → 回调 → V4L2 写入    │
└──────────────────────────────────────────────────────────────┘
```

---

## 6. 三层帧率统一

> 这是本次实现的核心价值 —— 将项目中原先独立的三层帧率概念统一到用户可控的单一参数上。

| 层级 | 实现前 | 实现后 |
|------|--------|--------|
| **采集帧率** (V4L2) | 动态测量 `updateFPS()` 每 30 帧计算一次 | 用户通过滑块设置 → `VIDIOC_S_PARM` |
| **GUI 刷新** (displayTimer) | 固定 `setInterval(33)` | 帧率变更 → `setInterval(1000/fps)` |
| **RTSP SDP/RTP** | 硬编码 `setStreamInfo(640, 480, 30)` | 初始化读实际帧率 + 帧率变更同步 `setStreamInfo()` |

**注意事项**:
- `CameraCapture::updateFPS()` 仍保留其动态测量机制（每 30 帧计算实际 FPS），用于状态栏显示和录像文件的 FPS 字段。
- `updateFPS()` 测量的值与 `VIDIOC_S_PARM` 设定值可能略有偏差（V4L2 驱动修正、系统抖动等），建议以 `getCurrentFPS()` 返回值作为"实际帧率"参考。

---

## 7. 兼容性考虑

### 7.1 设备不支持枚举帧率（`enumFrameRates` 返回 -ENOENT）

`enumFrameRates()` 内部有完整的安全回退：当 `VIDIOC_ENUM_FRAMEINTERVALS` ioctl 调用失败或返回空列表时，返回 1~120 的完整帧率列表，同时返回 `-ENOENT`。

在 `main.cpp` 中，若 `enumRet != 0` 或 `supportedFps.empty()`，使用 `setFramerateRange(1, 60, currentFps)` 设置安全范围 1~60fps。

### 7.2 设备不支持设置帧率（`setFramerate` 返回负值）

`CameraCapture::setFramerate()` 通过 `VIDIOC_S_PARM` 写入，若 ioctl 失败仅输出 `LOG_WRN` 日志，不会崩溃或中断系统。回调中检查 `ret < 0` 时只做日志警告。

### 7.3 Mock 模式

Mock 模式下 `capture == nullptr`，帧率查询代码块和回调注册在 `if (!device.isEmpty())` 分支内，不会执行。GUI 中帧率滑块保持默认占位值 30fps，拖动不会触发有效回调。

### 7.4 低性能硬件保护

- **上限 120fps**：`setFramerateRange()` 中有 `std::min(120, maxFps)` 硬截断，防止 iMX6ULL 被过高帧率压垮。
- **下线 1fps**：`std::max(1, minFps)` 保证系统不会因帧率为 0 而挂死。
- **displayTimer clamp**: 定时器间隔被限制在 `[10ms, 100ms]`，确保即使帧率被设为 200fps（若未来去除上限），GUI 刷新也不会超过 100fps，避免主线程过度消耗。

---

## 8. 测试建议

### 8.1 PC Mock 模式

```bash
./smartcam -platform xcb
```

**测试点**:
1. 打开 Settings 弹窗 → 确认 "Framerate" 滑块在 Camera Controls 分组末尾
2. 拖动滑块 → 确认右侧标签 "XX fps" 实时更新
3. 点击 Reset Defaults → 确认帧率恢复默认值
4. 关闭弹窗再重新打开 → 确认帧率值保持上次设置

### 8.2 开发板真实摄像头测试

```bash
./smartcam --device /dev/video0 --fmt mjpeg -platform linuxfb
```

**测试点**:
1. 查看日志: `Framerate: supported=XX rates, range=[X, Y], current=Z` 确认枚举成功
2. 从 30fps 调到 15fps → 观察预览画面更新变慢，状态栏 FPS 下降
3. 从 15fps 调到 5fps → 确认画面仍正常（未崩溃）
4. 同时观看 RTSP 流 (`ffplay rtsp://<IP>:8554/stream`) → 确认流畅度与滑块设定一致
5. 点击 Reset Defaults → 确认帧率回到驱动默认值
6. `journalctl -u smartcam -f` 检查是否有 `setFramerate failed` 错误
   - 部分 UVC 摄像头不支持 `VIDIOC_S_PARM`，预期出现日志 `VIDIOC_S_PARM (fps) not supported`

### 8.3 不支持帧率调整的设备

部分廉价 USB UVC 摄像头不支持 `VIDIOC_S_PARM`。此时：
- 日志输出 `VIDIOC_S_PARM (fps) not supported`
- 滑块仍可拖动，数值可变化，但实际采集帧率不变
- 系统不会崩溃

---

## 9. 未来扩展

| 方向 | 描述 |
|------|------|
| 帧率与分辨率联动 | 部分摄像头在不同分辨率下支持不同帧率范围，切换分辨率时重新 `enumFrameRates()` 更新滑块范围 |
| 配置持久化 | 将帧率写入 `smartcam.conf`，下次启动时恢复用户设定 |
| 固定帧率 vs 可变帧率 | 增加"自动帧率"模式，由摄像头根据光照条件动态决定（`V4L2_CID_EXPOSURE_AUTO_APERTURE`） |
| 录像帧率独立 | 允许录像时使用不同于预览的帧率（需要采集两路流或帧重复/跳帧） |
| 丢帧策略 | 当系统负载过高时，自动降帧率并通知用户 |
