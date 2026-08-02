/**
 * @file    test_frame_pool.cpp
 * @brief   FramePool 帧池单元测试
 *
 * 测试内容:
 *   1. 借还循环：capacity 个槽反复 acquire/release 不泄漏、refs 守恒
 *   2. 池满：acquire 返回 nullptr
 *   3. publish/share：share 到的一定是已发布且数据完整（用 seq 校验）
 *   4. 并发：多线程混合 share/release 下 refs 守恒（无 data race）
 *   5. SlotGuard RAII：作用域结束自动 release，不泄漏
 *
 * 编译（PC）:
 *   cd build/pc && cmake .. && make test_frame_pool && ./test_frame_pool
 * 或直接:
 *   g++ -std=c++17 -O2 -pthread -I.. -o /tmp/test_frame_pool \
 *       tests/test_frame_pool.cpp && /tmp/test_frame_pool
 */

#include <cstdio>
#include <cstring>
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>

#include "include/common/frame_pool.h"

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name) \
    printf("  [TEST] %s ... ", name)

#define PASS() \
    do { printf("PASS\n"); testsPassed++; } while(0)

#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); testsFailed++; } while(0)

// ============================================================
// 1. 借还循环：槽反复 acquire/release，refs 守恒
// ============================================================
static void test_acquire_release_loop() {
    TEST("借还循环 refs 守恒");
    FramePool pool(3);
    const int N = 1000;
    for (int i = 0; i < N; ++i) {
        FrameSlot* s = pool.acquire();
        if (!s) { FAIL("循环中 acquire 意外失败"); return; }
        s->data.assign(100, (uint8_t)i);   // 写入数据
        pool.release(s);                   // 归还 → refs 归 0
    }
    // 全部归还后，3 槽都应再次可借出
    int borrowCount = 0;
    while (pool.acquire()) borrowCount++;
    if (borrowCount != 3) { FAIL("归还后应能借出全部 3 槽"); return; }
    PASS();
}

// ============================================================
// 2. 池满：acquire 返回 nullptr
// ============================================================
static void test_pool_full() {
    TEST("池满 acquire 返回 nullptr");
    FramePool pool(2);
    FrameSlot* a = pool.acquire();
    FrameSlot* b = pool.acquire();
    if (!a || !b) { FAIL("借前两槽失败"); return; }
    FrameSlot* c = pool.acquire();
    if (c != nullptr) { FAIL("池满应返回 nullptr"); return; }
    pool.release(a);
    pool.release(b);
    PASS();
}

// ============================================================
// 3. publish/share：share 到已发布且数据完整
// ============================================================
static void test_publish_share() {
    TEST("publish/share 数据完整性");
    FramePool pool(3);
    FrameSlot* s = pool.acquire();
    if (!s) { FAIL("acquire 失败"); return; }
    // 写数据
    s->data.assign({1, 2, 3, 4, 5, 6, 7, 8});
    s->seq    = 42;
    s->width  = 8;
    s->height = 1;
    s->format = PixelFormat::FMT_MJPEG;
    pool.publish(s);   // publish 自动释放生产者写引用（refs 1→0），槽归消费者所有

    // share 应拿到发布槽且数据完整
    FrameSlot* cur = pool.share();
    if (!cur) { FAIL("share 返回 nullptr"); return; }
    if (cur->seq != 42) { FAIL("seq 不一致"); return; }
    if (cur->data.size() != 8 || cur->data[7] != 8) { FAIL("数据不一致"); return; }
    if (cur->width != 8 || cur->height != 1) { FAIL("宽高不一致"); return; }
    if (cur->format != PixelFormat::FMT_MJPEG) { FAIL("格式不一致"); return; }

    // share 期间该槽 refs==1（仅消费者），再次 acquire 不应借出它
    FrameSlot* a = pool.acquire();
    FrameSlot* b = pool.acquire();
    if (!a || !b) { FAIL("剩余 2 槽应可借出"); pool.release(cur); return; }
    FrameSlot* c = pool.acquire();
    if (c != nullptr) { FAIL("share 持有时该槽不应被借出"); pool.release(a); pool.release(b); pool.release(cur); return; }
    pool.release(a);
    pool.release(b);
    pool.release(cur);
    PASS();
}

// ============================================================
// 4. 并发：多线程混合 share/release，refs 守恒
// ============================================================
static void test_concurrent() {
    TEST("并发 share/release refs 守恒");
    FramePool pool(4);
    // 先发布一帧
    FrameSlot* prod = pool.acquire();
    if (!prod) { FAIL("acquire 失败"); return; }
    prod->data.assign(64, 0xAB);
    prod->seq = 7;
    pool.publish(prod);   // publish 自动释放生产者引用

    // 生产者继续循环发布新帧
    std::atomic<bool> stop{false};
    std::atomic<int>  errors{0};

    std::thread producer([&]() {
        uint64_t seq = 8;
        while (!stop.load()) {
            FrameSlot* s = pool.acquire();
            if (!s) { std::this_thread::yield(); continue; }  // 池满丢帧
            s->data.assign(64, (uint8_t)(seq & 0xFF));
            s->seq = seq++;
            pool.publish(s);   // publish 自动释放生产者引用
            std::this_thread::yield();
        }
    });

    // 4 个消费者 share → 读数据 → release
    std::vector<std::thread> consumers;
    for (int t = 0; t < 4; ++t) {
        consumers.emplace_back([&]() {
            int iterations = 0;
            while (!stop.load()) {
                FrameSlot* s = pool.share();
                if (!s) { std::this_thread::yield(); continue; }
                // 校验读取的帧数据与 seq 低字节一致（数据完整性抽查）
                if (!s->data.empty() &&
                    s->data[0] != (uint8_t)(s->seq & 0xFF) &&
                    s->seq > 8) {
                    errors.fetch_add(1);
                }
                pool.release(s);
                iterations++;
                if (iterations >= 2000) break;
            }
        });
    }

    // 运行一段时间后停止
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true);
    producer.join();
    for (auto& c : consumers) c.join();

    if (errors.load() != 0) { FAIL("并发数据校验出错"); return; }
    // 结束后：消费者的 share 引用应已全部归还。
    // 池中除 current 槽（refs=1 被池持有）外，其余 3 槽应可全部借出。
    int borrow = 0;
    while (pool.acquire()) borrow++;
    if (borrow != 3) { FAIL("并发结束后 refs 未守恒（应剩 current 槽被持有）"); return; }
    PASS();
}

// ============================================================
// 5. SlotGuard RAII：作用域结束自动 release
// ============================================================
static void test_slot_guard() {
    TEST("SlotGuard RAII 自动归还");
    FramePool pool(1);
    {
        FrameSlot* s = pool.share();   // 无发布槽 → nullptr
        SlotGuard g(&pool, s);
        if (g.get() != nullptr) { FAIL("未发布时 share 应返回 nullptr"); return; }
    }
    // 发布一帧
    FrameSlot* prod = pool.acquire();
    prod->seq = 1;
    pool.publish(prod);   // current 槽 refs 保持 1（池持有）
    {
        SlotGuard g(&pool, pool.share());   // share → refs 2
        if (!g) { FAIL("share 应返回槽"); return; }
        (void)g.get()->seq;
        // 离开作用域自动 release → refs 回到 1
    }
    // SlotGuard 已归还消费者的引用；但 current 槽仍被池持有（refs=1），
    // 单槽池中无其他空闲槽可借 → acquire 应返回 nullptr（这是正确的双缓冲行为）
    FrameSlot* s = pool.acquire();
    if (s != nullptr) { FAIL("单槽池 current 槽应被池持有，不可再借"); pool.release(s); return; }
    PASS();
}

int main() {
    printf("=== FramePool 单元测试 ===\n");
    test_acquire_release_loop();
    test_pool_full();
    test_publish_share();
    test_concurrent();
    test_slot_guard();
    printf("=========================\n");
    printf("通过: %d, 失败: %d\n", testsPassed, testsFailed);
    return testsFailed == 0 ? 0 : 1;
}
