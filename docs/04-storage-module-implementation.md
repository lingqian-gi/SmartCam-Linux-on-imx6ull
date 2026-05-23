# 存储管理模块 — 实现记录

> **编号**：MOD-04
> **创建日期**：2026-05-23
> **状态**：✅ 已实现，语法检查通过
> **依赖**：C++17、POSIX（statvfs/dirent）、单线程 safe（mutex 保护）

---

## 一、模块概述

基于文档 [3.6 存储管理模块](../求职项目-智能相机流媒体系统.md) 的设计，实现完整的本地存储功能，支持 JPEG 照片保存、MJPEG 帧封装 AVI 录像，以及 SD 卡空间管理。

### 本模块在项目中的位置

```
SmartCam 多线程架构
                    ┌──────────────┐
                    │   main.cpp   │
                    │  (主线程)     │
                    └──────┬───────┘
                           │
         ┌─────────────────┼──────────────────┐
         │                 │                  │
         ▼                 ▼                  ▼
  ┌────────────┐  ┌──────────────┐  ┌──────────────────┐
  │ 采集线程    │  │ 显示线程(GUI) │  │ MJPEG 流媒体服务器 │
  │ (Capture)  │  │   + 交互      │  │   (HTTP 推流)     │
  └─────┬──────┘  └──────┬───────┘  └────────┬─────────┘
        │                │                   │
        │    g_recording │  GUI 按钮回调      │
        │       标志触发  │  (Capture/Record) │
        └───────────────┬┴───────────────────┘
                        │
                        ▼
               ┌─────────────────┐
               │ StorageManager  │ ◄── 本模块
               │ ┌─────────────┐ │
               │ │ savePhoto() │ │ JPEG 直写
               │ │ startRecord │ │
               │ │ writeFrame  │ │ MJPEG → AVI
               │ │ stopRecord  │ │ RIFF 封装
               │ │ autoCleanup │ │ SD 卡管理
               │ └─────────────┘ │
               └─────────────────┘
```

### 核心设计理念

- **JPEG 零拷贝透传**：摄像头 MJPEG 硬件直出的帧数据直接写入文件，零 CPU 编码开销
- **手写 AVI 容器**：不依赖 ffmpeg 等第三方库，纯 C `FILE*` API 逐字节写入 RIFF 格式
- **Stop 时回填**：录制开始时总帧数未知，`stopRecord()` 时 `fseek` 回填 header 中的 `dwTotalFrames` 并写入 `idx1` 索引
- **按日期分目录**：照片和录像自动按 `YYYYMMDD/` 子目录组织，便于管理
- **线程安全**：`std::mutex` 保护录制状态，采集线程在 lock 内写帧

### 功能清单

| 功能 | 状态 |
|------|------|
| JPEG 照片保存（按日期目录，自动命名） | ✅ |
| AVI 录像：start → writeFrame… → stop 完整流程 | ✅ |
| AVI 头：RIFF → hdrl(avih+strl(strh+strf)) | ✅ |
| "00dc" 块写入（每个 MJPEG 帧一个 chunk） | ✅ |
| idx1 索引块（Stop 时回填，支持快进快退） | ✅ |
| 中途回填：dwTotalFrames / dwLength / movi Size / RIFF Size | ✅ |
| statvfs 磁盘剩余空间查询 | ✅ |
| opendir/readdir 自动清理旧文件（最旧优先） | ✅ |
| 递归目录创建 ensureDir() | ✅ |
| JSON→错误处理（空指针/写入短/目录创建失败） | ✅ |
| `isRecording()` / `currentRecordPath()` 状态查询 | ✅ |
| 与 main.cpp 集成（GUI 按钮 + 采集线程联动） | ✅ |
| 析构自动停止录制 | ✅ |

---

## 二、文件清单

### 2.1 新建文件（2 个）

```
SmartCam-Linux-on-imx6ull/
├── include/
│   └── storage/
│       └── manager.h         # StorageManager 类声明 + AVI/RIFF 结构体 (~280 行)
├── src/
│   └── storage/
│       └── manager.cpp       # StorageManager 实现 (~460 行)
└── docs/
    └── 04-storage-module-implementation.md  # 本文档
```

### 2.2 修改文件（2 个）

```
SmartCam-Linux-on-imx6ull/
├── CMakeLists.txt             # 新增 STORAGE_SOURCES 变量
└── src/
    └── main.cpp               # 集成 StorageManager + GUI 回调对接
```

---

## 三、类接口设计

### 3.1 StorageManager 核心 API

```cpp
class StorageManager {
public:
    StorageManager(const std::string& photoDir,
                   const std::string& videoDir);

    // ---- 拍照 ----
    std::string savePhoto(const uint8_t* jpeg_data, int len);
    // → 按日期自动命名: "photoDir/20260523/IMG_20260523_143025.jpg"
    // 返回完整路径，失败返回 ""

    // ---- 录像 (MJPEG → AVI) ----
    int  startRecord(int width, int height, int fps);
    int  writeRecordFrame(const uint8_t* jpeg_data, int len);
    int  stopRecord();
    bool isRecording() const;

    // ---- 空间管理 ----
    int getFreeSpaceMB(const std::string& path = "");
    int autoCleanup(int keep_mb = 100);

    // ---- 配置 ----
    void setPhotoDir(const std::string& dir);
    void setVideoDir(const std::string& dir);
};
```

### 3.2 调用流程

```
GUI 按钮点击              main.cpp             采集线程               StorageManager
    │                       │                    │                       │
    │── Record ────────────►│                    │                       │
    │                       │── startRecord() ──────────────────────────►│
    │                       │   (w,h,fps)       │                       │
    │                       │                    │                       │── fopen(avi)
    │                       │                    │                       │── writeAviHeader()
    │                       │                    │                       │── fseek 记录各偏移
    │                       │                    │                       │
    │                       │   g_recording=true │                       │
    │                       │                    │                       │
    │                       │                    │── if(g_recording):    │
    │                       │                    │     writeFrame() ────►│── "00dc" + fwrite
    │                       │                    │     writeFrame() ────►│── "00dc" + fwrite
    │                       │                    │     ...               │── ...
    │                       │                    │                       │
    │── Stop ──────────────►│                    │                       │
    │                       │   g_recording=false│                       │
    │                       │── stopRecord() ───────────────────────────►│── fseek 回填 size
    │                       │                    │                       │── 写 idx1 索引
    │                       │                    │                       │── fclose(avi)
```

---

## 四、AVI 容器格式实现

### 4.1 文件结构（面试重点）

```
AVI 文件 (RIFF 容器)
┌──────────────────────────────────────────────┐
│ RIFF 头                                       │
│   └─ fourcc: "RIFF", size: 文件总大小-8      │
│   └─ 形式类型: "AVI "                        │
├──────────────────────────────────────────────┤
│ LIST: hdrl  (头信息列表)                      │
│   ├─ "avih"  AVI 主文件头                     │
│   │   ├─ dwMicroSecPerFrame (1000000/fps)    │
│   │   ├─ dwWidth / dwHeight                  │
│   │   ├─ dwTotalFrames (0 → Stop时回填)      │
│   │   └─ dwStreams = 1                       │
│   └─ LIST: strl (流信息列表)                  │
│       ├─ "strh"  流头 (vids / MJPG)          │
│       │   ├─ dwScale=1, dwRate=fps           │
│       │   ├─ dwLength (0 → Stop时回填)       │
│       │   └─ dwSampleSize=0 (变长帧)         │
│       └─ "strf"  BITMAPINFOHEADER            │
│           ├─ biWidth / biHeight              │
│           ├─ biBitCount = 24                 │
│           └─ biCompression = "MJPG"          │
├──────────────────────────────────────────────┤
│ LIST: movi (帧数据)                           │
│   ├─ "00dc" [size=XXXX] [JPEG binary ...]   │
│   ├─ "00dc" [size=XXXX] [JPEG binary ...]   │
│   └─ "00dc" [size=XXXX] [JPEG binary ...]   │
├──────────────────────────────────────────────┤
│ idx1 索引块                                   │
│   ├─ { "00dc", offset, length }  // 帧 1     │
│   ├─ { "00dc", offset, length }  // 帧 2     │
│   └─ ...                                     │
└──────────────────────────────────────────────┘
```

### 4.2 回填策略

录制开始时不写入最终大小（因为帧数未知），而是在 `stopRecord()` 时通过 **fseek 回填**：

```cpp
// Stop 时按照预先记录的偏移回填
fseek(fp, m_rifSizeOffset,  SEEK_SET);   // RIFF file size
fseek(fp, m_avihFramesOffset, SEEK_SET);  // avih.dwTotalFrames
fseek(fp, m_strhLengthOffset, SEEK_SET);  // strh.dwLength
fseek(fp, moviSizePos, SEEK_SET);         // movi LIST size
```

各偏移在 `writeAviHeader()` 中通过 `ftell()` 记录，精确到字节。这种方式的代价是一次 `fseek` 开销，但避免了内存中缓存所有帧数据。

### 4.3 MJPEG 四字符码

```cpp
// FOURCC 常量（小端序）
constexpr uint32_t FOURCC_vids = 0x73646976;  // "vids"
constexpr uint32_t FOURCC_MJPG = 0x47504A4D;  // "MJPG"
constexpr uint32_t FOURCC_00dc = 0x63643030;  // "00dc" (compressed video data)
```

### 4.4 帧块结构

```cpp
#pragma pack(push, 1)
struct AviFrameChunk {
    uint32_t ckid;  // "00dc"
    uint32_t size;  // JPEG 数据大小
    // uint8_t data[size];  // 可变长 JPEG 二进制
};
#pragma pack(pop)
```

每帧 = 8 字节头 + JPEG 数据，写入时：

```cpp
writeFourCC(fp, "00dc");
writeU32(fp, jpeg_len);
fwrite(jpeg_data, 1, jpeg_len, fp);
```

---

## 五、关键设计决策

### 5.1 为何手写 AVI 而不是依赖 ffmpeg/libav？

| 对比项 | ffmpeg/libav | 手写 AVI (本实现) |
|--------|-------------|-------------------|
| 库大小 | ~30 MB | 0 KB（纯 C 标准库） |
| 编译复杂度 | 需交叉编译 10+ 依赖 | 零依赖 |
| iMX6ULL 适配 | 需移植 ARM NEON 汇编 | 通用 C，直接编译 |
| 面试价值 | 调用 API | ✅ 深入理解 RIFF 格式 |
| 功能 | 完整多媒体框架 | 基础 MJPEG AVI |

对于 iMX6ULL 的 Cortex-A7 单核 + 8GB SD 卡，手写 AVI 是 **实用主义的最优解**。

### 5.2 为何用 `statvfs` 而不是 `df` 命令？

- `statvfs()` 是 POSIX 系统调用，无子进程开销，适合嵌入式
- 返回 `f_bavail`（非特权用户可用块数），准确反映实际可写空间
- 出错直接返回 errno，比解析 `df` 输出可靠

### 5.3 为何用 `FILE*` 而不是 C++ `fstream`？

- AVI 格式需要大量 `fseek` + `ftell` 精确偏移控制
- `FILE*` 的 `fseek(f, offset, SEEK_SET)` 语义比 `fstream::seekp` 更直观
- 二进制写入性能相当，C API 在嵌入式环境更稳定
- `.avi` 文件被 vlc/ffplay 广泛支持，方便验证

### 5.4 线程安全设计

```cpp
// 录像操作全部在 lock_guard 下执行
int writeRecordFrame(...) {
    std::lock_guard<std::mutex> lock(m_recordMtx);  // ← 保护 m_recordFile
    // ... fseek + fwrite ...
}

// isRecording() 使用 std::atomic<bool>
bool isRecording() const { return m_recording; }  // 无锁读取
```

- `m_recordMtx` 保护 `fileno()` 相关的 `fwrite`/`fseek` 操作（并发写入导致文件交错）
- `m_recording` 用 `std::atomic<bool>`，采集线程无锁判断是否继续写帧
- `savePhoto()` 天然线程安全：每次打开独立文件句柄，写入后关闭

---

## 六、与 main.cpp 的集成

### 6.1 初始化

```cpp
// main.cpp: 构造时自动创建 /tmp/smartcam/photos 和 /tmp/smartcam/videos
StorageManager storage("/tmp/smartcam/photos", "/tmp/smartcam/videos");
g_storage = &storage;  // 全局指针，给采集线程访问
```

### 6.2 拍照回调

```cpp
gui.onCaptureRequest([capture]() {
    std::lock_guard<std::mutex> lock(g_state.mtx);
    if (g_state.format == PixelFormat::FMT_MJPEG) {
        // MJPEG 模式：帧数据已经是 JPEG，直接保存
        g_storage->savePhoto(g_state.frameData.data(),
                             g_state.frameData.size());
    }
#ifdef HAS_LIBJPEG
    else if (g_state.format == PixelFormat::FMT_YUYV) {
        // YUV 模式：先编码为 JPEG 再保存
        uint8_t* jpeg_out = nullptr;
        unsigned long jpeg_len = 0;
        VideoProcessor::encodeYUYVtoJPEG(
            g_state.frameData.data(),
            g_state.width, g_state.height,
            85, &jpeg_out, &jpeg_len);
        g_storage->savePhoto(jpeg_out, jpeg_len);
        free(jpeg_out);
    }
#endif
});
```

### 6.3 录像回调

```cpp
gui.onRecordToggle([capture, &gui](bool start) -> bool {
    if (start) {
        // 检查必须是 MJPEG 格式
        if (g_state.format != PixelFormat::FMT_MJPEG) return false;
        if (g_storage->startRecord(w, h, fps) == 0) {
            g_recording = true;
            gui.setRecordingStatus(true);
            return true;
        }
        return false;
    } else {
        g_recording = false;
        g_storage->stopRecord();
        gui.setRecordingStatus(false);
        return true;
    }
});
```

### 6.4 采集线程写入帧

```cpp
// 采集线程 lambda 内，每获取一帧后：
if (g_recording && fb.format == PixelFormat::FMT_MJPEG && g_storage) {
    g_storage->writeRecordFrame(fb.data, fb.length);
}
```

---

## 七、文件命名规则

| 类型 | 路径示例 | 规格 |
|------|----------|------|
| 照片 | `/data/photos/20260523/IMG_20260523_143025.jpg` | `IMG_YYYYMMDD_HHMMSS.jpg` |
| 录像 | `/data/videos/20260523/VID_20260523_143025.avi` | `VID_YYYYMMDD_HHMMSS.avi` |

`makeDatePath()` 函数自动按当前时间生成路径，并递归创建日期子目录：

```cpp
std::string makeDatePath(base, prefix, ext) {
    // 1. localtime() 获取当前时间
    // 2. strftime() 生成 "20260523" + "143025"
    // 3. ensureDir(base/20260523/)  → mkdir 递归创建
    // 4. 返回 "base/20260523/prefix_20260523_143025.ext"
}
```

---

## 八、空间管理实现

### 8.1 `getFreeSpaceMB()`

```cpp
int StorageManager::getFreeSpaceMB(const std::string& path) {
    struct statvfs vfs;
    statvfs(path.c_str(), &vfs);
    unsigned long long freeBytes =
        (unsigned long long)vfs.f_bsize * vfs.f_bavail;
    return (int)(freeBytes / (1024 * 1024));
}
```

### 8.2 `autoCleanup(keep_mb)`

1. `opendir()` 遍历 `photoDir` 和 `videoDir` 下的日期子目录 (`YYYYMMDD/`)
2. 对每个 `d_type == DT_REG` 文件，`stat()` 获取 `st_mtime`
3. `std::sort()` 按时间升序 → 最旧的文件排最前
4. 循环 `unlink()` 删除 → 直到 `getFreeSpaceMB() >= keep_mb` 或无文件可删

清理逻辑以 100MB 为默认阈值，可在初始化时通过 `config` 调整。

---

## 九、典型用法

### 9.1 拍照

```cpp
// 采集线程获取到 MJPEG 帧后
FrameBuffer fb;
capture->getFrame(&fb, 1000);

// 保存为 JPEG（自动按日期命名）
std::string path = storage->savePhoto(fb.data, fb.length);
// → /data/photos/20260523/IMG_20260523_143025.jpg
```

### 9.2 录像

```cpp
// 开始
storage->startRecord(640, 480, 30);

// 采集线程每帧写入
while (recording) {
    capture->getFrame(&fb, 1000);
    storage->writeRecordFrame(fb.data, fb.length);  // "00dc" chunk
}

// 停止 → 回填 header + 写 idx1
storage->stopRecord();

// 产物: VLC/ffplay 可直接播放的 AVI 文件
```

### 9.3 空间管理

```cpp
int freeMB = storage->getFreeSpaceMB();
if (freeMB < 50) {
    storage->autoCleanup(100);  // 清理到至少 100MB 剩余
}
```

---

## 十、面试追问准备

### Q：为什么 MJPEG 不压缩直接存？

> MJPEG 是摄像头硬件编码的 JPEG 流，已经压缩过一次。二次编码只会降低画质且浪费 CPU。直接 "00dc" 封装可以让 AVI 文件中的每一帧保持原始画质，CPU 零开销。

### Q：AVI 文件为什么能直接播放？

> 因为遵循了标准 RIFF 容器格式。VLC/ffplay 会先读取 `hdrl` 中的 `avih` 和 `strh` 获取编码器类型 "MJPG"、分辨率、帧率，然后解析 `movi` 中的 "00dc" 块，最后一帧一帧解码 JPEG 数据播放。`idx1` 索引块还让播放器支持随机拖拽。

### Q：如果录制中途断电怎么办？

> 当前实现的弱点是 header 在 Stop 时才回填。中途断电会导致 AVI 文件不完整。改进方案：在内存中维护 `FrameIndex` 列表，Stop 时一次性回填。如果追求断点续录，可以定期 `fflush()` + `fdatasync()` 并在文件中预留 idx1 区域（已知最大帧数时）。对于本项目，8GB SD 卡 + 定时录制场景，断电恢复优先级较低。

### Q：YUV 模式拍照为什么需要 libjpeg-turbo？

> 摄像头 YUYV 格式输出的是原始像素数据，浏览器和图片查看器无法直接打开。开发板无 VPU，使用 libjpeg-turbo（ARM NEON 优化版）做 YUYV→RGB24→JPEG 编码，640x480 约 25ms，勉强可用。推荐使用 MJPEG 模式拍照（硬件已编码），CPU 零开销。

---

## 十一、构建配置

```cmake
# CMakeLists.txt 中的 STORAGE_SOURCES
set(STORAGE_SOURCES
    src/storage/manager.cpp
    include/storage/manager.h
)

set(ALL_SOURCES
    ${CAMERA_SOURCES}
    ${DISPLAY_SOURCES}
    ${NETWORK_SOURCES}
    ${STORAGE_SOURCES}    # ← 本模块
    ${MAIN_SOURCES}
)
```

无外部库依赖，纯 C 标准库 + POSIX 系统调用，ARM/x86 零配置编译。
