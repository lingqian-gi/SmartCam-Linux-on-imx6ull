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

# --- ARM 交叉编译 Qt5 路径（可选，仅宿主机交叉编译时需要）---
#   不设置 → cmake 自动查找（multiarch / 系统路径）
#   设置   → 指定野火 BSP 等自定义路径
#   例: export ARM_QT_DIR="/opt/arm-linux-gnueabihf/qt5"
ARM_QT_DIR="${ARM_QT_DIR:-}"

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

    local arm_build="${PROJECT_ROOT}/build/arm"
    local toolchain="${PROJECT_ROOT}/configs/toolchain.arm.cmake"

    mkdir -p "${arm_build}"
    cd "${arm_build}"

    # 收集 Qt5 cmake 参数
    #   - 已设置 ARM_QT_DIR 环境变量 → 使用自定义路径（野火 BSP 等）
    #   - 未设置 → cmake 自动查找（multiarch 安装 / 系统默认）
    local qt5_args=()
    if [ -n "${ARM_QT_DIR:-}" ] && [ -d "${ARM_QT_DIR}" ]; then
        qt5_args+=("-DQt5_DIR=${ARM_QT_DIR}/lib/cmake/Qt5")
        echo "  使用 ARM Qt5: ${ARM_QT_DIR}"
    fi

    cmake "${PROJECT_ROOT}" \
        -DCMAKE_TOOLCHAIN_FILE="${toolchain}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="/usr/local" \
        "${qt5_args[@]}"
    make -j"$(nproc)"
    echo "✓ 完成: ${arm_build}/smartcam"
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
