# SmartCam Linux — Debug 总结文档

> 持续更新，记录 imx6ull-pro 开发板调试过程中遇到的所有问题。

---

## 8. 相册模块实现

| 属性 | 值 |
|------|-----|
| **模块** | `include/display/gallery.h`、`src/display/gallery.cpp`（新增） |
| **现象** | 没有浏览和管理已拍摄照片的功能 |
| **严重程度** | 🟡 次要 — 用户体验提升 |

### 实现

1. **StorageManager 扩展** — 新增 `listPhotos()`、`getPhotoCount()`、`deletePhoto()`、`readJpegSize()`
2. **PhotoGallery 类** — 缩略图网格（3列）+ 全屏查看 + 翻页/删除
3. **CameraGUI 集成** — QStackedWidget 切换实时预览/相册，Gallery 按钮
4. **JPEG 快速尺寸读取** — 扫描 SOF0 标记，只读 4KB 不解码像素

**涉及文件：** manager.h/cpp, gallery.h/cpp, gui.h/cpp, main.cpp, CMakeLists.txt

---

## 7. 配置文件解析实现

| 属性 | 值 |
|------|-----|
| **模块** | `include/common/config.h`、`src/main.cpp` |
| **现象** | `configs/smartcam.conf` 已写好但从未被解析 |
| **严重程度** | ⚠️ 中等 — 需要手动指定所有参数 |

### 实现

1. 新建 `ConfigManager` — header-only INI 解析器
2. `main.cpp` 集成：`--config` 命令行选项 + 配置合并
3. 优先级：命令行 > 配置文件 > 硬编码默认值

**涉及文件：** `include/common/config.h`（新增）、`src/main.cpp`、`configs/smartcam.conf`

---

## 6. YUYV 格式推流打通

| 属性 | 值 |
|------|-----|
| **模块** | `src/main.cpp` — 采集线程推流逻辑 |
| **现象** | `--fmt yuyv` 模式下 MJPEG/RTSP 流均不可用 |
| **严重程度** | ⚠️ 中等 |

### 解决

1. MJPEG/RTSP 服务器始终启动
2. 采集线程 `encodeYUYVtoJPEG()` 编码一次，复用给两个流

**涉及文件：** `src/main.cpp`

---

## 6b. systemd 服务文件完善

| 属性 | 值 |
|------|-----|
| **模块** | `configs/smartcam.service` |
| **现象** | `Type=forking` 不匹配，`ExecStop` 引用不存在子命令 |

### 解决

`Type=simple` + 删除 `ExecStop` + 新增安全加固

**涉及文件：** `configs/smartcam.service`

---

## 5. GUI 显示闪烁 / 坏帧

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` |
| **严重程度** | ❌ 严重 |

### 解决

深拷贝帧数据 + libjpeg 自定义静默错误处理器

**涉及文件：** `src/display/gui.cpp`、`include/display/gui.h`

---

## 4. 线程自 join 死锁 (EDEADLK)

| 属性 | 值 |
|------|-----|
| **模块** | `src/network/mjpeg_server.cpp` |
| **严重程度** | ❌ 严重 — 进程 abort |

### 解决

`detach` 模式替代 `thread->join()`

**涉及文件：** `src/network/mjpeg_server.cpp`

---

## 3. ARM 交叉编译 jpeglib.h 包含位置错误

| 属性 | 值 |
|------|-----|
| **模块** | `src/camera/processor.cpp` |
| **严重程度** | ❌ 严重 — 编译不通过 |

### 解决

`#include <jpeglib.h>` 移到文件顶部

---

## 2. Corrupt JPEG data 警告刷屏

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` |
| **严重程度** | ⚠️ 次要 |

### 解决

自定义 libjpeg 错误处理器静默所有输出

---

## 1. GUI 不支持的像素格式 (MJPG)

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` |
| **严重程度** | ❌ 严重 — 画面不显示 |

### 解决

`frameToQImage()` 添加 `FMT_MJPEG` 解码分支

