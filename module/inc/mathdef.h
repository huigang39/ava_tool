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
#include "motor_control/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef FAST_MATH
#define SIN(x)       fast_sinf_rt(x)
#define COS(x)       fast_cosf_rt(x)
#define TAN(x)       fast_tanf_rt(x)
#define EXP(x)       fast_expf_rt(x)
#define ABS(x)       fast_absf_rt(x)
#define SQRT(x)      fast_sqrtf_rt(x)
#define MOD(x, y)    fast_modf_rt(x, y)
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
#define DIV_1_BY_3           (0.3333333F)
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
#define IS_SAME_DIR(x, y)                ((x) * (y) > 0 ? true : false)

HAPI uint8_t
f32_is_finite_rt(const float32_t val)
{
    return !IS_NAN(val) && !IS_INF(val);
}

HAPI float32_t
f32_finite_or_rt(const float32_t val, const float32_t fallback)
{
    return f32_is_finite_rt(val) ? val : fallback;
}

HAPI float32_t
wrap_pi_once_rt(const float32_t rad)
{
    if (rad > PI)
        return rad - TAU;
    if (rad < -PI)
        return rad + TAU;
    return rad;
}

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

#define RAW2THETA(cnt, full_cnt)         ((float32_t)(cnt) / (float32_t)full_cnt * TAU)

#define TOGGLE_THETA(dir, theta)         ((dir) == -1 ? (TAU) - (theta) : (theta))
#define TOGGLE_OMEGA(dir, omega)         ((dir) == -1 ? -(omega) : (omega))

#define CYCLE_CNT(cnt, theta, prev_theta)                       \
    do {                                                        \
        if ((cnt) == 0 && (prev_theta) == 0.0F) {               \
            (prev_theta) = (theta);                             \
            break;                                              \
        }                                                       \
        const float32_t __delta_theta = (theta) - (prev_theta); \
        if (__delta_theta < -PI)                                \
            (cnt)++;                                            \
        else if (__delta_theta > PI)                            \
            (cnt)--;                                            \
        (prev_theta) = (theta);                                 \
    } while (0)

#define IS_POWER_OF_2(n)    ((n) != 0 && (((n) & ((n) - 1)) == 0))
#define ROUNDUP_POW_OF_2(n) ((n) == 0 ? 1 : (1ULL << (sizeof(n) * 8 - clz64((n) - 1))))

#define INTEGRATOR(ret, val, gain, fs)  \
    do {                                \
        (ret) += (gain) * (val) / (fs); \
    } while (0)

#define DERIVATIVE(ret, val, prev_val, gain, fs)           \
    do {                                                   \
        (ret)      = (gain) * ((val) - (prev_val)) * (fs); \
        (prev_val) = (val);                                \
    } while (0)

#define THETA_DERIVATIVE(ret, theta, prev_theta, gain, fs) \
    do {                                                   \
        float32_t __delta_theta;                           \
        WARP_PI(__delta_theta, (theta) - (prev_theta));    \
        (ret)        = (gain) * (__delta_theta) * (fs);    \
        (prev_theta) = (theta);                            \
    } while (0)

/**
 * @brief 一阶低通滤波
 *
 * @param ret 滤波后的值
 * @param val 待滤波的值
 * @param wc  截止频率 (rad/s)
 * @param fs  采样频率 (Hz)
 */
#define LOWPASS(ret, val, wc, fs)                                                   \
    do {                                                                            \
        if ((wc) == 0.0F)                                                           \
            (ret) = (val);                                                          \
        else {                                                                      \
            const float32_t __rc    = 1.0F / (wc);                                  \
            const float32_t __alpha = 1.0F / (1.0F + (__rc) * (fs));                \
            (ret)                   = (__alpha) * (val) + (1.0F - __alpha) * (ret); \
        }                                                                           \
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
#define HIGHPASS(ret, val, prev_val, wc, fs)                                   \
    do {                                                                       \
        if ((wc) == 0.0F)                                                      \
            (ret) = 0.0F;                                                      \
        else {                                                                 \
            const float32_t __rc    = 1.0F / (wc);                             \
            const float32_t __alpha = 1.0F / (1.0F + (__rc) * (fs));           \
            const float32_t __beta  = 1.0F - (__alpha);                        \
            (ret)                   = (__beta) * ((ret) + (val) - (prev_val)); \
            (prev_val)              = (val);                                   \
        }                                                                      \
    } while (0)

#define CLAMP(ret, min, max)                              \
    do {                                                  \
        (ret) = ((ret) <= (min)) ? (min) : MIN(ret, max); \
    } while (0)

#define CLAMP_RET(val, min, max) ((val) <= (min)) ? (min) : MIN(val, max)

#define UVW_CLAMP(ret, min, max)      \
    do {                              \
        CLAMP((ret).u, (min), (max)); \
        CLAMP((ret).v, (min), (max)); \
        CLAMP((ret).w, (min), (max)); \
    } while (0)

#define WARP_PI(ret, rad)            \
    do {                             \
        float32_t __tmp;             \
        if (ABS(rad) > TAU)          \
            __tmp = MOD((rad), TAU); \
        else                         \
            __tmp = (rad);           \
        if (__tmp > PI)              \
            (ret) = __tmp - TAU;     \
        else if (__tmp < -PI)        \
            (ret) = __tmp + TAU;     \
        else                         \
            (ret) = __tmp;           \
    } while (0)

#define WARP_TAU(ret, rad)           \
    do {                             \
        float32_t __tmp;             \
        if (ABS(rad) > TAU)          \
            __tmp = MOD((rad), TAU); \
        else                         \
            __tmp = (rad);           \
        if (__tmp < 0.0F)            \
            (ret) = __tmp + TAU;     \
        else                         \
            (ret) = __tmp;           \
    } while (0)

#define UVW_ADD_VEC(ret, x, y)   \
    do {                         \
        (ret).u = (x).u + (y).u; \
        (ret).v = (x).v + (y).v; \
        (ret).w = (x).w + (y).w; \
    } while (0)

#define UVW_SUB_VEC(ret, x, y)   \
    do {                         \
        (ret).u = (x).u - (y).u; \
        (ret).v = (x).v - (y).v; \
        (ret).w = (x).w - (y).w; \
    } while (0)

#define UVW_MUL_VEC(ret, x, y)   \
    do {                         \
        (ret).u = (x).u * (y).u; \
        (ret).v = (x).v * (y).v; \
        (ret).w = (x).w * (y).w; \
    } while (0)

#define UVW_DIV_VEC(ret, x, y)   \
    do {                         \
        (ret).u = (x).u / (y).u; \
        (ret).v = (x).v / (y).v; \
        (ret).w = (x).w / (y).w; \
    } while (0)

#define UVW_ADD(ret, x, y)     \
    do {                       \
        (ret).u = (x).u + (y); \
        (ret).v = (x).v + (y); \
        (ret).w = (x).w + (y); \
    } while (0)

#define UVW_SUB(ret, x, y)     \
    do {                       \
        (ret).u = (x).u - (y); \
        (ret).v = (x).v - (y); \
        (ret).w = (x).w - (y); \
    } while (0)

#define UVW_MUL(ret, x, y)     \
    do {                       \
        (ret).u = (x).u * (y); \
        (ret).v = (x).v * (y); \
        (ret).w = (x).w * (y); \
    } while (0)

#define UVW_DIV(ret, x, y)     \
    do {                       \
        (ret).u = (x).u / (y); \
        (ret).v = (x).v / (y); \
        (ret).w = (x).w / (y); \
    } while (0)

#define AB_ADD_VEC(ret, x, y)    \
    do {                         \
        (ret).a = (x).a + (y).a; \
        (ret).b = (x).b + (y).b; \
    } while (0)

#define AB_SUB_VEC(ret, x, y)    \
    do {                         \
        (ret).a = (x).a - (y).a; \
        (ret).b = (x).b - (y).b; \
    } while (0)

#define AB_MUL_VEC(ret, x, y)    \
    do {                         \
        (ret).a = (x).a * (y).a; \
        (ret).b = (x).b * (y).b; \
    } while (0)

#define AB_DIV_VEC(ret, x, y)    \
    do {                         \
        (ret).a = (x).a / (y).a; \
        (ret).b = (x).b / (y).b; \
    } while (0)

HAPI uint8_t
is_f32_equal(const float32_t x, const float32_t y, const float32_t rel_tol, const float32_t abs_tol)
{
    /* 计算两数的绝对差值 */
    const float32_t diff = ABS(x - y);

    /* 绝对误差检查 */
    if (diff <= abs_tol)
        return true;

    /* 相对误差检查 */
    const float32_t abs_x   = ABS(x);
    const float32_t abs_y   = ABS(y);
    const float32_t max_val = (abs_x > abs_y) ? abs_x : abs_y;

    /* 判定差值是否在最大值的相对允许比例范围内 */
    return diff <= (max_val * rel_tol);
}

HAPI struct f32_ab
clarke_amp_rt(const struct f32_uvw f32_abc)
{
    struct f32_ab f32_ab;
    f32_ab.a = DIV_2_BY_3 * (f32_abc.u - 0.5F * (f32_abc.v + f32_abc.w));
    f32_ab.b = DIV_2_BY_3 * (f32_abc.v - f32_abc.w) * DIV_SQRT_3_BY_2;
    return f32_ab;
}

HAPI struct f32_ab
clarke_pow(const struct f32_uvw f32_abc)
{
    struct f32_ab f32_ab;
    f32_ab.a = DIV_SQRT_2_BY_SQRT_3 * (f32_abc.u - 0.5F * (f32_abc.v + f32_abc.w));
    f32_ab.b = DIV_SQRT_2_BY_SQRT_3 * (f32_abc.v - f32_abc.w) * DIV_SQRT_3_BY_2;
    return f32_ab;
}

HAPI struct f32_uvw
inv_clarke_rt(const struct f32_ab f32_ab)
{
    struct f32_uvw  f32_uvw;
    const float32_t f32_a = -(f32_ab.a * 0.5F);
    const float32_t f32_b = f32_ab.b * DIV_SQRT_3_BY_2;
    f32_uvw.u             = f32_ab.a;
    f32_uvw.v             = f32_a + f32_b;
    f32_uvw.w             = f32_a - f32_b;
    return f32_uvw;
}

HAPI struct f32_dq
park_sincos_rt(const struct f32_ab f32_ab, const float32_t sin_theta, const float32_t cos_theta)
{
    struct f32_dq f32_dq;
    f32_dq.d = f32_ab.b * sin_theta + f32_ab.a * cos_theta;
    f32_dq.q = f32_ab.b * cos_theta - f32_ab.a * sin_theta;
    return f32_dq;
}

HAPI struct f32_ab
inv_park_sincos_rt(const struct f32_dq f32_dq, const float32_t sin_theta, const float32_t cos_theta)
{
    struct f32_ab f32_ab;
    f32_ab.a = f32_dq.d * cos_theta - f32_dq.q * sin_theta;
    f32_ab.b = f32_dq.d * sin_theta + f32_dq.q * cos_theta;
    return f32_ab;
}

HAPI struct f32_dq
park_rt(const struct f32_ab f32_ab, const float32_t theta)
{
    return park_sincos_rt(f32_ab, SIN(theta), COS(theta));
}

HAPI struct f32_ab
inv_park_rt(const struct f32_dq f32_dq, const float32_t theta)
{
    return inv_park_sincos_rt(f32_dq, SIN(theta), COS(theta));
}

HAPI void
find_max(const float32_t *arr, const size_t n, float32_t *max_val, size_t *max_idx)
{
    if (n == 0) {
        *max_val = 0.0F;
        *max_idx = 0;
        return;
    }

#if defined(__AVX__)
    size_t    i       = 0;
    __m256    max_vec = _mm256_set1_ps(arr[0]);
    float32_t tmp_max = arr[0];
    size_t    idx_max = 0;

    for (; i + 7 < n; i += 8) {
        __m256 v = _mm256_loadu_ps(arr + i);
        max_vec  = _mm256_max_ps(max_vec, v);

        float32_t tmp[8];
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
    size_t    i       = 0;
    __m128    max_vec = _mm_set1_ps(arr[0]);
    float32_t tmp_max = arr[0];
    size_t    idx_max = 0;

    for (; i + 3 < n; i += 4) {
        const __m128 v = _mm_loadu_ps(arr + i);
        max_vec        = _mm_max_ps(max_vec, v);

        float32_t tmp[4];
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
    size_t      i       = 0;
    float32x4_t max_vec = vdupq_n_f32(arr[0]);
    float32_t   tmp_max = arr[0];
    size_t      idx_max = 0;

    for (; i + 3 < n; i += 4) {
        const float32x4_t v = vld1q_f32(arr + i);
        max_vec             = vmaxq_f32(max_vec, v);

        float32_t tmp[4];
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
    float32_t tmp_max = arr[0];
    size_t    idx_max = 0;
    for (size_t i = 1; i < n; i++) {
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
 * @return float32_t   输出值
 */
HAPI float32_t
poly_eval_rt(const float32_t *coeffs, const uint32_t order, const float32_t x)
{
    float32_t res = coeffs[order];
    for (uint32_t i = order; i > 0; i--)
        res = res * x + coeffs[i - 1];

    return res;
}

HAPI uint16_t
f32_to_f16(const float32_t f32_val)
{
    const union {
        float32_t f;
        uint32_t  i;
    } u = {.f = f32_val};

    const uint32_t sign     = u.i >> 16 & 0x8000;
    const uint32_t exponent = u.i >> 23 & 0xFF;
    const uint32_t mantissa = u.i & 0x007FFFFF;

    /* 处理零,无穷大,NaN */
    if (exponent == 0)
        return (uint16_t)sign;

    if (exponent == 255)
        return (uint16_t)(sign | (mantissa == 0 ? 0x7C00 : 0x7E00));

    /* 计算 FP16 指数 */
    const int32_t f16_exp = (int32_t)exponent - 112; // -127 + 15 = -112

    /* 处理溢出和下溢 */
    if (f16_exp >= 31)
        return (uint16_t)(sign | 0x7C00); // 溢出 -> 无穷大

    if (f16_exp <= 0)
        return (uint16_t)sign; // 下溢 -> 零

    /* 组合结果 */
    return (uint16_t)(sign | f16_exp << 10 | mantissa >> 13);
}

HAPI float32_t
f16_to_f32(const uint16_t f16_val)
{
    const uint32_t sign     = ((uint32_t)f16_val & 0x8000) << 16;
    uint32_t       exponent = (f16_val >> 10) & 0x1F;
    uint32_t       mantissa = (uint32_t)f16_val & 0x03FF;

    if (exponent == 0) {
        if (mantissa == 0) {
            const union {
                float32_t f;
                uint32_t  i;
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
            float32_t f;
            uint32_t  i;
        } u = {.i = sign | (mantissa == 0 ? 0x7F800000 : 0x7FC00000)};
        return u.f;
    }

    const uint32_t f32_exp      = (exponent + 112) << 23; // 112 = 127 - 15
    const uint32_t f32_mantissa = mantissa << 13;

    const union {
        float32_t f;
        uint32_t  i;
    } u = {.i = sign | f32_exp | f32_mantissa};
    return u.f;
}

HAPI void
f32_sort_asc(float32_t *buf, size_t len)
{
    if (!buf || len < 2)
        return;

    for (size_t gap = len >> 1; gap > 0; gap >>= 1) {
        for (size_t i = gap; i < len; i++) {
            float32_t temp = buf[i];
            size_t    j;
            for (j = i; j >= gap && buf[j - gap] > temp; j -= gap)
                buf[j] = buf[j - gap];

            buf[j] = temp;
        }
    }
}

HAPI void
f32_sort_desc(float32_t *buf, size_t len)
{
    if (!buf || len < 2)
        return;

    for (size_t gap = len >> 1; gap > 0; gap >>= 1) {
        for (size_t i = gap; i < len; i++) {
            float32_t temp = buf[i];
            size_t    j;
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
