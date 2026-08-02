# SmartCam 帧池零拷贝优化 — 实施指南与性能验证文档

> 版本: v1.0 | 日期: 2026-08-03 | 状态: 基线已采集，待实现
> 对应计划: `docs/plan-frame-pool-zero-copy.md`（设计稿）
> 本文档定位: **实施 + 验证的完整操作手册**，包含基线数据、对比方案、实现步骤、风险对策，供后续维护与复现。

---

## 目录

1. [改进动机与目标](#一改进动机与目标)
2. [现有架构的局限性分析](#二现有架构的局限性分析)
3. [新方案的设计理念与核心改进点](#三新方案的设计理念与核心改进点)
4. [性能对比测试方案（详细）](#四性能对比测试方案详细)
5. [分步骤实现流程](#五分步骤实现流程)
6. [关键代码与配置变更说明](#六关键代码与配置变更说明)
7. [可能遇到的问题及解决方案](#七可能遇到的问题及解决方案)
8. [性能评估与结论](#八性能评估与结论)
9. [附：实测基线数据（改进前）](#九附实测基线数据改进前)

---

## 一、改进动机与目标

### 1.1 动机

i.MX6ULL（Cortex-A7 @ 792MHz，**单核**，512MB DDR3）上，SmartCam 同时承担采集、解码、推流、录像、GUI 显示五类工作。当前架构对每一帧数据进行**多次深拷贝**（详见第二章量化），在单核上：
- 纯 memcpy 就吃掉大量 CPU 带宽；
- 采集线程、处理线程、GUI 线程在单核上互相竞争；
- 实测（见第九章基线数据）**CPU 已 99% 打满，帧率仅 ~10fps**，远低于设计目标 30fps；
- 分辨率升级（720p/1080p）后拷贝成本线性翻倍，当前架构无法支撑。

帧池零拷贝（双缓冲 + 引用计数）的目标是**消除无意义的重复搬运**，把 CPU 还给编解码与推流，并为主要带宽升级预留空间。

### 1.2 目标（可度量）

| 目标编号 | 目标 | 度量方式 | 验收线 |
|----------|------|----------|--------|
| P1 | 原始帧（JPEG/YUYV）全链路只拷贝 1 次 | `[PERF] copy` 中原始帧部分 | 从 ~3 次/帧 → 1 次/帧 |
| P2 | RGB 显示帧 GUI 侧零拷贝 | `[PERF] copy` 中 RGB 部分 | 从 3 次/帧 → 0 次/帧 |
| P3 | 生命周期安全（无泄漏/悬垂/use-after-free） | ASAN + 长时间运行 | 1h 运行内存曲线平稳 |
| P4 | 池满丢旧帧而非阻塞 | acquire 失败即丢帧 | 采集线程永不等待 |
| P5 | 对上层语义兼容 | 推流/录像内容一致 | 画面与改造前一致 |

**核心收益量化（理论）**：每帧 memcpy 从 ~3MB 降到 ~1MB（**-70%**），按 400MB/s 带宽估算**每秒释放约 215ms 单核 CPU**。

---

## 二、现有架构的局限性分析

### 2.1 当前拷贝链（逐帧 5~6 次 memcpy）

以当前代码（GUI 线程直接解码）为准，一帧从摄像头到屏幕：

```
V4L2 mmap
  │
  ├─① 采集线程  memcpy → g_state.frameData       (JPEG ~0.1MB / YUYV 0.61MB)
  │              main.cpp: g_state.frameData.assign(fb.data, ...)
  │
  ├─② 处理线程  localFrame = g_state.frameData    深拷贝（推流/录像用）
  │              main.cpp: localFrame = g_state.frameData
  │
  ├─③ GUI      setFrame → m_frameBuffer.assign    深拷贝（原始帧，GUI 内部）
  │              gui.cpp: m_frameBuffer.assign(data, data+len)
  │
  ├─④ GUI      解码后 QImage(...).copy()          深拷贝 (RGB24 0.92MB)
  │              gui.cpp: frameToQImage 内 .copy()
  │
  └─⑤ GUI      QPixmap::fromImage                 上屏必需拷贝 (RGB24 0.92MB)
                   gui.cpp: refreshFrame 内 setPixmap(QPixmap::fromImage(img))
```

**量化（640x480，按实测 10fps 计算）**：

| 项 | 每帧大小 | 次数/帧 | 10fps 合计 | 说明 |
|----|---------|---------|-----------|------|
| ①②③（原始帧 JPEG） | ~0.1MB | 3 | ~3MB/s | 同一 JPEG 被搬 3 遍 |
| ④（解码后 RGB24） | 0.92MB | 1 | ~9.2MB/s | 解码产物上屏前再拷一次 |
| ⑤（上屏 RGB24） | 0.92MB | 1 | ~9.2MB/s | **物理必需**，不消除 |
| **合计** | | **5 次** | **~21MB/s** | |

> 注：文档 `plan-frame-pool-zero-copy.md` 原假设拷贝链含独立解码线程（~120MB/s），但解码线程已回退（单核实测更卡，见 2.3），当前基线的拷贝链为上述 5 次版本。

### 2.2 三个具体问题

1. **无效重复搬运**：同一份 JPEG 被拷贝 3 次（①②③），解码后的 RGB24 又被拷贝 2 次（④⑤）。其中 ②③④ 都是纯浪费——消费者只是"读"数据，不需要"拥有"独立副本。
2. **动态分配抖动**：`std::vector::assign` 每次可能 realloc，高频分配/释放造成内存碎片与耗时。
3. **锁粒度粗**：`g_state.mtx` 保护整个 `frameData` 拷贝，多消费者串行化。

### 2.3 为什么"独立解码线程"方案在单核上失败（重要经验）

历史回退记录：曾实现独立 `decodeThread`（MJPEG→RGB24 移出 GUI 线程），实测**更卡**。原因：
- i.MX6ULL 是**单核**，线程不能并行，解码线程仍占用同一个核；
- 多线程引入额外 2 次 0.92MB RGB24 深拷贝（`rgb = g_display.rgb` + `m_frameBuffer.assign`）+ 线程竞争/切换开销；
- 收益（不阻塞 GUI 事件循环）被拷贝开销反超。

**结论**：本项目的优化方向必须是"**少拷贝**"，而不是"多线程并行"。帧池零拷贝正是这一方向的正确实现——它**不增加线程**，只消除重复 memcpy。

---

## 三、新方案的设计理念与核心改进点

### 3.1 设计理念

> **"数据共享，而非数据搬移"**——所有消费者通过共享引用读同一份数据，谁最后用完谁释放。

核心不变量只有一条：**`acquire` 只借空闲槽**。由此天然实现：
- **读写分离（双缓冲）**：生产者写 `refs==1` 的槽，消费者读 `m_current` 指向的已发布槽，二者永不冲突；
- **多消费者共享**：引用计数让多个线程同时持有同一帧的引用；
- **池满丢帧（反压）**：`acquire` 失败返回 nullptr → 丢帧，符合项目既有的"拉模式丢最新帧"哲学，采集线程永不阻塞。

### 3.2 核心数据结构

```cpp
struct FrameSlot {                       // 帧槽：一块可共享的帧缓冲
    std::vector<uint8_t> data;           // 帧数据（预分配，稳态零 realloc）
    std::atomic<int>     refs{0};        // 引用计数：0=空闲，>0=被持有
    uint64_t             seq{0};         // 帧序号（消费者去重）
    int                  width{0}, height{0};
    PixelFormat          format{PixelFormat::FMT_RGB24};
};

class FramePool {
    FrameSlot* acquire();                // 借空闲槽（refs 0→1），无空闲返回 nullptr
    FrameSlot* share();                  // 取得当前槽共享引用（refs+1）
    void       release(FrameSlot* s);    // 归还引用（refs-1，归 0 后槽可复用）
    void       publish(FrameSlot* s);    // 原子发布为"当前"（release 语义）
private:
    std::vector<std::unique_ptr<FrameSlot>> m_slots;
    std::atomic<FrameSlot*>                 m_current{nullptr};
};
```

**内存序**：`publish` 用 release 语义（写完 data 再发布指针），`share` 用 acquire 语义（先取指针再读 data），保证"看到指针就一定看到完整数据"。

### 3.3 两个池

| 池 | 容量 | 生产者 | 消费者 | 说明 |
|----|------|--------|--------|------|
| `g_rawPool` | 3 | 采集线程 | 处理线程（推流/录像）、GUI（显示解码） | 原始帧 JPEG/YUYV；2 消费者 + 1 写槽 |
| `g_rgbPool` | 2 | 解码方（GUI 线程内解码） | GUI displayTimer | RGB24 显示帧；GUI 持 1 + 写槽 1 |

> **本实施选择**：为控制风险，**先只做 `g_rgbPool`（RGB 显示池）**。收益占 90%+（消掉 ④ 的解码拷贝 + ③ 的原始帧拷贝中属于显示的部分），且不碰采集/推流链路（① ② 保留），风险最低。raw 池留作二期（见第八章）。

### 3.4 改造前后拷贝链对比

**改造前（现状，5 次）**：
```
①采集→g_state(0.1MB)  ②处理localFrame(0.1MB)  ③setFrame(0.1MB)  ④解码copy(0.92MB)  ⑤上屏(0.92MB)
```

**改造后（RGB 池，显示链路零拷贝）**：
```
①采集→raw槽(0.1MB)  ②处理localFrame(0.1MB)  ──RGB池──  GUI浅引用 → ⑤上屏(0.92MB)
                                             ③④被消除：解码直接写池槽，GUI 用 QImage 浅引用
```

| 项 | 改造前 | 改造后 | 节省 |
|----|--------|--------|------|
| 原始帧拷贝（显示侧） | ③ 1 次/帧 | 0 次/帧 | ~1MB/s@10fps |
| RGB 拷贝 | ④ 1 次/帧 | 0 次/帧 | ~9.2MB/s@10fps |
| 上屏拷贝 | 1 次/帧 | 1 次/帧 | 0（必需） |
| **合计** | **~21MB/s** | **~12MB/s** | **~9MB/s（-43%）** |

### 3.5 收益全景

- **CPU**：实测基线 CPU 已 99%（10fps）。消除 RGB 大拷贝后，预计 CPU 下降或同 CPU 下帧率提升；
- **内存**：池预分配固定 2 槽 ≈ 1.84MB，消除每帧 realloc 抖动；RSS 曲线更平稳；
- **延迟**：显示链路少 1~2 次 memcpy，帧到达→上屏链路缩短；
- **扩展性**：720p 时 RGB24 2.76MB/帧，零拷贝使分辨率升级不再受搬运带宽约束（640x480 的 0.92MB 拷贝已吃掉 CPU，720p 更无法承受）。

---

## 四、性能对比测试方案（详细）

### 4.1 测试环境

| 项 | 值 |
|----|-----|
| 开发板 | 野火 i.MX6ULL Pro（Cortex-A7 @ 792MHz，单核，512MB DDR3） |
| 屏幕 | 7 寸 800x480 电容触摸屏（framebuffer /dev/fb0） |
| 摄像头 | USB UVC，MJPEG 640x480@30fps 硬件直出 |
| 系统 | Debian Buster（glibc 2.28） |
| Qt | **板厂手动 Qt 套**（`/usr/lib`，linuxfb 平台，含 tslib） |
| 运行环境 | `LD_LIBRARY_PATH=/usr/lib` + `QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/plugins/platforms` + `QT_QPA_PLATFORM=linuxfb`（见 README，避免 Debian Qt 套的光标崩溃） |
| 采集代码 | commit `416bf2d`（含 `[PERF]` 插桩） |

### 4.2 测试指标

| 指标 | 来源 | 说明 |
|------|------|------|
| `copy` (MB/s) | `[PERF]` 行 | ①②③④ 拷贝合计，**帧池直接收益** |
| `+pix` (MB/s) | `[PERF]` 行 | ⑤ 上屏拷贝，必需，改造后不变 |
| `frames` (fps) | `[PERF]` 行 | 处理帧率，确认不掉帧 |
| `cpu` (%) | `[PERF]` 行 | 全进程 CPU%（/proc/self jiffies），单核 0~100% |
| `rss` (KB) | `[PERF]` 行 | 物理内存占用（VmRSS） |
| 稳定运行 | 观察 | 1h 无崩溃、无内存增长 |

### 4.3 测试用例（控制变量）

| 用例 | 描述 | 关注点 |
|------|------|--------|
| A. 纯显示 | 无人访问 HTTP/RTSP | GUI 显示链路本身的拷贝/CPU |
| B. 显示+1浏览器 | 浏览器开 `http://<IP>:8080/` 观看 | 显示+推流叠加的真实负载 |
| C. 显示+RTSP | VLC/ffplay 播放 rtsp:// | 多消费者共享场景 |
| D. 1h 稳定性 | 用例 A 持续 1h | 内存曲线、泄漏 |

**同条件保证**：同一摄像头、同一光照、同一分辨率、同一参数 `--fmt mjpeg --http-port 8080`。

### 4.4 数据采集方法

**改造前与改造后必须用同一套命令、同一采集方法**，保证可比：

```bash
cd ~/smartcam/SmartCam-Linux-on-imx6ull
unset LD_LIBRARY_PATH QT_QPA_PLATFORM_PLUGIN_PATH
export LD_LIBRARY_PATH=/usr/lib
export QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/plugins/platforms
export QT_QPA_PLATFORM=linuxfb

# 用例 A：纯显示，60s，日志只留 PERF 行
./build/arm/smartcam --device /dev/video0 --fmt mjpeg --http-port 8080 2>&1 | grep PERF

# 用例 B：浏览器开 http://<IP>:8080/ 后再跑同样命令
# 用例 C：ffplay rtsp://<IP>:8554/stream 后再跑同样命令
# 用例 D：不用 grep，全量日志跑 1h，另开终端采样 rss
while true; do grep VmRSS /proc/$(pidof smartcam)/status; sleep 10; done
```

**数据处理**：
1. 每种用例**跑 3 次 × 60s**；
2. 跳过前 10s（启动预热），取后续稳定值；
3. 计算平均 ± 标准差，记录波动；
4. 前后对比 `copy`、`cpu`、`frames`、`rss`。

### 4.5 预期结果

| 指标 | 改造前（基线） | 改造后（预期） | 判定标准 |
|------|---------------|----------------|----------|
| `copy` | ~10MB/s | ~2~3MB/s | 显著下降（消除 ③④） |
| `+pix` | ~9.5MB/s | ~9.5MB/s | 基本不变（必需拷贝） |
| `frames` | ~10fps | ≥10fps（期望↑） | 不低于基线 |
| `cpu` | 99% | 下降或同帧率下降低 | 明显下降为佳 |
| `rss` | ~25MB | 平稳，无增长 | 1h 无泄漏 |

**验收结论标准**：若改造后 `cpu` 下降 ≥10% 或 `frames` 提升 ≥20%，且 `rss` 平稳，则判定优化有效；否则回退并分析。

---

## 五、分步骤实现流程

### Phase 0：环境准备（0.5 天）

1. **确认编译环境**：
   ```bash
   # 宿主机（云端/PC）
   arm-linux-gnueabihf-g++ --version      # ARM 交叉编译器
   docker --version                        # Docker（sysroot 交叉编译用）
   ls npi-sysroot/usr/lib/arm-linux-gnueabihf/libQt5Core.so.5.11.3   # sysroot 已解压
   ```
2. **确认构建流程可用**（跑通一次基线编译）：
   ```bash
   ./scripts/sysroot-setup.sh             # 一键：解压 sysroot + docker build + docker run
   # 产物: build/arm/smartcam
   ```
3. **开发板准备**：`git pull` 到 `416bf2d`（含 PERF 插桩），确认 `[PERF]` 能正常打印。

### Phase 1：FramePool 实现 + 单元测试（1.5 天）

1. 新建 `include/common/frame_pool.h`：`FrameSlot` + `FramePool` + `SlotGuard`（RAII 句柄）声明；
2. 新建 `src/common/frame_pool.cpp`：`acquire/share/release/publish` 实现（原子 + release/acquire 内存序）；
3. 新建 `tests/test_frame_pool.cpp` 单元测试：
   - 借还循环：capacity 个槽反复 acquire/release 不泄漏；
   - 并发：4 线程混合 share/release 下 refs 守恒（ASAN/TSan 无告警）；
   - 池满：acquire 返回 nullptr；
   - publish/share：share 到的必是已发布且数据完整（用 seq 校验）；
4. `tests/CMakeLists.txt` 注册测试；`CMakeLists.txt` 注册 `src/common/frame_pool.cpp`；
5. 本机 PC 编译跑单测通过。

### Phase 2：RGB 显示池接入 GUI（1.5 天）

> 本阶段只做 `g_rgbPool`（容量 2），不碰采集/推流。

1. `main.cpp`：创建 `g_rgbPool`（capacity=2）；
2. `gui.h/cpp`：新增 `setFrameShared(FrameSlot*)` + `m_heldSlot` 成员；`refreshFrame` 改用 QImage **浅引用**（去掉 `.copy()`）；
3. `main.cpp` displayTimer：改为 `g_rgbPool.share()` → `setFrameShared()`；
4. **解码位置调整**：当前解码在 `gui.setFrame → frameToQImage` 内部（GUI 线程）。改造后需在**调用 setFrameShared 前**先解码（仍 GUI 线程）写入 rgb 池槽，再 share 给 GUI——即把"解码+上屏"改为"解码入池→浅引用上屏"，消除 ④ 的 `.copy()`；
5. Mock 模式：不建池，保留旧 `setFrame` 直通（仅供 PC 调试）；
6. 编译 + Mock 冒烟 + 板上运行验证无花屏、帧序号单调。

### Phase 3：性能对比验证（1 天）

1. 用第四章流程分别采集改造前（基线，已有）与改造后数据；
2. 对比 `copy/cpu/frames/rss`，判定是否达标；
3. 达标 → 进入 Phase 4；不达标 → 分析（见第七章）后迭代或回退。

### Phase 4：稳定性验证 + 文档收尾（1 天）

1. ASAN/TSan 全套跑通（重点：refs 守恒、无 data race、无 use-after-free）；
2. 用例 D：1h 连续运行 + rss 采样，确认无泄漏；
3. 720p 模拟压测（如摄像头支持）验证丢帧与反压策略；
4. 更新本文档（填入改造后实测数据）+ 同步 `plan-frame-pool-zero-copy.md` + 面试复习文档。

**总工期：4~6 天**（与设计稿一致）。

---

## 六、关键代码与配置变更说明

### 6.1 `FramePool` 核心实现要点

```cpp
FrameSlot* FramePool::acquire() {
    for (auto& s : m_slots) {
        int expected = 0;
        if (s->refs.compare_exchange_strong(expected, 1))  // 0→1 借出
            return s.get();
    }
    return nullptr;   // 池满 → 丢帧
}

FrameSlot* FramePool::share() {
    FrameSlot* cur = m_current.load(std::memory_order_acquire);  // 先取指针
    if (cur) cur->refs.fetch_add(1);
    return cur;
}

void FramePool::publish(FrameSlot* s) {
    // 先确保 data 写完整，再发布指针（release 语义）
    std::atomic_thread_fence(std::memory_order_release);
    m_current.store(s, std::memory_order_release);
}

void FramePool::release(FrameSlot* s) {
    s->refs.fetch_sub(1);   // 归 0 后槽重新可被 acquire
}
```

### 6.2 `SlotGuard`（RAII，防漏 release）

```cpp
struct SlotGuard {           // 所有 share() 结果必须包进此句柄
    FramePool* pool;
    FrameSlot* slot;
    ~SlotGuard() { if (slot) pool->release(slot); }
    FrameSlot* get() const { return slot; }
    // 禁拷贝，允许移动
};
```

### 6.3 GUI 侧变更

```cpp
// gui.h 新增
void setFrameShared(FrameSlot* slot);   // GUI 持有该槽引用
FrameSlot* m_heldSlot = nullptr;        // GUI 当前持有槽

// gui.cpp
void CameraGUI::setFrameShared(FrameSlot* slot) {
    if (m_heldSlot) g_rgbPool.release(m_heldSlot);   // 释放上一帧
    m_heldSlot = slot;                                // 持新引用（refs 已+1）
    m_currentFrame.data   = slot->data.data();        // 零拷贝指向共享数据
    m_currentFrame.length = (int)slot->data.size();
    m_currentFrame.width  = slot->width;
    m_currentFrame.height = slot->height;
    m_currentFrame.format = PixelFormat::FMT_RGB24;
    m_currentFrame.index++;
}

// refreshFrame 中：QImage 浅引用，不 .copy()
QImage img(m_currentFrame.data, w, h, w*3, QImage::Format_RGB888);
m_videoDisplay->setPixmap(QPixmap::fromImage(img));   // 唯一上屏拷贝
```

**生命周期不变量**：
- GUI 始终持有且仅持有 1 份 `m_heldSlot` 引用（refs≥1）→ 解码方不会回收该槽；
- `refreshFrame` 内 QImage 是临时对象，作用域结束即毁，不跨帧持有 → 无悬垂；
- 下一帧 `setFrameShared` 才 release 旧槽 → 归零后复用。

### 6.4 CMakeLists 变更

```cmake
# 新增源文件
set(COMMON_SOURCES
    src/common/frame_pool.cpp
    include/common/frame_pool.h
)
# 加入 ALL_SOURCES
```

---

## 七、可能遇到的问题及解决方案

| # | 问题 | 影响 | 解决方案 |
|---|------|------|----------|
| 1 | **漏 release → 槽永不回收** | 池被占满 → 持续丢帧/画面停更 | `SlotGuard` RAII 全覆盖；加"槽泄漏探针"（空闲槽数 < 预期则告警） |
| 2 | **多 release / 悬垂** | use-after-free 崩溃 | 只允许 `SlotGuard` 管理引用；单元测试用 ASAN |
| 3 | **QImage 浅引用悬垂** | 花屏/崩溃 | 生命周期不变量（GUI 始终持 m_heldSlot）；refreshFrame 内 QImage 为临时对象 |
| 4 | **原子内存序用错** | 读到半写帧 | publish=release / share=acquire；TSan 验证 |
| 5 | **GUI 重复绘制同一帧** | 浪费 setPixmap | `seq` 去重：setFrameShared 记录 seq，refreshFrame 比对后跳过 |
| 6 | **`g_state.frameData` 被其他模块引用** | 编译错误/遗漏 | 全局 grep `frameData` 逐一确认；元数据字段（width/height/fps）保留在 g_state |
| 7 | **单核竞争加剧** | 更卡 | 帧池**不增加线程**（区别于已回退的解码线程方案）；若仍卡，降显示解码分辨率（见第八章） |
| 8 | **内存增加（池预分配）** | 常驻 +1.84MB | 已计入预算（512MB 占比极小） |
| 9 | **解码仍在 GUI 线程（本实施）** | 解码 ~25ms 阻塞事件循环 | 当前基线本就如此（GUI 线程解码）；若帧率仍受限，二期考虑"低分辨率显示解码"（320x240 解码 ~8ms）而非独立线程 |
| 10 | **基线 CPU 已 99%** | 优化空间有限 | 重点验证"同帧率下 CPU 下降"或"同 CPU 下帧率提升"；结合推流场景综合评估 |

---

## 八、性能评估与结论

### 8.1 评估方法

以第四章采集的**改造前后实测数据**为准，填充下表：

| 指标 | 改造前基线 | 改造后实测 | 变化 | 判定 |
|------|-----------|-----------|------|------|
| copy (MB/s) | ~10 | 待测 | 期望 -40% 以上 | |
| +pix (MB/s) | ~9.5 | 待测 | 期望不变 | |
| frames (fps) | ~10 | 待测 | 期望 ≥10 | |
| cpu (%) | 99 | 待测 | 期望下降 | |
| rss (KB) | ~25M | 待测 | 期望平稳 | |

### 8.2 结论判定

- **优化有效**：`cpu` 下降 ≥10% 或 `frames` 提升 ≥20%，且 `rss` 平稳、无花屏、推流/录像内容一致；
- **优化无效**：回退，并考虑"低分辨率显示解码"（见 8.3）；
- **部分有效**：保留 RGB 池，二期再评估 raw 池。

### 8.3 后续演进方向（二期，不在本期范围）

| 方向 | 说明 | 收益 |
|------|------|------|
| raw 池（原始帧零拷贝） | 采集/处理/显示共享 JPEG | 再省 ~3MB/s（收益小，风险高，优先度低） |
| 低分辨率显示解码 | 显示解码到 320x240 再放大 | 解码 25ms→~8ms，GUI 线程不卡（**比独立线程更适配单核**） |
| 720p 支持 | RGB 池使 2.76MB/帧搬运归零 | 分辨率升级不受搬运带宽约束 |
| 推流路径优化 | HTTP/RTSP 直接引用池槽发送 | 网络路径零拷贝 |

---

## 九、附：实测基线数据（改进前）

> 采集时间: 2026-08-03 | commit: `416bf2d`（含 PERF 插桩）
> 场景: 用例 A（纯显示，MJPEG 640x480，无人访问流）
> 命令:
> ```bash
> ./build/arm/smartcam --device /dev/video0 --fmt mjpeg --http-port 8080 2>&1 | grep PERF
> ```

### 9.1 原始输出

```
[PERF] copy=0.0MB/s (+pix 0.0) frames=0.0fps cpu=0%  rss=25076KB   ← 启动预热
[PERF] copy=10.0MB/s (+pix 9.5) frames=10.1fps cpu=99% rss=25080KB
[PERF] copy=10.0MB/s (+pix 9.5) frames=10.0fps cpu=99% rss=25080KB
[PERF] copy=9.9MB/s (+pix 9.4) frames=10.0fps cpu=99% rss=26132KB
[PERF] copy=10.1MB/s (+pix 9.6) frames=9.8fps cpu=99% rss=25080KB
[PERF] copy=9.9MB/s (+pix 9.4) frames=10.0fps cpu=100% rss=26132KB
[PERF] copy=10.1MB/s (+pix 9.6) frames=10.0fps cpu=99% rss=25080KB
[PERF] copy=10.0MB/s (+pix 9.5) frames=9.9fps cpu=99% rss=25080KB
[PERF] copy=9.9MB/s (+pix 9.4) frames=10.0fps cpu=99% rss=26132KB
[PERF] copy=10.2MB/s (+pix 9.6) frames=10.0fps cpu=99% rss=25084KB
[PERF] copy=10.0MB/s (+pix 9.4) frames=10.0fps cpu=100% rss=26136KB
[PERF] copy=10.2MB/s (+pix 9.6) frames=10.0fps cpu=100% rss=25084KB
[PERF] copy=9.9MB/s (+pix 9.4) frames=10.0fps cpu=99% rss=26136KB
[PERF] copy=10.2MB/s (+pix 9.6) frames=9.8fps cpu=99% rss=25084KB
[PERF] copy=10.0MB/s (+pix 9.5) frames=10.0fps cpu=99% rss=25084KB
```

### 9.2 基线数据统计（跳过前 10s 预热）

| 指标 | 均值 | 波动范围 | 结论 |
|------|------|----------|------|
| `copy` | **10.0 MB/s** | 9.9 ~ 10.2 | 稳定 |
| `+pix` | **9.5 MB/s** | 9.4 ~ 9.6 | 稳定 |
| `frames` | **10.0 fps** | 9.8 ~ 10.1 | **远低于 30fps 设计目标** |
| `cpu` | **99%** | 99 ~ 100 | **单核已打满** |
| `rss` | **~25.3 MB** | 25080 ~ 26136 | 平稳 |

### 9.3 基线解读（关键发现）

1. **CPU 已 99% 打满，帧率仅 10fps**：单核资源耗尽，帧率被 CPU 瓶颈卡死（摄像头硬件 30fps 但软件处理不过来）；
2. `copy` 10MB/s 中，**RGB 部分（④⑤）占 ~19MB/s**（9.5×2），是最大可优化项——**消除 ④ 后预计节省 ~9.2MB/s**；
3. `+pix` 9.5MB/s 是上屏必需拷贝，改造后不变，属于合理保留；
4. `rss` 25MB 平稳，无内存泄漏迹象，可作为后续对比基准。

**改造后的预期基线对比**（填入第八章表格）：
- `copy`：~10 → **~2~3MB/s**（消除 ③④）
- `+pix`：~9.5 → ~9.5（不变）
- `cpu`：99% → **期望明显下降**（RGB 拷贝释放的 CPU）
- `frames`：10 → **期望 ≥10**（若 CPU 释放后采集/解码加快，可能提升）

---

## 附：变更历史

| 日期 | commit | 内容 |
|------|--------|------|
| 2026-08-03 | `416bf2d` | 新增 `[PERF]` 性能插桩，采集基线 |
| 2026-08-03 | 本文档 | 建立实施指南，记录基线数据 |
| 待定 | Phase 1~4 | FramePool 实现 → RGB 池接入 → 对比验证 → 稳定性 |
