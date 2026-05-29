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
    setStyleSheet("background-color: #0F1117;");

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
    updateStorageInfo();
}

void PhotoGallery::reset() {
    m_currentIndex = -1;
    // 退出多选模式
    if (m_selectMode) {
        m_selectMode = false;
        m_selectedIndices.clear();
        m_btnSelectToggle->setText("Select");
        m_btnSelectToggle->setStyleSheet(
            "QPushButton { font-size: 13px; font-weight: bold;"
            "  padding: 6px 12px; background: #21262D; color: #E6EDF3;"
            "  border: 2px solid #30363D; border-radius: 4px; }"
            "QPushButton:pressed { background: #161B22; }");
        m_selectToolBar->hide();
    }
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
        "  padding: 6px 14px; background: #21262D; color: #E6EDF3;"
        "  border: 2px solid #30363D; border-radius: 4px; }"
        "QPushButton:pressed { background: #161B22; }");
    btnBack->setFixedHeight(34);
    connect(btnBack, &QPushButton::clicked, this, &PhotoGallery::backToLive);

    m_galleryTitle = new QLabel("Gallery", this);
    m_galleryTitle->setStyleSheet(
        "font-size: 15px; font-weight: bold; color: #E6EDF3;"
        "padding: 2px 10px;");

    m_storageInfoLabel = new QLabel(this);
    m_storageInfoLabel->setStyleSheet(
        "font-size: 12px; color: #484F58; padding: 2px 6px;");

    m_btnSelectToggle = new QPushButton("Select", this);
    m_btnSelectToggle->setStyleSheet(
        "QPushButton { font-size: 13px; font-weight: bold;"
        "  padding: 6px 12px; background: #21262D; color: #E6EDF3;"
        "  border: 2px solid #30363D; border-radius: 4px; }"
        "QPushButton:pressed { background: #161B22; }");
    m_btnSelectToggle->setFixedHeight(34);
    connect(m_btnSelectToggle, &QPushButton::clicked,
            this, &PhotoGallery::onToggleSelectMode);

    topBar->addWidget(btnBack);
    topBar->addWidget(m_galleryTitle);
    topBar->addStretch();
    topBar->addWidget(m_btnSelectToggle);
    topBar->addWidget(m_storageInfoLabel);
    layout->addLayout(topBar);

    // ---- 多选操作工具栏（默认隐藏） ----
    m_selectToolBar = new QWidget(this);
    m_selectToolBar->setStyleSheet("background: #1A1D24; border-radius: 4px;");
    auto* selLayout = new QHBoxLayout(m_selectToolBar);
    selLayout->setContentsMargins(8, 4, 8, 4);
    selLayout->setSpacing(8);

    m_btnSelectAll = new QPushButton("Select All", this);
    m_btnSelectAll->setStyleSheet(
        "QPushButton { font-size: 12px; padding: 4px 10px;"
        "  background: #1F6FEB; color: white; border: 1px solid #58A6FF;"
        "  border-radius: 3px; }"
        "QPushButton:pressed { background: #1c6ea4; }");
    connect(m_btnSelectAll, &QPushButton::clicked,
            this, &PhotoGallery::onSelectAll);

    QPushButton* btnDeselectAll = new QPushButton("Deselect All", this);
    btnDeselectAll->setStyleSheet(
        "QPushButton { font-size: 12px; padding: 4px 10px;"
        "  background: #484F58; color: white; border: 1px solid #484F58;"
        "  border-radius: 3px; }"
        "QPushButton:pressed { background: #6c7a7d; }");
    connect(btnDeselectAll, &QPushButton::clicked,
            this, &PhotoGallery::onDeselectAll);

    m_btnDeleteSelected = new QPushButton("Delete (0)", this);
    m_btnDeleteSelected->setStyleSheet(
        "QPushButton { font-size: 12px; padding: 4px 10px;"
        "  background: #F85149; color: white; border: 1px solid #F85149;"
        "  border-radius: 3px; }"
        "QPushButton:pressed { background: #962d22; }"
        "QPushButton:disabled { background: #5a2d28; color: #7a5a58;"
        "  border-color: #6a3a38; }");
    m_btnDeleteSelected->setEnabled(false);
    connect(m_btnDeleteSelected, &QPushButton::clicked,
            this, &PhotoGallery::onDeleteSelected);

    selLayout->addWidget(m_btnSelectAll);
    selLayout->addWidget(btnDeselectAll);
    selLayout->addStretch();
    selLayout->addWidget(m_btnDeleteSelected);
    m_selectToolBar->hide();
    layout->addWidget(m_selectToolBar);

    // ---- 滚动区域 ----
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setStyleSheet(
        "QScrollArea { border: none; background: #0F1117; }"
        "QScrollBar:vertical { width: 8px; background: #1A1D24; }"
        "QScrollBar::handle:vertical { background: #30363D;"
        "  border-radius: 4px; min-height: 20px; }");

    auto* scrollWidget = new QWidget();
    scrollWidget->setStyleSheet("background: #0F1117;");
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
// 存储空间状态
// ============================================================

void PhotoGallery::updateStorageInfo() {
    if (!m_storage || !m_storageInfoLabel) return;

    int freeMB = m_storage->getFreeSpaceMB();
    int totalMB = m_storage->getTotalSpaceMB();

    if (freeMB < 0 || totalMB < 0) {
        m_storageInfoLabel->setText("Storage: N/A");
        return;
    }

    int usedMB = totalMB - freeMB;
    QString text;

    if (totalMB >= 1024) {
        double totalGB = totalMB / 1024.0;
        double usedGB = usedMB / 1024.0;
        text = QString("Storage: %1 / %2 GB").arg(usedGB, 0, 'f', 1).arg(totalGB, 0, 'f', 1);
    } else {
        text = QString("Storage: %1 / %2 MB").arg(usedMB).arg(totalMB);
    }

    // 空间不足时用红色警告
    double freeRatio = (totalMB > 0) ? static_cast<double>(freeMB) / totalMB : 0.0;
    if (freeRatio < 0.05) {
        m_storageInfoLabel->setStyleSheet(
            "font-size: 12px; color: #F85149; padding: 2px 6px; font-weight: bold;");
        text += "  LOW!";
    } else if (freeRatio < 0.15) {
        m_storageInfoLabel->setStyleSheet(
            "font-size: 12px; color: #f39c12; padding: 2px 6px;");
    } else {
        m_storageInfoLabel->setStyleSheet(
            "font-size: 12px; color: #484F58; padding: 2px 6px;");
    }

    m_storageInfoLabel->setText(text);
}

// ============================================================
// 加载可见缩略图
// ============================================================

void PhotoGallery::clearThumbnails() {
    // 清空网格中所有子控件
    if (!m_gridLayout) return;
    m_checkBoxes.clear();
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
                "font-size: 12px; color: #484F58; padding: 4px 8px;");
            dateLabel->setAlignment(Qt::AlignCenter);
            m_gridLayout->addWidget(dateLabel, row, 0, 1, THUMB_COLS);
            row++;
        }

        // 缩略图按钮
        auto* btn = new QPushButton(this);
        btn->setFixedSize(THUMB_W, THUMB_H);
        btn->setStyleSheet(
            "QPushButton {"
            "  background: #1A1D24;"
            "  border: 2px solid #30363D;"
            "  border-radius: 4px;"
            "  padding: 2px;"
            "}"
            "QPushButton:pressed {"
            "  border-color: #4493F8;"
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
                    " color: #484F58; font-size: 24px;");
            }
        }

        // 点击 → 全屏（用 lambda 捕获索引）
        int idx = static_cast<int>(i);
        connect(btn, &QPushButton::clicked, this, [this, idx]() {
            if (m_selectMode) {
                // 多选模式下点击缩略图切换勾选
                if (m_selectedIndices.contains(idx)) {
                    m_selectedIndices.remove(idx);
                } else {
                    m_selectedIndices.insert(idx);
                }
                // 同步对应 checkbox
                if (idx < m_checkBoxes.size() && m_checkBoxes[idx]) {
                    m_checkBoxes[idx]->setChecked(m_selectedIndices.contains(idx));
                }
                updateDeleteSelectedButton();
                return;
            }
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
            "font-size: 11px; color: #484F58; padding: 2px;");

        // 多选复选框（覆盖在缩略图左上角）
        auto* checkBox = new QCheckBox(this);
        checkBox->setStyleSheet(
            "QCheckBox { spacing: 0; }"
            "QCheckBox::indicator { width: 22px; height: 22px;"
            "  border: 2px solid #30363D; border-radius: 3px;"
            "  background: rgba(10,10,26,180); }"
            "QCheckBox::indicator:checked {"
            "  background: #1F6FEB; border-color: #58A6FF; }");
        checkBox->setChecked(m_selectedIndices.contains(idx));
        checkBox->setVisible(m_selectMode);
        connect(checkBox, &QCheckBox::toggled, this, [this, idx](bool checked) {
            onItemSelectionChanged(idx, checked);
        });
        m_checkBoxes.push_back(checkBox);

        // 按钮 + 复选框 + 标签放入一个容器
        auto* cellWidget = new QWidget(this);
        cellWidget->setStyleSheet("position: relative;");
        auto* cellLayout = new QVBoxLayout(cellWidget);
        cellLayout->setContentsMargins(0, 0, 0, 0);
        cellLayout->setSpacing(2);

        // 缩略图 + 复选框叠放
        auto* thumbStack = new QWidget(this);
        auto* thumbStackLayout = new QVBoxLayout(thumbStack);
        thumbStackLayout->setContentsMargins(0, 0, 0, 0);
        thumbStackLayout->setSpacing(0);
        thumbStackLayout->addWidget(btn, 0, Qt::AlignCenter);

        // 绝对定位复选框在缩略图左上角
        checkBox->setParent(btn);
        checkBox->move(4, 4);
        checkBox->raise();

        cellLayout->addWidget(thumbStack, 0, Qt::AlignCenter);
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
        "  padding: 6px 14px; background: #21262D; color: #E6EDF3;"
        "  border: 2px solid #30363D; border-radius: 4px; }"
        "QPushButton:pressed { background: #161B22; }");
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
    m_fullMediaStack->setStyleSheet("background-color: #0F1117; border: none;");

    m_fullPhotoDisplay = new QLabel(this);
    m_fullPhotoDisplay->setAlignment(Qt::AlignCenter);
    m_fullPhotoDisplay->setStyleSheet(
        "background-color: #0F1117; border: none;");
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
            .arg(border, bg, bg == "#1F6FEB"   ? "#1c6ea4" :
                              bg == "#F85149"   ? "#962d22" :
                              bg == "#27ae60"   ? "#1e8449" : "#161B22");
    };

    m_btnPrev = new QPushButton("\u25C0 Prev", this);
    m_btnPrev->setStyleSheet(btnStyle("#4493F8", "#58A6FF"));
    m_btnPrev->setFixedHeight(36);
    connect(m_btnPrev, &QPushButton::clicked, this, &PhotoGallery::onPrevPhoto);

    m_btnNext = new QPushButton("Next \u25B6", this);
    m_btnNext->setStyleSheet(btnStyle("#4493F8", "#58A6FF"));
    m_btnNext->setFixedHeight(36);
    connect(m_btnNext, &QPushButton::clicked, this, &PhotoGallery::onNextPhoto);

    m_btnDelete = new QPushButton("\u2672 Delete", this);
    m_btnDelete->setStyleSheet(btnStyle("#F85149", "#FF6B61"));
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
                "font-size: 18px; color: #F85149;"
                "background-color: #0F1117; border: none;"
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
                "background-color: #0F1117; border: none;");
        } else {
            m_fullPhotoDisplay->setText("Failed to load image");
            m_fullPhotoDisplay->setStyleSheet(
                "font-size: 18px; color: #F85149;"
                "background-color: #0F1117; border: none;"
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

    QMessageBox msgBox(this);
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setWindowTitle(QString("Delete %1").arg(typeStr));
    msgBox.setText(QString("Delete this %1?").arg(typeStr));
    msgBox.setInformativeText(QString::fromStdString(info.filename));
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Cancel);
    msgBox.setStyleSheet(
        "QMessageBox { background-color: #21262D; }"
        "QMessageBox { border: 2px solid #FF6B61; }"
        "QLabel { color: #E6EDF3; font-size: 14px; }"
        "QPushButton { background-color: #30363D; color: #E6EDF3;"
        "  border: 1px solid #30363D; border-radius: 4px;"
        "  padding: 6px 16px; font-size: 13px; min-width: 80px; }"
        "QPushButton:hover { background-color: #4a6785; }"
        "QPushButton:pressed { background-color: #161B22; }");

    if (msgBox.exec() != QMessageBox::Yes) return;

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
// 多选模式
// ============================================================

void PhotoGallery::onToggleSelectMode() {
    m_selectMode = !m_selectMode;

    if (m_selectMode) {
        // 进入多选模式
        m_btnSelectToggle->setText("Cancel");
        m_btnSelectToggle->setStyleSheet(
            "QPushButton { font-size: 13px; font-weight: bold;"
            "  padding: 6px 12px; background: #F85149; color: #E6EDF3;"
            "  border: 2px solid #FF6B61; border-radius: 4px; }"
            "QPushButton:pressed { background: #962d22; }");
        m_selectToolBar->show();
        m_selectedIndices.clear();
        updateDeleteSelectedButton();
    } else {
        // 退出多选模式
        m_btnSelectToggle->setText("Select");
        m_btnSelectToggle->setStyleSheet(
            "QPushButton { font-size: 13px; font-weight: bold;"
            "  padding: 6px 12px; background: #21262D; color: #E6EDF3;"
            "  border: 2px solid #30363D; border-radius: 4px; }"
            "QPushButton:pressed { background: #161B22; }");
        m_selectToolBar->hide();
        m_selectedIndices.clear();
    }

    // 刷新缩略图以显示/隐藏复选框
    loadVisibleThumbnails();
}

void PhotoGallery::onSelectAll() {
    m_selectedIndices.clear();
    for (int i = 0; i < static_cast<int>(m_flatPhotos.size()); ++i) {
        m_selectedIndices.insert(i);
    }
    // 同步所有 checkbox
    for (int i = 0; i < m_checkBoxes.size() && i < static_cast<int>(m_flatPhotos.size()); ++i) {
        if (m_checkBoxes[i]) m_checkBoxes[i]->setChecked(true);
    }
    updateDeleteSelectedButton();
}

void PhotoGallery::onDeselectAll() {
    m_selectedIndices.clear();
    for (int i = 0; i < m_checkBoxes.size(); ++i) {
        if (m_checkBoxes[i]) m_checkBoxes[i]->setChecked(false);
    }
    updateDeleteSelectedButton();
}

void PhotoGallery::onItemSelectionChanged(int flatIndex, bool checked) {
    if (checked) {
        m_selectedIndices.insert(flatIndex);
    } else {
        m_selectedIndices.remove(flatIndex);
    }
    updateDeleteSelectedButton();
}

void PhotoGallery::updateDeleteSelectedButton() {
    int count = m_selectedIndices.size();
    m_btnDeleteSelected->setText(QString("Delete (%1)").arg(count));
    m_btnDeleteSelected->setEnabled(count > 0);
}

void PhotoGallery::onDeleteSelected() {
    if (m_selectedIndices.isEmpty()) return;

    int count = m_selectedIndices.size();

    QMessageBox msgBox(this);
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setWindowTitle("Delete Selected");
    msgBox.setText(QString("Delete %1 selected item(s)?").arg(count));
    msgBox.setInformativeText("This cannot be undone.");
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Cancel);
    msgBox.setStyleSheet(
        "QMessageBox { background-color: #21262D; }"
        "QMessageBox { border: 2px solid #FF6B61; }"
        "QLabel { color: #E6EDF3; font-size: 14px; }"
        "QPushButton { background-color: #30363D; color: #E6EDF3;"
        "  border: 1px solid #30363D; border-radius: 4px;"
        "  padding: 6px 16px; font-size: 13px; min-width: 80px; }"
        "QPushButton:hover { background-color: #4a6785; }"
        "QPushButton:pressed { background-color: #161B22; }");

    if (msgBox.exec() != QMessageBox::Yes) return;

    // 收集要删除的路径
    QStringList failedItems;
    int deletedCount = 0;

    for (int idx : m_selectedIndices) {
        if (idx < 0 || idx >= static_cast<int>(m_flatPhotos.size())) continue;

        const auto& info = m_flatPhotos[static_cast<size_t>(idx)];
        int result = info.isVideo
            ? m_storage->deleteVideo(info.path)
            : m_storage->deletePhoto(info.path);

        if (result == 0) {
            deletedCount++;
        } else {
            failedItems << QString::fromStdString(info.filename);
        }
    }

    if (!failedItems.isEmpty()) {
        QMessageBox msgBox(this);
        msgBox.setIcon(QMessageBox::Critical);
        msgBox.setWindowTitle("Delete Partially Failed");
        msgBox.setText(QString("Deleted %1 item(s).").arg(deletedCount));
        msgBox.setInformativeText(QString("Failed to delete:\n%1").arg(failedItems.join("\n")));
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.setStyleSheet(
            "QMessageBox { background-color: #21262D; }"
            "QMessageBox { border: 2px solid #e67e22; }"
            "QLabel { color: #E6EDF3; font-size: 14px; }"
            "QPushButton { background-color: #30363D; color: #E6EDF3;"
            "  border: 1px solid #30363D; border-radius: 4px;"
            "  padding: 6px 16px; font-size: 13px; min-width: 80px; }"
            "QPushButton:hover { background-color: #4a6785; }"
            "QPushButton:pressed { background-color: #161B22; }");
        msgBox.exec();
    }

    // 退出多选模式并刷新
    m_selectMode = false;
    m_btnSelectToggle->setText("Select");
    m_btnSelectToggle->setStyleSheet(
        "QPushButton { font-size: 13px; font-weight: bold;"
        "  padding: 6px 12px; background: #21262D; color: #E6EDF3;"
        "  border: 2px solid #30363D; border-radius: 4px; }"
        "QPushButton:pressed { background: #161B22; }");
    m_selectToolBar->hide();
    m_selectedIndices.clear();

    refresh();
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
