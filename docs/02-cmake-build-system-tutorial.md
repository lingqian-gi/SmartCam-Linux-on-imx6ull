# CMake 构建系统详解 — 以 SmartCam 项目为例

> **阅读前提**：已了解 `g++ -c main.cpp -o main.o` 和 `g++ main.o -o app` 的基本编译链接流程。  
> **适用人群**：零 CMake 基础，想彻底理解 build/ 目录如何运作。

---

## 一、先回答核心问题：CMake 到底干了什么？

CMake **不直接编译代码**。它的职责是：

```
CMakeLists.txt  ──cmake──▶  Makefile  ──make──▶  可执行文件 smartcam
  (你写的)                  (自动生成)           (最终产物)
```

打个比方：
- **cmake** = 建筑设计师 → 根据图纸（CMakeLists.txt）画出施工图（Makefile）
- **make**   = 施工队 → 按照施工图（Makefile）真正盖房子（编译出 binary）

---

## 二、build/ 目录全景图

先看看我们现在项目的 build 目录：

```
build/                                    ← cmake 的输出目录
│
├── CMakeCache.txt                        ← ① 缓存：所有配置变量
├── CMakeFiles/                           ← ② CMake 内部文件
│   ├── 3.25.1/                           │   编译器检测结果
│   │   ├── CMakeCCompiler.cmake           │   C 编译器探测
│   │   ├── CMakeCXXCompiler.cmake         │   C++ 编译器探测
│   │   ├── CMakeSystem.cmake              │   系统信息
│   │   └── CompilerIdC[XX]/               │   编译器 ID 检测
│   ├── Makefile.cmake                     │   根 Makefile 的 cmake 描述
│   ├── Makefile2                          │   内部递归 Makefile
│   ├── TargetDirectories.txt              │   构建目标目录清单
│   │
│   ├── smartcam.dir/                      ← ③ smartcam 目标专属文件
│   │   ├── build.make                     │   编译规则（怎么说 .cpp → .o）
│   │   ├── depend.make                    │   依赖关系（.h 变了该重编译谁）
│   │   ├── flags.make                     │   编译 flags（-O2 -g -fPIC ...）
│   │   ├── link.txt                       │   链接命令（.o → 可执行文件）
│   │   ├── DependInfo.cmake               │   依赖信息缓存
│   │   └── progress.make                  │   编译进度标记
│   │
│   └── smartcam_autogen.dir/              ← ④ Qt MOC 自动生成代码
│       ├── build.make                     │   MOC 编译规则
│       └── ParseCache.txt                 │   Q_OBJECT 解析缓存
│
├── Makefile                              ← ⑤ 顶层入口 Makefile
├── cmake_install.cmake                    ← ⑥ install 规则
│
├── smartcam_autogen/                      ← ⑦ MOC 生成的 .cpp 源码
│   ├── ELDKJXD2K7/                        │   (随机目录名，防冲突)
│   │   └── moc_gui.cpp                    │   gui.h 的 MOC 产物
│   ├── moc_predefs.h                      │   MOC 预定义宏
│   └── mocs_compilation.cpp               │   MOC 聚合编译文件
│
└── smartcam                               ← ⑧ 最终可执行文件！
```

---

## 三、逐层拆解：从 CMakeLists.txt 到 build/ 目录

### 3.1 第一步：`cmake ..` 做了什么？

当你执行：
```bash
mkdir build && cd build
cmake ..
```

CMake 的流水线：

```
CMakeLists.txt
    │
    ├── [1] cmake_minimum_required(VERSION 3.10)
    │       → 检查 cmake 版本是否 ≥ 3.10
    │
    ├── [2] project(SmartCam VERSION 0.1.0 LANGUAGES CXX C)
    │       → 设定项目名、版本、需要的语言
    │       → CMake 此时探测编译器 → 生成 CMakeCCompiler.cmake 等
    │
    ├── [3] set(CMAKE_CXX_STANDARD 17)
    │       → 读写 CMakeCache.txt，记录 "我要用 C++17"
    │
    ├── [4] find_package(Qt5 REQUIRED COMPONENTS Widgets)
    │       → 搜索系统里有没有 Qt5
    │       → 找到 → 设置 Qt5::Widgets 这个目标
    │       → 没找到 → 报错退出
    │
    ├── [5] set(ALL_SOURCES ...)
    │       → 纯变量赋值，还没任何动作
    │
    ├── [6] set(CMAKE_AUTOMOC ON)
    │       → 告诉 CMake："遇到 Q_OBJECT 时自动调用 moc"
    │       → 这会生成 smartcam_autogen/ 目录的规则
    │
    ├── [7] add_executable(smartcam ${ALL_SOURCES})
    │       → 核心！定义一个构建目标 smartcam
    │       → 为它创建 CMakeFiles/smartcam.dir/ 目录
    │       → 为每个 .cpp 生成 .o 编译规则
    │
    ├── [8] target_include_directories(smartcam PRIVATE ...)
    │       → 给 smartcam 目标加上 -I 头文件搜索路径
    │       → 写入 flags.make 的 CXX_INCLUDES 行
    │
    ├── [9] target_link_libraries(smartcam PRIVATE Qt5::Widgets)
    │       → 链接 Qt5 库 → 写入 link.txt
    │
    └── [10] 生成 Makefile
            → 写出 build/ 里所有文件
```

### 3.2 关键文件逐行解释

#### ① CMakeCache.txt — 全局配置变量

这是 cmake 的"记忆"。每次跑 cmake 都会先读这个文件，所以第二次 cmake 比第一次快。

关键内容示例：
```cmake
// C++ 编译器
CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/c++

// 构建类型（Debug / Release）
CMAKE_BUILD_TYPE:STRING=

// Qt 安装路径（find_package 找到的）
Qt5_DIR:PATH=/usr/lib/x86_64-linux-gnu/cmake/Qt5

// 是否自动 MOC
CMAKE_AUTOMOC:BOOL=ON
```

**实用技巧**：`cmake -DCMAKE_BUILD_TYPE=Debug ..` 就是往 CMakeCache.txt 写入 `Debug`，等价于：
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug
```

#### ⑤ Makefile — 顶层入口

这是你敲 `make` 时实际执行的文件。它只有 298 行，大部分是 CMake 自动生成的模板代码。核心逻辑是：

```makefile
all: cmake_check_build_system
    $(MAKE) -f CMakeFiles/Makefile2 all    # ← 委托给 Makefile2
```

**所以真正的编译规则在 `CMakeFiles/Makefile2` → `CMakeFiles/smartcam.dir/build.make`**

#### ③ `smartcam.dir/build.make` — 编译规则

这是最重要的文件。它定义了每条编译命令。以 `src/main.cpp` 为例：

```makefile
# 编译: main.cpp → main.cpp.o
src/main.cpp.o:
    /usr/bin/c++ \
        -DQT_CORE_LIB -DQT_WIDGETS_LIB \       # ← 来自 flags.make 的 DEFINES
        -I/workspace/.../include \              # ← 来自 flags.make 的 INCLUDES
        -I/usr/include/.../Qt5Widgets \
        -O2 -g -fPIC -std=c++17 \               # ← 来自 flags.make 的 CXX_FLAGS
        -c src/main.cpp \                       # -c = 只编译不链接
        -o CMakeFiles/smartcam.dir/src/main.cpp.o
```

链接规则在 **`link.txt`**：
```bash
# 链接: main.cpp.o + gui.cpp.o + moc_gui.cpp.o → smartcam
/usr/bin/c++ \
    CMakeFiles/smartcam.dir/smartcam_autogen/mocs_compilation.cpp.o \
    CMakeFiles/smartcam.dir/src/display/gui.cpp.o \
    CMakeFiles/smartcam.dir/src/main.cpp.o \
    -o smartcam \
    /usr/lib/.../libQt5Widgets.so \
    /usr/lib/.../libQt5Core.so
```

> **核心理解**：CMake 不直接运行 g++。它只是**算出**该用什么参数，**写出**这些参数到文件里。make 才是真正调用 g++ 的那个。

#### ③ `flags.make` — 编译参数集中管理

```makefile
CXX_DEFINES  = -DQT_CORE_LIB -DQT_NO_DEBUG -DQT_WIDGETS_LIB
CXX_INCLUDES = -I.../include -I.../Qt5Widgets ...
CXX_FLAGS    = -O2 -g -fPIC -std=c++17
```

**为什么分离出来？** 如果一百个 .cpp 都用相同的 flags，CMake 把它们写在一个文件里，所有编译规则引用它即可。改一个地方 = 全局生效。

#### ⑦ `smartcam_autogen/` — Qt MOC 的黑魔法

Qt 的信号/槽机制（`Q_OBJECT` 宏）不是标准 C++，编译器不认识。所以 Qt 提供了一个**元对象编译器（MOC）**，它扫描头文件，找到 `Q_OBJECT`，自动生成额外的 `.cpp` 代码。

```
gui.h (你写的)                    moc 扫描
  ├── class CameraGUI : QWidget    ──────────▶
  ├── Q_OBJECT                                     │
  ├── signals: captureClicked()                    ▼
  └── slots: refreshFrame()              moc_gui.cpp (自动生成)
                                            ├── qt_meta_stringdata_CameraGUI
                                            ├── qt_meta_data_CameraGUI
                                            ├── CameraGUI::qt_metacast()
                                            ├── CameraGUI::qt_metacall()
                                            └── CameraGUI::captureClicked() 实现
```

**目录结构：**
```
smartcam_autogen/
├── ELDKJXD2K7/           ← 随机哈希目录，防多个目标命名冲突
│   └── moc_gui.cpp       ← gui.h 的元对象代码（signal/slot 的实现）
├── moc_predefs.h         ← 编译器预定义宏（给 moc 用）
└── mocs_compilation.cpp  ← 聚合文件：只包含一行 #include "moc_gui.cpp"
```

`mocs_compilation.cpp` 就一行：
```cpp
#include "ELDKJXD2K7/moc_gui.cpp"
```

**为什么这样设计？** 早期写法是在 gui.cpp 末尾 `#include "moc_gui.cpp"`，现在 CMake AUTOMOC 把它抽离出来，独立编译，源文件更干净。

#### ⑧ smartcam — 最终二进制

`file build/smartcam` 会输出：
```
ELF 64-bit executable, x86-64, dynamically linked
```
这就是你可以 `./smartcam` 启动的东西。

---

## 四、完整流水线：一次 `make` 到底发生了什么？

```
make (读取 Makefile)
│
├── [Target 1] smartcam_autogen
│   └── 调用 moc，扫描 gui.h
│       → 生成 smartcam_autogen/ELDKJXD2K7/moc_gui.cpp
│
├── [Target 2] 编译 moc_gui.cpp → mocs_compilation.cpp.o
│   └── g++ -c ...  mocs_compilation.cpp -o .../mocs_compilation.cpp.o
│       └── 实际编译的是 moc_gui.cpp（通过 #include）
│
├── [Target 3] 编译 gui.cpp → gui.cpp.o
│   └── g++ -c ...  src/display/gui.cpp -o .../gui.cpp.o
│
├── [Target 4] 编译 main.cpp → main.cpp.o
│   └── g++ -c ...  src/main.cpp -o .../main.cpp.o
│
└── [Target 5] 链接 → smartcam
    └── g++  mocs_compilation.cpp.o gui.cpp.o main.cpp.o \
             -o smartcam  -lQt5Widgets -lQt5Core
```

每一步是否有 `.o`，由 make 自动判定（时间戳比较）。所以 **`make` 的第二遍几乎秒出**。

---

## 五、CMakeLists.txt 逐段精讲

### 5.1 项目声明

```cmake
cmake_minimum_required(VERSION 3.10)   # 最低 CMake 版本
project(SmartCam VERSION 0.1.0 LANGUAGES CXX C)
```

`project()` 会触发：
- 设置 `PROJECT_NAME` = "SmartCam"
- 设置 `CMAKE_CXX_COMPILER`（自动检测 g++）
- 设置 `CMAKE_SOURCE_DIR` = `CMakeLists.txt` 所在目录
- 设置 `CMAKE_BINARY_DIR` = `build/`

### 5.2 平台条件编译

```cmake
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    add_compile_options(-march=armv7-a -mfpu=neon)
else()
    add_compile_options(-O2 -g)
endif()
```

- `CMAKE_CROSSCOMPILING` — 当你用了 `-DCMAKE_TOOLCHAIN_FILE=toolchain.cmake` 时为真
- `add_compile_options` — 给所有目标加编译 flag（等价于 Make 的 `CFLAGS`）

### 5.3 查找依赖

```cmake
find_package(Qt5 REQUIRED COMPONENTS Widgets)
```

这一行背后做了什么：
1. 查找 `Qt5Config.cmake`（系统 Qt 安装自带）
2. 导入 `Qt5::Widgets` 这个**导入目标**（imported target）
3. `Qt5::Widgets` 自动包含：
   - 头文件路径：`/usr/include/x86_64-linux-gnu/qt5/QtWidgets`
   - 链接库：`libQt5Widgets.so`
   - 传递依赖：`Qt5::Core`、`Qt5::Gui`（自动传递！）

所以你在后面写 `target_link_libraries(smartcam PRIVATE Qt5::Widgets)` 时，**Qt5::Core 和 Qt5::Gui 也会自动链接**，不用你手动写。

### 5.4 源文件列表

```cmake
set(DISPLAY_SOURCES
    src/display/gui.cpp
    include/display/gui.h    # ← 注意！头文件也放进来了
)
```

**为什么把 .h 也放进来？**

因为 `gui.h` 里有 `Q_OBJECT` 宏。CMake 的 AUTOMOC 需要扫描这个文件并生成 `moc_gui.cpp`。把 .h 加入源文件列表 = 告诉 CMake "请对这个文件运行 MOC"。

> **最佳实践**：只有含 `Q_OBJECT` 的头文件需要加入源文件列表。普通 `.h` 不需要。

### 5.5 定义可执行文件

```cmake
add_executable(smartcam ${ALL_SOURCES})
```

这是最核心的一条命令。它：
1. 创建一个叫 `smartcam` 的构建目标
2. 为每个源文件生成 `.o` 编译规则
3. 生成一条链接规则（所有 `.o` → `smartcam` 可执行文件）

后续的 `target_*` 命令都是**修饰**这个目标：

```cmake
# "这个目标编译时，去哪些目录找头文件"
target_include_directories(smartcam PRIVATE
    ${CMAKE_SOURCE_DIR}             # 项目根目录
    ${CMAKE_SOURCE_DIR}/include     # include/ 目录
)

# "这个目标链接时，需要哪些库"
target_link_libraries(smartcam PRIVATE
    Qt5::Widgets
)
```

### 5.6 关键字 PRIVATE / PUBLIC / INTERFACE

这是 CMake 里最让人困惑的概念，但核心很简单：

| 关键字 | 含义 | 什么时候用 |
|--------|------|------------|
| `PRIVATE` | 只自己用，不给依赖者 | **可执行文件**必然 PRIVATE |
| `PUBLIC` | 自己用，也给依赖者 | **库**暴露接口时 |
| `INTERFACE` | 自己不用，只给依赖者 | header-only 库 |

SmartCam 是 **可执行文件**，不是库，所以全部用 `PRIVATE`。

### 5.7 安装规则

```cmake
install(TARGETS smartcam RUNTIME DESTINATION /usr/local/bin)
install(FILES configs/smartcam.conf DESTINATION /etc/smartcam OPTIONAL)
```

这些规则**在 `make` 时不会执行**。只有你显式跑 `make install` 时才生效。等价于：
```bash
cp build/smartcam  /usr/local/bin/
cp configs/smartcam.conf  /etc/smartcam/
```

---

## 六、常用操作速查

### 6.1 编译

```bash
# 标准三件套
mkdir build && cd build
cmake ..
make -j$(nproc)            # -j$(nproc) = 用所有 CPU 核心并行编译

# 一句话版本
cmake -B build && make -C build -j$(nproc)
```

### 6.2 改配置重新编译

```bash
cd build

# 切换构建类型
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 或者，cmake 检测到 CMakeLists.txt 变了会自动重新生成
make -j$(nproc)   # 如果 CMakeLists.txt 改了，这一步就够了
```

### 6.3 交叉编译

```bash
mkdir build/arm && cd build/arm
cmake ../.. -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake
make -j$(nproc)
```

`toolchain.cmake` 的内容：
```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER   arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
```
CMake 读到这个文件后，`CMAKE_CROSSCOMPILING` 自动变为 `ON`。

### 6.4 调试编译过程

```bash
# 看 CMake 实际执行的命令
make VERBOSE=1

# 只看一个文件的编译命令
make VERBOSE=1 2>&1 | grep "main.cpp"

# 手动重现某条编译（从 build.make 或 flags.make 复制）
/usr/bin/c++ -O2 -g -fPIC -std=c++17 -I.../include -c src/main.cpp -o main.o
```

### 6.5 清理

```bash
# 删掉所有编译产物，保留 CMake 配置
make clean

# 完全重置（清掉 CMakeCache.txt）
rm -rf build && mkdir build && cd build && cmake ..
```

---

## 七、CMake 最佳实践总结

| 原则 | 说明 |
|------|------|
| **用 target_\* 不用全局 set** | `target_include_directories` 优于 `include_directories`。每个目标的依赖隔离，不会互相污染 |
| **头文件放进源文件列表** | 含 `Q_OBJECT` 的 .h 必须加入源文件，让 AUTOMOC 处理 |
| **build 目录外置** | 永远在 `build/` 下编译。`.gitignore` 里加 `build/` |
| **用 PRIVATE 修饰可执行文件** | 可执行文件不需要暴露接口，全部 PRIVATE |
| **find_package 而不是硬编码路径** | `find_package(Qt5)` 跨平台，`-I/usr/local/qt/include` 不跨平台 |
| **用 CMakeCache.txt 排查问题** | 变量找不到？先看 CMakeCache.txt 里有没有 |

---

## 八、对照练习

试试在 VM 里执行：

```bash
# 1. 完全清空重来
cd ~/SmartCam-Linux-on-imx6ull
rm -rf build && mkdir build && cd build

# 2. 只生成不编译（检查 CMake 阶段有无报错）
cmake ..

# 3. 看 CMakeCache.txt 里记录了什么
grep -E "CMAKE_CXX|Qt5_DIR" CMakeCache.txt

# 4. 看编译 flags
cat CMakeFiles/smartcam.dir/flags.make

# 5. 看链接命令
cat CMakeFiles/smartcam.dir/link.txt

# 6. 看 MOC 生成的代码（理解了就不神了）
cat smartcam_autogen/ELDKJXD2K7/moc_gui.cpp | head -40

# 7. 实际编译
make -j$(nproc)

# 8. 重复编译（观察"nothing to be done"）
make -j$(nproc)    # 看到 [100%] Built target smartcam 秒出

# 9. 看最终产物
file smartcam
ls -lh smartcam
./smartcam --help   # (VM 有桌面的话会出 GUI)
```

这样你就能彻底理解 CMake 的每一步在做什么了。
