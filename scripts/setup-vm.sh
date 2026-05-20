#!/bin/bash
# ============================================================
# SmartCam Linux — 野火 Ubuntu VM 一键环境搭建
#
# 用法 (在 VM 终端执行):
#   chmod +x scripts/setup-vm.sh
#   ./scripts/setup-vm.sh              # 全套: 依赖 → 编译 → 启动
#   ./scripts/setup-vm.sh deps         # 仅安装依赖
#   ./scripts/setup-vm.sh build        # 仅编译 (x86 + ARM)
#   ./scripts/setup-vm.sh run          # 仅启动 x86 Mock GUI
#   ./scripts/setup-vm.sh deploy       # 编译 ARM 版并 SCP 到开发板
#
# 前提:
#   - 野火 Ubuntu VM 已启动, 有桌面 GUI
#   - 项目目录已放在 VM 中 (如 ~/SmartCam-Linux-on-imx6ull/)
#   - (可选) 开发板通过网线/USB 连接到 VM, IP 已知
# ============================================================
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

banner() {
    echo ""
    echo -e "${BOLD}${GREEN}═══════════════════════════════════════════${NC}"
    echo -e "${BOLD}${GREEN}  $1${NC}"
    echo -e "${BOLD}${GREEN}═══════════════════════════════════════════${NC}"
    echo ""
}

# ============================================================
# 1. 依赖安装
# ============================================================
install_deps() {
    banner "Step 1/4: 安装系统依赖"

    # ---- x86 本地编译依赖 ----
    echo -e "${CYAN}[x86] 构建工具 + Qt5 GUI...${NC}"
    sudo apt-get update -qq
    sudo apt-get install -y -qq \
        cmake g++ gcc make pkg-config git \
        qtbase5-dev \
        libjpeg-dev \
        v4l-utils

    echo -e "${GREEN}✓ x86 依赖已就绪${NC}"

    # ---- ARM 交叉编译器 ----
    echo ""
    echo -e "${CYAN}[ARM] 查找交叉编译器...${NC}"

    if command -v arm-linux-gnueabihf-gcc &>/dev/null; then
        echo -e "${GREEN}✓ arm-linux-gnueabihf-gcc 已存在: $(arm-linux-gnueabihf-gcc --version | head -1)${NC}"
    else
        echo -e "${YELLOW}⚠ 交叉编译器未找到, 尝试安装...${NC}"
        sudo apt-get install -y -qq gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf || {
            echo -e "${YELLOW}⚠ apt 安装失败. 野火 VM 通常预装在 /opt 下, 请检查:${NC}"
            echo "    ls /opt/arm-linux-gnueabihf*/bin/"
            echo "    如已安装, 将路径加入 PATH: export PATH=/opt/xxx/bin:\$PATH"
        }
    fi

    # ---- ARM Qt5 ----
    echo ""
    echo -e "${CYAN}[ARM] 查找 Qt5 交叉编译库...${NC}"

    # 野火 VM 常见路径
    local arm_qt_paths=(
        "/opt/qt5-arm"
        "/opt/arm-linux-gnueabihf/qt5"
        "/opt/arm-qt"
        "/usr/arm-linux-gnueabihf/qt5"
    )
    local found_qt=""
    for p in "${arm_qt_paths[@]}"; do
        if [ -d "$p/lib/cmake/Qt5" ]; then
            found_qt="$p"
            echo -e "${GREEN}✓ ARM Qt5 找到: $p${NC}"
            break
        fi
    done

    if [ -z "$found_qt" ]; then
        echo -e "${YELLOW}⚠ ARM Qt5 未在上述路径找到"
        echo "   如果野火 BSP 已将 Qt5 安装到其他位置，"
        echo "   请在编译 ARM 版时手动指定:"
        echo "     cmake -DQt5_DIR=/your/path/lib/cmake/Qt5 ..${NC}"
    fi

    echo ""
    echo -e "${GREEN}✓ 依赖安装完成${NC}"
}

# ============================================================
# 2. x86 编译 (本地测试 GUI)
# ============================================================
build_x86() {
    banner "Step 2/4: x86 本地编译"

    local build_dir="$PROJECT_ROOT/build/x86"
    mkdir -p "$build_dir"

    cmake -S "$PROJECT_ROOT" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Debug
    make -C "$build_dir" -j"$(nproc)"

    echo ""
    echo -e "${GREEN}✓ x86 编译完成: $build_dir/smartcam${NC}"
    echo ""
    echo -e "${CYAN}启动命令:${NC}"
    echo "  cd $PROJECT_ROOT"
    echo "  ./build/x86/smartcam"
    echo "  # 或带参数: ./build/x86/smartcam --device /dev/video0"
}

# ============================================================
# 3. ARM 交叉编译
# ============================================================
build_arm() {
    banner "Step 3/4: ARM 交叉编译 (iMX6ULL)"

    if ! command -v arm-linux-gnueabihf-gcc &>/dev/null; then
        echo -e "${YELLOW}⚠ 未找到 arm-linux-gnueabihf-gcc, 跳过 ARM 编译${NC}"
        echo "  请先安装交叉编译器: sudo apt install gcc-arm-linux-gnueabihf"
        return 0
    fi

    local build_dir="$PROJECT_ROOT/build/arm"
    local toolchain_file="$build_dir/toolchain.cmake"

    mkdir -p "$build_dir"

    # ---- 生成 toolchain 文件 ----
    cat > "$toolchain_file" << 'EOC'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER   arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOC

    # ---- 检测 ARM Qt5 路径 ----
    local qt5_dir=""
    for p in "/opt/qt5-arm" "/opt/arm-linux-gnueabihf/qt5" "/opt/arm-qt" "/usr/arm-linux-gnueabihf/qt5"; do
        if [ -d "$p/lib/cmake/Qt5" ]; then
            qt5_dir="$p"
            break
        fi
    done

    local qt5_flag=""
    if [ -n "$qt5_dir" ]; then
        qt5_flag="-DQt5_DIR=$qt5_dir/lib/cmake/Qt5"
        echo -e "${CYAN}使用 ARM Qt5: $qt5_dir${NC}"
    else
        echo -e "${YELLOW}⚠ 未找到 ARM Qt5, 编译可能失败${NC}"
        echo "  如需指定路径, 请修改 ARM_QT5_DIR 后重新运行"
    fi

    cmake -S "$PROJECT_ROOT" -B "$build_dir" \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain_file" \
        -DCMAKE_BUILD_TYPE=Release \
        $qt5_flag

    make -C "$build_dir" -j"$(nproc)"

    echo ""
    echo -e "${GREEN}✓ ARM 编译完成: $build_dir/smartcam${NC}"
    file "$build_dir/smartcam"
}

# ============================================================
# 4. 部署到开发板
# ============================================================
deploy_to_board() {
    banner "Step 4/4: 部署到开发板"

    local arm_bin="$PROJECT_ROOT/build/arm/smartcam"

    if [ ! -f "$arm_bin" ]; then
        echo -e "${YELLOW}⚠ ARM 二进制不存在, 先编译:${NC}"
        build_arm
    fi

    if [ ! -f "$arm_bin" ]; then
        echo -e "${RED}✗ ARM 编译产物仍不存在, 放弃部署${NC}"
        return 1
    fi

    # 询问开发板 IP
    local board_ip="${1:-}"
    if [ -z "$board_ip" ]; then
        echo -n "请输入开发板 IP (如 192.168.1.100): "
        read -r board_ip
    fi

    if [ -z "$board_ip" ]; then
        echo -e "${YELLOW}未输入 IP, 跳过部署${NC}"
        return 0
    fi

    echo -e "${CYAN}部署 smartcam → root@$board_ip:/usr/local/bin/${NC}"
    scp "$arm_bin" "root@$board_ip:/usr/local/bin/smartcam"

    echo -e "${CYAN}部署 configs → root@$board_ip:/etc/smartcam/${NC}"
    ssh "root@$board_ip" "mkdir -p /etc/smartcam"
    scp "$PROJECT_ROOT/configs/smartcam.conf" "root@$board_ip:/etc/smartcam/"

    echo ""
    echo -e "${GREEN}✓ 部署完成!${NC}"
    echo ""
    echo -e "${CYAN}在开发板上执行:${NC}"
    echo "  export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0"
    echo "  smartcam --device /dev/video0 --http-port 8080"

    # 可选: 部署 systemd 服务
    echo ""
    echo -n "是否部署 systemd 服务 (开机自启)? [y/N] "
    read -r yn
    if [[ "$yn" =~ ^[Yy]$ ]]; then
        scp "$PROJECT_ROOT/configs/smartcam.service" "root@$board_ip:/etc/systemd/system/"
        ssh "root@$board_ip" "systemctl daemon-reload && systemctl enable smartcam"
        echo -e "${GREEN}✓ systemd 服务已部署${NC}"
    fi
}

# ============================================================
# 启动 x86 GUI
# ============================================================
run_gui() {
    local bin="$PROJECT_ROOT/build/x86/smartcam"

    if [ ! -f "$bin" ]; then
        echo -e "${YELLOW}⚠ x86 二进制不存在, 先编译:${NC}"
        build_x86
    fi

    if [ ! -f "$bin" ]; then
        echo -e "${RED}✗ 编译失败, 无法启动${NC}"
        return 1
    fi

    banner "启动 SmartCam GUI (Mock 模式)"
    echo -e "${CYAN}彩色测试条 + 流动动画将在窗口中显示${NC}"
    echo -e "${YELLOW}提示: 关闭窗口即可退出程序${NC}"
    echo ""

    "$bin"
}

# ============================================================
# 主入口
# ============================================================
main() {
    echo ""
    echo -e "${BOLD}╔══════════════════════════════════════════╗${NC}"
    echo -e "${BOLD}║  SmartCam Linux — 野火 Ubuntu VM 安装   ║${NC}"
    echo -e "${BOLD}║  目标: x86 GUI 测试 + ARM iMX6ULL 部署  ║${NC}"
    echo -e "${BOLD}╚══════════════════════════════════════════╝${NC}"

    case "${1:-all}" in
        deps)
            install_deps
            ;;
        build)
            install_deps
            build_x86
            build_arm
            ;;
        run)
            build_x86
            run_gui
            ;;
        deploy)
            build_arm
            deploy_to_board "${2:-}"
            ;;
        all|*)
            install_deps
            build_x86
            build_arm
            echo ""
            echo -e "${BOLD}${GREEN}═══════════════════════════════════════════${NC}"
            echo -e "${BOLD}${GREEN}  全部完成!${NC}"
            echo -e "${BOLD}${GREEN}═══════════════════════════════════════════${NC}"
            echo ""
            echo -e "${CYAN}启动 x86 GUI 测试:${NC}"
            echo "  ./scripts/setup-vm.sh run"
            echo ""
            echo -e "${CYAN}部署到开发板 (替换 <IP>):${NC}"
            echo "  ./scripts/setup-vm.sh deploy <开发板IP>"
            echo ""
            echo -e "${CYAN}检查依赖:${NC}"
            echo "  ./scripts/check-deps.sh"
            ;;
    esac
}

main "$@"
