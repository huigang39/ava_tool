#ifndef MATHDEF_H
#define MATHDEF_H

#ifdef ARM_MATH
#include "arm_math.h"
#endif

#if defined(__linux__) || defined(_WIN32)
#include <immintrin.h>
#endif

#include <math.h>

#include "fastmath.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef FAST_MATH
#define SIN(x)       (fast_sinf(x))
#define COS(x)       (fast_cosf(x))
#define TAN(x)       (fast_tanf(x))
#define EXP(x)       (fast_expf(x))
#define ABS(x)       (fast_absf(x))
#define SQRT(x)      (fast_sqrtf(x))
#define MOD(x, y)    (fast_modf(x, y))
#define CPYSGN(x, y) (copysignf(x, y))
#define LOG(x)       (logf(x))
#elif defined(ARM_MATH)
#define SIN(x)       (arm_sin_f32(x))
#define COS(x)       (arm_cos_f32(x))
#define ATAN2(y, x)  (atan2f(y, x))
#define ABS(x)       (fabsf(x))
#define EXP(x)       (expf(x))
#define SQRT(x)      (sqrtf(x))
#define MOD(x, y)    (fmodf(x, y))
#define CPYSGN(x, y) (copysignf(x, y))
#define LOG(x)       (logf(x))
#else
#define SIN(x)       (sinf(x))
#define SINH(x)      (sinhf(x))
#define COS(x)       (cosf(x))
#define EXP(x)       (expf(x))
#define ATAN2(y, x)  (atan2f(y, x))
#define ABS(x)       (fabsf(x))
#define SQRT(x)      (sqrtf(x))
#define MOD(x, y)    (fmodf(x, y))
#define CPYSGN(x, y) (copysignf(x, y))
#define LOG(x)       (logf(x))
#endif

#ifndef PI
#define PI (3.1415926F)
#endif

#ifndef E
#define E (2.7182818F)
#endif

#define TAU             (6.2831853F)
#define DIV_PI_BY_2     (1.5707963F)
#define LN2             (0.6931471F)
#define DIV_2_BY_3      (0.6666666F)
#define SQRT_2          (1.4142135F)
#define SQRT_3          (1.7320508F)
#define DIV_1_BY_SQRT_3 (0.5773502F)
#define DIV_SQRT_3_BY_2 (0.8660254F)

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif

#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif

#define IS_NAN(x)                   (isnan(x))
#define IS_INF(x)                   (isinf(x))
#define IS_IN_RANGE(val, min, max)  ((val) >= (min) && (val) <= (max))
#define IS_EQ(x, y)                 (ABS((x) - (y)) <= (1E-6F * MAX(ABS(x), ABS(y)) + 1E-6F))

#define SGN(x)                      ((x) == 0.0F ? 0.0F : (x) > 0.0F ? 1.0F : -1.0F)

#define SQ(x)                       ((x) * (x))        // 平方
#define K(x)                        ((x) * 1000)       // 乘以 1K
#define M(x)                        ((x) * 1000000)    // 乘以 1M
#define G(x)                        ((x) * 1000000000) // 乘以 1G

#define RAD2DEG(rad)                ((rad) * 57.2957795F)
#define DEG2RAD(deg)                ((deg) / 57.2957795F)

#define RPM2RADS(rpm)               (((rpm) / 60.0F) * TAU)
#define RADS2RPM(rads)              (((rads) * 60.0F) / TAU)

#define HZ2RADS(hz)                 ((hz) * TAU)
#define RADS2HZ(rads)               ((rads) / TAU)

#define MECH2ELEC(theta, npp)       ((theta) * (npp))
#define ELEC2MECH(theta, npp)       ((theta) / (npp))
#define OUTSHAFT2MECH(theta, ratio) ((theta) * (ratio))
#define MECH2OUTSHAFT(theta, ratio) ((theta) / (ratio))

#define RAW2THETA(cnt, full_cnt)    ((f32)(cnt) / (f32)full_cnt * TAU)

#define TOGGLE_THETA(dir, theta)    ((dir) == 1 ? (theta) : (TAU) - (theta))

#define CYCLE_CNT(cnt, theta, prev_theta)                 \
        do {                                              \
                if ((cnt) == 0 && (prev_theta) == 0.0F) { \
                        (prev_theta) = (theta);           \
                        break;                            \
                }                                         \
                f32 theta_err = (theta) - (prev_theta);   \
                if (theta_err < -PI)                      \
                        (cnt)++;                          \
                else if (theta_err > PI)                  \
                        (cnt)--;                          \
                (prev_theta) = (theta);                   \
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

#define THETA_DERIVATIVE(ret, theta, prev_theta, gain, fs)  \
        do {                                                \
                f32 theta_err = (theta) - (prev_theta);     \
                WARP_PI(theta_err, theta_err);              \
                (ret)        = (gain) * (theta_err) * (fs); \
                (prev_theta) = (theta);                     \
        } while (0)

#define LOWPASS(ret, val, wc, fs)                                     \
        do {                                                          \
                f32 rc    = 1.0F / (wc);                              \
                f32 alpha = 1.0F / (1.0F + (rc) * (fs));              \
                (ret)     = (alpha) * (val) + (1.0F - alpha) * (ret); \
        } while (0)

#define CLAMP(ret, min, max)                                      \
        do {                                                      \
                (ret) = ((ret) <= (min)) ? (min) : MIN(ret, max); \
        } while (0)

#define UVW_CLAMP(ret, min, max)              \
        do {                                  \
                CLAMP((ret).u, (min), (max)); \
                CLAMP((ret).v, (min), (max)); \
                CLAMP((ret).w, (min), (max)); \
        } while (0)

#define WARP_PI(ret, rad)                      \
        do {                                   \
                f32 tmp;                       \
                if (ABS(rad) > TAU)            \
                        tmp = MOD((rad), TAU); \
                else                           \
                        tmp = (rad);           \
                if (tmp > PI)                  \
                        (ret) = tmp - TAU;     \
                else if (tmp < -PI)            \
                        (ret) = tmp + TAU;     \
                else                           \
                        (ret) = tmp;           \
        } while (0)

#define WARP_TAU(ret, rad)                     \
        do {                                   \
                f32 tmp;                       \
                if (ABS(rad) > TAU)            \
                        tmp = MOD((rad), TAU); \
                else                           \
                        tmp = (rad);           \
                if (tmp < 0.0F)                \
                        (ret) = tmp + TAU;     \
                else                           \
                        (ret) = tmp;           \
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

#define UVW_ADD(ret, x, y)             \
        do {                           \
                (ret).u = (x).u + (y); \
                (ret).v = (x).v + (y); \
                (ret).w = (x).w + (y); \
        } while (0)

#define UVW_MUL(ret, x, y)             \
        do {                           \
                (ret).u = (x).u * (y); \
                (ret).v = (x).v * (y); \
                (ret).w = (x).w * (y); \
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

HAPI f32
poly_eval(const f32 *coeffs, const u32 order, const f32 x)
{
        f32 res = coeffs[0];
        for (u32 i = 1; i <= order; i++)
                res = res * x + coeffs[i];

        return res;
}

HAPI u16
f32_to_f16(const f32 f32_val)
{
        const union {
                f32 f;
                u32 i;
        } u = {.f = f32_val};

        const u32 sign     = (u.i >> 16) & 0x8000;
        const u32 exponent = (u.i >> 23) & 0xFF;
        const u32 mantissa = u.i & 0x007FFFFF;

        // 处理零、无穷大、NaN
        if (exponent == 0)
                return (u16)sign;

        if (exponent == 255)
                return (u16)(sign | ((mantissa == 0) ? 0x7C00 : 0x7E00));

        // 计算FP16指数
        i32 f16_exp = (i32)exponent - 112; // -127 + 15 = -112

        // 处理溢出和下溢
        if (f16_exp >= 31)
                return (u16)(sign | 0x7C00); // 溢出 -> 无穷大

        if (f16_exp <= 0)
                return (u16)sign; // 下溢 -> 零

        // 组合结果
        return (u16)(sign | (f16_exp << 10) | (mantissa >> 13));
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
                // 非规格化数处理
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
                } u = {.i = sign | ((mantissa == 0) ? 0x7F800000 : 0x7FC00000)};
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

#ifdef __cplusplus
}
#endif

#endif // !MATHDEF_H
