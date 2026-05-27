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

# ── CMake 配置 + 编译 ──
rm -rf build/arm
mkdir -p build/arm
cd build/arm

cmake ../.. \
    -DCMAKE_TOOLCHAIN_FILE=/opt/toolchain/toolchain.cmake \
    -DCMAKE_SYSROOT=$SYSROOT \
    -DCMAKE_BUILD_TYPE=Release \
    2>&1

make -j"$(nproc)" 2>&1

echo "========================================"
file smartcam
echo "========================================"
echo "产物: build/arm/smartcam"
