#!/bin/bash
# ============================================================
# 从开发板导出 sysroot 到 x86 宿主机
#
# 用法（在 x86 上执行，需要能 ssh 到开发板）:
#   ./scripts/setup-sysroot.sh debian@192.168.1.100
#
# 产物:
#   npi-sysroot/   (开发板根文件系统副本，交叉编译时用)
# ============================================================
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYSROOT_DIR="${PROJECT_ROOT}/npi-sysroot"
BOARD_USER_HOST="${1:-}"

if [ -z "$BOARD_USER_HOST" ]; then
    echo "用法: $0 <user@board_ip>"
    echo "示例: $0 debian@192.168.1.100"
    exit 1
fi

echo "============================================"
echo "  SmartCam sysroot 导出工具"
echo "============================================"
echo "  目标开发板: $BOARD_USER_HOST"
echo "  sysroot 将导出到: $SYSROOT_DIR"
echo ""

# =========================================
# Step 1: 在开发板上打包 sysroot
# =========================================
echo "[1/3] 在开发板上打包 sysroot（可能需要几分钟）..."

ssh "$BOARD_USER_HOST" "bash -s" << 'SSH_EOF'
set -euo pipefail
TARBALL=/tmp/npi-sysroot.tar.gz
echo "  正在打包 /lib /usr/lib /usr/include ..."

# 只打包存在的目录，避免 tar 报错
DIRS=""
for d in /lib /usr/lib /usr/include /usr/local/include /usr/local/lib /opt; do
    if [ -d "$d" ]; then
        DIRS="$DIRS $d"
    fi
done
echo "  目录: $DIRS"

tar czf "$TARBALL" $DIRS 2>/dev/null
echo "  打包完成: $(du -sh $TARBALL 2>/dev/null | cut -f1)"
SSH_EOF

# =========================================
# Step 2: 拷贝到 x86 宿主机
# =========================================
echo "[2/3] 拷贝到本地..."
rm -rf "$SYSROOT_DIR"
mkdir -p "$SYSROOT_DIR"

scp "${BOARD_USER_HOST}:/tmp/npi-sysroot.tar.gz" /tmp/npi-sysroot.tar.gz
echo "  正在解压..."
cd "$SYSROOT_DIR"
tar xzf /tmp/npi-sysroot.tar.gz
rm -f /tmp/npi-sysroot.tar.gz

# 清理开发板上的临时文件
ssh "$BOARD_USER_HOST" rm -f /tmp/npi-sysroot.tar.gz

# =========================================
# Step 3: 修复符号链接（相对路径 → 绝对路径）
# =========================================
echo "[3/3] 修复库符号链接..."
find "$SYSROOT_DIR" -type l 2>/dev/null | while read -r link; do
    target=$(readlink "$link")
    # 如果是相对路径的符号链接，改成绝对路径
    if [[ "$target" != /* ]]; then
        link_dir=$(dirname "$link")
        abs_target=$(cd "$link_dir" 2>/dev/null && realpath -m "$target" 2>/dev/null || echo "")
        if [ -n "$abs_target" ] && [ -e "$abs_target" ]; then
            ln -sf "$abs_target" "$link"
        fi
    fi
done

echo ""
echo "============================================"
echo "  ✓ sysroot 导出完成!"
echo "    路径: $SYSROOT_DIR"
echo "    大小: $(du -sh "$SYSROOT_DIR" 2>/dev/null | cut -f1)"
echo ""
echo "  下一步: 用 Docker 交叉编译"
echo "    docker build -f Dockerfile.arm-sysroot -t smartcam-cross-sysroot ."
echo "    docker run --rm -v \$(pwd):/workspace smartcam-cross-sysroot"
echo "============================================"
