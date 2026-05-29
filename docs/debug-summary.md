# SmartCam Linux — Debug 总结文档

> 持续更新，记录 imx6ull-pro 开发板调试过程中遇到的所有问题。

---

## 1. GUI 不支持的像素格式 (MJPG) ✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` |
| **现象** | GUI 不显示 MJPEG 格式的画面 |
| **严重程度** | ❌ 严重 — 画面不显示 |

### 解决

`frameToQImage()` 添加 `FMT_MJPEG` 解码分支

---

## 2. Corrupt JPEG data 警告刷屏 ✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` |
| **现象** | 控制台不断输出 `Corrupt JPEG data` 警告 |
| **严重程度** | ⚠️ 次要 |

### 解决

自定义 libjpeg 错误处理器静默所有输出

---

## 3. ARM 交叉编译 jpeglib.h 包含位置错误 ✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | `src/camera/processor.cpp` |
| **现象** | ARM 交叉编译时 `jpeglib.h` 找不到 |
| **严重程度** | ❌ 严重 — 编译不通过 |

### 解决

`#include <jpeglib.h>` 移到文件顶部

---

## 4. 线程自 join 死锁 (EDEADLK) ✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | `src/network/mjpeg_server.cpp` |
| **现象** | 进程在退出时 abort，错误码 `EDEADLK` |
| **严重程度** | ❌ 严重 — 进程 abort |

### 解决

`detach` 模式替代 `thread->join()`

**涉及文件：** `src/network/mjpeg_server.cpp`

---

## 5. GUI 显示闪烁 / 坏帧 ✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | `src/display/gui.cpp` |
| **现象** | GUI 画面闪烁、出现坏帧 |
| **严重程度** | ❌ 严重 |

### 解决

深拷贝帧数据 + libjpeg 自定义静默错误处理器

**涉及文件：** `src/display/gui.cpp`、`include/display/gui.h`

---

## 6. YUYV 格式推流打通 ✅ 已解决

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

## 6b. systemd 服务文件完善 ✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | `configs/smartcam.service` |
| **现象** | `Type=forking` 不匹配，`ExecStop` 引用不存在子命令 |
| **严重程度** | — |

### 解决

`Type=simple` + 删除 `ExecStop` + 新增安全加固

**涉及文件：** `configs/smartcam.service`

---

## 7. 配置文件解析实现 ✅ 已解决

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

## 8. 相册模块实现 ✅ 已解决

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

## 9. 触摸屏无响应（开发板）✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | Qt linuxfb 内置 evdev 输入 / 设备权限 |
| **现象** | 开发板上 GUI 正常显示，但触摸按钮无任何反馈 |
| **严重程度** | ❌ 严重 — GUI 完全不可操作 |

### 原因

**设备权限不足**，与 Qt 插件无关。`/dev/input/event2` 默认属主 `root:input`，权限 `660`，当前用户 `debian` 不在 `input` 组中，无法读取触摸事件。

> linuxfb 后端**内置了 evdev 输入支持**，会自动检测 `/dev/input/` 下的触摸设备，**不需要**任何环境变量或额外插件。

### 解决（永久）

```bash
sudo usermod -a -G input debian
# 重新登录或重启生效
```

### 解决（临时，重启失效）

```bash
sudo chmod 666 /dev/input/event2
```

### 正确的启动命令

```bash
# 不需要任何环境变量！
./smartcam --device /dev/video0 --fmt mjpeg -platform linuxfb
```

### 排查方法

```bash
# 1. 确认触摸设备节点
ls -la /dev/input/event*
# 触摸屏幕看是否有输出 → 有输出说明内核驱动正常，只是用户无权限
cat /dev/input/event2 | hexdump

# 2. 确认当前用户在 input 组
groups | grep input

# 3. 确认 linuxfb 已识别触摸设备（看日志有无 "evdev" 字样）
export QT_LOGGING_RULES="qt.qpa.input=true"
./smartcam -platform linuxfb 2>&1 | head -20
```

### 常见误区 ❌

以下做法**不仅无效，还会导致 segfault**：

- `export QT_QPA_GENERIC_PLUGINS=evdevtouch` — linuxfb 内置了 evdev 支持，手动加载会**冲突**导致崩溃
- `export QT_PLUGIN_PATH=...` — 覆盖默认插件搜索路径，导致其他必需插件丢失
- `export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=...` — 无头附加无插件的参数，无作用

**涉及文件：** `docs/debug-summary.md`

---

## 10. Gallery 不显示视频文件 ✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | `src/storage/manager.cpp` / `src/display/gallery.cpp` |
| **现象** | 拍照后在 Gallery 正常看到照片，但录像后 Gallery 看不到任何视频文件 |
| **严重程度** | ⚠️ 中等 — 用户无法确认录像是否成功保存 |

### 原因

`StorageManager` 只实现了 `listPhotos()`（扫描 `m_photoDir` 下的 `.jpg`），完全没有视频列表功能。视频虽然被录制到 `m_videoDir` 下的 `.avi` 文件，但 Gallery 的 `refresh()` 只调 `listPhotos()`，从不查询视频目录。

### 解决

1. **`PhotoInfo` 新增 `isVideo` 字段** — 标记媒体类型
2. **`StorageManager` 新增 3 个方法**：
   - `listVideos()` — 扫描 `m_videoDir` 下的 `.avi` 文件，逻辑与 `listPhotos` 对称
   - `getVideoCount()` — 快速统计视频数量
   - `deleteVideo()` — 删除视频文件并清理空目录
3. **`PhotoGallery::refresh()` 合并照片+视频列表** — 按时间戳混排，重建日期分组
4. **缩略图网格视频区分**：视频项显示 ▶ 绿箭头图标 + 底部 `[VID]` 标签
5. **全屏视图视频占位**：显示 ▶ + 文件名 + 大小（AVI 暂不渲染缩略图）
6. **删除逻辑适配**：`onDeletePhoto()` 根据 `isVideo` 调用不同删除方法

### 涉及文件

`include/storage/manager.h`、`src/storage/manager.cpp`、`src/display/gallery.cpp`

---

## 11. 视频播放器编译错误：AviIndexEntry/AviMainHeader 非类成员 ✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | `include/display/video_player.h` / `src/display/video_player.cpp` |
| **现象** | `'AviIndexEntry' is not a member of 'StorageManager'` — 所有引用处报错，连 `gui.cpp` / `main.cpp` 也因间接 include 被连带 |
| **严重程度** | ❌ 严重 — 编译不通过 |

### 原因

`video_player.h` 和 `video_player.cpp` 中使用了 `StorageManager::AviIndexEntry` 和 `StorageManager::AviMainHeader`，但这些结构体定义在 `StorageManager` **类外部**（`manager.h` 中类声明之前，全局命名空间）：

```cpp
// include/storage/manager.h

#pragma pack(push, 1)      // ← 全局作用域

struct RiffChunk { ... };
struct AviMainHeader { ... };   // ← 全局 struct，不属于 StorageManager
struct AviStreamHeader { ... };
struct BitmapInfoHeader { ... };
struct AviIndexEntry { ... };   // ← 全局 struct，不属于 StorageManager
struct AviFrameChunk { ... };

#pragma pack(pop)

class StorageManager {          // ← 类定义从这里才开始
    ...
    // PhotoInfo / PhotoDayGroup 等定义在类内部 → 需要 StorageManager:: 前缀
};
```

**区分规则**：
- `PhotoInfo`、`PhotoDayGroup` → 定义在 `StorageManager` 类内 → 必须用 `StorageManager::PhotoInfo`
- `AviMainHeader`、`AviIndexEntry`、`RiffChunk` 等 → 定义在类外（全局）→ **不能用** `StorageManager::` 前缀

### 解决

去掉所有 AVI 结构体的 `StorageManager::` 前缀（共 4 处）：

| 文件 | 错误写法 | 正确写法 |
|------|----------|----------|
| `video_player.h:101` | `std::vector<StorageManager::AviIndexEntry>` | `std::vector<AviIndexEntry>` |
| `video_player.cpp:334` | `StorageManager::AviMainHeader avih` | `AviMainHeader avih` |
| `video_player.cpp:388` | `sizeof(StorageManager::AviIndexEntry)` | `sizeof(AviIndexEntry)` |
| `video_player.cpp:391` | `sizeof(StorageManager::AviIndexEntry)` | `sizeof(AviIndexEntry)` |

### 涉及文件

`include/display/video_player.h`（1 处）、`src/display/video_player.cpp`（3 处）

---

## 12. ARM 交叉编译产物 GLIBC 版本不兼容 ✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | 交叉编译工具链 / Docker 镜像 |
| **现象** | 开发板运行交叉编译产物报：`GLIBCXX_3.4.29 not found`、`GLIBC_2.33 not found`、`GLIBC_2.34 not found` |
| **严重程度** | ❌ 严重 — 程序无法启动 |

### 原因

Docker 基于 Ubuntu 22.04（glibc 2.35, GCC 11），其 armhf 仓库中的 Qt5/libjpeg 都链接到 glibc 2.35。但 i.MX6ULL 开发板一般跑 Debian Buster/Bullseye（glibc 2.28~2.31），运行时报这些符号不存在。

| 依赖库 | Docker (Ubuntu 22.04 armhf) | 开发板 (Debian) |
|--------|---------------------------|-----------------|
| libstdc++ | GCC 11 → `GLIBCXX_3.4.29` | GCC 8/10 → `GLIBCXX_3.4.25` |
| glibc | 2.35 | 2.28~2.31 |

### 解决方案：sysroot 交叉编译

**核心思路**：不使用 Docker 中 Ubuntu armhf 包作为依赖，而是直接使用**开发板自身的根文件系统**作为 ARM 库来源。开发板上已有的 Qt5、libjpeg 和系统库，版本和板子完全匹配。

**完整链路**（适用于云端开发环境无法直连开发板）：

```
开发板 ──[tar + git push]──▶ GitHub ──[git pull]──▶ 云端编译环境
```

**解决方案涉及的代码改动：**

1. **`CMakeLists.txt`** — ARM 交叉编译时 `target_link_options(smartcam PRIVATE -static-libstdc++ -static-libgcc)` 消除 libstdc++ 依赖
2. **`configs/toolchain.arm.cmake`** — 支持 `CMAKE_SYSROOT`，允许指定开发板根文件系统路径
3. **`Dockerfile.arm-sysroot`**（新增）— 精简 Docker 镜像，只装交叉编译器 + cmake，不安装 armhf 包
4. **`.gitattributes` / `.gitignore`** — 管理 sysroot 分包文件
5. **`scripts/sysroot-from-board.sh`**（新增）— 在开发板上打包、提交并推送 sysroot 到 GitHub
6. **`scripts/sysroot-setup.sh`**（新增）— 在编译环境拉取、合并 sysroot 并执行交叉编译

**开发板打包注意事项（i.MX6ULL 内存不足问题）：**

板子仅 512MB 物理内存（CMA 占 327MB，可用 ~185MB），`git push` 时默认 delta 压缩会触发 OOM killer。

解决方案：
- `split -b 10M` — 分包到 10MB，减小单文件处理压力
- `git -c pack.window=0 -c pack.depth=0 push` — 关闭 delta 压缩，CPU/RAM 零压力
- `git config http.postBuffer 52428800` — 加大 HTTP 缓冲区，防止慢速网络超时

**使用方式：**

```bash
# Step 1: 在开发板上
cd ~/smartcam/SmartCam-Linux-on-imx6ull
./scripts/sysroot-from-board.sh

# Step 2: 在编译环境
git pull
./scripts/sysroot-setup.sh
```

---

## 13. sysroot 交叉编译：cmake/Qt5 工具链连环坑 ✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | `Dockerfile.arm-sysroot` / `scripts/cross-build.sh` / cmake 配置 |
| **现象** | `./scripts/sysroot-setup.sh` 依次报多个不同错误，每个错误修复后暴露下一个 |
| **严重程度** | ❌ 严重 — 编译完全中断 |

---

### 背景

sysroot 交叉编译方案在上节已设计完毕，但实际执行过程中遇到一系列意外问题。本节记录每个错误的现象、根因和修复。

---

### 坑 1：`qmake` 符号链接断裂 → CMake configure 失败

**错误信息：**

```
The imported target "Qt5::Core" references the file
  "/workspace/npi-sysroot/usr/lib/arm-linux-gnueabihf/qt5/bin/qmake"
but this file does not exist.
```

**根因：**

sysroot 中 `qmake` 的软链接指向了未被导出的目录：

```
npi-sysroot/usr/lib/arm-linux-gnueabihf/qt5/bin/qmake
  → ../../../../bin/arm-linux-gnueabihf-qmake
  → npi-sysroot/usr/bin/arm-linux-gnueabihf-qmake  ← 不存在！
```

导出 sysroot 的脚本（`sysroot-from-board.sh`）只捕获了 `/lib`、`/usr/lib`、`/usr/include` 等，**没有导出 `/usr/bin/`**。对比 `moc`/`rcc`/`uic` 的链接指向 `../../../qt5/bin/...`（在 `/usr/lib/` 下），均正常。

CMake `find_package(Qt5)` 加载 `Qt5CoreConfigExtras.cmake`，其中 `_qt5_Core_check_file_exists()` 检查 `qmake` 是否存在，文件不存在则 FATAL_ERROR。

**修复：**

在 `cross-build.sh` 中检测并修复断链，重定向到宿主 `qmake`：

```bash
QMAKE_SYMLINK="$SYSROOT/usr/lib/arm-linux-gnueabihf/qt5/bin/qmake"
if [ -L "$QMAKE_SYMLINK" ] && [ ! -e "$QMAKE_SYMLINK" ]; then
    rm -f "$QMAKE_SYMLINK"
    ln -s "$HOST_QMAKE" "$QMAKE_SYMLINK"
fi
```

---

### 坑 2：ARM 版 `moc` 通过 qemu-arm 执行 → 失败（make 阶段）

**错误信息：**

```
/workspace/npi-sysroot/usr/lib/arm-linux-gnueabihf/qt5/bin/moc ...

Output
------
qemu-arm: Could not open '/lib/ld-linux-armhf.so.3': No such file or directory
make[2]: *** [CMakeFiles/smartcam_autogen.dir/build.make:58: ...] Error 1
```

**根因：**

解决了坑 1 后，CMake configure 通过，进入 make 阶段。AUTOMOC 调用 `moc`，但 sysroot 中的 `moc` 是 ARM 二进制，宿主内核通过 `binfmt_misc` 用 `qemu-arm` 模拟执行，却找不到 ARM 动态链接器 `/lib/ld-linux-armhf.so.3`（qemu 默认在宿主 `/lib` 下寻找，而非 sysroot）。

> **注**：此时调用的 moc 路径仍然指向 sysroot 中的 ARM 二进制。仅靠替换软链接不足以解决——见坑 5。

**初步尝试：**

1. 安装 `qemu-user-static` 并设置 `QEMU_LD_PREFIX=$SYSROOT`
2. 这能解决 qemu 运行 ARM 二进制的问题，但**引出更严重的坑 3**

---

### 坑 3：glibc 版本冲突（link 阶段）

**错误信息：**

```
/usr/lib/gcc-cross/arm-linux-gnueabihf/11/../../../../arm-linux-gnueabihf/bin/ld:
  /workspace/npi-sysroot/lib/arm-linux-gnueabihf/libpthread.so.0:
    undefined reference to `__libc_current_sigrtmax_private@GLIBC_PRIVATE'
/usr/.../ld: /workspace/npi-sysroot/lib/arm-linux-gnueabihf/libdl.so.2:
    undefined reference to `_dl_vsym@GLIBC_PRIVATE'
collect2: error: ld returned 1 exit status
```

**根因：**

| | sysroot（来自开发板） | 交叉编译器 |
|------|------|------|
| glibc 版本 | 2.28（libpthread / libdl 独立 `.so`） | 2.35（libpthread / libdl 已合并进 libc） |
| 机制 | 链接到 `libpthread.so.0` + `libdl.so.2` | `-lpthread` / `-ldl` 直接走 libc 内建符号 |

链接器同时加载了 sysroot 中 glibc 2.28 版的 `libpthread.so.0` 和交叉编译器自带的 glibc 2.35 版 `libc.so.6`。`libpthread` 引用了 `libc` 的 `GLIBC_PRIVATE` 内部符号（如 `__libc_current_sigrtmax_private`），但 2.35 版 libc 中该符号已改为不同实现，导致 undefined reference。

更深层的问题：**如果强行用 qemu + ARM moc 方案编译成功，产物会链接到 glibc 2.35，拿到 glibc 2.28 的开发板上同样运行不了**。

**根本修复方向：**

❌ 隐藏 sysroot 中的 glibc 库 → 能编译但产物版本不对
✅ **换基础镜像** — 交叉编译器的 glibc 版本必须与目标开发板一致

---

### 坑 4：`moc: could not find a Qt installation of ''` + sed 模式不匹配

**错误信息：**

```
moc: could not find a Qt installation of ''
make[2]: *** [CMakeFiles/smartcam_autogen.dir/build.make:58: ...] Error 1
```

**根因（两层）：**

**第 1 层 — qtchooser 包装器问题：**

确定使用 `debian:buster`（glibc 2.28）作为基础镜像后，出于性能考虑决定放弃 qemu 执行 ARM 二进制，改为使用宿主 x86 版 Qt5 工具（`qtbase5-dev` 提供的 moc/rcc/uic/qmake）。

但 Debian 中 `/usr/bin/moc` 是指向 `qtchooser` 的符号链接，qtchooser 需要默认配置文件（`/usr/lib/x86_64-linux-gnu/qtchooser/default.conf`）才能选择正确的 Qt 版本。buster 的 `qtbase5-dev` 安装了 qtchooser 但**没有创建默认配置**，于是 `moc` 报 "could not find a Qt installation of ''"。

实际可用的 x86_64 moc 在 `/usr/lib/qt5/bin/moc`（真实二进制，非 symlink）。

**第 2 层 — sed pattern 不匹配：**

sysroot 的 `Qt5CoreConfigExtras.cmake` 中工具路径写法是绝对路径：
```cmake
set(imported_location "/usr/bin/qmake")
set(imported_location "/usr/bin/moc")
set(imported_location "/usr/bin/rcc")
```

`Qt5WidgetsConfigExtras.cmake` 中 uic 使用变量引用：
```cmake
set(imported_location "${_qt5Widgets_install_prefix}/lib/qt5/bin/uic")
```

最初的 `cross-build.sh` sed 模式写的是 `qt5/bin/moc` 等字符串，**完全不匹配**上面的 `/usr/bin/moc` 或变量引用形式，patch 静默失效。


### 坑 5：`_qt5Core_install_prefix` 动态路径拼接 → CMake 始终找到 ARM 二进制

**错误信息（与前文 qemu-arm 相同，但原因更深层）：**

```
Command
-------
/workspace/npi-sysroot/usr/lib/qt5/bin/moc -I/workspace/ ...

Output
------
qemu-arm: Could not open '/lib/ld-linux-armhf.so.3': No such file or directory
```

> 这个错误在修复坑 1~4 后依然存在。即使 sed patch 正确匹配了 `/usr/bin/moc` 并替换为 `/usr/lib/qt5/bin/moc`，make 阶段 AUTOMOC 调用的仍然是 ARM 二进制。

**根因：CMake 通过 `_qt5Core_install_prefix` 动态推导工具路径**

`Qt5CoreConfig.cmake` 第 9–14 行（位于 `npi-sysroot/usr/lib/arm-linux-gnueabihf/cmake/Qt5Core/`）计算安装前缀：

```cmake
get_filename_component(_realCurr "${_IMPORT_PREFIX}" REALPATH)
get_filename_component(_realOrig "/usr/lib/arm-linux-gnueabihf/cmake/Qt5Core" REALPATH)
if(_realCurr STREQUAL _realOrig)
    get_filename_component(_qt5Core_install_prefix "/usr/lib/arm-linux-gnueabihf/../../" ABSOLUTE)
else()
    # 在 sysroot 中，CMAKE_CURRENT_LIST_DIR ≠ /usr，走这个分支
    get_filename_component(_qt5Core_install_prefix
        "${CMAKE_CURRENT_LIST_DIR}/../../../../" ABSOLUTE)
endif()
```

推导过程：

```
CMAKE_CURRENT_LIST_DIR = /workspace/npi-sysroot/usr/lib/arm-linux-gnueabihf/cmake/Qt5Core

_qt5Core_install_prefix = CMAKE_CURRENT_LIST_DIR / ../../.. / ..
                        = /workspace/npi-sysroot/usr/
```

然后 `Qt5CoreConfigExtras.cmake` 第 16 行用它拼接 moc 路径：

```cmake
set(imported_location "${_qt5Core_install_prefix}/lib/qt5/bin/moc")
# = /workspace/npi-sysroot/usr/lib/qt5/bin/moc  ← ARM ELF 二进制！
```

**这就是核心问题**：无论我们在 `arm-linux-gnueabihf/qt5/bin/` 目录下如何改软链接，CMake 最终找到的是 **`sysroot/usr/lib/qt5/bin/moc`**——这是从开发板导出的真实 ARM ELF 可执行文件，不是软链接！

```
sysroot/usr/lib/qt5/bin/moc (ARM ELF) → binfmt_misc → qemu-arm → 失败 (缺 ld-linux-armhf.so.3)
```

**为什么 `-DQt5Core_MOC_EXECUTABLE` 也不生效？**

CMake 的 AUTOMOC 使用 `Qt5::moc` **IMPORTED 目标**（`Qt5CoreConfigExtras.cmake` 第 14 行）：

```cmake
add_executable(Qt5::moc IMPORTED)
set(imported_location "${_qt5Core_install_prefix}/lib/qt5/bin/moc")
set_target_properties(Qt5::moc PROPERTIES
    IMPORTED_LOCATION ${imported_location}
)
```

`-DQt5Core_MOC_EXECUTABLE=/usr/lib/qt5/bin/moc` 设置的是 CMake **缓存变量**，但 AUTOMOC 的 make 规则读取的是 `Qt5::moc` 目标的 `IMPORTED_LOCATION` **属性**，该属性已在 `Qt5CoreConfigExtras.cmake` 中被硬编码为 sysroot 路径，**不会被同名 CMake 变量覆盖**。

**为什么必须两处都替换？**

| 位置 | 内容 | CMake 是否使用 |
|------|------|:---:|
| `sysroot/usr/lib/qt5/bin/moc` | 真实 ARM ELF 二进制 | ✅ 是，通过 `_qt5Core_install_prefix` 拼接 |
| `sysroot/usr/lib/arm-linux-gnueabihf/qt5/bin/moc` | 软链接 → 上面的 ARM 二进制 | 兜底路径 |

即使把第二个位置的软链接改为指向宿主工具，CMake 直接拼接出的 `sysroot/usr/lib/qt5/bin/moc` 仍然是 ARM 二进制。**必须两个位置都替换**为指向宿主 x86_64 的软链接。

**修复：**

```bash
# cross-build.sh 中同时对两个目录执行替换
for tool in moc rcc uic qmake; do
    for dir in \
        "$SYSROOT/usr/lib/qt5/bin" \                    # ← CMake 实际拼接的路径
        "$SYSROOT/usr/lib/arm-linux-gnueabihf/qt5/bin"; do  # ← 原软链接目录（兜底）
        rm -f "$dir/$tool"
        ln -s "/usr/lib/qt5/bin/$tool" "$dir/$tool"  # → 宿主 x86_64
    done
done
```

替换后的路径解析：

```
CMake 查找: sysroot/usr/lib/qt5/bin/moc
  → 软链接 → /usr/lib/qt5/bin/moc (x86_64 宿主二进制)
  → 直接在本机执行 ✓
```

**对最终 ARM 程序的影响：完全无影响**

`moc`/`rcc`/`uic`/`qmake` 是**代码生成器**，仅在编译期间运行，不参与最终链接：

| 工具 | 输入 | 输出 | 是否链接到产物 |
|------|------|------|:---:|
| `moc` | `.h` (含 Q_OBJECT) | `moc_xxx.cpp`（C++ 文本） | ❌ |
| `rcc` | `.qrc` (资源) | `qrc_xxx.cpp`（C++ 文本） | ❌ |
| `uic` | `.ui` (界面 XML) | `ui_xxx.h`（C++ 文本） | ❌ |
| `qmake` | `.pro` | `Makefile`（构建配置） | ❌ |

最终由 `arm-linux-gnueabihf-g++` 编译所有源码 + 链接 sysroot 中的 ARM 库 → 产物仍是 ARM ELF。

### 最终完整解决方案

#### 1) Docker 基础镜像选型

```dockerfile
# Dockerfile.arm-sysroot
FROM debian:buster     # glibc 2.28 / Qt 5.11.3 — 与开发板完全一致
```

#### 2) 安装依赖选择

| 安装 | 用途 | 说明 |
|------|------|------|
| `gcc-arm-linux-gnueabihf` `g++-arm-linux-gnueabihf` | ARM 交叉编译器 | Debian Buster → GCC 8.3, glibc 2.28 |
| `qtbase5-dev` | 宿主 x86 Qt5 工具 | 提供 `moc`/`rcc`/`uic`（真实路径：`/usr/lib/qt5/bin/`） |
| `qt5-qmake` | qtchooser 配置文件 | 提供 `/usr/lib/x86_64-linux-gnu/qtchooser/qt5.conf` |

**不安装** `qemu-user-static`、任何 `*:armhf` 包。

#### 3) CMake 配置文件 patch（`cross-build.sh` 核心逻辑）

构建前对 sysroot 的 cmake 配置文件做修补，将工具路径从 ARM 版改为宿主 x86 版：

```bash
# Qt5CoreConfigExtras.cmake — 修改 3 处 imported_location
sed -i 's|/usr/bin/qmake|/usr/lib/qt5/bin/qmake|g' "$CORE_EXTRAS"
sed -i 's|/usr/bin/moc|/usr/lib/qt5/bin/moc|g'     "$CORE_EXTRAS"
sed -i 's|/usr/bin/rcc|/usr/lib/qt5/bin/rcc|g'     "$CORE_EXTRAS"

# Qt5WidgetsConfigExtras.cmake — 修改变量引用中的 uic 路径
sed -i 's|${_qt5Widgets_install_prefix}/lib/qt5/bin/uic|/usr/lib/qt5/bin/uic|g' "$WIDGETS_EXTRAS"
```

patch 后的效果（以 moc 为例）：
```cmake
# patch 后
set(imported_location "/usr/lib/qt5/bin/moc")
_qt5_Core_check_file_exists(${imported_location})   # ✓ 文件存在，检查通过
```

patch 后的效果（以 uic 为例）：
```cmake
# patch 后
set(imported_location "/usr/lib/qt5/bin/uic")
_qt5_Widgets_check_file_exists(${imported_location}) # ✓ 文件存在，检查通过
```

这样 CMake 的 AUTOMOC/AUTOUIC/AUTORCC 会调用 x86 版工具，直接在本机运行，无需 qemu 模拟 ARM。

**但仅靠 sed patch 不够！** 因为 CMake 通过 `_qt5Core_install_prefix` 动态拼接出 `sysroot/usr/lib/qt5/bin/moc`（ARM 二进制），sed 改的是 `/usr/bin/moc`，这个路径不是 CMake 最终使用的路径。必须同时做第 4 步。

#### 4) 替换 sysroot 中 Qt5 工具为宿主 x86_64 版本（关键步骤）

sysroot 中 `usr/lib/qt5/bin/{moc,rcc,uic,qmake}` 均为 ARM ELF 二进制。**必须全部替换**为指向宿主 x86_64 工具的软链接：

```bash
# 两处目录都要替换：
#   ① sysroot/usr/lib/qt5/bin/          ← CMake _qt5Core_install_prefix 实际拼接的路径
#   ② sysroot/usr/lib/arm-linux-gnueabihf/qt5/bin/  ← 原始软链接目录（兜底）
for tool in moc rcc uic qmake; do
    for dir in \
        "$SYSROOT/usr/lib/qt5/bin" \
        "$SYSROOT/usr/lib/arm-linux-gnueabihf/qt5/bin"; do
        rm -f "$dir/$tool"
        ln -s "/usr/lib/qt5/bin/$tool" "$dir/$tool"
    done
done
```

> **为什么必须两处都替换？**
>
> CMake 通过 `_qt5Core_install_prefix` 动态拼接为 `sysroot/usr/lib/qt5/bin/moc`（目录 ①），
> 但 `arm-linux-gnueabihf/qt5/bin/moc`（目录 ②）是原软链接位置，某些路径解析也可能用到。两处都替换最安全。

#### 5) 环境变量

```dockerfile
ENV QT_SELECT=5    # 确保 qtchooser 选择 Qt5（备用，实际已 bypass qtchooser）
```

#### 6) 完整调用链

```
sysroot-setup.sh
  ├──[1/3] 合并分包 → npi-sysroot/
  ├──[2/3] docker build Dockerfile.arm-sysroot
  └──[3/3] docker run → cross-build.sh
              ├── patch Qt5CoreConfigExtras.cmake  (/usr/bin/moc → /usr/lib/qt5/bin/moc)
              ├── patch Qt5WidgetsConfigExtras.cmake (uic 路径)
              ├── 替换 sysroot 两处目录的 ARM Qt5 工具 → 宿主 x86_64
              ├── cmake -DCMAKE_SYSROOT=npi-sysroot -DCMAKE_TOOLCHAIN_FILE=...
              └── make -j$(nproc)
                    └── AUTOMOC 调用宿主 x86 moc ✓
```

#### 7) 产物验证

```bash
file build/arm/smartcam
# ELF 32-bit LSB pie executable, ARM, EABI5

arm-linux-gnueabihf-readelf -V build/arm/smartcam | grep GLIBC | sort -u
# 最高版本: GLIBC_2.28 ← 开发板 glibc 完全匹配

arm-linux-gnueabihf-readelf -d build/arm/smartcam | grep NEEDED
# libQt5Widgets.so.5, libQt5Gui.so.5, libQt5Core.so.5, libpthread.so.0, libm.so.6, libc.so.6
```

### 涉及文件

| 文件 | 角色 |
|------|------|
| `Dockerfile.arm-sysroot` | Docker 镜像定义，选型 `debian:buster` + `qtbase5-dev` |
| `scripts/cross-build.sh` | 构建入口，含 cmake 配置 patch + **两处 ARM 工具替换** + cmake/make 全流程 |
| `configs/toolchain.arm.cmake` | 交叉编译工具链文件，支持 `CMAKE_SYSROOT` |
| `scripts/sysroot-setup.sh` | 顶层脚本，组装 sysroot + 触发 Docker 构建 |
| `CMakeLists.txt` | 目标编译选项中添加 `-static-libstdc++ -static-libgcc` |
| `npi-sysroot/usr/lib/arm-linux-gnueabihf/cmake/Qt5Core/Qt5CoreConfigExtras.cmake` | **被 patch 的文件** — moc/rcc/qmake 的 `IMPORTED_LOCATION` 和 `_qt5_Core_check_file_exists` |
| `npi-sysroot/usr/lib/arm-linux-gnueabihf/cmake/Qt5Core/Qt5CoreConfig.cmake` | moc 路径来源 — `_qt5Core_install_prefix` 推导逻辑（路径动态拼接的根因） |
| `npi-sysroot/usr/lib/arm-linux-gnueabihf/cmake/Qt5Widgets/Qt5WidgetsConfigExtras.cmake` | **被 patch 的文件** — uic 的 `IMPORTED_LOCATION` |
| `npi-sysroot/usr/lib/qt5/bin/` | **被替换的目录** — ARM Qt5 工具 (moc/rcc/uic/qmake) → 宿主 x86_64 软链接 |
| `npi-sysroot/usr/lib/arm-linux-gnueabihf/qt5/bin/` | **被替换的目录** — 原始软链接位置，也替换为宿主 x86_64 |

---

## 14. 录像 AVI 文件损坏：movi 四字符被覆盖 ✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | `src/storage/manager.cpp` — `finalizeAvi()` |
| **现象** | 录像后打开 Gallery 报错，视频无法播放、无缩略图 |
| **严重程度** | ❌ 严重 — 所有录制后的 AVI 文件均不可用 |

---

### 错误信息

```
[ERROR] manager.cpp:846 (extractAviThumbnail) extractAviThumbnail: expected movi LIST type, got ▒▒
[VideoPlayer] Expected movi LIST, got "\xEC\xCE\n\x00"
```

两个独立的 AVI 解析器（`extractAviThumbnail` / `VideoPlayer::parseAviHeader`）在跳过 hdrl LIST 后，都无法在预期位置找到 movi LIST，读到的 4 个字节不是 `"movi"` (0x6D 0x6F 0x76 0x69)，而是乱码 `\xEC\xCE\x0A\x00`。

---

### 根因

`finalizeAvi()` 回填 movi LIST 大小时，偏移量计算错误，**把 size 值写到了 `"movi"` FOURCC 的位置**。

#### AVI 文件布局

```
... "LIST"(4) | size(4) | "movi"(4) | "00dc"(4) | size(4) | JPEG 数据 ...
      offset   offset+4   offset+8   offset+12 = m_moviDataOffset
```

#### 错误代码

```cpp
// finalizeAvi()
long moviSizePos = m_moviDataOffset - 4;   // ← offset+8，指向 "movi"
fseek(fp, moviSizePos, SEEK_SET);
writeU32(fp, static_cast<uint32_t>(moviSize));  // 把 "movi" 覆盖成 uint32 数值
```

`m_moviDataOffset` 是第一个帧的 `"00dc"` 位置（offset+12），减 4 得到 offset+8，正好是 `"movi"` FOURCC 的地址。`writeU32` 将 movi LIST 大小（如 725KB = 0x000ACEEC）以小端序写入，覆盖了 `"movi"`：

```
┌───────┬───────┬───────┬───────┐
│  'm'  │  'o'  │  'v'  │  'i'  │  ← 原本的 "movi"
│  0x6D │  0x6F │  0x76 │  0x69 │
├───────┼───────┼───────┼───────┤
│  0xEC │  0xCE │  0x0A │  0x00 │  ← 被覆盖成 0x000ACEEC
│   'ì'  │   'Î'  │  '\n' │  NUL  │
└───────┴───────┴───────┴───────┘
```

这就是为何错误信息中显示 `\xEC\xCE\x0A\x00` — 它其实是 movi LIST 的 size 数值。

同时，正确的 size 字段位置（offset+4）从未被回填，保留了初始值 0，导致 `VideoPlayer::parseAviHeader` 后续跳帧到 idx1 的逻辑也失效。

---

### 修复

#### 修复 1：moviSizePos 偏移更正（核心修复）

```cpp
// 修复前
long moviSizePos = m_moviDataOffset - 4;  // → "movi"，错误！

// 修复后
long moviSizePos = m_moviDataOffset - 8;  // → size 字段，正确！
```

```
... "LIST"(4) | size(4) | "movi"(4) | "00dc" ...
         ↑          ↑
      offset+4   offset+8 = m_moviDataOffset - 8 ✓
```

#### 修复 2：帧数据 WORD 对齐填充

RIFF 规范要求所有 chunk 数据 2 字节对齐。当 JPEG 帧大小为奇数时，需要在数据后追加 1 字节 padding（不计入 chunk size）：

```cpp
// writeRecordFrame() — 写入帧数据后
if (len % 2 != 0) {
    uint8_t pad = 0;
    fwrite(&pad, 1, 1, m_recordFile);
}
```

此前缺失 padding 会导致后续 chunk 起始偏移为奇数，虽然本项目内用 idx1 索引 seek 不受影响，但不符合 RIFF 规范，外部播放器可能拒绝。

#### 修复 3：idx1 `dwChunkLength` 规范修正

```cpp
// 修复前：包含了 chunk 头（"00dc" + size = 8 字节）
idx.length = static_cast<uint32_t>(sizeof(AviFrameChunk) + len);

// 修复后：按 RIFF idx1 规范，只记录帧数据大小
idx.length = static_cast<uint32_t>(len);
```

`readFrameJpeg()` 不依赖 `dwChunkLength`（它直接从 chunk 头读取 size），所以不影响内部播放器，但修复后符合规范，兼容第三方工具。

---

### 影响范围

- 修复前录制的 AVI 文件**已永久损坏**，"movi" FOURCC 已被覆盖无法恢复，需要删除后重新录制
- 修复后新录制的 AVI 文件结构完全正确，Gallery 缩略图提取和视频播放均正常

### 涉及文件

| 文件 | 修改内容 |
|------|----------|
| `src/storage/manager.cpp:finalizeAvi()` | `m_moviDataOffset - 4` → `m_moviDataOffset - 8` |
| `src/storage/manager.cpp:writeRecordFrame()` | 新增奇数帧 WORD 对齐 padding |
| `src/storage/manager.cpp:writeRecordFrame()` | idx1 `dwChunkLength` = `len`（去除 chunk 头 8 字节） |


---

## 15. YUYV→RGB NEON SIMD 加速优化 ✅ 已实现

| 属性 | 值 |
|------|-----|
| **模块** | `src/camera/processor_neon.cpp`（新增）/ `src/camera/processor.cpp` / `include/display/gui.h` |
| **现象** | YUYV 模式下 GUI 实时预览帧率低、CPU 占用高；640×480 YUYV→RGB24 标量转换耗时 ~8ms，30fps 下仅转换就占 24% CPU 时间 |
| **严重程度** | 🟡 次要 — 性能优化，功能正常但用户体验可提升 |

### 背景

项目在 ARM 交叉编译时已启用 `-mfpu=neon`（CMakeLists.txt），但 YUYV→RGB24 转换使用纯 C++ 标量循环——每轮处理 1 个宏像素（2 像素），共做 6 组 BT.601 定点乘法+clip。

i.MX6ULL 的 Cortex-A7 @ 792MHz 单核标量处理 640×480 帧约需 8ms。虽然 30fps 仍可达成，但 YUYV→RGB 转换占用了大量 CPU 时间，剩余预算紧张。

### 优化目标

利用 Cortex-A7 的 NEON 128-bit SIMD 指令集，将每轮处理量从 **2 像素 → 16 像素**，目标：
- YUYV→RGB24 耗时从 ~8ms → ~1ms（理论加速 8×）
- 减少 CPU 占用，为 GUI 渲染和网络推流留出更多预算

### 实现方案

**新增文件：** `src/camera/processor_neon.cpp`（~150 行）

**核心技术：** ARM NEON Intrinsics（`arm_neon.h`），非手写汇编

| 步骤 | NEON 指令 | 说明 |
|:---:|------|------|
| 1 | `vld2q_u8(src)` | 一次加载 32 字节 YUYV，去交织分离 Y 和 UV |
| 2 | `vuzp_u8` + `vmovl_u8` | 从 UV 交错数组中提取 U、V，扩展到 16-bit 并减 128 |
| 3 | `vmovl_u8` | Y 扩展到 16-bit（低 8 像素 / 高 8 像素） |
| 4 | `vmulq_s16` + `vaddq_s16` + `vshrq_n_s16` | BT.601 矩阵运算: R=Y+V×359>>8, G=Y−(U×88+V×183)>>8, B=Y+U×454>>8 |
| 5 | `vqmovun_s16` | 饱和转换 int16→uint8，自动 clip 到 [0,255]（替代标量的手动 `clip()`） |
| 6 | `vst3_u8(dst, rgb)` | 交织写入 RGB24（R0,G0,B0,R1,G1,B1,...） |

**主循环**：每轮 8 宏像素 = 16 像素 = 32B YUYV → 48B RGB，尾部 <16 像素退化为标量。

**关键设计决策：**

- **Intrinsics 而非手写汇编** — 编译器管寄存器分配，同代码 armv7/armv8 通用
- **两处调用点统一代理** — `processor.cpp` 和 `gui.h` 两处 `yuyv_to_rgb24` 都转发到同一个 NEON 函数，避免代码重复
- **编译期分支 (`#ifdef __ARM_NEON`)** — 零运行时开销，x86 本地调试不受影响

### 调用方集成

三处调用点，编译期自动分流：

| 调用点 | 文件 | ARM (`__ARM_NEON`) | x86 (无 NEON) |
|--------|------|:---:|:---:|
| `VideoProcessor::yuyvToRgb24()` | `processor.cpp` | → `yuyv_to_rgb24_neon()` | 原标量代码 |
| GUI 预览 `yuyv_to_rgb24()` | `gui.h` (inline) | → `yuyv_to_rgb24_neon()` | 原标量代码 |
| `encodeYUYVtoJPEG()` | `processor.cpp` | 调用上面的 → NEON | 标量 |

编译行为：ARM 交叉编译（`-mfpu=neon`）→ `__ARM_NEON` 自动定义 → NEON 路径；x86 PC 本地调试 → 标量路径，功能完全一致。

### 性能对比（估算，640×480）

| 指标 | 标量 C++ | NEON Intrinsics | 加速比 |
|------|:-------:|:---------------:|:-----:|
| 每轮处理像素 | 2 | **16** | 8× |
| 核心循环总次数 | ~153,600 | **~19,200** | 8× |
| 单帧耗时 | ~8 ms | **~1 ms** | 8× |
| CPU 占用 (30fps) | ~24% | **~3%** | — |
| 指令数/像素 | ~30 | **~2** | 15× |
| 代码行数 | 35 (标量) | 150 (含注释) | — |

### 涉及文件

| 文件 | 改动 |
|------|------|
| `src/camera/processor_neon.cpp` | 🆕 新增，150 行 NEON intrinsics 实现 |
| `src/camera/processor.cpp` | `yuyvToRgb24()` 入口添加 `#ifdef __ARM_NEON` → NEON 分流 |
| `include/display/gui.h` | inline `yuyv_to_rgb24()` 添加 `#ifdef __ARM_NEON` → NEON 分流 |
| `include/camera/processor.h` | 新增 `yuyvToRgb24Neon()` 声明 (`#ifdef __ARM_NEON`) |
| `CMakeLists.txt` | `CAMERA_SOURCES` 加入 `processor_neon.cpp` |

---

## 16. 开发板重启后 Gallery 照片消失（tmpfs 导致数据不持久化）✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | 存储 / Gallery / ConfigManager |
| **现象** | 程序关闭再启动时 Gallery 能正常显示照片，但重启开发板后照片全部消失 |
| **严重程度** | ❌ 严重 — 用户数据丢失 |

### 排查过程

```
现象分析
  ├─ 程序结束再启动 → 照片可见 ✅          说明：不是程序逻辑 bug
  ├─ 重启开发板后再启动 → 照片消失 ❌       说明：文件系统层问题
  └─ 怀疑 /data 不是持久化存储
        ├─ df -h → /data 不显示独立挂载点
        ├─ mount | grep /data → 无输出
        └─ 结论：/data 在 tmpfs overlay 中
```

### 根因

i.MX6ULL 开发板的 Buildroot/Yocto 默认 rootfs 配置中：

- `/dev/mmcblk1p2` 是主 eMMC 分区，挂载在 `/`
- `/data` 未独立挂载，由 rootfs 的 **tmpfs overlay** 提供可写支持
- tmpfs = RAM 文件系统，**断电/重启后数据丢失**

照片写入 `/data/photos/YYYYMMDD/IMG_*.jpg`，实际存储位置是 RAM，不是 eMMC。程序重启只是进程生命周期，tmpfs 内容仍在；但系统重启会清空 RAM，照片全部丢失。

### 解决

1. **ConfigManager 增强** — `setString()`/`save()`/`saveAs()` 支持从 GUI 保存配置：
   ```
   include/common/config.h
     + setString(section, key, value)   — 内存修改
     + save()                           — 写回原文件
     + saveAs(path)                     — 写入指定文件（自动 mkdir -p）
     + mkdirParents(path)               — 递归创建目录
   ```

2. **Settings 面板新增 Store 下拉框** — 选择存储路径：
   ```
   | Temporary (/data)          | /data                   ← tmpfs，重启丢失 |
   | Persistent (eMMC)          | /home/debian/smartcam   ← eMMC，持久化   |
   ```

3. **配置加载三级优先级**（`main.cpp`）：
   ```
   --config 显式  >  ~/.config/smartcam/smartcam.conf  >  /etc/smartcam/smartcam.conf
   ```

4. **切换即时生效 + 持久化** — 回调中同时更新 `StorageManager` 的 `photoDir`/`videoDir` 并写入配置文件。非 root 用户自动 fallback 到用户级路径。

### 涉及文件

| 文件 | 改动 |
|------|------|
| `include/common/config.h` | 🆕 新增 `setString()`、`save()`、`saveAs()`、`mkdirParents()` |
| `include/display/gui.h` | 🆕 `m_storageCombo` 成员；`onStoragePathChanged` 回调声明 |
| `src/display/gui.cpp` | 🆕 Settings 面板添加 Store 下拉框 + 信号/slot |
| `src/main.cpp` | 🆕 配置加载优先级 + 存储路径变更回调 |
| `configs/smartcam.conf` | 📝 添加存储路径注释 |

---

## 17. 帧率滑块卡死（仅枚举到单帧率 → minFps==maxFps）✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | `src/main.cpp` — 帧率范围初始化逻辑 |
| **现象** | 部分 UVC 摄像头（如某些 USB 摄像头）`enumFrameRates()` 只返回一个离散帧率（如仅 30fps），导致 `minFps == maxFps == 30`，滑块 min=30/max=30，拖动无任何效果，完全卡死在 30fps |
| **严重程度** | ❌ 严重 — 帧率不可调 |

### 原因

许多 UVC 摄像头的固件通过 `VIDIOC_ENUM_FRAMEINTERVALS` 只报告一个离散帧间隔（通常为 30fps），但实际的 `VIDIOC_S_PARM` 仍可接受其他帧率值（驱动内部做近似/适配）。原始代码直接将枚举到的唯一值设为滑块 `[min, max]` 范围，导致滑块被锁定在单一值。

### 解决

在 `main.cpp` 帧率初始化代码中，当 `enumFrameRates()` 返回成功但仅有一个离散帧率时（`minFps == maxFps`），回退到安全范围 1~60fps，并输出提示日志：

```cpp
if (minFps == maxFps) {
    LOG_INF("Framerate: only one discrete rate (%d fps) enumerated, "
            "falling back to safe range 1-60", minFps);
    minFps = 1;
    maxFps = 60;
}
```

### 涉及文件

| 文件 | 改动 |
|------|------|
| `src/main.cpp` | 帧率范围初始化中新增 `minFps == maxFps` 检测，回退到 1~60 |

---

## 18. stepwise 帧率枚举的 min/max 语义反转 ✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | `src/camera/capture.cpp` — `enumFrameRates()` |
| **现象** | 部分摄像头以 stepwise 类型报告帧间隔时，滑块范围颠倒（如低帧率在高位、高帧率在低位），导致滑块初始位置错误 |
| **严重程度** | ⚠️ 中等 — 功能可用但滑块行为异常 |

### 原因

V4L2 `V4L2_FRMIVAL_TYPE_STEPWISE` 中的语义：

- `stepwise.min` = **最短帧间隔**（1/最高帧率），如 `{1, 30}` → 1/30s → **30fps**
- `stepwise.max` = **最长帧间隔**（1/最低帧率），如 `{1, 5}` → 1/5s → **5fps**

原始代码直接按 `min → minFps`、`max → maxFps` 理解，将 30fps 当成了最小值、5fps 当成了最大值。这是一个语义层面的误区：帧间隔的"最小"对应帧率的"最大"。

### 修复

```cpp
// 修复前（错误）：
int minFps = static_cast<int>(frmival.stepwise.min.denominator) /
             static_cast<int>(frmival.stepwise.min.numerator);  // 30 → 当成 minFps
int maxFps = static_cast<int>(frmival.stepwise.max.denominator) /
             static_cast<int>(frmival.stepwise.max.numerator);  // 5  → 当成 maxFps

// 修复后（正确）：
int highFps = static_cast<int>(frmival.stepwise.min.denominator) /
              static_cast<int>(frmival.stepwise.min.numerator);  // 30 → highFps
int lowFps  = static_cast<int>(frmival.stepwise.max.denominator) /
              static_cast<int>(frmival.stepwise.max.numerator);  // 5  → lowFps
// 确保 lowFps <= highFps
if (lowFps > highFps) std::swap(lowFps, highFps);
// 从 lowFps 迭代到 highFps
for (int f = lowFps; f <= highFps && count < 20; f += stepFps) { ... }
```

### 涉及文件

| 文件 | 改动 |
|------|------|
| `src/camera/capture.cpp`—`enumFrameRates()` | stepwise 分支：`min`→`highFps`，`max`→`lowFps`，添加 swap 保护 |

---

## 19. VIDIOC_S_PARM 在 STREAMON 期间返回 EBUSY ✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | `src/main.cpp` — 帧率回调 lambda |
| **现象** | 拖动帧率滑块后日志输出 `setFramerate(XX) failed: Device or resource busy`，帧率实际未改变 |
| **严重程度** | ❌ 严重 — 帧率调整完全失效 |

### 原因

V4L2 规范要求 `VIDIOC_S_PARM` 在流开启（`STREAMON`）期间对某些设备是不可用的，驱动返回 `-EBUSY`。原始帧率回调直接调用 `capture->setFramerate()`，未先停止流，导致 ioctl 被驱动拒绝。

### 解决

帧率回调重构为以下流程（`stop → set → start`）：

```cpp
// 1. 暂停采集线程（避免采集线程在 stopCapture 时使用 mmap 缓冲区）
g_state.paused = true;
/* wait for pause ack with 1.1s timeout */

// 2. 停止采集流 → 设置帧率 → 重启采集流
capture->stopCapture();
capture->setFramerate(1, fps);   // ← 此时无 EBUSY
capture->startCapture();

// 3. 设置软件帧率节流目标（兜底）
g_state.targetFps = fps;

// 4. 恢复采集线程
g_state.paused = false;
g_state.pauseCv.notify_one();
```

同时新增**软件帧率节流**作为兜底机制：即使硬件 `VIDIOC_S_PARM` 成功，采集线程也会按 `targetFps` 丢帧以确保实际输出帧率匹配用户设定。

### 涉及文件

| 文件 | 改动 |
|------|------|
| `src/main.cpp` | 帧率回调重构：增加 pause→stop→set→start→resume 流程 + 软件节流 |
| `src/main.cpp` (g_state) | 新增 `targetFps` 原子变量 |
| `src/main.cpp` (采集线程) | 新增基于 `targetFps` 的帧率节流逻辑（以 `steady_clock` 控制帧间隔） |

---

## 20. 帧率滑块快速拖拽导致流频繁中断（防抖优化）✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | `include/display/gui.h` / `src/display/gui.cpp` |
| **现象** | 快速拖动帧率滑块时，每经过一个整数值都触发 stop→set→start 完整流程，导致视频流频繁中断（1秒内可能 stop/start 5~10 次），画面闪烁 |
| **严重程度** | ⚠️ 中等 — 功能可用但用户体验差 |

### 原因

原始 `onFramerateSliderChanged(int value)` 槽函数直接调用回调，没有任何防抖机制。`QSlider` 的 `valueChanged` 信号在拖动过程中高频触发（每经过一个 step 就 emit 一次），每次触发都执行完整的流停止/帧率设置/流启动流程。

### 解决

使用 `QTimer::singleShot` 实现 300ms 防抖窗口：

```cpp
// gui.h 新增成员
QTimer* m_framerateDebounceTimer = nullptr;

// gui.cpp — buildSettingsDialog() 中初始化
m_framerateDebounceTimer = new QTimer(this);
m_framerateDebounceTimer->setSingleShot(true);
connect(m_framerateDebounceTimer, &QTimer::timeout,
        this, &CameraGUI::onFramerateDebounced);

// onFramerateSliderChanged — 立即更新标签，重启防抖计时器
void CameraGUI::onFramerateSliderChanged(int value) {
    m_framerateValue->setText(QString("%1 fps").arg(value));  // 即时视觉反馈
    m_framerateInfo.current = value;
    m_framerateDebounceTimer->start(300);   // 300ms 内无新变化才触发
}

// onFramerateDebounced — 防抖结束时真正执行
void CameraGUI::onFramerateDebounced() {
    int value = m_framerateSlider->value();
    m_framerateInfo.current = value;
    if (m_onFramerate) m_onFramerate(value);
}
```

关键设计：
- **标签即时更新**：滑块值变化立即反映到 UI（`"XX fps"`），用户有拖拽反馈
- **实际变更延迟**：300ms 内连续拖拽只触发最后一次的 `onFramerateDebounced`
- **Reset Defaults 特殊处理**：恢复默认帧率时停止防抖计时器，直接执行，避免延迟

### 涉及文件

| 文件 | 改动 |
|------|------|
| `include/display/gui.h` | 新增 `m_framerateDebounceTimer` 成员、`onFramerateDebounced` slot |
| `src/display/gui.cpp` | `buildSettingsDialog()` — 防抖计时器初始化；`onFramerateSliderChanged()` — 仅更新标签 + 重启计时器；`onFramerateDebounced()` — 新 slot；`onResetDefaults()` — 停止防抖直接执行 |

---

## 21. 自动曝光导致帧率从 30fps 掉到 ~10fps ✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | `src/main.cpp` — V4L2 初始化时曝光控制 |
| **现象** | 开发板上设为 30fps，实际采集帧率仅 ~10fps（日志：`avg=10.2 fps, raw interval min=98ms`），画面严重卡顿 |
| **严重程度** | ❌ 严重 — 帧率无法达到标称值，影响实时预览和推流 |

### 原因

摄像头的**自动曝光（Auto Exposure Aperture Priority mode, V4L2_EXPOSURE_APERTURE_PRIORITY=3）**在室内暗光环境下自动增加曝光时间以提高画面亮度。典型情况：曝光时间增大到 ~100ms（远超 33ms），摄像头每 100ms 才产出一帧，导致帧率自然掉到 ~10fps。

```
自动曝光模式 (模式3) → 暗光场景 → 曝光时间自动拉长 → 帧间隔增大 → 实际帧率骤降
                                                100ms → 10fps
                                                <= 33ms → 30fps
```

### 解决

在 `main.cpp` 的 V4L2 初始化阶段，**强制切换到手动曝光模式**（`V4L2_EXPOSURE_MANUAL = 1`），并将曝光值限制在 ≤300（对应 V4L2 曝光绝对值单位，确保 <33ms/帧）：

```cpp
// 自动曝光 → 查询并强制手动模式以保持帧率
if (capture->queryControl(V4L2_CID_EXPOSURE_AUTO, ...) == 0) {
    capture->getControl(V4L2_CID_EXPOSURE_AUTO, expVal);
    if (expVal != 1) {  // 1 = V4L2_EXPOSURE_MANUAL
        capture->setControl(V4L2_CID_EXPOSURE_AUTO, 1);
        LOG_INF("Auto Exposure disabled (set to manual) to preserve framerate");
    }
}

// 曝光绝对值 → 限制 ≤300 保证 30fps 的时间预算
if (capture->queryControl(V4L2_CID_EXPOSURE_ABSOLUTE, ...) == 0) {
    capture->getControl(V4L2_CID_EXPOSURE_ABSOLUTE, absCur);
    int targetExposure = (absCur > 0) ? absCur : (absDef > 0 ? absDef : 312);
    if (targetExposure > 300) targetExposure = 300;
    capture->setControl(V4L2_CID_EXPOSURE_ABSOLUTE, targetExposure);
}
```

同时为暴露控制查询设置 GUI → 见问题 #23。

### 涉及文件

| 文件 | 改动 |
|------|------|
| `src/main.cpp` | V4L2 初始化阶段新增曝光控制查询 + 手动模式强制设置 |
| `include/camera/capture.h` | 新增 `V4L2_CID_EXPOSURE_AUTO`、`V4L2_CID_EXPOSURE_ABSOLUTE` 常量 |

---

## 22. 暗光下手动曝光导致画面模糊 ✅ 已解决

| 属性 | 值 |
|------|-----|
| **模块** | `src/main.cpp` + GUI 曝光面板（与 #21、#23 联动） |
| **现象** | 强制手动曝光后（#21），暗光环境下画面非常暗，用户不可见；若手动调大曝光值则产生严重拖影 |
| **严重程度** | 🟡 次要 — 画面可用但暗光场景画质受限 |

### 原因

手动曝光模式下，曝光时间是固定值。室内暗光需要更长曝光时间才能获得可接受的亮度，但曝光时间超过帧间隔（>33ms）会导致：
- **帧率下降**（与 #21 同一根因）
- **运动拖影**（物体在曝光窗口内移动）

这是硬件限制（传感器灵敏度 + 镜头光圈 + 室内照度）的固有矛盾：
- 短曝光 → 帧率高 → 画面暗
- 长曝光 → 帧率低 → 画面亮但有拖影

### 解决

通过**曝光面板**（#23）让用户能自主在帧率和亮度之间权衡：

1. **Auto Exposure 复选框** — 用户可切换回自动曝光模式（接受帧率下降换取自动亮度调节）
2. **Exposure 滑块** — 手动模式下精细调节曝光值，在可接受的帧率范围内找到最优亮度
3. **默认值** — 初始化时将曝光设为 300（上限，保证 30fps 的前提下尽可能亮）

这不是一个"彻底解决"型的修复，而是通过**曝光面板暴露控制权**让用户根据实际场景自主选择。

### 涉及文件

| 文件 | 改动 |
|------|------|
| 与 #21、#23 相同 | 曝光控制的初始化和 GUI |

---

## 23. 曝光控制面板（GUI 新增 Auto Exposure + Exposure 滑块）✅ 已实现

| 属性 | 值 |
|------|-----|
| **模块** | `include/display/gui.h` / `src/display/gui.cpp` / `include/camera/capture.h` / `src/main.cpp` |
| **现象** | 设置面板中没有任何曝光控制选项，用户无法调节曝光参数 |
| **严重程度** | 🟡 次要 — 功能缺口 |

### 实现

在 Settings 弹窗的 "Camera Controls" 分组中新增曝光控制行，与白平衡控制行结构对称：

**UI 控件：**
```
| Auto Exposure: | QCheckBox "Auto" | 弹性空白                    |
| Exposure:       | QSlider(H)       | QLabel "312" (60px 右对齐) |
```

**互锁逻辑（与 Auto WB 一致）：**
- Auto Exposure 勾选 → Exposure 滑块禁用 → `V4L2_CID_EXPOSURE_AUTO = 3`（Aperture Priority）
- Auto Exposure 取消 → Exposure 滑块启用 → `V4L2_CID_EXPOSURE_AUTO = 1`（Manual），同时写入当前滑块值到 `V4L2_CID_EXPOSURE_ABSOLUTE`

**V4L2 控制 ID 常量（capture.h 新增）：**
```cpp
static constexpr uint32_t V4L2_CID_EXPOSURE_AUTO     = 0x009a0901;
static constexpr uint32_t V4L2_CID_EXPOSURE_ABSOLUTE = 0x009a0902;
```

**GUI 新增方法：**
```cpp
void setExposureRange(int min, int max, int step, int value);
void setAutoExposure(bool enabled);
```

**槽函数：**
- `onAutoExposureChanged(int state)` — 切换 manual/auto + 联动滑块启用状态
- `onExposureChanged(int value)` — 写入 `V4L2_CID_EXPOSURE_ABSOLUTE`

**Reset Defaults 扩展：** 恢复曝光参数到 V4L2 默认值

### 涉及文件

| 文件 | 改动 |
|------|------|
| `include/camera/capture.h` | 🆕 `V4L2_CID_EXPOSURE_AUTO`、`V4L2_CID_EXPOSURE_ABSOLUTE` 常量 |
| `include/display/gui.h` | 🆕 `m_exposureSlider`、`m_exposureValue`、`m_autoExposureCheckBox`、`m_exposureInfo`、`setExposureRange()`、`setAutoExposure()`、`onAutoExposureChanged`、`onExposureChanged` |
| `src/display/gui.cpp` | 🆕 `buildSettingsDialog()` 曝光行 UI；`setExposureRange()`、`setAutoExposure()`；`onAutoExposureChanged()`、`onExposureChanged()` 槽函数；`connectSignals()` 连接；`onResetDefaults()` 曝光恢复 |
| `src/main.cpp` | 🆕 V4L2 初始化阶段曝光控制查询 + 手动模式强制设置

