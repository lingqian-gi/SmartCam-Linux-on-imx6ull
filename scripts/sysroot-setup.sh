#!/bin/bash
# ============================================================
# 在云端环境中执行：拉取 sysroot 并 Docker 交叉编译
#
# 用法:
#   git pull                     # 先拉取最新代码
#   ./scripts/sysroot-setup.sh   # 解压 sysroot + 编译
#
# 产物:
#   build/arm/smartcam
# ============================================================
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYSROOT_DIR="${PROJECT_ROOT}/npi-sysroot"
TARBALL="${PROJECT_ROOT}/npi-sysroot.tar.gz"

echo "============================================"
echo "  SmartCam — sysroot 交叉编译"
echo "============================================"

# =========================================
# Step 1: 组装 sysroot
# =========================================
if [ -d "$SYSROOT_DIR" ] && [ -f "$SYSROOT_DIR/lib/arm-linux-gnueabihf/libc.so.6" ]; then
    echo "[1/3] sysroot 已存在，跳过"
elif [ -f "$TARBALL" ]; then
    echo "[1/3] 解压完整压缩包 ..."
    rm -rf "$SYSROOT_DIR"
    mkdir -p "$SYSROOT_DIR"
    tar xzf "$TARBALL" -C "$SYSROOT_DIR"
elif ls npi-sysroot.part-* 1>/dev/null 2>&1; then
    echo "[1/3] 合并分包 ..."
    cat npi-sysroot.part-* > "$TARBALL"
    rm -rf "$SYSROOT_DIR"
    mkdir -p "$SYSROOT_DIR"
    tar xzf "$TARBALL" -C "$SYSROOT_DIR"
    echo "      完成 ($(du -sh "$SYSROOT_DIR" | cut -f1))"
else
    echo "错误: 找不到 npi-sysroot.tar.gz 或 npi-sysroot.part-*"
    echo "请先在开发板执行 scripts/sysroot-from-board.sh 并 git push"
    exit 1
fi

# =========================================
# Step 2: Docker 交叉编译
# =========================================
echo "[2/3] 构建 Docker 镜像 ..."
docker build -f "${PROJECT_ROOT}/Dockerfile.arm-sysroot" -t smartcam-cross-sysroot "$PROJECT_ROOT"

echo "[3/3] 交叉编译 ..."
docker run --rm -v "${PROJECT_ROOT}:/workspace" smartcam-cross-sysroot

echo ""
echo "============================================"
echo "  ✓ 编译完成!"
echo "    产物: build/arm/smartcam"
echo "  部署到开发板: scp build/arm/smartcam debian@<IP>:~/smartcam/"
echo "============================================"
