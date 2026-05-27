/**
 * @file    processor_neon.cpp
 * @brief   YUYV→RGB24 的 ARM NEON SIMD 加速实现
 *
 * 目标平台: Cortex-A7 (armv7-a), i.MX6ULL
 * 编译条件: __ARM_NEON 已定义 (由 -mfpu=neon 自动开启)
 *
 * 加速原理:
 *   - YUYV 4:2:2 每 4 字节 = [Y0,U,Y1,V] = 2 像素，U/V 共用
 *   - NEON 128-bit 寄存器一次加载 16 字节 = 4 个宏像素 = 8 真实像素
 *   - BT.601 系数使用定点 Q8 运算 (乘加 → 右移 8 位)
 *   - vqmovun 内置饱和转换，无需手动 clamp
 *
 * 轮次处理:
 *   主循环: 一次处理 8 宏像素 = 16 像素 = 32B YUYV 输入 → 48B RGB 输出
 *   尾部:   < 16 像素的残量退化为标量 C++ 循环
 */

#include <arm_neon.h>
#include <cstdint>
#include <cstddef>

/**
 * @brief NEON 加速 YUYV → RGB24 (完整帧)
 *
 * @param yuyv   输入 YUYV 数据 (w*h*2 字节, 宏像素布局)
 * @param rgb    输出 RGB24 数据 (w*h*3 字节, 调用者分配)
 * @param width  图像宽度 (必须是偶数，YUYV 要求)
 * @param height 图像高度
 */
void yuyv_to_rgb24_neon(const uint8_t* yuyv, uint8_t* rgb,
                         int width, int height) {
    const int totalPixels = width * height;
    const uint8_t* src = yuyv;
    uint8_t*       dst = rgb;

    // ── BT.601 定点系数 (Q8, 即值 × 256 取整) ──
    // R = Y + 1.402   × (V-128) → 359/256
    // G = Y - 0.34414 × (U-128) - 0.71414 × (V-128) → 88/256, 183/256
    // B = Y + 1.772   × (U-128) → 454/256
    const int16x8_t vRcoeff   = vdupq_n_s16(359);
    const int16x8_t vGcoeff_U = vdupq_n_s16(88);
    const int16x8_t vGcoeff_V = vdupq_n_s16(183);
    const int16x8_t vBcoeff   = vdupq_n_s16(454);

    // 常量: −128 用于去 U/V 偏移; +16 = (1<<4) 用于 (x+128)>>8 的四舍五入
    const int16x8_t v128 = vdupq_n_s16(128);
    const int16x8_t vHalf = vdupq_n_s16(128);  // (1 << 7) 用于正数四舍五入

    // ── 主循环: 每次处理 8 宏像素 = 16 像素 ──
    //   输入 32 字节 → 输出 48 字节 RGB
    int i = 0;
    for (; i + 15 < totalPixels; i += 16, src += 32) {
        // Step 1: 加载并去交织 YUYV
        // vld2 将偶数字节 (Y) 和奇数字节 (U/V) 分到两个寄存器
        uint8x16x2_t yu = vld2q_u8(src);
        uint8x16_t  y16   = yu.val[0];  // [Y0,...,Y15] 16 个 Y
        uint8x16_t  uv16  = yu.val[1];  // [U0,V0,...,U7,V7] 8 对 UV

        // Step 2: 从 uv16 中分离 U 和 V，扩展到 16-bit 并去偏移
        // uv16 = [U0,V0,U1,V1,U2,V2,U3,V3, U4,V4,U5,V5,U6,V6,U7,V7]
        uint8x8_t uv_lo = vget_low_u8(uv16);
        uint8x8_t uv_hi = vget_high_u8(uv16);

        // vuzp (unzip) 将 [U0,V0,U2,V2,...] → U=[U0,U2,...], V=[V0,V2,...]
        uint8x8x2_t uv_lo_sep = vuzp_u8(uv_lo, uv_lo);
        uint8x8x2_t uv_hi_sep = vuzp_u8(uv_hi, uv_hi);

        // 合并高低半 → 8 个 U 和 8 个 V
        uint8x8_t u8 = vuzp_u8(uv_lo_sep.val[0], uv_hi_sep.val[0]).val[0];
        uint8x8_t v8 = vuzp_u8(uv_lo_sep.val[1], uv_hi_sep.val[1]).val[0];
        // u8 = [U0,U2,U4,U6, U8,U10,U12,U14]
        // v8 = [V0,V2,V4,V6, V8,V10,V12,V14]

        // 扩展到 16-bit → 减去 128 (去偏移)
        int16x8_t u = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(u8)), v128);
        int16x8_t v = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(v8)), v128);

        // Step 3: Y 扩展到 16-bit (低 8 和高 8)
        int16x8_t y_lo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(y16)));
        int16x8_t y_hi = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(y16)));

        // Step 4: BT.601 矩阵 — R = Y + (V*359 + 128) >> 8
        int16x8_t r_lo = vaddq_s16(y_lo,
            vshrq_n_s16(vaddq_s16(vmulq_s16(v, vRcoeff), vHalf), 8));
        int16x8_t g_lo = vsubq_s16(y_lo,
            vshrq_n_s16(vaddq_s16(
                vaddq_s16(vmulq_s16(u, vGcoeff_U),
                          vmulq_s16(v, vGcoeff_V)), vHalf), 8));
        int16x8_t b_lo = vaddq_s16(y_lo,
            vshrq_n_s16(vaddq_s16(vmulq_s16(u, vBcoeff), vHalf), 8));

        int16x8_t r_hi = vaddq_s16(y_hi,
            vshrq_n_s16(vaddq_s16(vmulq_s16(v, vRcoeff), vHalf), 8));
        int16x8_t g_hi = vsubq_s16(y_hi,
            vshrq_n_s16(vaddq_s16(
                vaddq_s16(vmulq_s16(u, vGcoeff_U),
                          vmulq_s16(v, vGcoeff_V)), vHalf), 8));
        int16x8_t b_hi = vaddq_s16(y_hi,
            vshrq_n_s16(vaddq_s16(vmulq_s16(u, vBcoeff), vHalf), 8));

        // Step 5: 饱和转换 int16 → uint8 (自动 clip 到 [0,255])
        uint8x8_t r8_lo = vqmovun_s16(r_lo);
        uint8x8_t g8_lo = vqmovun_s16(g_lo);
        uint8x8_t b8_lo = vqmovun_s16(b_lo);

        uint8x8_t r8_hi = vqmovun_s16(r_hi);
        uint8x8_t g8_hi = vqmovun_s16(g_hi);
        uint8x8_t b8_hi = vqmovun_s16(b_hi);

        // Step 6: 交织 R,G,B → interleaved RGB24 并写入
        // vst3 将 3 个 8-wide 寄存器按 R0,G0,B0,R1,G1,B1,... 写入内存
        uint8x8x3_t rgb_lo;
        rgb_lo.val[0] = r8_lo;
        rgb_lo.val[1] = g8_lo;
        rgb_lo.val[2] = b8_lo;
        vst3_u8(dst, rgb_lo);
        dst += 24;  // 8 像素 × 3 = 24 字节

        uint8x8x3_t rgb_hi;
        rgb_hi.val[0] = r8_hi;
        rgb_hi.val[1] = g8_hi;
        rgb_hi.val[2] = b8_hi;
        vst3_u8(dst, rgb_hi);
        dst += 24;
    }

    // ── 尾部: 剩余 < 16 像素，退化为标量循环 ──
    for (; i + 1 < totalPixels; i += 2) {
        int si = static_cast<int>(src - yuyv) + (i * 2);
        int y0 = yuyv[si];
        int u  = yuyv[si + 1] - 128;
        int y1 = yuyv[si + 2];
        int v  = yuyv[si + 3] - 128;

        // BT.601 定点 (与 NEON 系数相同)
        constexpr int YUV_R = 359;
        constexpr int YUV_G_U = 88;
        constexpr int YUV_G_V = 183;
        constexpr int YUV_B = 454;

        auto clip = [](int x) -> uint8_t {
            return static_cast<uint8_t>(x < 0 ? 0 : (x > 255 ? 255 : x));
        };

        *dst++ = clip(y0 + ((v * YUV_R + 128) >> 8));
        *dst++ = clip(y0 - ((u * YUV_G_U + v * YUV_G_V + 128) >> 8));
        *dst++ = clip(y0 + ((u * YUV_B + 128) >> 8));
        *dst++ = clip(y1 + ((v * YUV_R + 128) >> 8));
        *dst++ = clip(y1 - ((u * YUV_G_U + v * YUV_G_V + 128) >> 8));
        *dst++ = clip(y1 + ((u * YUV_B + 128) >> 8));
    }
}
