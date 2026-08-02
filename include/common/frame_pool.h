#ifndef SMART_CAM_COMMON_FRAME_POOL_H
#define SMART_CAM_COMMON_FRAME_POOL_H

/**
 * @file    frame_pool.h
 * @brief   帧池零拷贝（双缓冲 + 引用计数）
 *
 * 核心思想："数据共享，而非数据搬移"。
 * 生产者在池槽中写数据并发布，消费者通过共享引用读取同一份数据，
 * 谁最后用完谁释放。核心不变量只有一条：
 *
 *   **acquire 只借空闲槽（refs==0）**
 *
 * 由此天然实现：
 *   - 读写分离（双缓冲）：生产者写 refs==1 的槽，消费者读已发布槽，永不冲突；
 *   - 多消费者共享：引用计数让多个线程同时持有同一帧；
 *   - 池满丢帧（反压）：acquire 失败返回 nullptr → 调用方丢帧，不阻塞。
 *
 * 内存序：
 *   - publish  用 release 语义（写完 data 再发布指针）
 *   - share    用 acquire 语义（先取指针再读 data）
 *   保证"看到指针就一定看到完整数据"。
 *
 * 配套 RAII 句柄 SlotGuard：所有 share() 结果必须包进句柄，
 * 编译器保证任何返回路径都归还引用（防漏 release）。
 */

#include <cstdint>
#include <vector>
#include <memory>
#include <atomic>

#include "include/common/types.h"

/**
 * @brief 帧槽：一块可被多线程共享的帧缓冲
 *
 * refs 引用计数语义：
 *   - 0 = 空闲，可被 acquire() 借出
 *   - >0 = 正在被生产者写入或被消费者读取
 * 生产者写完发布；消费者用完 release；最后一个 release 使槽回到空闲。
 */
struct FrameSlot {
    std::vector<uint8_t> data;          // 帧数据（预分配，稳态零 realloc）
    std::atomic<int>     refs{0};       // 引用计数
    uint64_t             seq{0};        // 帧序号（消费者去重）
    int                  width{0};
    int                  height{0};
    PixelFormat          format{PixelFormat::FMT_RGB24};
};

/**
 * @brief 帧池：管理固定数量帧槽的多线程共享池
 *
 * 线程安全：acquire/share/release/publish 均为原子操作，无外部锁。
 */
class FramePool {
public:
    /**
     * @brief 构造帧池，预分配 capacity 个槽
     * @param capacity 槽数量（原始帧池建议 3，RGB 显示池建议 2）
     */
    explicit FramePool(int capacity) {
        m_slots.reserve(capacity);
        for (int i = 0; i < capacity; ++i)
            m_slots.push_back(std::make_unique<FrameSlot>());
    }

    FramePool(const FramePool&) = delete;
    FramePool& operator=(const FramePool&) = delete;

    /**
     * @brief 借一个空闲槽（refs 0→1）
     * @return 空闲槽指针；无空闲返回 nullptr（调用方应丢帧，勿等待）
     */
    FrameSlot* acquire() {
        for (auto& s : m_slots) {
            int expected = 0;
            if (s->refs.compare_exchange_strong(expected, 1))
                return s.get();
        }
        return nullptr;
    }

    /**
     * @brief 取得当前发布槽的共享引用（refs+1）
     * @return 当前槽指针；无发布槽返回 nullptr
     *
     * 注意：返回的指针必须由调用方通过 release() 归还
     * （推荐包进 SlotGuard）。
     */
    FrameSlot* share() {
        FrameSlot* cur = m_current.load(std::memory_order_acquire);
        if (cur)
            cur->refs.fetch_add(1, std::memory_order_relaxed);
        return cur;
    }

    /**
     * @brief 归还引用（refs-1）；归 0 后该槽重新可被 acquire
     */
    void release(FrameSlot* s) {
        if (!s) return;
        s->refs.fetch_sub(1, std::memory_order_release);
    }

    /**
     * @brief 发布一个新槽为"当前"（原子替换指针）
     *
     * 调用前必须已完成对 s->data 的写入；s 必须是刚 acquire 的槽（refs==1）。
     *
     * ⚠️ 所有权语义：
     *   - publish **不释放** s 的引用——s 以 refs==1 持续被"池持有"（current 槽），
     *     保证发布期间不会被生产者重写；
     *   - 同时**释放旧 current 槽的池持有引用**（refs 1→0），旧槽归零后可被复用；
     *   - 消费者 share 使 current 槽 refs 1→2，release 后回到 1；
     *     生产者 acquire 借不到 refs≠0 的槽 → current 槽不会被写。
     *
     * publish 用 release 语义保证数据可见性先于指针可见性。
     */
    void publish(FrameSlot* s) {
        // 先确保 data 写完整，再发布指针（release fence 与消费者 acquire 配对）
        std::atomic_thread_fence(std::memory_order_release);
        FrameSlot* old = m_current.exchange(s, std::memory_order_acq_rel);
        if (old)
            release(old);   // 释放旧 current 的池持有引用（可复用）
    }

    /** @brief 当前槽的帧序号（无发布槽返回 0） */
    uint64_t currentSeq() const {
        FrameSlot* cur = m_current.load(std::memory_order_acquire);
        return cur ? cur->seq : 0;
    }

private:
    std::vector<std::unique_ptr<FrameSlot>> m_slots;
    std::atomic<FrameSlot*>                 m_current{nullptr};
};

/**
 * @brief RAII 句柄：离开作用域自动 release
 *
 * 用法：
 *   SlotGuard g(pool, pool->share());
 *   if (!g.get()) { ... 丢帧 ... }
 *   // 使用 g.get()->data ...
 *   // 离开作用域自动 release，任何返回路径都不会漏
 */
class SlotGuard {
public:
    SlotGuard(FramePool* pool, FrameSlot* slot) : m_pool(pool), m_slot(slot) {}
    ~SlotGuard() {
        if (m_slot && m_pool) m_pool->release(m_slot);
    }

    SlotGuard(const SlotGuard&) = delete;
    SlotGuard& operator=(const SlotGuard&) = delete;

    FrameSlot* get() const { return m_slot; }
    explicit operator bool() const { return m_slot != nullptr; }

    /** @brief 释放所有权（主动归还并置空，防止析构二次 release） */
    FrameSlot* releaseOwnership() {
        FrameSlot* s = m_slot;
        m_slot = nullptr;
        return s;
    }

private:
    FramePool* m_pool;
    FrameSlot* m_slot;
};

#endif // SMART_CAM_COMMON_FRAME_POOL_H
