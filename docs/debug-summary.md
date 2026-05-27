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
