// ============================================================
// cursor_hook.cpp — 修复 Qt 5.11.3 linuxfb 光标初始化崩溃
//
// 背景：
//   Qt 5.11.3 的 linuxfb 平台插件在无平台主题（无 XDG/主题资源）时，
//   QPlatformCursorImage::set(Qt::CursorShape) 内部 createSystemCursor
//   返回 null，随后对 null 做 QImage 赋值 → Segfault（QImage::operator= this=0）。
//   这是系统 Qt 的 bug，环境变量 QT_QPA_FB_HIDECURSOR / 插件参数 hidecursor
//   均无法阻止（它们只控制"绘制光标"，不阻止内部光标图像加载）。
//
// 本库通过 LD_PRELOAD 拦截 QPlatformCursorImage::set(Qt::CursorShape)，
// 将其改为空操作：linuxfb 调用时直接返回，光标图像保持空，避免崩溃。
// 窗口渲染（framebuffer）不受影响。
//
// 编译（ARM 交叉）：
//   arm-linux-gnueabihf-g++ -shared -fPIC -O2 \
//       -o cursor_hook.so scripts/cursor_hook.cpp
//
// 使用（开发板上）：
//   LD_PRELOAD=/path/to/cursor_hook.so \
//   QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0 \
//   ./smartcam --device /dev/video0 --fmt mjpeg --http-port 8080
// ============================================================

#include <cstddef>

// 目标符号：QPlatformCursorImage::set(Qt::CursorShape)
// Itanium ABI mangled: _ZN20QPlatformCursorImage3setEN2Qt11CursorShapeE
//
// 注意：这里用 C 链接导出同名符号，配合 asm 指定名字覆盖 C++ 符号。
// 我们无需包含任何 Qt 头文件——签名只用到 Qt::CursorShape（底层 int）。
extern "C" {

// 拦截 QPlatformCursorImage::set(Qt::CursorShape)
// 原签名（Qt5 内部）：
//   void QPlatformCursorImage::set(Qt::CursorShape shape)
// 参数：r0 = this, r1 = shape(int)
void _ZN20QPlatformCursorImage3setEN2Qt11CursorShapeE(void*, int)
{
    // 空操作：不加载光标图像，避免 createSystemCursor 返回 null 后崩溃。
    return;
}

// 兜底：拦截 set(const QImage&, int, int) —— 某些路径也会调用
void _ZN20QPlatformCursorImage3setERK6QImageii(void*, const void*, int, int)
{
    return;
}

} // extern "C"
