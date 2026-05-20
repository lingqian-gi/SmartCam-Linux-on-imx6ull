# 显示与交互模块 — 实现记录

> **编号**：MOD-01  
> **创建日期**：2026-05-20  
> **状态**：✅ 已实现，编译通过  
> **依赖**：Qt5 Widgets、C++17  

---

## 一、模块概述

基于文档 [3.3 显示与交互模块](../求职项目-智能相机流媒体系统.md) 的设计，实现完整的 Qt5 触摸屏 GUI，适配野火 iMX6ULL Pro + 7 寸电容触摸屏。

### 功能清单

| 功能 | 状态 |
|------|------|
| 实时视频预览 (QTimer 33ms ≈ 30fps) | ✅ |
| 拍照按钮 | ✅ |
| 录像按钮 (toggle) | ✅ |
| 分辨率选择 (640x480 / 320x240 / 1280x720) | ✅ |
| 格式选择 (YUV / MJPEG) | ✅ |
| FPS 实时显示 | ✅ |
| 推流状态指示 | ✅ |
| 客户端计数 | ✅ |
| 录像指示 | ✅ |
| YUYV → RGB24 转换 | ✅ |
| YUYV → RGB565 转换 (16-bit LCD) | ✅ |
| Mock 模式 (无硬件可运行) | ✅ |
| 回调接口 (供主程序注入业务逻辑) | ✅ |
| Qt 信号 (供外部监听) | ✅ |

---

## 二、文件清单

```
SmartCam-Linux-on-imx6ull/
├── CMakeLists.txt                  # 构建系统 (x86/ARM 双平台, AUTOMOC)
├── include/
│   ├── common/
│   │   └── types.h                 # 共享类型定义
│   └── display/
│       └── gui.h                   # CameraGUI 类声明 + 颜色转换函数
├── src/
│   ├── display/
│   │   └── gui.cpp                 # CameraGUI 实现 (441 行)
│   └── main.cpp                    # 入口 (Mock 模式 + 回调注入)
├── configs/
│   └── smartcam.conf               # 配置文件
├── scripts/
│   └── build.sh                    # 构建脚本
└── docs/
    └── 01-display-module-implementation.md  # 本文件
```

---

## 三、关键设计决策

### 3.1 为何使用 C++17？

- `inline constexpr` 全局常量需要 C++17 支持
- `constexpr` lambda 在 C++17 可用（用于 YUV 转换的 clip 函数）
- `std::optional` / `std::variant` 等现代特性为后续扩展预留

### 3.2 为何使用 CMake + AUTOMOC 而不是 qmake？

| 对比项 | qmake | CMake + AUTOMOC |
|--------|-------|-----------------|
| 跨平台 | Qt 限定 | 全平台 |
| IDE 支持 | Qt Creator | CLion / VSCode / Qt Creator |
| ARM 交叉编译 | 手动指定 | `CMAKE_TOOLCHAIN_FILE` 一条命令 |
| moc 处理 | 自动 | `set(CMAKE_AUTOMOC ON)` 一条命令 |
| 行业标准 | 遗留 | ✅ 当前主流 |

### 3.3 Deep-Tone 配色方案

适配嵌入式 7 寸屏的暗色主题：

| 元素 | 颜色 | 用途 |
|------|------|------|
| 背景 | `#0a0a1a` | 主背景 |
| 预览区 | `#1a1a2e` + `#0f3460` 边框 | 视频区域 |
| 按钮 | `#0f3460` (拍照) / `#533483` (录像) | 大触控目标 |
| 状态栏 | `#16213e` + 彩色文字 | 信息行 |
| 文字 | `#e0e0e0` / `#c0c0d0` | 高对比度 |

### 3.4 Mock 模式设计

无硬件时自动生成 8 色彩条测试图，帧间横移 2 像素形成流动效果，叠加帧号水印。用于 PC 端独立开发调试 UI 布局与交互动画。

---

## 四、类接口文档

### 4.1 CameraGUI

```cpp
class CameraGUI : public QWidget {
    Q_OBJECT
public:
    explicit CameraGUI(QWidget* parent = nullptr);

    // ---- 数据输入 ----
    void setFrame(const uint8_t* data, int len, int w, int h, PixelFormat fmt);
    void setFPS(double fps);
    void setClientCount(int count);
    void setRecordingStatus(bool recording);
    void setStreamingStatus(bool streaming);

    // ---- 回调注入（供主程序接入业务逻辑） ----
    void onCaptureRequest(std::function<void()> cb);
    void onRecordToggle(std::function<void(bool)> cb);        // true=开始
    void onResolutionChanged(std::function<void(int,int)> cb); // (w, h)
    void onFormatChanged(std::function<void(PixelFormat)> cb);

signals:
    void captureClicked();
    void recordToggled(bool start);
    void resolutionChanged(int w, int h);
    void formatChanged(PixelFormat fmt);
};
```

### 4.2 共享类型 (include/common/types.h)

| 类型 | 字段 | 说明 |
|------|------|------|
| `PixelFormat` | enum: FMT_YUYV, FMT_MJPEG, FMT_RGB24, FMT_RGB565 | 像素格式 |
| `Resolution` | int width, height | 分辨率，已注册 Q_DECLARE_METATYPE |
| `FrameBuffer` | data, length, width, height, format, index, timestamp | 帧数据结构 |
| `CameraStatus` | streaming, recording, fps, resolution, format, client_count | 状态快照 |

### 4.3 颜色空间转换函数

```cpp
// YUYV 4:2:2 → RGB24 (BT.601, 定点运算)
inline void yuyv_to_rgb24(const uint8_t* yuyv, uint8_t* rgb, int w, int h);

// YUYV 4:2:2 → RGB565 (16-bit LCD framebuffer)
inline void yuyv_to_rgb565(const uint8_t* yuyv, uint8_t* rgb565, int w, int h);
```

**性能估算**（iMX6ULL Cortex-A7 @ 792MHz）：
- 640x480 RGB24：~5ms/帧
- 640x480 RGB565：~3ms/帧

---

## 五、构建与运行

### 5.1 PC 本地编译（开发调试）

```bash
cd SmartCam-Linux-on-imx6ull
mkdir -p build && cd build
cmake .. && make -j$(nproc)
./smartcam                     # Mock 彩条模式
```

### 5.2 ARM 交叉编译（部署到开发板）

```bash
./scripts/build.sh arm
# 产物位于 build/arm/smartcam
scp build/arm/smartcam root@<开发板IP>:/usr/local/bin/
```

### 5.3 真实硬件运行

```bash
# 开发板上执行
export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0
smartcam --device /dev/video0 --http-port 8080
```

### 5.4 编译验证记录

```
日期: 2026-05-20
CMake: 3.25.1
编译器: GCC 12.2.0 (x86_64)
Qt: 5.15.8
结果: ✅ 0 error, 0 warning, 产物 1.6MB
offscreen 模式: ✅ 正常启动, Mock 模式输出正确
```

---

## 六、与其他模块的集成点

```
                    ┌──────────────┐
                    │   main.cpp   │
                    │  (主线程)     │
                    └──┬───┬───┬──┘
                       │   │   │
          ┌────────────┼───┼───┼────────────┐
          │            │   │   │            │
          ▼            ▼   │   ▼            ▼
   ┌──────────┐  ┌─────────┐ ┌──────────┐ ┌──────────┐
   │ Capture  │  │ Display │ │ Stream   │ │ Control  │
   │ Thread   │  │ (本模块) │ │ Thread   │ │ Thread   │
   └────┬─────┘  └────┬────┘ └──────────┘ └──────────┘
        │             │
        │ setFrame()  │  ← 采集线程推帧到 GUI
        │             │
        └─────────────┘

回调注入方向 (main.cpp → 各模块):
  gui.onCaptureRequest()       → CameraCapture / StorageManager
  gui.onRecordToggle()         → StorageManager
  gui.onResolutionChanged()    → CameraCapture::setFormat()
  gui.onFormatChanged()        → CameraCapture::setFormat()
```

---

## 七、技术要点总结

| 技术点 | 实现方式 | 面试可讲 |
|--------|----------|----------|
| 实时刷新 | QTimer 33ms ≈ 30fps, QImage → QPixmap 渲染 | ✅ |
| 触摸适配 | 按钮 min-width 80px, padding 8px, 适合手指 | ✅ |
| 颜色转换 | BT.601 定点运算, 避免浮点, NEON 友好 | ✅ |
| framebuffer | `-platform linuxfb` 跳过 X11, 零开销渲染 | ✅ |
| Mock 测试 | 离线生成彩条动画, 无硬件可开发 | ✅ |
| Qt MOC | CMake AUTOMOC 自动处理 Q_OBJECT 宏 | 可讲 |
| 设计模式 | 回调注入 = 观察者模式, 信号/槽 = Qt 版观察者 | ✅ |
| 像素格式 | 支持 RGB24/RGB565/YUYV 三种格式的 QImage 构造 | ✅ |

---

## 八、后续 TODO

- [ ] 集成真实 V4L2 采集 (CameraCapture) → `setFrame()`
- [ ] 触摸滑动切换模式（需要手势识别逻辑）
- [ ] 设置面板弹窗（亮度/对比度/白平衡调节）
- [ ] YUV→RGB NEON 汇编优化（提升 30-50%）
- [ ] 录制状态下的帧计数器叠加
- [ ] 电量/存储空间状态栏显示

---

## 九、变更记录

| 日期 | 变更内容 |
|------|----------|
| 2026-05-20 | 初始实现：CameraGUI 类、Mock 模式、YUYV→RGB 转换、CMake 构建、编译通过 |
