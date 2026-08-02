# SmartCam 帧池零拷贝（双缓冲 + 引用计数）— 实现计划

> 版本: v1.0 | 日期: 2026-08-02 | 预计工期: 4~6 天
> 目标: 用"帧池 + 双缓冲 + 引用计数"消除多线程间反复深拷贝，把每帧 memcpy 从 5 次降到 1 次（mmap→槽）+ 1 次上屏必需拷贝，释放 CPU 给编码/推流，并为 720p/60fps 预留带宽。
> 前置: 已完成的"解码移出 GUI 线程"重构（新增 `decodeThread` + `g_display`），本计划在其基础上进一步消除拷贝。

---

## 一、背景与目标

### 1.1 现状：每一帧被拷贝 5~6 次

以当前最新代码（含独立解码线程）为准，一帧从摄像头到屏幕经过的拷贝链：

```
V4L2 mmap
  │
  ├─① 采集线程  memcpy → g_state.frameData          (JPEG ~0.1MB / YUYV 0.61MB)
  │
  ├─② 处理线程  localFrame = g_state.frameData      深拷贝（推流/录像用）
  │
  ├─③ 解码线程  raw = g_state.frameData             深拷贝（解码成 RGB）
  │
  ├─④ displayTimer  rgb = g_display.rgb             深拷贝 (RGB24 0.92MB)
  │
  ├─⑤ setFrame      m_frameBuffer.assign(rgb)       深拷贝 (RGB24 0.92MB)
  │
  └─⑥ frameToQImage QImage(...).copy()              深拷贝 (RGB24 0.92MB)
       └─⑦ QPixmap::fromImage                        上屏必需拷贝 (RGB24 0.92MB)
```

**量化（640x480）**：

| 项 | 每帧大小 | 次数 | 30fps 合计 |
|----|---------|------|-----------|
| ①②③（原始帧 JPEG） | ~0.1MB | 3 | ~9MB/s |
| ④⑤⑥（RGB24） | 0.92MB | 3 | ~83MB/s |
| ⑦ 上屏 | 0.92MB | 1 | ~28MB/s |
| **合计** | | **7 次** | **~120MB/s** |

i.MX6ULL（Cortex-A7 @792MHz）实际内存拷贝带宽约 300~500MB/s（总线受限），**仅 memcpy 就吃掉约 25%~40% 的单核带宽**。这是纯浪费——同一份数据被搬了 3 遍。

### 1.2 目标

| 目标 | 度量 |
|------|------|
| P1: 原始帧（JPEG/YUYV）全链路只拷贝 1 次 | mmap→池槽 1 次，处理/解码线程零拷贝共享 |
| P2: RGB 显示帧 GUI 侧零拷贝 | displayTimer/setFrame/QImage 不再深拷贝 RGB |
| P3: 生命周期安全（无泄漏、无悬垂、无 use-after-free） | ASAN + 长时间运行验证 |
| P4: 池满时丢旧帧而非阻塞（延续"拉模式丢中间帧"设计） | acquire 失败 → 丢帧，采集线程不等待 |
| P5: 对上层（GUI/推流）语义兼容 | setFrame 接口变更但行为不变；推流/录像数据完全一致 |

### 1.3 非本期目标

- ~~所有拷贝归零~~：`QPixmap::fromImage`（⑦）与 mmap→槽（①）是物理必要的（mmap 缓冲必须尽快归还、pixmap 必须持有像素），不在消除范围。
- ~~跨进程零拷贝（dmabuf 直通）~~：本项目为单进程架构，Linux `sendmsg`/`DMABUF` 跨进程共享不适用。
- ~~把 `g_state`/`g_display` 全部替换~~：保留两个共享结构作为"当前发布指针"的角色，池子接管内存所有权。

---

## 二、好处详细分析

### 2.1 CPU 收益（核心动机）

**改造后每帧拷贝**：

```
V4L2 mmap
  ├─① 采集线程  memcpy → raw池槽      (0.1MB，唯一原始帧拷贝)
  │
  ├─处理线程    share() 共享引用，零拷贝 → 推流/录像
  ├─解码线程    share() 共享引用，零拷贝 → 解码出 RGB → 写入 rgb池槽
  │
  └─GUI        share() 共享引用 → QImage 浅引用 → ⑦ 上屏拷贝 (0.92MB)
```

| 项 | 改造前 | 改造后 | 节省 |
|----|--------|--------|------|
| 原始帧拷贝 | 3 次 | 1 次 | 2 次/帧 ≈ 6MB/s |
| RGB 拷贝 | 3 次 | 0 次 | 3 次/帧 ≈ 83MB/s |
| 上屏拷贝 | 1 次 | 1 次 | 0（必需） |
| **每秒 memcpy** | **~120MB/s** | **~34MB/s** | **~86MB/s** |

按 400MB/s 带宽估算，**每秒释放约 215ms 的单核 CPU**（30fps）。这些 CPU 可以还给：
- YUYV 模式的 JPEG 编码（~25ms/帧）；
- 未来 720p/60fps 的编解码；
- 网络推流的封包与发送。

### 2.2 内存收益

| 项 | 改造前 | 改造后 |
|----|--------|--------|
| 原始帧驻留 | g_state + localFrame + raw 各 1 帧 ≈ 3×0.1MB，且每次 `assign` 可能 realloc | raw 池固定 3 槽 ≈ 0.3MB，预分配无 realloc |
| RGB 驻留 | g_display + m_frameBuffer + QImage 临时 ≈ 3×0.92MB 峰值 | rgb 池固定 2 槽 ≈ 1.84MB，无临时峰值 |
| 分配行为 | 每帧多次 vector realloc/释放 | 池预分配，稳态零分配 |

**固定内存** ≈ raw 3 槽 + rgb 2 槽 ≈ **2.1MB**（远小于改造前的峰值 3.5MB+，且无抖动）。

### 2.3 延迟收益

- 每帧少 4~5 次 memcpy，帧从"到达"到"上屏"的链路缩短（每次 memcpy 约 0.5~2ms，RGB 0.92MB 拷贝在 A7 上约 2~3ms）。
- 帧数据"共享"而非"搬移"，意味着**所有消费者看到的都是同一份最新帧**，不存在"解码线程解的是第 N 帧、GUI 显示的是第 N-2 帧"的错位。

### 2.4 可扩展性收益（最重要）

拷贝成本随分辨率线性增长：

| 分辨率 | RGB24/帧 | 30fps 的 RGB 拷贝带宽 |
|--------|---------|---------------------|
| 640x480 | 0.92MB | 83MB/s（改造后 0） |
| 1280x720 | 2.76MB | 249MB/s（改造后 0） |
| 1920x1080 | 6.2MB | 560MB/s（改造后 0） |

**720p 下若保持现架构，RGB 拷贝一项就把 A7 总线吃满**；零拷贝让分辨率升级不再受"搬运"约束，只受编解码能力约束。同理，未来接入 H.264/H.265 解码帧（YUV420 也是大帧）同样受益。

### 2.5 架构收益

- 所有消费者统一走 `share()/release()` 接口，**新增消费者（OSD 叠加、AI 分析、多路推流）无需再各自拷贝一份**；
- "谁最后用完谁释放"的引用计数把生命周期管理集中到一个组件（FramePool），而非散落在各线程手写深拷贝；
- 与现有"拉模式丢中间帧"哲学一致：池满丢帧是显式策略，不是意外行为。

---

## 三、核心设计：帧池 + 双缓冲 + 引用计数

### 3.1 数据结构：`FrameSlot` + `FramePool`

```cpp
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

class FramePool {
public:
    explicit FramePool(int capacity);

    /** @brief 借一个空闲槽（refs 0→1）；无空闲返回 nullptr（调用方丢帧） */
    FrameSlot* acquire();

    /** @brief 取得当前发布槽的共享引用（refs+1）；无发布槽返回 nullptr */
    FrameSlot* share();

    /** @brief 归还引用（refs-1）；归 0 后该槽重新可被 acquire */
    void release(FrameSlot* s);

    /** @brief 发布一个新槽为"当前"（原子替换指针；旧 current 的所有权归消费者） */
    void publish(FrameSlot* s);

private:
    std::vector<std::unique_ptr<FrameSlot>> m_slots;
    std::atomic<FrameSlot*>                 m_current{nullptr};
};
```

**为什么能双缓冲/多缓冲而不加锁**：
- 生产者写槽时，该槽 `refs==1`（只有自己），**不会有消费者读它**；
- 消费者读的永远是 `m_current` 指向的槽（发布过的），**生产者不会再写它**（写的是新 acquire 的槽）；
- 读写分离靠"acquire 只借空闲槽"这一条不变量保证，消费者之间靠原子引用计数共享。

**内存序**：`publish` 用 `release` 语义（写 data 完成后再发布指针），`share` 用 `acquire` 语义（先取指针再读 data），保证"看到指针就一定看到完整数据"。

### 3.2 两个池：原始帧池 + RGB 显示池

| 池 | 容量 | 生产者 | 消费者 | 说明 |
|----|------|--------|--------|------|
| `g_rawPool` | 3（2 消费者 + 1 写槽） | 采集线程 | 处理线程（推流/录像）、解码线程 | 原始帧 JPEG/YUYV |
| `g_rgbPool` | 2（双缓冲） | 解码线程 | GUI（displayTimer） | RGB24 显示帧 |

**为什么 raw 池要 3 槽**：2 个消费者（处理 + 解码）可能同时 share 同一帧（各 +1），加上采集线程需要一个空闲写槽，共需 ≥3。若消费者太慢导致 3 槽都被占，`acquire` 返回 nullptr → 丢帧（符合"拉模式"哲学）。

**为什么 rgb 池 2 槽就够**：GUI 始终持有一帧引用（refs≥1），解码线程需要一个空闲写槽，共 2。GUI 若一直不换帧（页面卡死），解码线程 acquire 不到 → 丢帧，天然反压。

### 3.3 四线程交互流程（伪代码）

**采集线程**（唯一写原始帧的地方）：

```cpp
while (g_state.running) {
    if (capture->getFrame(&fb, 1000) < 0) continue;
    FrameSlot* s = g_rawPool.acquire();          // 借空闲槽
    if (!s) { capture->putFrame(&fb); continue; } // 池满 → 丢帧
    memcpy(s->data.data(), fb.data, fb.length);  // ← 唯一一次原始帧拷贝
    s->width = fb.width; s->height = fb.height;
    s->format = fb.format; s->seq = ++g_frameSeq;
    g_rawPool.publish(s);                        // 原子发布
    capture->putFrame(&fb);                      // 立即归还 mmap
}
```

**处理线程**（推流/录像，零拷贝）：

```cpp
FrameSlot* s = g_rawPool.share();                // current->refs++
if (!s) continue;
// 直接使用 s->data —— 与旧 localFrame 完全等价，但零拷贝
mjpegServer->updateFrame(s->data.data(), s->data.size());
rtspServer->feedFrame(...);
if (recording) storage->writeRecordFrame(...);
g_rawPool.release(s);                            // 用完归还
```

**解码线程**（显示解码，源零拷贝、目标入 rgb 池）：

```cpp
FrameSlot* s = g_rawPool.share();
if (!s) { sleep(5ms); continue; }
FrameSlot* d = g_rgbPool.acquire();              // 借 RGB 写槽
if (!d) { g_rawPool.release(s); continue; }      // RGB 池满 → 丢帧
decode(s->data → d->data);                       // MJPEG/YUYV → RGB24
d->width/height/seq = ...;
g_rgbPool.publish(d);
g_rawPool.release(s);
```

**GUI displayTimer**（零拷贝 + QImage 浅引用）：

```cpp
FrameSlot* s = g_rgbPool.share();                // 当前 RGB 槽引用
if (!s) return;
gui.setFrameShared(s);                           // GUI 持有该槽引用
```

### 3.4 GUI 侧：`setFrame` 接口变更与 QImage 生命周期（最关键的侵入点）

**现状**：`setFrame(const uint8_t* data, int len, int w, int h, PixelFormat fmt)` 内部 `m_frameBuffer.assign()` 深拷贝，`refreshFrame` 里 `frameToQImage` 再 `.copy()` 一次。

**改后**：

```cpp
// gui.h 新增：接收共享槽（GUI 内部负责引用管理）
void setFrameShared(FrameSlot* slot);
// 移除：setFrame(const uint8_t*, ...) 的深拷贝路径（或保留仅供 Mock 用）
```

`setFrameShared` 的实现要点：

```cpp
void CameraGUI::setFrameShared(FrameSlot* slot) {
    // 1. 释放上一帧持有的引用
    if (m_heldSlot) g_rgbPool.release(m_heldSlot);
    // 2. 持有新帧引用（slot 的 refs 已由 share() 加 1）
    m_heldSlot = slot;
    // 3. m_currentFrame 直接指向共享数据（零拷贝）
    m_currentFrame.data   = slot->data.data();
    m_currentFrame.length = (int)slot->data.size();
    m_currentFrame.width  = slot->width;
    m_currentFrame.height = slot->height;
    m_currentFrame.format = PixelFormat::FMT_RGB24;
    m_currentFrame.index++;
}
```

`refreshFrame` 中：

```cpp
// QImage 浅引用共享槽（不拷贝），用完即毁 —— 生命周期由 m_heldSlot 保证
QImage img(m_currentFrame.data, w, h, w*3, QImage::Format_RGB888);  // 不 .copy()！
if (!img.isNull())
    m_videoDisplay->setPixmap(QPixmap::fromImage(img));  // 唯一上屏拷贝
```

**生命周期不变量**：
- GUI 始终持有且仅持有一份 `m_heldSlot` 引用（refs≥1）；
- 解码线程因 `refs≥1` 不会回收该槽 → `m_currentFrame.data` 与 QImage 浅引用始终有效；
- 下一帧 `setFrameShared` 时才 release 旧槽 → 旧槽归零后可被复用；
- `refreshFrame` 内 QImage 是临时对象，作用域结束即毁，不跨帧持有 → 无悬垂。

**注意**：`m_refreshTimer` 仍每 33ms 调用 `refreshFrame`；若 `displayTimer` 未拉到新帧（`setFrameShared` 未调用），`m_heldSlot` 保持旧帧 → 重复绘制同一帧，行为与现状一致（可加 `seq` 去重跳过绘制，见 §5 风险）。

### 3.5 引用计数实现选型

| 方案 | 优点 | 缺点 | 选型 |
|------|------|------|------|
| 手写 `std::atomic<int>` + 显式 acquire/share/release | 零依赖、语义清晰、可控 | 漏 release 难排查；必须 RAII 包裹 | ✅ 首选，配合 `ScopeGuard` |
| `std::shared_ptr<FrameSlot>` | RAII 自动管理 | `publish` 需要 `std::atomic<std::shared_ptr>`（C++20）或加锁；引用计数随指针拷贝而变，语义不如显式清晰 | 备选（若升级 C++20） |

**必须配套 RAII 句柄**（防漏 release）：

```cpp
struct SlotGuard {           // RAII：离开作用域自动 release
    FramePool* pool;
    FrameSlot* slot;
    ~SlotGuard() { if (slot) pool->release(slot); }
    FrameSlot* get() const { return slot; }
};
```

所有 `share()` 返回值立即包进 `SlotGuard`，编译器保证任何返回路径都归还。这是本计划**安全性的第一道防线**。

### 3.6 与现有 `g_state` / `g_display` 的演进关系

- **不删除** `g_state` / `g_display`：它们退化为"池的当前发布指针 + 状态元数据"（fps、running、paused 等仍用 `g_state`）；
- `g_state.frameData` / `g_display.rgb` 这两个 `std::vector` 成员**移除**（数据所有权移交池）；
- 其他模块（拍照回调、status provider、录像回调）凡是直接读 `g_state.frameData` 的，统一改为 `g_rawPool.share()` 拿共享槽——**注意这些回调原本在锁内访问，改成池后要防跨锁嵌套**（见 §5）。

---

## 四、实现步骤（分阶段）

### Phase 1：`FramePool` 实现 + 单元测试（1.5 天）

- [ ] 新建 `include/common/frame_pool.h` + `src/common/frame_pool.cpp`（FrameSlot + FramePool + SlotGuard）；
- [ ] 实现 acquire/share/release/publish，正确使用原子与内存序（release/acquire）；
- [ ] 单元测试 `tests/test_frame_pool.cpp`：
  - 借还循环：capacity 个槽反复 acquire/release 不泄漏；
  - 并发：4 线程混合 share/release 下 refs 始终守恒（ASAN 无 data race）；
  - 池满：acquire 返回 nullptr；
  - publish/share：share 到的一定是已发布且数据完整（用 seq 校验）。
- [ ] CMakeLists 注册测试。

### Phase 2：原始帧池接入（采集/处理/解码）（1.5 天）

- [ ] `main.cpp` 创建 `g_rawPool`（capacity=3）；
- [ ] 采集线程：`acquire` + `memcpy` + `publish`，替换 `g_state.frameData.assign`；
- [ ] 处理线程：`share/release`（SlotGuard）替换 `localFrame` 深拷贝；
- [ ] 解码线程：`share/release` 替换 `raw` 深拷贝；
- [ ] 所有读 `g_state.frameData` 的回调（拍照/录像/status）改为 `share/release`；
- [ ] 保留 `g_state.width/height/format/fps/frameSeq`（元数据仍放 g_state，但 frameData 移除）；
- [ ] 编译 + Mock 冒烟 + 录像/推流功能回归。

### Phase 3：RGB 显示池 + GUI 集成（1.5 天）

- [ ] `main.cpp` 创建 `g_rgbPool`（capacity=2）；
- [ ] 解码线程输出改为 `acquire` rgb 槽 → `publish`；
- [ ] `gui.h/cpp`：新增 `setFrameShared(FrameSlot*)`，`m_heldSlot` 引用管理；`refreshFrame` 改用浅引用 QImage（去掉 `.copy()`）；
- [ ] `displayTimer` 改为 `share` rgb 槽 → `setFrameShared`；
- [ ] Mock 模式：不建池，`setFrameShared` 走纯 RGB24 直通分支（或保留旧 `setFrame` 仅供 Mock）；
- [ ] 编译 + 冒烟（Mock + 真实无头验证）。

### Phase 4：验证、调优与文档（1 天）

- [ ] ASAN + ThreadSanitizer 全套跑通（重点：refs 守恒、无 data race、无 use-after-free）；
- [ ] 性能对比：`perf stat`/`mpstat` 测量改造前后 memcpy 带宽与 CPU 占用；
- [ ] 长时间运行 1 小时（记录内存曲线，验证无泄漏、无槽永久占用）；
- [ ] 720p 模拟压测（如摄像头支持）验证丢帧策略与反压；
- [ ] 更新文档（本计划 + 面试复习文档同步）。

---

## 五、风险与对策

| 风险 | 影响 | 对策 |
|------|------|------|
| **漏 release → 槽永不回收** | 池被占满 → 持续丢帧/画面停更 | SlotGuard RAII 全覆盖；Phase 4 加"槽泄漏探针"（空闲槽数 < 预期则告警） |
| **多 release / 悬垂** | use-after-free 崩溃 | 只允许 SlotGuard 管理；单元测试用 ASAN |
| **QImage 浅引用悬垂** | 花屏/崩溃 | 生命周期不变量（GUI 始终持有 m_heldSlot）；refreshFrame 内 QImage 为临时对象 |
| **回调锁嵌套**（现回调在 g_state.mtx 内读 frameData） | 死锁 | 回调先 `share`（拿引用）再释放锁，锁外使用数据；明确"池操作与 g_state.mtx 不嵌套"纪律 |
| 原子内存序用错 | 读到半写帧 | publish=release / share=acquire；TSan 验证 |
| GUI 重复绘制同一帧（无新帧时） | 浪费 setPixmap | `seq` 去重：`setFrameShared` 记录 seq，`refreshFrame` 比对后跳过 |
| 现有 `g_state.frameData` 被其他模块引用 | 编译错误/遗漏 | Phase 2 全局 grep `frameData` 逐一改；保留元数据字段 |
| 内存增加（池预分配） | 常驻 +2.1MB | 已计入预算（512MB 中占比极小）；720p 时 RGB 槽 2×2.76MB |

---

## 六、测试方案

| 测试 | 方法 | 通过标准 |
|------|------|----------|
| 单元（FramePool） | 借还/并发/池满/publish-share | refs 守恒；ASAN/TSan 无告警 |
| 集成（推流） | 改造后浏览器/VLC 观看 | 画面与改造前一致（逐帧比对前几秒） |
| 集成（录像） | 录 30s AVI 播放 | 帧数、尺寸、内容正确 |
| 集成（显示） | 真实/Mock 运行 | 无花屏、无撕裂、帧序号单调 |
| 性能 | `perf stat -e cycles,instructions` 对比 | 每帧 memcpy 从 ~3MB 降到 ~1MB；CPU 占用下降 |
| 稳定性 | 1 小时连续运行 + 内存采样 | 无内存增长、无槽泄漏 |
| 回归 | ctest 全部 | 全绿 |

---

## 七、代码变更清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/common/frame_pool.h` | **新增** | FrameSlot / FramePool / SlotGuard 声明（~120 行） |
| `src/common/frame_pool.cpp` | **新增** | acquire/share/release/publish 实现（~120 行） |
| `tests/test_frame_pool.cpp` | **新增** | 池单元测试（~150 行） |
| `tests/CMakeLists.txt` | 修改 | 注册新测试 |
| `src/main.cpp` | 修改 | 创建两个池；采集/处理/解码线程改共享；displayTimer 改 share；拍照/录像/status 回调改 share（~120 行） |
| `include/display/gui.h` | 修改 | 新增 `setFrameShared`；`m_heldSlot` 成员（~15 行） |
| `src/display/gui.cpp` | 修改 | setFrameShared 实现；refreshFrame 浅引用（~40 行） |
| `CMakeLists.txt` | 修改 | 新增 frame_pool.cpp（~3 行） |
| `docs/` | 新增 | `plan-frame-pool-zero-copy.md`（本文档）+ 实现记录 |

**预计新增 ~390 行，修改 ~180 行**。

---

## 八、工期估算

| 阶段 | 内容 | 工期 |
|------|------|------|
| Phase 1 | FramePool + 单测 | 1.5 天 |
| Phase 2 | 原始帧池接入（采集/处理/解码/回调） | 1.5 天 |
| Phase 3 | RGB 池 + GUI 集成 | 1.5 天 |
| Phase 4 | 验证（ASAN/TSan/性能/稳定性）+ 文档 | 1 天 |
| **合计** | | **4~6 天** |

---

## 九、结论

> 帧池零拷贝（双缓冲 + 引用计数）是当前显示/推流链路最大的单点优化：**每帧 memcpy 从 ~3MB 降到 ~1MB（-70%），释放约每秒 215ms 的单核 CPU**，并让分辨率升级（720p/1080p）不再受搬运带宽约束。核心不变量只有一条——"acquire 只借空闲槽"，由此天然实现读写分离（双缓冲）与多消费者共享（引用计数），与项目既有的"拉模式丢最新帧"哲学完全一致。主要风险在**生命周期管理**（漏 release / QImage 悬垂），对策是 SlotGuard RAII 全覆盖 + ASAN/TSan 强制验证 + 槽泄漏探针。若追求最小风险，可先只做 RGB 池（Phase 3），跳过 raw 池——已单独标注收益差异（raw 池省 ~6MB/s，RGB 池省 ~83MB/s）。
