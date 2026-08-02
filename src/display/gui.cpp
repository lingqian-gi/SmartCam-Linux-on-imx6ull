#include "include/display/gui.h"
#include "include/camera/capture.h"
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

    buildSettingsDialog();

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

    // 相机控制滑块
    connect(m_brightnessSlider, &QSlider::valueChanged, this, &CameraGUI::onBrightnessChanged);
    connect(m_contrastSlider, &QSlider::valueChanged, this, &CameraGUI::onContrastChanged);
    connect(m_autoWbCheckBox, &QCheckBox::stateChanged, this, &CameraGUI::onAutoWbChanged);
    connect(m_wbSlider, &QSlider::valueChanged, this, &CameraGUI::onWbChanged);
    connect(m_autoExposureCheckBox, &QCheckBox::stateChanged, this, &CameraGUI::onAutoExposureChanged);
    connect(m_exposureSlider, &QSlider::valueChanged, this, &CameraGUI::onExposureChanged);
    connect(m_framerateSlider, QOverload<int>::of(&QSlider::valueChanged),
            this, &CameraGUI::onFramerateSliderChanged);
    connect(m_btnResetDefaults, &QPushButton::clicked, this, &CameraGUI::onResetDefaults);
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
    if (m_settingsDialog) {
        m_settingsDialog->exec();
    }
    qDebug() << "[GUI] Settings dialog closed";
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
        // 关闭设置对话框（如果打开的话）
        if (m_settingsDialog && m_settingsDialog->isVisible()) {
            m_settingsDialog->close();
        }
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
void CameraGUI::onCameraControlChanged(CallbackCameraControl cb) { m_onCameraControl = std::move(cb); }
void CameraGUI::onFramerateChanged(CallbackFramerate cb)       { m_onFramerate = std::move(cb); }

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
// 设置面板弹窗（亮度/对比度/白平衡调节）
// ============================================================

void CameraGUI::buildSettingsDialog() {
    m_settingsDialog = new QDialog(this);
    m_settingsDialog->setWindowTitle(QStringLiteral("Settings"));
    m_settingsDialog->setMinimumSize(640, 440);
    m_settingsDialog->setModal(true);

    // 深色主题
    m_settingsDialog->setStyleSheet(
        "QDialog { background-color: #0a0a1a; }"
        "QLabel { color: #e0e0e0; font-size: 13px; }"
        "QGroupBox { color: #e0e0e0; font-size: 14px; font-weight: bold; "
        "  border: 1px solid #0f3460; border-radius: 4px; margin-top: 12px; padding-top: 16px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; }"
    );

    auto* mainLayout = new QVBoxLayout(m_settingsDialog);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(16, 16, 16, 16);

    // ---- 样式定义 ----
    QString comboStyle =
        "QComboBox { font-size: 13px; padding: 4px 8px;"
        "  background: #16213e; color: #e0e0e0; border: 1px solid #0f3460;"
        "  border-radius: 4px; min-width: 160px; }"
        "QComboBox::drop-down { border: none; }"
        "QComboBox QAbstractItemView {"
        "  background: #16213e; color: #e0e0e0;"
        "  selection-background-color: #0f3460; }";

    QString sliderStyle =
        "QSlider::groove:horizontal {"
        "  border: 1px solid #0f3460; height: 6px; background: #16213e;"
        "  border-radius: 3px; }"
        "QSlider::handle:horizontal {"
        "  background: #3498db; border: 1px solid #2980b9; width: 18px;"
        "  margin: -7px 0; border-radius: 9px; }"
        "QSlider::sub-page:horizontal { background: #2980b9; border-radius: 3px; }";

    // ================================================================
    // (1) 视频设置分组
    // ================================================================
    auto* videoGroup = new QGroupBox(QStringLiteral("Video Settings"), m_settingsDialog);
    auto* videoLayout = new QVBoxLayout(videoGroup);
    videoLayout->setSpacing(8);

    // 分辨率
    auto* resRow = new QHBoxLayout();
    auto* resLabel = new QLabel(QStringLiteral("Resolution:"), videoGroup);
    resLabel->setFixedWidth(100);
    m_resolutionCombo = new QComboBox(videoGroup);
    m_resolutionCombo->addItem(QStringLiteral("640x480"),  QVariant::fromValue(RES_640x480));
    m_resolutionCombo->addItem(QStringLiteral("320x240"),  QVariant::fromValue(RES_320x240));
    m_resolutionCombo->addItem(QStringLiteral("1280x720"), QVariant::fromValue(RES_1280x720));
    m_resolutionCombo->setStyleSheet(comboStyle);
    resRow->addWidget(resLabel);
    resRow->addWidget(m_resolutionCombo, 1);
    videoLayout->addLayout(resRow);

    // 格式
    auto* fmtRow = new QHBoxLayout();
    auto* fmtLabel = new QLabel(QStringLiteral("Format:"), videoGroup);
    fmtLabel->setFixedWidth(100);
    m_formatCombo = new QComboBox(videoGroup);
    m_formatCombo->addItem(QStringLiteral("YUYV"),   static_cast<int>(PixelFormat::FMT_YUYV));
    m_formatCombo->addItem(QStringLiteral("MJPEG"),  static_cast<int>(PixelFormat::FMT_MJPEG));
    m_formatCombo->setStyleSheet(comboStyle);
    fmtRow->addWidget(fmtLabel);
    fmtRow->addWidget(m_formatCombo, 1);
    videoLayout->addLayout(fmtRow);

    // 存储路径
    auto* storeRow = new QHBoxLayout();
    auto* storeLabel = new QLabel(QStringLiteral("Storage:"), videoGroup);
    storeLabel->setFixedWidth(100);
    m_storageCombo = new QComboBox(videoGroup);
    m_storageCombo->addItem(QStringLiteral("Temporary (/data)"),     QString("/data"));
    m_storageCombo->addItem(QStringLiteral("Persistent (eMMC)"),    QString("/home/debian/smartcam"));
    m_storageCombo->setStyleSheet(comboStyle);
    storeRow->addWidget(storeLabel);
    storeRow->addWidget(m_storageCombo, 1);
    videoLayout->addLayout(storeRow);

    mainLayout->addWidget(videoGroup);

    // ================================================================
    // (2) 相机控制分组 — 亮度/对比度/白平衡
    // ================================================================
    auto* camGroup = new QGroupBox(QStringLiteral("Camera Controls"), m_settingsDialog);
    auto* camLayout = new QVBoxLayout(camGroup);
    camLayout->setSpacing(8);

    // 亮度
    auto* brightRow = new QHBoxLayout();
    auto* brightLabel = new QLabel(QStringLiteral("Brightness:"), camGroup);
    brightLabel->setFixedWidth(100);
    m_brightnessSlider = new QSlider(Qt::Horizontal, camGroup);
    m_brightnessSlider->setRange(0, 100);
    m_brightnessSlider->setValue(50);
    m_brightnessSlider->setStyleSheet(sliderStyle);
    m_brightnessValue = new QLabel(QStringLiteral("50"), camGroup);
    m_brightnessValue->setFixedWidth(50);
    m_brightnessValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    brightRow->addWidget(brightLabel);
    brightRow->addWidget(m_brightnessSlider, 1);
    brightRow->addWidget(m_brightnessValue);
    camLayout->addLayout(brightRow);

    // 对比度
    auto* contrastRow = new QHBoxLayout();
    auto* contrastLabel = new QLabel(QStringLiteral("Contrast:"), camGroup);
    contrastLabel->setFixedWidth(100);
    m_contrastSlider = new QSlider(Qt::Horizontal, camGroup);
    m_contrastSlider->setRange(0, 100);
    m_contrastSlider->setValue(50);
    m_contrastSlider->setStyleSheet(sliderStyle);
    m_contrastValue = new QLabel(QStringLiteral("50"), camGroup);
    m_contrastValue->setFixedWidth(50);
    m_contrastValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    contrastRow->addWidget(contrastLabel);
    contrastRow->addWidget(m_contrastSlider, 1);
    contrastRow->addWidget(m_contrastValue);
    camLayout->addLayout(contrastRow);

    // 自动白平衡
    auto* autoWbRow = new QHBoxLayout();
    auto* autoWbLabel = new QLabel(QStringLiteral("Auto WB:"), camGroup);
    autoWbLabel->setFixedWidth(100);
    m_autoWbCheckBox = new QCheckBox(QStringLiteral("Auto"), camGroup);
    m_autoWbCheckBox->setChecked(true);
    m_autoWbCheckBox->setStyleSheet(
        "QCheckBox { color: #e0e0e0; font-size: 13px; spacing: 6px; }"
        "QCheckBox::indicator { width: 20px; height: 20px; }"
        "QCheckBox::indicator:unchecked { "
        "  border: 2px solid #7f8c8d; border-radius: 3px; background: #16213e; }"
        "QCheckBox::indicator:checked { "
        "  border: 2px solid #3498db; border-radius: 3px; background: #3498db; }");
    autoWbRow->addWidget(autoWbLabel);
    autoWbRow->addWidget(m_autoWbCheckBox);
    autoWbRow->addStretch();
    camLayout->addLayout(autoWbRow);

    // 白平衡色温
    auto* wbRow = new QHBoxLayout();
    auto* wbLabel = new QLabel(QStringLiteral("WB Temp:"), camGroup);
    wbLabel->setFixedWidth(100);
    m_wbSlider = new QSlider(Qt::Horizontal, camGroup);
    m_wbSlider->setRange(2500, 10000);
    m_wbSlider->setValue(5000);
    m_wbSlider->setEnabled(false);  // 自动白平衡开启时禁用手动色温
    m_wbSlider->setStyleSheet(sliderStyle);
    m_wbValueLabel = new QLabel(QStringLiteral("5000K"), camGroup);
    m_wbValueLabel->setFixedWidth(60);
    m_wbValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    wbRow->addWidget(wbLabel);
    wbRow->addWidget(m_wbSlider, 1);
    wbRow->addWidget(m_wbValueLabel);
    camLayout->addLayout(wbRow);

    // 自动曝光
    auto* autoExpRow = new QHBoxLayout();
    auto* autoExpLabel = new QLabel(QStringLiteral("Auto Exposure:"), camGroup);
    autoExpLabel->setFixedWidth(100);
    m_autoExposureCheckBox = new QCheckBox(QStringLiteral("Auto"), camGroup);
    m_autoExposureCheckBox->setChecked(true);
    m_autoExposureCheckBox->setStyleSheet(
        "QCheckBox { color: #e0e0e0; font-size: 13px; spacing: 6px; }"
        "QCheckBox::indicator { width: 20px; height: 20px; }"
        "QCheckBox::indicator:unchecked { "
        "  border: 2px solid #7f8c8d; border-radius: 3px; background: #16213e; }"
        "QCheckBox::indicator:checked { "
        "  border: 2px solid #3498db; border-radius: 3px; background: #3498db; }");
    autoExpRow->addWidget(autoExpLabel);
    autoExpRow->addWidget(m_autoExposureCheckBox);
    autoExpRow->addStretch();
    camLayout->addLayout(autoExpRow);

    // 手动曝光值
    auto* expRow = new QHBoxLayout();
    auto* expLabel = new QLabel(QStringLiteral("Exposure:"), camGroup);
    expLabel->setFixedWidth(100);
    m_exposureSlider = new QSlider(Qt::Horizontal, camGroup);
    m_exposureSlider->setRange(1, 5000);
    m_exposureSlider->setValue(312);
    m_exposureSlider->setEnabled(false);  // 自动曝光开启时禁用
    m_exposureSlider->setStyleSheet(sliderStyle);
    m_exposureValue = new QLabel(QStringLiteral("312"), camGroup);
    m_exposureValue->setFixedWidth(60);
    m_exposureValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    expRow->addWidget(expLabel);
    expRow->addWidget(m_exposureSlider, 1);
    expRow->addWidget(m_exposureValue);
    camLayout->addLayout(expRow);

    // 帧率
    auto* fpsRow = new QHBoxLayout();
    auto* fpsLabel = new QLabel(QStringLiteral("Framerate:"), camGroup);
    fpsLabel->setFixedWidth(100);
    m_framerateSlider = new QSlider(Qt::Horizontal, camGroup);
    m_framerateSlider->setRange(1, 120);
    m_framerateSlider->setValue(30);
    m_framerateSlider->setSingleStep(1);
    m_framerateSlider->setPageStep(5);
    m_framerateSlider->setStyleSheet(sliderStyle);
    m_framerateValue = new QLabel(QStringLiteral("30 fps"), camGroup);
    m_framerateValue->setFixedWidth(60);
    m_framerateValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    fpsRow->addWidget(fpsLabel);
    fpsRow->addWidget(m_framerateSlider, 1);
    fpsRow->addWidget(m_framerateValue);
    camLayout->addLayout(fpsRow);

    mainLayout->addWidget(camGroup);

    // ================================================================
    // (3) 底部按钮
    // ================================================================
    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();

    m_btnResetDefaults = new QPushButton(QStringLiteral("Reset Defaults"), m_settingsDialog);
    m_btnResetDefaults->setStyleSheet(
        "QPushButton { font-size: 13px; font-weight: bold; padding: 8px 16px;"
        "  background-color: #2c3e50; color: #ecf0f1;"
        "  border: 2px solid #7f8c8d; border-radius: 4px; }"
        "QPushButton:pressed { background-color: #1a252f; }");

    auto* btnClose = new QPushButton(QStringLiteral("Close"), m_settingsDialog);
    btnClose->setStyleSheet(
        "QPushButton { font-size: 13px; font-weight: bold; padding: 8px 24px;"
        "  background-color: #1a6fb5; color: white;"
        "  border: 2px solid #5aa9e6; border-radius: 4px; }"
        "QPushButton:pressed { background-color: #0d4a7a; }");

    btnRow->addWidget(m_btnResetDefaults);
    btnRow->addSpacing(12);
    btnRow->addWidget(btnClose);
    mainLayout->addLayout(btnRow);

    // ---- 关闭按钮连接 ----
    connect(btnClose, &QPushButton::clicked, m_settingsDialog, &QDialog::close);

    // ---- 帧率防抖计时器 ----
    m_framerateDebounceTimer = new QTimer(this);
    m_framerateDebounceTimer->setSingleShot(true);  // 单次触发
    connect(m_framerateDebounceTimer, &QTimer::timeout,
            this, &CameraGUI::onFramerateDebounced);
}

// ============================================================
// 相机控制参数范围设置（由 main.cpp 在 V4L2 查询后调用）
// ============================================================

void CameraGUI::setBrightnessRange(int min, int max, int step, int value) {
    m_brightnessInfo = {min, max, step, value, value};
    m_cameraControlsAvailable = true;

    // 阻止信号，避免设置范围时触发回调
    m_brightnessSlider->blockSignals(true);
    m_brightnessSlider->setRange(min, max);
    m_brightnessSlider->setSingleStep(step);
    m_brightnessSlider->setPageStep(step * 10);
    m_brightnessSlider->setValue(value);
    m_brightnessSlider->blockSignals(false);
    m_brightnessValue->setText(QString::number(value));
}

void CameraGUI::setContrastRange(int min, int max, int step, int value) {
    m_contrastInfo = {min, max, step, value, value};

    m_contrastSlider->blockSignals(true);
    m_contrastSlider->setRange(min, max);
    m_contrastSlider->setSingleStep(step);
    m_contrastSlider->setPageStep(step * 10);
    m_contrastSlider->setValue(value);
    m_contrastSlider->blockSignals(false);
    m_contrastValue->setText(QString::number(value));
}

void CameraGUI::setWhiteBalanceRange(int min, int max, int step, int value) {
    m_wbInfo = {min, max, step, value, value};

    m_wbSlider->blockSignals(true);
    m_wbSlider->setRange(min, max);
    m_wbSlider->setSingleStep(step);
    m_wbSlider->setPageStep(step * 100);
    m_wbSlider->setValue(value);
    m_wbSlider->blockSignals(false);
    m_wbValueLabel->setText(QString("%1K").arg(value));
}

void CameraGUI::setAutoWhiteBalance(bool enabled) {
    m_autoWbDefault = enabled;
    m_autoWbCheckBox->blockSignals(true);
    m_autoWbCheckBox->setChecked(enabled);
    m_autoWbCheckBox->blockSignals(false);
    m_wbSlider->setEnabled(!enabled);
}

void CameraGUI::setExposureRange(int min, int max, int step, int value) {
    m_exposureInfo = {min, max, step, value, value};

    m_exposureSlider->blockSignals(true);
    m_exposureSlider->setRange(min, max);
    m_exposureSlider->setSingleStep(step);
    m_exposureSlider->setPageStep(step * 10);
    m_exposureSlider->setValue(value);
    m_exposureSlider->blockSignals(false);
    m_exposureValue->setText(QString::number(value));
}

void CameraGUI::setAutoExposure(bool enabled) {
    m_autoExposureDefault = enabled;
    m_autoExposureCheckBox->blockSignals(true);
    m_autoExposureCheckBox->setChecked(enabled);
    m_autoExposureCheckBox->blockSignals(false);
    m_exposureSlider->setEnabled(!enabled);
}

void CameraGUI::onAutoExposureChanged(int state) {
    bool autoExp = (state == Qt::Checked);
    m_exposureSlider->setEnabled(!autoExp);
    if (m_onCameraControl) {
        m_onCameraControl(static_cast<int>(CameraCapture::V4L2_CID_EXPOSURE_AUTO),
                          autoExp ? 3 : 1);  // 3=auto(Aperture Priority), 1=manual
    }
    // 切换到手动模式时，写入当前滑块值
    if (!autoExp) {
        int val = m_exposureSlider->value();
        m_exposureInfo.current = val;
        if (m_onCameraControl) {
            m_onCameraControl(static_cast<int>(CameraCapture::V4L2_CID_EXPOSURE_ABSOLUTE), val);
        }
    }
    qDebug() << "[GUI] Auto Exposure changed:" << (autoExp ? "ON" : "OFF");
}

void CameraGUI::onExposureChanged(int value) {
    m_exposureValue->setText(QString::number(value));
    m_exposureInfo.current = value;
    if (m_onCameraControl) {
        m_onCameraControl(static_cast<int>(CameraCapture::V4L2_CID_EXPOSURE_ABSOLUTE), value);
    }
    qDebug() << "[GUI] Exposure changed:" << value;
}

void CameraGUI::setFramerateRange(int minFps, int maxFps, int currentFps) {
    // 限制合理范围：最小 1fps，最大 120fps
    minFps = std::max(1, minFps);
    maxFps = std::min(120, maxFps);
    if (maxFps < minFps) maxFps = minFps;
    currentFps = std::max(minFps, std::min(maxFps, currentFps));

    m_framerateInfo = {minFps, maxFps, 1, currentFps, currentFps};
    m_framerateDefault = currentFps;

    m_framerateSlider->blockSignals(true);
    m_framerateSlider->setRange(minFps, maxFps);
    m_framerateSlider->setSingleStep(1);
    m_framerateSlider->setPageStep(5);
    m_framerateSlider->setValue(currentFps);
    m_framerateSlider->blockSignals(false);
    m_framerateValue->setText(QString("%1 fps").arg(currentFps));
}

// ============================================================
// 相机控制槽函数
// ============================================================

void CameraGUI::onBrightnessChanged(int value) {
    m_brightnessValue->setText(QString::number(value));
    m_brightnessInfo.current = value;
    if (m_onCameraControl) {
        m_onCameraControl(static_cast<int>(CameraCapture::V4L2_CID_BRIGHTNESS), value);
    }
    qDebug() << "[GUI] Brightness changed:" << value;
}

void CameraGUI::onContrastChanged(int value) {
    m_contrastValue->setText(QString::number(value));
    m_contrastInfo.current = value;
    if (m_onCameraControl) {
        m_onCameraControl(static_cast<int>(CameraCapture::V4L2_CID_CONTRAST), value);
    }
    qDebug() << "[GUI] Contrast changed:" << value;
}

void CameraGUI::onAutoWbChanged(int state) {
    bool autoWb = (state == Qt::Checked);
    m_wbSlider->setEnabled(!autoWb);
    if (m_onCameraControl) {
        m_onCameraControl(static_cast<int>(CameraCapture::V4L2_CID_AUTO_WHITE_BALANCE),
                          autoWb ? 1 : 0);
    }
    qDebug() << "[GUI] Auto WB changed:" << (autoWb ? "ON" : "OFF");
}

void CameraGUI::onWbChanged(int value) {
    m_wbValueLabel->setText(QString("%1K").arg(value));
    m_wbInfo.current = value;
    if (m_onCameraControl) {
        m_onCameraControl(static_cast<int>(CameraCapture::V4L2_CID_WHITE_BALANCE_TEMPERATURE),
                          value);
    }
    qDebug() << "[GUI] WB Temperature changed:" << value;
}

void CameraGUI::onFramerateSliderChanged(int value) {
    // 立即更新标签（视觉反馈），延迟触发实际帧率变更
    m_framerateValue->setText(QString("%1 fps").arg(value));
    m_framerateInfo.current = value;
    // 重启防抖计时器：300ms 内无新变化才真正执行
    m_framerateDebounceTimer->start(300);
}

void CameraGUI::onFramerateDebounced() {
    // 防抖结束后，用滑块最终停留的值触发回调
    int value = m_framerateSlider->value();
    m_framerateInfo.current = value;
    if (m_onFramerate) {
        m_onFramerate(value);
    }
    qDebug() << "[GUI] Framerate changed (debounced):" << value;
}

void CameraGUI::onResetDefaults() {
    qDebug() << "[GUI] Reset camera controls to defaults";

    // 恢复亮度
    m_brightnessSlider->setValue(m_brightnessInfo.def);
    m_brightnessValue->setText(QString::number(m_brightnessInfo.def));
    m_brightnessInfo.current = m_brightnessInfo.def;
    if (m_onCameraControl) {
        m_onCameraControl(static_cast<int>(CameraCapture::V4L2_CID_BRIGHTNESS),
                          m_brightnessInfo.def);
    }

    // 恢复对比度
    m_contrastSlider->setValue(m_contrastInfo.def);
    m_contrastValue->setText(QString::number(m_contrastInfo.def));
    m_contrastInfo.current = m_contrastInfo.def;
    if (m_onCameraControl) {
        m_onCameraControl(static_cast<int>(CameraCapture::V4L2_CID_CONTRAST),
                          m_contrastInfo.def);
    }

    // 恢复白平衡
    m_autoWbCheckBox->setChecked(m_autoWbDefault);
    m_wbSlider->setValue(m_wbInfo.def);
    m_wbValueLabel->setText(QString("%1K").arg(m_wbInfo.def));
    m_wbInfo.current = m_wbInfo.def;
    m_wbSlider->setEnabled(!m_autoWbDefault);
    if (m_onCameraControl) {
        m_onCameraControl(static_cast<int>(CameraCapture::V4L2_CID_AUTO_WHITE_BALANCE),
                          m_autoWbDefault ? 1 : 0);
        m_onCameraControl(static_cast<int>(CameraCapture::V4L2_CID_WHITE_BALANCE_TEMPERATURE),
                          m_wbInfo.def);
    }

    // 恢复曝光
    m_autoExposureCheckBox->setChecked(m_autoExposureDefault);
    m_exposureSlider->setValue(m_exposureInfo.def);
    m_exposureValue->setText(QString::number(m_exposureInfo.def));
    m_exposureInfo.current = m_exposureInfo.def;
    m_exposureSlider->setEnabled(!m_autoExposureDefault);
    if (m_onCameraControl) {
        m_onCameraControl(static_cast<int>(CameraCapture::V4L2_CID_EXPOSURE_AUTO),
                          m_autoExposureDefault ? 3 : 1);
        m_onCameraControl(static_cast<int>(CameraCapture::V4L2_CID_EXPOSURE_ABSOLUTE),
                          m_exposureInfo.def);
    }

    // 恢复帧率（停止防抖计时器，直接执行，避免重复触发）
    m_framerateDebounceTimer->stop();
    m_framerateSlider->setValue(m_framerateInfo.def);
    m_framerateValue->setText(QString("%1 fps").arg(m_framerateInfo.def));
    m_framerateInfo.current = m_framerateInfo.def;
    if (m_onFramerate) {
        m_onFramerate(m_framerateInfo.def);
    }
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
