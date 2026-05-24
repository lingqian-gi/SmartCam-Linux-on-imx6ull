# SmartCam 相册功能 — 实现计划

> 版本: v1.0 | 日期: 2026-05-24 | 预计工期: 2-3天
> 目标: 在7寸触摸屏上浏览、查看、管理已拍摄的照片

---

## 一、功能概述

### 1.1 核心需求

| 需求 | 说明 |
|------|------|
| 照片浏览 | 缩略图网格展示，按日期分组，3列布局适配 800x480 屏幕 |
| 全屏查看 | 点击缩略图进入全屏模式，JPEG 缩放适配屏幕 |
| 前后翻页 | 左右箭头/触摸滑动切换上一张/下一张 |
| 删除照片 | 全屏模式下删除当前照片（带确认弹窗） |
| 返回按钮 | 全屏模式返回缩略图网格，网格模式返回实时预览 |
| 照片信息 | 显示文件名、日期、分辨率、文件大小 |
| 响应式加载 | 只加载可见区域的缩略图（懒加载），节省内存 |

### 1.2 非本期需求（后续迭代）

- ~~幻灯片自动播放~~
- ~~照片导出到 USB~~
- ~~视频回放（.avi）~~
- ~~多选批量删除~~

---

## 二、模块划分

```
SmartCam 相册功能
├── MOD-09: StorageManager 扩展          ← 新增/修改
│   ├── listPhotos()      获取照片列表
│   ├── getPhotoInfo()    获取单张元信息
│   ├── deletePhoto()     删除单张照片
│   └── getPhotoCount()   照片总数统计
│
├── MOD-10: PhotoGallery 类              ← 新增
│   ├── 缩略图网格视图 (QScrollArea + QGridLayout)
│   ├── 全屏查看器 (QDialog / QWidget)
│   ├── 触摸手势支持 (左右滑动)
│   ├── 删除确认弹窗 (QMessageBox)
│   └── 与 StorageManager 交互
│
└── MOD-11: CameraGUI 集成               ← 修改
    ├── 新增"Gallery"按钮
    ├── QStackedWidget 切换实时预览/相册
    └── 按钮状态联动
```

---

## 三、数据结构设计

### 3.1 照片信息结构体

```cpp
// include/storage/manager.h 新增

struct PhotoInfo {
    std::string path;         // 完整路径 /data/photos/20260524/IMG_20260524_143025.jpg
    std::string filename;     // IMG_20260524_143025.jpg
    std::string dateStr;      // 2026-05-24
    std::string timeStr;      // 14:30:25
    time_t      timestamp;    // Unix 时间戳（用于排序）
    int         width;        // 图片宽度（从 JPEG 头快速读取）
    int         height;       // 图片高度
    size_t      fileSize;     // 文件大小（字节）
};

// 日期分组（按天）
struct PhotoDayGroup {
    std::string dateStr;
    std::vector<PhotoInfo> photos;
};
```

### 3.2 StorageManager 新增接口

```cpp
// include/storage/manager.h 新增

class StorageManager {
public:
    // ... 现有接口 ...

    /**
     * @brief 获取所有照片列表（按时间倒序）
     * @param out  输出：按日期分组的照片列表
     * @return 照片总数
     */
    int listPhotos(std::vector<PhotoDayGroup>& out,
                   bool includeInfo = false);  // false=只读文件名，true=解析宽高

    /**
     * @brief 获取照片总数
     */
    int getPhotoCount();

    /**
     * @brief 删除一张照片（文件 + 清理空目录）
     * @return 0 成功，-1 失败
     */
    int deletePhoto(const std::string& path);

    /**
     * @brief 从 JPEG 文件头快速读取宽高（不完整解码）
     */
    static bool readJpegSize(const std::string& path, int& w, int& h);
};
```

### 3.3 JPEG 快速尺寸读取算法（面试亮点）

```cpp
// 解析 JPEG 的 APP0/SOF0 标记获取宽高，只需读取前 2KB，不解码像素
// JPEG 结构: FF D8 → ... → FF C0 (SOF0) → 长度(2B) → 精度(1B) → 高度(2B) → 宽度(2B)

bool StorageManager::readJpegSize(const std::string& path, int& w, int& h) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;

    uint8_t buf[2048];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);

    if (n < 100 || buf[0] != 0xFF || buf[1] != 0xD8) return false;

    for (size_t i = 2; i < n - 8; i++) {
        if (buf[i] == 0xFF && buf[i+1] == 0xC0) {  // SOF0 标记
            h = (buf[i+5] << 8) | buf[i+6];
            w = (buf[i+7] << 8) | buf[i+8];
            return true;
        }
    }
    return false;
}
```

---

## 四、UI 设计

### 4.1 缩略图网格视图（700x400 可视区域）

```
┌─────────────────────────────────────────┐
│  [← Back]  Gallery (12 photos)  [Date]  │  ← 顶部导航栏 (40px)
├─────────────────────────────────────────┤
│ ┌───────┐ ┌───────┐ ┌───────┐          │
│ │       │ │       │ │       │          │  ← 缩略图 180x135
│ │ IMG1  │ │ IMG2  │ │ IMG3  │          │     3 列 × 2 行可见
│ │       │ │       │ │       │          │     = 6 个同时可见
│ ├───────┤ ├───────┤ ├───────┤          │
│ │05-24  │ │05-24  │ │05-24  │          │  ← 日期标签
│ └───────┘ └───────┘ └───────┘          │
│                                         │
│ ┌───────┐ ┌───────┐ ┌───────┐          │
│ │       │ │       │ │       │          │
│ │ IMG4  │ │ IMG5  │ │ IMG6  │          │
│ │       │ │       │ │       │          │
│ ├───────┤ ├───────┤ ├───────┤          │
│ │05-23  │ │05-23  │ │05-23  │          │
│ └───────┘ └───────┘ └───────┘          │
│                                         │
│  ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─   │  ← 日期分隔线
│                                         │
│        ... 更多缩略图 (可滚动) ...       │
└─────────────────────────────────────────┘
```

### 4.2 全屏查看视图（800x480 全屏）

```
┌─────────────────────────────────────────┐
│ [← Back]  IMG_20260524_143025.jpg       │  ← 顶部信息栏
│            640x480  (45 KB)              │     文件名/尺寸/文件大小
├─────────────────────────────────────────┤
│                                         │
│                                         │
│          ┌─────────────────┐            │
│          │                 │            │  ← 照片居中显示
│          │   全屏照片       │            │     等比缩放填满可用空间
│          │   (最大 660x350) │            │
│          │                 │            │
│          └─────────────────┘            │
│                                         │
├─────────────────────────────────────────┤
│  [◀ Prev]     [🗑 Delete]     [Next ▶] │  ← 底部操作栏
└─────────────────────────────────────────┘
```

### 4.3 删除确认弹窗

```
     ┌──────────────────────┐
     │  Delete this photo?   │
     │                       │
     │   [Cancel]  [Delete]  │
     └──────────────────────┘
```

---

## 五、类设计

### 5.1 PhotoGallery 类 (新增 `include/display/gallery.h`)

```cpp
class PhotoGallery : public QWidget {
    Q_OBJECT

public:
    explicit PhotoGallery(StorageManager* storage, QWidget* parent = nullptr);
    ~PhotoGallery() override;

    /**
     * @brief 刷新照片列表（从 StorageManager 重新拉取）
     */
    void refresh();

    /**
     * @brief 外部要求退出相册模式
     * @signature: 发射 backToLive() 信号
     */
    void goBack();

signals:
    /// 用户点击返回，通知主程序切换回实时预览
    void backToLive();

private slots:
    void onThumbnailClicked(int index);    // 缩略图被点击 → 进入全屏
    void onPrevPhoto();                    // 上一张
    void onNextPhoto();                    // 下一张
    void onDeletePhoto();                  // 删除当前照片

private:
    // ---- UI 构建 ----
    void buildGalleryView();               // 缩略图网格
    void buildFullscreenView();            // 全屏查看器
    void loadThumbnails();                 // 懒加载可见缩略图

    // ---- 辅助 ----
    QPixmap createThumbnail(const std::string& jpegPath, int w, int h);
    void showFullscreen(int photoIndex);
    void updateFullscreenDisplay();
    bool eventFilter(QObject* obj, QEvent* event) override;  // 触摸滑动

    // ---- 数据 ----
    StorageManager*           m_storage;
    std::vector<PhotoDayGroup> m_groups;        // 按日期分组
    std::vector<PhotoInfo>     m_allPhotos;      // 扁平列表（方便翻页）
    int                        m_currentIndex;    // 当前全屏显示的照片索引

    // ---- 网格视图控件 ----
    QWidget*      m_galleryView;
    QScrollArea*  m_scrollArea;
    QGridLayout*  m_gridLayout;
    std::vector<QPushButton*> m_thumbBtns;  // 缩略图按钮池（复用）

    // ---- 全屏视图控件 ----
    QWidget*      m_fullscreenView;
    QLabel*       m_fullPhotoDisplay;
    QLabel*       m_fullInfoLabel;     // 文件名、尺寸
    QPushButton*  m_btnPrev;
    QPushButton*  m_btnNext;
    QPushButton*  m_btnDelete;
    QPushButton*  m_btnBack;

    // ---- 当前视图 ----
    QStackedWidget* m_stack;  // index 0 = gallery, 1 = fullscreen
};
```

### 5.2 CameraGUI 修改 (修改 `include/display/gui.h`)

```cpp
class CameraGUI : public QWidget {
    // ... 现有成员 ...

    // 新 增
    void showGallery();                     // 切换到相册
    void showLivePreview();                 // 切换回实时预览
    void onBackFromGallery();               // 用户从相册返回

private:
    // 新 增
    QPushButton*   m_btnGallery   = nullptr;   // 相册按钮
    QStackedWidget* m_mainStack    = nullptr;  // 主栈：index 0=实时, 1=相册
    PhotoGallery*   m_gallery      = nullptr;  // 相册组件
};
```

### 5.3 布局调整

当前布局是 `QVBoxLayout` → 视频区 + 状态栏 + 按钮栏 + 设置栏。

修改为：
```
QVBoxLayout
├── QStackedWidget (m_mainStack)       ← 新增顶层容器
│   ├── [0] 原视频区 (QWidget)
│   │       └── 原来的 m_videoDisplay
│   └── [1] PhotoGallery (QWidget)
├── 状态栏 (不变)
├── 按钮栏 (新增 Gallery 按钮)
└── 设置栏 (不变)
```

---

## 六、文件变更清单

| 文件 | 变更类型 | 说明 |
|------|----------|------|
| `include/storage/manager.h` | 修改 | 新增 `PhotoInfo`/`PhotoDayGroup` 结构体 + `listPhotos()`/`deletePhoto()`/`readJpegSize()` |
| `src/storage/manager.cpp` | 修改 | 实现上述新方法 (~250行) |
| `include/display/gallery.h` | **新增** | `PhotoGallery` 类声明 |
| `src/display/gallery.cpp` | **新增** | `PhotoGallery` 类实现 (~500行) |
| `include/display/gui.h` | 修改 | 新增 `m_btnGallery`、`m_mainStack`、`m_gallery`、`showGallery()` 等 |
| `src/display/gui.cpp` | 修改 | 集成相册按钮 + QStackedWidget 布局调整 (~80行) |
| `src/main.cpp` | 修改 | 将 `g_storage` 指针传给 `PhotoGallery` (~5行) |
| `CMakeLists.txt` | 修改 | 新增 `gallery.cpp` / `gallery.h` 到 DISPLAY_SOURCES |
| `docs/09-gallery-module-implementation.md` | **新增** | 相册模块实现文档 |

**预估总代码增量：~850 行**

---

## 七、内存与性能估算 (iMX6ULL)

### 7.1 缩略图内存

| 项目 | 计算 | 结果 |
|------|------|------|
| 单张缩略图 | 180×135×3 (RGB24) | ~73 KB |
| 同时可见缩略图 | 6 张 (3列×2行) | ~440 KB |
| 预加载（上/下各一屏） | 6×3 | ~1.3 MB |
| 全屏照片 | 800×480×3 | ~1.1 MB |
| **峰值内存** | 1.3 + 1.1 | **~2.4 MB** |

### 7.2 性能

| 操作 | 估算耗时 | 说明 |
|------|----------|------|
| 列出 100 张照片 | ~50ms | 只读目录项 + stat，不读 JPEG 内容 |
| 创建单张缩略图 | ~15ms | libjpeg 解码 + Qt 缩放 |
| 首次加载 6 张缩略图 | ~100ms | 6×15ms，异步加载不阻塞 UI |
| 全屏照片渲染 | ~30ms | libjpeg 解码 640×480 → 缩放 → QPixmap |

**在 Cortex-A7 @ 792MHz 上可接受，不会造成明显卡顿。**

### 7.3 优化策略

1. **懒加载**：只创建 6 个缩略图按钮池，滚动时复用（类似 TableView 的 cell reuse）
2. **异步加载**：`QTimer::singleShot(0, ...)` 延迟单个缩略图加载，不阻塞 UI 线程
3. **缩放缓存**：全屏模式下缓存前/后各 1 张的缩放 QPixmap，避免重复解码
4. **仅列表模式不读 JPEG**：`listPhotos(cached=true)` 只做目录遍历，不解析文件头

---

## 八、实现步骤（顺序）

### Step 1: StorageManager 扩展 (1-2小时)
- 实现 `listPhotos()` — 遍历 `m_photoDir`，按日期分组
- 实现 `readJpegSize()` — 快速读取 JPEG 宽高
- 实现 `deletePhoto()` — 删除文件 + 清理空目录
- 编译测试

### Step 2: PhotoGallery 类 (2-3小时)
- 骨架 + CMake 集成
- 缩略图网格视图 (`buildGalleryView` + `loadThumbnails`)
- 全屏查看视图 (`buildFullscreenView`)  
- 翻页/删除/返回逻辑
- 触摸事件处理 (eventFilter)
- PC 端 Mock 测试

### Step 3: CameraGUI 集成 (1小时)
- QStackedWidget 布局调整
- 新增 Gallery 按钮 + `showGallery()` / `showLivePreview()`
- 主程序传递 `StorageManager*` 给 `PhotoGallery`
- 按钮状态联动（相册模式下隐藏 Capture/Record 按钮）

### Step 4: 测试与文档 (1小时)
- 真实照片文件测试（拷贝几张大/小 JPEG 到 `/data/photos`）
- 删除功能验证
- 空目录、大量文件边界测试
- 编写 `docs/09-gallery-module-implementation.md`

---

## 九、注意事项

1. **线程安全**：`listPhotos()` 和 `deletePhoto()` 只被 UI 线程调用，不加锁。`savePhoto()` 仍从采集线程调用，已有的文件写入是原子操作。

2. **空相册处理**：无照片时显示 `"No photos yet"` 占位文字 + 拍照提示。

3. **换行符安全**：目录路径使用 `/`，代码中不硬编码路径分隔符。

4. **JPEG 解码容错**：缩略图加载失败时显示占位图标（灰色方块 + "?"），不崩溃。

5. **触摸友好**：按钮最小 60×40px，间距 ≥ 8px。

6. **保持 800x480 适配**：所有新增 UI 都需适配 7寸屏分辨率。

---

## 十、可扩展性预留

| 扩展点 | 预留方式 |
|--------|----------|
| 视频回放 | `PhotoDayGroup` 可扩展 `type` 字段区分 photo/video |
| 幻灯片播放 | 全屏视图已有 `QTimer` 插入点 |
| 多选操作 | `m_thumbBtns` 已有独立索引，可加 checkable 属性 |
| USB 导出 | `deletePhoto()` 可扩展为 `movePhoto(dst)` |
| 远程相册 API | `listPhotos()` 返回结构化数据，可直接序列化为 JSON |
