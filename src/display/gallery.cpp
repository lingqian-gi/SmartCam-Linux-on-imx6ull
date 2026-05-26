/**
 * @file    gallery.cpp
 * @brief   PhotoGallery 实现 — 相册缩略图浏览 + 全屏查看
 */

#include "include/display/gallery.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QScrollBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QDebug>
#include <QFont>
#include <algorithm>
#include <QSizePolicy>
#include <algorithm>
#include <cmath>

#ifdef HAS_LIBJPEG
#include <jpeglib.h>
#endif

// ============================================================
// 构造 / 析构
// ============================================================

PhotoGallery::PhotoGallery(StorageManager* storage, QWidget* parent)
    : QWidget(parent)
    , m_storage(storage)
{
    setMinimumSize(700, 400);
    setStyleSheet("background-color: #0a0a1a;");

    // ---- 视图栈 ----
    m_stack = new QStackedWidget(this);
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(m_stack);

    buildGalleryView();
    buildFullscreenView();

    m_stack->setCurrentIndex(0);  // 默认网格视图
}

PhotoGallery::~PhotoGallery() = default;

// ============================================================
// 公共接口
// ============================================================

void PhotoGallery::refresh() {
    m_groups.clear();
    m_flatPhotos.clear();

    // 列出照片
    m_storage->listPhotos(m_groups, /*includeInfo=*/true);

    for (const auto& g : m_groups) {
        for (const auto& p : g.photos) {
            m_flatPhotos.push_back(p);
        }
    }

    // 列出视频（合并到同一列表）
    std::vector<StorageManager::PhotoDayGroup> videoGroups;
    m_storage->listVideos(videoGroups);
    for (const auto& g : videoGroups) {
        for (const auto& p : g.photos) {
            m_flatPhotos.push_back(p);
        }
    }

    // 按时间戳倒序重新排序（照片+视频混合）
    std::sort(m_flatPhotos.begin(), m_flatPhotos.end(),
              [](const StorageManager::PhotoInfo& a,
                 const StorageManager::PhotoInfo& b) {
                  return a.timestamp > b.timestamp;
              });

    // 按日期重建分组
    m_groups.clear();
    std::string lastDate;
    for (const auto& p : m_flatPhotos) {
        if (p.dateStr != lastDate) {
            StorageManager::PhotoDayGroup newGroup;
            newGroup.dateStr = p.dateStr;
            m_groups.push_back(newGroup);
            lastDate = p.dateStr;
        }
        m_groups.back().photos.push_back(p);
    }

    loadVisibleThumbnails();
}

void PhotoGallery::reset() {
    m_currentIndex = -1;
    m_stack->setCurrentIndex(0);
    refresh();
}

// ============================================================
// 网格视图
// ============================================================

void PhotoGallery::buildGalleryView() {
    m_galleryView = new QWidget(this);

    auto* layout = new QVBoxLayout(m_galleryView);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    // ---- 顶部标题栏 ----
    auto* topBar = new QHBoxLayout();

    auto* btnBack = new QPushButton("\u2190 Back", this);   // ←
    btnBack->setStyleSheet(
        "QPushButton { font-size: 14px; font-weight: bold;"
        "  padding: 6px 14px; background: #2c3e50; color: #ecf0f1;"
        "  border: 2px solid #5a6c7d; border-radius: 4px; }"
        "QPushButton:pressed { background: #1a252f; }");
    btnBack->setFixedHeight(34);
    connect(btnBack, &QPushButton::clicked, this, &PhotoGallery::backToLive);

    m_galleryTitle = new QLabel("Gallery", this);
    m_galleryTitle->setStyleSheet(
        "font-size: 15px; font-weight: bold; color: #e0e0e0;"
        "padding: 2px 10px;");

    topBar->addWidget(btnBack);
    topBar->addWidget(m_galleryTitle);
    topBar->addStretch();
    layout->addLayout(topBar);

    // ---- 滚动区域 ----
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setStyleSheet(
        "QScrollArea { border: none; background: #0a0a1a; }"
        "QScrollBar:vertical { width: 8px; background: #16213e; }"
        "QScrollBar::handle:vertical { background: #0f3460;"
        "  border-radius: 4px; min-height: 20px; }");

    auto* scrollWidget = new QWidget();
    scrollWidget->setStyleSheet("background: #0a0a1a;");
    m_gridLayout = new QGridLayout(scrollWidget);
    m_gridLayout->setContentsMargins(4, 4, 4, 4);
    m_gridLayout->setSpacing(THUMB_SPACING);
    m_gridLayout->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

    m_scrollArea->setWidget(scrollWidget);
    layout->addWidget(m_scrollArea);

    // ---- 空相册提示 ----
    m_galleryEmpty = new QLabel("No photos yet\nTap Capture to take one!", this);
    m_galleryEmpty->setAlignment(Qt::AlignCenter);
    m_galleryEmpty->setStyleSheet(
        "font-size: 20px; color: #4a4a6a; padding: 80px;");
    m_galleryEmpty->hide();
    layout->addWidget(m_galleryEmpty);

    m_stack->addWidget(m_galleryView);  // index 0
}

// ============================================================
// 缩略图生成
// ============================================================

bool PhotoGallery::createThumbnail(const std::string& jpegPath,
                                    int thumbW, int thumbH,
                                    QPixmap& out) {
#ifdef HAS_LIBJPEG
    FILE* fp = fopen(jpegPath.c_str(), "rb");
    if (!fp) return false;

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);

    // 缩放解码：直接到缩略图大小（节省内存 & CPU）
    int scaleDenom = 1;
    if (cinfo.image_width > thumbW * 8)  scaleDenom = 8;
    else if (cinfo.image_width > thumbW * 4)  scaleDenom = 4;
    else if (cinfo.image_width > thumbW * 2)  scaleDenom = 2;
    cinfo.scale_num   = 1;
    cinfo.scale_denom = scaleDenom;

    jpeg_start_decompress(&cinfo);

    int w = cinfo.output_width;
    int h = cinfo.output_height;
    std::vector<uint8_t> rgb(static_cast<size_t>(w * h * 3));

    while (cinfo.output_scanline < static_cast<JDIMENSION>(h)) {
        JSAMPROW row = rgb.data() + cinfo.output_scanline * w * 3;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);

    QImage img(rgb.data(), w, h, w * 3, QImage::Format_RGB888);
    out = QPixmap::fromImage(img.scaled(thumbW, thumbH,
                                         Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation));
    return !out.isNull();
#else
    // 退路：Qt 内置加载
    QImage img(QString::fromStdString(jpegPath));
    if (img.isNull()) return false;
    out = QPixmap::fromImage(img.scaled(thumbW, thumbH,
                                         Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation));
    return !out.isNull();
#endif
}

bool PhotoGallery::createThumbnailFromJpegData(
        const std::vector<uint8_t>& jpegData,
        int thumbW, int thumbH, QPixmap& out) {
    if (jpegData.empty()) return false;

#ifdef HAS_LIBJPEG
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpegData.data(), jpegData.size());

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    int scaleDenom = 1;
    if (cinfo.image_width > static_cast<JDIMENSION>(thumbW * 8))  scaleDenom = 8;
    else if (cinfo.image_width > static_cast<JDIMENSION>(thumbW * 4))  scaleDenom = 4;
    else if (cinfo.image_width > static_cast<JDIMENSION>(thumbW * 2))  scaleDenom = 2;
    cinfo.scale_num   = 1;
    cinfo.scale_denom = scaleDenom;

    jpeg_start_decompress(&cinfo);

    int w = cinfo.output_width;
    int h = cinfo.output_height;
    std::vector<uint8_t> rgb(static_cast<size_t>(w * h * 3));

    while (cinfo.output_scanline < static_cast<JDIMENSION>(h)) {
        JSAMPROW row = rgb.data() + cinfo.output_scanline * w * 3;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    QImage img(rgb.data(), w, h, w * 3, QImage::Format_RGB888);
    out = QPixmap::fromImage(img.scaled(thumbW, thumbH,
                                         Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation));
    return !out.isNull();
#else
    QImage img;
    img.loadFromData(jpegData.data(),
                     static_cast<int>(jpegData.size()), "JPEG");
    if (img.isNull()) return false;
    out = QPixmap::fromImage(img.scaled(thumbW, thumbH,
                                         Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation));
    return !out.isNull();
#endif
}

bool PhotoGallery::createVideoThumbnail(const std::string& aviPath,
                                         int thumbW, int thumbH,
                                         QPixmap& out) {
    std::vector<uint8_t> jpegData;
    if (!StorageManager::extractAviThumbnail(aviPath, jpegData))
        return false;

    return createThumbnailFromJpegData(jpegData, thumbW, thumbH, out);
}

// ============================================================
// 加载可见缩略图
// ============================================================

void PhotoGallery::clearThumbnails() {
    // 清空网格中所有子控件
    if (!m_gridLayout) return;
    QLayoutItem* item;
    while ((item = m_gridLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }
}

void PhotoGallery::loadVisibleThumbnails() {
    clearThumbnails();

    if (m_flatPhotos.empty()) {
        m_galleryEmpty->show();
        m_scrollArea->hide();
        m_galleryTitle->setText("Gallery (empty)");
        return;
    }

    m_galleryEmpty->hide();
    m_scrollArea->show();

    int photoCount = 0, videoCount = 0;
    for (const auto& p : m_flatPhotos) {
        if (p.isVideo) videoCount++; else photoCount++;
    }
    QString title = QString("Gallery (%1 photos").arg(photoCount);
    if (videoCount > 0) title += QString(", %1 videos").arg(videoCount);
    title += ")";
    m_galleryTitle->setText(title);

    // 按分组渲染
    int row = 0;
    std::string lastDate;

    for (size_t i = 0; i < m_flatPhotos.size(); i++) {
        const auto& info = m_flatPhotos[i];

        // 日期分隔线
        if (info.dateStr != lastDate) {
            lastDate = info.dateStr;
            auto* dateLabel = new QLabel(
                QString("  \u2014\u2014  %1  \u2014\u2014")
                    .arg(QString::fromStdString(info.dateStr)), this);
            dateLabel->setStyleSheet(
                "font-size: 12px; color: #5a6c7d; padding: 4px 8px;");
            dateLabel->setAlignment(Qt::AlignCenter);
            m_gridLayout->addWidget(dateLabel, row, 0, 1, THUMB_COLS);
            row++;
        }

        // 缩略图按钮
        auto* btn = new QPushButton(this);
        btn->setFixedSize(THUMB_W, THUMB_H);
        btn->setStyleSheet(
            "QPushButton {"
            "  background: #16213e;"
            "  border: 2px solid #0f3460;"
            "  border-radius: 4px;"
            "  padding: 2px;"
            "}"
            "QPushButton:pressed {"
            "  border-color: #1a6fb5;"
            "}");

        if (info.isVideo) {
            // 视频：从 AVI 提取第一帧 JPEG 作为缩略图
            QPixmap thumb;
            if (createVideoThumbnail(info.path, THUMB_W, THUMB_H, thumb)) {
                btn->setIcon(QIcon(thumb));
                btn->setIconSize(QSize(THUMB_W, THUMB_H));
            } else {
                // 提取失败，显示 ▶ 占位符
                btn->setText("\u25B6");
                btn->setStyleSheet(btn->styleSheet() +
                    " color: #2ecc71; font-size: 42px;");
            }
        } else {
            // 照片：加载 JPEG 缩略图
            QPixmap thumb;
            if (createThumbnail(info.path, THUMB_W, THUMB_H, thumb)) {
                btn->setIcon(QIcon(thumb));
                btn->setIconSize(QSize(THUMB_W, THUMB_H));
            } else {
                btn->setText("?");
                btn->setStyleSheet(btn->styleSheet() +
                    " color: #5a6c7d; font-size: 24px;");
            }
        }

        // 点击 → 全屏（用 lambda 捕获索引）
        int idx = static_cast<int>(i);
        connect(btn, &QPushButton::clicked, this, [this, idx]() {
            m_currentIndex = idx;
            updateFullscreenDisplay();
            m_stack->setCurrentIndex(1);
        });

        // 信息标签在按钮下
        QString detail;
        if (info.isVideo) {
            detail = QString("%1  [VID]").arg(
                QString::fromStdString(info.timeStr));
        } else {
            detail = QString("%1  %2").arg(
                QString::fromStdString(info.timeStr),
                info.width > 0 && info.height > 0
                    ? QString("%1x%2").arg(info.width).arg(info.height)
                    : "");
        }
        auto* infoLabel = new QLabel(detail, this);
        infoLabel->setAlignment(Qt::AlignCenter);
        infoLabel->setStyleSheet(
            "font-size: 11px; color: #7f8c8d; padding: 2px;");

        // 按钮 + 标签放入一个容器
        auto* cellWidget = new QWidget(this);
        auto* cellLayout = new QVBoxLayout(cellWidget);
        cellLayout->setContentsMargins(0, 0, 0, 0);
        cellLayout->setSpacing(2);
        cellLayout->addWidget(btn, 0, Qt::AlignCenter);
        cellLayout->addWidget(infoLabel, 0, Qt::AlignCenter);

        int col = static_cast<int>(i) % THUMB_COLS;
        m_gridLayout->addWidget(cellWidget, row, col);
        if (col == THUMB_COLS - 1) row++;
    }
}

// ============================================================
// 全屏视图
// ============================================================

void PhotoGallery::buildFullscreenView() {
    m_fullView = new QWidget(this);

    auto* layout = new QVBoxLayout(m_fullView);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ---- 顶部信息栏 ----
    auto* topBar = new QHBoxLayout();
    topBar->setContentsMargins(8, 4, 8, 4);

    m_btnBackGal = new QPushButton("\u2190 Gallery", this);
    m_btnBackGal->setStyleSheet(
        "QPushButton { font-size: 14px; font-weight: bold;"
        "  padding: 6px 14px; background: #2c3e50; color: #ecf0f1;"
        "  border: 2px solid #5a6c7d; border-radius: 4px; }"
        "QPushButton:pressed { background: #1a252f; }");
    m_btnBackGal->setFixedHeight(34);
    connect(m_btnBackGal, &QPushButton::clicked,
            this, &PhotoGallery::onBackToGallery);

    m_fullInfoLabel = new QLabel(this);
    m_fullInfoLabel->setStyleSheet(
        "font-size: 13px; color: #c0c0d0; padding: 2px 10px;");

    topBar->addWidget(m_btnBackGal);
    topBar->addWidget(m_fullInfoLabel);
    topBar->addStretch();
    layout->addLayout(topBar);

    // ---- 媒体显示区（QStackedWidget: [0]照片 / [1]视频播放器） ----
    m_fullMediaStack = new QStackedWidget(this);
    m_fullMediaStack->setStyleSheet("background-color: #0a0a1a; border: none;");

    m_fullPhotoDisplay = new QLabel(this);
    m_fullPhotoDisplay->setAlignment(Qt::AlignCenter);
    m_fullPhotoDisplay->setStyleSheet(
        "background-color: #0a0a1a; border: none;");
    m_fullPhotoDisplay->setScaledContents(false);
    m_fullPhotoDisplay->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_fullMediaStack->addWidget(m_fullPhotoDisplay);  // index 0

    m_videoPlayer = new VideoPlayer(this);
    connect(m_videoPlayer, &VideoPlayer::playbackFinished,
            this, &PhotoGallery::onVideoPlaybackFinished);
    m_fullMediaStack->addWidget(m_videoPlayer);  // index 1

    layout->addWidget(m_fullMediaStack, 1);

    // ---- 底部操作栏 ----
    auto* bottomBar = new QHBoxLayout();
    bottomBar->setContentsMargins(8, 4, 8, 8);
    bottomBar->setSpacing(12);

    auto btnStyle = [](const QString& bg, const QString& border) {
        return QString(
            "QPushButton { font-size: 14px; font-weight: bold;"
            "  padding: 8px 18px; color: white; border-radius: 4px;"
            "  border: 2px solid %1; background-color: %2;"
            "  min-width: 80px; }"
            "QPushButton:pressed { background-color: %3; }")
            .arg(border, bg, bg == "#2980b9"   ? "#1c6ea4" :
                              bg == "#c0392b"   ? "#962d22" :
                              bg == "#27ae60"   ? "#1e8449" : "#1a252f");
    };

    m_btnPrev = new QPushButton("\u25C0 Prev", this);
    m_btnPrev->setStyleSheet(btnStyle("#2980b9", "#5aa9e6"));
    m_btnPrev->setFixedHeight(36);
    connect(m_btnPrev, &QPushButton::clicked, this, &PhotoGallery::onPrevPhoto);

    m_btnNext = new QPushButton("Next \u25B6", this);
    m_btnNext->setStyleSheet(btnStyle("#2980b9", "#5aa9e6"));
    m_btnNext->setFixedHeight(36);
    connect(m_btnNext, &QPushButton::clicked, this, &PhotoGallery::onNextPhoto);

    m_btnDelete = new QPushButton("\u2672 Delete", this);
    m_btnDelete->setStyleSheet(btnStyle("#c0392b", "#e74c3c"));
    m_btnDelete->setFixedHeight(36);
    connect(m_btnDelete, &QPushButton::clicked,
            this, &PhotoGallery::onDeletePhoto);

    bottomBar->addStretch();
    bottomBar->addWidget(m_btnPrev);
    bottomBar->addWidget(m_btnDelete);
    bottomBar->addWidget(m_btnNext);
    bottomBar->addStretch();
    layout->addLayout(bottomBar);

    // ---- 触摸滑动（在全屏视图上安装事件过滤器） ----
    m_fullView->installEventFilter(this);
    m_fullView->setMouseTracking(true);

    m_stack->addWidget(m_fullView);  // index 1
}

// ============================================================
// 全屏视图操作
// ============================================================

void PhotoGallery::updateFullscreenDisplay() {
    // 先停止可能正在播放的视频
    stopVideoPlayback();

    if (m_currentIndex < 0 ||
        m_currentIndex >= static_cast<int>(m_flatPhotos.size()))
        return;

    const auto& info = m_flatPhotos[static_cast<size_t>(m_currentIndex)];

    // 信息栏
    QString sizeStr;
    if (info.fileSize >= 1024 * 1024) {
        sizeStr = QString::number(info.fileSize / (1024.0 * 1024.0), 'f', 1) + " MB";
    } else {
        sizeStr = QString::number(info.fileSize / 1024.0, 'f', 0) + " KB";
    }
    m_fullInfoLabel->setText(
        QString("%1%2  |  %3  |  %4/%5")
            .arg(info.isVideo ? "[VID] " : "")
            .arg(QString::fromStdString(info.filename))
            .arg(sizeStr)
            .arg(m_currentIndex + 1)
            .arg(m_flatPhotos.size()));

    if (info.isVideo) {
        // 视频：加载到 VideoPlayer 并自动播放
        if (m_videoPlayer->loadVideo(info.path)) {
            m_fullMediaStack->setCurrentIndex(1);
            m_videoPlayer->play();
        } else {
            // 加载失败，回退到照片显示区显示占位符
            m_fullPhotoDisplay->setText(
                QString("\u25B6  %1\n\n(%2)\n\nFailed to load video")
                    .arg(QString::fromStdString(info.filename))
                    .arg(sizeStr));
            m_fullPhotoDisplay->setStyleSheet(
                "font-size: 18px; color: #e74c3c;"
                "background-color: #0a0a1a; border: none;"
                "padding: 40px;");
            m_fullMediaStack->setCurrentIndex(0);
        }
    } else {
        // 照片显示
        m_fullMediaStack->setCurrentIndex(0);
        QImage img(QString::fromStdString(info.path));
        if (!img.isNull()) {
            QPixmap pix = QPixmap::fromImage(img.scaled(
                660, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            m_fullPhotoDisplay->setPixmap(pix);
            m_fullPhotoDisplay->setStyleSheet(
                "background-color: #0a0a1a; border: none;");
        } else {
            m_fullPhotoDisplay->setText("Failed to load image");
            m_fullPhotoDisplay->setStyleSheet(
                "font-size: 18px; color: #e74c3c;"
                "background-color: #0a0a1a; border: none;"
                "padding: 40px;");
        }
    }

    // 按钮状态
    m_btnPrev->setEnabled(m_currentIndex > 0);
    m_btnNext->setEnabled(
        m_currentIndex < static_cast<int>(m_flatPhotos.size()) - 1);
}

void PhotoGallery::onPrevPhoto() {
    stopVideoPlayback();
    if (m_currentIndex > 0) {
        m_currentIndex--;
        updateFullscreenDisplay();
    }
}

void PhotoGallery::onNextPhoto() {
    stopVideoPlayback();
    if (m_currentIndex < static_cast<int>(m_flatPhotos.size()) - 1) {
        m_currentIndex++;
        updateFullscreenDisplay();
    }
}

void PhotoGallery::onDeletePhoto() {
    stopVideoPlayback();

    if (m_currentIndex < 0 ||
        m_currentIndex >= static_cast<int>(m_flatPhotos.size()))
        return;

    const auto& info = m_flatPhotos[static_cast<size_t>(m_currentIndex)];

    QString typeStr = info.isVideo ? "video" : "photo";
    auto reply = QMessageBox::question(
        this, QString("Delete %1").arg(typeStr),
        QString("Delete this %1?\n\n%2")
            .arg(typeStr, QString::fromStdString(info.filename)),
        QMessageBox::Cancel | QMessageBox::Yes,
        QMessageBox::Cancel);

    if (reply != QMessageBox::Yes) return;

    int result = info.isVideo
        ? m_storage->deleteVideo(info.path)
        : m_storage->deletePhoto(info.path);

    if (result == 0) {
        // 重新加载列表
        refresh();

        if (m_flatPhotos.empty()) {
            // 全部删完，回到网格
            m_currentIndex = -1;
            m_stack->setCurrentIndex(0);
        } else {
            // 调整索引
            int newIdx = m_currentIndex;
            if (newIdx >= static_cast<int>(m_flatPhotos.size())) {
                newIdx = static_cast<int>(m_flatPhotos.size()) - 1;
            }
            m_currentIndex = newIdx;
            updateFullscreenDisplay();
        }
    }
}

void PhotoGallery::onBackToGallery() {
    stopVideoPlayback();
    m_stack->setCurrentIndex(0);
    loadVisibleThumbnails();  // 可能删了照片，刷新
}

void PhotoGallery::stopVideoPlayback() {
    if (m_videoPlayer && m_videoPlayer->isPlaying()) {
        m_videoPlayer->stop();
    }
}

void PhotoGallery::onVideoPlaybackFinished() {
    // 视频播放到末尾，保持暂停状态（显示最后一帧）
    // 用户可以点击 Prev/Next 切换
    qDebug() << "[Gallery] Video playback finished, index:" << m_currentIndex;
}

// ============================================================
// 触摸滑动
// ============================================================

bool PhotoGallery::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_fullView && m_stack->currentIndex() == 1) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            m_touchStartX = me->x();
            return false;
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            auto* me = static_cast<QMouseEvent*>(event);
            int dx = me->x() - m_touchStartX;
            if (std::abs(dx) > 60) {  // 滑动阈值
                if (dx < 0) onNextPhoto();
                else        onPrevPhoto();
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}
