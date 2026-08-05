# src/storage/ 模块面试复习指南（存储管理）

> 定位：以「资深嵌入式存储/文件系统面试官 + 应聘者」双视角，对 `src/storage/manager.cpp`（约 1040 行）与 `include/storage/manager.h`（约 390 行）做**逐函数级**拆解，全程结合代码。
> 阅读前建议先通读这两个文件，并对照 `src/main.cpp` 的调用点（处理线程 `writeRecordFrame`、TCP/GUI 的 `startRecord`/`stopRecord`/`savePhoto`）、`src/display/gallery.cpp`（相册消费 `listPhotos`/`listVideos`/`extractAviThumbnail`）、`src/display/video_player.cpp`（AVI 播放/seek）、`docs/debug-summary.md`（AVI 容器排障）与 `docs/learn/04-storage-module-implementation.md`。
>
> 组织方式：**模块全景（概览）→ 逐函数详解（代码意图 + 面试官追问/理想应答）→ 综合思辨（可替换性/横向关联/设计模式）**。

---

## 目录

1. [第一部分 模块全景（概览）](#第一部分-模块全景概览)
   - 1.1 模块职责与边界
   - 1.2 技术选型与背景（文件I/O/是否用数据库/按时间检索）
   - 1.3 数据流与控制流（编码器→落盘的完整路径）
   - 1.4 模块接口与解耦
   - 1.5 线程安全模型
2. [第二部分 逐函数详解（含代码意图 + 追问）](#第二部分-逐函数详解含代码意图--追问)
   - 2.1 目录工具：makeDatePath / ensureDir
   - 2.2 拍照：savePhoto
   - 2.3 录像核心：startRecord
   - 2.4 录像核心：writeRecordFrame（含 AVI chunk 与 WORD 对齐）
   - 2.5 录像收尾：stopRecord / finalizeAvi（回填 + idx1）
   - 2.6 AVI 头部封装：writeAviHeader（逐字段）
   - 2.7 空间管理：getFreeSpaceMB / autoCleanup
   - 2.8 相册查询：listPhotos / listVideos / getPhotoCount / getVideoCount
   - 2.9 删除与缩略图：deletePhoto / deleteVideo / readJpegSize / extractAviThumbnail
3. [第三部分 面试攻防演练（高频追问）](#第三部分-面试攻防演练高频追问)
   - 3.1 可靠性（掉电/拔卡/损坏）
   - 3.2 并发（录像+抓图/锁竞争）
   - 3.3 性能瓶颈（帧率/IO/排查）
   - 3.4 存储介质（低速 SD 卡/4GB 限制）
   - 3.5 文件系统与格式（FAT32/exFAT/AVI/MP4）
4. [第四部分 综合思辨（系统观）](#第四部分-综合思辨系统观)
   - 4.1 模块解耦与可替换性
   - 4.2 与网络传输的资源竞争
   - 4.3 数据索引与回放
   - 4.4 设计模式评估
   - 4.5 面试「一句话总结」

---

# 第一部分 模块全景（概览）

## 1.1 模块职责与边界

`StorageManager` 的职责（头文件注释 `manager.h:1-32` 明示）：

| 职责 | 说明 |
|------|------|
| **拍照** | JPEG 数据 → 按日期分目录的文件（`savePhoto`） |
| **录像** | MJPEG 帧流 → AVI（RIFF 容器）封装（`startRecord`/`writeRecordFrame`/`stopRecord`） |
| **空间管理** | 查询剩余空间（`getFreeSpaceMB`）、自动清理旧文件（`autoCleanup`） |
| **相册浏览** | 列照片/视频、按日期分组、删除、缩略图提取（`listPhotos`/`listVideos`/`deletePhoto`/`extractAviThumbnail`） |

**边界**（本模块不负责）：
- **不压缩编码**——JPEG 由摄像头硬件直出或 `VideoProcessor` 编码，存储只"收字节流"
- **不管理 V4L2 缓冲**——那是 `src/camera/`
- **不直接触屏显示**——相册 UI 在 `src/display/gallery.cpp`

**一句话**：存储模块是"数据落盘 + 文件管理"的需求侧——上接处理线程的帧流，下接 SD/eMMC 文件系统，旁挂相册的查询接口。

## 1.2 技术选型与背景

> **先纠正一个常见误解**：面试题库常假设"H.264 NAL / SQLite / aio / libuv / MP4"。**本项目实情是：MJPEG + AVI 自封装 + 标准 C 库 fopen/fwrite + 无数据库**。回答必须基于实情，并解释"为什么这么做"。

### 1.2.1 文件 I/O：为什么用标准 C 库而非 POSIX / mmap / O_DIRECT？

| 候选 | 本项目 | 理由 |
|------|--------|------|
| **标准 C 库** `fopen/fwrite/fclose` | ✅ **选用** | ① i.MX6ULL 无 VPU，只能 MJPEG（硬件直出 JPEG），单帧 30~100KB、30fps 码率 ~2MB/s，**stdio 带缓冲足够**；② `fwrite` 有用户态缓冲，配合 `fflush` 可控落盘粒度，代码简洁；③ 可移植 |
| POSIX `open/write` | 未用 | 能省一次用户态拷贝，但 2MB/s 量级收益可忽略 |
| `mmap` | 未用 | 帧数据是"实时流"不是"文件视图"，mmap 无意义 |
| `O_DIRECT` 绕过页缓存 | 未用 | 需 512 字节对齐（MJPEG 帧长不固定，要额外拷贝对齐），内存压力未到那个量级 |

**关键判断**：**量级决定选型**——MJPEG 2MB/s 的写入量，stdio 的"用户态缓冲 + 内核页缓存 + 异步刷盘"链路完全够用，任何更底层的优化都是过度设计。

### 1.2.2 为什么不用 SQLite / 轻量数据库？

- **数据量级**：录像/照片是"天 × 几十个文件"，目录扫描耗时 <100ms，SQLite 的索引收益不成立
- **内存**：可用内存 <200MB，SQLite 多占 500KB+，不值
- **复杂度**：引入第三方库增加交叉编译与崩溃面

**按时间检索靠什么**：目录结构 + 文件名 + mtime。

```cpp
// manager.cpp:48-67 makeDatePath —— 文件名即时间戳
std::string StorageManager::makeDatePath(const std::string& base,
                                         const std::string& prefix,
                                         const std::string& ext) {
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);

    char dateDir[16];   // 如 "20260523"
    strftime(dateDir, sizeof(dateDir), "%Y%m%d", tm_info);
    char timeStr[16];   // 如 "143025"
    strftime(timeStr, sizeof(timeStr), "%H%M%S", tm_info);

    std::string fullDir = base + "/" + dateDir;
    ensureDir(fullDir);
    ...
    oss << fullDir << "/" << prefix << "_" << dateDir << "_" << timeStr << ext;
    return oss.str();
}
```

生成路径：`/data/photos/20260523/IMG_20260523_143025.jpg`。
- **按日期检索** = 进对应日期目录
- **按时间排序** = `listPhotos` 里 `std::sort` 按 `st_mtime` 倒序（见 2.8）
- **无数据库的代价**：全量扫描 O(N)，但 N 小（几十~几百）时毫秒级，可接受

【面试官追问】"如果照片量达到 10 万张，目录扫描会不会卡死 GUI？"

> 【理想应答】会，这是当前实现的可扩展性上限。`listPhotos` 是全量 `opendir` + `stat` 扫描，10 万张时单次扫描可能几百 ms。但当前**没在 GUI 主线程直接全量跑**：相册组件在自己的刷新时机调用，且 `includeInfo=false` 时不解析文件内容（只 stat 元数据，快）；GUI 侧还有"只加载可见缩略图"的懒加载兜底。真要支撑海量文件，应上 SQLite 或 `inotify` 增量索引。**诚实说出上限 + 给出演进路径**，比硬吹"我做了数据库"更可信。

## 1.3 数据流与控制流

### 完整落盘路径（MJPEG 模式，零编码直写）

```
摄像头 DMA
  → V4L2 mmap 缓冲（引用#0，零拷贝）
  → 采集线程：assign 深拷贝到 g_state（拷贝#1）→ putFrame 归还
  → 处理线程：localFrame 深拷贝（拷贝#2，锁外）→ 推流 / 录像
       └→ g_storage->writeRecordFrame(localFrame.data(), size)
             └→ StorageManager: writeFourCC("00dc") + writeU32(len) + fwrite(JPEG)
                                + [奇数补1字节] + fflush
```

### 控制流（谁调用存储）

| 动作 | 调用点 | 线程 |
|------|--------|------|
| 拍照（GUI） | `main.cpp:1271` `onCaptureRequest` → `savePhoto` | Qt 主线程 |
| 拍照（TCP） | `main.cpp:751` `CMD_CAPTURE` handler → `savePhoto` | 控制线程 |
| 开始录像 | `main.cpp:1315` `onRecordToggle` → `startRecord` | Qt 主线程 |
| 录像写帧 | `main.cpp:1053` 处理线程 → `writeRecordFrame` | **处理线程** |
| 停止录像 | `main.cpp:1325` `onRecordToggle` → `stopRecord` | Qt 主线程 |

**关键**：`writeRecordFrame` 是**唯一在录像热路径上**的调用，且它运行在**处理线程**（不是采集线程、不是 GUI 线程）——这是"磁盘慢不拖垮取帧"的根基（详见 1.5）。

## 1.4 模块接口与解耦

**API 一览**（`manager.h`）：

| 类别 | API | 线程安全 |
|------|-----|---------|
| 拍照 | `savePhoto(jpeg_data, len)` | 可跨线程（无共享状态） |
| 录像 | `startRecord(w,h,fps)` / `writeRecordFrame()` / `stopRecord()` / `isRecording()` / `currentRecordPath()` | `m_recordMtx` 保护 |
| 空间 | `getFreeSpaceMB(path)` / `autoCleanup(keep_mb)` | 只读/独立锁 |
| 相册 | `listPhotos` / `listVideos` / `getPhotoCount` / `getVideoCount` / `deletePhoto` / `deleteVideo` | 只读目录 |
| 工具 | `readJpegSize` / `extractAviThumbnail` | 静态，只读文件 |

**解耦方式**：
- main.cpp 直接持有 `StorageManager*`（`g_storage`，`main.cpp:353`），通过 `gui.setGalleryStorage(&storage)`（`main.cpp:356`）传给相册——**GUI 相册直接调用** `listPhotos`/`deletePhoto` 等（`gallery.cpp:59,69,635`）
- 录像/拍照由处理线程/GUI 回调触发，存储不感知谁在调

**为什么 GUI 直接持指针而非回调？**（`gallery.cpp:59` `m_storage->listPhotos(...)`）——相册需要**大量双向数据交互**（列目录、删文件、读缩略图、查空间，十几个方法），全走回调太啰嗦；而 camera 控制低频语义简单，用回调更轻。**松紧结合**是合理设计。

**异常通知上层**：当前靠**返回值**（`savePhoto` 空串、`writeRecordFrame` -1）+ **日志**（LOG_ERR_）。**未做**事件回调/信号通知 GUI 弹"存储已满"——可演进点（见 4.4 观察者）。

## 1.5 线程安全模型

```cpp
// manager.h:366-374 —— 录像状态（关键成员）
std::atomic<bool> m_recording{false};   // 跨线程标志
mutable std::mutex m_recordMtx;         // 录像操作互斥锁
FILE*             m_recordFile = nullptr;
std::string       m_recordPath;
std::vector<FrameIndex> m_frameIndexList;  // idx1 索引（内存累计）
```

**设计**：
- `m_recording` 是 **atomic**——`isRecording()` 可从任意线程无锁读
- `startRecord`/`writeRecordFrame`/`stopRecord` 全部 `std::lock_guard(m_recordMtx)`——保证"同一时刻只有一个线程动录像文件"
- 这样 `startRecord`（GUI 线程）与 `writeRecordFrame`（处理线程）不冲突：GUI 点停止时，处理线程正在写的帧会等锁，写完后才 `stopRecord` 回填

**注意**：这个锁是**串行化**的——录像写帧期间 GUI 调 `stopRecord` 会等当前帧写完。对 30fps 单帧 ~1ms 的写入，等待可接受。

---

# 第二部分 逐函数详解（含代码意图 + 追问）

## 2.1 目录工具：makeDatePath / ensureDir

### makeDatePath（`manager.cpp:48-67`）

已在 1.2.2 展示。要点：`strftime` 格式化日期目录与时间戳，`ensureDir` 保证目录存在，返回完整路径。

### ensureDir（`manager.cpp:69-92`）—— 递归建目录

```cpp
bool StorageManager::ensureDir(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {          // 已存在
        if (S_ISDIR(st.st_mode)) return true;
        LOG_ERR_("... not a directory"); return false;
    }
    if (mkdir(path.c_str(), 0755) == 0) return true;   // 直接建成功
    if (errno == ENOENT) {                       // 父目录不存在 → 递归
        size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0) {
            if (!ensureDir(path.substr(0, slash))) return false;
            return (mkdir(path.c_str(), 0755) == 0);
        }
    }
    LOG_ERR_("... strerror(errno)"); return false;
}
```

**意图**：实现 `mkdir -p` 语义。`/data/photos/20260523` 首次写入时 `20260523` 不存在、父目录 `/data/photos` 也不存在（首次启动）→ 递归先建父再建子。**这是嵌入式相机"首次开机建目录树"的标准需求**。

【面试官追问】"为什么不用系统调用 `mkdir -p`？"

> 【理想应答】`mkdir -p` 是 shell 工具不是系统调用；标准 C 没有跨平台递归建目录函数，所以手写。递归终止条件：`path == "/"` 或 `slash <= 0` 时不再递归。错误处理：`errno == ENOENT` 才递归（其他错误如 EACCES 直接失败），避免无限递归。这个函数在 `savePhoto` 和 `startRecord` 里都被间接调用，是"写文件前先保证目录存在"的防御。

## 2.2 拍照：savePhoto（`manager.cpp:98-126`）

```cpp
std::string StorageManager::savePhoto(const uint8_t* jpeg_data, int len) {
    if (!jpeg_data || len <= 0) { LOG_ERR_(...); return ""; }

    std::string path = makeDatePath(m_photoDir, "IMG", ".jpg");

    FILE* fp = fopen(path.c_str(), "wb");       // 二进制写
    if (!fp) { LOG_ERR_(...); return ""; }

    size_t written = fwrite(jpeg_data, 1, static_cast<size_t>(len), fp);
    fclose(fp);

    if (written != static_cast<size_t>(len)) {  // 短写检测
        LOG_ERR_("fwrite short: ...");
        unlink(path.c_str());                   // 删除半成品
        return "";
    }
    LOG_INF("Photo saved: %s (%d bytes)", ...);
    return path;
}
```

**设计意图**：
- 一次性 `fwrite` 整张 JPEG（照片是单帧，非流式）→ 简单可靠
- **短写检测 + 删除半成品**：SD 卡满/拔出时 `fwrite` 可能只写了部分字节，此时删除不完整文件，**不把坏照片留在相册里**
- 失败返回空字符串，上层（`main.cpp:1271`）据此判定失败

【面试官追问】"照片写入失败时，相册怎么知道？会不会显示坏图？"

> 【理想应答】两层：① `savePhoto` 返回空串，上层 `LOG_INF("Photo captured: FAILED")` 记录；② 相册侧 `createThumbnail` 解码失败会显示占位符（"?"），不崩溃（display 篇有 `scale_denom` + 静默错误处理器）。**当前确实没有"拍照失败弹窗"**——这是体验可改进点，可通过 `onCaptureRequest` 回调的返回值扩展到 GUI。

## 2.3 录像核心：startRecord（`manager.cpp:132-179`）

```cpp
int StorageManager::startRecord(int width, int height, int fps) {
    std::lock_guard<std::mutex> lock(m_recordMtx);

    if (m_recording) { LOG_WRN("Already recording"); return -1; }   // 幂等
    if (width <= 0 || height <= 0 || fps <= 0) return -1;           // 参数校验

    std::string path = makeDatePath(m_videoDir, "VID", ".avi");
    m_recordFile = fopen(path.c_str(), "wb");
    if (!m_recordFile) { LOG_ERR_(...); return -1; }

    // 记录参数 + 清空索引
    m_recordPath = path; m_recordWidth = width; m_recordHeight = height;
    m_recordFps = fps; m_recordFrameCount = 0;
    m_frameIndexList.clear();
    m_moviDataOffset = 0; m_hdrlListOffset = 0; m_rifSizeOffset = 0;
    m_strhLengthOffset = 0; m_avihFramesOffset = 0;

    if (writeAviHeader() < 0) {            // 写 AVI 头
        LOG_ERR_(...);
        fclose(m_recordFile); m_recordFile = nullptr;
        unlink(path.c_str());              // 头写失败 → 删文件
        return -1;
    }
    m_recording = true;
    LOG_INF("Recording started: %s (%dx%d @ %dfps)", ...);
    return 0;
}
```

**设计意图**：
- **幂等**：已在录则拒绝（防止 GUI 连点开始）
- **参数校验**：非法参数（0 宽高等）直接拒绝
- **5 个偏移量清零**：为 `finalizeAvi` 的回填做准备（见 2.5）
- **头写失败 → 删文件**：不留下无头的坏 AVI

## 2.4 录像核心：writeRecordFrame（`manager.cpp:181-225`）

```cpp
int StorageManager::writeRecordFrame(const uint8_t* jpeg_data, int len) {
    std::lock_guard<std::mutex> lock(m_recordMtx);

    if (!m_recording || !m_recordFile || !jpeg_data || len <= 0) return -1;

    // 记录当前帧在 movi 数据区的偏移（用于 idx1）
    long currentPos = ftell(m_recordFile);
    long frameDataOffset = currentPos;     // 数据区起始偏移

    // 写入 "00dc" 帧块：chunk 标识 + 长度 + JPEG 数据
    writeFourCC(m_recordFile, "00dc");
    writeU32(m_recordFile, static_cast<uint32_t>(len));
    size_t written = fwrite(jpeg_data, 1, static_cast<size_t>(len), m_recordFile);

    // RIFF 规范：chunk 数据必须 WORD(2字节) 对齐，奇数大小需补 1 字节
    if (len % 2 != 0) { uint8_t pad = 0; fwrite(&pad, 1, 1, m_recordFile); }

    fflush(m_recordFile);                  // 刷到内核页缓存

    if (written != static_cast<size_t>(len)) { LOG_ERR_("short"); return -1; }

    if (m_recordFrameCount == 0)           // 第一帧：记录 movi 数据区偏移
        m_moviDataOffset = frameDataOffset;

    FrameIndex idx;
    idx.offset = static_cast<uint32_t>(frameDataOffset - m_moviDataOffset);
    idx.length = static_cast<uint32_t>(len);   // 帧数据大小（不含 chunk 头）
    m_frameIndexList.push_back(idx);           // 内存索引（供 finalizeAvi 回写）

    m_recordFrameCount++;
    return 0;
}
```

**AVI 帧块内存布局**（每帧在 movi 区的字节序列）：

```
[00dc(4B)] [len(4B, 小端)] [JPEG 数据 len 字节] [奇数时补 1 字节 0x00]
└─ chunk 头 8B ──────────┘ └──────── 帧数据 ────────┘
```

**两个关键细节**：
1. **WORD 对齐**：RIFF 规范要求每个 chunk 的数据区起始偏移是偶数（16-bit 对齐）。JPEG 帧长奇数时补 1 字节 0x00，否则下一个 chunk 的 `fseek`/解析会错位（VLC 播放会花屏/进度错乱）。
2. **`ftell` 记偏移**：每个 `00dc` 的起始位置相对 movi 数据区（`m_moviDataOffset`）的偏移记录到 `m_frameIndexList`——这是 `finalizeAvi` 写 idx1 的数据源。

**`fflush` 的作用**：把 stdio 用户态缓冲刷到内核页缓存。为什么每帧刷？保证"录到哪一帧，文件里就有哪一帧"的语义——`stopRecord` 前的任何时刻，已写入的帧都在文件里（页缓存层）。**注意：`fflush` 不等于 `fsync`**——页缓存还没落盘到物理介质（见 3.1）。

【面试官追问】"为什么每帧 fflush 而不是靠 stdio 自动刷？会不会很慢？"

> 【理想应答】stdio 默认缓冲 4KB，MJPEG 帧 30~100KB，**一帧就可能填满多个缓冲**，会自动刷；但"自动刷"的时机不受控——用户可能看到"录了但文件里没有"。每帧 `fflush` 是**语义保证**：把"已写入帧"和"文件可见"对齐。性能上 `fflush` 是 `write()` 系统调用 + 页缓存复制，2MB/s 量级下占 CPU 极小（<1%），不是瓶颈。**真正的落盘是内核异步刷盘，`fflush` 不等待物理写完成**——这是与 `fsync` 的本质区别。

## 2.5 录像收尾：stopRecord / finalizeAvi（`manager.cpp:227-256` + `391-446`）

### stopRecord

```cpp
int StorageManager::stopRecord() {
    std::lock_guard<std::mutex> lock(m_recordMtx);
    if (!m_recording || !m_recordFile) { LOG_WRN("not recording"); return -1; }

    m_recording = false;
    LOG_INF("Recording stopping: wrote %d frames", m_recordFrameCount);

    int ret = finalizeAvi();           // 回填头 + 写 idx1
    fclose(m_recordFile);
    m_recordFile = nullptr;
    m_frameIndexList.clear();
    if (ret < 0) LOG_ERR_("finalizeAvi failed, file may be corrupt: %s", ...);
    return (ret < 0) ? -1 : 0;
}
```

### finalizeAvi（核心回填逻辑）

```cpp
int StorageManager::finalizeAvi() {
    if (!m_recordFile || m_frameIndexList.empty()) return -1;
    FILE* fp = m_recordFile;

    // ① 回填 movi LIST size（m_moviDataOffset 往前 8 字节是 LIST size 字段）
    long moviEnd = ftell(fp);
    long moviSize = moviEnd - m_moviDataOffset + 4;   // +4 补 "movi" FOURCC
    long moviSizePos = m_moviDataOffset - 8;
    fseek(fp, moviSizePos, SEEK_SET);
    writeU32(fp, static_cast<uint32_t>(moviSize));
    fseek(fp, moviEnd, SEEK_SET);

    // ② 写 idx1 索引块
    writeFourCC(fp, "idx1");
    uint32_t idx1Size = m_frameIndexList.size() * sizeof(AviIndexEntry);
    writeU32(fp, idx1Size);
    for (size_t i = 0; i < m_frameIndexList.size(); ++i) {
        AviIndexEntry entry;
        entry.ckid = 0x63643030;         // "00dc"
        entry.dwFlags = 0x10;            // 关键帧
        entry.dwChunkOffset = m_frameIndexList[i].offset;
        entry.dwChunkLength = m_frameIndexList[i].length;
        fwrite(&entry, sizeof(entry), 1, fp);
    }

    // ③ 回填 avih.dwTotalFrames 和 strh.dwLength
    if (m_avihFramesOffset > 0) {
        fseek(fp, m_avihFramesOffset, SEEK_SET);
        writeU32(fp, m_frameIndexList.size());
    }
    if (m_strhLengthOffset > 0) {
        fseek(fp, m_strhLengthOffset, SEEK_SET);
        writeU32(fp, m_frameIndexList.size());
    }

    // ④ 回填 RIFF 文件总大小
    long riffEnd = ftell(fp);
    long riffSize = riffEnd - 8;         // RIFF size = 文件大小 - 8
    if (m_rifSizeOffset > 0) {
        fseek(fp, m_rifSizeOffset, SEEK_SET);
        writeU32(fp, static_cast<uint32_t>(riffSize));
    }
    fseek(fp, 0, SEEK_END);
    fflush(fp);
    return 0;
}
```

**"边写边回填"策略的完整逻辑**：

```
录制中（writeRecordFrame）：
  写帧数据 → 记录每个偏移（m_moviDataOffset / m_avihFramesOffset / ...）
             → 记录每帧 idx {offset, length} 到内存 m_frameIndexList
结束时（finalizeAvi）：
  fseek 到记录的偏移 → 回填真实值（movi size / 总帧数 / RIFF size）
  追加写 idx1 索引块
```

**为什么头里要写占位（0）而不是最后一次性写头？** 因为 AVI 头的 `dwTotalFrames` 在录制开始时**不知道会录多少帧**。两种解法：
- **方案 A（本项目）**：先写占位 → 记录偏移 → 结束回填。优点：流式写、文件头信息完整、VLC 兼容
- 方案 B：录完再一次性写整个头。缺点：如果录制中崩溃，连头都没有，文件完全不可读

**方案 A 的掉电缺陷**：录制中崩溃，头里 `dwTotalFrames=0`、`movi size`/`RIFF size` 未回填、idx1 没写——但帧数据（00dc chunks）已顺序写入，**有经验的播放器能容错播放**（见 3.1）。

【面试官追问】"为什么要记录 `m_moviDataOffset - 8` 这个偏移？"

> 【理想应答】因为 movi 的 `LIST size` 字段位置在 `LIST`(4B) 之后、`movi`(4B) 之前：布局是 `[LIST(4)][size(4)][movi(4)][00dc...]`。`m_moviDataOffset` 指向第一个 `00dc` 的数据起始，所以 size 字段在 `m_moviDataOffset - 8` 处（8 = LIST 的 4 字节 + "movi" 的 4 字节）。`moviSize` 计算 `moviEnd - m_moviDataOffset + 4` 是因为 size 字段的值 = 从 "movi" FOURCC 之后到 LIST 结束的长度（含所有帧数据 + idx1 之前）。**这些偏移算术是手写容器最容易错的地方**，`debug-summary.md` 记录了实际踩过的坑（`dwChunkLength` 曾多算 chunk 头导致 VLC 进度错乱）。

## 2.6 AVI 头部封装：writeAviHeader（`manager.cpp:267-389`）

**结构体定义**（`manager.h:45-124`，全部 `#pragma pack(push,1)`）：

```cpp
#pragma pack(push, 1)   // ★ 关键：禁止编译器对齐填充
struct RiffChunk { uint32_t fourcc; uint32_t size; };
struct AviMainHeader {   // avih
    uint32_t dwMicroSecPerFrame;   // 1000000/fps
    uint32_t dwMaxBytesPerSec;
    uint32_t dwPaddingGranularity;
    uint32_t dwFlags;              // 0x10 = 含 idx1
    uint32_t dwTotalFrames;        // 0（录制中）→ 回填
    uint32_t dwInitialFrames;
    uint32_t dwStreams;            // 1 = 仅视频
    uint32_t dwSuggestedBufferSize;
    uint32_t dwWidth;
    uint32_t dwHeight;
    uint32_t dwReserved[4];
};
struct AviStreamHeader {  // strh：fccType="vids", fccHandler="MJPG", dwRate=fps ...
    ... dwScale=1; dwRate=fps; dwSampleSize=0(变长帧) ...
};
struct BitmapInfoHeader { // strf：biCompression="MJPG", biBitCount=24 ...
    ... biWidth; biHeight; biCompression=0x47504A4D("MJPG"); ...
};
struct AviIndexEntry { uint32_t ckid; uint32_t dwFlags; uint32_t dwChunkOffset; uint32_t dwChunkLength; };
struct AviFrameChunk { uint32_t ckid; uint32_t size; /* JPEG data */ };
#pragma pack(pop)
```

**为什么 `#pragma pack(push,1)` 是必须的？** AVI 是**标准文件格式**，VLC/Windows Media Player 按规范逐字节解析。编译器默认会给 `uint32_t` 等成员做对齐填充，若结构体里出现 `uint16_t`（如 `AviStreamHeader` 的 `wPriority`/`wLanguage`），会在奇数偏移处插填充字节 → 写出的文件**比规范多出字节** → 播放器解析错位 → 花屏/打不开。`pack(1)` 保证 `fwrite(&avih, sizeof(avih), 1, fp)` 写出的字节与 RIFF 规范逐位一致。

**writeAviHeader 的写入序列**：

```
RIFF(size=0占位) "AVI "
  LIST(size=0占位) "hdrl"
    "avih"(size=40) AviMainHeader{...}
    LIST(size回填) "strl"
      "strh"(size=56) AviStreamHeader{...}
      "strf"(size=40) BitmapInfoHeader{...}
  LIST(size=0占位) "movi"     ← 之后所有帧数据都追加在这里
```

**偏移记录**（为回填做准备）：

```cpp
m_avihFramesOffset = ftell(fp) - sizeof(AviMainHeader)
                     + offsetof(AviMainHeader, dwTotalFrames);
m_strhLengthOffset = ftell(fp) - sizeof(AviStreamHeader)
                     + offsetof(AviStreamHeader, dwLength);
```

用 `offsetof` 精确算出 `dwTotalFrames`/`dwLength` 字段在文件中的绝对位置——回填时 `fseek` 直达。

**strl/hdrl LIST size 立即回填**（`manager.cpp:363-375`）：写完 strf 后立即 `ftell` 拿 `strlEnd`，算 `strlSize`，`fseek` 回去填——因为这两个 LIST 的大小在写头时**已经确定**（不含后续帧数据），可以当场填，不需要等结束。

【面试官追问】"为什么 avih/strh 的 size 要写成 sizeof(struct)，而不是手写 40/56？"

> 【理想应答】`sizeof(AviMainHeader)` 在 `#pragma pack(push,1)` 下恰好等于 40（AVI 规范值），用 sizeof 避免魔法数字、且与结构体定义自动同步。**前提是 pack(1) 保证无填充**——如果忘了 pack，sizeof 可能变 44/60，写出的头就不规范了。这也是为什么 `static_assert(sizeof(X)==N)` 验证是协议结构体的最佳实践（`control.h:191` 用了同样手法）。

## 2.7 空间管理：getFreeSpaceMB / autoCleanup（`manager.cpp:468-580`）

### getFreeSpaceMB

```cpp
int StorageManager::getFreeSpaceMB(const std::string& path) {
    std::string checkPath = path.empty() ? m_videoDir : path;
    struct statvfs vfs;
    if (statvfs(checkPath.c_str(), &vfs) < 0) { LOG_ERR_(...); return -1; }
    // 剩余空间 = 可用块数 × 块大小
    unsigned long long freeBytes =
        static_cast<unsigned long long>(vfs.f_bsize) *
        static_cast<unsigned long long>(vfs.f_bavail);
    return static_cast<int>(freeBytes / (1024 * 1024));
}
```

**`statvfs` 是查文件系统剩余空间的 POSIX 接口**。`f_bsize` = 块大小，`f_bavail` = 非 root 可用块数（比 `f_bfree` 更保守，为特权预留）。返回 MB。

### autoCleanup（循环覆盖核心）

```cpp
int StorageManager::autoCleanup(int keep_mb) {
    struct FileEntry { std::string path; time_t mtime; };
    std::vector<FileEntry> files;

    // ① 收集：遍历照片/录像目录的 8 位数字日期子目录，stat 拿 mtime
    auto collectFiles = [&files](const std::string& dir) {
        DIR* dp = opendir(dir.c_str());
        if (!dp) return;
        struct dirent* entry;
        while ((entry = readdir(dp)) != nullptr) {
            if (strcmp(entry->d_name,".")==0 || strcmp(entry->d_name,"..")==0) continue;
            if (strlen(entry->d_name) != 8) continue;          // 只处理 8 位日期目录
            bool allDigit = true;
            for (int i=0;i<8;++i) if (entry->d_name[i]<'0'||entry->d_name[i]>'9') allDigit=false;
            if (!allDigit) continue;
            std::string dateDir = dir + "/" + entry->d_name;
            DIR* dp2 = opendir(dateDir.c_str());
            if (!dp2) continue;
            struct dirent* file;
            while ((file = readdir(dp2)) != nullptr) {
                if (file->d_type != DT_REG) continue;          // 只删普通文件
                std::string fullPath = dateDir + "/" + file->d_name;
                struct stat st;
                if (stat(fullPath.c_str(), &st) == 0)
                    files.push_back({fullPath, st.st_mtime});
            }
            closedir(dp2);
        }
        closedir(dp);
    };
    collectFiles(m_photoDir);
    collectFiles(m_videoDir);

    // ② 排序：按 mtime 升序（最旧在前）
    std::sort(files.begin(), files.end(),
              [](const FileEntry& a, const FileEntry& b) { return a.mtime < b.mtime; });

    // ③ 删除：直到剩余空间 >= keep_mb 或无文件可删
    int currentFree = getFreeSpaceMB();
    for (const auto& fe : files) {
        if (currentFree >= keep_mb) break;
        if (unlink(fe.path.c_str()) == 0) LOG_DBG("deleted %s", fe.path.c_str());
        else LOG_WRN("unlink(%s) failed: %s", fe.path.c_str(), strerror(errno));
        // 注意：此处 currentFree 未及时刷新，循环内实际靠 stat 判断
    }
    currentFree = getFreeSpaceMB();
    return currentFree;
}
```

**设计意图**：
- **只清 8 位数字目录**：防止误删用户放在目录里的其他文件（白名单式清理）
- **只删普通文件**（`d_type == DT_REG`）：跳过目录/链接
- **按 mtime 升序 = 最旧的先删**：FIFO 覆盖策略（循环录像的核心语义）
- **直到 keep_mb 阈值**：不是删固定数量，而是"删到够用为止"

【面试官追问】"删除逻辑里 currentFree 为什么没在循环内实时刷新？会不会多删或漏删？"

> 【理想应答】这是一个**实现缺陷/简化**：`currentFree` 在进入循环前取一次，循环内删除后**没有重新 `statvfs`**，导致 `if (currentFree >= keep_mb)` 的判断用的是旧值——可能多删几个文件（删除后实际已够但没退出循环）。注释里也写了"删除后不立即触发 statvfs 刷新，估算"。**正确做法**：每删 N 个文件重新 `statvfs`，或按文件累计大小估算。诚实指出这个细节 + 给修复方案，面试加分。

【面试官追问】"autoCleanup 会和正在进行的录像冲突吗？会删掉正在写的文件吗？"

> 【理想应答】**有隐患**。① 锁冲突：`autoCleanup` 内部没加 `m_recordMtx`，如果录像中调用，两者并行——`unlink` 正在写的文件在 Linux 上是允许的（inode 引用还在，写不会崩），但**已打开的 FILE 指针写的数据会"丢失"**（文件已删，写继续但不可见）。② 防御缺失：没检查 `fe.path != m_recordPath`，理论上可能删到当前录像文件。**改进**：① `autoCleanup` 加锁或放到独立线程；② 删除前跳过 `m_recordPath`；③ 录像路径上按阈值预检（`startRecord` 前检查空间）。当前实现是"手动触发、非实时"，面试要诚实 + 给方案。

## 2.8 相册查询：listPhotos / listVideos / getPhotoCount / getVideoCount

### listPhotos（`manager.cpp:626-737`）

```cpp
int StorageManager::listPhotos(std::vector<PhotoDayGroup>& out, bool includeInfo) {
    out.clear();
    int totalCount = 0;
    DIR* dp = opendir(m_photoDir.c_str());
    ...
    // ① 收集所有 .jpg 文件（根目录 + 日期子目录），stat 拿 mtime
    struct RawEntry { std::string path; time_t mtime; };
    std::vector<RawEntry> rawEntries;
    // ... 双层 opendir 遍历 ...

    // ② 按时间倒序排序
    std::sort(rawEntries.begin(), rawEntries.end(),
              [](const RawEntry& a, const RawEntry& b) { return a.mtime > b.mtime; });

    // ③ 按日期分组（std::map，日期倒序输出）
    std::map<std::string, PhotoDayGroup> groupMap;
    for (const auto& re : rawEntries) {
        PhotoInfo info;
        info.path = re.path; info.timestamp = re.mtime;
        // 提取文件名、格式化日期/时间
        ...
        info.fileSize = st.st_size;
        if (includeInfo) readJpegSize(re.path, info.width, info.height);  // 可选：解析宽高
        groupMap[info.dateStr].photos.push_back(std::move(info));
        totalCount++;
    }
    for (auto it = groupMap.rbegin(); it != groupMap.rend(); ++it)
        out.push_back(std::move(it->second));
    return totalCount;
}
```

**设计意图**：
- **两层扫描**：兼容"根目录直接放文件（旧格式）"和"日期子目录（新格式）"
- **`includeInfo` 开关**：`false` 时只 stat 元数据（快，几十个文件 ms 级）；`true` 时才解析 JPEG 宽高（慢，`readJpegSize` 只读 4KB 不解码，见 2.9）——**"按需加载信息"**，GUI 相册初次列表用 `false`（`gallery.cpp:59` 实际传 `true`，因为相册要显示尺寸）
- **按 mtime 倒序**：最新在前，符合相册习惯
- **`std::map` 按日期分组**：`rbegin()` 迭代使日期倒序（最新日期组在前）

### listVideos（`manager.cpp:896-993`）

逻辑与 listPhotos 几乎相同，差异：
- 只匹配 `.avi`/`.AVI`
- `info.isVideo = true`
- **width/height 固定 0**——不解析 AVI 头（注释明确"视频不解析 AVI 头"）

【面试官追问】"为什么 listVideos 不解析 AVI 头拿宽高，而 listPhotos 可以 includeInfo 解析？"

> 【理想应答】① JPEG 的宽高在文件头（SOF 标记），只读前 4KB 即可解析，**成本极低**；② AVI 的宽高在 hdrl 的 avih 里，也要读文件头，其实也可以解析——但**视频列表通常不需要显示尺寸**（相册里视频显示缩略图 + 时长），当前偷懒设 0 是"够用即可"的取舍。若要显示，可复用 `extractAviThumbnail` 的头部解析逻辑扩展。**面试点**：诚实说这是简化，且知道怎么扩展。

### getPhotoCount / getVideoCount（`manager.cpp:739-767` / `995-1021`）

只 `opendir` 数文件数，**不 stat 每个文件**（比 listPhotos 快）——用于状态栏/角标显示"共 N 张照片"，避免全量扫描的开销。

## 2.9 删除与缩略图

### deletePhoto / deleteVideo（`manager.cpp:769-789` / `1023-1041`）

```cpp
int StorageManager::deletePhoto(const std::string& path) {
    if (unlink(path.c_str()) != 0) { LOG_ERR_(...); return -1; }
    LOG_INF("Photo deleted: %s", path.c_str());
    // 尝试清理空的日期目录（仅当在 photoDir 内且非根目录）
    size_t slash = path.rfind('/');
    if (slash != std::string::npos) {
        std::string dir = path.substr(0, slash);
        if (dir.find(m_photoDir) == 0 && dir != m_photoDir) {
            rmdir(dir.c_str());   // 忽略返回值：非空目录会失败，无害
        }
    }
    return 0;
}
```

**设计意图**：
- `unlink` 删除文件
- **顺手清理空日期目录**：删光某天所有照片后，`rmdir` 该日期目录（`rmdir` 只删空目录，非空自动失败，无害）——保持目录树干净
- **路径白名单检查**：`dir.find(m_photoDir) == 0` 防止误删 photoDir 外的目录

【面试官追问】"删除失败返回 -1，GUI 怎么处理？"

> 【理想应答】`gallery.cpp:634-638`：`info.isVideo ? m_storage->deleteVideo(info.path) : m_storage->deletePhoto(info.path)`，结果非 0 时弹窗汇报"删除失败"。多选删除 `onDeleteSelected` 会收集失败项统一弹窗。**删除后必须刷新列表**（否则相册还显示已删文件）——`deletePhoto` 后 GUI 调 `refresh()` 重建列表，且对索引越界做钳制回退（display 篇提过"删除后索引漂移"的处理）。

### readJpegSize（`manager.cpp:586-624`）—— 只读头，不解码

```cpp
bool StorageManager::readJpegSize(const std::string& path, int& w, int& h) {
    FILE* fp = fopen(path.c_str(), "rb");
    uint8_t buf[4096];                     // 只读前 4KB
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (n < 100 || buf[0] != 0xFF || buf[1] != 0xD8) return false;  // SOI 检查

    // 扫描标记段，找 SOF0/SOF1 (0xC0-0xC2)
    for (size_t i = 2; i < n - 8; i++) {
        if (buf[i] == 0xFF) {
            uint8_t marker = buf[i + 1];
            if (marker == 0xFF) continue;                  // 填充字节
            if (marker >= 0xD0 && marker <= 0xD7) continue; // 无数据标记
            if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
                if (i + 8 >= n) return false;
                h = (buf[i+5] << 8) | buf[i+6];            // 高度
                w = (buf[i+7] << 8) | buf[i+8];            // 宽度
                return (w > 0 && h > 0);
            }
            if (i + 3 < n) {                               // 跳过变长段
                uint16_t segLen = (buf[i+2] << 8) | buf[i+3];
                if (segLen >= 2) i += segLen + 1;
            }
        }
    }
    return false;
}
```

**设计意图**：JPEG 的 SOF（Start of Frame）标记 `0xFF 0xC0` 后第 5/6 字节是高（2B）、第 7/8 字节是宽（2B）。**只读前 4KB 就能拿到宽高**，不需要 `libjpeg` 解码整张图——这是"读元数据的最小代价"原则。标记段扫描要点：跳过填充（0xFF 0xFF）、跳过无数据标记（D0-D7）、变长段按段长度跳过。

### extractAviThumbnail（`manager.cpp:795-890`）—— AVI 封面提取

```cpp
bool StorageManager::extractAviThumbnail(const std::string& aviPath,
                                          std::vector<uint8_t>& jpegData) {
    // ① 校验 RIFF + "AVI " 头
    // ② 跳过 hdrl LIST → 定位 movi LIST
    // ③ 读第一个 "00dc" chunk → 提取其中的 JPEG 数据
    ...
}
```

**解析路径**：RIFF 头(12B) → 第一个 LIST(hdrl) 按 size 跳过 → 读第二个 LIST(movi) → 读第一个 chunk `"00dc"` → 读 `frameSize` → 读 frameSize 字节 JPEG。**只读第一帧**，不解码完整视频。

**sanity check**：`frameSize > 500MB` 拒绝（防恶意/损坏文件的超大分配）。

【面试官追问】"如果 AVI 头损坏但 movi 里有数据，extractAviThumbnail 还能工作吗？"

> 【理想应答】当前实现**假设头完好**：严格按"先 LIST(hdrl) 再 LIST(movi)"的顺序解析，若 hdrl size 字段损坏，`fseek` 会跳到错误位置，后续解析失败返回 false。**容错改进**：可改为"扫描式"——不依赖 size 字段，而是从头找 `LIST`+`movi` 特征码（暴力扫描前几 MB）。这是"严格解析 vs 容错解析"的取舍，当前选严格（自家格式可控），损坏时缩略图显示占位符，不崩溃。

---

# 第三部分 面试攻防演练（高频追问）

## 3.1 可靠性

### 【面试官追问】"你如何测试掉电场景下录像文件的完整性？遇到过损坏吗？"

**【理想应答】** 测试方法分层：
1. **脚本模拟**：录制中 `kill -9` 进程（模拟突然掉电），检查文件头字段（`dwTotalFrames`/`movi size`/`RIFF size`）是否被回填
2. **真机拔卡**：录制中直接拔 SD 卡，观察进程行为 + 事后文件状态
3. **工具验证**：`ffprobe`/`ffplay` 打开，VLC 播放；自写解析器检查 RIFF 结构、idx1 与 movi 的偏移一致性
4. **内存一致性**：用 `valgrind`/ASAN 跑录像路径，防内存问题放大为文件损坏

**实际遇到过的坑**（`debug-summary.md`）：
- **`dwChunkLength` 写错**：早期把"含 8 字节 chunk 头的长度"写进 idx1，VLC 播放时按错误偏移 seek，进度错乱。修复为"帧数据大小（不含 chunk 头）"。
- **movi size 算术错**：`moviEnd - m_moviDataOffset + 4` 的 +4 补 "movi" FOURCC，漏掉会导致 VLC 报"movi size 不匹配"。

**掉电损坏的定位**：录制中崩溃 → 头字段为 0/未回填 → 多数播放器打不开，但 **00dc 帧数据已顺序写盘** → 支持容错的播放器/ffmpeg 能扫描到 EOF 播放。**这是 AVI 自封装的固有弱点**（元数据在头、最后回填）。

### 【面试官追问】"如果要求掉电最多丢 1 秒数据，怎么改？"

**【理想应答】** 三件套：
1. **周期性 `fsync`/`fdatasync`**：每 1 秒或每 N 帧调一次 `fdatasync(fileno(fp))`（只刷数据不刷元数据，比 fsync 快），把页缓存强制落盘。代价：SD 卡 fsync 延迟 ~几 ms~几十 ms，每秒一次对 30fps 录制影响可控。
2. **分段录制**：每 5 分钟自动滚动一个新 AVI 文件（`startRecord`→`stopRecord` 循环），把"损坏窗口"从"整个录制"缩小到"最多 5 分钟"。
3. **崩溃恢复**：下次启动扫描 `movi` 区，若能定位到完整 00dc 帧，重建 idx1 + 回填头（把损坏文件"救回来"）。

**为什么当前没做 fsync？** 工程权衡：每帧 fsync 会杀掉 30fps；不 fsync 换来实时性，代价是掉电丢最近几秒。**"相机优先保实时"是产品定位**——面试要讲清这个取舍，而非只说"没做"。

### 【面试官追问】"检测到 SD 卡被拔出的流程是什么？"

**【理想应答】** 标准三通道：
1. **`statvfs` 失败**：`getFreeSpaceMB` 返回 -1（ENOENT/EIO）→ 判定设备不可用
2. **`fwrite`/`fflush` 出错**：写失败（返回错误、errno 为 EIO/ENOSPC）→ 判定写异常
3. **挂载状态轮询**：周期 `stat /media/sd` 或 `inotify` 监听挂载点

**当前代码只到第②层的日志**（`writeRecordFrame` 短写 LOG_ERR_），缺设备级检测与"拔卡自动停录、插卡自动恢复"。**改进方案**：录像线程周期检查 `statvfs` 失败次数，连续失败则自动 `stopRecord`（保数据）+ 通知 GUI；`inotify` 监听挂载事件恢复。

## 3.2 并发

### 【面试官追问】"如果同时录像和抓图，会相互阻塞吗？"

**【理想应答】** **当前设计基本不会**：
- **抓图**（`savePhoto`）：独立打开文件、一次性 fwrite、关闭——**不碰 `m_recordMtx`**，与录像并行
- **录像**（`writeRecordFrame`）：`m_recordMtx` 保护，只有录像自己用
- **唯一的锁竞争点**：`startRecord`/`stopRecord` 与 `writeRecordFrame` 抢 `m_recordMtx`（GUI 点停止时要等当前帧写完，~1ms，可接受）

**潜在冲突**：抓图与 autoCleanup 并行时——`autoCleanup` 可能删掉刚拍的图（如果按 mtime 最旧优先且空间紧张）。**改进**：抓图后立即刷相册列表，或 autoCleanup 跳过"最近 N 分钟"的文件。

## 3.3 性能瓶颈

### 【面试官追问】"你的最大稳定录制帧率是多少？瓶颈在哪？怎么排查？"

**【理想应答】** 设计目标 30fps，**但实测摄像头硬件实际输出只有 10fps**（`v4l2-ctl` 直测，能力列表声称 30fps）——**瓶颈在采集端（摄像头硬件），不在存储**。

**排查方法论**（`[PERF]` 插桩）：
1. 插桩显示拷贝 0.5MB/s（帧池优化后）、处理线程已跳过时帧率仍 10 → CPU 有空闲却提不上去
2. → **供给端（硬件）** 瓶颈，改应用代码无效
3. → `v4l2-ctl` 直测一锤定音：摄像头实际 10fps

**存储侧的证据**：`fwrite` 单帧耗时远小于帧间隔（~ms 级 vs 33ms），`fflush` 刷页缓存也是异步的——**存储 I/O 不是瓶颈**。若换 30fps 摄像头，才需评估存储侧（届时 `fwrite` + `fflush` 的 CPU 占比会上升）。

### 【面试官追问】"低速 SD 卡（Class 4）时怎么保证写入不中断？"

**【理想应答】** 当前**没有主动降码率/降帧率的机制**——写入变慢时：
- 处理线程 `fwrite` 阻塞变长 → 中间帧被覆盖丢弃（拉模式防堆积的副作用）→ **帧率下降但采集不断**
- `fwrite` short → LOG_ERR_（检测到但无恢复动作）

**产品化改进**：
1. **写入速度监控**：统计 `fwrite` 平均耗时，超过阈值（如 10ms/帧）→ 自动降帧率（跳过部分帧不写）或降 JPEG 质量（改 `quality` 从 80→70）
2. **空间预检**：`startRecord` 前检查 `getFreeSpaceMB`，不足拒绝开始
3. **背压通知**：处理线程写太慢时通知采集线程降速（虽然当前靠丢帧兜底，但显式降帧率更可控）

**面试点**："Class 4 卡也能录但会丢帧"是诚实答案；"主动降码率"是加分演进。

## 3.4 存储介质与文件系统

### 【面试官追问】"FAT32 单文件 4GB 限制怎么解决？为什么不用 exFAT？"

**【理想应答】** 
- **当前无此问题**：MJPEG 640x480 @10~30fps ~2MB/s，录满 4GB 需 ~30 分钟；但**长时间录像是可能超 4GB 的**（若支持 30fps 硬件 + 高码率）
- **分段录制解决**：录制中检测文件大小接近阈值（如 3.5GB）→ 自动 `stopRecord` 结束当前段 → `startRecord` 开新段。用户看到的是"连续分段"（文件名时间戳递增），播放时按段拼接
- **exFAT**：i.MX6ULL 的 U-Boot/内核需启用 `CONFIG_VFAT_FS` + exFAT 支持，且**很多旧播放器不认 exFAT**；FAT32 兼容性最广（相机/PC/电视都能读）。**选 FAT32 + 分段**是"兼容性优先"的工程决策

### 【面试官追问】"ext4 和 FAT32 对嵌入式相机有什么区别？"

**【理想应答】**
- **FAT32**：① 兼容性最好（拔卡插 PC/电视直接读）；② 无日志，掉电损坏需 chkdsk；③ 单文件 4GB 上限
- **ext4**：① 有 journal（掉电恢复更强）；② 支持大文件、权限；③ **PC/电视不认**（需装驱动），且 ext4 的 journal 在频繁掉电下也可能损坏
- **本项目**：用 `/data`（tmpfs，重启即失）与 eMMC/SD 的 FAT32 分区（产品化常选 FAT32 保证可移植性）

**面试点**："临时数据放 tmpfs（快但易失）、持久数据放 FAT32（可移植）"是嵌入式相机常见双区方案，README 有 storage 路径切换的说明。

## 3.5 文件格式

### 【面试官追问】"录像为什么选 AVI 而不是 MP4？"

**【理想应答】**

| 维度 | AVI（RIFF） | MP4 |
|------|-----------|-----|
| 结构复杂度 | 低（hdrl+movi+idx1） | 高（box 嵌套 + sample table） |
| 手写难度 | 可控（本项目 ~500 行） | 高（stbl/stts/stsz/stco 等） |
| seek | idx1 O(1) | stco/stss 也要索引表 |
| 掉电健壮性 | 头在文件头、最后回填 → 易损 | moov 可前置/后置（快速启动），但同样依赖 box 完整性 |
| 兼容性 | VLC/ffplay 认 | 更广泛 |
| 依赖 | 零依赖 | 无库手写成本大 |

**选择理由**：① MJPEG 是"单视频流、帧内编码"，AVI 的 idx1 索引天然适配；② **手写 AVI 能体现"容器格式"的工程深度**（本项目的 VideoPlayer 也自实现，闭环）；③ 不引入 ffmpeg/libmp4，保持二进制体积与交叉编译简单。

**若需求升级到 MP4/加密/多轨**：才值得上库（libmp4v2/ffmpeg），届时 `startRecord`/`writeRecordFrame`/`stopRecord` 接口不变（**接口与实现分离**的收益）。

---

# 第四部分 综合思辨（系统观）

## 4.1 模块解耦与可替换性

**现状**：`StorageManager` 直接操作本地文件系统，**不是可插拔后端**。

**改网络存储（FTP/云）的改动量评估**：
- **改动小**：`savePhoto`/`writeRecordFrame`/`getFreeSpaceMB` 替换实现（FTP 上传/HTTP PUT）
- **改动大**：`listPhotos`/`listVideos`/`deletePhoto`/`extractAviThumbnail` 等"目录遍历"方法要改成"远端索引查询"——网络存储没有本地目录语义
- **结论**：需引入 `IMediaProvider` 接口（`list/delete/extractThumbnail/save`），本地 FS 与 FTP 各一个实现，GUI 相册面向接口编程

```cpp
// 演进方向（面试可画）
class IMediaProvider {
public:
    virtual ~IMediaProvider() = default;
    virtual std::string save(const uint8_t* data, int len, MediaType type) = 0;
    virtual int list(std::vector<PhotoDayGroup>& out, MediaType type) = 0;
    virtual int remove(const std::string& path) = 0;
};
class LocalFsProvider : public IMediaProvider { /* 现有逻辑 */ };
class FtpProvider     : public IMediaProvider { /* FTP 实现 */ };
```

**【面试官追问】"抽象后热路径损失多少性能？"**

> 【理想应答】纯虚调用 = 一次 vtable 查表 + 间接跳转（~ns 级），对比 `fwrite` 的 µs~ms 级可忽略。**但 YAGNI 原则**：当前后端确定（本地 FS）且无变更预期，抽象是"未来成本"；面试主动指出"何时该抽象"（需求变更时）比现在硬抽象更显判断力。

## 4.2 与网络传输的资源竞争

**同帧多路消费**（处理线程，`main.cpp:1026-1055`）：

```cpp
// 同一份 localFrame 喂给三路
if (hasHttpViewer)  mjpegServer->updateFrame(...);   // HTTP 存副本
if (hasRtspViewer)  rtspServer->feedFrame(...);      // RTSP 存副本 + 分片
if (g_recording)    g_storage->writeRecordFrame(...); // 录像 fwrite
```

**竞争点**：
- **CPU**：都在处理线程**串行**执行（顺序 HTTP → RTSP → 录像），网络慢/磁盘慢互相拖累
- **内存**：HTTP/RTSP 各存一份 JPEG 副本（~0.1MB/份），录像走 fwrite 不占额外内存
- **无优先级**：先来后到，没区分"录像优先"或"推流优先"

**已有的省资源设计**：`hasHttpViewer`/`hasRtspViewer` 判断——**无人观看时跳过推流分发与深拷贝**（`main.cpp:987-993`），给 CPU 减负。

**【面试官追问】"如果要求'录像优先、网络可丢帧'，怎么改？"**

> 【理想应答】两个思路：① **处理线程内调整顺序**——先 `writeRecordFrame` 再推流，录像失败时跳过网络分发；② **分离消费**——录像单独线程 + 生产者-消费者队列（丢旧帧策略防堆积），网络保持现状。当前"无人观看跳过推流"已是"按需消费"雏形，优先级策略可在其上扩展。**面试点**：先讲清当前资源共享模型，再给优先级演进方案。

## 4.3 数据索引与回放

**查询接口**：`listPhotos`/`listVideos` 返回按日期分组的倒序列表。

**如何不卡 GUI**：
- **列表**：目录扫描 + sort，几十个文件 ms 级
- **缩略图**：`extractAviThumbnail` 只读 AVI 头 + 第一帧；`createThumbnail` 用 `scale_denom` 缩放解码（`gallery.cpp:283-286`）——**"只读必要的最小数据"**
- **播放**：VideoPlayer 靠 idx1 O(1) seek（`video_player.cpp` 的 `seekToFrame` → `fseek(moviOffset + dwChunkOffset)`）

**【面试官追问】"100 张照片 + 50 个视频，相册打开要多快？怎么优化？"**

> 【理想应答】当前"伪懒加载"：`listPhotos` 全量扫目录（快，ms 级），但 `createThumbnail` 若全量生成会慢（每张 ~15ms，100 张 ~1.5s）。**真懒加载**：只生成可见区域的缩略图（滚动到哪建哪），配合 QScrollArea 滚动事件按需触发。**内存**：每张缩略图 QPixmap ~82KB，可见 6 张 ~0.5MB，全量 100 张 ~8MB——必须按需。README"相册峰值内存 ~2.5MB"正是**只建可见部分**才能达到的数字。

## 4.4 设计模式评估

| 模式 | 现状 | 评估 |
|------|------|------|
| **生产者-消费者** | 采集线程（产）→ 处理线程（消）→ 存储（写盘） | ✅ 显式实现（`procCv` + `frameReady`），消费端是"取最新帧" |
| **观察者** | 无存储事件通知 | ❌ 存储异常（写失败/空间不足）无 GUI 告警；可加 `StorageListener` 回调（`onStorageError`/`onSpaceLow`） |
| **策略** | 无 | 可引入"存储策略"（循环覆盖/报警预录/手动保留），对应不同 `autoCleanup` 行为 |
| **模板方法** | `writeAviHeader`/`finalizeAvi` 固定流程 | 隐式模板方法 |
| **RAII** | 析构 `stopRecord` 兜底（`~StorageManager:37-42`） | ✅ 异常安全 |

**内存泄漏防护**：
- `m_frameIndexList`：`startRecord` 时 `clear`、`stopRecord` 后 `clear`，无泄漏
- `fclose`：所有路径保证（含 `writeAviHeader` 失败路径 `fclose + unlink`）
- **每帧无堆分配**：`fwrite` 直接写 FILE 缓冲，稳态内存稳定

## 4.5 面试「一句话总结」

> "存储模块用**标准 C 库 + AVI 自封装 + 无数据库**支撑 MJPEG 录像与 JPEG 拍照：文件按'日期目录 + 时间戳名'组织实现按时间检索；录像用**'写占位→记偏移→结束回填'**把元数据（分辨率/帧率/编码器）写进 AVI 头并生成 idx1 索引（O(1) seek）；空间管理按 mtime 升序删旧文件；写入被隔离在**处理线程**（不阻塞采集），靠 fflush 平衡实时与持久、诚实承认无 fsync 的掉电窗口。核心设计哲学是**'在 MJPEG 2MB/s 的码率量级下，用最简方案做对'**——stdio 足够就不上 O_DIRECT、目录扫描足够就不上 SQLite、AVI 足够就不上 MP4/ffmpeg；同时**每条选型都知道何时该升级**（海量文件上索引、掉电关键场景加 fsync、需求变更上 IMediaProvider 抽象）。面试要敢说'我的上限在哪、怎么演进'，这比堆功能更显工程判断力。"

## 5. 专题：#pragma pack(push, 1) 是什么？

> 用户问：`#pragma pack(push, 1)` 这是什么？——**这是本项目三处协议/容器结构体（AVI 文件格式、私有 TCP 协议、RTP/RTCP 网络头）的共同地基**，理解它才能理解"结构体直接映射为字节流"的写法。本节结合代码完整讲解。

### 5.1 一句话定义

`#pragma pack(push, 1)` 是**告诉编译器："从这里开始，结构体成员按 1 字节对齐，禁止自动插入填充字节"**；配套的 `#pragma pack(pop)` 是"恢复之前的对齐设置"。`push`/`pop` 是一对**栈式操作**：`push` 把当前对齐设置压栈保存，`pop` 弹出恢复——保证 pack 只影响中间这段代码，**不会污染后面无关的结构体**。

```cpp
#pragma pack(push, 1)   // 压栈保存旧对齐 + 设置为 1 字节对齐
struct RiffChunk { uint32_t fourcc; uint32_t size; };  // 布局 = 4 + 4 = 8 字节
#pragma pack(pop)        // 弹栈，恢复编译器默认对齐（如 4/8 字节）
```

### 5.2 为什么需要它？—— 编译器默认"对齐填充"会破坏结构体

C/C++ 编译器默认按**自然对齐（natural alignment）**分配成员：每个成员偏移 = `min(自身大小, 默认对齐) * 整数倍`，结构体大小也会对齐到最大成员对齐。看 `manager.h:72-92` 的 `AviStreamHeader`：

```cpp
struct AviStreamHeader {
    uint32_t fccType;    // 偏移 0，4 字节
    uint32_t fccHandler; // 偏移 4
    uint32_t dwFlags;    // 偏移 8
    uint16_t wPriority;  // 偏移 12，2 字节
    uint16_t wLanguage;  // 偏移 14
    uint32_t dwInitialFrames; // 默认对齐下偏移 16 —— 正好，因为 16 是 4 的倍数
    ...
};
```

**这个例子恰好天然对齐**。真正会出问题的是"大小不同的成员交错"：

```cpp
// 默认 4 字节对齐时的布局：
struct Bad {
    uint8_t  a;   // 偏移 0
    uint32_t b;   // 偏移 4 —— 偏移 1~3 是编译器插入的填充字节！
    uint8_t  c;   // 偏移 8
    uint32_t d;   // 偏移 12
};               // 总大小 16（含填充）

// #pragma pack(push, 1) 后：
struct Good {
    uint8_t  a;   // 偏移 0
    uint32_t b;   // 偏移 1 —— 紧挨着，无填充
    uint8_t  c;   // 偏移 5
    uint32_t d;   // 偏移 6
};               // 总大小 10（紧凑）
```

### 5.3 本项目三处使用场景（都是"结构体 ↔ 字节流"的边界）

#### 场景一：AVI 容器 —— 磁盘文件格式（`manager.h:45-124`）

```cpp
#pragma pack(push, 1)
struct RiffChunk { uint32_t fourcc; uint32_t size; };
struct AviMainHeader { /* avih，40 字节 */ };
struct AviStreamHeader { /* strh，56 字节，含 wPriority/wLanguage 两个 uint16_t */ };
struct BitmapInfoHeader { /* strf，40 字节 */ };
struct AviIndexEntry { uint32_t ckid; uint32_t dwFlags; uint32_t dwChunkOffset; uint32_t dwChunkLength; };
struct AviFrameChunk { uint32_t ckid; uint32_t size; };
#pragma pack(pop)
```

**为什么必须 pack(1)？** AVI 是**标准文件格式**，VLC/Windows 播放器按 RIFF 规范逐字节解析。若 `AviStreamHeader` 默认对齐，`wPriority`(2B) 和 `dwInitialFrames`(4B) 之间虽然不填，但**整个结构体尾部会按 4 对齐补零**，`sizeof` 变成 60 而不是规范要求的 56 → 写出的头比规范多 4 字节 → 播放器偏移错位 → 花屏/打不开。`pack(1)` 保证 `fwrite(&avih, sizeof(avih), 1, fp)` 写出的字节与 RIFF 规范**逐位一致**。

#### 场景二：私有 TCP 控制协议 —— 网络线协议（`control.h:121-193`）

```cpp
#pragma pack(push, 1)
struct ProtoHeader {
    uint8_t  magic[2];     // 0xEB 0x90
    uint8_t  version;      // 0x01
    uint8_t  cmd;          // 命令类型
    uint16_t payload_len;  // 负载长度（网络字节序）
};                        // 1+1+1+1+2 = 7 字节，天然紧凑
struct StatusPayload {
    uint8_t  streaming;    // 0=idle, 1=streaming
    uint8_t  recording;
    uint8_t  client_count;
    uint8_t  reserved;
    uint16_t width;        // 网络字节序
    uint16_t height;
    uint8_t  format;
    uint8_t  fps;
};                        // 4 + 2 + 2 + 1 + 1 = 10 字节
#pragma pack(pop)
static_assert(sizeof(StatusPayload) == 10, "StatusPayload must be 10 bytes");
```

这里 pack(1) 是**预防性**的：字段全是 `uint8_t`/`uint16_t` 交错，若默认对齐，`width` 会从偏移 4 挪到 6，插入 2 字节填充，结构体变 12 字节 → 客户端按 10 字节解析就错位。`static_assert(sizeof(...) == N)` 是**协议结构体的最佳实践**：编译器在编译期校验布局，任何对齐/增删字段导致尺寸变化都会立刻报错（`control.h:172/182/191` 三处都在用）。

#### 场景三：RTP/RTCP 网络头 —— RFC 标准格式（`rtsp_server.h:68-130`）

```cpp
#pragma pack(push, 1)
struct RTPHeader {          // RFC 3550 §5.1，必须恰好 12 字节
    uint8_t  cc        : 4; // 位域：版本/填充/扩展/CSRC 计数挤进第 1 个 32-bit 字
    uint8_t  extension : 1;
    uint8_t  padding   : 1;
    uint8_t  version   : 2;
    uint8_t  payload_type : 7;
    uint8_t  marker        : 1;
    uint16_t sequence;      // 网络字节序
    uint32_t timestamp;
    uint32_t ssrc;
};
#pragma pack(pop)
```

**位域 + pack(1) 的配合**：RTP 固定头要求"第 1 个字 = version(2b) + padding(1b) + extension(1b) + cc(4b)，第 2 个字 = marker(1b) + payload_type(7b)"。位域负责**按位打包**，pack(1) 保证这些位域字节**不因对齐散开**、整个头严格 12 字节，才能直接把结构体 memcpy 进 socket 发送缓冲。RFC 2435 的 `RTPJPEGHeader`、RFC 3550 §6.4 的 `RTCPHeader` 同理。

### 5.4 push/pop 为什么要成对出现？

`pack` 设置是**"粘性"的**——一旦 `#pragma pack(1)` 不带 `push`，会一直影响**整个文件后续所有结构体**，直到另一个 `#pragma pack()`。而本项目头文件里既有协议结构体又有业务类（如 `StorageManager`、`RTSPServer`），业务类**必须保持默认对齐**（否则成员函数指针、虚表、std::atomic 等内部布局可能错乱）。`push`/`pop` 的栈语义保证了：

```
对齐设置: [默认4] --push(1)--> [1] --pop--> [默认4]   // 中间夹着的都是协议结构体
```

### 5.5 三个必须知道的"坑"（面试加分点）

1. **pack(1) 可能产生非对齐访问**：`struct { uint8_t a; uint32_t b; }` 在 pack(1) 下 `b` 偏移为 1，访问时 ARM/x86 可能**性能下降或异常**。但本项目**只读不写**（解析收包/写文件头），且 iMX6ULL 的 ARMv7 支持非对齐访问，收益（协议正确）远大于代价。**通用原则：pack 用于"字节流映射"，业务数据留在默认对齐。**

2. **pack 不解决字节序（endianness）**：`control.h` 注释反复标注"网络字节序"，是因为 RTP/TCP 协议规定**大端**，而 iMX6ULL 是小端。pack 只解决"布局对齐"，**字节序要靠 `htons/htonl/ntohs/ntohl` 显式转换**——这是面试最容易追问的区分点。

3. **pack 是编译器扩展，不是 C++ 标准**：GCC/MSVC 都支持 `#pragma pack(push,1)`，但语义细节有差异；跨编译器时优先用 `static_assert(sizeof(...))` 兜底验证。此外 `#pragma pack(push,1)` 的 `1` 是**字节对齐粒度**，还可以是 2/4/8（例如某些平台用 pack(2) 兼容结构）。

【面试官追问】"`#pragma pack(push, 1)` 和 `#pragma pack(1)` 有什么区别？为什么本项目用前者？"

> 【理想应答】`#pragma pack(1)` 是直接设置对齐为 1，**无法恢复之前的值**，会污染后面所有代码；`#pragma pack(push, 1)` 先把旧设置压栈再设置，`pop` 时精确恢复。本项目头文件里协议结构体（pack(1)）和业务类（默认对齐）共存，必须用 push/pop 把影响范围**严格限定在结构体定义之间**，否则 `StorageManager` 等类的布局会被意外改变。另外 `push` 还支持 `#pragma pack(push, 2)` 这种"压栈 + 指定新值"的写法。

【面试官追问】"如果忘记 `#pragma pack`，`AviStreamHeader` 会出什么问题？怎么在编译期就发现？"

> 【理想应答】`AviStreamHeader` 规范是 56 字节：`fccType..dwFlags`(12) + `wPriority/wLanguage`(4) + `dwInitialFrames`(4) + `dwScale/dwRate/dwStart/dwLength`(16) + `dwSuggestedBufferSize/dwQuality/dwSampleSize`(12) + `rcFrame`(8) = 56，恰好 4 的倍数，**这个结构体天然对齐，忘 pack 也不会错**。真正会被打穿的是 `AviFrameChunk` 这类 `uint32_t ckid; uint32_t size;` 后紧跟变长 `uint8_t data[]` 的——若编译器把结构体按 4 对齐，`sizeof` 会是 8 而 `data` 偏移不变（还是 8，因为前面恰好对齐），但如果前面有 `uint16_t` 混排的结构体（如 `StatusPayload`），填充字节就会让**固定偏移协议字段错位**。所以项目在 `control.h` 用 `static_assert(sizeof(StatusPayload) == 10)` 做编译期校验——这是最可靠的防线，比肉眼数偏移强得多。

【面试官追问】"pack(1) 会影响性能吗？为什么嵌入式场景还大量使用？"

> 【理想应答】会：pack(1) 下 `uint32_t` 可能落在非 4 字节对齐地址，x86 上访问可能降速（部分 CPU 甚至 fault），ARM 上也可能多周期。但本项目 pack(1) 结构体**只用于协议编解码边界**（一次 memcpy 进/出缓冲区），不是热路径上的高频访问；而且 iMX6ULL (Cortex-A7) 支持非对齐访问，代价是微秒级的编解码耗时，对比磁盘 I/O 和网络传输完全可忽略。**工程判断**：pack(1) 的正确性收益（协议逐位一致）远大于可忽略的性能损失，这是嵌入式领域"结构体直映字节流"的通行做法；真正需要极致性能的热点结构体应保留默认对齐并用 `memcpy` 显式做字节序/对齐转换。

---

## 6. 专题：ftell 与回填（backfill）是什么？

> 用户问：文档里 `ftell` 是什么？为什么要回填？——**这两问是手写 AVI 容器"边写边回填"策略的核心**：`ftell` 是"记账工具"，回填是"事后补数"，二者配套出现（2.4/2.5/2.6 节的 `writeRecordFrame`/`finalizeAvi`/`writeAviHeader` 都用到了）。

### 6.1 ftell 是什么？

`ftell` 是 **C 标准库 `<stdio.h>` 里的文件定位函数**，原型：

```c
long ftell(FILE *stream);
```

**作用**：返回 `stream` 指向的文件流**当前读写位置**——即从文件开头到当前指针的**字节偏移量**。成功返回非负偏移，失败返回 `-1L` 并置 `errno`。

它和 `fseek` 是一对**"定位 ↔ 记录"**的组合：`fseek(fp, offset, SEEK_SET)` 把指针移到指定位置，`ftell(fp)` 把当前位置读出来。

**本项目三处用途**（都是"记偏移"）：

| 位置 | 代码 | 记什么偏移 |
|------|------|-----------|
| `writeRecordFrame`（`manager.cpp:181-225`） | `long currentPos = ftell(m_recordFile);` | 每帧 `00dc` 数据块的起始位置 → 存进 `m_frameIndexList` → 供 `finalizeAvi` 写 idx1 |
| `finalizeAvi`（`manager.cpp:391-446`） | `long moviEnd = ftell(fp);` / `long riffEnd = ftell(fp);` | movi 区结束位置 → 算 movi size；写完 idx1 后的文件总长 → 算 RIFF size |
| `writeAviHeader`（`manager.cpp:267-389`） | `m_avihFramesOffset = ftell(fp) - ...` | avih/strh 中 `dwTotalFrames`、`dwLength` 字段的绝对偏移 → 供结束时回填 |

**一句话**：`ftell` = "当前文件指针离文件头多远"。它是回填的"记账本"——没有它，结束时就不知道要 `fseek` 回哪里去补数。

### 6.2 回填是什么？

**背景**：AVI 文件头里有一批"只有录完才知道"的数字：

```
RIFF size="??" "AVI "
  LIST size="??" "hdrl"          ← 元数据头
    "avih"  { ..., dwTotalFrames=? , ... }   ← 总帧数
    LIST size="??" "strl"
      "strh"  { ..., dwLength=?, ... }       ← 流长度（帧数）
  LIST size="??" "movi"          ← 真正的帧数据，逐帧追加
    "00dc" (len) JPEG帧
    "00dc" (len) JPEG帧
    ...
  "idx1" { 每帧的偏移+长度 ... } ← 索引
```

关键矛盾：**`dwTotalFrames`（总帧数）、`movi` 区大小、RIFF 总大小，这些值只有在录制结束时才知道**——开始录时根本不知道用户会录几帧、录多久。

**回填（backfill）定义**：

> 先把这些"未知数"写一个**占位值（0）**放在文件头里，同时用 `ftell` 记住这些占位符在文件里的**绝对位置**（记账）；等录制结束、所有数据都写完、数字终于确定时，再用 `fseek` 跳回那些位置，把真实数值**覆盖写进去**。

```
录制中:  [ dwTotalFrames = 0 ]  [ movi size = 0 ]  [ 帧数据... ]
                ↑ ftell 记下这三个位置
结束时:   fseek 跳回 → 覆写成真实值
          [ dwTotalFrames = 123 ]  [ movi size = 4567 ]  [ 帧数据... ]  [ idx1 ]
```

### 6.3 为什么要回填？（为什么不能最后一次性写头）

因为录制是**流式的**：必须先写头、再边录边往 `movi` 区追加帧。所以只有两个方案：

| 方案 | 做法 | 后果 |
|------|------|------|
| **A（本项目）** | 先写占位头 → 记偏移 → 录完回填 | 录制中随时崩溃，文件里已有全部帧数据，有经验的播放器能容错播放 |
| B | 录完再一次性写整个头 | 录制中一旦崩溃/掉电，**连头都没有，文件完全不可读** |

文档 2.5 节原话就是这个意思：**方案 A 的收益是"流式写 + 文件头信息完整 + VLC 兼容"，代价是掉电时头字段未回填（值为 0），但帧数据已顺序写盘，可容错播放。**

### 6.4 项目代码里的完整链路（记账 → 边录边写 → 回填）

**记账**（`writeAviHeader`，写头时用 `ftell` + `offsetof` 精确算出各字段绝对偏移并保存）：

```cpp
m_avihFramesOffset = ftell(fp) - sizeof(AviMainHeader)
                     + offsetof(AviMainHeader, dwTotalFrames);
m_strhLengthOffset = ftell(fp) - sizeof(AviStreamHeader)
                     + offsetof(AviStreamHeader, dwLength);
m_rifSizeOffset    = ...;   // RIFF size 字段位置
m_moviDataOffset   = ...;   // 第一帧数据起始位置
```

**边录边记**（`writeRecordFrame`，每帧写前 `ftell` 记下起始偏移）：

```cpp
long currentPos = ftell(m_recordFile);   // 这一帧 00dc 块的起始位置
long frameDataOffset = currentPos;
...  // 写 "00dc" + len + JPEG 数据
FrameIndex idx;
idx.offset = static_cast<uint32_t>(frameDataOffset - m_moviDataOffset);
m_frameIndexList.push_back(idx);         // 内存索引，供 finalizeAvi 回填 idx1
```

**回填**（`finalizeAvi`，结束时 `fseek` 跳回占位符位置覆写真实值）：

```cpp
// ① 回填 movi LIST size
long moviEnd = ftell(fp);
long moviSize = moviEnd - m_moviDataOffset + 4;
fseek(fp, m_moviDataOffset - 8, SEEK_SET);   // 跳回占位符位置
writeU32(fp, moviSize);                      // ← 覆盖回填

// ② 回填 avih.dwTotalFrames 和 strh.dwLength
fseek(fp, m_avihFramesOffset, SEEK_SET);
writeU32(fp, m_frameIndexList.size());       // ← 覆盖成真实帧数

// ④ 回填 RIFF 总大小
fseek(fp, m_rifSizeOffset, SEEK_SET);
writeU32(fp, riffSize);                      // ← 覆盖
```

### 6.5 一句话总结

> **`ftell` = "当前文件指针离文件头多远"**，它是回填的记账工具；**回填 = 把录制开始时"留的坑"（值为 0 的占位字段）在结束时用真实数据填上**。二者配套出现：`ftell` 记账 → 边录边写数据 → `fseek` 跳回 → 覆写占位符（回填）。之所以必须这样，是因为 AVI 头的总帧数/大小只有录完才知道，而录制又是流式边写边录的——用"先占位、记偏移、最后回填"换来"随时崩溃文件都不至于完全废掉"的健壮性。

【面试官追问】"`ftell` 返回的类型是什么？文件超过 2GB 会怎样？"

> 【理想应答】`ftell` 返回 `long`，在 32 位系统（iMX6ULL 是 32 位 ARM）上 `long` 只有 4 字节，文件超过 2GB 会溢出。本项目 MJPEG ~2MB/s，录满 2GB 需 ~17 分钟，若支持 30fps 硬件 + 高码率长时间录制是有可能超的（配合 FAT32 单文件 4GB 上限，见 3.4）。**演进方案**：换 `fseeko`/`ftello`（`off_t` 64 位），或直接 `lseek`/`lstat` 用 64 位偏移。当前用 `long` 是"量级够用"的取舍。

---

## 7. 专题：录像文件（AVI）的完整数据布局

> 用户问：给出录像文件的数据布局和详细解释。——这是 AVI 自封装的全貌，与专题 6（ftell/回填）、专题 8（关键偏移）配套。依据 `manager.h:45-124` 的结构体定义与 `manager.cpp` 的 `writeAviHeader`/`writeRecordFrame`/`finalizeAvi` 实现。

### 7.1 整体结构一览

本项目生成的 AVI 是**标准 RIFF 容器**，共 4 层：

```
RIFF 根块
├── hdrl LIST        ← 元数据头（avih + strl）
│   ├── avih         AVI 主文件头（帧率/分辨率/总帧数…）
│   └── strl LIST
│       ├── strh     视频流头（编码器 MJPG、帧率、帧数…）
│       └── strf     位图信息（宽/高/位深/压缩格式）
├── movi LIST        ← 帧数据区（每帧一个 "00dc" 块）
│   └── "00dc" × N
└── idx1             ← 索引块（每帧的偏移+长度，O(1) seek 用）
```

### 7.2 逐字节布局图

以 640×480 录制 N 帧为例，**右侧标注了每个字段由谁写入、哪些是"占位→回填"**：

```
偏移      长度   内容                                 说明 / 回填点
─────────────────────────────────────────────────────────────────────────────
0         4     "RIFF"                              RIFF 块标识
4         4     riff size                           = 文件总长 - 8   ← ①回填 (m_rifSizeOffset)
8         4     "AVI "                              RIFF 形式类型（AVI 固定）
─────────────────────────── hdrl LIST ─────────────────────────────
12        4     "LIST"                              LIST 块标识
16        4     hdrl size                           = hdrl 内容长度   ← ②写头时当场回填
20        4     "hdrl"                              块类型
24        4     "avih"                              块标识
28        4     40                                  avih 数据大小
32        40    AviMainHeader{...}                  ↓ 见 7.3-2
                      dwTotalFrames                 avih 内偏移 16 → 文件偏移 48 ← ③回填 (m_avihFramesOffset)
72        4     "LIST"
76        4     strl size                            = strl 内容长度   ← ④写头时当场回填
80        4     "strl"
84        4     "strh"
88        4     56                                  strh 数据大小
92        56    AviStreamHeader{...}                ↓ 见 7.3-3
                      dwLength                      strh 内偏移 32 → 文件偏移 124 ← ⑤回填 (m_strhLengthOffset)
148       4     "strf"
152       4     40                                  strf 数据大小
156       40    BitmapInfoHeader{...}               ↓ 见 7.3-4
─────────────────────────── movi LIST ─────────────────────────────
196       4     "LIST"                              LIST 块标识
200       4     movi size                           = "movi" + 全部帧块长度 ← ⑥回填 (m_moviDataOffset-8)
204       4     "movi"                              块类型
208       4     "00dc"                              ← m_moviDataOffset 指向这里（第1帧）
212       4     len(帧1)                            第1帧 JPEG 字节数
216       len1  JPEG 帧1 数据
216+len1  0/1   [奇数时补 1 字节 0x00]              RIFF WORD 对齐
         ...    "00dc" + len + JPEG 帧2 ...
         ...    "00dc" + len + JPEG 帧3 ...
─────────────────────────── idx1 ───────────────────────────────────
末尾      4     "idx1"                              索引块标识
末尾+4   4     idx1 size                            = N × 16（每条目 16 字节）
末尾+8   16×N  AviIndexEntry × N                    ↓ 见 7.3-6
```

### 7.3 各结构体字段详解

**① RIFF 根块（偏移 0-11）**
- `"RIFF"`（4B）：RIFF 容器标志。
- `riff size`（4B）：**= 文件总大小 − 8**（减去 `"RIFF"` + `size` 自身 8 字节），播放器靠它知道文件边界。
- `"AVI "`（4B）：形式类型（注意最后有一个空格，是格式要求）。

**② avih — AVI 主文件头（偏移 32，40 字节）**

| 字段 | 值（本项目） | 含义 |
|------|------------|------|
| `dwMicroSecPerFrame` | 1000000/fps | 每帧间隔微秒（30fps → 33333） |
| `dwMaxBytesPerSec` | 0 | 最大码率（未知留 0） |
| `dwPaddingGranularity` | 0 | 对齐粒度 |
| `dwFlags` | 0x10 | **0x10 = 文件含 idx1 索引** |
| `dwTotalFrames` | 0 → **N** | **总帧数（结束回填）** |
| `dwStreams` | 1 | 只有 1 条视频流 |
| `dwSuggestedBufferSize` | w×h×3 | 建议缓冲 |
| `dwWidth` / `dwHeight` | 640 / 480 | 视频分辨率 |

**③ strh — 视频流头（偏移 92，56 字节）**

| 字段 | 值（本项目） | 含义 |
|------|------------|------|
| `fccType` | `"vids"` | 流类型 = 视频流 |
| `fccHandler` | `"MJPG"` | 编码器 = MJPEG |
| `dwScale` / `dwRate` | 1 / fps | 帧率 = dwRate/dwScale |
| `dwLength` | 0 → **N** | **总帧数（结束回填）** |
| `dwSampleSize` | 0 | 0 = 帧长可变（MJPEG 每帧 JPEG 大小不一） |
| `rcFrame` | (0,0,w,h) | 目标帧矩形 |

**④ strf — BITMAPINFOHEADER（偏移 156，40 字节）**
`biWidth`/`biHeight` = 640/480；`biBitCount` = 24；`biCompression` = `"MJPG"`；`biSizeImage` = 0（变长帧）。播放器靠它知道画面尺寸和压缩格式。

**⑤ movi LIST — 帧数据区（偏移 196 起）**

每一帧是一个 `"00dc"` 块，**8 字节块头 + 变长 JPEG**：

```
[ "00dc"(4B) | len(4B) | JPEG 数据 len 字节 | 奇数帧补 1B 0x00 ]
└── chunk 头 ──┘  └──────── 帧数据 ────────┘
```

- **块头**：`"00dc"`（视频帧标识）+ `len`（小端，JPEG 字节数）。
- **WORD 对齐**：RIFF 规范要求每个 chunk 数据区从偶数偏移开始，奇数长帧补 1 字节 0x00，否则后续帧解析错位（VLC 花屏/进度错乱）。
- **`m_moviDataOffset`** = 第一个 `"00dc"` 的偏移（208），是 idx1 的偏移基准（详见专题 8）。

**⑥ idx1 — 索引块（文件末尾）**

每条目 16 字节（`AviIndexEntry`，对应 `finalizeAvi` 里 `m_frameIndexList` 的每一项）：

| 字段 | 值 | 含义 |
|------|-----|------|
| `ckid` | `"00dc"` | 指向的块类型 |
| `dwFlags` | 0x10 | 关键帧 |
| `dwChunkOffset` | 相对 movi 数据区的偏移 | `= frameDataOffset − m_moviDataOffset` |
| `dwChunkLength` | 帧数据字节数 | **不含 8 字节块头**（此处曾踩坑，见 `debug-summary.md`） |

有了 idx1，播放器可 **O(1) 跳帧**（`video_player.cpp` 的 seek 就是 `fseek(movi 数据区绝对起点 + dwChunkOffset)`）。

### 7.4 六个回填点汇总

| 标记 | 字段 | 位置依据 | 填什么值 |
|------|------|---------|---------|
| ① | RIFF size | `m_rifSizeOffset`（偏移 4） | `riffEnd − 8` |
| ② | hdrl size | `m_hdrlListOffset`（偏移 16） | 写头时当场算好 |
| ③ | avih.dwTotalFrames | `m_avihFramesOffset`（偏移 48） | 帧数 N |
| ④ | strl size | 局部 `strlSizePos`（偏移 76） | 写头时当场算好 |
| ⑤ | strh.dwLength | `m_strhLengthOffset`（偏移 124） | 帧数 N |
| ⑥ | movi size | `m_moviDataOffset − 8`（偏移 200） | `moviEnd − m_moviDataOffset + 4` |

②④ 在 `writeAviHeader` 里**当场回填**（内容已确定）；①③⑤⑥ 在 `finalizeAvi` 里**结束时回填**（取决于总帧数）——"写占位 → `ftell` 记偏移 → `fseek` 回填"的完整落地。

---

## 8. 专题：m_moviDataOffset / m_avihFramesOffset / movi size / RIFF size 是什么？

> 用户问：`m_moviDataOffset` 和 `m_avihFramesOffset` 是什么？movi size 和 RIFF size 又是什么？——这四个概念都出自 AVI 的"边写边回填"策略，**前两个是"位置"（文件偏移量，记录坑在哪），后两个是"数值"（长度，结束时要填进去的值）**。

### 8.1 一句话区分

- **`m_moviDataOffset` / `m_avihFramesOffset`**：`long` 型成员变量，存的是**文件字节偏移**（`ftell` 记下来的"坑位/基准"）。
- **movi size / RIFF size**：**长度数值**（不是成员变量，是 `finalizeAvi` 里算出来的局部量），结束时回填进占位符。

### 8.2 m_moviDataOffset —— 第一个 `00dc` 帧块的绝对文件偏移

```cpp
// manager.cpp:212-213，写第一帧时记录
if (m_recordFrameCount == 0) {
    m_moviDataOffset = frameDataOffset;   // frameDataOffset 就是 ftell() 的结果
}
```

它是**基准点**，有两个用途：
- **算 idx1 索引里的每帧偏移**（相对 movi 数据区起点）：`idx.offset = frameDataOffset - m_moviDataOffset`（第 1 帧 = 0）。
- **定位 movi LIST 的 size 坑位**：布局是 `[LIST(4)][size(4)][movi(4)][00dc...]`，size 字段在 `m_moviDataOffset - 8` 处。

### 8.3 m_avihFramesOffset —— avih 里 `dwTotalFrames` 字段的绝对偏移

```cpp
// manager.cpp:305-306，写完 avih 后立即记账
m_avihFramesOffset = ftell(fp) - sizeof(AviMainHeader)
                     + offsetof(AviMainHeader, dwTotalFrames);
```

写头时 `ftell` 已越过 avih 结尾，`ftell - sizeof(AviMainHeader)` 反推出 avih 数据起始，再用 `offsetof` 定位到 `dwTotalFrames` 字段（avih 内偏移 16）。它就是"总帧数占位 0"的坑位，结束时 `fseek` 跳过去回填真实帧数。`m_strhLengthOffset` 同理（strh 里的 `dwLength`）。

### 8.4 movi size —— movi LIST 内容的字节数

movi size 是**坑位 `m_moviDataOffset - 8` 里要回填的数值**，含义是：**从 "movi" FOURCC 之后开始、到最后一个 `00dc` 帧块结束为止的总长度**（即"movi 区装了多少数据"）。

```cpp
// manager.cpp:401-405
long moviEnd = ftell(fp);                        // 所有帧写完后的当前位置
long moviSize = moviEnd - m_moviDataOffset + 4;  // +4 补上 "movi" 自己的4字节
fseek(fp, m_moviDataOffset - 8, SEEK_SET);
writeU32(fp, moviSize);                          // 回填
```

为什么 `+4`？RIFF 规范规定 LIST 的 size 值 = **LIST 内容**（含 "movi" 四个字符，不含 "LIST"+"size" 这 8 字节头）的大小。`moviEnd - m_moviDataOffset` 只是所有 `00dc` 块的长度，漏了 "movi" 本身，所以要补 4。

### 8.5 RIFF size —— 整个文件内容的字节数

RIFF size 是**坑位 `m_rifSizeOffset`（偏移 4）里要回填的数值**，含义是：**从 "AVI " 开始到文件末尾的总长度**，即"这个 RIFF 文件装了多少数据"。

```cpp
// manager.cpp:434-438
long riffEnd = ftell(fp);      // 写完 idx1 后文件末尾
long riffSize = riffEnd - 8;   // 减去 "RIFF"(4) + size(4) 这 8 字节头
fseek(fp, m_rifSizeOffset, SEEK_SET);
writeU32(fp, riffSize);        // 回填
```

播放器/解析器靠这个值知道"文件到哪结束"，写错或不写会导致解析错位。

### 8.6 归纳表

| 概念 | 类型 | 是什么 | 何时知道 |
|------|------|--------|---------|
| `m_moviDataOffset` | 位置 | 第一个 `00dc` 的绝对文件偏移（基准点） | 写第一帧时（`ftell`） |
| `m_avihFramesOffset` | 位置 | `dwTotalFrames` 字段的绝对偏移（坑位） | 写头时（`ftell`+`offsetof`） |
| movi size | 数值 | movi 内容总字节数 | 所有帧写完时（`moviEnd - m_moviDataOffset + 4`） |
| RIFF size | 数值 | 整个文件内容总字节数 | 写完 idx1 时（`riffEnd - 8`） |

位置型的是"**记账**"（`ftell` 记下坑位），数值型的是"**回填**"（`fseek` 跳回坑位覆写）——合起来就是完整的"先占位、记偏移、最后回填"策略。

【面试官追问】"idx1 的 `dwChunkOffset` 为什么是相对 movi 数据区而不是文件开头？"

> 【理想应答】RIFF 规范规定 idx1 的 `dwChunkOffset` 是相对 movi LIST 内容起始（即 "movi" FOURCC 之后）的偏移，不是文件绝对偏移。本项目 `m_moviDataOffset` 指向第一个 `"00dc"`（恰好是 "movi" 后的第一字节），所以 `frameDataOffset - m_moviDataOffset` 就是规范要求的相对偏移（第 1 帧 = 0）。好处：即使 movi 在文件里的绝对位置变化（如头扩展），idx1 索引依然有效；播放器只需在跳转时把相对偏移换算回文件绝对偏移（`video_player.cpp` 的 seek：`movi 数据区绝对起点 + dwChunkOffset`）。

---

## 9. 专题：POSIX 是什么？

> 用户问：POSIX 是什么？——这是贯穿全项目的"类 UNIX API 契约"。理解它才能说清存储模块 `statvfs`、网络模块 socket、采集模块 `mmap` 等接口的**标准属性**，以及哪些接口是 **Linux 扩展**（面试高频区分点）。

### 9.1 定义

**POSIX**（Portable Operating System Interface，可移植操作系统接口）是 **IEEE 制定的一组标准**（IEEE Std 1003），定义了类 UNIX 操作系统应该提供的**编程接口（API）**和工具规范。目标是让源码可移植——"**写一次，随处编译**"。

它**不是软件，而是一份规范文档**；glibc、musl 等库是它在 Linux 上的**实现**。

### 9.2 它规定了什么

| 范畴 | 内容 |
|------|------|
| 系统接口 | 系统调用封装：进程、线程（pthread）、信号、文件 I/O、目录操作、socket 网络 |
| C 库函数 | 在 ISO C 标准之上扩展：`statvfs`、`fork`、`mmap`、`pthread_create`… |
| Shell 与工具 | `/bin/sh` 语法、`grep`、`sed`、`ls` 等命令行工具行为 |
| 环境约定 | 路径（`/tmp`、`/dev`）、环境变量、退出码等 |

### 9.3 三个关键区分（面试常考）

1. **ISO C vs POSIX**：ISO C 是纯语言标准（`fopen/fprintf` 那套）；POSIX 是**操作系统接口**（`open/read/write`、pthread、socket），是 ISO C 的超集。本项目存储模块用 `fopen/fwrite/fseek/ftell`（ISO C），但 `statvfs`（2.7 节查剩余空间）、`unlink`、`mkdir`、`opendir` 都是 **POSIX 接口**。

2. **POSIX vs Linux**：POSIX 是**标准**（可以写在纸上），Linux 是**具体操作系统**。Linux 绝大部分兼容 POSIX，但反过来：**epoll、O_DIRECT、ioctl** 等都是 Linux 专有扩展，**不在 POSIX 里**。本项目网络模块 `socket/bind/accept` 是 POSIX，`epoll`（控制线程用的边缘触发）就是 Linux 特有。

3. **POSIX vs glibc**：glibc 是 GNU 对 ISO C + POSIX 的具体实现库；嵌入式交叉编译常提到的 `arm-linux-gnueabihf` 里的 "gnueabihf" 就是指 glibc + 硬浮点 ABI。

### 9.4 在本项目里的体现

| 位置 | 接口 | 标准归属 |
|------|------|---------|
| `manager.cpp` `getFreeSpaceMB` | `statvfs` | POSIX |
| 存储/相册 | `unlink` / `mkdir` / `opendir` / `readdir` | POSIX |
| 采集线程 | `mmap`（V4L2 零拷贝） | POSIX |
| 6 线程架构 | `std::thread` → `pthread_create` | POSIX |
| HTTP/RTSP/TCP | `socket` / `bind` / `accept` / `send` | POSIX |
| 控制线程 | `epoll`（边缘触发） | **Linux 扩展**，非 POSIX |
| 摄像头 | `ioctl`（V4L2 驱动框架） | **Linux 扩展** |
| 性能插桩（main.cpp） | `/proc/self/stat` 读 CPU | **Linux procfs**，非 POSIX |
| 专题 6 提到的 2GB 演进 | `fseeko` / `ftello` | POSIX（`off_t` 64 位） |

### 9.5 一句话总结

> **POSIX = "类 UNIX 系统的通用 API 契约"**。它保证"代码在 A 系统上能编译，在 B 系统上换个编译器也能编译运行"。面试时能说清"**哪些接口是标准、哪些是 Linux 扩展**"（如 epoll 不是 POSIX），比背定义更能体现功底。

【面试官追问】"你怎么判断一个接口是不是 POSIX 标准？项目里哪些不是？"

> 【理想应答】判断方法：查 IEEE 1003.1 规范（或 glibc 文档中标注 `POSIX` 字样的函数说明）。本项目三个明显非 POSIX 的：① `epoll`（控制线程边缘触发，Linux 2.6+ 特有，BSD/macOS 用 kqueue）；② V4L2 的 `ioctl`（摄像头驱动框架，Linux 媒体子系统专属）；③ `/proc/self/stat`（`main.cpp` 性能插桩读 CPU jiffies）是 Linux procfs 专有。能脱口而出"标准内/标准外"的边界，是嵌入式 Linux 开发的基本功。

---

## 10. 专题：readJpegSize 为什么要 for 循环扫描，不能直接跳到固定偏移？

> 用户问：`readJpegSize`（`manager.cpp:586-624`）"只读头，不解码"，为什么读宽高还要 for 循环，不能直接跳到对应位置读？——核心答案：**JPEG 的宽高字段没有固定偏移**，它只存在于 SOF 段里，而 SOF 前面的可选标记段都是变长的，所以必须"顺序扫描 + 按段长度跳过"，for 循环干的就是这件事。

### 10.1 JPEG 文件结构：为什么 SOF 位置不固定

JPEG 由一串"标记段"（marker segment）组成，基本骨架：

```
SOI (FF D8)                     ← 文件头
  APP0 (FF E0)  JFIF 头（可选）   ← 变长
  APP1 (FF E1)  EXIF 信息（可选） ← 变长，可能很大
  COM  (FF FE)  注释（可选）       ← 变长
  DQT  (FF DB)  量化表（可选）     ← 变长，且彩色图 3 张、灰度 1 张
  ...
SOF0 (FF C0)   ← 宽高在这里！
  DHT / SOS / 图像数据 ...
```

**关键点**：
- 每个标记段结构：`FF + marker(1B) + 段长度(2B, 大端) + 段数据`
- 段与段之间**可省略、可交换顺序、长度任意**——APP0（JFIF）、APP1（EXIF）、DQT 等都不是必须的
- 所以 **SOF 之前有多少字节完全不确定**：可能是几百字节（只有 JFIF），也可能几 KB（带 EXIF + 大缩略图）

结论：**宽高没有"固定偏移"可跳**，必须从 SOI 开始逐段扫描，遇到变长段就按它的长度字段跳过，直到撞上 SOF。

### 10.2 类比

就像一本书想查"总页数"，但**版权页不一定在第 2 页**——前面可能夹了序言、再版说明、目录，页数都不固定。只能一页页翻，每页看看是不是版权页（SOF），不是就按页码跳（按段长度跳）。

### 10.3 for 循环到底在干什么

```cpp
for (size_t i = 2; i < n - 8; i++) {
    if (buf[i] == 0xFF) {
        uint8_t marker = buf[i + 1];
        if (marker == 0xFF) continue;                    // ① 填充字节
        if (marker >= 0xD0 && marker <= 0xD7) continue;  // ② RST 无数据标记
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            h = (buf[i+5] << 8) | buf[i+6];              // ③ 找到 SOF，读宽高
            w = (buf[i+7] << 8) | buf[i+8];
            return (w > 0 && h > 0);
        }
        uint16_t segLen = (buf[i+2] << 8) | buf[i+3];
        if (segLen >= 2) i += segLen + 1;                // ④ 变长段：按长度"跳"过
    }
}
```

它不是傻乎乎逐字节找——**④ 处遇到变长段会直接 `i += segLen + 1` 跳过整个段**（`segLen+1` 是因为段长度字段从 marker 后算起，i 当前在 FF 处）。所以这个循环本质是"**扫描 + 跳变长段**"，代价与"SOF 前的总字节数"成正比，通常远小于整个文件。

SOF 段布局（`i` 指向 `FF C0`）：
```
FF C0 | segLen(2B) | precision(1B) | height(2B) | width(2B) | ...
0  1    2        3     4             5  6         7  8
```
所以 `h = buf[i+5..i+6]`、`w = buf[i+7..i+8]`。

### 10.4 为什么只读前 4KB 就够了

SOF 在几乎所有实际 JPEG 里都出现在**前几百字节**（APP0/APP1/DQT 加起来通常 <1KB），读 4KB 已覆盖绝大多数情况。这样既**不解码整张图**（不解压像素、不建解压状态），又能拿到宽高——这就是"读元数据的最小代价"。

### 10.5 有没有"直接定位"的替代方案？

有，但都不如扫描：
- **用 libjpeg `jpeg_read_header()`**：要初始化解压对象、读更多数据，比自扫描重，且项目想保持"零依赖轻量读取"
- **写入时在固定位置记录**：比如在 EXIF 里存宽高，但 EXIF 段本身也是变长定位，绕回原点
- JPEG 规范**没有**为"快速读宽高"提供固定偏移字段——所以扫描是行业标准做法，ffmpeg、各种图片库解析宽高也都是这么干的

### 10.6 一句话总结

> **for 循环 = "顺着标记段链表往前走，变长段按长度跳，专找 SOF 读宽高"**。因为 JPEG 把"宽高"放在一个位置不固定的段里，只能顺序扫描，无法"直接跳到"。

【面试官追问】"如果 JPEG 前面夹了很大的 EXIF（>4KB），SOF 超出缓冲区，这个函数会怎样？"

> 【理想应答】会返回 `false`（循环在 `n` 内找不到 SOF，走到底），相册侧 `createThumbnail` 解码失败显示占位符，不崩溃——功能降级而非崩溃是设计底线。**改进方案**：读满 4KB 仍未找到时继续扩读（按需翻倍到 16KB/64KB，`fseek`+`fread` 分块），或先读 4KB 未命中再 `fseek` 到段末尾继续；更彻底的是直接上 libjpeg 的 `jpeg_read_header`。当前固定 4KB 是"99% 场景覆盖 + 一次小读"的取舍，面试要诚实指出这个上限和扩读路径。

---

*本文档基于 SmartCam-Linux-on-imx6ull 项目源码，聚焦 `src/storage/`。配合阅读：`docs/learn/04-storage-module-implementation.md`（模块实现）、`docs/debug-summary.md`（AVI 容器排障）、`docs/interview/面试复习-display模块.md`（相册/VideoPlayer 上游）、`docs/interview/面试复习-camera模块.md`（采集/处理线程下游）、`docs/interview/面试复习-main模块.md`（线程编排与回调）。*
