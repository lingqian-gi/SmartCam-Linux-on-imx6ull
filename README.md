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
| 本地 GUI | Qt5 Widgets 界面，适配 7 寸 800x480 触摸屏；含亮度/对比度/白平衡设置弹窗 |
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
# Ubuntu / Debian（qt5-default 在较新发行版已废弃，使用 qtbase5-dev）
sudo apt install build-essential cmake qtbase5-dev libjpeg-dev

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

运行单元测试：

```bash
cd build/pc
ctest --output-on-failure   # TCP 控制协议单元测试
```

### ARM 交叉编译

SmartCam 提供两种交叉编译方式，根据你的开发环境选择。`scripts/` 下提供了一整套配套脚本：

| 脚本 | 在哪执行 | 职责 |
|------|---------|------|
| `scripts/sysroot-from-board.sh` | 开发板上 | 打包板子根文件系统 → `npi-sysroot.tar.gz`（>90MB 自动分包）→ git push 中转 |
| `scripts/setup-sysroot.sh` | x86 宿主 | 通过 ssh 直连开发板打包并拉回 sysroot（替代 GitHub 中转，适合能连到板子） |
| `scripts/sysroot-setup.sh` | 云编译环境 | **一键总控**：解压 sysroot + docker build + docker run 编译 |
| `scripts/cross-build.sh` | Docker 容器内 | **编译执行体**：patch sysroot cmake 配置 + cmake + make（被 Dockerfile CMD 调用） |
| `scripts/build.sh` | 宿主机 | PC 本地编译（`build.sh pc`）/ 宿主机交叉编译（`build.sh arm`） |
| `scripts/check-deps.sh` | 宿主机 | 检查编译依赖是否齐全 |

**sysroot 获取的两种路径**：

```
路径 A（GitHub 中转，适合云环境无法直连板子）:
  板子: sysroot-from-board.sh → git push → GitHub
  云上: git pull → sysroot-setup.sh（Step1 自动合并分包解压）

路径 B（ssh 直连，能连到板子时）:
  宿主: setup-sysroot.sh debian@<IP> → 板子打包 → scp 拉回 → 同样用 sysroot-setup.sh 继续
```

---

**方式一：Docker sysroot 交叉编译（GitHub 中转，适配老旧板子系统）**

适用于远程开发（如 CNB 云端环境）无法直连开发板，或板子系统库版本与 Docker 不匹配的场景。核心思路：通过 **GitHub 仓库** 中转，把开发板的根文件系统（sysroot）传到编译环境。

**整体链路：**

```
开发板 ──[tar + git push]──▶ GitHub ──[git pull]──▶ 云端编译环境
```

**step 1 — 在开发板上打包 sysroot 并推送**

```bash
# 确保在项目根目录
cd ~/smartcam/SmartCam-Linux-on-imx6ull

# 打包系统库 + Qt5 + libjpeg
tar czf npi-sysroot.tar.gz /lib /usr/lib /usr/include /usr/local /opt 2>/dev/null

# 分包（避免 GitHub 单文件 100MB 限制 + 降低 git 内存压力）
split -b 10M npi-sysroot.tar.gz npi-sysroot.part-
rm npi-sysroot.tar.gz
echo "npi-sysroot.tar.gz" >> .gitignore

# 提交并推送（关闭 delta 压缩以节省板子内存）
git add npi-sysroot.part-* .gitignore
git commit -m "add board sysroot (split parts)"

# ⚡ i.MX6ULL 内存仅 512MB，git 默认 delta 压缩会触发 OOM
git config http.postBuffer 52428800
git -c pack.window=0 -c pack.depth=0 push
```

> **关于板子内存不足**：`git push` 默认启用 delta 压缩，i.MX6ULL 只有 512MB 物理内存（CMA 占去 ~327MB，可用不足 200MB），压缩时会被 OOM killer 杀掉。`-c pack.window=0 -c pack.depth=0` 关闭 delta 压缩，CPU 和内存无压力，代价是推送体积增大，但板子 WiFi 速度慢（~780 KiB/s）才是瓶颈；实际测试 10MB 分包 + 无压缩推送，每次只需上传少量数据，耗时约 5~10 分钟。

**step 2 — 在编译环境拉取并交叉编译**

推荐直接用一键脚本（自动完成"合并分包 → 构建镜像 → 编译"三步）：

```bash
cd SmartCam-Linux-on-imx6ull
git pull
./scripts/sysroot-setup.sh          # 一键：解压 sysroot + docker build + docker run
# 产物: build/arm/smartcam
```

分步执行（了解内部机制时用）：

```bash
# ① 合并分包并解压
cat npi-sysroot.part-* > npi-sysroot.tar.gz
mkdir -p npi-sysroot
tar xzf npi-sysroot.tar.gz -C npi-sysroot

# ② 构建 Docker 镜像（Debian Buster = glibc 2.28 / Qt 5.11.3，与开发板完全一致）
docker build -f Dockerfile.arm-sysroot -t smartcam-cross-sysroot .

# ③ 编译（容器内执行 scripts/cross-build.sh：patch sysroot → cmake → make）
docker run --rm -v $(pwd):/workspace smartcam-cross-sysroot
```

> **为什么用 Debian Buster**：`Dockerfile.arm-sysroot` 基于 `debian:buster`（glibc 2.28 / Qt 5.11.3），与 i.MX6ULL 开发板逐版本一致。镜像内安装**宿主 x86_64 的 Qt5 工具**（moc/rcc/uic，版本 5.11），配合 ARM gcc-8 交叉编译；`cross-build.sh` 会自动把 sysroot 里的 ARM 工具软链替换为宿主工具、并 patch cmake 配置——**moc 生成代码与板子 Qt 头文件版本严格匹配**，避免"新版本 moc 生成 `QMetaObject::SuperData` 等新 API 导致编译失败"的问题。

**产物验证**：

```bash
readelf -d build/arm/smartcam | grep NEEDED    # 依赖 Qt 5.11 soname + glibc
readelf --version-info build/arm/smartcam | grep -oE 'GLIBC_[0-9.]+' | sort -V | uniq | tail
# 最高应为 GLIBC_2.28（与板子一致）；libjpeg / libstdc++ / libgcc 已静态链接
```

> **注意**：`npi-sysroot/` 目录在编译过程中会被 `cross-build.sh` 修改（工具软链 + cmake 配置 patch，均有 `.bak` 备份、幂等），可随时从 `npi-sysroot.tar.gz` 重建。`.dockerignore` 已排除 sysroot 与分包，避免拖慢镜像构建。

---

**方式二：宿主机直接编译（无需 Docker）**

```bash
# 安装交叉编译器
sudo apt install -y gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf

# 安装 ARM Qt5（关键步骤）
sudo dpkg --add-architecture armhf
sudo apt install -y qtbase5-dev:armhf libjpeg-dev:armhf

# 编译
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

> **重要**：imx6ULL 无 X server，必须使用 `linuxfb` 后端。linuxfb 内置了 evdev 输入支持，会自动检测 `/dev/input/` 下的触摸设备，**不需要 `QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS` 等环境变量**。触摸不响应的唯一原因是 **设备权限不足**。

```bash
# ---- 解决触摸权限问题 ----
# /dev/input/event2 默认权限 660 (root:input)，普通用户无权读取

# ① 临时修复（重启失效）
sudo chmod 666 /dev/input/event2

# ② 永久修复（将当前用户加入 input 组，重新登录生效）
sudo usermod -a -G input $USER
# 退出重新登录后验证：
groups | grep input

# ---- 启动应用（重要：必须使用板厂手动 Qt 套，否则 linuxfb 光标初始化崩溃）----
# 板子上存在两套 Qt 5.11.3：
#   - Debian 官方套 (/usr/lib/arm-linux-gnueabihf)：无内置光标资源，linuxfb 显示时
#     QPlatformCursorImage::set 对空 QImage 做 operator= → Segfault
#   - 板厂手动套 (/usr/lib)：内置完整光标位图 + tslib 触摸支持（推荐）
# 必须用 LD_LIBRARY_PATH 强制加载板厂手动套 + 配套插件，详见 docs/debug-summary.md #25
unset LD_LIBRARY_PATH QT_QPA_PLATFORM_PLUGIN_PATH
export LD_LIBRARY_PATH=/usr/lib
export QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/plugins/platforms
export QT_QPA_PLATFORM=linuxfb

# MJPEG 模式（摄像头硬件输出 JPEG，零 CPU 编码开销，推荐）
./smartcam --device /dev/video0 --fmt mjpeg --http-port 8080

# ⚠️⚠️ 重要警告：上面的 export LD_LIBRARY_PATH=/usr/lib 会污染整个 shell！
#   之后在这个终端里运行 git / curl / apt 等命令会报错：
#     - git pull → "Error -50 setting GnuTLS cipher list"
#     - curl   → "symbol curl_url_get version CURL_OPENSSL_4 not defined"
#   原因：板子上 /usr/lib 下还有一套手动装的 OpenSSL 版 libcurl，
#   LD_LIBRARY_PATH 让 git/curl 加载了错误版本（详见 docs/debug-summary.md #26）。
#   解决：跑完 smartcam 后立即 unset，或改用"单行临时设置"方式：
#     LD_LIBRARY_PATH=/usr/lib QT_QPA_PLATFORM=linuxfb ./smartcam --device /dev/video0 --fmt mjpeg --http-port 8080
#   强烈建议用单行方式，避免污染后续命令。

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
cat /dev/input/event2 | hexdump    # event2 换成你的实际设备
# 查看 Qt 输入调试日志：
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
│   │   ├── capture.cpp          # V4L2 采集引擎（mmap 零拷贝、4 缓冲池、双格式）
│   │   ├── processor.cpp        # 图像处理（YUV 转换、libjpeg-turbo 编解码、JPEG 解码）
│   │   └── processor_neon.cpp   # YUYV→RGB NEON SIMD 加速（仅 ARM 交叉编译启用）
│   ├── display/
│   │   ├── gui.cpp              # Qt5 相机界面（预览、拍照、录像、设置弹窗）
│   │   ├── gallery.cpp          # 相册组件（缩略图网格 + 全屏查看 + 多选删除）
│   │   └── video_player.cpp     # 轻量 AVI 播放器（手写 RIFF 解析，无 ffmpeg 依赖）
│   ├── network/
│   │   ├── mjpeg_server.cpp     # MJPEG-over-HTTP 流（multipart/x-mixed-replace）
│   │   ├── rtsp_server.cpp      # RFC 2326 RTSP 服务器（RTP/RTCP、RFC 2435 JPEG 载荷）
│   │   └── control.cpp          # TCP 二进制控制协议（CRC16、epoll ET、命令分发）
│   ├── storage/
│   │   └── manager.cpp          # 存储管理（拍照、AVI 录像、磁盘空间管理）
│   └── main.cpp                 # 程序入口，线程编排（采集/处理/解码/网络/显示）
├── include/
│   ├── camera/    (capture.h, processor.h)
│   ├── display/   (gui.h, gallery.h, video_player.h)
│   ├── network/   (mjpeg_server.h, rtsp_server.h, control.h)
│   ├── storage/   (manager.h)
│   └── common/    (types.h, ringbuf.h, logger.h, config.h)
├── configs/
│   ├── smartcam.conf            # 主配置文件（INI 格式）
│   ├── smartcam.service         # systemd 服务单元
│   └── toolchain.arm.cmake      # ARM 交叉编译工具链文件
├── scripts/
│   ├── build.sh                 # PC / 宿主机 ARM 编译
│   ├── check-deps.sh            # 编译依赖检查
│   ├── cross-build.sh           # Docker 容器内编译（patch sysroot + cmake + make）
│   ├── setup-sysroot.sh         # ssh 直连板子导出 sysroot
│   ├── sysroot-from-board.sh    # 板端打包 sysroot 并 git push
│   └── sysroot-setup.sh         # 一键总控（解压 sysroot + docker build + run）
├── tests/
│   ├── CMakeLists.txt
│   └── test_protocol.cpp        # TCP 二进制协议单元测试
├── docs/
│   ├── 01~11-*-implementation.md         # 各模块实现文档（display/camera/mjpeg/…）
│   ├── 02-cmake-build-system-tutorial.md # CMake 构建系统教程
│   ├── debug-summary.md                  # 调试问题总结
│   ├── rtsp-protocol-learning-notes.md   # RTSP 协议学习笔记
│   ├── changelog-2026-05-29.md           # 更新日志
│   ├── plan-gallery-module.md            # 相册模块实现计划
│   ├── plan-pxp-acceleration.md          # PXP 硬件加速计划（当前未启用）
│   ├── plan-frame-pool-zero-copy.md      # 帧池零拷贝计划（双缓冲 + 引用计数）
│   ├── interview/                        # 面试问答
│   │   └── 01-模拟面试问答-嵌入式Linux.md
│   ├── learn/                            # 模块面试复习
│   │   ├── 面试复习-camera模块.md
│   │   ├── 面试复习-display模块.md
│   │   └── 面试项目介绍.md
│   └── 求职项目-智能相机流媒体系统.md
├── CODE_WALKTHROUGH.md        # 逐文件源代码精读
├── CMakeLists.txt
├── Dockerfile.arm-sysroot     # ARM 交叉编译 Docker 镜像（Debian Buster）
├── .dockerignore              # 排除 sysroot/分包进 Docker 构建上下文
└── README.md
```

---

## 系统架构

共 **6 个线程**：Qt 主线程（GUI）+ 采集线程 + 处理线程 + **解码线程** + RTSP 线程 + 控制线程。

```
采集线程 (captureThread)              处理线程 (processThread)            网络线程
=======================             ===========================          ==========
V4L2 dqbuf → mmap 帧                等 procCv 通知                      epoll_wait
  ├─► g_state.frameData             ├─ MJPEG: JPEG 直通推流             ├─ HTTP: multipart JPEG
  │    (mutex + frameSeq 序号)       ├─ YUYV: encodeYUYVtoJPEG           ├─ RTSP: RTP 分片
  └─► putFrame 归还 mmap             └─ 录像: 写 AVI（仅 MJPEG）          └─ TCP: 命令分发

解码线程 (decodeThread)              GUI 线程 (Qt 主线程)
=====================              ====================
  ├─ share() 最新原始帧              ├─ displayTimer(33ms)
  ├─ MJPEG 解码 / YUYV→RGB24         │    └─ 拉 g_display → setFrame(RGB24)
  └─ publish → g_display (RGB)       └─ m_refreshTimer(33ms)
                                        └─ QImage 浅引用 → setPixmap → 上屏
```

**线程同步**：
- 采集线程把帧深拷贝进 `g_state.frameData`（`std::mutex` 保护），写完递增 `frameSeq` 序号后立即归还 V4L2 mmap 缓冲；
- 处理线程与解码线程各自从 `g_state` 取最新帧（互不阻塞），处理线程推流/录像，解码线程负责显示转换；
- **解码线程是独立 `std::thread`**（MJPEG ~25ms 解码不再占用 GUI 线程），解码结果发布到 `g_display`（RGB24 + mutex）；
- GUI 线程从 `g_display` 取 RGB24 帧，`setFrame()` 内深拷贝一次（`m_frameBuffer`），`refreshFrame()` 里 `QImage` 构造后 `.copy()` 深拷贝，**双重深拷贝避免悬垂指针**。

**零拷贝路径**：
- MJPEG 模式推流/录像：摄像头硬件直接输出 JPEG，处理线程**原样转发**到 HTTP/RTSP、直写 AVI——完全零编码零解码（仅本地显示需解一次码，在解码线程进行）。
- YUYV 模式：处理线程每帧调用一次 `encodeYUYVtoJPEG()`，编码结果供 HTTP 和 RTSP 共用；显示路径由解码线程做 YUYV→RGB24。

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
| 图像处理 | YUYV 转 RGB24（定点运算 BT.601）、libjpeg-turbo 编解码（NEON 加速）、JPEG 解码（自定义静默错误处理器） |
| **显示解码线程** | MJPEG/YUYV → RGB24 在独立 `std::thread` 完成（`VideoProcessor::decodeJPEGtoRGB`），GUI 线程只做拷贝 + `setPixmap`，事件循环不被解码阻塞 |
| 存储管理 | AVI RIFF 容器格式（含 idx1 索引块）、按修改时间自动清理、按日期分目录存储 |
| 配置解析 | Header-only INI 解析器，支持分段、注释、bool/int/string 类型 |
| systemd 服务 | Type=simple、崩溃自动重启、安全加固（ProtectSystem、RestrictAddressFamilies 等） |
| 相册 | 3 列缩略图网格、libjpeg scale_denom 快速解码、触摸滑动翻页、删除确认弹窗 |
| AVI 播放 | 手写 RIFF 解析 + idx1 索引 O(1) seek，复用 libjpeg 解码，零 ffmpeg 依赖 |

---

## 性能数据（iMX6ULL Cortex-A7 @ 792MHz）

| 操作 | 640x480 | 说明 |
|------|---------|------|
| MJPEG 硬件输出 | < 1ms | USB UVC 摄像头直出 |
| YUYV 转 RGB24 | ~5ms | 定点运算，无查表法 |
| libjpeg-turbo 编码 | ~25ms | NEON 加速 |
| JPEG 显示解码 | ~25ms | 在**独立解码线程**执行，不阻塞 GUI |
| JPEG 缩略图解码 | ~15ms | Scale 1/2 缩小到 170px |
| 运行内存（推流） | ~8 MB | 帧缓冲 + JPEG 拷贝 |
| 相册峰值内存 | ~2.5 MB | 6 张可见缩略图 + 1 张全尺寸 |

> **显示帧率上限**：本地预览受 `m_refreshTimer` 固定 33ms 节拍限制，最多 30fps（匹配 MJPEG 解码能力）；**推流/录像帧率不受此限制**，跟随采集目标帧率（最高 60fps）。升 720p 需先"解码移出 GUI 线程"（已完成）并评估 CPU。

---

## 许可证

MIT
