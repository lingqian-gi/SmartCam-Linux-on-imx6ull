# SmartCam Linux — Debug 总结文档

> 持续更新，记录 imx6ull-pro 开发板调试过程中遇到的所有问题。

---

## 9. 开发板按钮无法点击（linuxfb 无触摸输入）

| 属性 | 值 |
|------|-----|
| **模块** | Qt linuxfb 平台插件 / 输入事件 |
| **现象** | PC 端 (xcb) 按钮正常点击并响应，但 imx6ull 开发板上按钮无任何反馈 |
| **严重程度** | ❌ 严重 — GUI 完全不可操作 |

### 原因

`-platform linuxfb` 只负责将像素写入 `/dev/fb0` 帧缓冲区，**不处理任何输入事件**。触摸事件需要额外的 Qt 输入插件（`evdevtouch` / `tslib` / `libinput`）来从 `/dev/input/event*` 读取并转换为 Qt 事件。

PC 端默认使用 `xcb` 平台插件，由 X11 统一处理输入，开发者容易忽略此差异。

### 解决

运行前必须设置环境变量：

```bash
export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0
export QT_QPA_FB_HIDECURSOR=1
# ⚠️ 关键：指定触摸输入设备
export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=/dev/input/event1:rotate=0
# 如不生效，显式加载插件:
export QT_QPA_GENERIC_PLUGINS=evdevtouch

./smartcam --device /dev/video0 --fmt mjpeg --http-port 8080
```

### 排查方法

```bash
# 1. 确认触摸设备节点（触摸屏幕看是否有 hex 输出）
ls /dev/input/event*
cat /dev/input/event1 | hexdump

# 2. 确认 evdevtouch 插件存在
find / -name "*evdevtouch*" 2>/dev/null

# 3. 开启 Qt 输入调试日志
export QT_LOGGING_RULES="qt.qpa.input=true"
./smartcam ... 2>&1 | grep -iE "touch|event|input"
```

**涉及文件：** `README.md`、`scripts/setup-vm.sh`、`docs/01-display-module-implementation.md`、`CMakeLists.txt`

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

