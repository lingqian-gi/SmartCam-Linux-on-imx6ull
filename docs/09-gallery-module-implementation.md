# SmartCam 相册模块 — 实现文档

> 版本: v1.0 | 日期: 2026-05-24 | 作者: SmartCam Team

---

## 一、概述

相册模块为用户提供已拍摄照片的浏览、查看和管理功能，直接运行在 7 寸触摸屏上（800×480）。利用现有的照片存储目录结构（`/data/photos/YYYYMMDD/IMG_*.jpg`），自动索引所有 JPEG 文件并按日期分组展示。

### 功能清单

| 功能 | 描述 |
|------|------|
| 照片浏览 | 3 列缩略图网格，按日期分组，自动滚动 |
| 全屏查看 | 点击缩略图进入，等比缩放适配屏幕 |
| 前后翻页 | 左右箭头按钮 + 触摸滑动翻页（阈值 60px） |
| 照片信息 | 显示文件名、分辨率、文件大小、当前页码 |
| 删除照片 | 确认弹窗后删除，自动清理空目录 |
| 返回 | 全屏→网格→实时预览，三级导航 |
| 空相册提示 | 无照片时显示 "No photos yet — Tap Capture to take one!" |
| 自动刷新 | 删除后自动刷新列表，回到正确位置 |

---

## 二、文件变更清单

| 文件 | 操作 | 行数变化 | 说明 |
|------|------|----------|------|
| `include/display/gallery.h` | **新增** | +90 行 | `PhotoGallery` 类声明 |
| `src/display/gallery.cpp` | **新增** | +410 行 | `PhotoGallery` 完整实现 |
| `include/storage/manager.h` | **修改** | +65 行 | 新增 `PhotoInfo`/`PhotoDayGroup` 结构体 + 4 个新方法声明 |
| `src/storage/manager.cpp` | **修改** | +210 行 | 实现 `listPhotos()`/`deletePhoto()`/`readJpegSize()`/`getPhotoCount()` |
| `include/display/gui.h` | **修改** | +20 行 | 新增 QStackedWidget、Gallery 按钮、PhotoGallery 成员及 3 个公开方法 |
| `src/display/gui.cpp` | **修改** | +90 行 | 布局重组 + Gallery 按钮/信号 + showGallery/showLivePreview/setGalleryStorage |
| `src/main.cpp` | **修改** | +3 行 | 调用 `gui.setGalleryStorage(&storage)` 绑定存储到相册 |
| `CMakeLists.txt` | **修改** | +2 行 | 添加 `gallery.cpp/h` 到 DISPLAY_SOURCES |

**代码总量：约 890 行（新增约 500 行 + 存储扩展约 210 行 + GUI 集成约 90 行 + 其他约 90 行）**

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
    int         width;        // 图片宽度
    int         height;       // 图片高度
    size_t      fileSize;     // 文件大小（字节）
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
| `width / height` | 缩略图标签 + 全屏信息栏 | 可选：`readJpegSize()` 解析文件头 |
| `fileSize` | 全屏信息栏（KB/MB） | `stat().st_size` |

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

---

## 五、PhotoGallery 类详解

### 5.1 类结构

**文件**: `include/display/gallery.h`（新增 90 行）

```
PhotoGallery : public QWidget
├── 公开接口: refresh(), reset()
├── 信号: backToLive()
├── 事件: eventFilter() — 触摸滑动
├── 私有 Slot: onPrevPhoto, onNextPhoto, onDeletePhoto, onBackToGallery
└── 私有方法:
    ├── buildGalleryView()      — 构建缩略图网格 UI
    ├── buildFullscreenView()   — 构建全屏查看 UI
    ├── loadVisibleThumbnails()  — 加载缩略图到网格
    ├── clearThumbnails()        — 清空网格控件
    ├── createThumbnail()        — libjpeg 解码 + Qt 缩放到缩略图尺寸
    └── updateFullscreenDisplay()— 刷新全屏视图显示
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

**布局结构**：
```
QVBoxLayout
├── QHBoxLayout (顶部信息栏)
│   ├── ← Gallery 按钮 → onBackToGallery()
│   └── QLabel "IMG_xxx.jpg | 640×480 | 45 KB | 3/12"
├── QLabel (照片显示区, Expanding)
│   └── QPixmap: 等比缩放 (最大 660×360)
└── QHBoxLayout (底部操作栏)
    ├── ◀ Prev (蓝色)
    ├── ♲ Delete (红色)
    └── Next ▶ (蓝色)
```

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
    m_storage->listPhotos(m_groups, true);  // includeInfo=true 读宽高
    // 扁平化：group → vector
    for (auto& g : m_groups)
        for (auto& p : g.photos)
            m_flatPhotos.push_back(p);
    loadVisibleThumbnails();  // 重建缩略图网格
}

void PhotoGallery::reset() {
    m_currentIndex = -1;
    m_stack->setCurrentIndex(0);  // 回到网格
    refresh();
}
```

---

## 六、CameraGUI 集成修改

### 6.1 新增成员 (`include/display/gui.h`)

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

### 6.2 布局重构 (`src/display/gui.cpp:buildUI`)

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

### 6.3 核心方法实现 (`src/display/gui.cpp`)

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

## 七、main.cpp 集成

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

## 八、CMakeLists.txt 修改

```cmake
set(DISPLAY_SOURCES
    src/display/gui.cpp
    include/display/gui.h    # 需要 MOC 处理 (含 Q_OBJECT)
    src/display/gallery.cpp        # 新增
    include/display/gallery.h      # 新增 (需要 MOC)
)
```

`gallery.h` 含 `Q_OBJECT` 宏，MOC 会自动处理。

---

## 九、用户交互流程

```
主界面 — 实时预览
│
├─ 点击 [Gallery] → showGallery()
│   │
│   ├─ 缩略图网格视图
│   │   ├─ 显示日期分组 (── 2026-05-24 ──)
│   │   ├─ 每张照片: 180×135 缩略图 + 时间/分辨率标签
│   │   ├─ 点击缩略图 → 全屏查看 (当前索引)
│   │   ├─ 点击 [← Back] → 发射 backToLive → 回到实时预览
│   │   └─ 空相册 → "No photos yet" 提示
│   │
│   └─ 全屏查看视图
│       ├─ 顶部: ← Gallery | IMG_xxx.jpg | 640×480 | 45 KB | 3/12
│       ├─ 中央: 等比缩放的照片 (最大 660×360)
│       ├─ 底部: [◀ Prev] [♲ Delete] [Next ▶]
│       ├─ 左右滑动 60px+ → 翻页
│       ├─ 点击 [♲ Delete] → 确认弹窗 → 删除并刷新列表
│       └─ 点击 [← Gallery] → 回到缩略图网格
│
└─ onBackFromGallery → showLivePreview() → 回到实时预览
```

---

## 十、内存与性能分析

### 10.1 内存占用

| 项目 | 计算 | 估计 |
|------|------|------|
| 单张缩略图 QPixmap | 170×135×4 (RGBA32) | ~92 KB |
| 同时可见缩略图 | 3 列 × 2 行 = 6 张 | ~550 KB |
| 额外缓冲（上/下各一屏） | 6×2 | ~1.1 MB |
| 全屏照片（解码后 QPixmap） | 660×360×4 | ~0.95 MB |
| 文件列表（100张 PhotoInfo） | 100×~200 字节 | ~20 KB |
| **峰值合计** | | **~2.6 MB** |

在 iMX6ULL 512MB RAM 上完全可行。

### 10.2 CPU 耗时

| 操作 | 估算 | 说明 |
|------|------|------|
| 列出 100 张照片 | <50ms | 目录遍历 + stat，不读文件 |
| 创建单张缩略图（libjpeg scale_denom=4） | ~8ms | 解码 1/4 大小 ≈ 160×120 |
| 首次加载 6 张缩略图（异步） | ~50ms | 6×8ms，不阻塞 UI |
| 全屏加载 Qt 解码 | ~25ms | QImage::load 正常 JPEG |

### 10.3 优化措施

1. **libjpeg `scale_denom`**：大图解码时缩小 2/4/8 倍，节省 4-16× 内存和 CPU
2. **includeInfo=true 仅在 listPhotos 时解析宽高**，后续操作只访问已缓存的数据
3. **deleteLater()** 清理旧缩略图控件，Qt 事件循环统一释放
4. **触摸滑动防抖**：阈值 60px，避免误触

---

## 十一、与现有模块的关系

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
   │         │    deletePhoto()
   ▼         ▼    readJpegSize()
 [0]实时  [1]相册
 预览    PhotoGallery
              │
         ┌────┴────┐
         ▼         ▼
     网格视图    全屏视图
```

- `StorageManager` 负责 I/O（读目录、读文件头、删除文件）
- `PhotoGallery` 负责 UI（缩略图渲染、翻页、删除确认）
- `CameraGUI` 负责切换（QStackedWidget index 0↔1）
- `main.cpp` 负责绑定（`setGalleryStorage`）

---

## 十二、编译说明

相册模块**无需额外依赖**，编译方式与整个项目一致：

```bash
mkdir -p build && cd build
cmake .. && make -j$(nproc)
```

新增文件被自动加入 `CMakeLists.txt` 的 `DISPLAY_SOURCES`，MOC 自动处理 `gallery.h` 中的 `Q_OBJECT` 宏。

---

## 十三、测试方法

### 13.1 PC 端 Mock 测试

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

### 13.2 空相册测试

```bash
rm -rf /tmp/smartcam/photos/*
./smartcam
# → 点 Gallery 应显示 "No photos yet"
```

### 13.3 真实开发板测试

```bash
./smartcam --device /dev/video0 --fmt mjpeg -platform linuxfb
# → 拍摄几张 → 点 Gallery 浏览 → 删除 → 验证
```

---

## 十四、已知限制与后续改进

| 限制 | 说明 | 改进思路 |
|------|------|----------|
| 缩略图一次性全部加载 | 照片 > 100 张时首次加载较慢 | 虚拟滚动：只加载可见 ±1 屏的缩略图 |
| 无幻灯片播放 | 无法自动轮播 | 添加 QTimer + Play 按钮 |
| 无视频回放 | AVI 文件不在网格中显示 | 扩展 `listFiles()` 支持 `.avi` 并区分图标 |
| 无多选删除 | 每次只能删一张 | 全选/反选 + 批量删除 |
| 无导出功能 | 无法拷贝到 USB | 添加 Export 按钮调用 shell 脚本 |

---

## 十五、变更记录

| 日期 | 变更内容 |
|------|----------|
| 2026-05-24 | 初始实现：StorageManager 扩展（listPhotos/deletePhoto/readJpegSize）、PhotoGallery 类、CameraGUI 集成、CMake 构建 |
| 2026-05-24 | 修复编译错误：移除头文件中未实现的 `onThumbnailClicked()` slot 声明 |
