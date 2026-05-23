#include "include/display/gui.h"
#include <QApplication>
#include <QScreen>
#include <QPainter>
#include <QImage>
#include <QFont>
#include <QDateTime>
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
    setWindowTitle(QStringLiteral("SmartCam Linux — 智能相机流媒体系统"));

    // 适配 7 寸屏 (800x480)，同时也兼容 PC 调试
    setMinimumSize(800, 480);
    resize(800, 520);

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
    m_labelStreaming->setText(QStringLiteral("⚫ 就绪"));
    m_labelStreaming->setStyleSheet("color: gray;");
    m_labelClients->setText(QStringLiteral("客户端: 0"));
    m_labelRecording->setText(QStringLiteral("● REC"));
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

    // --- (1) 视频预览区 ---
    m_videoDisplay = new QLabel(this);
    m_videoDisplay->setAlignment(Qt::AlignCenter);
    m_videoDisplay->setMinimumSize(640, 360);
    m_videoDisplay->setStyleSheet(
        "background-color: #1a1a2e;"
        "border: 2px solid #0f3460;"
        "border-radius: 4px;"
        "color: #4a4a6a;"
    );
    m_videoDisplay->setText(QStringLiteral("等待摄像头…"));
    m_videoDisplay->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_videoDisplay->setScaledContents(true);
    mainLayout->addWidget(m_videoDisplay, 1);

    // --- (2) 状态栏 ---
    auto* statusLayout = new QHBoxLayout();
    statusLayout->setSpacing(12);

    m_labelFPS = new QLabel(QStringLiteral("FPS: 0.0"), this);
    m_labelStreaming = new QLabel(QStringLiteral("⚫ 就绪"), this);
    m_labelClients = new QLabel(QStringLiteral("客户端: 0"), this);
    m_labelRecording = new QLabel(QStringLiteral("● REC"), this);

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

    QString btnStyle =
        "QPushButton {"
        "  font-size: 14px; font-weight: bold; padding: 8px 16px;"
        "  border-radius: 6px; border: none;"
        "  min-width: 80px;"
        "}"
        "QPushButton:hover  { opacity: 0.9; }"
        "QPushButton:pressed { padding-top: 10px; padding-bottom: 6px; }";

    m_btnCapture = new QPushButton(QStringLiteral("📷 拍照"), this);
    m_btnRecord  = new QPushButton(QStringLiteral("⏺ 录像"), this);
    m_btnSettings = new QPushButton(QStringLiteral("⚙ 设置"), this);

    m_btnCapture->setStyleSheet(btnStyle +
        "background-color: #0f3460; color: white;");
    m_btnRecord->setStyleSheet(btnStyle +
        "background-color: #533483; color: white;");
    m_btnSettings->setStyleSheet(btnStyle +
        "background-color: #1a1a2e; color: #a0a0c0;");

    btnLayout->addWidget(m_btnCapture);
    btnLayout->addWidget(m_btnRecord);
    btnLayout->addWidget(m_btnSettings);
    btnLayout->addStretch();
    mainLayout->addLayout(btnLayout);

    // --- (4) 设置栏（可展开/收起） ---
    m_settingsPanel = new QWidget(this);
    auto* settingsLayout = new QHBoxLayout(m_settingsPanel);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsLayout->setSpacing(12);

    auto* resLabel = new QLabel(QStringLiteral("分辨率:"), this);
    m_resolutionCombo = new QComboBox(this);
    m_resolutionCombo->addItem(QStringLiteral("640x480"),  QVariant::fromValue(RES_640x480));
    m_resolutionCombo->addItem(QStringLiteral("320x240"),  QVariant::fromValue(RES_320x240));
    m_resolutionCombo->addItem(QStringLiteral("1280x720"), QVariant::fromValue(RES_1280x720));

    auto* fmtLabel = new QLabel(QStringLiteral("格式:"), this);
    m_formatCombo = new QComboBox(this);
    m_formatCombo->addItem(QStringLiteral("YUV (本地预览)"),  static_cast<int>(PixelFormat::FMT_YUYV));
    m_formatCombo->addItem(QStringLiteral("MJPEG (硬件直出)"), static_cast<int>(PixelFormat::FMT_MJPEG));

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

    settingsLayout->addWidget(resLabel);
    settingsLayout->addWidget(m_resolutionCombo);
    settingsLayout->addSpacing(16);
    settingsLayout->addWidget(fmtLabel);
    settingsLayout->addWidget(m_formatCombo);
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
    connect(m_btnSettings, &QPushButton::clicked, this, &CameraGUI::onSettings);
    connect(m_resolutionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CameraGUI::onResolutionComboChanged);
    connect(m_formatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CameraGUI::onFormatComboChanged);
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
    qDebug() << "[GUI] 拍照按钮点击";
    emit captureClicked();
    if (m_onCapture) m_onCapture();
}

void CameraGUI::onRecord() {
    bool shouldRecord = !m_isRecording;
    qDebug() << "[GUI] 录像请求:" << (shouldRecord ? "开始" : "停止");

    // 先询问回调，回调返回 true 才允许切换状态
    if (m_onRecordToggle) {
        if (!m_onRecordToggle(shouldRecord)) {
            qDebug() << "[GUI] 录像被拒绝（格式不支持等）";
            return;  // 回调拒绝，不切换 UI
        }
    }

    m_isRecording = shouldRecord;

    if (m_isRecording) {
        m_btnRecord->setText(QStringLiteral("⏹ 停止"));
        m_btnRecord->setStyleSheet(
            "background-color: #c0392b; color: white;"
            "font-size: 14px; font-weight: bold; padding: 8px 16px;"
            "border-radius: 6px; border: none; min-width: 80px;");
        m_labelRecording->setStyleSheet(
            "font-size: 13px; font-weight: bold; padding: 2px 6px;"
            "background: #16213e; border-radius: 3px; color: #e74c3c;");
    } else {
        m_btnRecord->setText(QStringLiteral("⏺ 录像"));
        m_btnRecord->setStyleSheet(
            "background-color: #533483; color: white;"
            "font-size: 14px; font-weight: bold; padding: 8px 16px;"
            "border-radius: 6px; border: none; min-width: 80px;");
        m_labelRecording->setStyleSheet(
            "font-size: 13px; font-weight: bold; padding: 2px 6px;"
            "background: #16213e; border-radius: 3px; color: gray;");
    }

    emit recordToggled(m_isRecording);
}

void CameraGUI::onSettings() {
    bool visible = m_settingsPanel->isVisible();
    m_settingsPanel->setVisible(!visible);
    qDebug() << "[GUI] 设置面板" << (visible ? "隐藏" : "显示");
}

void CameraGUI::onResolutionComboChanged(int index) {
    QVariant data = m_resolutionCombo->itemData(index);
    if (!data.canConvert<Resolution>()) return;

    Resolution res = data.value<Resolution>();
    if (res != Resolution{m_currentFrame.width, m_currentFrame.height}) {
        qDebug() << "[GUI] 分辨率切换:" << res.width << "x" << res.height;
        if (m_mockMode) {
            enterMockMode();   // 重新生成模拟帧
        }
        emit resolutionChanged(res.width, res.height);
        if (m_onResolutionChanged) m_onResolutionChanged(res.width, res.height);
    }
}

void CameraGUI::onFormatComboChanged(int index) {
    PixelFormat fmt = static_cast<PixelFormat>(m_formatCombo->itemData(index).toInt());
    qDebug() << "[GUI] 格式切换:" << static_cast<int>(fmt);
    emit formatChanged(fmt);
    if (m_onFormatChanged) m_onFormatChanged(fmt);
}

// ============================================================
// 公共接口
// ============================================================

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
    m_labelClients->setText(QString("客户端: %1").arg(count));
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
        m_labelStreaming->setText(QStringLiteral("🟢 推流中"));
        m_labelStreaming->setStyleSheet(
            "font-size: 13px; font-weight: bold; padding: 2px 6px;"
            "background: #16213e; border-radius: 3px; color: #2ecc71;");
    } else {
        m_labelStreaming->setText(QStringLiteral("⚫ 就绪"));
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
    m_labelStreaming->setText(QStringLiteral("🟡 模拟中"));
    m_labelStreaming->setStyleSheet(
        "font-size: 13px; font-weight: bold; padding: 2px 6px;"
        "background: #16213e; border-radius: 3px; color: #f39c12;");

    qDebug() << "[GUI] 进入 Mock 模式:" << w << "x" << h;
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
        qWarning() << "[GUI] 不支持的像素格式:" << static_cast<int>(fmt);
        return {};
    }
}
