#ifndef SMART_CAM_STORAGE_MANAGER_H
#define SMART_CAM_STORAGE_MANAGER_H

/**
 * @file    manager.h
 * @brief   StorageManager — 存储管理模块
 *
 * 负责：
 *   1. 拍照保存 JPEG（按日期分目录，自动命名）
 *   2. 录像：MJPEG 帧封装 AVI（RIFF 容器格式）
 *   3. 空间管理：查询剩余空间、自动清理旧文件
 *
 * 线程安全：savePhoto() / writeRecordFrame() 可从采集线程调用。
 *
 * 典型用法:
 * @code
 *   StorageManager sm("/data/photos", "/data/videos");
 *
 *   // 拍照
 *   sm.savePhoto(jpeg_data, jpeg_len);
 *
 *   // 录像
 *   sm.startRecord(640, 480, 30);
 *   while (recording) {
 *       sm.writeRecordFrame(jpeg_data, jpeg_len);
 *   }
 *   sm.stopRecord();
 *
 *   // 空间管理
 *   if (sm.getFreeSpaceMB() < 50) sm.autoCleanup(100);
 * @endcode
 */

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

// ============================================================
// AVI 容器格式相关结构体（RIFF 格式）
// ============================================================

#pragma pack(push, 1)

/** @brief RIFF 块头 */
struct RiffChunk {
    uint32_t fourcc;   // 块标识，如 "RIFF", "LIST", "avih", "strh", "movi"
    uint32_t size;     // 后续数据大小（不含 fourcc + size 自身）
};

/** @brief AVI 主文件头 (avih) */
struct AviMainHeader {
    uint32_t dwMicroSecPerFrame;    // 每帧间隔（微秒），1000000/fps
    uint32_t dwMaxBytesPerSec;      // 最大数据速率（字节/秒）
    uint32_t dwPaddingGranularity;  // 对齐粒度（通常为 0）
    uint32_t dwFlags;               // 标志位
                                     //   0x0010 = 包含 idx1 索引
                                     //   0x0100 = 交错存储
                                     //   0x1000 = 受信任
    uint32_t dwTotalFrames;         // 总帧数（录制开始时未知，填 0）
    uint32_t dwInitialFrames;       // 初始帧（通常为 0）
    uint32_t dwStreams;             // 流数量（1 = 只有视频流）
    uint32_t dwSuggestedBufferSize; // 建议缓冲区大小
    uint32_t dwWidth;               // 视频宽度
    uint32_t dwHeight;              // 视频高度
    uint32_t dwReserved[4];         // 保留
};

/** @brief AVI 流头 (strh) */
struct AviStreamHeader {
    uint32_t fccType;    // 流类型: "vids" — 视频流
    uint32_t fccHandler; // 编码器: "MJPG" — MJPEG
    uint32_t dwFlags;    // 标志位
    uint16_t wPriority;
    uint16_t wLanguage;
    uint32_t dwInitialFrames;
    uint32_t dwScale;    // 时间尺度，分母（恒 = frame_rate_denominator）
    uint32_t dwRate;     // 帧率，分子（恒 = fps）
    uint32_t dwStart;    // 起始帧号（0）
    uint32_t dwLength;   // 总帧数（未知时填 0）
    uint32_t dwSuggestedBufferSize;
    uint32_t dwQuality;
    uint32_t dwSampleSize;  // 样本大小（MJPEG 为 0，表示变长帧）
    struct {
        int16_t left;
        int16_t top;
        int16_t right;
        int16_t bottom;
    } rcFrame;  // 目标帧矩形
};

/** @brief BITMAPINFOHEADER (strf, MJPG 格式的视频帧信息) */
struct BitmapInfoHeader {
    uint32_t biSize;          // 本结构大小 = 40
    int32_t  biWidth;         // 宽
    int32_t  biHeight;        // 高
    uint16_t biPlanes;        // 1
    uint16_t biBitCount;      // 24 (JPEG 编码后存为 24bpp)
    uint32_t biCompression;   // "MJPG" — MJPEG
    uint32_t biSizeImage;     // 图像大小（0 = 变长）
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
};

/** @brief AVI 索引条目（idx1 中的每一项） */
struct AviIndexEntry {
    uint32_t ckid;    // 块标识，如 "00dc"
    uint32_t dwFlags; // 0x0010 = 关键帧
    uint32_t dwChunkOffset;  // 当前块相对 movi 的偏移
    uint32_t dwChunkLength;  // 块大小
};

/** @brief "00dc" 帧块格式 */
struct AviFrameChunk {
    uint32_t ckid;  // "00dc"
    uint32_t size;  // 帧数据大小
    // uint8_t data[size];  // JPEG 数据（可变长）
};

#pragma pack(pop)

/**
 * @brief 存储管理类
 *
 * 非拷贝、非移动。建议由主线程创建，通过指针共享给采集线程。
 */
class StorageManager {
public:
    /**
     * @brief 构造存储管理器
     * @param photoDir  照片保存根目录，如 "/data/photos"
     * @param videoDir  录像保存根目录，如 "/data/videos"
     */
    StorageManager(const std::string& photoDir = "/data/photos",
                   const std::string& videoDir = "/data/videos");

    ~StorageManager();

    // 禁用拷贝
    StorageManager(const StorageManager&) = delete;
    StorageManager& operator=(const StorageManager&) = delete;

    // ============================================================
    // 拍照
    // ============================================================

    /**
     * @brief 保存一张 JPEG 照片
     *
     * 自动在 photoDir 下按日期创建子目录，文件命名格式:
     *   IMG_20260523_143025.jpg
     *
     * @param jpeg_data  JPEG 图像数据
     * @param len        数据长度
     * @return 成功返回文件全路径，失败返回空字符串
     */
    std::string savePhoto(const uint8_t* jpeg_data, int len);

    // ============================================================
    // 录像 (MJPEG → AVI)
    // ============================================================

    /**
     * @brief 开始录像
     *
     * 创建 AVI 文件并写入文件头。
     * 文件命名格式: VID_20260523_143025.avi，目录: videoDir/日期/
     *
     * @param width   视频宽度
     * @param height  视频高度
     * @param fps     帧率
     * @return 0 成功，-1 失败（已在录制中 / 文件创建失败）
     */
    int startRecord(int width, int height, int fps);

    /**
     * @brief 写入一帧 MJPEG 数据到录像文件
     * @param jpeg_data  JPEG 帧数据
     * @param len        JPEG 帧大小
     * @return 0 成功，-1 未在录制
     */
    int writeRecordFrame(const uint8_t* jpeg_data, int len);

    /**
     * @brief 停止录像
     *
     * 回填 AVI 头中的总帧数，写入 idx1 索引块，关闭文件。
     * @return 0 成功
     */
    int stopRecord();

    /** @brief 是否正在录像 */
    bool isRecording() const { return m_recording; }

    /** @brief 获取当前录像路径（录制中有效） */
    std::string currentRecordPath() const;

    // ============================================================
    // 磁盘空间管理
    // ============================================================

    /**
     * @brief 获取指定路径的剩余空间
     * @param path  路径，默认使用 videoDir
     * @return 剩余空间（MB），失败返回 -1
     */
    int getFreeSpaceMB(const std::string& path = "");

    /**
     * @brief 自动清理旧文件，释放空间到指定阈值
     *
     * 按文件修改时间升序删除（最旧的先删），
     * 直到剩余空间 >= keep_mb MB 或无文件可删。
     *
     * @param keep_mb  至少要保留的空闲空间（MB）
     * @return 清理后剩余空间（MB）
     */
    int autoCleanup(int keep_mb = 100);

    // ============================================================
    // 相册浏览 (v0.2)
    // ============================================================

    /**
     * @brief 媒体文件元信息（照片 / 视频共用）
     */
    struct PhotoInfo {
        std::string path;         // 完整路径
        std::string filename;     // 文件名
        std::string dateStr;      // "2026-05-24"
        std::string timeStr;      // "14:30"
        time_t      timestamp;    // Unix 时间戳（用于排序）
        int         width;        // 宽（照片）或 0（视频）
        int         height;       // 高（照片）或 0（视频）
        size_t      fileSize;     // 文件大小（字节）
        bool        isVideo = false;  // true = AVI 视频，false = JPEG 照片
    };

    /** @brief 按日期分组 */
    struct PhotoDayGroup {
        std::string dateStr;                 // "2026-05-24"
        std::vector<PhotoInfo> photos;
    };

    /**
     * @brief 获取所有照片列表（按时间倒序，按日期分组）
     * @param out          输出分组列表
     * @param includeInfo  是否解析 JPEG 头部获取宽高（true 时较慢）
     * @return 照片总数
     */
    int listPhotos(std::vector<PhotoDayGroup>& out, bool includeInfo = false);

    /**
     * @brief 获取所有视频列表（按时间倒序，按日期分组）
     *
     * 扫描 m_videoDir 目录，收集所有 .avi 文件。
     * 分组逻辑和 listPhotos 相同，但 PhotoInfo::isVideo = true，
     * width/height 固定为 0（视频不解析 AVI 头）。
     *
     * @param out  输出分组列表
     * @return 视频总数
     */
    int listVideos(std::vector<PhotoDayGroup>& out);

    /** @brief 获取照片总数（快速，只读目录不读文件） */
    int getPhotoCount();

    /** @brief 获取视频总数（快速，只读目录不读文件） */
    int getVideoCount();

    /**
     * @brief 删除一张照片
     * @return 0 成功，-1 失败
     */
    int deletePhoto(const std::string& path);

    /**
     * @brief 删除一个视频文件
     * @return 0 成功，-1 失败
     */
    int deleteVideo(const std::string& path);

    /**
     * @brief 从 JPEG 文件头快速读取宽高（只读前 4KB，不解码像素）
     * @return true 成功
     */
    static bool readJpegSize(const std::string& path, int& w, int& h);

    /**
     * @brief 从 AVI 文件提取第一帧 JPEG 数据（用于视频缩略图 / 封面）
     *
     * 解析 AVI RIFF 容器，定位 movi LIST 中第一个 "00dc" 帧块，
     * 将其中的 MJPEG/JPEG 数据提取到 jpegData 向量中。
     * 只读取文件头部元数据 + 第一帧，不解码完整视频。
     *
     * @param aviPath   AVI 文件路径
     * @param jpegData  输出 JPEG 数据
     * @return true 成功提取，false 失败（文件不存在 / 格式不兼容等）
     */
    static bool extractAviThumbnail(const std::string& aviPath,
                                    std::vector<uint8_t>& jpegData);

    // ============================================================
    // 配置
    // ============================================================

    /** @brief 设置照片保存目录 */
    void setPhotoDir(const std::string& dir) { m_photoDir = dir; }

    /** @brief 设置录像保存目录 */
    void setVideoDir(const std::string& dir) { m_videoDir = dir; }

    /** @brief 获取当前照片目录 */
    const std::string& photoDir() const { return m_photoDir; }

private:
    // ---- 内部工具 ----

    /**
     * @brief 生成按日期组织的路径
     * @param base  根目录
     * @param prefix 文件名前缀（IMG / VID）
     * @param ext   文件扩展名（.jpg / .avi）
     * @return 完整路径，如 "/data/photos/20260523/IMG_20260523_143025.jpg"
     */
    std::string makeDatePath(const std::string& base,
                             const std::string& prefix,
                             const std::string& ext);

    /**
     * @brief 确保目录存在（递归创建）
     */
    bool ensureDir(const std::string& path);

    // ---- AVI 编解码 ----

    /**
     * @brief 写入 AVI 文件头（RIFF + hdrl 列表）
     * @return 写入的字节数，-1 失败
     */
    int writeAviHeader();

    /**
     * @brief 回填总帧数到 AVI 文件头 & 写入 idx1 索引
     * @return 0 成功
     */
    int finalizeAvi();

    /** @brief 写入 4 字节 FOURCC */
    static void writeFourCC(FILE* fp, const char fourcc[4]);

    /** @brief 写入小端序 uint32 */
    static void writeU32(FILE* fp, uint32_t val);

    /** @brief 写入小端序 uint16 */
    static void writeU16(FILE* fp, uint16_t val);

    // ---- 目录 ----
    std::string m_photoDir;
    std::string m_videoDir;

    // ---- 录像状态 ----
    std::atomic<bool> m_recording{false};
    mutable std::mutex m_recordMtx;
    FILE*             m_recordFile = nullptr;
    std::string       m_recordPath;
    int               m_recordWidth  = 0;
    int               m_recordHeight = 0;
    int               m_recordFps    = 0;
    int               m_recordFrameCount = 0;

    // JPEG 帧索引（用于 idx1 回写）
    struct FrameIndex {
        uint32_t offset;  // 相对于 movi 数据区起始
        uint32_t length;  // 帧数据大小
    };
    std::vector<FrameIndex> m_frameIndexList;

    // movi chunk 在文件中的偏移（用于回填大小）
    long m_moviDataOffset = 0;   // 第一个 "00dc" 数据起始偏移
    long m_hdrlListOffset = 0;   // hdrl LIST 的 size 字段位置
    long m_rifSizeOffset  = 0;   // RIFF 文件 size 字段位置
    long m_strhLengthOffset = 0; // strh 中 dwLength 偏移
    long m_avihFramesOffset = 0; // avih 中 dwTotalFrames 偏移
};

#endif // SMART_CAM_STORAGE_MANAGER_H
