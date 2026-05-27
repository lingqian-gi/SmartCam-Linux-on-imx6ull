#ifndef SMART_CAM_COMMON_CONFIG_H
#define SMART_CAM_COMMON_CONFIG_H

/**
 * @file    config.h
 * @brief   简单 INI 配置文件解析器 (header-only)
 *
 * 解析格式:
 *   [section]
 *   key = value
 *   # 这是注释
 *
 * 用法:
 *   ConfigManager cfg;
 *   cfg.load("/etc/smartcam/smartcam.conf");
 *   std::string dev = cfg.getString("camera", "device", "/dev/video0");
 *   int port = cfg.getInt("network", "http_port", 8080);
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>

class ConfigManager {
public:
    ConfigManager() = default;

    /**
     * @brief 从文件加载配置
     * @return true 加载成功，false 文件不存在或读取失败
     */
    bool load(const std::string& path) {
        m_path = path;
        m_data.clear();

        std::ifstream file(path);
        if (!file.is_open()) {
            return false;  // 文件不存在不是致命错误
        }

        std::string line;
        std::string currentSection;

        while (std::getline(file, line)) {
            // 去除行首尾空白
            line = trim(line);

            // 跳过空行和注释
            if (line.empty() || line[0] == '#' || line[0] == ';') {
                continue;
            }

            // 检测 section: [camera]
            if (line[0] == '[' && line.back() == ']') {
                currentSection = line.substr(1, line.size() - 2);
                currentSection = trim(currentSection);
                continue;
            }

            // 解析 key = value
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key   = trim(line.substr(0, eq));
            std::string value = trim(line.substr(eq + 1));

            // 去掉行尾注释（# 之后的内容，但不是值的一部分）
            // 简单处理：value 中最后一个 # 前若有空格则截断
            size_t hash = value.find('#');
            if (hash != std::string::npos && hash > 0 && value[hash - 1] == ' ') {
                value = trim(value.substr(0, hash));
            }

            if (!key.empty() && !currentSection.empty()) {
                m_data[currentSection][key] = value;
            }
        }

        return true;
    }

    /** @brief 获取配置文件的路径 */
    const std::string& path() const { return m_path; }

    /** @brief 检查某个 section 是否存在 */
    bool hasSection(const std::string& section) const {
        return m_data.find(section) != m_data.end();
    }

    // ============================================================
    // 类型安全取值
    // ============================================================

    /** @brief 获取字符串值 */
    std::string getString(const std::string& section,
                          const std::string& key,
                          const std::string& defaultValue = "") const {
        auto secIt = m_data.find(section);
        if (secIt == m_data.end()) return defaultValue;

        auto keyIt = secIt->second.find(key);
        if (keyIt == secIt->second.end()) return defaultValue;

        return keyIt->second;
    }

    /** @brief 获取整数值 */
    int getInt(const std::string& section,
               const std::string& key,
               int defaultValue = 0) const {
        std::string val = getString(section, key);
        if (val.empty()) return defaultValue;
        return std::atoi(val.c_str());
    }

    /** @brief 获取布尔值 (true/yes/1/on) */
    bool getBool(const std::string& section,
                 const std::string& key,
                 bool defaultValue = false) const {
        std::string val = getString(section, key);
        if (val.empty()) return defaultValue;

        // 转为小写
        std::transform(val.begin(), val.end(), val.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        return (val == "true" || val == "yes" || val == "1" || val == "on");
    }

    /** @brief 检查某个 key 是否存在 */
    bool hasKey(const std::string& section, const std::string& key) const {
        auto secIt = m_data.find(section);
        if (secIt == m_data.end()) return false;
        return secIt->second.find(key) != secIt->second.end();
    }

    /** @brief 设置/修改字符串值（内存中） */
    void setString(const std::string& section,
                   const std::string& key,
                   const std::string& value) {
        m_data[section][key] = value;
    }

    /** @brief 将当前配置写回文件 */
    bool save() const {
        if (m_path.empty()) return false;

        std::ofstream file(m_path);
        if (!file.is_open()) return false;

        for (const auto& sec : m_data) {
            file << "[" << sec.first << "]\n";
            for (const auto& kv : sec.second) {
                file << kv.first << " = " << kv.second << "\n";
            }
            file << "\n";
        }

        return true;
    }

    /** @brief 保存到指定路径（自动创建父目录） */
    bool saveAs(const std::string& path) const {
        // 确保父目录存在
        size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0) {
            mkdirParents(path.substr(0, slash));
        }

        std::ofstream file(path);
        if (!file.is_open()) return false;

        for (const auto& sec : m_data) {
            file << "[" << sec.first << "]\n";
            for (const auto& kv : sec.second) {
                file << kv.first << " = " << kv.second << "\n";
            }
            file << "\n";
        }

        return true;
    }

private:
    static std::string trim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    /** @brief 递归创建目录 (类似 mkdir -p) */
    static void mkdirParents(const std::string& path) {
        if (path.empty() || path == "/") return;
        std::string parent = path;
        size_t slash = parent.rfind('/');
        if (slash != std::string::npos && slash > 0) {
            mkdirParents(parent.substr(0, slash));
        }
        mkdir(path.c_str(), 0755);  // 忽略 EEXIST 错误
    }

    std::string m_path;
    std::map<std::string, std::map<std::string, std::string>> m_data;
};

#endif // SMART_CAM_COMMON_CONFIG_H
