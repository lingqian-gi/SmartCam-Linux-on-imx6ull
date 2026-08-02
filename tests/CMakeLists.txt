# ============================================================
# SmartCam 单元测试
#
# 说明:
#   - 测试目标不依赖 Qt，仅编译纯协议逻辑 (control.cpp)
#   - 通过 CTest 注册, 构建后运行 `ctest` 或 `ctest --verbose`
# ============================================================

add_executable(test_protocol
    test_protocol.cpp
    ${CMAKE_SOURCE_DIR}/src/network/control.cpp
)

target_include_directories(test_protocol PRIVATE
    ${CMAKE_SOURCE_DIR}
)

# ControlServer 实现使用 std::thread / std::mutex
find_package(Threads REQUIRED)
target_link_libraries(test_protocol PRIVATE Threads::Threads)

# 注册到 CTest
add_test(NAME test_protocol COMMAND test_protocol)
set_tests_properties(test_protocol PROPERTIES
    TIMEOUT 30
    # 构建产物随主程序一起安装到 build/install 时无需运行测试
)

# ============================================================
# 帧池单元测试（FramePool：双缓冲 + 引用计数）
# 纯 C++17 + Qt 头文件（types.h 的 PixelFormat 依赖 QtCore）
# ============================================================
add_executable(test_frame_pool
    test_frame_pool.cpp
)

target_include_directories(test_frame_pool PRIVATE
    ${CMAKE_SOURCE_DIR}
)

find_package(Qt5 REQUIRED COMPONENTS Core)
target_link_libraries(test_frame_pool PRIVATE
    Threads::Threads
    Qt5::Core
)

add_test(NAME test_frame_pool COMMAND test_frame_pool)
set_tests_properties(test_frame_pool PROPERTIES
    TIMEOUT 30
)
