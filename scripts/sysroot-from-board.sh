#!/bin/bash
# ============================================================
# 在开发板上执行：打包 sysroot 并推到 GitHub
#
# 用法（在开发板项目根目录下）:
#   chmod +x scripts/sysroot-from-board.sh
#   ./scripts/sysroot-from-board.sh
#
# 然后在云端环境:
#   git pull
#   ./scripts/sysroot-setup.sh
# ============================================================
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

TARBALL="npi-sysroot.tar.gz"
MAX_SIZE_MB=90  # 单文件超过这个大小就分包（GitHub 限制 100MB）

echo "==> [1/3] 打包 sysroot ..."

# 收集存在的目录
DIRS=""
for d in /lib /usr/lib /usr/include /usr/local/include /usr/local/lib /opt; do
    [ -d "$d" ] && DIRS="$DIRS $d"
done
echo "    包含: $DIRS"

tar czf "$TARBALL" $DIRS 2>/dev/null
SIZE_MB=$(( $(stat -c%s "$TARBALL") / 1024 / 1024 ))
echo "    大小: ${SIZE_MB} MB"

# =========================================
echo "==> [2/3] 提交到 Git ..."

# 先确保 .gitattributes 存在（声明 LFS 或分包策略）
if [ "$SIZE_MB" -gt "$MAX_SIZE_MB" ]; then
    echo "    文件较大，分包处理（避免 GitHub 单文件限制）..."
    # 删除旧分包
    rm -f npi-sysroot.part-*
    split -b ${MAX_SIZE_MB}M "$TARBALL" npi-sysroot.part-
    PART_COUNT=$(ls npi-sysroot.part-* | wc -l)
    echo "    分成 ${PART_COUNT} 个包"
    git add npi-sysroot.part-* .gitignore
    # 不提交整包（太大），改为 .gitignore 忽略
    echo "npi-sysroot.tar.gz" >> .gitignore
    git add .gitignore
    COMMIT_MSG="add board sysroot (${PART_COUNT} parts, ${SIZE_MB}MB total)"
else
    echo "    直接提交压缩包"
    git add "$TARBALL" .gitignore
    COMMIT_MSG="add board sysroot (${SIZE_MB} MB)"
fi

git commit -m "$COMMIT_MSG" 2>/dev/null || echo "    (没有新变更)"

# =========================================
echo "==> [3/3] 推送到 GitHub ..."
git push

echo ""
echo "✓ 完成! 在云端环境执行:"
echo "    git pull"
echo "    ./scripts/sysroot-setup.sh"
