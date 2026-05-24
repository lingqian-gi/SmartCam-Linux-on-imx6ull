# SmartCam Linux — Debug 总结文档

> 持续更新，记录 imx6ull-pro 开发板调试过程中遇到的所有问题。

---

## 7. 配置文件解析实现

| 属性 | 值 |
|------|-----|
| **模块** | `include/common/config.h`、`src/main.cpp` |
| **现象** | `configs/smartcam.conf` 已写好但**从未被解析**，所有配置走命令行硬编码默认值 |
| **严重程度** | ⚠️ 中等 — 需要手动指定所有参数，部署不便 |

### 实现

1. 新建 `include/common/config.h` — header-only INI 解析器 `ConfigManager` 类
   - 支持 `[section]` / `key = value` / `#` 注释
   - 类型安全接口：`getString()` / `getInt()` / `getBool()`
   - 优先级：命令行 > 配置文件 > 硬编码默认值

2. `main.cpp` 集成：
   - 新增 `--config` 命令行选项（默认 `/etc/smartcam/smartcam.conf`）
   - 配置加载顺序：命令行解析 → `cfg.load()` → 合并配置
   - 支持配置的模块：camera(device/format)、network(http_port/rtsp_port/control_port)、storage(photo_dir/video_dir)、logging(level/use_syslog)

**涉及文件：**
- `include/common/config.h`（新增）
- `src/main.cpp`
- `configs/smartcam.conf`

---

## 6. YUYV 格式推流打通

| 属性 | 值 |
|------|-----|
| **模块** | `src/main.cpp` — 采集线程推流逻辑 |
| **现象** | `--fmt yuyv` 模式下，MJPEG HTTP 流和 RTSP 流均不可用（服务器不启动） |
| **严重程度** | ⚠️ 中等 — YUYV 模式只能本地预览，无法远程观看 |

### 原因

原设计中 MJPEG 和 RTSP 服务器仅在摄像头 MJPEG 格式时启动。

### 解决

1. MJPEG 和 RTSP 服务器**始终启动**，不依赖摄像头格式
2. 采集线程中增加 YUYV→JPEG 编码分支，`encodeYUYVtoJPEG()` 编码一次，复用给两个流
3. `free(jpeg_out)` 释放临时编码缓冲

**涉及文件：** `src/main.cpp`（4 处修改）

---

## 6b. systemd 服务文件完善

| 属性 | 值 |
|------|-----|
| **模块** | `configs/smartcam.service` |
| **现象** | `Type=forking` 与程序实现不匹配，`ExecStop` 引用不存在的 `smartcam stop` 子命令 |
| **严重程度** | ⚠️ 中等 — 崩溃时 `Restart=on-failure` 可能不生效 |

### 解决

1. `Type=forking` → `Type=simple`
2. 删除 `ExecStop`（systemd 发 SIGTERM 即可）
3. 新增 `ConditionPathExists=/dev/video0`、`Environment=QT_QPA_PLATFORM=linuxfb` 等
4. 新增 `ProtectSystem=full`、`RestrictAddressFamilies` 等安全加固

**涉及文件：** `configs/smartcam.service`

---

## 5. GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `setFrame()` 和 `frameToQImage()` |
| **现象** | 7寸 LCD (linuxfb) 画面闪烁、部分帧花屏；同一时刻 HTTP 流 (浏览器) 画面清晰流畅 |
| **严重程度** | ❌ 严重 — 本地预览不可用 |

### 原因

**问题 A：悬垂指针（数据竞争）** — `setFrame()` 只存指针不拷贝数据，`std::vector::assign()` 导致重新分配内存。

**问题 B：MJPEG 软件解码慢** — Cortex-A7 上 JPEG 解码 30~50ms/帧，跟不上 33ms 刷新间隔。

### 解决

1. `setFrame()` 改为**深拷贝**（`m_frameBuffer.assign`）
2. 新增 `decodeMjpegToRgb()` 静态函数 + 自定义 libjpeg 错误处理器（静默坏帧警告）

**涉及文件：** `src/display/gui.cpp`、`include/display/gui.h`

---

## 4. 线程自 join 死锁 (EDEADLK)

| 属性 | 值 |
|------|-----|
| **模块** | `src/network/mjpeg_server.cpp` — `removeClient()` |
| **现象** | 手机浏览器连接后立即崩溃：`terminate called after throwing an instance of 'std::system_error'` |
| **严重程度** | ❌ 严重 — 进程 abort |

### 原因

`clientHandler()` 线程执行结束时调用 `removeClient(fd)`，`removeClient()` 内部对匹配 fd 的条目执行 `it->thread->join()`，即线程 join 自身。

### 解决

将 `addClient()` 中的线程创建改为 `detach` 模式，不再存储 `thread` 指针。

**涉及文件：** `src/network/mjpeg_server.cpp`、`include/network/mjpeg_server.h`

---

## 3. ARM 交叉编译 jpeglib.h 包含位置错误

| 属性 | 值 |
|------|-----|
| **模块** | `src/camera/processor.cpp` — `encodeRGBtoJPEG()` |
| **现象** | ARM 交叉编译报错：`expected unqualified-id before string constant` |
| **严重程度** | ❌ 严重 — 编译不通过 |

### 原因

`#include <jpeglib.h>` 被放在函数体内，嵌套在 C++ 函数体内是非法语法。

### 解决

将 `#include <jpeglib.h>` 移到文件顶部的 `#include` 区域。

**涉及文件：** `src/camera/processor.cpp`

---

## 2. Corrupt JPEG data 警告刷屏

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — 解码路径 (libjpeg) |
| **现象** | 运行后终端持续输出 `Corrupt JPEG data: premature end of data segment` |
| **严重程度** | ⚠️ 次要 — 画面能显示，但终端被刷屏 |

### 原因

USB UVC 摄像头偶发帧不完整，libjpeg 检测到缺少 EOI 标记后向 stderr 输出警告。

### 解决

新增自定义 libjpeg 错误处理器：`jpegSilentErrorExit` + `jpegSilentOutputMessage`（空函数，彻底静默所有输出）。

**涉及文件：** `src/display/gui.cpp`

---

## 1. GUI 不支持的像素格式 (MJPG)

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` — `frameToQImage()` |
| **现象** | 启动后 GUI 输出 `[GUI] 不支持的像素格式: 1196444237`，framebuffer 无画面 |
| **严重程度** | ❌ 严重 — 画面完全不显示 |

### 原因

`1196444237` = `0x47504A4D` = FourCC `"MJPG"`。`frameToQImage()` 的 `switch` 缺少 `FMT_MJPEG` 分支。

### 解决

添加 `FMT_MJPEG` 解码分支，使用 libjpeg-turbo 解码。

**涉及文件：** `src/display/gui.cpp`、`include/display/gui.h`

---

## 目录

1. [GUI 不支持的像素格式 (MJPG)](#1-gui-不支持的像素格式-mjpg)
2. [Corrupt JPEG data 警告刷屏](#2-corrupt-jpeg-data-警告刷屏)
3. [ARM 交叉编译 jpeglib.h 包含位置错误](#3-arm-交叉编译-jpeglibh-包含位置错误)
4. [线程自 join 死锁 (EDEADLK)](#4-线程自-join-死锁-edeadlk)
5. [GUI 显示闪烁 / 坏帧（指针悬垂 + MJPEG 解码瓶颈）](#5-gui-显示闪烁--坏帧指针悬垂--mjpeg-解码瓶颈)
6. [YUYV 格式推流打通](#6-yuyv-格式推流打通)
7. [配置文件解析实现](#7-配置文件解析实现)
