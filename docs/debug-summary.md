# SmartCam Linux — Debug 总结文档

> 持续更新，记录 imx6ull-pro 开发板调试过程中遇到的所有问题。

---

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

---

## 目录

1. [GUI 不支持的像素格式 (MJPG)](#1-gui-不支持的像素格式-mjpg)
2. [Corrupt JPEG data 警告刷屏](#2-corrupt-jpeg-data-警告刷屏)
3. [ARM 交叉编译 jpeglib.h 包含位置错误](#3-arm-交叉编译-jpeglibh-包含位置错误)
4. [线程自 join 死锁 (EDEADLK)](#4-线程自-join-死锁-edeadlk)

---

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

---

## 1. GUI 不支持的像素格式 (MJPG)

| 属性 | 值 |
|---

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

------

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

---|---

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

-----|
| **模块** | `src/display/gui.cpp` — `frameToQImage()` |
| **现象** | 启动后 GUI 输出 `[GUI] 不支持的像素格式: 1196444237`，framebuffer 无画面 |
| **严重程度** | ❌ 严重 — 画面完全不显示 |

### 原因

`1196444237` = `0x47504A4D` = FourCC `"MJPG"`。摄像头以 MJPEG 格式采集帧，`gui.setFrame()` 将 `PixelFormat::FMT_MJPEG` 传给 `frameToQImage()`，但该函数 `switch` 只处理了 `FMT_RGB24`、`FMT_RGB565`、`FMT_YUYV`，缺少 `FMT_MJPEG` 分支，走到 `default` 返回空 QImage。

### 解决

1. `frameToQImage()` 新增 `int len` 参数
2. 添加 `FMT_MJPEG` 解码分支，使用 Qt `loadFromData("JPEG")` 或 libjpeg-turbo 解码
3. 更新 `refreshFrame()` 调用处传递 `m_currentFrame.length`

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

---

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

---

## 2. Corrupt JPEG data 警告刷屏

| 属性 | 值 |
|---

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

------

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

---|---

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

-----|
| **模块** | `src/display/gui.cpp` — 解码路径 (libjpeg) |
| **现象** | 运行后终端持续输出 `Corrupt JPEG data: premature end of data segment` |
| **严重程度** | ⚠️ 次要 — 画面能显示，但终端被刷屏 |

### 原因

MJPEG 帧通过 Qt 的 `loadFromData("JPEG")` 解码 → 底层调用 libjpeg。USB UVC 摄像头偶发帧不完整（`vbuf.bytesused` 与 JPEG 实际结尾不匹配／传输丢包），libjpeg 检测到缺少 EOI 标记 (`0xFFD9`) 后向 stderr 输出警告。libjpeg 默认的 `output_message` 回调直接 `fprintf` 到 stderr，无法被应用层拦截。

### 解决

1. 在 `src/display/gui.cpp` 中新增自定义 libjpeg 错误处理器：
   - `jpegSilentErrorExit` — 用 `setjmp`/`longjmp` 安全跳过坏帧
   - `jpegSilentOutputMessage` — 空函数，**彻底静默所有 libjpeg 输出**
2. 新增 `decodeMjpegToRgb()` 静态函数代替 Qt 原生加载
3. CMake 启用 libjpeg-turbo 链接（`find_package(JPEG REQUIRED)`）

**涉及文件：**
- `src/display/gui.cpp`
- `CMakeLists.txt`

---

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

---

## 3. ARM 交叉编译 jpeglib.h 包含位置错误

| 属性 | 值 |
|---

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

------

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

---|---

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

-----|
| **模块** | `src/camera/processor.cpp` — `encodeRGBtoJPEG()` |
| **现象** | ARM 交叉编译报错：`expected unqualified-id before string constant`，大量 `jpeg_*` 函数未声明 |
| **严重程度** | ❌ 严重 — 编译不通过 |

### 原因

`processor.cpp` 中 `#include <jpeglib.h>` 被放在 `encodeRGBtoJPEG()` 函数体内部的 `#ifdef HAS_LIBJPEG` 块中：

```cpp
int VideoProcessor::encodeRGBtoJPEG(...) {
#ifdef HAS_LIBJPEG
#include <jpeglib.h>    // ← 在 C++ 函数体内！
```

`<jpeglib.h>` 内部用 `extern "C" { ... }` 包裹所有声明，嵌套在 C++ 函数体内是非法语法。GCC 8.3 ARM 交叉编译器严格拒绝。x86 本地 GCC 11 宽松通过，因此本地编译未暴露此问题。

### 解决

将 `#include <jpeglib.h>` 移到文件顶部的 `#include` 区域（文件作用域），函数体内只保留业务逻辑。

**涉及文件：**
- `src/camera/processor.cpp`

---

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

---

## 4. 线程自 join 死锁 (EDEADLK)

| 属性 | 值 |
|---

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

------

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

---|---

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

-----|
| **模块** | `src/network/mjpeg_server.cpp` — `removeClient()` |
| **现象** | 手机浏览器连接后立即崩溃：`terminate called after throwing an instance of 'std::system_error' what(): Resource deadlock avoided` |
| **严重程度** | ❌ 严重 — 进程 abort |

### 原因

`clientHandler()` 线程执行结束时调用 `removeClient(client_fd)`。`removeClient()` 内部获取锁后遍历客户端列表，对匹配 fd 的条目执行 **`it->thread->join()`**。但此时调用线程就是 `clientHandler` 线程自身，即 `it->thread` 指向的就是当前执行线程。

```
clientHandler(fd)             ← 子线程A
  → 客户端断开
    → removeClient(fd)
      → lock(m_clientsMtx)
        → it->thread->join()  ← 线程A join 自身 → EDEADLK
```

C++ 标准禁止线程 join 自身，`std::thread::join()` 抛出 `std::system_error`。

### 解决

将 `addClient()` 中的线程创建改为 `detach` 模式，不再存储 `thread` 指针：

- `addClient()`：`new std::thread(...)` → `std::thread(...).detach()`
- `removeClient()`：只 close fd 并从列表移除，不 join
- `ClientInfo` 结构体移除 `std::thread* thread` 成员
- `stop()` 中清理客户端时只 close fd，不 join

**涉及文件：**
- `src/network/mjpeg_server.cpp`
- `include/network/mjpeg_server.h`

---

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**两个独立问题叠加导致：**

#### 问题 A：悬垂指针（数据竞争）

`setFrame()` 只存了指向 `g_state.frameData` 的**指针**，没有拷贝数据：

```cpp
// 修改前（有 bug）：
void CameraGUI::setFrame(const uint8_t* data, ...) {
    m_currentFrame.data = const_cast<uint8_t*>(data);  // 只存指针！
    ...
}
```

采集线程和 Qt 主线程交错执行：

```
采集线程                              Qt 主线程
  lock(g_state.mtx)                   
  frameData.assign(new_frame)  ← 可能 realloc
  unlock(g_state.mtx)              
                                        displayTimer 触发:
                                          lock(g_state.mtx)
                                          setFrame(frameData.data())
                                          // 存的指针指向 frameData 内部
                                          unlock(g_state.mtx)
  lock(g_state.mtx)                   
  frameData.assign(next_frame) ← realloc，旧指针变野指针！
  unlock(g_state.mtx)              
                                        refreshFrame 触发:
                                          frameToQImage(data)
                                          // 从野指针读数据 → 坏帧/花屏
```

`g_state.frameData` 是 `std::vector`，`assign()` 在数据增长时会重新分配内存，之前存的指针变成悬垂指针。

#### 问题 B：MJPEG 软件解码慢

即使指针不野，`frameToQImage()` 每次都要在 Cortex-A7 上软件解码 JPEG（640×480，30~50ms/帧），而 `m_refreshTimer` 间隔仅 33ms（目标 30fps），解码跟不上导致丢帧闪烁。

相比之下 HTTP 流直接发送原始 JPEG 数据（零解码），所以浏览器画面正常。

### 解决

1. `setFrame()` 改为**深拷贝**数据到内部缓冲区：

```cpp
// 修改后：
void CameraGUI::setFrame(const uint8_t* data, int len, ...) {
    m_frameBuffer.assign(data, data + len);       // 拷贝
    m_currentFrame.data = m_frameBuffer.data();    // 指向内部 buffer
    ...
}
```

2. 新增 `std::vector<uint8_t> m_frameBuffer` 成员变量，生命周期与 GUI 对象一致

**涉及文件：**
- `src/display/gui.cpp`
- `include/display/gui.h`

### 后续优化方向

Cortex-A7 上 MJPEG 软件解码仍可能掉帧，后续可考虑：

- **降低分辨率**：在 imx6ull 上用 `320×240` 预览，`640×480` 仅用于 HTTP 流
- **YUYV 模式**：`--fmt yuyv` 绕开 JPEG 解码，YUYV→RGB24 是轻量查表转换（~2ms/帧）
- **硬件 JPEG 解码**：iMX6ULL 的 PXP 引擎不支持 JPEG，需外挂或换平台
- **双缓冲 / 异步解码**：解码放在采集线程，GUI 只渲染 RGB24

---
