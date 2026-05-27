# SmartCam 相册模块 — 实现文档

> 版本: v1.0 | 日期: 2026-05-24 | 作者: SmartCam Team

---

## 一、概述

相册模块为用户提供已拍摄照片的浏览、查看和管理功能，直接运行在 7 寸触摸屏上（800×480）。利用现有的照片存储目录结构（`/data/photos/YYYYMMDD/IMG_*.jpg`），自动索引所有 JPEG 文件并按日期分组展示。

### 功能清单

| 功能 | 描述 |
|------|------|
| 照片浏览 | 3 列缩略图网格，按日期分组，自动滚动 |
| **视频缩略图** | v0.4：从 AVI 提取第一帧 JPEG 作为封面缩略图，与照片缩略图视觉一致 |
| **视频播放** | v0.4：全屏模式下自动加载 AVI，libjpeg-turbo 逐帧解码播放，QTimer 帧率控制 |
| 全屏查看 | 点击缩略图进入，等比缩放适配屏幕；照片 / 视频自动切换 |
| 前后翻页 | 左右箭头按钮 + 触摸滑动翻页（阈值 60px）；翻页自动停止当前视频 |
| 媒体信息 | 显示文件名、分辨率（照片）/ 文件大小、当前页码 |
| 删除媒体 | 确认弹窗后删除（照片/视频分别处理），自动清理空目录 |
| 返回 | 全屏→网格→实时预览，三级导航 |
| 空相册提示 | 无媒体时显示 "No photos yet — Tap Capture to take one!" |
| 自动刷新 | 删除后自动刷新列表，回到正确位置 |

---

## 二、文件变更清单

| 文件 | 操作 | 行数变化 | 说明 |
|------|------|----------|------|
| `include/display/gallery.h` | **新增** | +90 行 | `PhotoGallery` 类声明 |
| `src/display/gallery.cpp` | **新增** | +430 行 | `PhotoGallery` 完整实现（含视频支持） |
| `include/storage/manager.h` | **修改** | +80 行 | `PhotoInfo` 新增 `isVideo` 字段；新增 `VideoDayGroup` 别名 + 6 个新方法声明 |
| `src/storage/manager.cpp` | **修改** | +360 行 | 实现 `listPhotos()`/`deletePhoto()`/`readJpegSize()`/`getPhotoCount()` + `listVideos()`/`deleteVideo()`/`getVideoCount()` |
| `include/display/gui.h` | **修改** | +20 行 | 新增 QStackedWidget、Gallery 按钮、PhotoGallery 成员及 3 个公开方法 |
| `src/display/gui.cpp` | **修改** | +90 行 | 布局重组 + Gallery 按钮/信号 + showGallery/showLivePreview/setGalleryStorage |
| `src/main.cpp` | **修改** | +3 行 | 调用 `gui.setGalleryStorage(&storage)` 绑定存储到相册 |
| `CMakeLists.txt` | **修改** | +2 行→+4 行 | v0.3 添加 `gallery.cpp/h`；v0.4 添加 `video_player.cpp/h` |
| **v0.4 新增** | | | |
| `include/display/video_player.h` | **新增** | +110 行 | `VideoPlayer` 轻量 AVI 播放器类声明 |
| `src/display/video_player.cpp` | **新增** | +340 行 | `VideoPlayer` 完整实现：AVI 解析、逐帧解码、UI 控制 |
| `include/storage/manager.h` | **修改** | +17 行 | 新增 `extractAviThumbnail()` 静态方法声明 |
| `src/storage/manager.cpp` | **修改** | +80 行 | 实现 `extractAviThumbnail()`：RIFF 解析 + 第一帧 JPEG 提取 |
| `include/display/gallery.h` | **修改** | +12 行 | 新增 VideoPlayer 成员、QStackedWidget 媒体栈、3 个缩略图方法 |
| `src/display/gallery.cpp` | **修改** | +120 行 | 视频缩略图生成、全屏集成 VideoPlayer 自动播放、翻页停止逻辑 |

**代码总量：v0.3 约 1040 行 + v0.4 约 680 行 = 约 1720 行**

---

## 三、新增数据结构 — `PhotoInfo` 和 `PhotoDayGroup`

**文件**: `include/storage/manager.h`（新增于第 228 行，`磁盘空间管理` 之后、`配置` 之前）

```cpp
struct PhotoInfo {
    std::string path;         // 完整路径 /data/photos/20260524/IMG_20260524_143025.jpg
    std::string filename;     // IMG_20260524_143025.jpg
    std::string dateStr;      // "2026-05-24"
    std::string timeStr;      // "14:30"
    time_t      timestamp;    // Unix 时间戳（用于排序）
    int         width;        // 图片宽度（视频为 0）
    int         height;       // 图片高度（视频为 0）
    size_t      fileSize;     // 文件大小（字节）
    bool        isVideo;      // true = AVI 视频, false = JPEG 照片 (v0.3 新增)
};

struct PhotoDayGroup {
    std::string dateStr;                 // "2026-05-24"
    std::vector<PhotoInfo> photos;
};
```

| 字段 | 用途 | 获取方式 |
|------|------|----------|
| `path` | 完整路径，删除和加载时使用 | `stat()` + 路径拼接 |
| `filename` | 缩略图下方标签 + 全屏信息栏 | 截取 `path` 最后一段 |
| `dateStr` | 日期分组 + 分隔线文字 | `localtime()` + `strftime()` |
| `timeStr` | 缩略图下方标签 | `localtime()` + `strftime()` |
| `timestamp` | 倒序排序 | `stat().st_mtime` |
| `width / height` | 缩略图标签 + 全屏信息栏（照片） | `readJpegSize()` 解析文件头；视频恒为 0 |
| `fileSize` | 全屏信息栏（KB/MB） | `stat().st_size` |
| `isVideo` | 区分照片/视频，影响缩略图图标和删除方式 | `listVideos()` 中设为 `true`，`listPhotos()` 中默认为 `false` |

---

## 四、StorageManager 新增方法

### 4.1 `readJpegSize()` — JPEG 快速尺寸读取（无需完整解码）

**文件**: `src/storage/manager.cpp:574`

```cpp
bool StorageManager::readJpegSize(const std::string& path, int& w, int& h);
```

**原理**：只读取文件前 4KB，扫描 JPEG SOF0（Start Of Frame）标记段提取宽高信息。完全不调用 libjpeg 解码，速度极快（< 1ms）。

**JPEG 文件结构**：
```
FF D8              ← SOI (Start of Image)
FF E0 00 10 ...    ← APP0 (JFIF header)
FF C0 00 11 08 HH HH WW WW   ← SOF0: 08=8-bit精度, HH=高度(2B大端), WW=宽度(2B大端)
FF DA ...          ← SOS (Start of Scan, 图像数据开始)
...
FF D9              ← EOI (End of Image)
```

**扫描算法**：
1. 从偏移 2 开始逐字节扫描
2. 遇到 `FF` 检查下一个字节：`D0-D7`（RST 标记）跳过，`D8`（SOI）/`D9`（EOI）/`DA`（SOS）忽略
3. 遇到 `C0`/`C1`/`C2`（SOF0/SOF1/SOF2）：读取偏移 +5~+8 处的 4 字节（高2B=高度, 低2B=宽度）
4. 其他标记段：读取长度字段后跳过

### 4.2 `listPhotos()` — 遍历目录获取全部照片

**文件**: `src/storage/manager.cpp:616`

```cpp
int StorageManager::listPhotos(std::vector<PhotoDayGroup>& out,
                               bool includeInfo = false);
```

**处理流程**：
1. `opendir(m_photoDir)` 遍历照片根目录
2. 两层目录结构：根目录直接文件（兼容） + 日期子目录（如 `20260524/`）
3. 用 `stat()` 获取文件名和修改时间，过滤非 `.jpg`/`.JPG` 文件
4. 按 `st_mtime` **倒序排序**（最新的在前）
5. `includeInfo=true` 时调用 `readJpegSize()` 读取每张照片的宽高
6. 用 `std::map<dateStr, PhotoDayGroup>` 按日期分组
7. 转换为 `std::vector` 输出（日期也倒序）

**时间复杂度**：O(N log N)（排序），N 为照片数。

### 4.3 `deletePhoto()` — 删除单张照片

**文件**: `src/storage/manager.cpp:717`

```cpp
int StorageManager::deletePhoto(const std::string& path);
```

**处理流程**：
1. `unlink(path)` 删除文件
2. 获取文件所在目录（`path.rfind('/')` 截取）
3. 若目录在 `m_photoDir` 子树内且非根目录，调用 `rmdir()` 尝试清理空目录
4. `rmdir` 对非空目录会失败，无害忽略

### 4.4 `getPhotoCount()` — 快速统计

**文件**: `src/storage/manager.cpp:698`

```cpp
int StorageManager::getPhotoCount();
```

纯目录遍历计数，不读文件内容。返回值可用于标题栏显示。

### 4.5 新增 `photoDir()` 访问器

```cpp
const std::string& photoDir() const { return m_photoDir; }
```

供外部查询当前照片存储路径。在 `setPhotoDir()` 声明之后新增。

### 4.6 `listVideos()` — 遍历目录获取全部视频 (v0.3 新增)

**文件**: `src/storage/manager.cpp:784`

```cpp
int StorageManager::listVideos(std::vector<PhotoDayGroup>& out);
```

**处理流程**（与 `listPhotos` 对称）：
1. `opendir(m_videoDir)` 遍历视频根目录
2. 两层目录结构：根目录直接文件 + 日期子目录（如 `20260524/`）
3. 过滤 `.avi` / `.AVI` 文件，用 `stat()` 获取元信息
4. 按 `st_mtime` 倒序排序
5. 每条记录设置 `isVideo = true`，`width/height` 固定为 0（不解析 AVI 头）
6. 按日期分组，转换为 vector 输出

### 4.7 `deleteVideo()` — 删除视频文件 (v0.3 新增)

```cpp
int StorageManager::deleteVideo(const std::string& path);
```

与 `deletePhoto()` 逻辑相同，作用于 `m_videoDir` 子树。

### 4.8 `getVideoCount()` — 快速统计 (v0.3 新增)

```cpp
int StorageManager::getVideoCount();
```

纯目录遍历计数，不读文件内容。

### 4.9 `extractAviThumbnail()` — AVI 第一帧 JPEG 提取 (v0.4 新增)

**文件**: `src/storage/manager.cpp:760`

```cpp
static bool StorageManager::extractAviThumbnail(
    const std::string& aviPath,
    std::vector<uint8_t>& jpegData);
```

**背景**：v0.3 中视频在缩略图网格仅显示 ▶ 占位符图标，无法预览实际画面。视频采用自建 AVI + MJPEG 编码，每一帧都以完整 JPEG 形式存储在 `00dc` chunk 中，因此只需提取第一帧即可生成缩略图。

**处理流程**：

```
1. fopen(aviPath, "rb")
2. 读取 RIFF 文件头 (12 字节)：验证 "RIFF" 魔数 + "AVI " 类型
3. 定位并跳过 hdrl LIST:
   ├── 读取 "LIST" FOURCC
   ├── 读取 LIST 大小 (chunkSize)
   ├── 读取 LIST 类型 "hdrl"
   └── fseek 跳过 chunkSize - 4 字节（-4 是因为 "hdrl" 已计入 chunkSize）
4. 定位 movi LIST:
   ├── 读取 "LIST" FOURCC
   ├── 读取 LIST 大小 (moviSize)
   ├── 读取 LIST 类型 "movi"
   └── 文件指针已位于第一个帧块位置
5. 读取第一个帧 chunk:
   ├── 读取 "00dc" FOURCC
   ├── 读取帧数据大小 (frameSize)
   ├── 合法性检查：frameSize ∈ [1, 500MB]
   └── fread frameSize 字节 → jpegData 输出向量
6. fclose(fp)
```

**AVI 容器结构示意**（仅展示解析涉及的部分）：

```
Offset  内容
------  ------
0x0000  RIFF [fileSize] AVI                ← RIFF 文件头 (12B)
0x000C  LIST [hdrlSize] hdrl               ← hdrl LIST 头 (12B)
0x0018  avih [56] [AviMainHeader]          ← avih 块 (64B)
...     LIST [strlSize] strl               ← strl LIST (可变)
...       strh [48] + strf [40]            ← 流头 + 格式
0x00XX  LIST [moviSize] movi               ← ★ 目标：movi LIST 头 (12B)
0x00XX  "00dc" [frame0Size] JPEG_DATA...   ← ★ 第一帧：(8B头 + JPEG数据)
0x00XX  "00dc" [frame1Size] JPEG_DATA...   ← 第二帧
...     ...
0x00XX  idx1 [idx1Size] [entries...]       ← 索引（不需要）
```

**边界情况处理**：
- 文件不存在 / 无法 `fopen` → 返回 `false`
- 前 12 字节不是 `RIFF...AVI ` → 不是合法 AVI，返回 `false`
- 第一个 LIST 不是 `hdrl` → 尝试 fseek 跳过（容错）
- `movi` LIST 后的第一个 chunk 不是 `00dc` → 返回 `false`（格式异常）
- `frameSize` 为 0 或 >500MB → 安全拒绝（防止异常文件导致内存溢出）
- `fread` 返回字节数 != `frameSize` → 返回 `false`（文件截断）

**性能**：
- 仅读取 AVI 文件头部 + 第一帧数据，不解析 idx1 索引表
- 对典型 640×480 MJPEG AVI 文件，第一帧 JPEG 约 15-50KB，提取耗时 < 5ms
- 不依赖 libjpeg，纯 C 文件 I/O + memcmp 比较

**返回值**：`jpegData` 向量中存储的是原始 JPEG 比特流（含 SOI/EOI），可直接送入 libjpeg-turbo 解码器或 Qt `QImage::loadFromData()`。

---

## 五、PhotoGallery 类详解

### 5.1 类结构

**文件**: `include/display/gallery.h`（v0.3 90 行 → v0.4 ~105 行）

```
PhotoGallery : public QWidget
├── 公开接口: refresh(), reset()
├── 信号: backToLive()
├── 事件: eventFilter() — 触摸滑动
├── 私有 Slot:
│   ├── onPrevPhoto, onNextPhoto      — 翻页 (v0.4: 先调用 stopVideoPlayback)
│   ├── onDeletePhoto                 — 删除 (v0.4: 先调用 stopVideoPlayback)
│   ├── onBackToGallery               — 回到网格
│   └── onVideoPlaybackFinished()     — v0.4 新增：视频播放结束回调
└── 私有方法:
    ├── buildGalleryView()            — 构建缩略图网格 UI
    ├── buildFullscreenView()         — 构建全屏 UI (v0.4: 引入 QStackedWidget)
    ├── loadVisibleThumbnails()       — 加载缩略图 (v0.4: 视频项生成真实缩略图)
    ├── clearThumbnails()             — 清空网格控件
    ├── createThumbnail()             — 文件路径 → 缩略图 QPixmap
    ├── createThumbnailFromJpegData() — v0.4 新增：内存 JPEG → 缩略图
    ├── createVideoThumbnail()        — v0.4 新增：AVI → 首帧 → 缩略图
    ├── stopVideoPlayback()           — v0.4 新增：停止当前视频播放
    └── updateFullscreenDisplay()     — 刷新全屏 (v0.4: 切换 照片/视频 子页面)
```

### 5.2 网格视图 (`buildGalleryView`)

**布局结构**：
```
QVBoxLayout
├── QHBoxLayout (顶部导航栏)
│   ├── ← Back 按钮 → 发射 backToLive() 信号
│   └── QLabel "Gallery (N photos)"
├── QScrollArea (可滚动缩略图区域)
│   └── QWidget
│       └── QGridLayout (3 列)
│           ├── 日期分隔线 (QLabel, colspan=3)
│           ├── [缩略图按钮 180×135 + 时间/分辨率标签] ← 每列一个
│           └── ...
└── QLabel "No photos yet..." (空相册时显示，平时隐藏)
```

**缩略图参数**：
```cpp
static constexpr int THUMB_COLS    = 3;       // 3 列
static constexpr int THUMB_W       = 170;     // 宽 170px
static constexpr int THUMB_H       = 120;     // 高 120px
static constexpr int THUMB_SPACING = 10;      // 间距 10px
```

三个缩略图宽度 170×3 + 间距 10×2 = 530px，在 700px 可视区内舒适展示。

### 5.3 缩略图生成 (`createThumbnail`)

```
输入: JPEG 文件路径, 目标宽 170, 目标高 120
输出: QPixmap 缩略图

libjpeg-turbo 路径 (HAS_LIBJPEG):
  1. fopen 读取 JPEG 文件
  2. jpeg_create_decompress + jpeg_stdio_src
  3. jpeg_read_header → 获取原始宽高
  4. 设置 scale_denom: 1/2/4/8 (根据原始宽度自动选择)
     - 若原始宽 > 170*8 → scale 1/8 (解码到 1/8 大小)
     - 若原始宽 > 170*4 → scale 1/4
     - 若原始宽 > 170*2 → scale 1/2
     - 否则 → 不解码缩放
  5. jpeg_start_decompress → 读扫描线 → RGB24
  6. QImage → QImage::scaled() → QPixmap (二次等比缩放适配 170×120)

Qt 退路 (无 HAS_LIBJPEG):
  QImage::load(文件路径) → scaled → QPixmap
```

**性能**：libjpeg 的 `scale_denom` 在解码阶段就降采样，大图可节省 4-16 倍内存和时间。

### 5.4 全屏视图 (`buildFullscreenView`)

**v0.4 改造**：原先的 `m_fullPhotoDisplay`（QLabel）替换为 `m_fullMediaStack`（QStackedWidget），支持照片和视频子页面的无缝切换。

**布局结构**：
```
QVBoxLayout
├── QHBoxLayout (顶部信息栏)
│   ├── ← Gallery 按钮 → onBackToGallery()
│   └── QLabel "IMG_xxx.jpg | 640×480 | 45 KB | 3/12"
├── QStackedWidget (m_fullMediaStack)        ← v0.4 新增
│   ├── [0] QLabel (m_fullPhotoDisplay)      ← 照片显示
│   │       └── QPixmap: 等比缩放 (最大 660×360)
│   └── [1] VideoPlayer (m_videoPlayer)      ← v0.4 新增
│           ├── QLabel 视频显示区
│           └── 控制栏: [▶/⏸] [3/120  00:05.3] [═══════进度条═══════]
└── QHBoxLayout (底部操作栏)
    ├── ◀ Prev (蓝色)
    ├── ♲ Delete (红色)
    └── Next ▶ (蓝色)
```

**页面切换逻辑（在 `updateFullscreenDisplay()` 中）**：

```cpp
void PhotoGallery::updateFullscreenDisplay() {
    stopVideoPlayback();  // 先停止可能正在播放的视频

    if (info.isVideo) {
        // 加载 AVI 到 VideoPlayer，成功则切换到播放器页
        if (m_videoPlayer->loadVideo(info.path)) {
            m_fullMediaStack->setCurrentIndex(1);   // 显示 VideoPlayer
            m_videoPlayer->play();                   // 自动播放
        } else {
            // 加载失败，保留在照片页显示错误占位符
            m_fullMediaStack->setCurrentIndex(0);
            m_fullPhotoDisplay->setText("Failed to load video");
        }
    } else {
        // 照片：切换到照片页
        m_fullMediaStack->setCurrentIndex(0);
        QImage img(info.path);
        m_fullPhotoDisplay->setPixmap(...);
    }
}
```

**翻页时的视频生命周期**：
- `onPrevPhoto()` / `onNextPhoto()` → 首先调用 `stopVideoPlayback()` → 再切换索引
- `onDeletePhoto()` → 先 `stopVideoPlayback()` → 再弹确认窗删除
- `onBackToGallery()` → 先 `stopVideoPlayback()` → 再切回网格
- 触摸滑动翻页 → 也通过 `onPrevPhoto()`/`onNextPhoto()` 触发，会先停止视频

**信息栏格式**：
```
IMG_20260524_143025.jpg  |  640x480  |  45 KB  |  3/12
  文件名                   分辨率     文件大小   当前/总数
```

**文件大小格式化**：
- ≥ 1MB: 显示为 `1.2 MB`（1 位小数）
- < 1MB: 显示为 `450 KB`（整数）

### 5.5 触摸滑动 (`eventFilter`)

全屏模式下左右滑动切换照片：

```cpp
bool PhotoGallery::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_fullView && m_stack->currentIndex() == 1) {
        if (event->type() == QEvent::MouseButtonPress) {
            m_touchStartX = me->x();      // 记录起点
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            int dx = me->x() - m_touchStartX;
            if (std::abs(dx) > 60) {      // 滑动阈值 60px
                if (dx < 0) onNextPhoto();  // 左滑→下一张
                else        onPrevPhoto();  // 右滑→上一张
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}
```

### 5.6 删除流程 (`onDeletePhoto`)

```
1. QMessageBox::question("Delete this photo?\n\nIMG_xxx.jpg")
   ├── Cancel → 不操作
   └── Yes → 继续
2. m_storage->deletePhoto(info.path)
   ├── 成功 →
   │   ├── refresh() 重新拉取列表
   │   ├── 若全部删完 → 回到网格视图（空相册提示）
   │   └── 否则 → 调整 m_currentIndex 不越界，刷新全屏
   └── 失败 → 静默忽略
```

### 5.7 刷新接口 (`refresh` / `reset`)

```cpp
void PhotoGallery::refresh() {
    // 1. 列出照片
    m_storage->listPhotos(m_groups, true);  // includeInfo=true 读宽高
    for (auto& g : m_groups)
        for (auto& p : g.photos)
            m_flatPhotos.push_back(p);

    // 2. 列出视频（v0.3 新增）
    std::vector<StorageManager::PhotoDayGroup> videoGroups;
    m_storage->listVideos(videoGroups);
    for (auto& g : videoGroups)
        for (auto& p : g.photos)
            m_flatPhotos.push_back(p);       // isVideo=true

    // 3. 按时间戳倒序混排照片+视频
    std::sort(m_flatPhotos.begin(), m_flatPhotos.end(),
              [](const auto& a, const auto& b) {
                  return a.timestamp > b.timestamp;
              });

    // 4. 按日期重建分组
    m_groups.clear();
    std::string lastDate;
    for (const auto& p : m_flatPhotos) {
        if (p.dateStr != lastDate) {
            m_groups.push_back({p.dateStr, {}});
            lastDate = p.dateStr;
        }
        m_groups.back().photos.push_back(p);
    }

    loadVisibleThumbnails();  // 重建缩略图网格
}

void PhotoGallery::reset() {
    m_currentIndex = -1;
    m_stack->setCurrentIndex(0);  // 回到网格
    refresh();
}
```

### 5.8 缩略图生成 — 视频支持 (v0.4 新增)

v0.4 为视频新增了两个缩略图生成方法，与照片的 `createThumbnail` 互补。

#### 5.8.1 `createThumbnailFromJpegData()` — 内存 JPEG 解码

```cpp
bool createThumbnailFromJpegData(const std::vector<uint8_t>& jpegData,
                                 int thumbW, int thumbH, QPixmap& out);
```

**与 `createThumbnail()` 的区别**：
| 对比项 | `createThumbnail()` | `createThumbnailFromJpegData()` |
|--------|---------------------|-------------------------------|
| 输入 | 文件路径 | 内存中的 JPEG 比特流 (`std::vector<uint8_t>`) |
| 数据源 | `jpeg_stdio_src(fp)` 从文件读取 | `jpeg_mem_src(data, size)` 从内存读取 |
| 适用场景 | 照片缩略图（文件系统 .jpg） | 从 AVI 提取出的帧 JPEG 数据 |
| 解码逻辑 | 其余完全一致：`scale_denom` 缩放解码 → RGB24 → `QImage::scaled()` |

**解码流程**：
```
jpeg_mem_src(&cinfo, jpegData.data(), jpegData.size())
    ↓
jpeg_read_header() → 获取原始宽高
    ↓
计算 scale_denom (1/2/4/8 根据原始宽度自动选择)
    ↓
jpeg_start_decompress() → 逐扫描线读取 RGB24
    ↓
QImage(rgb, w, h) → scaled(thumbW, thumbH) → QPixmap
```

#### 5.8.2 `createVideoThumbnail()` — 视频缩略图完整流程

```cpp
bool createVideoThumbnail(const std::string& aviPath,
                          int thumbW, int thumbH, QPixmap& out);
```

**两步管道**：

```
AVI 文件路径
    │
    ▼
StorageManager::extractAviThumbnail(aviPath, jpegData)
    │  解析 AVI RIFF 容器 → 定位 movi LIST → 提取第一帧 JPEG
    │  返回 std::vector<uint8_t> jpegData
    │
    ▼
createThumbnailFromJpegData(jpegData, thumbW, thumbH, out)
    │  内存 JPEG → libjpeg scale_denom 解码 → QImage::scaled() → QPixmap
    │
    ▼
QPixmap 缩略图 → setIcon() 显示在 QPushButton 上
```

**错误处理**：
- `extractAviThumbnail()` 失败（文件不存在 / 格式异常）→ 返回 `false`
- `createThumbnailFromJpegData()` 失败（JPEG 损坏 / 内存不足）→ 返回 `false`
- 调用方（`loadVisibleThumbnails()`）降级为 ▶ 占位符图标

#### 5.8.3 网格视图集成

**`loadVisibleThumbnails()` 中视频项处理逻辑**：

```cpp
if (info.isVideo) {
    QPixmap thumb;
    if (createVideoThumbnail(info.path, THUMB_W, THUMB_H, thumb)) {
        // 成功提取：显示真实缩略图
        btn->setIcon(QIcon(thumb));
        btn->setIconSize(QSize(THUMB_W, THUMB_H));
    } else {
        // 提取失败：回退到 ▶ 占位符（保持 v0.3 行为）
        btn->setText("\u25B6");
        btn->setStyleSheet(btn->styleSheet() +
            " color: #2ecc71; font-size: 42px;");
    }
}
```

**性能考虑**：
- 每个视频项在渲染时同步调用 `extractAviThumbnail()`，从磁盘读取第一帧
- 典型耗时：文件打开 ~1ms + AVI 头解析 ~2ms + JPEG 读取 ~1ms + 解码 ~5ms = **~10ms/视频**
- 对于数十个视频项，首次加载延迟可感知但可接受（< 1 秒）
- 后续可优化为缓存缩略图文件（如 `.thumb.jpg`）

---

## 六、VideoPlayer 类详解 (v0.4 新增)

### 6.1 设计目标

VideoPlayer 是一个专为嵌入式平台设计的**零依赖轻量 AVI/MJPEG 播放器**，完全不依赖 ffmpeg、gstreamer、vlc 等第三方解码库。

**核心约束**：
- iMX6ULL Cortex-A7 单核，512MB RAM
- linuxfb 渲染后端，无 GPU 硬解码
- AVI 文件为自建格式（MJPEG 编码，每秒 15-30 帧）
- 必须复用已有依赖（libjpeg-turbo、Qt Widgets）

### 6.2 类结构

**文件**: `include/display/video_player.h` (~110 行)

```
VideoPlayer : public QWidget
├── 公开接口:
│   ├── loadVideo(aviPath) → bool    — 打开 AVI 并解析头信息
│   ├── play()                        — 开始/恢复播放
│   ├── pause()                       — 暂停
│   ├── stop()                        — 停止并关闭文件
│   ├── isPlaying() → bool            — 播放状态查询
│   └── duration() → double           — 视频总时长（秒）
├── 信号:
│   ├── playbackFinished()            — 播放到最后一帧
│   └── playbackError(msg)            — 加载/解码错误
├── 私有 Slot:
│   └── onTimerTick()                 — QTimer 驱动逐帧解码
├── 私有方法:
│   ├── parseAviHeader()              — AVI 头解析 (avih + movi + idx1)
│   ├── seekToFrame(idx)              — fseek 到指定帧的 chunk 位置
│   ├── readFrameJpeg(jpegData)       — 读取当前帧 JPEG 数据
│   ├── decodeAndDisplay(jpegData)    — libjpeg 解码 → QPixmap → m_videoLabel
│   ├── clearState()                  — 释放文件资源
│   └── buildUI()                     — 构建控制栏 UI
└── 成员变量:
    ├── FILE* m_file                   — AVI 文件句柄（流式读取，不全载内存）
    ├── QTimer* m_timer                — 帧间隔定时器 (1000/fps ms)
    ├── double m_fps                   — 从 avih.dwMicroSecPerFrame 计算
    ├── int m_totalFrames              — 总帧数 (avih.dwTotalFrames)
    ├── int m_currentFrame             — 当前播放帧索引
    ├── long m_moviDataOffset          — movi 数据区起始文件偏移
    ├── vector<AviIndexEntry> m_index  — idx1 帧索引表（◀— 直接复用 manager.h 中的结构体）
    └── UI 控件: QLabel m_videoLabel, QPushButton m_btnPlayPause,
                  QLabel m_frameInfo, QSlider m_progressBar
```

### 6.3 AVI 文件头解析 (`parseAviHeader`)

这是整个播放器最核心的解析函数，必须在 `loadVideo()` 时成功完成，后续所有帧操作都依赖它产出的数据。

```
parseAviHeader() 解析流程:
═══════════════════════════════════════════════════════════════

步骤1: 验证 RIFF 文件头
  fread 12B → 验证 "RIFF" + 4B_size + "AVI "
  不是 "AVI " 类型 → 返回 false

步骤2: 解析 hdrl LIST → 提取 avih 元数据
  ┌──────────────────────────────────────────┐
  │ fread "LIST" + hdrlSize + "hdrl"         │  LIST 头 = 12B
  │                                          │
  │ 在 hdrl 数据范围内扫描子块:              │
  │   while (当前位置 < hdrl数据末端):        │
  │     fread chunkId[4] + chunkSize[4]     │
  │     if chunkId == "avih":               │
  │       fread AviMainHeader (56B)         │
  │       → m_width   = avih.dwWidth        │
  │       → m_height  = avih.dwHeight       │
  │       → m_totalFrames = avih.dwTotalFrames│
  │       → m_fps = 1000000 / dwMicroSecPerFrame│
  │       break                             │
  │     else:                               │
  │       fseek(chunkSize, SEEK_CUR) 跳过   │
  └──────────────────────────────────────────┘

步骤3: 定位 movi LIST → 记录数据区起始偏移
  跳过 hdrl 剩余部分 → fread "LIST" + moviSize + "movi"
  → m_moviDataOffset = ftell(m_file)  ★ 后续所有帧 seek 的基址

步骤4: 跳过 movi 数据 → 解析 idx1 索引表
  fseek(m_moviDataOffset + moviSize - 4, SEEK_SET)  // -4: "movi" 已计入
  fread "idx1" + idx1Size
  → m_index.resize(idx1Size / sizeof(AviIndexEntry))
  → fread m_index 全部条目

  若 m_totalFrames == 0 (录制中途异常退出):
    → 以 idx1 条目数为准
```

**AviIndexEntry 结构体**（复用 `include/storage/manager.h` 中的定义）：

```cpp
struct AviIndexEntry {
    uint32_t ckid;          // "00dc" — 帧块标识
    uint32_t dwFlags;       // 0x0010 = 关键帧
    uint32_t dwChunkOffset; // 相对 m_moviDataOffset 的偏移
    uint32_t dwChunkLength; // 帧块总大小 (8B头 + JPEG数据)
};
```

**BIG-ENDIAN 注意事项**：这些字段在文件中均为小端序 (`writeU32` 写入)，在 ARM/x86 上读取无需字节交换。

### 6.4 逐帧解码播放 (`onTimerTick`)

QTimer 按 `1000.0 / m_fps` 毫秒间隔触发，播放下一帧：

```
onTimerTick() 逐帧逻辑:
══════════════════════════

m_currentFrame++

if m_currentFrame >= m_totalFrames:
    pause()                   // 到达末尾
    emit playbackFinished()   // 通知 PhotoGallery
    return

readFrameJpeg(jpegData):      // ★ 流式单帧读取
    │
    ├── seekToFrame(m_currentFrame):
    │     entry = m_index[m_currentFrame]
    │     目标偏移 = m_moviDataOffset + entry.dwChunkOffset
    │     fseek(m_file, 目标偏移, SEEK_SET)
    │
    ├── fread "00dc" + frameSize[4B]
    │     安全检查: 0 < frameSize ≤ 100MB
    │
    └── fread frameSize 字节 → jpegData 向量

    ↓

decodeAndDisplay(jpegData):   // ★ JPEG → QPixmap → 屏幕
    │
    ├── libjpeg: jpeg_mem_src() + read_header + scale_denom 加速
    ├── start_decompress() → 读扫描线 → RGB24 向量
    └── QImage → scaled(label.size) → QPixmap → m_videoLabel->setPixmap()

    ↓

更新 UI:
    m_progressBar->setValue(m_currentFrame)
    m_frameInfo->setText("120/300  01:05.8")  // 当前帧/总数 + 倒计时
```

**内存特征**：
- 每次只将**一帧** JPEG 数据读入内存（~15-50KB for 640×480 MJPEG）
- 解码后的 RGB 缓冲在函数栈上（`std::vector<uint8_t>`），函数返回后自动释放
- `m_index` 向量占用 `帧数 × 16 字节`（如 300 帧 = 4.8KB）
- **不将整个 AVI 文件加载到内存**，全部通过 `fseek`/`fread` 流式访问

### 6.5 帧解码 (`decodeAndDisplay`)

```cpp
void decodeAndDisplay(const std::vector<uint8_t>& jpegData) {
    // 1. libjpeg 内存输入
    jpeg_mem_src(&cinfo, jpegData.data(), jpegData.size());

    // 2. 自适应 scale_denom（根据显示区域大小动态选择）
    int maxDim = std::max(m_videoLabel->width(), m_videoLabel->height());
    int scaleDenom = 1;
    if (cinfo.image_width > maxDim * 4)  scaleDenom = 4;
    else if (cinfo.image_width > maxDim * 2)  scaleDenom = 2;

    // 3. 解码 → RGB24 → QImage
    cinfo.scale_num = 1;
    cinfo.scale_denom = scaleDenom;
    jpeg_start_decompress(&cinfo);
    // ... 逐行读取 ...

    // 4. QImage::scaled() 等比缩放到显示标签大小
    QPixmap pix = QImage(rgb, w, h, QImage::Format_RGB888)
                  .scaled(m_videoLabel->size(), Qt::KeepAspectRatio);
    m_videoLabel->setPixmap(pix);
}
```

**性能**：`scale_denom` 在解码阶段就降采样（在 IDCT 之前），比解码全尺寸再 `QImage::scaled()` 快 2-4 倍。

### 6.6 UI 控制栏

**控制栏布局**：
```
[⏸]  120 / 300  01:05.8  [═══════════════░░░░░░░]  ← 可拖动进度条
 ▶/⏸     帧计数+倒计时          播放进度
```

**交互行为**：
| 操作 | 行为 |
|------|------|
| 点击 ▶/⏸ 按钮 | 切换播放/暂停状态，更换按钮样式 |
| 拖动进度条 | 暂停定时器（`m_timer->stop()`），实时更新帧信息标签 |
| 释放进度条 | `seekToFrame(targetFrame)` → 解码当前帧 → 若之前为播放态则恢复 `m_timer->start()` |
| 播放到最后一帧 | 自动暂停，发射 `playbackFinished()` |

**按钮样式切换**：
```cpp
// 播放中 → 暂停按钮（橙色）
m_btnPlayPause->setText("⏸");
// 暂停中 → 播放按钮（蓝色）
m_btnPlayPause->setText("▶");
```

### 6.7 资源管理

```cpp
void VideoPlayer::stop() {
    m_playing = false;
    m_timer->stop();
    if (m_file) {
        fclose(m_file);
        m_file = nullptr;
    }
    // 清理状态...
}

~VideoPlayer() { stop(); }  // RAII 保证析构时关闭文件
```

`loadVideo()` 调用前也会先 `stop()`，确保每次加载新视频时前一个视频的文件句柄已关闭。

### 6.8 完整播放生命周期

```
用户点击视频缩略图
    │
    ▼
PhotoGallery::updateFullscreenDisplay()
    │  stopVideoPlayback()  ← 停止上一视频（如果有）
    │  m_videoPlayer->loadVideo(path)
    │      ├── parseAviHeader()   → 解析 fps/帧数/idx1 索引
    │      └── readFrameJpeg()    → 显示第一帧
    │
    ├── 成功 → m_fullMediaStack->setCurrentIndex(1)
    │          m_videoPlayer->play()
    │              └── m_timer->start(33ms)  ← 30fps → 33ms间隔
    │
    └── 失败 → 保持 index 0，显示错误占位符

播放中:
    onTimerTick() 每 33ms 触发一次
    │
    ├── m_currentFrame++
    ├── seekToFrame + readFrameJpeg + decodeAndDisplay
    └── 更新进度条 + 帧信息标签

用户操作:
    ├── 点击 ◀ Prev / ▶ Next → stopVideoPlayback() → 翻页
    ├── 点击 ♲ Delete      → stopVideoPlayback() → 确认弹窗
    ├── 点击 ← Gallery     → stopVideoPlayback() → 回到网格
    └── 视频自然播放结束    → pause() + emit playbackFinished()
```

---

## 七、CameraGUI 集成修改

### 7.1 新增成员 (`include/display/gui.h`)

```cpp
// === 新增 UI 控件 ===
QStackedWidget* m_mainStack     = nullptr;   // [0]=实时预览, [1]=相册
QWidget*        m_liveViewContainer = nullptr;
QPushButton*    m_btnGallery     = nullptr;   // 相册按钮
PhotoGallery*   m_gallery        = nullptr;   // 相册组件

// === 新增方法 ===
void setGalleryStorage(StorageManager* storage);  // 绑定存储指针
void showGallery();                                // 切换到相册
void showLivePreview();                            // 切换到实时预览

// === 新增 Slot ===
void onGallery();          // Gallery 按钮响应
void onBackFromGallery();  // 从相册返回
```

### 7.2 布局重构 (`src/display/gui.cpp:buildUI`)

**重构前**：顶层 `QVBoxLayout` → `m_videoDisplay`(QLabel) + 状态栏 + 按钮栏 + 设置栏

**重构后**：顶层 `QVBoxLayout` → `m_mainStack`(QStackedWidget) + 状态栏 + 按钮栏 + 设置栏

```
QStackedWidget (m_mainStack)
├── [0] m_liveViewContainer
│       └── m_videoDisplay (QLabel, 和之前一样)
└── [1] m_gallery (PhotoGallery)
```

**按钮栏新增**（顺序：Capture → Record → **Gallery** → Settings → Stretch）：

```cpp
btnLayout->addWidget(m_btnCapture);
btnLayout->addWidget(m_btnRecord);
btnLayout->addWidget(m_btnGallery);   // 新增
btnLayout->addWidget(m_btnSettings);
btnLayout->addStretch();
```

**Gallery 按钮样式**：与 Settings 按钮相同（深灰底色），统一视觉风格。

**信号连接**：
```cpp
connect(m_btnGallery, &QPushButton::clicked, this, &CameraGUI::onGallery);
connect(m_gallery, &PhotoGallery::backToLive, this, &CameraGUI::onBackFromGallery);
```

### 7.3 核心方法实现 (`src/display/gui.cpp`)

```cpp
void CameraGUI::onGallery() {
    if (m_gallery && m_mainStack->currentIndex() == 0)
        showGallery();
}

void CameraGUI::setGalleryStorage(StorageManager* storage) {
    if (m_gallery) delete m_gallery;
    m_gallery = new PhotoGallery(storage, this);
    connect(m_gallery, &PhotoGallery::backToLive, this, &CameraGUI::onBackFromGallery);
    m_mainStack->insertWidget(1, m_gallery);  // 替换旧的 index 1
}

void CameraGUI::showGallery() {
    m_gallery->reset();              // 刷新列表 + 回到网格
    m_mainStack->setCurrentIndex(1); // 切换到相册
    m_btnCapture->hide();            // 隐藏拍摄相关按钮
    m_btnRecord->hide();
    m_settingsPanel->hide();
}

void CameraGUI::showLivePreview() {
    m_mainStack->setCurrentIndex(0); // 切换回实时预览
    m_btnCapture->show();            // 恢复按钮
    m_btnRecord->show();
}

void CameraGUI::onBackFromGallery() { showLivePreview(); }
```

**设计要点**：
- `setGalleryStorage()` 延迟绑定 storage 指针——因为 `main.cpp` 中 `StorageManager` 在 `CameraGUI` 之后创建
- `showGallery()` 隐藏 Capture/Record/设置面板，避免用户误操作
- `showLivePreview()` 恢复这些按钮

---

## 八、main.cpp 集成

**文件**: `src/main.cpp:189`

```cpp
// ---- 初始化存储管理器 ----
StorageManager storage(photoDir, videoDir);
g_storage = &storage;

// 绑定存储到相册组件
gui.setGalleryStorage(&storage);   // 新增 1 行
```

仅需 1 行：将已创建的 `StorageManager` 实例指针传给 `CameraGUI`，后者再传给 `PhotoGallery`。

---

## 九、CMakeLists.txt 修改

### v0.3

```cmake
set(DISPLAY_SOURCES
    src/display/gui.cpp
    include/display/gui.h    # 需要 MOC 处理 (含 Q_OBJECT)
    src/display/gallery.cpp        # 新增
    include/display/gallery.h      # 新增 (需要 MOC)
)
```

### v0.4 更新

```cmake
set(DISPLAY_SOURCES
    src/display/gui.cpp
    include/display/gui.h         # MOC
    src/display/gallery.cpp
    include/display/gallery.h     # MOC
    src/display/video_player.cpp  # v0.4 新增
    include/display/video_player.h # v0.4 新增 (MOC)
)
```

`gallery.h` 和 `video_player.h` 含 `Q_OBJECT` 宏，MOC 会自动处理。`video_player.h` 引用了 `storage/manager.h` 中的 `AviIndexEntry` 结构体（复用，无额外依赖）。

---

## 十、用户交互流程

```
主界面 — 实时预览
│
├─ 点击 [Gallery] → showGallery()
│   │
│   ├─ 缩略图网格视图
│   │   ├─ 显示日期分组 (── 2026-05-24 ──)
│   │   ├─ 每张照片: 170×120 缩略图 + 时间/分辨率标签
│   │   ├─ 每个视频: 170×120 缩略图(第一帧封面) + 时间/[VID]标签  ← v0.4
│   │   ├─ 点击缩略图 → 全屏查看 (当前索引)
│   │   ├─ 点击 [← Back] → 发射 backToLive → 回到实时预览
│   │   └─ 空相册 → "No photos yet" 提示
│   │
│   └─ 全屏查看视图
│       ├─ 顶部: ← Gallery | [VID] VID_xxx.avi | 2.5 MB | 3/12
│       ├─ 中央: 
│       │   ├─ 照片: 等比缩放 QPixmap (最大 660×360)
│       │   └─ 视频: VideoPlayer 自动播放，含 ▶/⏸ + 进度条    ← v0.4
│       ├─ 底部: [◀ Prev] [♲ Delete] [Next ▶]
│       ├─ 左右滑动 60px+ → 翻页（停止视频后切换）
│       ├─ 点击 [♲ Delete] → 确认弹窗 → 删除并刷新
│       └─ 点击 [← Gallery] → 停止视频 → 缩略图网格
│
└─ onBackFromGallery → showLivePreview() → 回到实时预览
```

---

## 十一、内存与性能分析

### 11.1 内存占用

| 项目 | 计算 | 估计 |
|------|------|------|
| 单张缩略图 QPixmap | 170×120×4 (RGBA32) | ~82 KB |
| 同时可见缩略图 | 3 列 × 2 行 = 6 张 | ~490 KB |
| 额外缓冲（上/下各一屏） | 6×2 | ~1 MB |
| 全屏照片（解码后 QPixmap） | 660×360×4 | ~0.95 MB |
| 文件列表（100 条 PhotoInfo） | 100×~200 字节 | ~20 KB |
| **v0.4 新增** | | |
| VideoPlayer idx1 索引表（300 帧） | 300 × 16 字节 | ~4.8 KB |
| 视频单帧解码缓冲（RGB24） | 640×480×3 | ~0.9 MB |
| AVI 提取首帧 JPEG 缓冲 | ~50 KB | ~50 KB |
| **峰值合计** | | **~3.4 MB** |

在 iMX6ULL 512MB RAM 上仍有充足余量。

### 11.2 CPU 耗时

| 操作 | 估算 | 说明 |
|------|------|------|
| 列出 100 张照片 | <50ms | 目录遍历 + stat，不读文件 |
| 创建单张缩略图（libjpeg scale_denom=4） | ~8ms | 解码 1/4 大小 ≈ 160×120 |
| 首次加载 6 张缩略图 | ~50ms | 6×8ms |
| 全屏加载照片（Qt 解码） | ~25ms | `QImage::load` 正常 JPEG |
| **v0.4 新增** | | |
| 提取视频首帧缩略图（AVI→JPEG→解码） | ~10ms | `extractAviThumbnail` 2ms + 解码 8ms |
| 视频播放单帧解码（640×480） | ~6ms | libjpeg-turbo NEON 加速，scale_denom=2 |
| 视频帧间隔（30fps） | ~33ms | 解码留约 27ms 余量，无掉帧风险 |

### 11.3 优化措施

1. **libjpeg `scale_denom`**：大图解码时缩小 2/4/8 倍，节省 4-16× 内存和 CPU（照片缩略图 + 视频播放均使用）
2. **`includeInfo=true` 仅在 listPhotos 时解析宽高**，后续操作只访问已缓存的数据
3. **`deleteLater()`** 清理旧缩略图控件，Qt 事件循环统一释放
4. **触摸滑动防抖**：阈值 60px，避免误触
5. **v0.4：流式帧读取** — VideoPlayer 每次只从磁盘读取一帧，不预加载全部帧到内存
6. **v0.4：复用 AVI 结构体** — VideoPlayer 直接使用 `StorageManager::AviIndexEntry`，零冗余定义

---

## 十二、与现有模块的关系

```
                    main.cpp
                       │
                gui.setGalleryStorage(&storage)
                       │
        ┌──────────────┼──────────────┐
        ▼              ▼              ▼
   CameraGUI      StorageManager   MJPEGServer
        │              │              ...
   ┌────┴────┐    listPhotos()
   │         │    listVideos()
   ▼         ▼    deletePhoto()
 [0]实时  [1]相册   deleteVideo()
 预览    PhotoGallery   readJpegSize()
          │   │          extractAviThumbnail()  ← v0.4
     ┌────┴───┴────┐
     ▼             ▼
 网格视图       全屏视图 (QStackedWidget)
  │                 ├── [0] QLabel (照片)
  │                 ├── [1] VideoPlayer (视频) ← v0.4
  │                 │       ├── parseAviHeader()
  │                 │       ├── readFrameJpeg()
  │                 │       └── decodeAndDisplay()
  │                 └── onVideoPlaybackFinished()
  │
  ├── createThumbnail()        (照片，文件路径)
  ├── createVideoThumbnail()   (视频，AVI→首帧JPEG) ← v0.4
  │       └── extractAviThumbnail() + createThumbnailFromJpegData()
  └── createThumbnailFromJpegData() (内存JPEG→缩略图)
```

**模块依赖关系**：
- `StorageManager` 负责 I/O（目录遍历、文件读写、AVI 头解析/JPEG 提取）——不依赖 Qt
- `PhotoGallery` 负责 UI 编排（缩略图渲染、翻页、删除确认、视频启动/停止协调）
- `VideoPlayer` 负责视频播放（AVI 解析、逐帧解码、定时器控制）——依赖 `StorageManager::AviIndexEntry`
- `CameraGUI` 负责切换（QStackedWidget index 0↔1）
- `main.cpp` 负责绑定（`setGalleryStorage`）

---

## 十三、编译说明

相册模块**无需额外依赖**，编译方式与整个项目一致：

```bash
mkdir -p build && cd build
cmake .. && make -j$(nproc)
```

新增文件被自动加入 `CMakeLists.txt` 的 `DISPLAY_SOURCES`，MOC 自动处理 `gallery.h` 中的 `Q_OBJECT` 宏。

---

## 十四、测试方法

### 14.1 PC 端 Mock 测试

```bash
# 创建测试照片目录
mkdir -p /tmp/smartcam/photos/20260524
mkdir -p /tmp/smartcam/photos/20260523

# 放入测试用的 JPEG 文件
cp /path/to/photo1.jpg /tmp/smartcam/photos/20260524/IMG_20260524_143025.jpg
cp /path/to/photo2.jpg /tmp/smartcam/photos/20260524/IMG_20260524_143130.jpg
cp /path/to/photo3.jpg /tmp/smartcam/photos/20260523/IMG_20260523_090000.jpg

# 运行
./smartcam
# → 点击 Gallery 按钮即可测试
```

### 14.2 空相册测试

```bash
rm -rf /tmp/smartcam/photos/*
./smartcam
# → 点 Gallery 应显示 "No photos yet"
```

### 14.3 真实开发板测试

```bash
./smartcam --device /dev/video0 --fmt mjpeg -platform linuxfb
# → 拍摄几张 → 点 Gallery 浏览 → 删除 → 验证
```

---

## 十五、已知限制与后续改进

| 限制 | 说明 | 改进思路 |
|------|------|----------|
| ~~开发板重启后照片丢失~~ | ✅ **v0.5 已解决**：Settings 面板 Store 下拉框支持切换存储路径至 eMMC；系统级/用户级配置优先级 | - |
| 缩略图一次性全部加载 | 媒体 > 100 条时首次加载较慢 | 虚拟滚动：只加载可见 ±1 屏的缩略图 |
| 无幻灯片播放 | 无法自动轮播 | 添加 QTimer + Play 按钮 |
| ~~视频无预览缩略图~~ | ✅ **v0.4 已解决**：`extractAviThumbnail()` 从 AVI 提取第一帧 JPEG | - |
| ~~视频不支持播放~~ | ✅ **v0.4 已解决**：`VideoPlayer` 轻量 AVI 解码器 + QTimer 帧率控制 | - |
| 无多选删除 | 每次只能删一张 | 全选/反选 + 批量删除 |
| 无导出功能 | 无法拷贝到 USB | 添加 Export 按钮调用 shell 脚本 |

### 15.1 重启后照片丢失问题 (v0.5)

**现象**：程序关闭再启动后 Gallery 能看到照片，但重启开发板后照片消失。

**根因**：`/data` 在 i.MX6ULL 开发板上是 tmpfs（内存文件系统），内容在断电/重启时丢失。照片虽写入 `/data/photos/YYYYMMDD/IMG_*.jpg`，但实际存在于 RAM 中，不是持久存储。

**解决**：在 GUI Settings 面板新增 **Store** 下拉框，用户可选择：
- **Temporary (/data)** — tmpfs，速度快但重启丢失
- **Persistent (eMMC)** — `/home/debian/smartcam`，eMMC 持久化

切换即时生效（`StorageManager::setPhotoDir()` / `setVideoDir()`），并自动保存到配置文件（`~/.config/smartcam/smartcam.conf`），重启后自动恢复。

详见 `04-storage-module-implementation.md` 第十二节。

---

## 十六、变更记录

| 日期 | 变更内容 |
|------|----------|
| 2026-05-24 | 初始实现：StorageManager 扩展（listPhotos/deletePhoto/readJpegSize）、PhotoGallery 类、CameraGUI 集成、CMake 构建 |
| 2026-05-24 | 修复编译错误：移除头文件中未实现的 `onThumbnailClicked()` slot 声明 |
| 2026-05-26 | **v0.4**：视频缩略图 + 播放支持。新增 `extractAviThumbnail()` 从 AVI 提取第一帧 JPEG 作封面；新增 `VideoPlayer` 轻量 AVI 播放器；`PhotoGallery` 全屏集成视频播放 |
| 2026-05-27 | **v0.5**：Settings 面板增加 Store 下拉框（tmpfs/eMMC 存储路径切换）；ConfigManager 写支持；配置加载三级优先级；解决开发板重启后照片丢失问题 |
