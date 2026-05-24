/**
 * @file    manager.cpp
 * @brief   StorageManager 实现 — 拍照、AVI 录像、磁盘空间管理
 */

#include "include/storage/manager.h"
#include "include/common/logger.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <sstream>
#include <iomanip>
#include <dirent.h>

// ============================================================
// 构造 / 析构
// ============================================================

StorageManager::StorageManager(const std::string& photoDir,
                               const std::string& videoDir)
    : m_photoDir(photoDir)
    , m_videoDir(videoDir)
{
    // 确保根目录存在
    ensureDir(m_photoDir);
    ensureDir(m_videoDir);
    LOG_INF("StorageManager init: photo=%s, video=%s",
            m_photoDir.c_str(), m_videoDir.c_str());
}

StorageManager::~StorageManager() {
    if (m_recording) {
        LOG_INF("StorageManager dtor: stopping active recording");
        stopRecord();
    }
}

// ============================================================
// 目录工具
// ============================================================

std::string StorageManager::makeDatePath(const std::string& base,
                                         const std::string& prefix,
                                         const std::string& ext) {
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);

    char dateDir[16];   // 如 "20260523"
    strftime(dateDir, sizeof(dateDir), "%Y%m%d", tm_info);

    char timeStr[16];   // 如 "143025"
    strftime(timeStr, sizeof(timeStr), "%H%M%S", tm_info);

    std::string fullDir = base + "/" + dateDir;
    ensureDir(fullDir);

    // 完整文件名
    std::ostringstream oss;
    oss << fullDir << "/" << prefix << "_" << dateDir << "_" << timeStr << ext;
    return oss.str();
}

bool StorageManager::ensureDir(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) return true;
        LOG_ERR_("ensureDir: path exists but is not a directory: %s",
                 path.c_str());
        return false;
    }

    // 递归创建
    if (mkdir(path.c_str(), 0755) == 0) return true;

    if (errno == ENOENT) {
        // 父目录不存在，递归
        size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0) {
            if (!ensureDir(path.substr(0, slash))) return false;
            return (mkdir(path.c_str(), 0755) == 0);
        }
    }

    LOG_ERR_("ensureDir: mkdir(%s) failed: %s", path.c_str(), strerror(errno));
    return false;
}

// ============================================================
// 拍照
// ============================================================

std::string StorageManager::savePhoto(const uint8_t* jpeg_data, int len) {
    if (!jpeg_data || len <= 0) {
        LOG_ERR_("savePhoto: invalid data (ptr=%p, len=%d)",
                 static_cast<const void*>(jpeg_data), len);
        return "";
    }

    std::string path = makeDatePath(m_photoDir, "IMG", ".jpg");

    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) {
        LOG_ERR_("savePhoto: fopen(%s) failed: %s",
                 path.c_str(), strerror(errno));
        return "";
    }

    size_t written = fwrite(jpeg_data, 1, static_cast<size_t>(len), fp);
    fclose(fp);

    if (written != static_cast<size_t>(len)) {
        LOG_ERR_("savePhoto: fwrite short: expected=%d, written=%zu",
                 len, written);
        unlink(path.c_str());
        return "";
    }

    LOG_INF("Photo saved: %s (%d bytes)", path.c_str(), len);
    return path;
}

// ============================================================
// 录像 (MJPEG → AVI)
// ============================================================

int StorageManager::startRecord(int width, int height, int fps) {
    std::lock_guard<std::mutex> lock(m_recordMtx);

    if (m_recording) {
        LOG_WRN("Already recording, stop() first");
        return -1;
    }

    if (width <= 0 || height <= 0 || fps <= 0) {
        LOG_ERR_("startRecord: invalid params (%dx%d @ %d)", width, height, fps);
        return -1;
    }

    std::string path = makeDatePath(m_videoDir, "VID", ".avi");

    m_recordFile = fopen(path.c_str(), "wb");
    if (!m_recordFile) {
        LOG_ERR_("startRecord: fopen(%s) failed: %s",
                 path.c_str(), strerror(errno));
        return -1;
    }

    m_recordPath      = path;
    m_recordWidth     = width;
    m_recordHeight    = height;
    m_recordFps       = fps;
    m_recordFrameCount = 0;
    m_frameIndexList.clear();
    m_moviDataOffset  = 0;
    m_hdrlListOffset  = 0;
    m_rifSizeOffset   = 0;
    m_strhLengthOffset = 0;
    m_avihFramesOffset = 0;

    // 写入 AVI 文件头
    if (writeAviHeader() < 0) {
        LOG_ERR_("startRecord: writeAviHeader failed");
        fclose(m_recordFile);
        m_recordFile = nullptr;
        unlink(path.c_str());
        return -1;
    }

    m_recording = true;
    LOG_INF("Recording started: %s (%dx%d @ %dfps)",
            m_recordPath.c_str(), width, height, fps);
    return 0;
}

int StorageManager::writeRecordFrame(const uint8_t* jpeg_data, int len) {
    std::lock_guard<std::mutex> lock(m_recordMtx);

    if (!m_recording || !m_recordFile || !jpeg_data || len <= 0) {
        return -1;
    }

    // 记录当前帧在 movi 数据区的偏移（用于 idx1）
    long currentPos = ftell(m_recordFile);
    long frameDataOffset = currentPos;  // 数据区起始偏移

    // 写入 "00dc" 帧块
    writeFourCC(m_recordFile, "00dc");
    writeU32(m_recordFile, static_cast<uint32_t>(len));
    size_t written = fwrite(jpeg_data, 1, static_cast<size_t>(len), m_recordFile);
    fflush(m_recordFile);

    if (written != static_cast<size_t>(len)) {
        LOG_ERR_("writeRecordFrame: fwrite short (frame=%d): expected=%d, got=%zu",
                 m_recordFrameCount, len, written);
        return -1;
    }

    // 如果是第一帧，记录 movi 数据区起始偏移
    if (m_recordFrameCount == 0) {
        m_moviDataOffset = frameDataOffset;
    }

    // 记录帧索引
    FrameIndex idx;
    idx.offset = static_cast<uint32_t>(frameDataOffset - m_moviDataOffset);
    idx.length = static_cast<uint32_t>(sizeof(AviFrameChunk) + len);
    m_frameIndexList.push_back(idx);

    m_recordFrameCount++;
    return 0;
}

int StorageManager::stopRecord() {
    std::lock_guard<std::mutex> lock(m_recordMtx);

    if (!m_recording || !m_recordFile) {
        LOG_WRN("stopRecord: not recording");
        return -1;
    }

    m_recording = false;

    LOG_INF("Recording stopping: wrote %d frames", m_recordFrameCount);

    // 回填 AVI 头 + 写 idx1 索引
    int ret = finalizeAvi();

    fclose(m_recordFile);
    m_recordFile = nullptr;
    m_frameIndexList.clear();

    if (ret < 0) {
        LOG_ERR_("finalizeAvi failed, file may be corrupt: %s",
                 m_recordPath.c_str());
    } else {
        LOG_INF("Recording saved: %s (%d frames, %dx%d @ %dfps)",
                m_recordPath.c_str(), m_recordFrameCount,
                m_recordWidth, m_recordHeight, m_recordFps);
    }

    return (ret < 0) ? -1 : 0;
}

std::string StorageManager::currentRecordPath() const {
    std::lock_guard<std::mutex> lock(m_recordMtx);
    return m_recordPath;
}

// ============================================================
// AVI 编解码内部实现
// ============================================================

int StorageManager::writeAviHeader() {
    if (!m_recordFile) return -1;

    FILE* fp = m_recordFile;

    // ---- RIFF 头 ----
    writeFourCC(fp, "RIFF");
    m_rifSizeOffset = ftell(fp);
    writeU32(fp, 0);           // 文件总大小，稍后回填

    writeFourCC(fp, "AVI ");   // 形式类型

    // ---- LIST: hdrl ----
    writeFourCC(fp, "LIST");
    m_hdrlListOffset = ftell(fp);
    writeU32(fp, 0);           // hdrl LIST 大小，稍后回填

    writeFourCC(fp, "hdrl");

    // ---- avih (AVI 主文件头) ----
    writeFourCC(fp, "avih");
    writeU32(fp, sizeof(AviMainHeader));

    AviMainHeader avih;
    memset(&avih, 0, sizeof(avih));
    avih.dwMicroSecPerFrame    = static_cast<uint32_t>(1000000.0 / m_recordFps);
    avih.dwMaxBytesPerSec      = 0;   // 未知
    avih.dwPaddingGranularity  = 0;
    avih.dwFlags               = 0x10; // 包含 idx1 索引
    avih.dwTotalFrames         = 0;   // 稍后回填
    avih.dwInitialFrames       = 0;
    avih.dwStreams             = 1;
    avih.dwSuggestedBufferSize = static_cast<uint32_t>(m_recordWidth * m_recordHeight * 3);
    avih.dwWidth               = static_cast<uint32_t>(m_recordWidth);
    avih.dwHeight              = static_cast<uint32_t>(m_recordHeight);

    fwrite(&avih, sizeof(avih), 1, fp);
    // 记录 dwTotalFrames 偏移（avih 起始 + 16 字节）
    m_avihFramesOffset = ftell(fp) - sizeof(AviMainHeader)
                         + offsetof(AviMainHeader, dwTotalFrames);

    // ---- LIST: strl ----
    writeFourCC(fp, "LIST");
    size_t strlSizePos = ftell(fp);
    writeU32(fp, 0);           // strl LIST 大小，后续回填
    writeFourCC(fp, "strl");

    // ---- strh ----
    writeFourCC(fp, "strh");
    writeU32(fp, sizeof(AviStreamHeader));

    AviStreamHeader strh;
    memset(&strh, 0, sizeof(strh));
    strh.fccType    = 0x73646976;  // "vids"
    strh.fccHandler = 0x47504A4D;  // "MJPG"
    strh.dwFlags    = 0;
    strh.wPriority  = 0;
    strh.wLanguage  = 0;
    strh.dwInitialFrames = 0;
    strh.dwScale    = 1;
    strh.dwRate     = static_cast<uint32_t>(m_recordFps);
    strh.dwStart    = 0;
    strh.dwLength   = 0;     // 稍后回填
    strh.dwSuggestedBufferSize = static_cast<uint32_t>(m_recordWidth * m_recordHeight * 3);
    strh.dwQuality  = 0xFFFFFFFF;
    strh.dwSampleSize = 0;   // 变长帧
    strh.rcFrame.left   = 0;
    strh.rcFrame.top    = 0;
    strh.rcFrame.right  = static_cast<int16_t>(m_recordWidth);
    strh.rcFrame.bottom = static_cast<int16_t>(m_recordHeight);

    fwrite(&strh, sizeof(strh), 1, fp);
    // 记录 dwLength 偏移
    m_strhLengthOffset = ftell(fp) - sizeof(AviStreamHeader)
                         + offsetof(AviStreamHeader, dwLength);

    // ---- strf (BITMAPINFOHEADER) ----
    writeFourCC(fp, "strf");
    writeU32(fp, sizeof(BitmapInfoHeader));

    BitmapInfoHeader bih;
    memset(&bih, 0, sizeof(bih));
    bih.biSize          = sizeof(BitmapInfoHeader);
    bih.biWidth         = m_recordWidth;
    bih.biHeight        = m_recordHeight;
    bih.biPlanes        = 1;
    bih.biBitCount      = 24;
    bih.biCompression   = 0x47504A4D;  // "MJPG"
    bih.biSizeImage     = 0;  // 变长
    bih.biXPelsPerMeter = 0;
    bih.biYPelsPerMeter = 0;
    bih.biClrUsed       = 0;
    bih.biClrImportant  = 0;

    fwrite(&bih, sizeof(bih), 1, fp);

    // ---- 回填 strl LIST 大小 ----
    long strlEnd = ftell(fp);
    long strlSize = strlEnd - (long)(strlSizePos + 4);
    fseek(fp, strlSizePos, SEEK_SET);
    writeU32(fp, static_cast<uint32_t>(strlSize));
    fseek(fp, strlEnd, SEEK_SET);

    // ---- 回填 hdrl LIST 大小 ----
    long hdrlEnd = ftell(fp);
    long hdrlSize = hdrlEnd - (long)(m_hdrlListOffset + 4);
    fseek(fp, m_hdrlListOffset, SEEK_SET);
    writeU32(fp, static_cast<uint32_t>(hdrlSize));
    fseek(fp, hdrlEnd, SEEK_SET);

    // ---- LIST: movi ----
    writeFourCC(fp, "LIST");
    long moviSizePos = ftell(fp);
    writeU32(fp, 0);           // movi size，稍后回填
    writeFourCC(fp, "movi");

    fflush(fp);

    // 记录 "movi" 块头写入后的偏移，使得帧索引可回填
    // （帧数据在客户端调用 writeRecordFrame 时追加）

    return 0;
}

int StorageManager::finalizeAvi() {
    if (!m_recordFile || m_frameIndexList.empty()) return -1;

    FILE* fp = m_recordFile;

    // ---- 回填 movi LIST 大小 ----
    // movi LIST 的 size 字段位于 m_moviDataOffset - 4 处
    long moviEnd = ftell(fp);
    long moviSize = moviEnd - m_moviDataOffset + 4;  // +4 补上 "movi" FOURCC
    long moviSizePos = m_moviDataOffset - 4;           // movi LIST size 位置
    fseek(fp, moviSizePos, SEEK_SET);
    writeU32(fp, static_cast<uint32_t>(moviSize));
    fseek(fp, moviEnd, SEEK_SET);

    // ---- 写入 idx1 索引块 ----
    writeFourCC(fp, "idx1");
    uint32_t idx1Size = static_cast<uint32_t>(m_frameIndexList.size()
                                              * sizeof(AviIndexEntry));
    writeU32(fp, idx1Size);

    for (size_t i = 0; i < m_frameIndexList.size(); ++i) {
        AviIndexEntry entry;
        entry.ckid    = 0x63643030;  // "00dc"
        entry.dwFlags = 0x10;         // 关键帧
        entry.dwChunkOffset = m_frameIndexList[i].offset;
        entry.dwChunkLength = m_frameIndexList[i].length;
        fwrite(&entry, sizeof(entry), 1, fp);
    }

    // ---- 回填 avih.dwTotalFrames 和 strh.dwLength ----
    if (m_avihFramesOffset > 0) {
        fseek(fp, m_avihFramesOffset, SEEK_SET);
        writeU32(fp, static_cast<uint32_t>(m_frameIndexList.size()));
    }
    if (m_strhLengthOffset > 0) {
        fseek(fp, m_strhLengthOffset, SEEK_SET);
        writeU32(fp, static_cast<uint32_t>(m_frameIndexList.size()));
    }

    // ---- 回填 RIFF 文件总大小 ----
    long riffEnd = ftell(fp);
    long riffSize = riffEnd - 8;  // RIFF size = 文件大小 - 8 (FOURCC+size自身)
    if (m_rifSizeOffset > 0) {
        fseek(fp, m_rifSizeOffset, SEEK_SET);
        writeU32(fp, static_cast<uint32_t>(riffSize));
    }

    // 回到文件末尾
    fseek(fp, 0, SEEK_END);
    fflush(fp);

    return 0;
}

// ============================================================
// 辅助写入函数
// ============================================================

void StorageManager::writeFourCC(FILE* fp, const char fourcc[4]) {
    fwrite(fourcc, 1, 4, fp);
}

void StorageManager::writeU32(FILE* fp, uint32_t val) {
    fwrite(&val, sizeof(val), 1, fp);
}

void StorageManager::writeU16(FILE* fp, uint16_t val) {
    fwrite(&val, sizeof(val), 1, fp);
}

// ============================================================
// 磁盘空间管理
// ============================================================

int StorageManager::getFreeSpaceMB(const std::string& path) {
    std::string checkPath = path.empty() ? m_videoDir : path;

    struct statvfs vfs;
    if (statvfs(checkPath.c_str(), &vfs) < 0) {
        LOG_ERR_("getFreeSpaceMB: statvfs(%s) failed: %s",
                 checkPath.c_str(), strerror(errno));
        return -1;
    }

    // statvfs 返回可用块数 × 块大小（字节）
    unsigned long long freeBytes =
        static_cast<unsigned long long>(vfs.f_bsize) *
        static_cast<unsigned long long>(vfs.f_bavail);
    return static_cast<int>(freeBytes / (1024 * 1024));
}

int StorageManager::autoCleanup(int keep_mb) {
    // 收集两个目录中所有照片和视频文件
    struct FileEntry {
        std::string path;
        time_t      mtime;
    };
    std::vector<FileEntry> files;

    auto collectFiles = [&files](const std::string& dir) {
        DIR* dp = opendir(dir.c_str());
        if (!dp) {
            if (errno != ENOENT) {
                LOG_WRN("autoCleanup: opendir(%s) failed: %s",
                        dir.c_str(), strerror(errno));
            }
            return;
        }

        struct dirent* entry;
        while ((entry = readdir(dp)) != nullptr) {
            // 跳过 . 和 ..
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            // 只处理日期目录 (8位数字)
            if (strlen(entry->d_name) != 8) continue;
            bool allDigit = true;
            for (int i = 0; i < 8; ++i) {
                if (entry->d_name[i] < '0' || entry->d_name[i] > '9') {
                    allDigit = false;
                    break;
                }
            }
            if (!allDigit) continue;

            // 遍历日期目录中的文件
            std::string dateDir = dir + "/" + entry->d_name;
            DIR* dp2 = opendir(dateDir.c_str());
            if (!dp2) continue;

            struct dirent* file;
            while ((file = readdir(dp2)) != nullptr) {
                if (file->d_type != DT_REG) continue;
                std::string fullPath = dateDir + "/" + file->d_name;
                struct stat st;
                if (stat(fullPath.c_str(), &st) == 0) {
                    files.push_back({fullPath, st.st_mtime});
                }
            }
            closedir(dp2);
        }
        closedir(dp);
    };

    collectFiles(m_photoDir);
    collectFiles(m_videoDir);

    if (files.empty()) {
        LOG_INF("autoCleanup: no files to clean");
        return getFreeSpaceMB();
    }

    // 按修改时间升序排序（最旧的在前）
    std::sort(files.begin(), files.end(),
              [](const FileEntry& a, const FileEntry& b) {
                  return a.mtime < b.mtime;
              });

    int currentFree = getFreeSpaceMB();
    LOG_INF("autoCleanup: current free=%d MB, target=%d MB, files=%zu",
            currentFree, keep_mb, files.size());

    // 删除旧文件直到满足阈值
    for (const auto& fe : files) {
        if (currentFree >= keep_mb) break;

        if (unlink(fe.path.c_str()) == 0) {
            LOG_DBG("autoCleanup: deleted %s", fe.path.c_str());
        } else {
            LOG_WRN("autoCleanup: unlink(%s) failed: %s",
                    fe.path.c_str(), strerror(errno));
        }

        // 重新检测（删除后不立即触发 statvfs 刷新，估算）
        struct stat st;
        if (stat(fe.path.c_str(), &st) != 0) {
            // 文件已删除，估算释放空间
            // 简单方式：重新查询
        }
    }

    // 清理删除文件后留下的空日期目录
    currentFree = getFreeSpaceMB();
    LOG_INF("autoCleanup: done, free=%d MB", currentFree);
    return currentFree;
}

// ============================================================
// 相册浏览 (v0.2)
// ============================================================

bool StorageManager::readJpegSize(const std::string& path, int& w, int& h) {
    w = 0; h = 0;
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;

    uint8_t buf[4096];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);

    // 检查 JPEG SOI 标记
    if (n < 100 || buf[0] != 0xFF || buf[1] != 0xD8) return false;

    // 扫描标记段，查找 SOF0 (0xFF 0xC0) 标记
    for (size_t i = 2; i < n - 8; i++) {
        if (buf[i] == 0xFF) {
            uint8_t marker = buf[i + 1];
            // 跳过填充字节 (0xFF 0xFF)
            if (marker == 0xFF) continue;
            // 跳过无数据标记 (D0-D7, D8=SOI, D9=EOI, DA=SOS)
            if (marker >= 0xD0 && marker <= 0xD7) continue;

            // SOF0 (Baseline DCT) 或 SOF1 (Extended): 0xC0-0xC2
            if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
                if (i + 8 >= n) return false;
                h = (buf[i + 5] << 8) | buf[i + 6];
                w = (buf[i + 7] << 8) | buf[i + 8];
                return (w > 0 && h > 0);
            }

            // 跳过变长段: 读取长度
            if (i + 3 < n) {
                uint16_t segLen = (buf[i + 2] << 8) | buf[i + 3];
                if (segLen >= 2) i += segLen + 1;  // +1 because loop does i++
            }
        }
    }

    return false;
}

int StorageManager::listPhotos(std::vector<PhotoDayGroup>& out,
                               bool includeInfo) {
    out.clear();
    int totalCount = 0;

    DIR* dp = opendir(m_photoDir.c_str());
    if (!dp) {
        if (errno != ENOENT) {
            LOG_WRN("listPhotos: opendir(%s) failed: %s",
                    m_photoDir.c_str(), strerror(errno));
        }
        return 0;
    }

    // 收集所有文件 (path → mtime)
    struct RawEntry {
        std::string path;
        time_t      mtime;
    };
    std::vector<RawEntry> rawEntries;

    struct dirent* entry;
    while ((entry = readdir(dp)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        if (entry->d_type == DT_REG) {
            // 根目录直接文件（兼容旧格式）
            if (strstr(entry->d_name, ".jpg") || strstr(entry->d_name, ".JPG")) {
                std::string fp = m_photoDir + "/" + entry->d_name;
                struct stat st;
                if (stat(fp.c_str(), &st) == 0) {
                    rawEntries.push_back({fp, st.st_mtime});
                }
            }
        } else if (entry->d_type == DT_DIR) {
            // 日期子目录
            std::string dateDir = m_photoDir + "/" + entry->d_name;
            DIR* dp2 = opendir(dateDir.c_str());
            if (!dp2) continue;

            struct dirent* file;
            while ((file = readdir(dp2)) != nullptr) {
                if (file->d_type != DT_REG) continue;
                if (!strstr(file->d_name, ".jpg") && !strstr(file->d_name, ".JPG"))
                    continue;

                std::string fp = dateDir + "/" + file->d_name;
                struct stat st;
                if (stat(fp.c_str(), &st) == 0) {
                    rawEntries.push_back({fp, st.st_mtime});
                }
            }
            closedir(dp2);
        }
    }
    closedir(dp);

    if (rawEntries.empty()) return 0;

    // 按时间倒序排序
    std::sort(rawEntries.begin(), rawEntries.end(),
              [](const RawEntry& a, const RawEntry& b) {
                  return a.mtime > b.mtime;
              });

    // 解析信息 & 按日期分组
    std::map<std::string, PhotoDayGroup> groupMap;

    for (const auto& re : rawEntries) {
        PhotoInfo info;
        info.path = re.path;
        info.timestamp = re.mtime;

        // 提取文件名
        size_t slash = re.path.rfind('/');
        info.filename = (slash != std::string::npos)
            ? re.path.substr(slash + 1) : re.path;

        // 格式化日期 & 时间
        struct tm* tm_info = localtime(&re.mtime);
        char dateBuf[16], timeBuf[16];
        strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", tm_info);
        strftime(timeBuf, sizeof(timeBuf), "%H:%M",   tm_info);
        info.dateStr = dateBuf;
        info.timeStr = timeBuf;
        info.fileSize = 0;

        // 文件大小 & 可选宽高
        struct stat st;
        if (stat(re.path.c_str(), &st) == 0) {
            info.fileSize = static_cast<size_t>(st.st_size);
        }

        info.width  = 0;
        info.height = 0;
        if (includeInfo) {
            readJpegSize(re.path, info.width, info.height);
        }

        auto& group = groupMap[info.dateStr];
        if (group.dateStr.empty()) group.dateStr = info.dateStr;
        group.photos.push_back(std::move(info));
        totalCount++;
    }

    // 转换为 vector（保持日期倒序）
    for (auto it = groupMap.rbegin(); it != groupMap.rend(); ++it) {
        out.push_back(std::move(it->second));
    }

    return totalCount;
}

int StorageManager::getPhotoCount() {
    int count = 0;
    DIR* dp = opendir(m_photoDir.c_str());
    if (!dp) return 0;

    struct dirent* entry;
    while ((entry = readdir(dp)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        if (entry->d_type == DT_REG &&
            (strstr(entry->d_name, ".jpg") || strstr(entry->d_name, ".JPG"))) {
            count++;
        } else if (entry->d_type == DT_DIR) {
            std::string dateDir = m_photoDir + "/" + entry->d_name;
            DIR* dp2 = opendir(dateDir.c_str());
            if (!dp2) continue;
            struct dirent* file;
            while ((file = readdir(dp2)) != nullptr) {
                if (file->d_type == DT_REG &&
                    (strstr(file->d_name, ".jpg") || strstr(file->d_name, ".JPG"))) {
                    count++;
                }
            }
            closedir(dp2);
        }
    }
    closedir(dp);
    return count;
}

int StorageManager::deletePhoto(const std::string& path) {
    if (unlink(path.c_str()) != 0) {
        LOG_ERR_("deletePhoto: unlink(%s) failed: %s",
                 path.c_str(), strerror(errno));
        return -1;
    }

    LOG_INF("Photo deleted: %s", path.c_str());

    // 尝试清理空的日期目录
    size_t slash = path.rfind('/');
    if (slash != std::string::npos) {
        std::string dir = path.substr(0, slash);
        // 仅当目录在 photoDir 里时才清理
        if (dir.find(m_photoDir) == 0 && dir != m_photoDir) {
            rmdir(dir.c_str());  // 忽略返回值：非空目录会失败，无害
        }
    }

    return 0;
}
