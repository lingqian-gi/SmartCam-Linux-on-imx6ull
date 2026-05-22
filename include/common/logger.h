#ifndef SMART_CAM_COMMON_LOGGER_H
#define SMART_CAM_COMMON_LOGGER_H

/**
 * @file    logger.h
 * @brief   简单日志系统 — 支持控制台输出和 syslog 集成
 *
 * 日志级别: DEBUG < INFO < WARN < ERROR
 * 线程安全：每个日志调用是原子的
 *
 * 用法:
 * @code
 *   LOG_INFO("Camera init OK, device=%s", "/dev/video0");
 *   LOG_ERROR("Failed to set format: %d", errno);
 *   LOG_DEBUG("Frame #%d: %d bytes", index, len);
 * @endcode
 */

#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <cstring>
#include <mutex>
#include <string>

// syslog 仅 Linux 可用
#ifndef _WIN32
#include <syslog.h>
#endif

/**
 * @brief 日志级别
 */
enum class LogLevel {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3,
    NONE  = 4   // 关闭所有日志
};

/**
 * @brief 日志管理器（单例）
 *
 * 支持:
 *   - 控制台输出（彩色）
 *   - syslog 输出（Linux 系统日志）
 *   - 日志级别过滤
 *   - 可选的时间戳打印
 */
class Logger {
public:
    static Logger* instance() {
        static Logger inst;
        return &inst;
    }

    // 禁用拷贝
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /**
     * @brief 设置日志级别
     */
    void setLevel(LogLevel level) { m_level = level; }

    /**
     * @brief 获取当前日志级别
     */
    LogLevel level() const { return m_level; }

    /**
     * @brief 设置是否启用 syslog
     */
    void setSyslogEnabled(bool enabled) {
#ifndef _WIN32
        m_useSyslog = enabled;
#endif
    }

    /**
     * @brief 设置是否打印时间戳
     */
    void setTimestampEnabled(bool enabled) { m_showTimestamp = enabled; }

    /**
     * @brief 核心日志方法
     */
    void log(LogLevel level, const char* file, int line,
             const char* func, const char* fmt, ...) {
        if (level < m_level) return;

        std::lock_guard<std::mutex> lock(m_mtx);

        // 格式化的消息
        char msg[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);

        output(level, file, line, func, msg);
    }

private:
    Logger()
        : m_level(LogLevel::DEBUG)
        , m_useSyslog(false)
        , m_showTimestamp(true)
    {
#ifdef SMART_CAM_USE_SYSLOG
        m_useSyslog = true;
#endif
    }

    ~Logger() {
#ifndef _WIN32
        if (m_useSyslog) {
            // closelog 由系统自动调用
        }
#endif
    }

    void output(LogLevel level, const char* file, int line,
                const char* func, const char* msg) {
        const char* levelStr = "";
        const char* color    = "";

        switch (level) {
        case LogLevel::DEBUG: levelStr = "DEBUG"; color = "\033[36m"; break;  // 青
        case LogLevel::INFO:  levelStr = "INFO";  color = "\033[32m"; break;  // 绿
        case LogLevel::WARN:  levelStr = "WARN";  color = "\033[33m"; break;  // 黄
        case LogLevel::ERROR: levelStr = "ERROR"; color = "\033[31m"; break;  // 红
        default: break;
        }

        // 简易文件名（去掉路径）
        const char* shortFile = file;
        const char* slash = strrchr(file, '/');
        if (slash) shortFile = slash + 1;

        // 时间戳
        char timebuf[32] = {0};
        if (m_showTimestamp) {
            time_t now = time(nullptr);
            struct tm* tm_info = localtime(&now);
            strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info);
        }

        // 控制台输出（带颜色）
        if (m_showTimestamp) {
            fprintf(stdout, "%s %s[%s]%s %s:%d (%s) %s\n",
                    timebuf, color, levelStr, "\033[0m",
                    shortFile, line, func, msg);
        } else {
            fprintf(stdout, "%s[%s]%s %s:%d (%s) %s\n",
                    color, levelStr, "\033[0m",
                    shortFile, line, func, msg);
        }
        fflush(stdout);

        // syslog 输出
#ifndef _WIN32
        if (m_useSyslog) {
            static bool syslogOpened = false;
            if (!syslogOpened) {
                openlog("smartcam", LOG_PID | LOG_NDELAY, LOG_USER);
                syslogOpened = true;
            }
            int prio = LOG_INFO;
            switch (level) {
            case LogLevel::DEBUG: prio = LOG_DEBUG;   break;
            case LogLevel::INFO:  prio = LOG_INFO;     break;
            case LogLevel::WARN:  prio = LOG_WARNING;  break;
            case LogLevel::ERROR: prio = LOG_ERR;      break;
            default: break;
            }
            syslog(prio, "%s:%d (%s) %s", shortFile, line, func, msg);
        }
#endif
    }

    LogLevel  m_level;
    bool      m_useSyslog;
    bool      m_showTimestamp;
    std::mutex m_mtx;
};

// ============================================================
// 便捷宏 — 避免与 syslog.h 的常量宏冲突（LOG_DEBUG, LOG_INFO, LOG_ERR 等）
// ============================================================

#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARNING
#undef LOG_ERR

#define LOG_DBG(fmt, ...) \
    Logger::instance()->log(LogLevel::DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_INF(fmt, ...) \
    Logger::instance()->log(LogLevel::INFO,  __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_WRN(fmt, ...) \
    Logger::instance()->log(LogLevel::WARN,  __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_ERR_(fmt, ...) \
    Logger::instance()->log(LogLevel::ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#endif // SMART_CAM_COMMON_LOGGER_H
