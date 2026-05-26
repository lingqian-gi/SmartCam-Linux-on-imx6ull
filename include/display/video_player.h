#ifndef SMART_CAM_DISPLAY_VIDEO_PLAYER_H
#define SMART_CAM_DISPLAY_VIDEO_PLAYER_H

/**
 * @file    video_player.h
 * @brief   VideoPlayer — 轻量 AVI/MJPEG 视频播放器
 *
 * 专为嵌入式平台 (iMX6ULL + linuxfb) 设计：
 *   - 解析自建 AVI 容器 (RIFF + MJPEG 帧)
 *   - 按帧索引逐帧解码 JPEG → QPixmap 渲染
 *   - QTimer 驱动帧率控制，无外部解码器依赖
 *   - 复用现有 libjpeg-turbo 做 JPEG→RGB 解码
 *
 * 不依赖 ffmpeg / gstreamer / vlc。
 */

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSlider>
#include <cstdio>
#include <vector>
#include <string>
#include <cstdint>

#include "include/storage/manager.h"

class VideoPlayer : public QWidget {
    Q_OBJECT

public:
    explicit VideoPlayer(QWidget* parent = nullptr);
    ~VideoPlayer() override;

    /**
     * @brief 加载并解析 AVI 文件头
     *
     * 完成以下步骤：
     *   1. 读取 RIFF 文件头，验证 "AVI " 类型
     *   2. 解析 avih 块 → fps / 宽高 / 总帧数
     *   3. 定位 movi 数据区起始偏移
     *   4. 解析 idx1 索引表 → 每帧偏移 + 长度
     *
     * @param aviPath  AVI 文件完整路径
     * @return true 成功解析, false 格式错误或文件不存在
     */
    bool loadVideo(const std::string& aviPath);

    /** @brief 开始 / 恢复播放 */
    void play();

    /** @brief 暂停播放，保持当前帧 */
    void pause();

    /** @brief 停止并释放文件句柄 */
    void stop();

    /** @brief 当前是否正在播放 */
    bool isPlaying() const { return m_playing; }

    /** @brief 获取视频总时长（秒） */
    double duration() const {
        return m_fps > 0 ? m_totalFrames / m_fps : 0.0;
    }

signals:
    /** @brief 播放到达最后一帧时发射 */
    void playbackFinished();

    /** @brief 加载或解码出错时发射 */
    void playbackError(const QString& msg);

private slots:
    void onTimerTick();

private:
    // ---- AVI 解析 ----
    bool parseAviHeader();
    bool seekToFrame(int frameIndex);
    bool readFrameJpeg(std::vector<uint8_t>& jpegData);
    void decodeAndDisplay(const std::vector<uint8_t>& jpegData);
    void clearState();

    // ---- UI 构建 ----
    void buildUI();

    // ---- 数据 ----
    FILE*        m_file          = nullptr;
    std::string  m_filePath;
    QTimer*      m_timer         = nullptr;

    // AVI 元数据
    double       m_fps           = 30.0;
    int          m_totalFrames   = 0;
    int          m_width         = 640;
    int          m_height        = 480;
    long         m_moviDataOffset = 0;   // movi 数据区起始（第一个 00dc 的偏移）
    std::vector<AviIndexEntry> m_index;  // 帧索引表（全局结构体，定义于 manager.h）

    int          m_currentFrame  = 0;
    bool         m_playing       = false;

    // ---- UI 控件 ----
    QLabel*      m_videoLabel    = nullptr;   // 视频显示区
    QPushButton* m_btnPlayPause  = nullptr;   // ▶ / ⏸
    QLabel*      m_frameInfo     = nullptr;   // "3/120  00:01.5"
    QSlider*     m_progressBar   = nullptr;   // 进度条（可选）
    bool         m_sliderDragging = false;     // 拖动进度条时暂停刷新
};

#endif // SMART_CAM_DISPLAY_VIDEO_PLAYER_H
