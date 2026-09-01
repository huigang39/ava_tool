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

#ifdef __cplusplus
extern "C" {
#endif

#ifdef FAST_MATH
#define SIN(x) fast_sinf_rt(x)
#define COS(x) fast_cosf_rt(x)
#define TAN(x) fast_tanf_rt(x)
#define EXP(x) fast_expf_rt(x)
#define ABS(x) fast_absf_rt(x)
#define SQRT(x) fast_sqrtf_rt(x)
#define MOD(x, y) fast_modf_rt(x, y)
#define CPYSGN(x, y) copysignf(x, y)
#define LOG(x) logf(x)
#elif defined(ARM_MATH)
#define SIN(x) arm_sin_f32(x)
#define COS(x) arm_cos_f32(x)
#define ATAN2(y, x) atan2f(y, x)
#define ABS(x) fabsf(x)
#define EXP(x) expf(x)
#define SQRT(x) sqrtf(x)
#define MOD(x, y) fmodf(x, y)
#define CPYSGN(x, y) copysignf(x, y)
#define LOG(x) logf(x)
#else
#define SIN(x) sinf(x)
#define SINH(x) sinhf(x)
#define COS(x) cosf(x)
#define EXP(x) expf(x)
#define ATAN2(y, x) atan2f(y, x)
#define ABS(x) fabsf(x)
#define SQRT(x) sqrtf(x)
#define MOD(x, y) fmodf(x, y)
#define CPYSGN(x, y) copysignf(x, y)
#define LOG(x) logf(x)
#endif

#ifndef PI
#define PI (3.1415926F)
#endif

#ifndef E
#define E (2.7182818F)
#endif

#define TAU (6.2831853F)
#define DIV_PI_BY_2 (1.5707963F)
#define LN2 (0.6931471F)
#define DIV_2_BY_3 (0.6666666F)
#define DIVIDER_3_BY_2 (1.5000000F)
#define SQRT_2 (1.4142135F)
#define SQRT_3 (1.7320508F)
#define DIVIDER_1_BY_3 (0.3333333F)
#define DIVIDER_1_BY_SQRT_3 (0.5773502F)
#define DIV_SQRT_3_BY_2 (0.8660254F)
#define DIV_SQRT_2_BY_SQRT_3 (0.8164966F)

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif

#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif

#define UP2INT(x) ceilf(x)    // 向上取整
#define DOWN2INT(x) floorf(x) // 向下取整

#define IS_NAN(x) isnan(x)
#define IS_INF(x) isinf(x)
#define IS_IN_RANGE_CLOSE(val, min, max) ((val) >= (min) && (val) <= (max))
#define IS_IN_RANGE_OPEN(val, min, max) ((val) > (min) && (val) < (max))
#define IS_SAME_DIRECTION(x, y) ((x) * (y) > 0 ? true : false)

/**
 * @brief         判断浮点数是否有限
 * @param[in] val 输入值
 * @return        有限浮点数返回 true, 否则返回 false
 */
HAPI uint8_t is_f32_finite_rt(const float32_t val)
{
    return !IS_NAN(val) && !IS_INF(val);
}

/**
 * @brief              对非有限浮点数使用替代值
 * @param[in] val      输入值
 * @param[in] fallback 非有限输入的替代值
 * @return             有限输入值或指定替代值
 */
HAPI float32_t f32_finite_or_rt(const float32_t val, const float32_t fallback)
{
    return is_f32_finite_rt(val) ? val : fallback;
}

/**
 * @brief         对角度执行一次正负 PI 范围修正
 * @param[in] rad 角度, 单位: rad
 * @return        经过一次正负 PI 范围修正的角度
 */
HAPI float32_t wrap_pi_once_rt(const float32_t rad)
{
    if (rad > PI) {
        return rad - TAU;
    }
    if (rad < -PI) {
        return rad + TAU;
    }
    return rad;
}

#define SGN(x) ((x) == 0.0F ? 0.0F : (x) > 0.0F ? 1.0F : -1.0F)

#define SQ(x) ((x) * (x))       // 平方
#define K(x) ((x) * 1000)       // 乘以 1K
#define M(x) ((x) * 1000000)    // 乘以 1M
#define G(x) ((x) * 1000000000) // 乘以 1G

#define RMS2PEAK(x) ((x) * SQRT_2)
#define PEAK2RMS(x) ((x) / SQRT_2)

#define RAD2DEG(rad) ((rad) * 57.2957795F)
#define DEG2RAD(deg) ((deg) * 0.01745329F)

#define RPM2RADS(rpm) (((rpm) / 60.0F) * TAU)
#define RADS2RPM(rads) (((rads) * 60.0F) / TAU)

#define HZ2RADS(hz) ((hz) * TAU)
#define RADS2HZ(rads) ((rads) / TAU)

#define MECH2ELEC(theta, npp) ((theta) * (npp))
#define ELEC2MECH(theta, npp) ((theta) / (npp))
#define OUTSHAFT2MECH(theta, ratio) ((theta) * (ratio))
#define MECH2OUTSHAFT(theta, ratio) ((theta) / (ratio))
#define OUTSHAFT2ELEC(theta, ratio, npp) ((theta) * (ratio) * (npp))
#define ELEC2OUTSHAFT(theta, ratio, npp) ((theta) / (ratio) / (npp))

#define RAW2THETA(cnt, full_cnt) ((float32_t)(cnt) / (float32_t)full_cnt * TAU)

#define TOGGLE_THETA(dir, theta) ((dir) == -1 ? (TAU) - (theta) : (theta))
#define TOGGLE_OMEGA(dir, omega) ((dir) == -1 ? -(omega) : (omega))

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

#define IS_POWER_OF_2(n) ((n) != 0 && (((n) & ((n) - 1)) == 0))
#define ROUNDUP_POW_OF_2(n) \
    ((n) == 0 ? 1 : (1ULL << (sizeof(n) * 8 - clz64((n) - 1))))

#define INTEGRATOR(result, val, gain, fs)  \
    do {                                   \
        (result) += (gain) * (val) / (fs); \
    } while (0)

#define DERIVATIVE(result, val, prev_val, gain, fs)      \
    do {                                                 \
        (result) = (gain) * ((val) - (prev_val)) * (fs); \
        (prev_val) = (val);                              \
    } while (0)

#define THETA_DERIVATIVE(result, theta, prev_theta, gain, fs) \
    do {                                                      \
        float32_t __delta_theta;                              \
        WARP_PI(__delta_theta, (theta) - (prev_theta));       \
        (result) = (gain) * (__delta_theta) * (fs);           \
        (prev_theta) = (theta);                               \
    } while (0)

/**
 * @brief                一阶低通滤波
 * @param[in,out] result 滤波后的值
 * @param[in]     val    待滤波的值
 * @param[in]     wc     截止频率 (rad/s)
 * @param[in]     fs     采样频率 (Hz)
 */
#define LOWPASS(result, val, wc, fs)                                    \
    do {                                                                \
        if ((wc) == 0.0F)                                               \
            (result) = (val);                                           \
        else {                                                          \
            const float32_t __rc = 1.0F / (wc);                         \
            const float32_t __alpha = 1.0F / (1.0F + (__rc) * (fs));    \
            (result) = (__alpha) * (val) + (1.0F - __alpha) * (result); \
        }                                                               \
    } while (0)

/**
 * @brief                  一阶高通滤波 y[k] = beta * (y[k-1] + x[k] - x[k-1])
 * @param[in,out] result   滤波后的输出值 (y[k])
 * @param[in]     val      待滤波的输入值 (x[k])
 * @param[in,out] prev_val 上一次的输入值 (x[k-1])
 * @param[in]     wc       截止频率 (rad/s)
 * @param[in]     fs       采样频率 (Hz)
 */
#define HIGHPASS(result, val, prev_val, wc, fs)                      \
    do {                                                             \
        if ((wc) == 0.0F)                                            \
            (result) = 0.0F;                                         \
        else {                                                       \
            const float32_t __rc = 1.0F / (wc);                      \
            const float32_t __alpha = 1.0F / (1.0F + (__rc) * (fs)); \
            const float32_t __beta = 1.0F - (__alpha);               \
            (result) = (__beta) * ((result) + (val) - (prev_val));   \
            (prev_val) = (val);                                      \
        }                                                            \
    } while (0)

#define CLAMP(result, min, max)                                    \
    do {                                                           \
        (result) = ((result) <= (min)) ? (min) : MIN(result, max); \
    } while (0)

#define CLAMP_RESULT(val, min, max) ((val) <= (min)) ? (min) : MIN(val, max)

#define SLEW_RATE_LIMIT(val, target, rate, fs)        \
    do {                                              \
        if ((rate) > 0.0F && (fs) > 0.0F) {           \
            const float32_t _step = ABS(rate) / (fs); \
            const float32_t _diff = (target) - (val); \
            if (_diff > _step) {                      \
                (val) += _step;                       \
            } else if (_diff < -_step) {              \
                (val) -= _step;                       \
            } else {                                  \
                (val) = (target);                     \
            }                                         \
        } else {                                      \
            (val) = (target);                         \
        }                                             \
    } while (0)

#define UVW_CLAMP(result, min, max)      \
    do {                                 \
        CLAMP((result).u, (min), (max)); \
        CLAMP((result).v, (min), (max)); \
        CLAMP((result).w, (min), (max)); \
    } while (0)

#define WARP_PI(result, rad)         \
    do {                             \
        float32_t __tmp;             \
        if (ABS(rad) > TAU)          \
            __tmp = MOD((rad), TAU); \
        else                         \
            __tmp = (rad);           \
        if (__tmp > PI)              \
            (result) = __tmp - TAU;  \
        else if (__tmp < -PI)        \
            (result) = __tmp + TAU;  \
        else                         \
            (result) = __tmp;        \
    } while (0)

#define WARP_TAU(result, rad)        \
    do {                             \
        float32_t __tmp;             \
        if (ABS(rad) > TAU)          \
            __tmp = MOD((rad), TAU); \
        else                         \
            __tmp = (rad);           \
        if (__tmp < 0.0F)            \
            (result) = __tmp + TAU;  \
        else                         \
            (result) = __tmp;        \
    } while (0)

#define UVW_ADD_VEC(result, x, y)   \
    do {                            \
        (result).u = (x).u + (y).u; \
        (result).v = (x).v + (y).v; \
        (result).w = (x).w + (y).w; \
    } while (0)

#define UVW_SUB_VEC(result, x, y)   \
    do {                            \
        (result).u = (x).u - (y).u; \
        (result).v = (x).v - (y).v; \
        (result).w = (x).w - (y).w; \
    } while (0)

#define UVW_MUL_VEC(result, x, y)   \
    do {                            \
        (result).u = (x).u * (y).u; \
        (result).v = (x).v * (y).v; \
        (result).w = (x).w * (y).w; \
    } while (0)

#define UVW_DIV_VEC(result, x, y)   \
    do {                            \
        (result).u = (x).u / (y).u; \
        (result).v = (x).v / (y).v; \
        (result).w = (x).w / (y).w; \
    } while (0)

#define UVW_ADD(result, x, y)     \
    do {                          \
        (result).u = (x).u + (y); \
        (result).v = (x).v + (y); \
        (result).w = (x).w + (y); \
    } while (0)

#define UVW_SUB(result, x, y)     \
    do {                          \
        (result).u = (x).u - (y); \
        (result).v = (x).v - (y); \
        (result).w = (x).w - (y); \
    } while (0)

#define UVW_MUL(result, x, y)     \
    do {                          \
        (result).u = (x).u * (y); \
        (result).v = (x).v * (y); \
        (result).w = (x).w * (y); \
    } while (0)

#define UVW_DIVIDER(result, x, y) \
    do {                          \
        (result).u = (x).u / (y); \
        (result).v = (x).v / (y); \
        (result).w = (x).w / (y); \
    } while (0)

#define AB_ADD_VEC(result, x, y)    \
    do {                            \
        (result).a = (x).a + (y).a; \
        (result).b = (x).b + (y).b; \
    } while (0)

#define AB_SUB_VEC(result, x, y)    \
    do {                            \
        (result).a = (x).a - (y).a; \
        (result).b = (x).b - (y).b; \
    } while (0)

#define AB_MUL_VEC(result, x, y)    \
    do {                            \
        (result).a = (x).a * (y).a; \
        (result).b = (x).b * (y).b; \
    } while (0)

#define AB_DIV_VEC(result, x, y)    \
    do {                            \
        (result).a = (x).a / (y).a; \
        (result).b = (x).b / (y).b; \
    } while (0)

/**
 * @brief             比较浮点数是否在容差范围内相等
 * @param[in] x       输入值
 * @param[in] y       第二输入值
 * @param[in] rel_tol 相对误差容限
 * @param[in] abs_tol 绝对误差容限
 * @return            容差范围内相等返回 true, 否则返回 false
 */
HAPI uint8_t is_f32_equal(const float32_t x,
                          const float32_t y,
                          const float32_t rel_tol,
                          const float32_t abs_tol)
{
    // 计算两数的绝对差值
    const float32_t diff = ABS(x - y);

    // 绝对误差检查
    if (diff <= abs_tol) {
        return true;
    }

    // 相对误差检查
    const float32_t abs_x = ABS(x);
    const float32_t abs_y = ABS(y);
    const float32_t max_val = (abs_x > abs_y) ? abs_x : abs_y;

    // 判定差值是否在最大值的相对允许比例范围内
    return diff <= (max_val * rel_tol);
}

/**
 * @brief              查找最大值
 * @param[in]  arr     待排序数组
 * @param[in]  n       位移量或元素数量, 具体含义见函数用途
 * @param[out] max_val 最大值
 * @param[out] max_idx 最大值索引
 */
HAPI void find_max(const float32_t *arr,
                   const size_t n,
                   float32_t *max_val,
                   size_t *max_idx)
{
    if (n == 0) {
        *max_val = 0.0F;
        *max_idx = 0;
        return;
    }

#if defined(__AVX__)
    size_t i = 0;
    __m256 max_vec = _mm256_set1_ps(arr[0]);
    float32_t tmp_max = arr[0];
    size_t idx_max = 0;

    for (; i + 7 < n; i += 8) {
        __m256 v = _mm256_loadu_ps(arr + i);
        max_vec = _mm256_max_ps(max_vec, v);

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
    size_t i = 0;
    __m128 max_vec = _mm_set1_ps(arr[0]);
    float32_t tmp_max = arr[0];
    size_t idx_max = 0;

    for (; i + 3 < n; i += 4) {
        const __m128 v = _mm_loadu_ps(arr + i);
        max_vec = _mm_max_ps(max_vec, v);

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
    size_t i = 0;
    float32x4_t max_vec = vdupq_n_f32(arr[0]);
    float32_t tmp_max = arr[0];
    size_t idx_max = 0;

    for (; i + 3 < n; i += 4) {
        const float32x4_t v = vld1q_f32(arr + i);
        max_vec = vmaxq_f32(max_vec, v);

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
    size_t idx_max = 0;
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
 * @brief          多项式计算
 * @param[in] coef 多项式系数, 升幂排列
 * @param[in] cnt  系数数量(阶数 + 1)
 * @param[in] x    输入值
 * @return         float32_t   输出值
 */
HAPI float32_t poly_eval_rt(const float32_t *coef,
                            const uint32_t cnt,
                            const float32_t x)
{
    if (cnt == 0U) {
        return 0.0F;
    }
    float32_t result = coef[cnt - 1U];
    for (uint32_t i = cnt - 1U; i > 0; i--) {
        result = result * x + coef[i - 1];
    }

    return result;
}

/**
 * @brief         将单精度数据转换为半精度
 * @param[in] val 单精度数据
 * @return        按函数所述变换或计算得到的结果
 */
HAPI uint16_t f32_to_f16(const float32_t val)
{
    const union {
        float32_t f;
        uint32_t i;
    } u = {.f = val};

    const uint32_t sign = u.i >> 16 & 0x8000;
    const uint32_t exponent = u.i >> 23 & 0xFF;
    const uint32_t mantissa = u.i & 0x007FFFFF;

    // 处理零,无穷大,NaN
    if (exponent == 0) {
        return (uint16_t)sign;
    }

    if (exponent == 255) {
        return (uint16_t)(sign | (mantissa == 0 ? 0x7C00 : 0x7E00));
    }

    // 计算 FP16 指数
    const int32_t f16_exp = (int32_t)exponent - 112; // -127 + 15 = -112

    // 处理溢出和下溢
    if (f16_exp >= 31) {
        return (uint16_t)(sign | 0x7C00); // 溢出 -> 无穷大
    }

    if (f16_exp <= 0) {
        return (uint16_t)sign; // 下溢 -> 零
    }

    // 组合结果
    return (uint16_t)(sign | f16_exp << 10 | mantissa >> 13);
}

/**
 * @brief             将半精度数据转换为单精度
 * @param[in] f16_val 半精度数据
 * @return            按函数所述变换或计算得到的结果
 */
HAPI float32_t f16_to_f32(const uint16_t f16_val)
{
    const uint32_t sign = ((uint32_t)f16_val & 0x8000) << 16;
    uint32_t exponent = (f16_val >> 10) & 0x1F;
    uint32_t mantissa = (uint32_t)f16_val & 0x03FF;

    if (exponent == 0) {
        if (mantissa == 0) {
            const union {
                float32_t f;
                uint32_t i;
            } u = {.i = sign};

            return u.f;
        }
        // 非规格化数处理
        exponent = 1;
        while ((mantissa & 0x0400) == 0) {
            mantissa <<= 1;
            exponent--;
        }
        mantissa &= 0x03FF;
    } else if (exponent == 31) {
        const union {
            float32_t f;
            uint32_t i;
        } u = {.i = sign | (mantissa == 0 ? 0x7F800000 : 0x7FC00000)};

        return u.f;
    }

    const uint32_t f32_exp = (exponent + 112) << 23; // 112 = 127 - 15
    const uint32_t f32_mantissa = mantissa << 13;

    const union {
        float32_t f;
        uint32_t i;
    } u = {.i = sign | f32_exp | f32_mantissa};

    return u.f;
}

/**
 * @brief             将浮点数组按升序排序
 * @param[in,out] buf 数据缓冲区
 * @param[in]     len 数组元素数量或缓冲区长度
 */
HAPI void f32_sort_asc(float32_t *buf, size_t len)
{
    if (!buf || len < 2) {
        return;
    }

    for (size_t gap = len >> 1; gap > 0; gap >>= 1) {
        for (size_t i = gap; i < len; i++) {
            float32_t insertion_val = buf[i];
            size_t j;
            for (j = i; j >= gap && buf[j - gap] > insertion_val; j -= gap) {
                buf[j] = buf[j - gap];
            }

            buf[j] = insertion_val;
        }
    }
}

/**
 * @brief             将浮点数组按降序排序
 * @param[in,out] buf 数据缓冲区
 * @param[in]     len 数组元素数量或缓冲区长度
 */
HAPI void f32_sort_desc(float32_t *buf, size_t len)
{
    if (!buf || len < 2) {
        return;
    }

    for (size_t gap = len >> 1; gap > 0; gap >>= 1) {
        for (size_t i = gap; i < len; i++) {
            float32_t insertion_val = buf[i];
            size_t j;
            for (j = i; j >= gap && buf[j - gap] < insertion_val; j -= gap) {
                buf[j] = buf[j - gap];
            }

            buf[j] = insertion_val;
        }
    }
}

#ifdef __cplusplus
}
#endif

#endif
