#!/bin/bash
# ============================================================
# SmartCam Linux — 依赖检查与安装脚本
#
# 用法:
#   ./scripts/check-deps.sh              # 仅检查，不安装
#   ./scripts/check-deps.sh --install    # 检查 + 自动安装缺失依赖
#   ./scripts/check-deps.sh --arm        # 检查 ARM 交叉编译依赖
#   ./scripts/check-deps.sh --all        # 检查全部依赖 (x86 + ARM)
#
# ============================================================
set -uo pipefail
# 注意: 不使用 set -e, 由统计计数器 PASS/FAIL 管理退出码

# ---- 颜色输出 ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'         # No Color
BOLD='\033[1m'

ok()   { echo -e "  ${GREEN}✓${NC} $1"; }
warn() { echo -e "  ${YELLOW}⚠${NC} $1"; }
fail() { echo -e "  ${RED}✗${NC} $1"; }
info() { echo -e "  ${CYAN}→${NC} $1"; }

# ---- 参数解析 ----
DO_INSTALL=false
CHECK_ARM=false
CHECK_ALL=false

for arg in "$@"; do
    case "$arg" in
        --install) DO_INSTALL=true ;;
        --arm)     CHECK_ARM=true  ;;
        --all)     CHECK_ALL=true; CHECK_ARM=true ;;
        --help|-h)
            echo "用法: $0 [--install] [--arm] [--all]"
            echo ""
            echo "  (无参数)     仅检查 x86 本地编译依赖"
            echo "  --install    检查并自动安装缺失依赖 (需 sudo)"
            echo "  --arm        检查 ARM 交叉编译依赖"
            echo "  --all        检查全部依赖 (x86 + ARM)"
            exit 0
            ;;
        *)
            echo -e "${RED}未知参数: $arg${NC}"
            exit 1
            ;;
    esac
done

# ---- 平台检测 ----
OS_RELEASE="$(cat /etc/os-release 2>/dev/null | grep '^ID=' | cut -d= -f2 | tr -d '"')"
OS_VERSION="$(cat /etc/os-release 2>/dev/null | grep '^VERSION_ID=' | cut -d= -f2 | tr -d '"')"

echo ""
echo -e "${BOLD}╔══════════════════════════════════════════╗${NC}"
echo -e "${BOLD}║  SmartCam Linux — 依赖检查              ║${NC}"
echo -e "${BOLD}╠══════════════════════════════════════════╣${NC}"
echo -e "${BOLD}║  系统: ${OS_RELEASE} ${OS_VERSION}${NC}"
echo -e "${BOLD}║  安装: $($DO_INSTALL && echo '是 (sudo apt)' || echo '否 (仅检查)')${NC}"
echo -e "${BOLD}╚══════════════════════════════════════════╝${NC}"
echo ""

# ---- 统计 ----
PASS=0
FAIL=0
MISSING_PACKAGES=()

# ============================================================
# 检查函数
# ============================================================

# type: 检查类型 (binary/header/library/pkg-config/cmake-script)
# name: 显示名称
# check: 用于 exists 的实际检查目标
# pkg:   对应的 apt 包名
check_dep() {
    local type="$1"
    local name="$2"
    local check="$3"
    local pkg="${4:-}"

    case "$type" in
        binary)
            if command -v "$check" &>/dev/null; then
                local ver="$($check --version 2>&1 | head -1 | cut -c1-60)"
                ok "$name — $ver"
                ((PASS++)) || true
                return 0
            fi
            ;;
        header)
            local cflags=""
            # Qt 头文件需要 pkg-config 提供的路径
            if [[ "$name" == Qt* ]]; then
                local module="${name// /}"
                cflags="$(pkg-config --cflags "${module}" 2>/dev/null || echo '')"
            fi
            if echo "#include <$check>" | g++ $cflags -fsyntax-only -x c++ - -o /dev/null 2>/dev/null; then
                ok "$name"
                ((PASS++)) || true
                return 0
            fi
            ;;
        pkg-config)
            if pkg-config --exists "$check" 2>/dev/null; then
                local ver="$(pkg-config --modversion "$check" 2>/dev/null)"
                ok "$name — $ver"
                ((PASS++)) || true
                return 0
            fi
            ;;
        cmake-script)
            if [ -f "$check" ]; then
                ok "$name"
                ((PASS++)) || true
                return 0
            fi
            ;;
        *)
            echo "unknown type: $type"
            return 1
            ;;
    esac

    # 未找到
    fail "$name"
    ((FAIL++)) || true
    if [ -n "$pkg" ]; then
        MISSING_PACKAGES+=("$pkg")
    fi
    return 1
}

# ============================================================
# X86 本地编译依赖
# ============================================================

check_x86() {
    echo -e "${BOLD}── x86 本地编译依赖 ──${NC}"
    echo ""

    # 基础构建工具
    check_dep binary   "CMake"               "cmake"     "cmake"
    check_dep binary   "GCC (C++)"           "g++"       "g++"
    check_dep binary   "GCC (C)"             "gcc"       "gcc"
    check_dep binary   "Make"                "make"      "make"
    check_dep binary   "pkg-config"          "pkg-config" "pkg-config"

    echo ""
    echo -e "${BOLD}── Qt5 图形框架 ──${NC}"
    echo ""

    check_dep pkg-config "Qt5 Core"          "Qt5Core"     "qtbase5-dev"
    check_dep pkg-config "Qt5 Widgets"       "Qt5Widgets"  "qtbase5-dev"
    check_dep pkg-config "Qt5 Gui"           "Qt5Gui"      "qtbase5-dev"

    # 综合编译测试: 能否用 Qt5 编译一段含 QWidget 的代码?
    local test_src="/tmp/smartcam-qt-test-$$.cpp"
    local test_bin="/tmp/smartcam-qt-test-$$"
    cat > "$test_src" << 'EOCPP'
#include <QApplication>
#include <QWidget>
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QWidget w; w.show();
    return 0;
}
EOCPP
    if g++ -fPIC "$test_src" -o "$test_bin" $(pkg-config --cflags --libs Qt5Widgets) 2>/dev/null; then
        ok "Qt5 编译测试 (QWidget + 链接)"
        ((PASS++)) || true
    else
        fail "Qt5 编译测试"
        MISSING_PACKAGES+=("qtbase5-dev")
        ((FAIL++)) || true
    fi
    rm -f "$test_src" "$test_bin"

    echo ""
    echo -e "${BOLD}── 可选依赖 (后续模块) ──${NC}"
    echo ""

    check_dep binary   "v4l2-ctl (摄像头调试)" "v4l2-ctl"   "v4l-utils"
    check_dep header   "libjpeg (JPEG编解码)"  "jpeglib.h"  "libjpeg-dev"
    check_dep binary   "git"                   "git"        "git"

    echo ""
}

# ============================================================
# ARM 交叉编译依赖
# ============================================================

check_arm() {
    echo -e "${BOLD}── ARM 交叉编译依赖 (iMX6ULL) ──${NC}"
    echo ""

    # 交叉编译器
    check_dep binary "arm-linux-gnueabihf-gcc"  "arm-linux-gnueabihf-gcc"  "gcc-arm-linux-gnueabihf"
    check_dep binary "arm-linux-gnueabihf-g++"  "arm-linux-gnueabihf-g++"  "g++-arm-linux-gnueabihf"

    # 交叉编译的 Qt (通常需要手动编译，这里只做路径检查)
    echo ""
    info "ARM Qt5 路径检查 (野火 BSP 通常位于 /opt/arm-linux-gnueabihf/qt5)"

    local arm_qt_dirs=(
        "/opt/arm-linux-gnueabihf/qt5"
        "/usr/arm-linux-gnueabihf/qt5"
        "${HOME}/arm-qt5"
    )
    local found_qt=false
    for dir in "${arm_qt_dirs[@]}"; do
        if [ -d "$dir/lib/cmake/Qt5" ]; then
            ok "ARM Qt5 已安装: $dir"
            found_qt=true
            ((PASS++)) || true
            break
        fi
    done
    if ! $found_qt; then
        warn "ARM Qt5 未找到. 需手动交叉编译 Qt5 到以下路径之一:"
        for dir in "${arm_qt_dirs[@]}"; do
            echo "       $dir"
        done
        warn "或修改 scripts/build.sh 中的 ARM_QT_DIR 变量"
        ((FAIL++)) || true
    fi

    echo ""
}

# ============================================================
# 安装缺失依赖
# ============================================================

install_deps() {
    if [ ${#MISSING_PACKAGES[@]} -eq 0 ]; then
        return
    fi

    # 去重
    local uniq_pkgs=($(printf '%s\n' "${MISSING_PACKAGES[@]}" | sort -u))

    echo ""
    echo -e "${YELLOW}═══════════════════════════════════════════${NC}"
    echo -e "${YELLOW}  将安装 ${#uniq_pkgs[@]} 个缺失的包:${NC}"
    for p in "${uniq_pkgs[@]}"; do
        echo -e "    ${CYAN}→${NC} $p"
    done
    echo -e "${YELLOW}═══════════════════════════════════════════${NC}"
    echo ""

    if [ "$(id -u)" -ne 0 ]; then
        echo -e "${RED}需要 sudo 权限来安装包${NC}"
        echo "正在执行: sudo apt-get install -y ${uniq_pkgs[*]}"
        echo ""
        sudo apt-get update -qq && sudo apt-get install -y "${uniq_pkgs[@]}"
    else
        apt-get update -qq && apt-get install -y "${uniq_pkgs[@]}"
    fi

    echo ""
    echo -e "${GREEN}✓ 安装完成${NC}"
    echo ""
    echo -e "${YELLOW}═══════════════════════════════════════════${NC}"
    echo -e "${YELLOW}  建议重新运行检查确认:${NC}"
    echo -e "${YELLOW}    ./scripts/check-deps.sh${NC}"
    echo -e "${YELLOW}═══════════════════════════════════════════${NC}"
}

# ============================================================
# 主流程
# ============================================================

check_x86

if $CHECK_ARM || $CHECK_ALL; then
    check_arm
fi

# ---- 汇总 ----
echo ""
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${BOLD}  检查结果:  ${GREEN}${PASS} 通过${NC}  ${RED}${FAIL} 缺失${NC}${NC}"
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo ""

if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}✓ 所有依赖已就绪，可以构建项目:${NC}"
    echo ""
    echo "    cd build && cmake .. && make -j\$(nproc)"
    echo "    ./smartcam"
    echo ""
    exit 0
fi

if $DO_INSTALL; then
    install_deps
    echo ""
    echo "安装后请重新运行检查:"
    echo "  ./scripts/check-deps.sh"
else
    echo -e "${YELLOW}提示: 运行以下命令安装缺失依赖:${NC}"
    echo ""
    echo "    ./scripts/check-deps.sh --install"
    echo ""
    exit 1
fi
