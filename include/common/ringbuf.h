#ifndef SMART_CAM_COMMON_RINGBUF_H
#define SMART_CAM_COMMON_RINGBUF_H

/**
 * @file    ringbuf.h
 * @brief   线程安全环形缓冲区模板
 *
 * 用于 V4L2 帧缓冲池管理，也适用于生产者-消费者帧队列。
 *
 * 特性：
 *   - 固定容量，无动态内存分配
 *   - 线程安全（std::mutex 保护）
 *   - 支持覆盖旧数据（pushOverwrite）
 *   - 支持清空操作
 *
 * @tparam T  元素类型（应为轻量可拷贝类型，如指针或 FrameBuffer）
 */

#include <cstddef>
#include <mutex>
#include <cstring>
#include <cstdint>

template<typename T>
class RingBuffer {
public:
    /**
     * @brief 构造环形缓冲区
     * @param capacity  最大元素数（必须 > 0）
     */
    explicit RingBuffer(int capacity)
        : m_capacity(capacity), m_head(0), m_tail(0), m_size(0)
    {
        m_buffer = new T[static_cast<size_t>(capacity)];
    }

    ~RingBuffer() {
        delete[] m_buffer;
        m_buffer = nullptr;
    }

    // 禁用拷贝
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // 允许移动
    RingBuffer(RingBuffer&& other) noexcept
        : m_buffer(other.m_buffer), m_capacity(other.m_capacity),
          m_head(other.m_head), m_tail(other.m_tail), m_size(other.m_size)
    {
        other.m_buffer = nullptr;
        other.m_capacity = 0;
        other.m_head = other.m_tail = other.m_size = 0;
    }

    // ============================================================
    // 基本操作
    // ============================================================

    /**
     * @brief 入队（队列满时返回 false）
     */
    bool push(const T& item) {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_size >= m_capacity) return false;
        m_buffer[m_tail] = item;
        m_tail = (m_tail + 1) % m_capacity;
        m_size++;
        return true;
    }

    /**
     * @brief 出队（队列空时返回 false）
     */
    bool pop(T& item) {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_size <= 0) return false;
        item = m_buffer[m_head];
        m_head = (m_head + 1) % m_capacity;
        m_size--;
        return true;
    }

    /**
     * @brief 入队，若满则覆盖最旧数据
     * @return 覆盖时返回 true，正常入队返回 false
     */
    bool pushOverwrite(const T& item) {
        bool overwritten = false;
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_size >= m_capacity) {
            // 覆盖最旧的数据
            m_buffer[m_tail] = item;
            m_tail = (m_tail + 1) % m_capacity;
            m_head = m_tail;  // 头追赶尾
            overwritten = true;
        } else {
            m_buffer[m_tail] = item;
            m_tail = (m_tail + 1) % m_capacity;
            m_size++;
        }
        return overwritten;
    }

    /**
     * @brief 查看队头元素（不移除）
     */
    bool peek(T& item) const {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_size <= 0) return false;
        item = m_buffer[m_head];
        return true;
    }

    /**
     * @brief 清空缓冲区
     */
    void clear() {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_head = m_tail = m_size = 0;
    }

    // ============================================================
    // 查询
    // ============================================================

    /** @brief 当前元素数 */
    int size() const {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_size;
    }

    /** @brief 容量 */
    int capacity() const { return m_capacity; }

    /** @brief 是否为空 */
    bool empty() const {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_size <= 0;
    }

    /** @brief 是否已满 */
    bool full() const {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_size >= m_capacity;
    }

private:
    T*           m_buffer;
    int          m_capacity;
    int          m_head;     // 读指针
    int          m_tail;     // 写指针
    int          m_size;     // 当前元素数
    mutable std::mutex m_mtx;
};

#endif // SMART_CAM_COMMON_RINGBUF_H
