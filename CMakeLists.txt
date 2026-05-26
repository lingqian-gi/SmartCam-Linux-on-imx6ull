cmake_minimum_required(VERSION 3.10)
project(SmartCam VERSION 0.1.0 LANGUAGES CXX C)

# ============================================================
# 编译选项
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 根据平台自动选择 flags
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    # ARM 交叉编译 (iMX6ULL Cortex-A7)
    add_compile_options(-march=armv7-a -mfpu=neon -mfloat-abi=hard -O2)
    message(STATUS "✓ ARM 交叉编译模式 (Cortex-A7 NEON)")
else()
    # x86 PC 开发调试
    add_compile_options(-O2 -g)
    message(STATUS "✓ x86 本地编译模式")
endif()

# ============================================================
# 依赖
# ============================================================

# Qt5 (Widgets)
find_package(Qt5 REQUIRED COMPONENTS Widgets)

# libjpeg-turbo (MJPEG 解码, 自定义错误处理器抑制坏帧警告)
find_package(JPEG REQUIRED)

# ARM 交叉编译时强制静态链接 libjpeg，避免开发板缺少 .so 库
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    find_library(JPEG_STATIC_LIB NAMES libjpeg.a libjpeg-turbo.a
                 PATHS ${CMAKE_FIND_ROOT_PATH}/usr/lib/arm-linux-gnueabihf
                       ${CMAKE_FIND_ROOT_PATH}/usr/lib/arm-linux-gnueabi
                       ${CMAKE_FIND_ROOT_PATH}/usr/lib
                       /usr/lib/arm-linux-gnueabihf
                       /usr/lib/arm-linux-gnueabi
                       ${JPEG_ROOT}
                       ${JPEG_LIBRARY_DIRS})
    if(JPEG_STATIC_LIB)
        set(JPEG_LIBRARIES ${JPEG_STATIC_LIB})
        message(STATUS "✓ ARM 静态链接 libjpeg: ${JPEG_STATIC_LIB}")
    else()
        message(WARNING "⚠ 未找到静态 libjpeg.a，回退到动态链接 (可能缺少 .so)")
    endif()
endif()

# ============================================================
# 源文件 (按模块组织)
# ============================================================

set(CAMERA_SOURCES
    src/camera/capture.cpp
    src/camera/processor.cpp
    include/camera/capture.h
    include/camera/processor.h
)

set(DISPLAY_SOURCES
    src/display/gui.cpp
    include/display/gui.h    # 需要 MOC 处理 (含 Q_OBJECT)
    src/display/gallery.cpp
    include/display/gallery.h  # 需要 MOC 处理 (含 Q_OBJECT)
    src/display/video_player.cpp
    include/display/video_player.h  # 需要 MOC 处理 (含 Q_OBJECT)
)

set(NETWORK_SOURCES
    src/network/mjpeg_server.cpp
    include/network/mjpeg_server.h
    src/network/control.cpp
    include/network/control.h
    src/network/rtsp_server.cpp
    include/network/rtsp_server.h
)

set(STORAGE_SOURCES
    src/storage/manager.cpp
    include/storage/manager.h
)

set(MAIN_SOURCES
    src/main.cpp
)

set(ALL_SOURCES
    ${CAMERA_SOURCES}
    ${DISPLAY_SOURCES}
    ${NETWORK_SOURCES}
    ${STORAGE_SOURCES}
    ${MAIN_SOURCES}
)

# 启用 Qt 元对象编译器自动处理
set(CMAKE_AUTOMOC ON)

# ============================================================
# 可执行文件
# ============================================================

add_executable(smartcam ${ALL_SOURCES})

# ARM 交叉编译时静态链接 C/C++ 标准库，消除 glibc/libstdc++ 版本不匹配问题
# （开发板可能跑 Debian oldstable，glibc 比 Docker 里的新很多）
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    target_link_options(smartcam PRIVATE -static-libstdc++ -static-libgcc)
    message(STATUS "✓ ARM 静态链接 libstdc++ / libgcc")
endif()

target_include_directories(smartcam PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(smartcam PRIVATE
    Qt5::Widgets
    pthread
)

# libjpeg-turbo (MJPEG 解码)
target_compile_definitions(smartcam PRIVATE HAS_LIBJPEG)
target_include_directories(smartcam PRIVATE ${JPEG_INCLUDE_DIRS})
target_link_libraries(smartcam PRIVATE ${JPEG_LIBRARIES})
message(STATUS "✓ libjpeg-turbo 已启用 (libs=${JPEG_LIBRARIES})")

# ============================================================
# 安装 (部署到开发板)
# ============================================================

install(TARGETS smartcam
    RUNTIME DESTINATION /usr/local/bin
)

install(FILES configs/smartcam.service
    DESTINATION /etc/systemd/system
    OPTIONAL
)

install(FILES configs/smartcam.conf
    DESTINATION /etc/smartcam
    OPTIONAL
)

# ============================================================
# 测试 (后续添加)
# ============================================================

# add_subdirectory(tests)

# ============================================================
# 打印构建摘要
# ============================================================

message(STATUS "")
message(STATUS "SmartCam 构建配置:")
message(STATUS "  Qt 版本:     ${Qt5_VERSION}")
message(STATUS "  C++ 标准:    C++${CMAKE_CXX_STANDARD}")
message(STATUS "  源文件数:    ${CMAKE_PROJECT_NAME}")
message(STATUS "")
message(STATUS "使用方法:")
message(STATUS "  cd build && cmake .. && make -j\$(nproc)")
message(STATUS "  ./smartcam                                    # Mock 模式 (PC 测试)")
message(STATUS "  ./smartcam --device /dev/video0                # 真实相机 (需 V4L2)")
message(STATUS "  ./smartcam --device /dev/video0 --fmt yuyv     # YUYV 模式")
message(STATUS "  # 开发板无 X server，linuxfb 后端 + 必须设置触摸输入环境变量:")
message(STATUS "  export QT_QPA_FB_HIDECURSOR=1")
message(STATUS "  export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=/dev/input/event1:rotate=0")
message(STATUS "  ./smartcam --device /dev/video0 --fmt yuyv -platform linuxfb")
message(STATUS "")
