# SmartCam Linux — 基于 iMX6ULL 的智能相机流媒体系统

基于野火 iMX6ULL Pro 开发板，搭载 7 寸电容触摸屏和 USB 摄像头（YUV/MJPEG），构建一个完整的嵌入式智能相机系统。

---

## 项目概述

SmartCam Linux 将低成本的 ARM Cortex-A7 开发板变成了功能完备的网络相机，支持本地触摸屏交互、多协议流媒体推流、远程控制指令和照片管理。

**核心功能**

| 功能 | 说明 |
|------|------|
| 视频采集 | V4L2 引擎，支持 MJPEG 硬件直出和 YUYV 原始格式双模式 |
| 流媒体服务器 | MJPEG-over-HTTP（浏览器直接观看）+ RFC 2326 RTSP/RTP（VLC/ffplay 播放） |
| 远程控制 | 自定义 TCP 二进制协议，含 CRC16 校验和 epoll 边缘触发 |
| 本地 GUI | Qt5 Widgets 界面，适配 7 寸 800x480 触摸屏 |
| 相册管理 | 缩略图网格浏览、全屏查看、删除已拍摄照片 |
| 存储管理 | JPEG 拍照保存 + AVI（MJPEG）录像，支持自动清理旧文件 |
| 系统集成 | systemd 服务管理、开机自启、安全加固、journald 日志 |
| 配置文件 | INI 格式配置文件，优先级：命令行 > 配置文件 > 硬编码默认值 |
| 跨平台开发 | PC Mock 模式可在无硬件环境下调试 UI |

---

## 硬件需求

| 组件 | 说明 |
|------|------|
| 开发板 | 野火 iMX6ULL Pro（Cortex-A7 @ 792MHz，512MB DDR3） |
| 屏幕 | 7 寸电容触摸屏（800x480，framebuffer `/dev/fb0`） |
| 摄像头 | USB UVC 摄像头，支持 MJPEG 和/或 YUYV 格式 |
| 存储 | SD 卡或 eMMC，需要 `/data` 分区存放照片和录像 |
| 操作系统 | Linux（Yocto / Buildroot，内核需支持 UVC + V4L2 + framebuffer） |

---

## 快速开始

### 安装依赖

```bash
# Ubuntu / Debian
sudo apt install build-essential cmake qt5-default libjpeg-dev

# Fedora
sudo dnf install gcc-c++ cmake qt5-qtbase-devel libjpeg-turbo-devel
```

### PC 端编译（本地调试）

```bash
cd SmartCam-Linux-on-imx6ull
scripts/build.sh          # PC Mock 模式
```

或手动编译：

```bash
mkdir build/pc && cd build/pc
cmake ../.. && make -j$(nproc)
./smartcam                # Mock 模式 — 显示彩色测试条
```

### ARM 交叉编译

```bash
# 先修改 scripts/build.sh 中的 ARM_TOOLCHAIN 为你的交叉编译器路径
scripts/build.sh arm
```

然后部署到开发板：

```bash
# 打包
cd build/arm
make install DESTDIR=/tmp/smartcam-pkg
cd /tmp/smartcam-pkg && tar czf smartcam-arm.tar.gz .

# 拷贝到开发板并解压
scp smartcam-arm.tar.gz root@<开发板IP>:/tmp/
ssh root@<开发板IP> "cd / && tar xzf /tmp/smartcam-arm.tar.gz"
```

### 开发板运行

> **重要**：imx6ULL 无 X server，必须使用 `linuxfb` 后端。linuxfb 只负责显示，**触摸/鼠标输入需额外配置 `QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS`，否则按钮无法点击。**

```bash
# ---- 必须先设置输入设备环境变量 ----
export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0
export QT_QPA_FB_HIDECURSOR=1

# ⚠️ 关键：指定触摸输入设备路径（根据实际设备调整 /dev/input/event1）
export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=/dev/input/event1:rotate=0
# 如果上面不生效，尝试同时加载 evdevtouch 插件：
# export QT_QPA_GENERIC_PLUGINS=evdevtouch

# ---- 启动应用 ----
# MJPEG 模式（摄像头硬件输出 JPEG，零 CPU 编码开销，推荐）
./smartcam --device /dev/video0 --fmt mjpeg --http-port 8080

# YUYV 模式（libjpeg-turbo 软件编码后推流）
./smartcam --device /dev/video0 --fmt yuyv --http-port 8080

# 自定义端口
./smartcam --device /dev/video0 --http-port 9090 --rtsp-port 9554

# 使用自定义配置文件
./smartcam --config /home/root/myconfig.conf --device /dev/video0

# ---- 排查触摸输入 ----
# 查看可用的输入设备：
ls -la /dev/input/event*
# 测试触摸是否工作（触摸屏幕看是否有输出）：
cat /dev/input/event1 | hexdump
# 检查 Qt 是否加载了 evdevtouch 插件：
export QT_LOGGING_RULES="qt.qpa.input=true"
./smartcam --device /dev/video0 --fmt mjpeg --http-port 8080 2>&1 | grep -i touch
```

若使用 systemd 管理（推荐用于实际部署）：

```bash
sudo systemctl enable smartcam
sudo systemctl start smartcam
journalctl -u smartcam -f   # 查看日志
```

### 观看视频流

| 客户端 | 地址 / 命令 |
|--------|-------------|
| 浏览器 | `http://<开发板IP>:8080/` |
| 快照（单帧） | `http://<开发板IP>:8080/snapshot` |
| 状态 JSON | `http://<开发板IP>:8080/status` |
| VLC 播放器 | `rtsp://<开发板IP>:8554/stream` |
| ffplay | `ffplay rtsp://<开发板IP>:8554/stream` |

---

## 项目结构

```
SmartCam-Linux-on-imx6ull/
├── src/
│   ├── camera/
│   │   ├── capture.cpp        # V4L2 采集引擎（mmap 零拷贝、双格式）
│   │   └── processor.cpp      # 图像处理（YUV 转换、libjpeg-turbo 编解码）
│   ├── display/
│   │   ├── gui.cpp            # Qt5 相机界面（预览、拍照、录像、设置）
│   │   └── gallery.cpp        # 相册组件（缩略图网格 + 全屏查看）
│   ├── network/
│   │   ├── mjpeg_server.cpp   # MJPEG-over-HTTP 流（multipart/x-mixed-replace）
│   │   ├── rtsp_server.cpp    # RFC 2326 RTSP 服务器（RTP/RTCP、RFC 2435 JPEG 载荷）
│   │   └── control.cpp        # TCP 二进制控制协议（CRC16、epoll ET、命令分发）
│   ├── storage/
│   │   └── manager.cpp        # 存储管理（拍照、AVI 录像、磁盘空间管理）
│   └── main.cpp               # 程序入口，各模块编排和线程管理
├── include/
│   ├── camera/    (capture.h, processor.h)
│   ├── display/   (gui.h, gallery.h)
│   ├── network/   (mjpeg_server.h, rtsp_server.h, control.h)
│   ├── storage/   (manager.h)
│   └── common/    (types.h, ringbuf.h, logger.h, config.h)
├── configs/
│   ├── smartcam.conf          # 主配置文件（INI 格式）
│   └── smartcam.service       # systemd 服务单元
├── scripts/
│   └── build.sh               # 构建脚本（PC / ARM 交叉编译）
├── tests/
│   └── test_protocol.cpp      # TCP 二进制协议单元测试
├── docs/
│   ├── 01-display-module-implementation.md
│   ├── 02-video-capture-module-implementation.md
│   ├── 03-mjpeg-stream-module-implementation.md
│   ├── 04-storage-module-implementation.md
│   ├── 05-control-module-implementation.md
│   ├── 06-common-module-implementation.md
│   ├── 07-video-processor-module-implementation.md
│   ├── 08-rtsp-module-implementation.md
│   ├── 09-gallery-module-implementation.md
│   ├── debug-summary.md
│   ├── plan-gallery-module.md
│   └── 求职项目-智能相机流媒体系统.md
├── CMakeLists.txt
└── README.md
```

---

## 系统架构

```
 采集线程                   Qt 主线程                    网络线程
 =========                 ==========                  ==========
 V4L2 dqbuf               displayTimer (33ms)         epoll_wait
   |                          |                          |
   |-> frameData.assign()     |-> lock(g_state)          |-> HTTP: 推送 JPEG
   |-> MJPEG: 直接推流         |-> setFrame()             |-> RTSP: RTP 分片
   |-> YUYV: 编码为 JPEG      |-> unlock                 |-> TCP: 命令分发
   |-> Storage: 拍照/录像     |-> refreshFrame()         |
   |-> unlock(g_state)        |-> QImage -> QLabel       |
```

**线程同步**：使用 `std::mutex` 保护共享帧数据；`setFrame()` 内部深拷贝数据，避免悬垂指针。

**零拷贝路径**：
- MJPEG 模式：摄像头硬件直接输出 JPEG，无需编码即推送到 HTTP 和 RTSP。
- YUYV 模式：每帧调用一次 `encodeYUYVtoJPEG()`，编码结果供 HTTP 和 RTSP 共用。

---

## 配置文件

配置优先级：**命令行 > 配置文件 > 硬编码默认值**

```ini
# /etc/smartcam/smartcam.conf
[camera]
device = /dev/video0
format = mjpeg          # yuyv | mjpeg

[network]
http_port = 8080
rtsp_port = 8554
control_port = 9000

[storage]
photo_dir = /data/photos
video_dir = /data/videos
auto_cleanup = true

[logging]
level = info             # debug | info | warn | error
use_syslog = true
```

---

## TCP 控制协议

自定义二进制协议，用于远程控制相机。帧格式如下：

```
| 魔数[2]  | 版本[1] | 命令[1] | 负载长度[2] | 负载[N] | CRC16[2] |
| 0xEB 0x90|   0x01  |         | (大端序)    |         | (大端序)  |
```

**支持的命令**：拍照 (0x01)、开始/停止录像 (0x02/0x03)、设置分辨率 (0x10)、设置格式 (0x11)、查询状态 (0x20)、心跳 (0xFF)。

详见：`docs/05-control-module-implementation.md`

---

## 关键技术点

| 模块 | 技术实现 |
|------|----------|
| V4L2 采集 | mmap 零拷贝、4 缓冲区轮转池、运行时格式/分辨率切换 |
| MJPEG 流 | HTTP multipart/x-mixed-replace、条件变量广播、`/snapshot` 和 `/status` 端点 |
| RTSP 流 | 自实现 RFC 2326 协议栈（DESCRIBE/SETUP/PLAY/TEARDOWN）、RTP RFC 2435 JPEG 载荷、epoll 边缘触发 |
| 图像处理 | YUYV 转 RGB24（定点运算 BT.601）、libjpeg-turbo 编解码（NEON 加速）、自定义静默错误处理器 |
| 存储管理 | AVI RIFF 容器格式（含 idx1 索引块）、按修改时间自动清理、按日期分目录存储 |
| 配置解析 | Header-only INI 解析器，支持分段、注释、bool/int/string 类型 |
| systemd 服务 | Type=simple、崩溃自动重启、安全加固（ProtectSystem、RestrictAddressFamilies 等） |
| 相册 | 3 列缩略图网格、libjpeg scale_denom 快速解码、触摸滑动翻页、删除确认弹窗 |

---

## 性能数据（iMX6ULL Cortex-A7 @ 792MHz）

| 操作 | 640x480 | 说明 |
|------|---------|------|
| MJPEG 硬件输出 | < 1ms | USB UVC 摄像头直出 |
| YUYV 转 RGB24 | ~5ms | 定点运算，无查表法 |
| libjpeg-turbo 编码 | ~25ms | NEON 加速 |
| JPEG 缩略图解码 | ~15ms | Scale 1/2 缩小到 170px |
| 运行内存（推流） | ~8 MB | 帧缓冲 + JPEG 拷贝 |
| 相册峰值内存 | ~2.5 MB | 6 张可见缩略图 + 1 张全尺寸 |

---

## 许可证

MIT
