#ifndef SMART_CAM_DISPLAY_GUI_H
#define SMART_CAM_DISPLAY_GUI_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QGroupBox>
#include <QStatusBar>
#include <QDialog>
#include <QSlider>
#include <QCheckBox>
#include <cstdint>

#include "include/common/types.h"
#include "include/display/gallery.h"

/**
 * @brief 智能相机主GUI界面
 *
 * 适配 7寸触摸屏 (800x480)，使用 Qt Widgets + framebuffer 渲染。
 * 支持：
 *   - 实时视频预览（QTimer 33ms ≈ 30fps）
 *   - 拍照保存 JPEG
 *   - 录像开始/停止
 *   - 分辨率 & 格式切换
 *   - FPS 实时显示
 *
 * 可独立测试：内置 MockCamera 模拟帧生成器，无需真实硬件。
 */
class CameraGUI : public QWidget {
    Q_OBJECT

public:
    explicit CameraGUI(QWidget* parent = nullptr);
    ~CameraGUI() override;

    /**
     * @brief 接收一帧图像数据并刷新到屏幕
     * @param data    帧数据（RGB24 或 RGB565）
     * @param len     数据长度
     * @param w       宽
     * @param h       高
     * @param fmt     格式
     */
    void setFrame(const uint8_t* data, int len, int w, int h, PixelFormat fmt);

    /**
     * @brief 更新状态栏信息
     */
    void setFPS(double fps);
    void setClientCount(int count);
    void setRecordingStatus(bool recording);
    void setStreamingStatus(bool streaming);
    void setStoragePath(const std::string& path);  // 同步存储路径到下拉框

    // ---- 回调注册（供主程序注入业务逻辑） ----
    using CallbackVoid   = std::function<void()>;
    using CallbackBool   = std::function<bool(bool)>;
    using CallbackIntInt = std::function<void(int, int)>;
    using CallbackFormat = std::function<void(PixelFormat)>;
    using CallbackString = std::function<void(const std::string&)>;
    using CallbackCameraControl = std::function<void(int cid, int value)>;
    using CallbackFramerate = std::function<void(int fps)>;

    void onCaptureRequest(CallbackVoid cb);
    void onRecordToggle(std::function<bool(bool)> cb);  // 回调返回 true=成功, false=拒绝
    void onResolutionChanged(CallbackIntInt cb);  // (w, h)
    void onFormatChanged(CallbackFormat cb);
    void onStoragePathChanged(CallbackString cb);  // 存储路径变更
    void onCameraControlChanged(CallbackCameraControl cb);  // 相机控制参数变更
    void onFramerateChanged(CallbackFramerate cb);  // 帧率变更回调

    // ---- 相机控制参数范围设置 ----
    void setBrightnessRange(int min, int max, int step, int value);
    void setContrastRange(int min, int max, int step, int value);
    void setWhiteBalanceRange(int min, int max, int step, int value);
    void setAutoWhiteBalance(bool enabled);

    // ---- 曝光控制 ----
    void setExposureRange(int min, int max, int step, int value);
    void setAutoExposure(bool enabled);

    // ---- 帧率设置 ----
    void setFramerateRange(int minFps, int maxFps, int currentFps);

    // ---- 相册集成 ----
    void setGalleryStorage(StorageManager* storage);
    void showGallery();
    void showLivePreview();

signals:
    /// 外部可通过信号感知 GUI 事件
    void captureClicked();
    void recordToggled(bool start);
    void resolutionChanged(int w, int h);
    void formatChanged(PixelFormat fmt);

private slots:
    void refreshFrame();
    void onCapture();
    void onRecord();
    void onSettings();
    void onGallery();          // 新增
    void onResolutionComboChanged(int index);
    void onFormatComboChanged(int index);
    void onBackFromGallery();  // 新增：从相册返回
    void onStorageComboChanged(int index);  // 新增：存储路径切换
    void onBrightnessChanged(int value);
    void onContrastChanged(int value);
    void onAutoWbChanged(int state);
    void onWbChanged(int value);
    void onResetDefaults();
    void onFramerateSliderChanged(int value);
    void onFramerateDebounced();        // 防抖后真正执行帧率变更
    void onAutoExposureChanged(int state);
    void onExposureChanged(int value);

private:
    void buildUI();
    void buildSettingsDialog();
    void connectSignals();
    void enterMockMode();        // 无摄像头时使用模拟帧
    QImage frameToQImage(const uint8_t* data, int len, int w, int h, PixelFormat fmt);

    // ====== UI 控件 ======
    QStackedWidget* m_mainStack     = nullptr;   // [0]=实时预览, [1]=相册
    QWidget*        m_liveViewContainer = nullptr;
    QLabel*         m_videoDisplay   = nullptr;   // 视频预览区
    QPushButton*    m_btnCapture     = nullptr;   // 拍照
    QPushButton*    m_btnRecord      = nullptr;   // 录像（toggle）
    QPushButton*    m_btnSettings    = nullptr;   // 设置
    QPushButton*    m_btnGallery     = nullptr;   // 相册
    PhotoGallery*   m_gallery        = nullptr;   // 相册组件

    // 状态栏
    QLabel*      m_labelFPS       = nullptr;
    QLabel*      m_labelStreaming = nullptr;
    QLabel*      m_labelClients   = nullptr;
    QLabel*      m_labelRecording = nullptr;

    // 定时器
    QTimer*      m_refreshTimer   = nullptr;

    // ====== 设置对话框 ======
    QDialog*     m_settingsDialog  = nullptr;
    QComboBox*   m_resolutionCombo = nullptr;  // 分辨率选择（在对话框中）
    QComboBox*   m_formatCombo     = nullptr;  // 格式选择（在对话框中）
    QComboBox*   m_storageCombo    = nullptr;  // 存储路径选择（在对话框中）
    QSlider*     m_brightnessSlider = nullptr;
    QLabel*      m_brightnessValue  = nullptr;
    QSlider*     m_contrastSlider   = nullptr;
    QLabel*      m_contrastValue    = nullptr;
    QSlider*     m_wbSlider         = nullptr;
    QLabel*      m_wbValueLabel     = nullptr;
    QCheckBox*   m_autoWbCheckBox   = nullptr;
    // 曝光控制
    QSlider*     m_exposureSlider   = nullptr;
    QLabel*      m_exposureValue    = nullptr;
    QCheckBox*   m_autoExposureCheckBox = nullptr;
    QPushButton* m_btnResetDefaults = nullptr;

    // 帧率控制
    QSlider*     m_framerateSlider  = nullptr;
    QLabel*      m_framerateValue   = nullptr;
    QTimer*      m_framerateDebounceTimer = nullptr;  // 帧率防抖计时器

    // ====== 数据 ======
    FrameBuffer  m_currentFrame;
    std::vector<uint8_t> m_frameBuffer;  // 内部帧数据拷贝（避免指针悬垂）
    bool         m_isRecording    = false;
    bool         m_mockMode       = false;

    // 相机控制参数范围信息（用于重置默认值）
    struct ControlInfo {
        int min = 0, max = 100, step = 1, def = 0, current = 0;
    };
    ControlInfo m_brightnessInfo;
    ControlInfo m_contrastInfo;
    ControlInfo m_wbInfo;
    bool        m_autoWbDefault       = true;
    ControlInfo m_exposureInfo;         // 手动曝光参数
    bool        m_autoExposureDefault = true;
    bool        m_cameraControlsAvailable = false;

    // 帧率控制参数
    ControlInfo m_framerateInfo;       // min/max/step/def/current
    int         m_framerateDefault     = 30;  // V4L2 默认帧率

    // 回调
    CallbackVoid        m_onCapture;
    CallbackBool        m_onRecordToggle;
    CallbackIntInt      m_onResolutionChanged;
    CallbackFormat      m_onFormatChanged;
    CallbackString      m_onStoragePathChanged;
    CallbackCameraControl m_onCameraControl;
    CallbackFramerate    m_onFramerate;

    // ---- 模拟器 ----
    int         m_mockFrameIndex = 0;
    std::vector<uint8_t> m_mockBuffer;        // 预分配 RGB 缓冲
};

// ============================================================
// 颜色空间转换工具函数（inline，供 display 模块内部使用）
// ============================================================

/**
 * @brief YUYV 4:2:2 → RGB24
 *
 * 公式 (BT.601):
 *   R = Y + 1.402   * (V - 128)
 *   G = Y - 0.34414 * (U - 128) - 0.71414 * (V - 128)
 *   B = Y + 1.772   * (U - 128)
 *
 * 使用查表法 + 定点运算加速。
 *
 * @param yuyv  输入 YUYV 数据
 * @param rgb   输出 RGB24 数据（调用者保证 w*h*3 足够）
 * @param w     宽 (像素)
 * @param h     高 (像素)
 */
inline void yuyv_to_rgb24(const uint8_t* yuyv, uint8_t* rgb, int w, int h) {
#ifdef __ARM_NEON
    // ARM 平台: 使用 NEON SIMD 加速 (processor_neon.cpp)
    extern void yuyv_to_rgb24_neon(const uint8_t*, uint8_t*, int, int);
    yuyv_to_rgb24_neon(yuyv, rgb, w, h);
    return;
#endif

    // x86 / 无 NEON 退路: 标量 C++ 实现
    const int pixels = w * h;
    int di = 0;
    for (int i = 0; i < pixels; i += 2) {
        int si = i * 2;
        int y0 = yuyv[si];
        int u  = yuyv[si + 1] - 128;
        int y1 = yuyv[si + 2];
        int v  = yuyv[si + 3] - 128;

        // BT.601 → RGB（clamp 到 0-255）
        auto clip = [](int x) -> uint8_t {
            return static_cast<uint8_t>(x < 0 ? 0 : (x > 255 ? 255 : x));
        };

        int r0 = y0 + ((v * 359) >> 8);
        int g0 = y0 - ((u * 88) >> 8) - ((v * 183) >> 8);
        int b0 = y0 + ((u * 454) >> 8);

        int r1 = y1 + ((v * 359) >> 8);
        int g1 = y1 - ((u * 88) >> 8) - ((v * 183) >> 8);
        int b1 = y1 + ((u * 454) >> 8);

        rgb[di++] = clip(r0); rgb[di++] = clip(g0); rgb[di++] = clip(b0);
        rgb[di++] = clip(r1); rgb[di++] = clip(g1); rgb[di++] = clip(b1);
    }
}

/**
 * @brief YUYV 4:2:2 → RGB565（适配 16-bit LCD framebuffer）
 */
inline void yuyv_to_rgb565(const uint8_t* yuyv, uint8_t* rgb565, int w, int h) {
    const int pixels = w * h;
    int di = 0;
    for (int i = 0; i < pixels; i += 2) {
        int si = i * 2;
        int y0 = yuyv[si];
        int u  = yuyv[si + 1] - 128;
        int y1 = yuyv[si + 2];
        int v  = yuyv[si + 3] - 128;

        auto clip = [](int x) -> int { return x < 0 ? 0 : (x > 255 ? 255 : x); };

        auto to565 = [&](int y, int u2, int v2) -> uint16_t {
            int r = y + ((v2 * 359) >> 8);
            int g = y - ((u2 * 88) >> 8) - ((v2 * 183) >> 8);
            int b = y + ((u2 * 454) >> 8);
            return static_cast<uint16_t>(
                ((clip(r) & 0xF8) << 8) | ((clip(g) & 0xFC) << 3) | ((clip(b) & 0xF8) >> 3)
            );
        };

        uint16_t p0 = to565(y0, u, v);
        uint16_t p1 = to565(y1, u, v);
        rgb565[di++] = p0 & 0xFF;
        rgb565[di++] = (p0 >> 8) & 0xFF;
        rgb565[di++] = p1 & 0xFF;
        rgb565[di++] = (p1 >> 8) & 0xFF;
    }
}

#endif // SMART_CAM_DISPLAY_GUI_H
