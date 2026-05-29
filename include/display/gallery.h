#ifndef SMART_CAM_DISPLAY_GALLERY_H
#define SMART_CAM_DISPLAY_GALLERY_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QPixmap>
#include <QVector>
#include <vector>
#include <string>

#include "include/storage/manager.h"
#include "include/display/video_player.h"

/**
 * @brief 相册浏览组件
 *
 * 两种视图：
 *   1. 缩略图网格 — 3 列 × N 行，可滚动，按日期分组
 *   2. 全屏查看 — 单张照片 + 翻页/删除
 *
 * 适配 7 寸触摸屏 (800x480)，轻量级实现。
 */
class PhotoGallery : public QWidget {
    Q_OBJECT

public:
    explicit PhotoGallery(StorageManager* storage, QWidget* parent = nullptr);
    ~PhotoGallery() override;

    /** @brief 刷新照片列表（从磁盘重新拉取） */
    void refresh();

    /** @brief 进入相册时调用，重置到网格视图 */
    void reset();

signals:
    /** @brief 用户点击返回，通知主程序切换回实时预览 */
    void backToLive();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onPrevPhoto();
    void onNextPhoto();
    void onDeletePhoto();
    void onBackToGallery();
    void onVideoPlaybackFinished();

private:
    // ---- UI 构建 ----
    void buildGalleryView();
    void buildFullscreenView();
    void loadVisibleThumbnails();     // 懒加载可见区域缩略图
    void updateStorageInfo();         // 更新存储空间状态栏

    // ---- 辅助 ----
    bool createThumbnail(const std::string& jpegPath,
                         int thumbW, int thumbH,
                         QPixmap& out);
    bool createThumbnailFromJpegData(const std::vector<uint8_t>& jpegData,
                                     int thumbW, int thumbH,
                                     QPixmap& out);
    bool createVideoThumbnail(const std::string& aviPath,
                              int thumbW, int thumbH,
                              QPixmap& out);
    void showFullscreen(int mediaIndex);
    void updateFullscreenDisplay();
    void clearThumbnails();
    void stopVideoPlayback();

    // ---- 数据 ----
    StorageManager*              m_storage;
    std::vector<StorageManager::PhotoDayGroup> m_groups;
    std::vector<StorageManager::PhotoInfo>   m_flatPhotos;    // 扁平列表
    int                           m_currentIndex = -1;

    // ---- 缩略图网格控件 ----
    QWidget*      m_galleryView     = nullptr;
    QScrollArea*  m_scrollArea      = nullptr;
    QGridLayout*  m_gridLayout      = nullptr;
    QLabel*       m_galleryTitle    = nullptr;
    QLabel*       m_galleryEmpty    = nullptr;   // 空相册提示
    QLabel*       m_storageInfoLabel = nullptr;  // 存储空间状态栏

    static constexpr int THUMB_COLS = 3;
    static constexpr int THUMB_W    = 170;
    static constexpr int THUMB_H    = 120;
    static constexpr int THUMB_SPACING = 10;

    // ---- 全屏视图控件 ----
    QWidget*      m_fullView        = nullptr;
    QStackedWidget* m_fullMediaStack = nullptr;   // [0]=照片显示, [1]=视频播放器
    QLabel*       m_fullPhotoDisplay = nullptr;
    VideoPlayer*  m_videoPlayer     = nullptr;
    QLabel*       m_fullInfoLabel   = nullptr;
    QPushButton*  m_btnPrev         = nullptr;
    QPushButton*  m_btnNext         = nullptr;
    QPushButton*  m_btnDelete       = nullptr;
    QPushButton*  m_btnBackGal      = nullptr;

    // ---- 视图栈 ----
    QStackedWidget* m_stack = nullptr;  // [0]=grid, [1]=full

    // ---- 触摸滑动 ----
    int m_touchStartX = 0;
};

#endif // SMART_CAM_DISPLAY_GALLERY_H
