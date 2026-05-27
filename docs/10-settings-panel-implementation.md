# 设置面板弹窗 — 亮度/对比度/白平衡调节

> 发布日期: 2026-05-27
> 关联模块: CameraGUI, CameraCapture, main

---

## 1. 功能概述

为 SmartCam 增加了一个**模态设置面板弹窗（Settings Dialog）**，将原有的行内设置栏（分辨率/格式/存储）迁移到弹窗内，并新增**亮度（Brightness）、对比度（Contrast）、白平衡（White Balance）**三项相机控制参数的实时调节功能。

### 1.1 核心功能

| 功能 | 说明 |
|------|------|
| 亮度调节 | 水平滑块，范围由 V4L2 设备实际支持决定（如 0~255），实时生效 |
| 对比度调节 | 水平滑块，范围由 V4L2 设备决定，实时生效 |
| 自动白平衡 | 复选框开关，启用则禁用色温滑块，关闭则启用手动色温 |
| 白平衡色温 | 水平滑块，范围由 V4L2 设备决定（如 2500K~10000K），仅在非自动模式下有效 |
| 重置默认值 | 一键恢复所有控制参数到 V4L2 驱动的出厂默认值 |
| 视频参数设置 | 分辨率、格式（YUYV/MJPEG）、存储路径（tmpfs/eMMC），从原行内面板迁移到弹窗中 |

### 1.2 设计理念

- **无需重启**：滑块拖动时，通过 `CameraCapture::setControl()` 直接写入 V4L2 驱动，下一帧即生效。
- **深色主题**：弹窗沿用主界面的深色配色方案（`#0a0a1a` 背景），适配 7 寸触摸屏。
- **模态弹窗**：使用 `QDialog::exec()` 模态显示，操作期间主窗口不可交互，确保参数设置不被意外打断。
- **防御性设计**：Mock 模式（无摄像头）时，滑块使用默认范围但仍可工作，回调注册后不会崩溃。

---

## 2. 交互流程

```
用户点击 Settings 按钮
    │
    ▼
CameraGUI::onSettings()
    │
    └─ m_settingsDialog->exec()  ← 打开模态弹窗
        │
        ├─ 拖动亮度滑块
        │    └─ onBrightnessChanged(value)
        │        └─ m_onCameraControl(V4L2_CID_BRIGHTNESS, value)
        │            └─ capture->setControl(V4L2_CID_BRIGHTNESS, value)   [main.cpp]
        │
        ├─ 拖动对比度滑块
        │    └─ onContrastChanged(value)
        │        └─ m_onCameraControl(V4L2_CID_CONTRAST, value)
        │            └─ capture->setControl(V4L2_CID_CONTRAST, value)     [main.cpp]
        │
        ├─ 勾选/取消 Auto WB
        │    └─ onAutoWbChanged(state)
        │        ├─ 启用：m_wbSlider->setEnabled(false)
        │        └─ m_onCameraControl(V4L2_CID_AUTO_WHITE_BALANCE, 1/0)
        │            └─ capture->setControl(..., ...)                     [main.cpp]
        │
        ├─ 拖动 WB Temp 滑块
        │    └─ onWbChanged(value)
        │        └─ m_onCameraControl(V4L2_CID_WHITE_BALANCE_TEMPERATURE, value)
        │            └─ capture->setControl(..., value)                   [main.cpp]
        │
        ├─ 改变分辨率/格式/存储
        │    └─ 复用原有下拉框逻辑 → 触发回调                      [main.cpp]
        │
        ├─ 点击 Reset Defaults
        │    └─ onResetDefaults()
        │        └─ 恢复所有滑块到驱动默认值
        │            └─ 逐个调用 m_onCameraControl(cid, def)               [main.cpp]
        │
        └─ 点击 Close 按钮
            └─ m_settingsDialog->close()
                返回实时预览
```

### 2.1 白平衡自动/手动联动

```
Auto WB CheckBox 勾选
    → m_wbSlider->setEnabled(false)         // 灰度色温滑块
    → capture->setControl(AUTO_WB, 1)       // 启用自动白平衡

Auto WB CheckBox 取消勾选
    → m_wbSlider->setEnabled(true)           // 启用色温滑块
    → capture->setControl(AUTO_WB, 0)       // 关闭自动白平衡
    → 自动将当前色温写入 V4L2               // 避免手动模式下硬件状态未同步
```

---

## 3. 修改文件清单

| 文件 | 修改类型 | 说明 |
|------|----------|------|
| `include/camera/capture.h` | **修改** | 新增 4 个 V4L2 控制 ID 静态常量 |
| `include/display/gui.h` | **修改** | 新增对话框 UI 成员、回调类型、方法声明、slot 声明 |
| `src/display/gui.cpp` | **修改** | 实现对话框构建、槽函数、范围设置方法；修改 `buildUI()` 和 `onSettings()` |
| `src/main.cpp` | **修改** | 新增 V4L2 控制参数查询逻辑和相机控制回调注册 |

---

## 4. 详细代码变更

### 4.1 `include/camera/capture.h` — 新增 V4L2 控制 ID 常量

**位置**: 在 `CameraCapture` 类的 public 静态常量区，紧跟 `V4L2_PIX_FMT_RGB24` 之后。

**新增代码**:

```cpp
/** @brief V4L2 控制 ID 常量（来自 linux/videodev2.h） */
static constexpr uint32_t V4L2_CID_BRIGHTNESS               = 0x00980900;
static constexpr uint32_t V4L2_CID_CONTRAST                 = 0x00980901;
static constexpr uint32_t V4L2_CID_AUTO_WHITE_BALANCE       = 0x0098090C;
static constexpr uint32_t V4L2_CID_WHITE_BALANCE_TEMPERATURE = 0x0098090A;
```

**说明**: 项目刻意避免 `#include <linux/videodev2.h>` 以防止类型冲突，因此在此处直接定义所需的 V4L2 控制 ID 常量。这些值与 Linux 内核头文件中定义的完全一致。

---

### 4.2 `include/display/gui.h` — 扩展 GUI 类

#### 4.2.1 新增头文件 include（第 14~16 行）

```cpp
#include <QDialog>
#include <QSlider>
#include <QCheckBox>
```

#### 4.2.2 新增回调类型（第 77 行）

```cpp
using CallbackCameraControl = std::function<void(int cid, int value)>;
```

**说明**: 回调签名 `(int cid, int value)` — `cid` 为 V4L2 控制 ID（如 0x00980900 亮度），`value` 为滑块当前值。由 `main.cpp` 注册，通过 `CameraCapture::setControl(cid, value)` 写入驱动。

#### 4.2.3 新增公共方法

**行 86**: 回调注册方法
```cpp
void onCameraControlChanged(CallbackCameraControl cb);
```

**行 89~92**: 滑块范围设置方法（由 `main.cpp` 在 V4L2 查询后调用）
```cpp
void setBrightnessRange(int min, int max, int step, int value);
void setContrastRange(int min, int max, int step, int value);
void setWhiteBalanceRange(int min, int max, int step, int value);
void setAutoWhiteBalance(bool enabled);
```

#### 4.2.4 新增 private slots（第 113~117 行）

```cpp
void onBrightnessChanged(int value);
void onContrastChanged(int value);
void onAutoWbChanged(int state);
void onWbChanged(int value);
void onResetDefaults();
```

#### 4.2.5 新增私有成员变量（第 13~28 行，方括号内为槽函数响应逻辑）

**对话框自身**:
```cpp
QDialog*     m_settingsDialog   = nullptr;
```

**视频设置控件（从行内面板迁移）**:
```cpp
QComboBox*   m_resolutionCombo  = nullptr;
QComboBox*   m_formatCombo      = nullptr;
QComboBox*   m_storageCombo     = nullptr;
```

**相机控制控件（全新）**:
```cpp
QSlider*     m_brightnessSlider  = nullptr;   // 亮度：横向滑块
QLabel*      m_brightnessValue   = nullptr;   // 亮度：右侧数值标签（如 "128"）
QSlider*     m_contrastSlider    = nullptr;   // 对比度：横向滑块
QLabel*      m_contrastValue     = nullptr;   // 对比度：右侧数值标签
QSlider*     m_wbSlider          = nullptr;   // 白平衡色温：横向滑块
QLabel*      m_wbValueLabel      = nullptr;   // 色温：右侧值标签（如 "5000K"）
QCheckBox*   m_autoWbCheckBox    = nullptr;   // 自动白平衡：复选框
QPushButton* m_btnResetDefaults  = nullptr;   // 重置默认值：按钮
```

**参数信息存储（用于 Reset Defaults 功能）**:
```cpp
struct ControlInfo {
    int min = 0, max = 100, step = 1, def = 0, current = 0;
};
ControlInfo m_brightnessInfo;
ControlInfo m_contrastInfo;
ControlInfo m_wbInfo;
bool        m_autoWbDefault       = true;
bool        m_cameraControlsAvailable = false;
```
**说明**: `ControlInfo` 结构体保存每个参数的 `min`（最小值）、`max`（最大值）、`step`（步进值）、`def`（V4L2 默认值）、`current`（当前值）。点击 "Reset Defaults" 按钮时，将所有滑块恢复到 `def` 值。

**回调成员**:
```cpp
CallbackCameraControl m_onCameraControl;
```

#### 4.2.6 移除的成员

```cpp
// 已删除：
QWidget* m_settingsPanel = nullptr;   // 原行内设置面板 → 替换为 QDialog 弹窗
```

#### 4.2.7 新增私有方法

```cpp
void buildSettingsDialog();    // 第 124 行：构建设置对话框的 UI
```

---

### 4.3 `src/display/gui.cpp` — 核心实现

#### 4.3.1 新增 include（第 2 行）

```cpp
#include "include/camera/capture.h"
```
**说明**: 引入此头文件是为了在槽函数中访问 `CameraCapture::V4L2_CID_BRIGHTNESS` 等静态常量，以便通过回调将正确的控制 ID 传递给 `main.cpp`。

#### 4.3.2 `buildUI()` 修改 — 替换行内设置栏为 `buildSettingsDialog()` 调用

**修改前** (约 50 行代码):
```cpp
// --- (4) 设置栏（可展开/收起） ---
m_settingsPanel = new QWidget(this);
auto* settingsLayout = new QHBoxLayout(m_settingsPanel);
// ... 创建 resLabel, m_resolutionCombo, fmtLabel, m_formatCombo,
//        storageLabel, m_storageCombo, comboStyle 定义 ...
settingsLayout->addWidget(resLabel);
settingsLayout->addWidget(m_resolutionCombo);
// ... 其他 widget 添加 ...
mainLayout->addWidget(m_settingsPanel);
m_settingsPanel->hide();
```

**修改后** (1 行):
```cpp
buildSettingsDialog();
```

**说明**: 原行内设置面板被完全移除，所有设置控件（分辨率/格式/存储下拉框）现在在 `buildSettingsDialog()` 方法中创建，并放置在 `QDialog` 弹窗内。

#### 4.3.3 `connectSignals()` 修改 — 新增相机控制信号连接

**新增代码段**（末尾追加）:
```cpp
// 相机控制滑块
connect(m_brightnessSlider, &QSlider::valueChanged, this, &CameraGUI::onBrightnessChanged);
connect(m_contrastSlider,   &QSlider::valueChanged, this, &CameraGUI::onContrastChanged);
connect(m_autoWbCheckBox,   &QCheckBox::stateChanged, this, &CameraGUI::onAutoWbChanged);
connect(m_wbSlider,         &QSlider::valueChanged, this, &CameraGUI::onWbChanged);
connect(m_btnResetDefaults, &QPushButton::clicked,   this, &CameraGUI::onResetDefaults);
```

**说明**: 5 个新连接分别处理：
- 亮度滑块值变化 → `onBrightnessChanged`
- 对比度滑块值变化 → `onContrastChanged`
- Auto WB 复选框状态变化 → `onAutoWbChanged`
- WB 色温滑块值变化 → `onWbChanged`
- Reset Defaults 按钮点击 → `onResetDefaults`

#### 4.3.4 `onSettings()` 修改 — 从切换行内面板改为打开弹窗

**修改前**:
```cpp
void CameraGUI::onSettings() {
    bool visible = m_settingsPanel->isVisible();
    m_settingsPanel->setVisible(!visible);
    qDebug() << "[GUI] Settings panel" << (visible ? "Hide" : "Show");
}
```

**修改后**:
```cpp
void CameraGUI::onSettings() {
    if (m_settingsDialog) {
        m_settingsDialog->exec();
    }
    qDebug() << "[GUI] Settings dialog closed";
}
```

**说明**: 点击 Settings 按钮后，以 `exec()` 模态方式打开设置弹窗。用户操作完成后点击 Close，弹窗关闭，`exec()` 返回，`onSettings` 结束。

#### 4.3.5 `showGallery()` 修改 — 关闭弹窗而非隐藏面板

**原代码**:
```cpp
m_settingsPanel->hide();
```

**新代码**:
```cpp
if (m_settingsDialog && m_settingsDialog->isVisible()) {
    m_settingsDialog->close();
}
```

**说明**: 进入相册前，如果设置弹窗正在显示，自动关闭它，避免弹窗残留。

#### 4.3.6 `buildSettingsDialog()` — 弹窗 UI 构建（全新方法）

**描述**: 在 CameraGUI 构造函数中通过 `buildUI()` → `buildSettingsDialog()` 调用。创建一次、复用多次。

**弹窗规格**:
| 属性 | 值 |
|------|-----|
| 标题 | "Settings" |
| 最小尺寸 | 640×440 像素 |
| 模态 | `true` (`setModal(true)`) |
| 背景色 | `#0a0a1a`（深色主题） |

**分组结构**:

**Group 1: "Video Settings"** (`QGroupBox`)
```
| Label "Resolution:" (宽 100px) | QComboBox: 640×480 / 320×240 / 1280×720 |
| Label "Format:"                | QComboBox: YUYV / MJPEG                   |
| Label "Storage:"               | QComboBox: Temporary(/data) / Persistent(eMMC) |
```
三行等宽的设置行，标签固定 100px 宽，下拉框自适应。

**Group 2: "Camera Controls"** (`QGroupBox`)
```
| Label "Brightness:" (宽 100px) | QSlider(Horizontal) | QLabel 数值 (宽 50px) |
| Label "Contrast:"              | QSlider(Horizontal) | QLabel 数值           |
| Label "Auto WB:"               | QCheckBox "Auto"    | 弹性空白              |
| Label "WB Temp:"               | QSlider(Horizontal) | QLabel "XXXXK" (60px) |
```
- 亮度、对比度滑块初始范围 0~100（占位值，随后由 `main.cpp` 通过 `setXxxRange()` 覆盖为实际 V4L2 范围）。
- Auto WB 复选框默认勾选。勾选时，WB Temp 滑块 **禁用**（`setEnabled(false)`）。
- WB Temp 滑块默认值 5000K，默认禁用状态。

**底部按钮**:
```
|<- 弹性空白 -->|  "Reset Defaults"  |   "Close"   |
```
- **Reset Defaults**: 深灰色按钮（`#2c3e50`），恢复到 V4L2 默认值。
- **Close**: 蓝色按钮（`#1a6fb5`），关闭弹窗。

**样式定义**:

滑块样式（所有滑块共用）:
```css
QSlider::groove:horizontal {
    border: 1px solid #0f3460; height: 6px;
    background: #16213e; border-radius: 3px;
}
QSlider::handle:horizontal {
    background: #3498db; border: 1px solid #2980b9;
    width: 18px; margin: -7px 0; border-radius: 9px;
}
QSlider::sub-page:horizontal { background: #2980b9; border-radius: 3px; }
```

复选框样式:
```css
QCheckBox { color: #e0e0e0; font-size: 13px; spacing: 6px; }
QCheckBox::indicator { width: 20px; height: 20px; }
QCheckBox::indicator:unchecked {
    border: 2px solid #7f8c8d; border-radius: 3px; background: #16213e;
}
QCheckBox::indicator:checked {
    border: 2px solid #3498db; border-radius: 3px; background: #3498db;
}
```

下拉框样式（沿用原行内面板样式）:
```css
QComboBox { ... background: #16213e; color: #e0e0e0;
    border: 1px solid #0f3460; border-radius: 4px; min-width: 160px; ... }
```

#### 4.3.7 范围设置方法（4 个新方法）

**方法**: `setBrightnessRange(int min, int max, int step, int value)`

```cpp
void CameraGUI::setBrightnessRange(int min, int max, int step, int value) {
    m_brightnessInfo = {min, max, step, value, value};       // 保存参数范围
    m_cameraControlsAvailable = true;

    m_brightnessSlider->blockSignals(true);                   // 阻止信号
    m_brightnessSlider->setRange(min, max);                   // 设置范围
    m_brightnessSlider->setSingleStep(step);                  // 单步值
    m_brightnessSlider->setPageStep(step * 10);               // 翻页步长
    m_brightnessSlider->setValue(value);                      // 设置当前值
    m_brightnessSlider->blockSignals(false);                  // 恢复信号
    m_brightnessValue->setText(QString::number(value));       // 更新标签
}
```

**调用时机**: `main.cpp` 中，相机初始化成功后，依次调用 `capture->queryControl()` + `capture->getControl()` 获取各参数范围，然后调用此方法填入 GUI 滑块。

**重要性**: `blockSignals(true/false)` 确保设置 `setRange()` 和 `setValue()` 时不会触发 `valueChanged` 信号，避免在初始化阶段错误调用 V4L2 写操作。

**类似方法**:
- `setContrastRange(min, max, step, value)` — 同上，操作 `m_contrastSlider` 和 `m_contrastInfo`。
- `setWhiteBalanceRange(min, max, step, value)` — 同上，操作 `m_wbSlider` 和 `m_wbInfo`。步长 `step * 100` 因为色温通常以 100K 为单位。
- `setAutoWhiteBalance(bool enabled)` — 设置 `m_autoWbCheckBox` 的选中状态和 `m_autoWbDefault`。

#### 4.3.8 槽函数实现（5 个新 slot）

**`onBrightnessChanged(int value)`**:
```cpp
void CameraGUI::onBrightnessChanged(int value) {
    m_brightnessValue->setText(QString::number(value));   // 更新右侧数值标签
    m_brightnessInfo.current = value;                     // 记录当前值
    if (m_onCameraControl) {
        m_onCameraControl(static_cast<int>(CameraCapture::V4L2_CID_BRIGHTNESS), value);
    }
}
```

**说明**:
1. 实时更新 QLabel 显示当前值。
2. 更新 `m_brightnessInfo.current`（用于 Reset 时的状态追踪）。
3. 若回调已注册，通过回调传递 `(cid=0x00980900, value)` 给 `main.cpp`。

**`onContrastChanged(int value)`** — 结构相同，cid 为 `V4L2_CID_CONTRAST`。

**`onAutoWbChanged(int state)`**:
```cpp
void CameraGUI::onAutoWbChanged(int state) {
    bool autoWb = (state == Qt::Checked);
    m_wbSlider->setEnabled(!autoWb);                     // 开关联动
    if (m_onCameraControl) {
        m_onCameraControl(static_cast<int>(CameraCapture::V4L2_CID_AUTO_WHITE_BALANCE),
                          autoWb ? 1 : 0);
    }
}
```

**说明**: 当 Auto WB 复选框状态变化时，联动手动色温滑块启用/禁用。回调传递 `(cid=0x0098090C, value=1/0)`。

**`onWbChanged(int value)`** — 结构同亮度，但标签格式为 `"XXXXK"`（带单位），cid 为 `V4L2_CID_WHITE_BALANCE_TEMPERATURE`。

**`onResetDefaults()`**:
```cpp
void CameraGUI::onResetDefaults() {
    // 亮度
    m_brightnessSlider->setValue(m_brightnessInfo.def);
    m_brightnessValue->setText(QString::number(m_brightnessInfo.def));
    m_brightnessInfo.current = m_brightnessInfo.def;
    if (m_onCameraControl) {
        m_onCameraControl(static_cast<int>(CameraCapture::V4L2_CID_BRIGHTNESS),
                          m_brightnessInfo.def);
    }

    // 对比度（同上）
    // ...

    // 白平衡
    m_autoWbCheckBox->setChecked(m_autoWbDefault);
    m_wbSlider->setValue(m_wbInfo.def);
    m_wbValueLabel->setText(QString("%1K").arg(m_wbInfo.def));
    m_wbInfo.current = m_wbInfo.def;
    m_wbSlider->setEnabled(!m_autoWbDefault);
    if (m_onCameraControl) {
        m_onCameraControl(...AUTO_WHITE_BALANCE..., m_autoWbDefault ? 1 : 0);
        m_onCameraControl(...WHITE_BALANCE_TEMPERATURE..., m_wbInfo.def);
    }
}
```

**说明**: 一次性将亮度、对比度、白平衡三个参数全部恢复到 V4L2 默认值。通过 `m_xxxInfo.def`（在 `setXxxRange()` 调用时记录的 `def` 参数）恢复。

#### 4.3.9 回调注册方法

```cpp
void CameraGUI::onCameraControlChanged(CallbackCameraControl cb) {
    m_onCameraControl = std::move(cb);
}
```

**说明**: 标准回调注册，与其他回调（如 `onCaptureRequest`、`onRecordToggle`）风格一致。

---

### 4.4 `src/main.cpp` — 查询 V4L2 控制参数并注册回调

**位置**: 在 `gui.setStreamingStatus(true); g_state.running = true;` 之后，`MJPEG HTTP 服务器启动` 之前。

**新增代码块**:

```cpp
// ============================================================
// 查询 V4L2 控制参数范围 & 注册相机控制回调
// ============================================================
{
    int min, max, step, def, val;

    // 亮度
    if (capture->queryControl(CameraCapture::V4L2_CID_BRIGHTNESS,
                               min, max, step, def) == 0) {
        capture->getControl(CameraCapture::V4L2_CID_BRIGHTNESS, val);
        gui.setBrightnessRange(min, max, step, (val != 0 ? val : def));
        LOG_INF("Brightness: min=%d max=%d step=%d def=%d cur=%d",
                 min, max, step, def, val);
    }

    // 对比度
    if (capture->queryControl(CameraCapture::V4L2_CID_CONTRAST,
                               min, max, step, def) == 0) {
        capture->getControl(CameraCapture::V4L2_CID_CONTRAST, val);
        gui.setContrastRange(min, max, step, (val != 0 ? val : def));
        LOG_INF("Contrast: min=%d max=%d step=%d def=%d cur=%d",
                 min, max, step, def, val);
    }

    // 白平衡色温
    if (capture->queryControl(CameraCapture::V4L2_CID_WHITE_BALANCE_TEMPERATURE,
                               min, max, step, def) == 0) {
        capture->getControl(CameraCapture::V4L2_CID_WHITE_BALANCE_TEMPERATURE, val);
        gui.setWhiteBalanceRange(min, max, step, (val != 0 ? val : def));
        LOG_INF("WB Temp: min=%d max=%d step=%d def=%d cur=%d",
                 min, max, step, def, val);
    }

    // 自动白平衡
    if (capture->queryControl(CameraCapture::V4L2_CID_AUTO_WHITE_BALANCE,
                               min, max, step, def) == 0) {
        capture->getControl(CameraCapture::V4L2_CID_AUTO_WHITE_BALANCE, val);
        gui.setAutoWhiteBalance(val != 0);
        LOG_INF("Auto WB: cur=%d", val);
    }

    // 注册统一回调：滑块变化 → V4L2 setControl
    gui.onCameraControlChanged([capture](int cid, int value) {
        int ret = capture->setControl(cid, value);
        if (ret < 0) {
            LOG_WRN("setControl(cid=0x%08X, val=%d) failed (ret=%d)",
                     static_cast<uint32_t>(cid), value, ret);
        } else {
            LOG_INF("Camera control: cid=0x%08X → %d",
                     static_cast<uint32_t>(cid), value);
        }
    });
}
```

**流程说明**:

1. **查询参数范围**: `capture->queryControl(cid, &min, &max, &step, &def)` 查询 V4L2 驱动中该控制项的合法范围。返回 0 表示设备支持该参数（不支持则跳过，滑块保持占位值）。
2. **获取当前值**: `capture->getControl(cid, &val)` 读取硬件当前值。
3. **设置 GUI 滑块**: `gui.setXxxRange(min, max, step, value)` 将查询到的范围写入 GUI 滑块。
4. **注册回调**: `gui.onCameraControlChanged(lambda)` 注册一个 lambda，捕获 `capture` 指针，在滑块变化时调用 `capture->setControl(cid, value)`。成功/失败均有日志。
5. **Mock 模式处理**: Mock 模式下 `capture` 为 `nullptr`，此代码块不会执行（在 `device.isEmpty()` 分支内）。GUI 滑块使用默认占位范围。

---

## 5. 架构数据流

```
┌──────────────────────────┐      ┌──────────────────────┐
│  main.cpp (初始化阶段)      │      │  CameraCapture       │
│                           │      │  (V4L2 驱动接口)      │
│  queryControl()  ──────────┼─────▶│                      │
│  getControl()    ──────────┼─────▶│  VIDIOC_QUERYCTRL   │
│                           │      │  VIDIOC_G_CTRL       │
│  setXxxRange()   ─────────┼─────▶│                      │
│                    gui    │      └──────────────────────┘
│  onCameraControlChanged()┐│
│                          ││
└──────────────────────────┘│
                            │
┌───────────────────────────┼──────────────────────────────┐
│  CameraGUI                │                              │
│                           ▼                              │
│  m_onCameraControl ◄──── lambda(cid, value)              │
│       ▲                                                  │
│       │  sliders emit valueChanged                        │
│       │                                                  │
│  ┌────┴───────────┐  ┌──────────┐  ┌──────────────────┐ │
│  │ Brightness     │  │ Contrast │  │ WB Temp          │ │
│  │ Slider         │  │ Slider   │  │ Slider           │ │
│  └────────────────┘  └──────────┘  └──────────────────┘ │
│                                                          │
│  ┌──────────────────┐                                    │
│  │ Auto WB          │──► enable/disable WB Temp Slider  │
│  │ CheckBox         │                                    │
│  └──────────────────┘                                    │
│                                                          │
│  ┌──────────────────┐                                    │
│  │ Reset Defaults   │──► All sliders → .def values       │
│  │ Button           │                                    │
│  └──────────────────┘                                    │
└──────────────────────────────────────────────────────────┘
```

---

## 6. 兼容性考虑

### 6.1 摄像头不支持某个参数

V4L2 设备可能不支持全部 4 个控制参数（例如，便宜的 USB 摄像头可能只支持亮度和对比度）。此时 `queryControl()` 返回非零值，`if (queryControl(...) == 0)` 条件不满足，对应代码块不执行。

| 场景 | 行为 |
|------|------|
| 不支持亮度 | 亮度滑块保持占位范围 (0~100)，拖动不会触发有效 V4L2 调用 |
| 不支持对比度 | 同上 |
| 不支持白平衡色温 | 同上 |
| 不支持自动白平衡 | 复选框保持默认（勾选），状态变化回调仍会调用（但 `setControl` 会失败） |

### 6.2 Mock 模式（无摄像头）

Mock 模式下 `capture == nullptr`，整个 V4L2 查询代码块不会执行。GUI 中所有滑块保持默认占位值，用户拖动无实际效果但不会崩溃。`m_onCameraControl` 回调未被注册，`onXxxChanged` 槽不会触发任何副作用。

### 6.3 分辨率/格式切换的安全性

分辨率和格式选择下拉框在弹窗中的信号连接与原来一致，通过 `m_onResolutionChanged` 和 `m_onFormatChanged` 回调触发 `main.cpp` 中的摄像头停止/重启逻辑。`main.cpp` 使用 `g_state.paused` 机制确保安全的格式切换，不会受到弹窗模态的影响。

### 6.4 崩溃恢复

- `blockSignals(true/false)` 确保设置初始范围时不触发 V4L2 写入。
- 回调中检查 `if (m_onCameraControl)`，防止未注册时调用空函数。
- `capture->setControl()` 失败时只输出 `LOG_WRN`，不会导致程序崩溃。

---

## 7. 测试建议

### 7.1 PC Mock 模式测试

```bash
cd SmartCam-Linux-on-imx6ull
mkdir build/test && cd build/test
cmake ../.. && make -j$(nproc)
./smartcam -platform xcb
```

**测试点**:
1. 点击 Settings 按钮 → 确认弹窗打开。
2. 拖动亮度/对比度滑块 → 确认数值标签实时更新。
3. 勾选 Auto WB → 确认色温滑块变为灰色禁用。
4. 取消 Auto WB → 确认色温滑块恢复可拖动。
5. 点击 Reset Defaults → 确认滑块恢复到默认值。
6. 点击 Close → 确认弹窗关闭，预览继续。
7. 改变分辨率/格式下拉框 → 确认 Mock 模式彩条更新。
8. 进入 Gallery → 确认弹窗被关闭。

### 7.2 开发板真机测试

```bash
./smartcam --device /dev/video0 --fmt mjpeg -platform linuxfb
```

**额外测试点**:
1. 日志输出确认 V4L2 控制参数范围（Brightness: min=... max=...）。
2. 拖动亮度滑块 → 观察预览画面亮度变化（实时生效）。
3. 调整对比度 → 观察画面变化。
4. 关闭 Auto WB、调色温→ 观察画面色温变化。
5. 点击 Reset Defaults → 观察画面恢复初始效果。
6. `journalctl -u smartcam -f` 检查日志，确认无 `setControl failed` 错误。

---

## 8. 未来扩展

| 方向 | 描述 |
|------|------|
| 饱和度控制 | 增加 `V4L2_CID_SATURATION` 滑块，与亮度/对比度并列。实现方式完全相同 |
| 锐度控制 | 增加 `V4L2_CID_SHARPNESS` 滑块 |
| 曝光控制 | 增加 `V4L2_CID_EXPOSURE_AUTO` 复选框 + `V4L2_CID_EXPOSURE_ABSOLUTE` 滑块 |
| 配置持久化 | 将相机控制参数写入 `smartcam.conf`，下次启动时恢复 |
| 触摸手势 | 滑块区域支持双指快速调整（适合小屏操作） |
