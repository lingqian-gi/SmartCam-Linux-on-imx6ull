#include "include/display/gui.h"
#include <QApplication>
#include <QScreen>
#include <QPainter>
#include <QImage>
#include <QFont>
#include <QDateTime>
#include <QStackedWidget>
#include <QScrollArea>
#include <QDebug>
#include <cmath>
#include <cstring>
#include <algorithm>

// ============================================================
// libjpeg-turbo 解码（自定义错误处理器，静默坏帧）
// ============================================================
#ifdef HAS_LIBJPEG
#include <jpeglib.h>
#include <setjmp.h>

struct jpegErrorMgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void jpegSilentErrorExit(j_common_ptr cinfo) {
    jpegErrorMgr* myerr = reinterpret_cast<jpegErrorMgr*>(cinfo->err);
    longjmp(myerr->setjmp_buffer, 1);
}

static void jpegSilentOutputMessage(j_common_ptr /*cinfo*/) {
    /* 完全静默 — 不输出任何警告 */
}

/**
 * @brief 将 MJPEG/JPEG 数据解码为 RGB24
 * @return true=成功, false=解码失败
 */
static bool decodeMjpegToRgb(const uint8_t* jpeg_data, size_t jpeg_len,
                              std::vector<uint8_t>& rgb, int& out_w, int& out_h) {
    struct jpeg_decompress_struct cinfo;
    jpegErrorMgr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit       = jpegSilentErrorExit;
    jerr.pub.output_message   = jpegSilentOutputMessage;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_len);
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);

    out_w = cinfo.output_width;
    out_h = cinfo.output_height;
    rgb.resize(static_cast<size_t>(out_w * out_h * 3));

    while (cinfo.output_scanline < static_cast<JDIMENSION>(out_h)) {
        JSAMPROW row = rgb.data() + cinfo.output_scanline * out_w * 3;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}
#endif // HAS_LIBJPEG

// ============================================================
// 构造 / 析构
// ============================================================

CameraGUI::CameraGUI(QWidget* parent)
    : QWidget(parent)
{
    setWindowTitle(QStringLiteral("SmartCam Linux"));

    // 适配 7 寸屏 (800x480)，同时也兼容 PC 调试
    // 适配 7寸屏 800x480
    setMinimumSize(800, 480);
    resize(800, 480);

    buildUI();
    connectSignals();

    // 启动刷新定时器 33ms ≈ 30fps
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(33);
    connect(m_refreshTimer, &QTimer::timeout, this, &CameraGUI::refreshFrame);
    m_refreshTimer->start();

    // 默认进入模拟模式
    enterMockMode();

    // 初始状态
    m_labelStreaming->setText(QStringLiteral("IDLE"));
    m_labelStreaming->setStyleSheet("color: gray;");
    m_labelClients->setText(QStringLiteral("Clients: 0"));
    m_labelRecording->setText(QStringLiteral("REC"));
    m_labelRecording->setStyleSheet("color: gray;");
}

CameraGUI::~CameraGUI() = default;

// ============================================================
// UI 构建
// ============================================================

void CameraGUI::buildUI() {
    // --- 整体布局 ---
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(6, 4, 6, 4);
    mainLayout->setSpacing(4);

    // --- (1) 视频预览区 + 相册（QStackedWidget 切换） ---
    m_mainStack = new QStackedWidget(this);
    m_mainStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // 实时预览容器
    m_liveViewContainer = new QWidget(this);
    auto* liveLayout = new QVBoxLayout(m_liveViewContainer);
    liveLayout->setContentsMargins(0, 0, 0, 0);

    m_videoDisplay = new QLabel(m_liveViewContainer);
    m_videoDisplay->setAlignment(Qt::AlignCenter);
    m_videoDisplay->setMinimumSize(640, 360);
    m_videoDisplay->setStyleSheet(
        "background-color: #1a1a2e;"
        "border: 2px solid #0f3460;"
        "border-radius: 4px;"
        "color: #4a4a6a;"
    );
    m_videoDisplay->setText(QStringLiteral("Waiting camera..."));
    m_videoDisplay->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_videoDisplay->setScaledContents(true);
    liveLayout->addWidget(m_videoDisplay);

    m_mainStack->addWidget(m_liveViewContainer);  // index 0

    // 相册（延迟创建，等 setGalleryStorage 绑定 storage）
    m_gallery = new PhotoGallery(nullptr, this);
    connect(m_gallery, &PhotoGallery::backToLive,
            this, &CameraGUI::onBackFromGallery);
    m_mainStack->addWidget(m_gallery);  // index 1

    m_mainStack->setCurrentIndex(0);
    mainLayout->addWidget(m_mainStack, 1);

    // --- (2) 状态栏 ---
    auto* statusLayout = new QHBoxLayout();
    statusLayout->setSpacing(12);

    m_labelFPS = new QLabel(QStringLiteral("FPS: 0.0"), this);
    m_labelStreaming = new QLabel(QStringLiteral("IDLE"), this);
    m_labelClients = new QLabel(QStringLiteral("Clients: 0"), this);
    m_labelRecording = new QLabel(QStringLiteral("REC"), this);

    QString statusStyle = "font-size: 13px; font-weight: bold; padding: 2px 6px;"
                          "background: #16213e; border-radius: 3px; color: #e0e0e0;";
    m_labelFPS->setStyleSheet(statusStyle);
    m_labelStreaming->setStyleSheet(statusStyle);
    m_labelClients->setStyleSheet(statusStyle);
    m_labelRecording->setStyleSheet(statusStyle);

    statusLayout->addWidget(m_labelFPS);
    statusLayout->addWidget(m_labelStreaming);
    statusLayout->addWidget(m_labelClients);
    statusLayout->addWidget(m_labelRecording);
    statusLayout->addStretch();
    mainLayout->addLayout(statusLayout);

    // --- (3) 按钮栏 ---
    auto* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(8);

    QString btnBaseStyle =
        "QPushButton {"
        "  font-size: 14px; font-weight: bold; padding: 8px 16px;"
        "  min-width: 80px;"
        "}";

    m_btnCapture = new QPushButton(QStringLiteral("Capture"), this);
    m_btnRecord  = new QPushButton(QStringLiteral("Record"), this);
    m_btnGallery = new QPushButton(QStringLiteral("Gallery"), this);
    m_btnSettings = new QPushButton(QStringLiteral("Settings"), this);

    // 使用亮色 + 实线边框，确保 linuxfb 下按钮清晰可见
    m_btnCapture->setStyleSheet(btnBaseStyle +
        "QPushButton {"
        "  background-color: #1a6fb5; color: white;"
        "  border: 2px solid #5aa9e6; border-radius: 4px;"
        "}"
        "QPushButton:pressed { background-color: #0d4a7a; }");
    m_btnRecord->setStyleSheet(btnBaseStyle +
        "QPushButton {"
        "  background-color: #8e44ad; color: white;"
        "  border: 2px solid #c084d6; border-radius: 4px;"
        "}"
        "QPushButton:pressed { background-color: #5e3370; }");
    m_btnSettings->setStyleSheet(btnBaseStyle +
        "QPushButton {"
        "  background-color: #2c3e50; color: #ecf0f1;"
        "  border: 2px solid #7f8c8d; border-radius: 4px;"
        "}"
        "QPushButton:pressed { background-color: #1a252f; }");

    m_btnGallery->setStyleSheet(btnBaseStyle +
        "QPushButton {"
        "  background-color: #2c3e50; color: #ecf0f1;"
        "  border: 2px solid #7f8c8d; border-radius: 4px;"
        "}"
        "QPushButton:pressed { background-color: #1a252f; }");

    btnLayout->addWidget(m_btnCapture);
    btnLayout->addWidget(m_btnRecord);
    btnLayout->addWidget(m_btnGallery);
    btnLayout->addWidget(m_btnSettings);
    btnLayout->addStretch();
    mainLayout->addLayout(btnLayout);

    // --- (4) 设置栏（可展开/收起） ---
    m_settingsPanel = new QWidget(this);
    auto* settingsLayout = new QHBoxLayout(m_settingsPanel);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsLayout->setSpacing(12);

    auto* resLabel = new QLabel(QStringLiteral("Res:"), this);
    m_resolutionCombo = new QComboBox(this);
    m_resolutionCombo->addItem(QStringLiteral("640x480"),  QVariant::fromValue(RES_640x480));
    m_resolutionCombo->addItem(QStringLiteral("320x240"),  QVariant::fromValue(RES_320x240));
    m_resolutionCombo->addItem(QStringLiteral("1280x720"), QVariant::fromValue(RES_1280x720));

    auto* fmtLabel = new QLabel(QStringLiteral("Fmt:"), this);
    m_formatCombo = new QComboBox(this);
    m_formatCombo->addItem(QStringLiteral("YUV"),       static_cast<int>(PixelFormat::FMT_YUYV));
    m_formatCombo->addItem(QStringLiteral("MJPEG"),     static_cast<int>(PixelFormat::FMT_MJPEG));

    QString labelStyle = "font-size: 13px; color: #c0c0d0;";
    resLabel->setStyleSheet(labelStyle);
    fmtLabel->setStyleSheet(labelStyle);

    QString comboStyle =
        "QComboBox { font-size: 13px; padding: 4px 8px;"
        "  background: #16213e; color: #e0e0e0; border: 1px solid #0f3460;"
        "  border-radius: 4px; min-width: 120px; }"
        "QComboBox::drop-down { border: none; }"
        "QComboBox QAbstractItemView {"
        "  background: #16213e; color: #e0e0e0;"
        "  selection-background-color: #0f3460; }";

    m_resolutionCombo->setStyleSheet(comboStyle);
    m_formatCombo->setStyleSheet(comboStyle);

    // 存储路径选择
    auto* storageLabel = new QLabel(QStringLiteral("Store:"), this);
    storageLabel->setStyleSheet(labelStyle);

    m_storageCombo = new QComboBox(this);
    m_storageCombo->addItem(QStringLiteral("Temporary (/data)"),     QString("/data"));
    m_storageCombo->addItem(QStringLiteral("Persistent (eMMC)"),    QString("/home/debian/smartcam"));
    m_storageCombo->setStyleSheet(comboStyle);

    settingsLayout->addWidget(resLabel);
    settingsLayout->addWidget(m_resolutionCombo);
    settingsLayout->addSpacing(16);
    settingsLayout->addWidget(fmtLabel);
    settingsLayout->addWidget(m_formatCombo);
    settingsLayout->addSpacing(16);
    settingsLayout->addWidget(storageLabel);
    settingsLayout->addWidget(m_storageCombo);
    settingsLayout->addStretch();
    mainLayout->addWidget(m_settingsPanel);

    // 默认隐藏设置面板
    m_settingsPanel->hide();

    // --- 整体配色 ---
    setStyleSheet("background-color: #0a0a1a;");
}

// ============================================================
// 信号连接
// ============================================================

void CameraGUI::connectSignals() {
    // 按钮 → 内部 slot → 回调/信号
    connect(m_btnCapture, &QPushButton::clicked, this, &CameraGUI::onCapture);
    connect(m_btnRecord,  &QPushButton::clicked, this, &CameraGUI::onRecord);
    connect(m_btnGallery, &QPushButton::clicked, this, &CameraGUI::onGallery);
    connect(m_btnSettings, &QPushButton::clicked, this, &CameraGUI::onSettings);
    connect(m_resolutionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CameraGUI::onResolutionComboChanged);
    connect(m_formatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CameraGUI::onFormatComboChanged);
    connect(m_storageCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CameraGUI::onStorageComboChanged);
}

// ============================================================
// Slots
// ============================================================

void CameraGUI::refreshFrame() {
    if (m_mockMode) {
        // ---- 模拟模式：生成移动彩条 ----
        if (m_mockBuffer.empty()) return;

        int w = m_currentFrame.width;
        int h = m_currentFrame.height;

        // 每帧移动 2 像素，形成流动效果
        int offset = (m_mockFrameIndex * 2) % w;
        m_mockFrameIndex++;

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int srcX = (x + w - offset) % w;
                int si = (y * w + srcX) * 3;
                int di = (y * w + x) * 3;
                m_mockBuffer[di]     = m_currentFrame.data[si];
                m_mockBuffer[di + 1] = m_currentFrame.data[si + 1];
                m_mockBuffer[di + 2] = m_currentFrame.data[si + 2];
            }
        }
        m_currentFrame.data = m_mockBuffer.data();
    }

    // 转换为 QImage 并渲染
    QImage img = frameToQImage(m_currentFrame.data,
                                m_currentFrame.length,
                                m_currentFrame.width,
                                m_currentFrame.height,
                                m_currentFrame.format);
    if (!img.isNull()) {
        m_videoDisplay->setPixmap(QPixmap::fromImage(img));
    }

    // 在模拟模式下，叠加文本信息
    if (m_mockMode) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        QPixmap pix = m_videoDisplay->pixmap(Qt::ReturnByValue);
#else
        const QPixmap* pp = m_videoDisplay->pixmap();
        QPixmap pix = pp ? *pp : QPixmap();
#endif
        if (!pix.isNull()) {
            QPainter p(&pix);
            p.setPen(QColor(255, 255, 255, 180));
            p.setFont(QFont("monospace", 11));
            QString overlay = QString("Mock Mode | Frame: %1 | %2x%3")
                .arg(m_mockFrameIndex)
                .arg(m_currentFrame.width)
                .arg(m_currentFrame.height);
            p.drawText(8, 22, overlay);
            p.end();
            m_videoDisplay->setPixmap(pix);
        }
    }
}

void CameraGUI::onCapture() {
    qDebug() << "[GUI] Capture button clicked";
    emit captureClicked();
    if (m_onCapture) m_onCapture();
}

void CameraGUI::onRecord() {
    bool shouldRecord = !m_isRecording;
    qDebug() << "[GUI] Record request:" << (shouldRecord ? "Start" : "Stop");

    // 先询问回调，回调返回 true 才允许切换状态
    if (m_onRecordToggle) {
        if (!m_onRecordToggle(shouldRecord)) {
            qDebug() << "[GUI] Record denied (format not supported)";
            return;  // 回调拒绝，不切换 UI
        }
    }

    m_isRecording = shouldRecord;

    if (m_isRecording) {
        m_btnRecord->setText(QStringLiteral("Stop"));
        m_btnRecord->setStyleSheet(
            "QPushButton {"
            "  background-color: #c0392b; color: white;"
            "  border: 2px solid #e74c3c; border-radius: 4px;"
            "  font-size: 14px; font-weight: bold; padding: 8px 16px; min-width: 80px;"
            "}");
        m_labelRecording->setStyleSheet(
            "font-size: 13px; font-weight: bold; padding: 2px 6px;"
            "background: #16213e; border-radius: 3px; color: #e74c3c;");
    } else {
        m_btnRecord->setText(QStringLiteral("Record"));
        m_btnRecord->setStyleSheet(
            "QPushButton {"
            "  background-color: #8e44ad; color: white;"
            "  border: 2px solid #c084d6; border-radius: 4px;"
            "  font-size: 14px; font-weight: bold; padding: 8px 16px; min-width: 80px;"
            "}");
        m_labelRecording->setStyleSheet(
            "font-size: 13px; font-weight: bold; padding: 2px 6px;"
            "background: #16213e; border-radius: 3px; color: gray;");
    }

    emit recordToggled(m_isRecording);
}

void CameraGUI::onSettings() {
    bool visible = m_settingsPanel->isVisible();
    m_settingsPanel->setVisible(!visible);
    qDebug() << "[GUI] Settings panel" << (visible ? "Hide" : "Show");
}

void CameraGUI::onResolutionComboChanged(int index) {
    QVariant data = m_resolutionCombo->itemData(index);
    if (!data.canConvert<Resolution>()) return;

    Resolution res = data.value<Resolution>();
    if (res != Resolution{m_currentFrame.width, m_currentFrame.height}) {
        qDebug() << "[GUI] Resolution changed:" << res.width << "x" << res.height;
        if (m_mockMode) {
            enterMockMode();   // 重新生成模拟帧
        }
        emit resolutionChanged(res.width, res.height);
        if (m_onResolutionChanged) m_onResolutionChanged(res.width, res.height);
    }
}

void CameraGUI::onFormatComboChanged(int index) {
    PixelFormat fmt = static_cast<PixelFormat>(m_formatCombo->itemData(index).toInt());
    qDebug() << "[GUI] Format changed:" << static_cast<int>(fmt);
    emit formatChanged(fmt);
    if (m_onFormatChanged) m_onFormatChanged(fmt);
}

void CameraGUI::onStorageComboChanged(int index) {
    QString path = m_storageCombo->itemData(index).toString();
    qDebug() << "[GUI] Storage path changed:" << path;
    if (m_onStoragePathChanged) m_onStoragePathChanged(path.toStdString());
}

void CameraGUI::onGallery() {
    qDebug() << "[GUI] Gallery button clicked";
    if (m_gallery && m_mainStack->currentIndex() == 0) {
        showGallery();
    }
}

void CameraGUI::onBackFromGallery() {
    qDebug() << "[GUI] Back from gallery";
    showLivePreview();
}

// ============================================================
// 公共接口
// ============================================================

void CameraGUI::setGalleryStorage(StorageManager* storage) {
    if (m_gallery) {
        m_gallery->setParent(nullptr);  // 从栈中移除旧实例
        delete m_gallery;
    }

    m_gallery = new PhotoGallery(storage, this);
    connect(m_gallery, &PhotoGallery::backToLive,
            this, &CameraGUI::onBackFromGallery);
    m_mainStack->insertWidget(1, m_gallery);  // 替换 index 1
}

void CameraGUI::showGallery() {
    if (m_gallery) {
        m_gallery->reset();  // 刷新列表 + 回到网格
        m_mainStack->setCurrentIndex(1);
        // 隐藏实时预览相关的操作按钮
        m_btnCapture->hide();
        m_btnRecord->hide();
        m_settingsPanel->hide();
    }
}

void CameraGUI::showLivePreview() {
    m_mainStack->setCurrentIndex(0);
    m_btnCapture->show();
    m_btnRecord->show();
}

void CameraGUI::setFrame(const uint8_t* data, int len, int w, int h, PixelFormat fmt) {
    if (!data || len <= 0) return;

    // 深拷贝帧数据到内部缓冲区，避免指针悬垂
    // （采集线程的 g_state.frameData 随时可能被下一帧覆盖或 realloc）
    m_frameBuffer.assign(data, data + len);
    m_currentFrame.data   = m_frameBuffer.data();
    m_currentFrame.length = len;
    m_currentFrame.width  = w;
    m_currentFrame.height = h;
    m_currentFrame.format = fmt;
    m_currentFrame.index++;

    m_mockMode = false;
}

void CameraGUI::setFPS(double fps) {
    m_labelFPS->setText(QString("FPS: %1").arg(fps, 0, 'f', 1));
}

void CameraGUI::setClientCount(int count) {
    m_labelClients->setText(QString("Clients: %1").arg(count));
}

void CameraGUI::setRecordingStatus(bool recording) {
    if (recording) {
        m_labelRecording->setStyleSheet(
            "font-size: 13px; font-weight: bold; padding: 2px 6px;"
            "background: #16213e; border-radius: 3px; color: #e74c3c;");
    } else {
        m_labelRecording->setStyleSheet(
            "font-size: 13px; font-weight: bold; padding: 2px 6px;"
            "background: #16213e; border-radius: 3px; color: gray;");
    }
}

void CameraGUI::setStreamingStatus(bool streaming) {
    if (streaming) {
        m_labelStreaming->setText(QStringLiteral("LIVE"));
        m_labelStreaming->setStyleSheet(
            "font-size: 13px; font-weight: bold; padding: 2px 6px;"
            "background: #16213e; border-radius: 3px; color: #2ecc71;");
    } else {
        m_labelStreaming->setText(QStringLiteral("IDLE"));
        m_labelStreaming->setStyleSheet(
            "font-size: 13px; font-weight: bold; padding: 2px 6px;"
            "background: #16213e; border-radius: 3px; color: #e0e0e0;");
    }
}

// ============================================================
// 回调注入
// ============================================================

void CameraGUI::onCaptureRequest(CallbackVoid cb)       { m_onCapture = std::move(cb); }
void CameraGUI::onRecordToggle(std::function<bool(bool)> cb) { m_onRecordToggle = std::move(cb); }
void CameraGUI::onResolutionChanged(CallbackIntInt cb)  { m_onResolutionChanged = std::move(cb); }
void CameraGUI::onFormatChanged(CallbackFormat cb)      { m_onFormatChanged = std::move(cb); }
void CameraGUI::onStoragePathChanged(CallbackString cb) { m_onStoragePathChanged = std::move(cb); }

void CameraGUI::setStoragePath(const std::string& path) {
    if (!m_storageCombo) return;
    // 查找匹配 path 的条目并选中
    for (int i = 0; i < m_storageCombo->count(); ++i) {
        if (m_storageCombo->itemData(i).toString().toStdString() == path) {
            m_storageCombo->setCurrentIndex(i);
            return;
        }
    }
    // 如果没匹配到，添加自定义路径
    m_storageCombo->addItem(QString::fromStdString(path),
                            QString::fromStdString(path));
    m_storageCombo->setCurrentIndex(m_storageCombo->count() - 1);
}

// ============================================================
// Mock 模式 — 生成彩色竖条测试图
// ============================================================

void CameraGUI::enterMockMode() {
    m_mockMode = true;

    Resolution res = RES_640x480;
    QVariant data = m_resolutionCombo->currentData();
    if (data.canConvert<Resolution>()) {
        res = data.value<Resolution>();
    }

    int w = res.width;
    int h = res.height;

    m_currentFrame.width  = w;
    m_currentFrame.height = h;
    m_currentFrame.format = PixelFormat::FMT_RGB24;
    m_currentFrame.length = w * h * 3;

    // 预分配 RGB buffer
    std::vector<uint8_t> buf(w * h * 3);
    m_mockBuffer.resize(w * h * 3);

    // 生成彩色测试条
    static constexpr uint8_t Colors[][3] = {
        {255, 255, 255}, {255, 255, 0}, {0,   255, 255},
        {0,   255, 0},   {255, 0,   255}, {255, 0, 0},
        {0,   0,   255}, {0,   0,   0},
    };
    constexpr int NumColors = sizeof(Colors) / sizeof(Colors[0]);
    int barWidth = w / NumColors;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int colorIdx = (x / barWidth) % NumColors;
            // 纵向渐变效果
            float factor = 0.6f + 0.4f * static_cast<float>(y) / h;
            int idx = (y * w + x) * 3;
            buf[idx]     = static_cast<uint8_t>(Colors[colorIdx][0] * factor);
            buf[idx + 1] = static_cast<uint8_t>(Colors[colorIdx][1] * factor);
            buf[idx + 2] = static_cast<uint8_t>(Colors[colorIdx][2] * factor);
        }
    }

    m_currentFrame.data = buf.data();
    std::memcpy(m_mockBuffer.data(), buf.data(), buf.size());
    m_currentFrame.data = m_mockBuffer.data();

    m_labelFPS->setText(QStringLiteral("FPS: 30.0"));
    m_labelStreaming->setText(QStringLiteral("MOCK"));
    m_labelStreaming->setStyleSheet(
        "font-size: 13px; font-weight: bold; padding: 2px 6px;"
        "background: #16213e; border-radius: 3px; color: #f39c12;");

    qDebug() << "[GUI] Entering Mock mode:" << w << "x" << h;
}

// ============================================================
// 辅助：FrameBuffer → QImage
// ============================================================

QImage CameraGUI::frameToQImage(const uint8_t* data, int len, int w, int h, PixelFormat fmt) {
    if (!data || w <= 0 || h <= 0) return {};

    switch (fmt) {
    case PixelFormat::FMT_RGB24: {
        // 直接构造（深拷贝避免悬空指针）
        return QImage(data, w, h, w * 3, QImage::Format_RGB888).copy();
    }
    case PixelFormat::FMT_RGB565: {
        return QImage(data, w, h, w * 2, QImage::Format_RGB16).copy();
    }
    case PixelFormat::FMT_YUYV: {
        // YUYV → RGB24 → QImage
        std::vector<uint8_t> rgb(w * h * 3);
        yuyv_to_rgb24(data, rgb.data(), w, h);
        return QImage(rgb.data(), w, h, w * 3, QImage::Format_RGB888).copy();
    }
    case PixelFormat::FMT_MJPEG: {
        // MJPEG → JPEG 解码 → QImage
        // 优先用 libjpeg-turbo（自定义错误处理器，不输出坏帧警告）
#ifdef HAS_LIBJPEG
        std::vector<uint8_t> rgb;
        int dw = 0, dh = 0;
        if (decodeMjpegToRgb(data, static_cast<size_t>(len), rgb, dw, dh)) {
            return QImage(rgb.data(), dw, dh, dw * 3, QImage::Format_RGB888).copy();
        }
#else
        // 退路：Qt 内置 JPEG 解码器（可能输出 libjpeg 警告到 stderr）
        QImage img;
        if (img.loadFromData(data, len, "JPEG")) {
            return img.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
#endif
        qWarning() << "[GUI] MJPEG 解码失败，跳过坏帧";
        return {};
    }
    default:
        qWarning() << "[GUI] Unsupported pixel format:" << static_cast<int>(fmt);
        return {};
    }
}
