/**
 * @file    video_player.cpp
 * @brief   VideoPlayer 实现 — 轻量 AVI/MJPEG 播放器 (v0.4)
 */

#include "include/display/video_player.h"

#include <QDebug>
#include <QPixmap>
#include <QFont>
#include <QSizePolicy>
#include <cstring>
#include <algorithm>

#ifdef HAS_LIBJPEG
#include <jpeglib.h>
#endif

// ============================================================
// 构造 / 析构
// ============================================================

VideoPlayer::VideoPlayer(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &VideoPlayer::onTimerTick);

    buildUI();
}

VideoPlayer::~VideoPlayer() {
    stop();
}

// ============================================================
// UI 构建
// ============================================================

void VideoPlayer::buildUI() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ---- 视频显示区 ----
    m_videoLabel = new QLabel(this);
    m_videoLabel->setAlignment(Qt::AlignCenter);
    m_videoLabel->setStyleSheet(
        "background-color: #0a0a1a; border: none; color: #4a4a6a;"
    );
    m_videoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_videoLabel->setMinimumSize(320, 240);
    m_videoLabel->setText("No video loaded");
    mainLayout->addWidget(m_videoLabel, 1);

    // ---- 控制栏 ----
    auto* ctrlBar = new QHBoxLayout();
    ctrlBar->setContentsMargins(8, 2, 8, 6);
    ctrlBar->setSpacing(8);

    // 播放/暂停按钮
    m_btnPlayPause = new QPushButton("▶", this);
    m_btnPlayPause->setFixedSize(36, 36);
    m_btnPlayPause->setStyleSheet(
        "QPushButton {"
        "  font-size: 16px; font-weight: bold;"
        "  background-color: #2980b9; color: white;"
        "  border: 2px solid #5aa9e6; border-radius: 18px;"
        "}"
        "QPushButton:pressed { background-color: #1c6ea4; }");
    connect(m_btnPlayPause, &QPushButton::clicked, this, [this]() {
        if (m_playing) pause();
        else           play();
    });

    // 帧信息标签
    m_frameInfo = new QLabel("0 / 0", this);
    m_frameInfo->setStyleSheet(
        "font-size: 12px; color: #7f8c8d; padding: 2px 8px;");

    // 进度条
    m_progressBar = new QSlider(Qt::Horizontal, this);
    m_progressBar->setRange(0, 0);
    m_progressBar->setValue(0);
    m_progressBar->setStyleSheet(
        "QSlider::groove:horizontal {"
        "  background: #16213e; height: 6px; border-radius: 3px;"
        "}"
        "QSlider::handle:horizontal {"
        "  background: #2ecc71; width: 14px; height: 14px;"
        "  margin: -4px 0; border-radius: 7px;"
        "}"
        "QSlider::sub-page:horizontal {"
        "  background: #2ecc71; border-radius: 3px;"
        "}");

    // 拖拽进度条时允许 seek
    connect(m_progressBar, &QSlider::sliderPressed, this, [this]() {
        m_sliderDragging = true;
        m_timer->stop();
    });
    connect(m_progressBar, &QSlider::sliderReleased, this, [this]() {
        int targetFrame = m_progressBar->value();
        if (seekToFrame(targetFrame)) {
            m_currentFrame = targetFrame;
            std::vector<uint8_t> jpegData;
            if (readFrameJpeg(jpegData)) {
                decodeAndDisplay(jpegData);
            }
        }
        m_sliderDragging = false;
        if (m_playing) {
            m_timer->start(static_cast<int>(1000.0 / m_fps));
        }
    });
    connect(m_progressBar, &QSlider::valueChanged, this, [this](int val) {
        if (m_sliderDragging) {
            int mins = static_cast<int>(val / m_fps / 60);
            int secs = static_cast<int>(val / m_fps) % 60;
            m_frameInfo->setText(
                QString("%1 / %2  %3:%4")
                    .arg(val + 1).arg(m_totalFrames)
                    .arg(mins, 2, 10, QChar('0'))
                    .arg(secs, 2, 10, QChar('0')));
        }
    });

    ctrlBar->addWidget(m_btnPlayPause);
    ctrlBar->addWidget(m_frameInfo);
    ctrlBar->addWidget(m_progressBar, 1);

    mainLayout->addLayout(ctrlBar);
}

// ============================================================
// 公开 API
// ============================================================

bool VideoPlayer::loadVideo(const std::string& aviPath) {
    stop();

    m_filePath = aviPath;
    m_file = fopen(aviPath.c_str(), "rb");
    if (!m_file) {
        emit playbackError(QString("Cannot open: %1")
                           .arg(QString::fromStdString(aviPath)));
        return false;
    }

    if (!parseAviHeader()) {
        fclose(m_file);
        m_file = nullptr;
        m_filePath.clear();
        emit playbackError("Failed to parse AVI header");
        return false;
    }

    m_currentFrame = 0;
    m_progressBar->setRange(0, m_totalFrames > 0 ? m_totalFrames - 1 : 0);
    m_progressBar->setValue(0);

    int totalSecs = static_cast<int>(duration());
    m_frameInfo->setText(
        QString("1 / %1  %2:%3")
            .arg(m_totalFrames)
            .arg(totalSecs / 60, 2, 10, QChar('0'))
            .arg(totalSecs % 60, 2, 10, QChar('0')));

    // 显示第一帧
    std::vector<uint8_t> jpegData;
    if (readFrameJpeg(jpegData)) {
        decodeAndDisplay(jpegData);
    }

    return true;
}

void VideoPlayer::play() {
    if (!m_file || m_totalFrames <= 0) return;

    if (m_currentFrame >= m_totalFrames - 1) {
        // 已到末尾，从头开始
        m_currentFrame = 0;
        if (!seekToFrame(0)) return;
    }

    m_playing = true;
    m_btnPlayPause->setText("⏸");
    m_btnPlayPause->setStyleSheet(
        "QPushButton {"
        "  font-size: 16px; font-weight: bold;"
        "  background-color: #f39c12; color: white;"
        "  border: 2px solid #f1c40f; border-radius: 18px;"
        "}"
        "QPushButton:pressed { background-color: #d68910; }");

    int interval = static_cast<int>(1000.0 / m_fps);
    if (interval < 1) interval = 1;
    m_timer->start(interval);
}

void VideoPlayer::pause() {
    m_playing = false;
    m_timer->stop();

    m_btnPlayPause->setText("▶");
    m_btnPlayPause->setStyleSheet(
        "QPushButton {"
        "  font-size: 16px; font-weight: bold;"
        "  background-color: #2980b9; color: white;"
        "  border: 2px solid #5aa9e6; border-radius: 18px;"
        "}"
        "QPushButton:pressed { background-color: #1c6ea4; }");
}

void VideoPlayer::stop() {
    m_playing = false;
    m_timer->stop();

    if (m_file) {
        fclose(m_file);
        m_file = nullptr;
    }
    m_filePath.clear();
    m_index.clear();
    m_currentFrame = 0;
    m_totalFrames = 0;

    m_videoLabel->setText("No video loaded");
    m_progressBar->setRange(0, 0);
    m_progressBar->setValue(0);
    m_frameInfo->setText("0 / 0");

    m_btnPlayPause->setText("▶");
    m_btnPlayPause->setStyleSheet(
        "QPushButton {"
        "  font-size: 16px; font-weight: bold;"
        "  background-color: #2980b9; color: white;"
        "  border: 2px solid #5aa9e6; border-radius: 18px;"
        "}"
        "QPushButton:pressed { background-color: #1c6ea4; }");
}

// ============================================================
// 定时器回调 — 播放下一帧
// ============================================================

void VideoPlayer::onTimerTick() {
    if (!m_file || !m_playing) return;

    m_currentFrame++;
    if (m_currentFrame >= m_totalFrames) {
        m_currentFrame = m_totalFrames - 1;
        pause();
        emit playbackFinished();
        return;
    }

    std::vector<uint8_t> jpegData;
    if (!readFrameJpeg(jpegData)) {
        emit playbackError("Failed to read video frame");
        pause();
        return;
    }

    decodeAndDisplay(jpegData);

    // 更新进度条
    if (!m_sliderDragging) {
        m_progressBar->blockSignals(true);
        m_progressBar->setValue(m_currentFrame);
        m_progressBar->blockSignals(false);
    }

    int frameIdx = m_currentFrame + 1;  // 1-based for display
    int totalSecs = static_cast<int>(duration());
    int currentSecs = static_cast<int>(m_currentFrame / m_fps);
    int remainSecs = totalSecs - currentSecs;
    m_frameInfo->setText(
        QString("%1 / %2  %3:%4")
            .arg(frameIdx).arg(m_totalFrames)
            .arg(remainSecs / 60, 2, 10, QChar('0'))
            .arg(remainSecs % 60, 2, 10, QChar('0')));
}

// ============================================================
// AVI 解析
// ============================================================

bool VideoPlayer::parseAviHeader() {
    if (!m_file) return false;

    rewind(m_file);

    // ---- RIFF 文件头 ----
    uint8_t riffHeader[12];
    if (fread(riffHeader, 1, 12, m_file) != 12) return false;
    if (memcmp(riffHeader, "RIFF", 4) != 0 || memcmp(riffHeader + 8, "AVI ", 4) != 0) {
        qWarning() << "[VideoPlayer] Not a valid AVI file";
        return false;
    }

    // ---- 解析 hdrl LIST → 提取 avih 元数据 ----
    uint8_t chunkId[4];
    uint32_t chunkSize;
    uint8_t listType[4];

    // 读取 LIST (hdrl)
    if (fread(chunkId, 1, 4, m_file) != 4) return false;
    if (memcmp(chunkId, "LIST", 4) != 0) return false;
    if (fread(&chunkSize, 4, 1, m_file) != 1) return false;
    if (fread(listType, 1, 4, m_file) != 4) return false;

    if (memcmp(listType, "hdrl", 4) != 0) {
        qWarning() << "[VideoPlayer] Expected hdrl LIST, got" << QByteArray(reinterpret_cast<char*>(listType), 4);
        return false;
    }

    // 在 hdrl 数据中查找 avih 块
    // hdrl 数据范围: 当前位置到 当前位置 + chunkSize - 4
    long hdrlStart = ftell(m_file);
    long hdrlEnd   = hdrlStart + chunkSize - 4;  // -4: "hdrl" 已计入 chunkSize

    bool foundAvih = false;
    while (ftell(m_file) < hdrlEnd - 8) {
        if (fread(chunkId, 1, 4, m_file) != 4) break;
        if (fread(&chunkSize, 4, 1, m_file) != 1) break;

        if (memcmp(chunkId, "avih", 4) == 0) {
            // 读取 AviMainHeader
            AviMainHeader avih;
            if (fread(&avih, sizeof(avih), 1, m_file) != 1) break;

            m_width        = static_cast<int>(avih.dwWidth);
            m_height       = static_cast<int>(avih.dwHeight);
            m_totalFrames  = static_cast<int>(avih.dwTotalFrames);
            m_fps          = avih.dwMicroSecPerFrame > 0
                               ? 1000000.0 / avih.dwMicroSecPerFrame
                               : 30.0;

            foundAvih = true;
            break;
        } else {
            // 跳过该块
            if (chunkSize > 0) fseek(m_file, chunkSize, SEEK_CUR);
        }
    }

    if (!foundAvih) {
        qWarning() << "[VideoPlayer] avih chunk not found";
        return false;
    }

    // ---- 跳过 hdrl 剩余部分 → 定位 movi ----
    fseek(m_file, hdrlEnd, SEEK_SET);

    // 读取 LIST (movi)
    if (fread(chunkId, 1, 4, m_file) != 4) return false;
    if (memcmp(chunkId, "LIST", 4) != 0) return false;
    if (fread(&chunkSize, 4, 1, m_file) != 1) return false;
    if (fread(listType, 1, 4, m_file) != 4) return false;

    if (memcmp(listType, "movi", 4) != 0) {
        qWarning() << "[VideoPlayer] Expected movi LIST, got" << QByteArray(reinterpret_cast<char*>(listType), 4);
        return false;
    }

    // movi 数据区起始偏移（第一个 "00dc" 的 FOURCC 位置）
    m_moviDataOffset = ftell(m_file);

    // ---- 跳过 movi 数据 → 解析 idx1 索引表 ----
    // chunkSize 包含 "movi" (4 字节) + 全部帧数据
    if (fseek(m_file, m_moviDataOffset + chunkSize - 4, SEEK_SET) != 0) return false;

    // 读取 idx1
    if (fread(chunkId, 1, 4, m_file) != 4) return false;
    if (memcmp(chunkId, "idx1", 4) != 0) {
        qWarning() << "[VideoPlayer] idx1 chunk not found";
        return false;
    }

    uint32_t idx1Size;
    if (fread(&idx1Size, 4, 1, m_file) != 1) return false;

    size_t numEntries = idx1Size / sizeof(AviIndexEntry);
    m_index.resize(numEntries);

    if (fread(m_index.data(), sizeof(AviIndexEntry),
              numEntries, m_file) != numEntries) {
        m_index.clear();
        return false;
    }

    // 若 avih.dwTotalFrames 为 0（录制中途异常退出），以 idx1 条目数为准
    if (m_totalFrames <= 0) {
        m_totalFrames = static_cast<int>(numEntries);
    }

    qDebug() << "[VideoPlayer] AVI parsed:" << m_width << "x" << m_height
             << "@" << m_fps << "fps," << m_totalFrames << "frames";

    return true;
}

bool VideoPlayer::seekToFrame(int frameIndex) {
    if (!m_file || frameIndex < 0 || frameIndex >= static_cast<int>(m_index.size()))
        return false;

    const auto& entry = m_index[static_cast<size_t>(frameIndex)];
    long framePos = m_moviDataOffset + static_cast<long>(entry.dwChunkOffset);

    return fseek(m_file, framePos, SEEK_SET) == 0;
}

bool VideoPlayer::readFrameJpeg(std::vector<uint8_t>& jpegData) {
    if (!m_file || m_currentFrame < 0 ||
        m_currentFrame >= static_cast<int>(m_index.size()))
        return false;

    if (!seekToFrame(m_currentFrame)) return false;

    // 读取 "00dc" FOURCC
    uint8_t chunkId[4];
    if (fread(chunkId, 1, 4, m_file) != 4) return false;
    if (memcmp(chunkId, "00dc", 4) != 0) return false;

    // 读取数据大小（JPEG 图像数据的大小，不含 chunk 头）
    uint32_t dataSize;
    if (fread(&dataSize, 4, 1, m_file) != 1) return false;

    if (dataSize == 0 || dataSize > 100 * 1024 * 1024) return false;  // 最大 100MB

    jpegData.resize(dataSize);
    if (fread(jpegData.data(), 1, dataSize, m_file) != dataSize)
        return false;

    return true;
}

void VideoPlayer::decodeAndDisplay(const std::vector<uint8_t>& jpegData) {
    if (jpegData.empty()) return;

    QImage img;

#ifdef HAS_LIBJPEG
    // ---- libjpeg-turbo 解码 ----
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpegData.data(), jpegData.size());

    if (jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK) {
        // 使用缩放解码加速
        int scaleDenom = 1;
        int maxDim = std::max(m_videoLabel->width(), m_videoLabel->height());
        if (maxDim > 0) {
            if (cinfo.image_width > static_cast<JDIMENSION>(maxDim * 4))
                scaleDenom = 4;
            else if (cinfo.image_width > static_cast<JDIMENSION>(maxDim * 2))
                scaleDenom = 2;
        }
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

        img = QImage(rgb.data(), w, h, w * 3, QImage::Format_RGB888).copy();
    } else {
        jpeg_destroy_decompress(&cinfo);
    }
#else
    // Qt 退路
    img.loadFromData(jpegData.data(), static_cast<int>(jpegData.size()), "JPEG");
#endif

    if (img.isNull()) {
        m_videoLabel->setText("Decode error");
        return;
    }

    // 等比缩放到显示区域
    QPixmap pix = QPixmap::fromImage(img.scaled(
        m_videoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_videoLabel->setPixmap(pix);
}
