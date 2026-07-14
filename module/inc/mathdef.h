#ifndef MATHDEF_H
#define MATHDEF_H

#include "platdef.h"

#ifdef ARM_MATH
#include "arm_math.h"
#endif

#if OS(HOSTED) && ARCH(X86_FAMILY)
#include <immintrin.h>
#elif OS(HOSTED) && ARCH(ARM64) && HAS(NEON)
#include <arm_neon.h>
#endif

#include <math.h>

#include "fastmath.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef FAST_MATH
#define SIN(x)       fast_sinf(x)
#define COS(x)       fast_cosf(x)
#define TAN(x)       fast_tanf(x)
#define EXP(x)       fast_expf(x)
#define ABS(x)       fast_absf(x)
#define SQRT(x)      fast_sqrtf(x)
#define MOD(x, y)    fast_modf(x, y)
#define CPYSGN(x, y) copysignf(x, y)
#define LOG(x)       logf(x)
#elif defined(ARM_MATH)
#define SIN(x)       arm_sin_f32(x)
#define COS(x)       arm_cos_f32(x)
#define ATAN2(y, x)  atan2f(y, x)
#define ABS(x)       fabsf(x)
#define EXP(x)       expf(x)
#define SQRT(x)      sqrtf(x)
#define MOD(x, y)    fmodf(x, y)
#define CPYSGN(x, y) copysignf(x, y)
#define LOG(x)       logf(x)
#else
#define SIN(x)       sinf(x)
#define SINH(x)      sinhf(x)
#define COS(x)       cosf(x)
#define EXP(x)       expf(x)
#define ATAN2(y, x)  atan2f(y, x)
#define ABS(x)       fabsf(x)
#define SQRT(x)      sqrtf(x)
#define MOD(x, y)    fmodf(x, y)
#define CPYSGN(x, y) copysignf(x, y)
#define LOG(x)       logf(x)
#endif

#ifndef PI
#define PI (3.1415926F)
#endif

#ifndef E
#define E (2.7182818F)
#endif

#define TAU                  (6.2831853F)
#define DIV_PI_BY_2          (1.5707963F)
#define LN2                  (0.6931471F)
#define DIV_2_BY_3           (0.6666666F)
#define DIV_3_BY_2           (1.5000000F)
#define SQRT_2               (1.4142135F)
#define SQRT_3               (1.7320508F)
#define DIV_1_BY_SQRT_3      (0.5773502F)
#define DIV_SQRT_3_BY_2      (0.8660254F)
#define DIV_SQRT_2_BY_SQRT_3 (0.8164966F)

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif

#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif

#define UP2INT(x)                        ceilf(x)  // 向上取整
#define DOWN2INT(x)                      floorf(x) // 向下取整

#define IS_NAN(x)                        isnan(x)
#define IS_INF(x)                        isinf(x)
#define IS_IN_RANGE_CLOSE(val, min, max) ((val) >= (min) && (val) <= (max))
#define IS_IN_RANGE_OPEN(val, min, max)  ((val) > (min) && (val) < (max))
#define IS_SAME_DIR(x, y)                ((x) * (y) > 0 ? TRUE : FALSE)

#define SGN(x)                           ((x) == 0.0F ? 0.0F : (x) > 0.0F ? 1.0F : -1.0F)

#define SQ(x)                            ((x) * (x))        // 平方
#define K(x)                             ((x) * 1000)       // 乘以 1K
#define M(x)                             ((x) * 1000000)    // 乘以 1M
#define G(x)                             ((x) * 1000000000) // 乘以 1G

#define RMS2PEAK(x)                      ((x) * SQRT_2)
#define PEAK2RMS(x)                      ((x) / SQRT_2)

#define RAD2DEG(rad)                     ((rad) * 57.2957795F)
#define DEG2RAD(deg)                     ((deg) * 0.01745329F)

#define RPM2RADS(rpm)                    (((rpm) / 60.0F) * TAU)
#define RADS2RPM(rads)                   (((rads) * 60.0F) / TAU)

#define HZ2RADS(hz)                      ((hz) * TAU)
#define RADS2HZ(rads)                    ((rads) / TAU)

#define MECH2ELEC(theta, npp)            ((theta) * (npp))
#define ELEC2MECH(theta, npp)            ((theta) / (npp))
#define OUTSHAFT2MECH(theta, ratio)      ((theta) * (ratio))
#define MECH2OUTSHAFT(theta, ratio)      ((theta) / (ratio))
#define OUTSHAFT2ELEC(theta, ratio, npp) ((theta) * (ratio) * (npp))
#define ELEC2OUTSHAFT(theta, ratio, npp) ((theta) / (ratio) / (npp))

#define RAW2THETA(cnt, full_cnt)         ((f32)(cnt) / (f32)full_cnt * TAU)

#define TOGGLE_THETA(dir, theta)         ((dir) == -1 ? (TAU) - (theta) : (theta))
#define TOGGLE_OMEGA(dir, omega)         ((dir) == -1 ? -(omega) : (omega))

#define CYCLE_CNT(cnt, theta, prev_theta)                         \
        do {                                                      \
                if ((cnt) == 0 && (prev_theta) == 0.0F) {         \
                        (prev_theta) = (theta);                   \
                        break;                                    \
                }                                                 \
                const f32 __delta_theta = (theta) - (prev_theta); \
                if (__delta_theta < -PI)                          \
                        (cnt)++;                                  \
                else if (__delta_theta > PI)                      \
                        (cnt)--;                                  \
                (prev_theta) = (theta);                           \
        } while (0)

#define IS_POWER_OF_2(n)    ((n) != 0 && (((n) & ((n) - 1)) == 0))
#define ROUNDUP_POW_OF_2(n) ((n) == 0 ? 1 : (1ULL << (sizeof(n) * 8 - clz64((n) - 1))))

#define INTEGRATOR(ret, val, gain, fs)          \
        do {                                    \
                (ret) += (gain) * (val) / (fs); \
        } while (0)

#define DERIVATIVE(ret, val, prev_val, gain, fs)                   \
        do {                                                       \
                (ret)      = (gain) * ((val) - (prev_val)) * (fs); \
                (prev_val) = (val);                                \
        } while (0)

#define THETA_DERIVATIVE(ret, theta, prev_theta, gain, fs)      \
        do {                                                    \
                f32 __delta_theta;                              \
                WARP_PI(__delta_theta, (theta) - (prev_theta)); \
                (ret)        = (gain) * (__delta_theta) * (fs); \
                (prev_theta) = (theta);                         \
        } while (0)

/**
 * @brief 一阶低通滤波
 *
 * @param ret 滤波后的值
 * @param val 待滤波的值
 * @param wc  截止频率 (rad/s)
 * @param fs  采样频率 (Hz)
 */
#define LOWPASS(ret, val, wc, fs)                                                         \
        do {                                                                              \
                if ((wc) == 0.0F)                                                         \
                        (ret) = (val);                                                    \
                else {                                                                    \
                        const f32 __rc    = 1.0F / (wc);                                  \
                        const f32 __alpha = 1.0F / (1.0F + (__rc) * (fs));                \
                        (ret)             = (__alpha) * (val) + (1.0F - __alpha) * (ret); \
                }                                                                         \
        } while (0)

/**
 * @brief 一阶高通滤波 y[k] = beta * (y[k-1] + x[k] - x[k-1])
 *
 * @param ret      滤波后的输出值 (y[k])
 * @param val      待滤波的输入值 (x[k])
 * @param prev_val 上一次的输入值 (x[k-1])
 * @param wc       截止频率 (rad/s)
 * @param fs       采样频率 (Hz)
 */
#define HIGHPASS(ret, val, prev_val, wc, fs)                                         \
        do {                                                                         \
                if ((wc) == 0.0F)                                                    \
                        (ret) = 0.0F;                                                \
                else {                                                               \
                        const f32 __rc    = 1.0F / (wc);                             \
                        const f32 __alpha = 1.0F / (1.0F + (__rc) * (fs));           \
                        const f32 __beta  = 1.0F - (__alpha);                        \
                        (ret)             = (__beta) * ((ret) + (val) - (prev_val)); \
                        (prev_val)        = (val);                                   \
                }                                                                    \
        } while (0)

#define CLAMP(ret, min, max)                                      \
        do {                                                      \
                (ret) = ((ret) <= (min)) ? (min) : MIN(ret, max); \
        } while (0)

#define CLAMP_RET(val, min, max) ((val) <= (min)) ? (min) : MIN(val, max)

#define UVW_CLAMP(ret, min, max)              \
        do {                                  \
                CLAMP((ret).u, (min), (max)); \
                CLAMP((ret).v, (min), (max)); \
                CLAMP((ret).w, (min), (max)); \
        } while (0)

#define WARP_PI(ret, rad)                        \
        do {                                     \
                f32 __tmp;                       \
                if (ABS(rad) > TAU)              \
                        __tmp = MOD((rad), TAU); \
                else                             \
                        __tmp = (rad);           \
                if (__tmp > PI)                  \
                        (ret) = __tmp - TAU;     \
                else if (__tmp < -PI)            \
                        (ret) = __tmp + TAU;     \
                else                             \
                        (ret) = __tmp;           \
        } while (0)

#define WARP_TAU(ret, rad)                       \
        do {                                     \
                f32 __tmp;                       \
                if (ABS(rad) > TAU)              \
                        __tmp = MOD((rad), TAU); \
                else                             \
                        __tmp = (rad);           \
                if (__tmp < 0.0F)                \
                        (ret) = __tmp + TAU;     \
                else                             \
                        (ret) = __tmp;           \
        } while (0)

#define UVW_ADD_VEC(ret, x, y)           \
        do {                             \
                (ret).u = (x).u + (y).u; \
                (ret).v = (x).v + (y).v; \
                (ret).w = (x).w + (y).w; \
        } while (0)

#define UVW_SUB_VEC(ret, x, y)           \
        do {                             \
                (ret).u = (x).u - (y).u; \
                (ret).v = (x).v - (y).v; \
                (ret).w = (x).w - (y).w; \
        } while (0)

#define UVW_MUL_VEC(ret, x, y)           \
        do {                             \
                (ret).u = (x).u * (y).u; \
                (ret).v = (x).v * (y).v; \
                (ret).w = (x).w * (y).w; \
        } while (0)

#define UVW_DIV_VEC(ret, x, y)           \
        do {                             \
                (ret).u = (x).u / (y).u; \
                (ret).v = (x).v / (y).v; \
                (ret).w = (x).w / (y).w; \
        } while (0)

#define UVW_ADD(ret, x, y)             \
        do {                           \
                (ret).u = (x).u + (y); \
                (ret).v = (x).v + (y); \
                (ret).w = (x).w + (y); \
        } while (0)

#define UVW_SUB(ret, x, y)             \
        do {                           \
                (ret).u = (x).u - (y); \
                (ret).v = (x).v - (y); \
                (ret).w = (x).w - (y); \
        } while (0)

#define UVW_MUL(ret, x, y)             \
        do {                           \
                (ret).u = (x).u * (y); \
                (ret).v = (x).v * (y); \
                (ret).w = (x).w * (y); \
        } while (0)

#define UVW_DIV(ret, x, y)             \
        do {                           \
                (ret).u = (x).u / (y); \
                (ret).v = (x).v / (y); \
                (ret).w = (x).w / (y); \
        } while (0)

#define AB_ADD_VEC(ret, x, y)            \
        do {                             \
                (ret).a = (x).a + (y).a; \
                (ret).b = (x).b + (y).b; \
        } while (0)

#define AB_SUB_VEC(ret, x, y)            \
        do {                             \
                (ret).a = (x).a - (y).a; \
                (ret).b = (x).b - (y).b; \
        } while (0)

#define AB_MUL_VEC(ret, x, y)            \
        do {                             \
                (ret).a = (x).a * (y).a; \
                (ret).b = (x).b * (y).b; \
        } while (0)

#define AB_DIV_VEC(ret, x, y)            \
        do {                             \
                (ret).a = (x).a / (y).a; \
                (ret).b = (x).b / (y).b; \
        } while (0)

HAPI u8
is_f32_equal(const f32 x, const f32 y, const f32 rel_tol, const f32 abs_tol)
{
        /* 计算两数的绝对差值 */
        const f32 diff = ABS(x - y);

        /* 绝对误差检查 */
        if (diff <= abs_tol)
                return TRUE;

        /* 相对误差检查 */
        const f32 abs_x   = ABS(x);
        const f32 abs_y   = ABS(y);
        const f32 max_val = (abs_x > abs_y) ? abs_x : abs_y;

        /* 判定差值是否在最大值的相对允许比例范围内 */
        return diff <= (max_val * rel_tol);
}

HAPI f32_ab_t
clarke_amp(const f32_uvw_t f32_abc)
{
        f32_ab_t f32_ab;
        f32_ab.a = DIV_2_BY_3 * (f32_abc.u - 0.5f * (f32_abc.v + f32_abc.w));
        f32_ab.b = DIV_2_BY_3 * (f32_abc.v - f32_abc.w) * DIV_SQRT_3_BY_2;
        return f32_ab;
}

HAPI f32_ab_t
clarke_pow(const f32_uvw_t f32_abc)
{
        f32_ab_t f32_ab;
        f32_ab.a = DIV_SQRT_2_BY_SQRT_3 * (f32_abc.u - 0.5f * (f32_abc.v + f32_abc.w));
        f32_ab.b = DIV_SQRT_2_BY_SQRT_3 * (f32_abc.v - f32_abc.w) * DIV_SQRT_3_BY_2;
        return f32_ab;
}

HAPI f32_uvw_t
inv_clarke(const f32_ab_t f32_ab)
{
        f32_uvw_t f32_uvw;
        const f32 f32_a = -(f32_ab.a * 0.5f);
        const f32 f32_b = f32_ab.b * DIV_SQRT_3_BY_2;
        f32_uvw.u       = f32_ab.a;
        f32_uvw.v       = f32_a + f32_b;
        f32_uvw.w       = f32_a - f32_b;
        return f32_uvw;
}

HAPI f32_dq_t
park(const f32_ab_t f32_ab, const f32 theta)
{
        f32_dq_t f32_dq;
        f32_dq.d = f32_ab.b * SIN(theta) + f32_ab.a * COS(theta);
        f32_dq.q = f32_ab.b * COS(theta) - f32_ab.a * SIN(theta);
        return f32_dq;
}

HAPI f32_ab_t
inv_park(const f32_dq_t f32_dq, const f32 theta)
{
        f32_ab_t f32_ab;
        f32_ab.a = f32_dq.d * COS(theta) - f32_dq.q * SIN(theta);
        f32_ab.b = f32_dq.d * SIN(theta) + f32_dq.q * COS(theta);
        return f32_ab;
}

HAPI void
find_max(const f32 *arr, const usize n, f32 *max_val, usize *max_idx)
{
        if (n == 0) {
                *max_val = 0.0f;
                *max_idx = 0;
                return;
        }

#if defined(__AVX__)
        usize  i       = 0;
        __m256 max_vec = _mm256_set1_ps(arr[0]);
        f32    tmp_max = arr[0];
        usize  idx_max = 0;

        for (; i + 7 < n; i += 8) {
                __m256 v = _mm256_loadu_ps(arr + i);
                max_vec  = _mm256_max_ps(max_vec, v);

                f32 tmp[8];
                _mm256_storeu_ps(tmp, max_vec);
                for (int j = 0; j < 8; j++) {
                        if (tmp[j] > tmp_max) {
                                tmp_max = tmp[j];
                                idx_max = i + j;
                        }
                }
        }

        for (; i < n; i++) {
                if (arr[i] > tmp_max) {
                        tmp_max = arr[i];
                        idx_max = i;
                }
        }

        *max_val = tmp_max;
        *max_idx = idx_max;

#elif defined(__SSE__)
        usize  i       = 0;
        __m128 max_vec = _mm_set1_ps(arr[0]);
        f32    tmp_max = arr[0];
        usize  idx_max = 0;

        for (; i + 3 < n; i += 4) {
                const __m128 v = _mm_loadu_ps(arr + i);
                max_vec        = _mm_max_ps(max_vec, v);

                f32 tmp[4];
                _mm_storeu_ps(tmp, max_vec);
                for (int j = 0; j < 4; j++) {
                        if (tmp[j] > tmp_max) {
                                tmp_max = tmp[j];
                                idx_max = i + j;
                        }
                }
        }

        for (; i < n; i++) {
                if (arr[i] > tmp_max) {
                        tmp_max = arr[i];
                        idx_max = i;
                }
        }

        *max_val = tmp_max;
        *max_idx = idx_max;

#elif HAS(NEON)
        usize       i       = 0;
        float32x4_t max_vec = vdupq_n_f32(arr[0]);
        f32         tmp_max = arr[0];
        usize       idx_max = 0;

        for (; i + 3 < n; i += 4) {
                const float32x4_t v = vld1q_f32(arr + i);
                max_vec             = vmaxq_f32(max_vec, v);

                f32 tmp[4];
                vst1q_f32(tmp, max_vec);
                for (int j = 0; j < 4; j++) {
                        if (tmp[j] > tmp_max) {
                                tmp_max = tmp[j];
                                idx_max = i + j;
                        }
                }
        }

        for (; i < n; i++) {
                if (arr[i] > tmp_max) {
                        tmp_max = arr[i];
                        idx_max = i;
                }
        }

        *max_val = tmp_max;
        *max_idx = idx_max;

#else
        f32   tmp_max = arr[0];
        usize idx_max = 0;
        for (usize i = 1; i < n; i++) {
                if (arr[i] > tmp_max) {
                        tmp_max = arr[i];
                        idx_max = i;
                }
        }
        *max_val = tmp_max;
        *max_idx = idx_max;
#endif
}

/**
 * @brief 多项式计算
 *
 * @param coeffs 多项式系数, 升幂排列
 * @param order  阶数
 * @param x      输入值
 * @return f32   输出值
 */
HAPI f32
poly_eval(const f32 *coeffs, const u32 order, const f32 x)
{
        f32 res = coeffs[order];
        for (u32 i = order; i > 0; i--)
                res = res * x + coeffs[i - 1];

        return res;
}

HAPI u16
f32_to_f16(const f32 f32_val)
{
        const union {
                f32 f;
                u32 i;
        } u = {.f = f32_val};

        const u32 sign     = u.i >> 16 & 0x8000;
        const u32 exponent = u.i >> 23 & 0xFF;
        const u32 mantissa = u.i & 0x007FFFFF;

        /* 处理零、无穷大、NaN */
        if (exponent == 0)
                return (u16)sign;

        if (exponent == 255)
                return (u16)(sign | (mantissa == 0 ? 0x7C00 : 0x7E00));

        /* 计算 FP16 指数 */
        const i32 f16_exp = (i32)exponent - 112; // -127 + 15 = -112

        /* 处理溢出和下溢 */
        if (f16_exp >= 31)
                return (u16)(sign | 0x7C00); // 溢出 -> 无穷大

        if (f16_exp <= 0)
                return (u16)sign; // 下溢 -> 零

        /* 组合结果 */
        return (u16)(sign | f16_exp << 10 | mantissa >> 13);
}

HAPI f32
f16_to_f32(const u16 f16_val)
{
        const u32 sign     = ((u32)f16_val & 0x8000) << 16;
        u32       exponent = (f16_val >> 10) & 0x1F;
        u32       mantissa = (u32)f16_val & 0x03FF;

        if (exponent == 0) {
                if (mantissa == 0) {
                        const union {
                                f32 f;
                                u32 i;
                        } u = {.i = sign};
                        return u.f;
                }
                /* 非规格化数处理 */
                exponent = 1;
                while ((mantissa & 0x0400) == 0) {
                        mantissa <<= 1;
                        exponent--;
                }
                mantissa &= 0x03FF;
        } else if (exponent == 31) {
                const union {
                        f32 f;
                        u32 i;
                } u = {.i = sign | (mantissa == 0 ? 0x7F800000 : 0x7FC00000)};
                return u.f;
        }

        const u32 f32_exp      = (exponent + 112) << 23; // 112 = 127 - 15
        const u32 f32_mantissa = mantissa << 13;

        const union {
                f32 f;
                u32 i;
        } u = {.i = sign | f32_exp | f32_mantissa};
        return u.f;
}

HAPI void
f32_sort_asc(f32 *buf, usize len)
{
        if (!buf || len < 2)
                return;

        for (usize gap = len >> 1; gap > 0; gap >>= 1) {
                for (usize i = gap; i < len; i++) {
                        f32   temp = buf[i];
                        usize j;
                        for (j = i; j >= gap && buf[j - gap] > temp; j -= gap)
                                buf[j] = buf[j - gap];

                        buf[j] = temp;
                }
        }
}

HAPI void
f32_sort_desc(f32 *buf, usize len)
{
        if (!buf || len < 2)
                return;

        for (usize gap = len >> 1; gap > 0; gap >>= 1) {
                for (usize i = gap; i < len; i++) {
                        f32   temp = buf[i];
                        usize j;
                        for (j = i; j >= gap && buf[j - gap] < temp; j -= gap)
                                buf[j] = buf[j - gap];

                        buf[j] = temp;
                }
        }
}

#ifdef __cplusplus
}
#endif

#endif // !MATHDEF_H
