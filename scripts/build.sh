#!/bin/bash
# ============================================================
# SmartCam Linux — 构建脚本
#
# 用法:
#   ./scripts/build.sh              # PC 本地编译 (x86)
#   ./scripts/build.sh arm          # ARM 交叉编译 (iMX6ULL)
#
# ============================================================
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

# --- ARM 交叉编译工具链 ---
ARM_TOOLCHAIN="/opt/arm-linux-gnueabihf"   # 根据你的环境修改
ARM_CROSS="${ARM_TOOLCHAIN}/bin/arm-linux-gnueabihf-"
ARM_QT_DIR="${ARM_TOOLCHAIN}/qt5"          # 野火 BSP 的 Qt 路径

build_pc() {
    echo "==> 构建目标: x86 PC (本地调试)"
    mkdir -p "${BUILD_DIR}/pc"
    cd "${BUILD_DIR}/pc"
    cmake "${PROJECT_ROOT}" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_INSTALL_PREFIX="${BUILD_DIR}/install/pc"
    make -j"$(nproc)"
    echo "✓ 完成: ${BUILD_DIR}/pc/smartcam"
}

build_arm() {
    echo "==> 构建目标: ARM (iMX6ULL Cortex-A7)"
    mkdir -p "${BUILD_DIR}/arm"
    cd "${BUILD_DIR}/arm"

    cat > "${BUILD_DIR}/arm/toolchain.cmake" << 'EOC'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER   arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOC

    cmake "${PROJECT_ROOT}" \
        -DCMAKE_TOOLCHAIN_FILE="${BUILD_DIR}/arm/toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="/usr/local" \
        -DQt5_DIR="${ARM_QT_DIR}/lib/cmake/Qt5"
    make -j"$(nproc)"
    echo "✓ 完成: ${BUILD_DIR}/arm/smartcam"
}

# --- 主入口 ---
case "${1:-pc}" in
    pc)
        build_pc
        ;;
    arm)
        build_arm
        ;;
    *)
        echo "用法: $0 [pc|arm]"
        exit 1
        ;;
esac
