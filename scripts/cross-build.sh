#!/bin/bash
set -e

SYSROOT=/workspace/npi-sysroot

if [ ! -d "$SYSROOT/lib" ]; then
    echo "错误: 找不到 sysroot! 请先运行 scripts/setup-sysroot.sh 导出开发板根文件系统" >&2
    echo "  ./scripts/setup-sysroot.sh debian@开发板IP" >&2
    exit 1
fi

echo "=== sysroot: $SYSROOT (glibc 2.28 / Qt 5.11.3) ==="

# ── 查找宿主 Qt5 工具 ──
# 直接使用 /usr/lib/qt5/bin/ 下的真实二进制，避开 qtchooser 包装器
HOST_MOC=/usr/lib/qt5/bin/moc
HOST_RCC=/usr/lib/qt5/bin/rcc
HOST_UIC=/usr/lib/qt5/bin/uic
HOST_QMAKE=/usr/lib/qt5/bin/qmake

for tool in moc rcc uic qmake; do
    bin_var="HOST_$(echo "$tool" | tr 'a-z' 'A-Z')"
    bin_path="${!bin_var}"
    if [ ! -x "$bin_path" ]; then
        echo "错误: 找不到宿主 $tool ($bin_path)" >&2
        exit 1
    fi
    echo "  $tool = $bin_path"
done

# ── 修复 sysroot 中的 cmake 配置 (工具路径 → 宿主) ──
# sysroot 的 Qt5CoreConfigExtras.cmake 中 /usr/bin/moc 指向 qtchooser（不可用）
# 需要改为 /usr/lib/qt5/bin/moc（真实的 x86_64 二进制）
# 同时 patch Qt5WidgetsConfigExtras.cmake 中的 uic 路径
echo "=== 修复 sysroot 中的 cmake 配置 (工具路径 → 宿主) ==="

CORE_EXTRAS="$SYSROOT/usr/lib/arm-linux-gnueabihf/cmake/Qt5Core/Qt5CoreConfigExtras.cmake"
WIDGETS_EXTRAS="$SYSROOT/usr/lib/arm-linux-gnueabihf/cmake/Qt5Widgets/Qt5WidgetsConfigExtras.cmake"

# 先恢复原始文件（如果之前 patch 过）
for f in "$CORE_EXTRAS" "$WIDGETS_EXTRAS"; do
    if [ -f "$f.bak" ]; then
        cp "$f.bak" "$f"
    fi
done
cp "$CORE_EXTRAS" "$CORE_EXTRAS.bak"
[ -f "$WIDGETS_EXTRAS" ] && cp "$WIDGETS_EXTRAS" "$WIDGETS_EXTRAS.bak"

# Patch Qt5CoreConfigExtras.cmake: /usr/bin/{moc,rcc,qmake} → /usr/lib/qt5/bin/{moc,rcc,qmake}
sed -i 's|/usr/bin/qmake|/usr/lib/qt5/bin/qmake|g' "$CORE_EXTRAS"
sed -i 's|/usr/bin/moc|/usr/lib/qt5/bin/moc|g' "$CORE_EXTRAS"
sed -i 's|/usr/bin/rcc|/usr/lib/qt5/bin/rcc|g' "$CORE_EXTRAS"

# Patch Qt5WidgetsConfigExtras.cmake: ${prefix}/lib/qt5/bin/uic → /usr/lib/qt5/bin/uic
if [ -f "$WIDGETS_EXTRAS" ]; then
    sed -i 's|${_qt5Widgets_install_prefix}/lib/qt5/bin/uic|/usr/lib/qt5/bin/uic|g' "$WIDGETS_EXTRAS"
fi

echo "  已 patch Qt5CoreConfigExtras.cmake + Qt5WidgetsConfigExtras.cmake"

# ── 替换 sysroot 中所有 Qt5 工具为宿主 (x86_64) 版本 ──
# sysroot 中的 moc/rcc/uic/qmake 均为 ARM 二进制（从开发板导出），
# 在 x86_64 Docker 宿主上执行需要 qemu-arm，且缺少 ARM 动态链接器。
#
# 关键：CMake 通过 _qt5Core_install_prefix 推导工具路径：
#   _qt5Core_install_prefix = CMAKE_CURRENT_LIST_DIR/../../../../ = /workspace/npi-sysroot/usr/
#   moc 路径 = ${prefix}/lib/qt5/bin/moc = /workspace/npi-sysroot/usr/lib/qt5/bin/moc  ← ARM!
#
# 所以必须替换以下两处的 ARM 二进制：
#   1) sysroot/usr/lib/qt5/bin/          ← CMake 实际使用的路径
#   2) sysroot/usr/lib/arm-linux-gnueabihf/qt5/bin/  ← 软链接指向上面
echo "=== 替换 sysroot Qt5 工具 → 宿主 x86_64 ==="

declare -A TOOL_MAP=(
    [qmake]="$HOST_QMAKE"
    [moc]="$HOST_MOC"
    [rcc]="$HOST_RCC"
    [uic]="$HOST_UIC"
)

for tool in "${!TOOL_MAP[@]}"; do
    target="${TOOL_MAP[$tool]}"

    for dir in \
        "$SYSROOT/usr/lib/qt5/bin" \
        "$SYSROOT/usr/lib/arm-linux-gnueabihf/qt5/bin"; do
        link_path="$dir/$tool"
        if [ -e "$link_path" ] || [ -L "$link_path" ]; then
            rm -f "$link_path"
            ln -s "$target" "$link_path"
            echo "  $link_path → $target"
        fi
    done
done

# ── CMake 配置 + 编译 ──
# 通过 -DQt5*_*_EXECUTABLE 强制使用宿主 x86_64 的 Qt 工具，
# 覆盖 sysroot 中 ARM 版本的自动检测（避免 qemu-arm 报错）
rm -rf build/arm
mkdir -p build/arm
cd build/arm

cmake ../.. \
    -DCMAKE_TOOLCHAIN_FILE=/opt/toolchain/toolchain.cmake \
    -DCMAKE_SYSROOT=$SYSROOT \
    -DCMAKE_BUILD_TYPE=Release \
    -DQt5Core_MOC_EXECUTABLE=$HOST_MOC \
    -DQt5Core_RCC_EXECUTABLE=$HOST_RCC \
    -DQt5Widgets_UIC_EXECUTABLE=$HOST_UIC \
    2>&1

make -j"$(nproc)" 2>&1

echo "========================================"
file smartcam
echo "========================================"
echo "产物: build/arm/smartcam"
