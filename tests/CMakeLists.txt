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
