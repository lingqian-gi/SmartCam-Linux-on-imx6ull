# SmartCam 项目变更日志 — 2026-05-29

> 本文记录本次会话中所有的代码修改，包含问题背景、解决方案、修改明细和影响范围。
> 共 4 项变更：相册多选删除 | FPS 诊断日志移除 | 删除确认弹窗样式修复 | 全局 UI 现代化改造。

---

## 变更 1：相册 Gallery 模块 — 多选删除功能

### 1. 遇到的问题

Gallery 模块原先仅支持**逐条删除**照片/视频（在全屏查看模式下点删除按钮）。当相册中媒体文件积累较多时（>50 条），逐条删除效率极低，用户体验差。

> 此需求来自项目待办文档 `docs/09-gallery-module-implementation.md` §十五："多选删除（全选/反选 + 批量删除）"。

### 2. 解决方案

在 Gallery 视图中引入**多选模式**：

- **进入多选**：网格视图顶部栏新增 `Select` 按钮，点击后每个缩略图左上角叠加 `QCheckBox`，同时显示多选工具栏
- **选择交互**：点击缩略图或复选框均可切换选中状态，`Delete (N)` 按钮实时显示选中计数
- **快捷操作**：`Select All` / `Deselect All` 一键全选/取消
- **批量删除**：点击 `Delete (N)` 弹出确认弹窗 → 逐个调用 `StorageManager::deletePhoto()` / `deleteVideo()` → 退出多选 → 刷新列表
- **退出多选**：点击 `Cancel` 按钮或 `reset()` 方法均可安全退出，隐藏所有复选框

### 3. 修改明细

#### 3.1 `include/display/gallery.h` — 新增成员

| 新增内容 | 行号区域 (约) | 说明 |
|----------|:--------:|------|
| `#include <QCheckBox>` / `#include <QSet>` | L8-L9 | 复选框和多选集合依赖 |
| `void onToggleSelectMode()` | L59 | 切换多选模式槽 |
| `void onSelectAll()` | L60 | 全选槽 |
| `void onDeselectAll()` | L61 | 取消全选槽 |
| `void onItemSelectionChanged(int, bool)` | L62 | 单个项勾选变化槽 |
| `void onDeleteSelected()` | L63 | 批量删除槽 |
| `void updateDeleteSelectedButton()` | L84 | 更新删除按钮文字/启用状态 |
| `bool m_selectMode` | L103 | 是否处于多选模式 |
| `QSet<int> m_selectedIndices` | L104 | 选中的 `m_flatPhotos` 索引集合 |
| `QPushButton* m_btnSelectToggle` | L105 | Select/Cancel 切换按钮 |
| `QPushButton* m_btnSelectAll` | L106 | 全选按钮 |
| `QPushButton* m_btnDeleteSelected` | L107 | 批量删除按钮 |
| `QWidget* m_selectToolBar` | L108 | 多选工具栏容器 |
| `QVector<QCheckBox*> m_checkBoxes` | L109 | 每个缩略图的复选框引用 |

#### 3.2 `src/display/gallery.cpp` — 核心逻辑新增

| 位置 | 修改内容 |
|------|----------|
| `buildGalleryView()` | 顶部栏新增 `m_btnSelectToggle` 按钮；新增 `m_selectToolBar` 工具栏（含全选/取消全选/删除按钮），默认隐藏 |
| `clearThumbnails()` | 增加 `m_checkBoxes.clear()` |
| `loadVisibleThumbnails()` | 每个缩略图左侧叠加 `QCheckBox`（`setParent(btn)` + `move(4,4)`）；多选模式下缩略图点击切换勾选（非全屏跳转） |
| `reset()` | 进入相册时自动退出多选模式 |
| `onToggleSelectMode()` (L852) | 切换 `m_selectMode` 标志，显示/隐藏工具栏和复选框，刷新缩略图 |
| `onSelectAll()` (L882) | 遍历全部 `m_flatPhotos` 插入 `m_selectedIndices`，同步所有 checkbox 状态 |
| `onDeselectAll()` (L—) | 清空 `m_selectedIndices`，所有 checkbox 设为未勾选 |
| `onItemSelectionChanged()` (L—) | 根据 checkbox 状态更新 `m_selectedIndices`，刷新删除按钮 |
| `updateDeleteSelectedButton()` (L—) | 更新 `m_btnDeleteSelected` 文字为 `Delete (N)` 并设置 enabled |
| `onDeleteSelected()` (L917) | 确认弹窗 → 遍历 `m_selectedIndices` 逐个删除 → 部分失败时弹出 warning → 退出多选 → `refresh()` |

### 4. 影响范围

| 模块 | 影响 |
|------|------|
| Gallery 模块 (`display/`) | 缩略图加载逻辑新增复选框叠层；多选模式下缩略图点击行为改变（不再跳转全屏） |
| Storage 模块 (`storage/`) | 无修改，仅调用已有 `deletePhoto()` / `deleteVideo()` |
| GUI 主界面 (`gui.cpp`) | 无修改，Gallery 通过 `reset()` 退出多选模式 |

---

## 变更 2：FPS 诊断日志移除

### 1. 遇到的问题

`main.cpp` 采集线程中每 100 帧输出一条诊断日志：

```
23:39:57 [INFO] [FPS Diag] avg=10.0 fps (99.9 ms/frame), raw interval: min=99.4 ms, max=103.7 ms, frame size=19450 bytes, throttle=0 fps
```

该日志在稳定运行期间产生大量冗余输出，淹没了其他有用日志，且不利于嵌入式设备的日志存储空间管理。

### 2. 解决方案

完全删除 FPS 诊断代码块，包括：
- 4 个诊断变量声明：`diagLastTime`、`diagFrameCount`、`diagMinInterval`、`diagMaxInterval`
- 每 100 帧触发的 `LOG_INF("[FPS Diag] ...")` 输出代码块（含 `g_state.mtx` 加锁读取 `g_state.fps`）

FPS 信息仍可通过状态栏实时看到（`CameraGUI::setFPS()`），无需冗余日志。

### 3. 修改明细

| 文件 | 位置（旧） | 删除内容 |
|------|:--------:|------|
| `src/main.cpp` | L749-L752 | 4 个诊断变量声明及初始化 |
| `src/main.cpp` | L789-L810 | 每帧间隔测量 + 100 帧汇总 LOG_INF 输出代码块（共 ~22 行） |

### 4. 影响范围

| 模块 | 影响 |
|------|------|
| 采集线程 (`main.cpp`) | 帧采集热路径减少 ~10 行代码，降低每帧开销 |
| 日志输出 | 每 100 帧减少一行 INFO 日志 |
| 其他模块 | 无影响 — FPS 数据仅通过 `CameraGUI::setFPS()` 在 UI 状态栏展示 |

---

## 变更 3：Gallery 删除确认弹窗样式修复

### 1. 遇到的问题

Gallery 背景色为 `#0a0a1a`（深黑蓝），删除照片/视频时弹出的 `QMessageBox::question()` 和 `QMessageBox::warning()` **继承了父窗口的背景风格**，导致弹窗背景与 Gallery 背景几乎融为一体，用户难以辨别弹窗边界。

### 2. 解决方案

将所有 3 处 `QMessageBox` 从静态方法调用改为手动构建对象，并显式设置 `setStyleSheet()`：

- 弹窗背景 `#21262D`（中深灰）— 与 Gallery 背景 `#0F1117` 形成清晰对比
- 警告类弹窗使用红色边框 `#F85149`（2px）
- 错误类弹窗使用橙色边框 `#D29922`（2px）
- 按钮使用 `#30363D` 背景 + hover 效果

### 3. 修改明细

| 文件 | 函数 | 旧代码 | 新代码 |
|------|------|--------|--------|
| `gallery.cpp` | `onDeletePhoto()` (L787) | `QMessageBox::question(this, ...)` 静态调用 | `QMessageBox msgBox(this)` + `setStyleSheet()` + `exec()` |
| `gallery.cpp` | `onDeleteSelected()` (L922) | `QMessageBox::question(this, ...)` 静态调用 | `QMessageBox msgBox(this)` + `setStyleSheet()` + `exec()` |
| `gallery.cpp` | `onDeleteSelected()` — 部分失败 (L961) | `QMessageBox::warning(this, ...)` 静态调用 | `QMessageBox msgBox(this)` + `msgBox.setIcon(Critical)` + `setStyleSheet()` + `exec()` |

关键样式：
```cpp
"QMessageBox { background-color: #21262D; }"
"QMessageBox { border: 2px solid #F85149; }"  // 警告类
"QMessageBox { border: 2px solid #D29922; }"  // 部分失败类
"QLabel { color: #E6EDF3; font-size: 14px; }"
"QPushButton { background-color: #30363D; ... }"
```

### 4. 影响范围

| 模块 | 影响 |
|------|------|
| Gallery 模块 | 3 处确认弹窗样式变更，功能行为不变 |
| 其他模块 | 无影响 — `QMessageBox` 仅在 Gallery 内使用 |

---

## 变更 4：全局 UI 现代化改造（配色体系焕新）

### 1. 遇到的问题

项目原有 UI 配色体系存在以下问题：

| 问题 | 具体表现 |
|------|----------|
| **低对比度** | 背景 `#0a0a1a` 与卡片 `#16213e` 对比度仅约 1.5:1，肉眼几乎无法区分层次 |
| **单调用色** | 全深蓝/深灰色调，缺乏色彩层次和视觉焦点 |
| **按钮扁平** | 按钮圆角仅 4px，配色暗淡（`#1a6fb5` / `#8e44ad`），缺乏现代感 |
| **状态标签粗糙** | FPS/LIVE/REC 标签像普通文字块，无 pill 质感 |
| **不符合设计规范** | 随机 hex 颜色散落在各处，无统一的 design token 体系 |

### 2. 解决方案

基于 [UI-UX-Pro-Max](https://github.com/nextlevelbuilder/ui-ux-pro-max-skill) 设计系统的 **Dark Mode (OLED) + Minimalism** 方案，并针对 SmartCam 的定位（Smart Home/IoT Dashboard）进行了定制适配：

#### 2.1 设计 Token 体系（Dark Slate 配色）

| Token | 旧值 | 新值 | 用途 |
|-------|------|------|------|
| `--bg-page` | `#0a0a1a` | `#0F1117` | 主背景，GitHub Dark 风格 |
| `--bg-surface` | `#16213e` | `#1A1D24` | 卡片/面板背景 |
| `--border` | `#0f3460` | `#30363D` | 通用边框，中性灰 |
| `--text-primary` | `#ecf0f1` | `#E6EDF3` | 主文字颜色 |
| `--text-secondary` | `#7f8c8d` | `#8B949E` | 次要文字（状态栏默认） |
| `--text-dim` | `#5a6c7d` | `#484F58` | 占位/禁用文字 |
| `--accent-blue` | `#1a6fb5` | `#4493F8` | 主色 — 捕获/关闭按钮 |
| `--accent-blue-border` | `#5aa9e6` | `#58A6FF` | 蓝色按钮边框 |
| `--accent-blue-pressed` | `#0d4a7a` | `#1F6FEB` | 蓝色按钮按下态 |
| `--accent-purple` | `#8e44ad` | `#A371F7` | 录像按钮 |
| `--accent-purple-border` | `#c084d6` | `#B892F9` | 录像按钮边框 |
| `--status-live-bg` | `#16213e` | `#15261A` | LIVE 状态背景（暗绿底） |
| `--status-live` | `#2ecc71` | `#3FB950` | LIVE 文字/边框（GitHub 绿） |
| `--status-rec-bg` | `#16213e` | `#2C1518` | REC 状态背景（暗红底） |
| `--status-rec` | `#e74c3c` | `#F85149` | REC 文字/边框 |
| `--status-mock-bg` | `#16213e` | `#272115` | MOCK 状态背景（暗琥珀底） |
| `--status-mock` | `#f39c12` | `#D29922` | MOCK 文字/边框 |
| `--btn-secondary-bg` | `#2c3e50` | `#21262D` | 次选按钮背景 |
| `--btn-secondary-border` | `#7f8c8d` | `#30363D` | 次选按钮边框 |
| `--slider-track` | `#0f3460` | `#21262D` | 滑块轨道 |
| `--slider-fill` | `#2980b9` | `#4493F8` | 滑块填充 |
| `--checkbox-unchecked` | `#7f8c8d` | `#30363D` | 复选框未选中边框 |

#### 2.2 交互细节改进

| 改进项 | 旧值 | 新值 |
|--------|------|------|
| 按钮圆角 | `4px` | `8px`（主界面），`18px`（圆形播放按钮） |
| 按钮字重 | `bold` | `700` |
| 状态标签样式 | 矩形带 `3px` 圆角 | Pill 胶囊 `12px` 圆角 + 细边框 |
| 状态标签 padding | `2px 6px` | `4px 10px` |
| 设置分组边框 | 直角 | `8px` 圆角 |
| 复选框尺寸 | `20px` | `22px` |
| 滑块拖动手柄 | `18px` | `20px` 带 `2px` 边框 |
| 弹窗按钮圆角 | `4px` | `8px` |

#### 2.3 linuxfb 兼容性约束

SmartCam 运行在嵌入式 linuxfb（Qt for Embedded Linux，无 X11/Wayland），以下 CSS 特性不可用：
- `box-shadow`（模糊阴影）→ 改用实色边框+圆角
- `backdrop-filter`（毛玻璃）→ 不适用
- 渐变（`linear-gradient`）→ 仅在两处简单的 2-stop 场景保留
- `@keyframes` 动画 → 不使用

所有样式均为纯色填充 + 实线边框，确保 linuxfb 下渲染正确。

### 3. 修改明细

#### 3.1 `src/display/gui.cpp`（主界面，~40 处修改）

| 行号区域 | 修改内容 |
|----------|----------|
| L132-L140 | 视频预览区：背景 `#0D1117`，边框 `#30363D`，圆角 `8px` |
| L165-L170 | 状态栏标签 pill 样式：`font-size: 12px`，`font-weight: 600`，`padding: 4px 10px`，`border-radius: 12px`，`border: 1px solid #30363D` |
| L188-L212 | 4 个主按钮：Capture 蓝 `#4493F8`，Record 紫 `#A371F7`，Gallery/Settings `#21262D`；全部 `font-weight: 700`，圆角 `8px`，`padding: 8px 18px`，`min-width: 84px` |
| L230 | 整体背景：`"background-color: #0F1117;"` |
| L340-L369 | Record 按钮 toggle：Stop 态 `#F85149` + 红底状态标签，正常态 `#A371F7` + 灰色状态标签 |
| L474-L491 | `setRecordingStatus()`：REC 标签红底 `#2C1518` + 文字 `#F85149`，off 态灰底 `#1A1D24` |
| L497-L506 | `setStreamingStatus()`：LIVE 标签绿底 `#15261A` + 文字 `#3FB950`，IDLE 态灰底 + 文字 `#8B949E` |
| L540-L545 | Settings 对话框：背景 `#0F1117`，分组 `#30363D` 边框 `8px` 圆角 |
| L554-L568 | ComboBox 滑块样式：背景 `#1A1D24`，选中高亮 `#388BFD`，滑块 `#4493F8` 手柄 `20px` |
| L660-L664 / L694-L703 | 复选框样式：`22px` 指示器，未选中 `#30363D` 边框 `#1A1D24` 背景，选中 `#4493F8` |
| L748-L759 | Settings 底部按钮：Reset `#21262D`，Close `#4493F8`，均 `8px` 圆角 |
| L1068-L1070 | Mock 模式标签：琥珀底 `#272115` + 文字 `#D29922` |

#### 3.2 `src/display/gallery.cpp`（相册模块，~30 处修改）

| 行号区域 | 修改内容 |
|----------|----------|
| L35 | 背景色：`#0F1117` |
| L108-L111 | Select 按钮按下态：`#161B22` |
| L134-L138 | Back 按钮样式同步 |
| L169 | 多选工具栏背景：`#1A1D24` |
| L175-L188 | Select All / Deselect All / Delete 按钮颜色同步 |
| L215-L221 | ScrollArea 背景 `#0F1117`，ScrollBar `#1A1D24` |
| L392-L394 | 存储信息标签：文字 `#484F58` |
| L400-L401 | 缩略图信息标签：文字 `#484F58` |
| L460 | 空相册提示：文字 `#484F58` |
| L470-L472 | 边距填充条：背景 `#1A1D24`，边框 `#30363D` |
| L501 | 网格分隔线：`#484F58` |
| L541-L552 | 缩略图按钮边框 `#30363D`，CheckBox 选中态 `#4493F8` |
| L606-L607 | 全屏 Back 按钮同步 |
| L623-L629 | 全屏媒体区背景 `#0F1117` |
| L730-L749 | 视频文件标签样式同步 |
| L660-L665 | Prev/Next 按钮：`#4493F8` / `#58A6FF` |
| L670 | Delete 按钮：`#F85149` / `#FF6B61` |
| L795-L800 | 单张删除确认弹窗样式 |
| L930-L935 | 批量删除确认弹窗样式 |
| L968-L973 | 删除部分失败弹窗样式 |

#### 3.3 `src/display/video_player.cpp`（视频播放器，~8 处修改）

| 行号区域 | 修改内容 |
|----------|----------|
| L52 | 视频显示区：背景 `#0F1117`，占位文字 `#484F58` |
| L70-L73 | 播放按钮：`#4493F8`，按下 `#1F6FEB` |
| L93-L97 | 进度条手柄和填充：`#3FB950` |
| L195-L198 | 暂停态按钮：`#D29922`，按下 `#B0881A` |

### 4. 影响范围

| 模块 | 影响程度 | 说明 |
|------|:------:|------|
| `display/gui.cpp` | 🔴 大 | 所有 UI 控件样式变更，layout 结构不变 |
| `display/gallery.cpp` | 🔴 大 | 网格视图 + 全屏视图 + 弹窗样式全量更新 |
| `display/video_player.cpp` | 🟡 中 | 仅颜色值替换，无结构变更 |
| Camera 采集 | 🟢 无 | 无修改 |
| MJPEG/RTSP 推流 | 🟢 无 | 无修改 |
| Storage 模块 | 🟢 无 | 无修改 |
| Control 协议 | 🟢 无 | 无修改 |

---

## 附录 A：UI 设计规范文件

本次改造遵循的 UI 设计规范已安装为项目 Skill 规则：

> **文件位置**: `.codebuddy/rules/ui-ux-pro-max.mdc`  
> **来源**: [UI-UX-Pro-Max v2.5.0](https://github.com/nextlevelbuilder/ui-ux-pro-max-skill)  
> **设计模式**: Smart Home/IoT Dashboard — Dark Mode (OLED) + Minimalism  
> **后续引用**: 执行 `@ui-ux-pro-max` 可激活该 Skill 的设计指南

## 附录 B：文件修改汇总

| 文件 | 本次变更行数（约） | 变更类型 |
|------|:------:|------|
| `include/display/gallery.h` | +20 行 | 新增成员/方法声明 |
| `src/display/gallery.cpp` | +180 / -20 行 | 多选删除 + 弹窗样式 + 配色 |
| `src/main.cpp` | -30 行 | 删除 FPS 诊断代码 |
| `src/display/gui.cpp` | ~45 处修改 | 全面配色焕新 |
| `src/display/video_player.cpp` | ~10 处修改 | 配色同步 |
| `.codebuddy/rules/ui-ux-pro-max.mdc` | 新增 | UI 设计规范 Skill |
| `docs/changelog-2026-05-29.md` | 本文档 | 变更日志 |
