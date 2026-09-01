#ifndef FASTMATH_H
#define FASTMATH_H

#include <math.h>

#include "macrodef.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define F32_DEG2RAD (0.01745329F)
#define F32_RAD2DEG (57.29578F)

/**
 * @brief              近似计算正弦和余弦
 * @param[in]  x       输入角度, 单位: rad
 * @param[out] sin_out 正弦结果, 指针必须有效
 * @param[out] cos_out 余弦结果, 指针必须有效
 * @note               常用角度使用多项式, 非有限输入或绝对值大于 128 时使用标准函数
 */
HAPI void fast_sincosf_rt(float32_t x, float32_t *sin_out, float32_t *cos_out)
{
    if (!isfinite(x) || fabsf(x) > 128.0F) {
        *sin_out = sinf(x);
        *cos_out = cosf(x);
        return;
    }

    const float32_t inverse_half_pi = 0.6366197724F;
    const int32_t quadrant =
        (int32_t)(x * inverse_half_pi + (x >= 0.0F ? 0.5F : -0.5F));

    // 分段减去半 PI, 降低角度归约的舍入误差
    x = fmaf(-(float32_t)quadrant, 1.5707962512969971F, x);
    x = fmaf(-(float32_t)quadrant, 7.5497894158615964e-8F, x);

    const float32_t x2 = x * x;
    const float32_t sin_x =
        x * (1.0F + x2 * (-0.1666666667F +
                          x2 * (0.0083333333F +
                                x2 * (-0.0001984127F + x2 * 0.0000027557F))));
    const float32_t cos_x =
        1.0F + x2 * (-0.5F + x2 * (0.0416666667F +
                                   x2 * (-0.0013888889F + x2 * 0.0000248016F)));

    switch ((uint32_t)quadrant & 3U) {
    case 0: {
        *sin_out = sin_x;
        *cos_out = cos_x;
        break;
    }
    case 1: {
        *sin_out = cos_x;
        *cos_out = -sin_x;
        break;
    }
    case 2: {
        *sin_out = -sin_x;
        *cos_out = -cos_x;
        break;
    }
    default: {
        *sin_out = -cos_x;
        *cos_out = sin_x;
        break;
    }
    }
}

/**
 * @brief       近似计算正弦
 * @param[in] x 输入角度, 单位: rad
 * @return      正弦值, 异常输入按标准 sinf 处理
 */
HAPI float32_t fast_sinf_rt(const float32_t x)
{
    float32_t sin_val;
    float32_t cos_val;
    fast_sincosf_rt(x, &sin_val, &cos_val);
    return sin_val;
}

/**
 * @brief       近似计算余弦
 * @param[in] x 输入角度, 单位: rad
 * @return      余弦值, 异常输入按标准 cosf 处理
 */
HAPI float32_t fast_cosf_rt(const float32_t x)
{
    float32_t sin_val;
    float32_t cos_val;
    fast_sincosf_rt(x, &sin_val, &cos_val);
    return cos_val;
}

/**
 * @brief       近似计算正切
 * @param[in] x 输入角度, 单位: rad
 * @return      正切值, 极点附近及异常输入按标准 tanf 处理
 */
HAPI float32_t fast_tanf_rt(const float32_t x)
{
    if (!isfinite(x) || fabsf(x) > 128.0F) {
        return tanf(x);
    }
    float32_t sin_val;
    float32_t cos_val;
    fast_sincosf_rt(x, &sin_val, &cos_val);
    if (fabsf(cos_val) < 0.001F) {
        return tanf(x);
    }
    return sin_val / cos_val;
}

/**
 * @brief       近似计算自然指数
 * @param[in] x 指数输入
 * @return      e 的 x 次幂, 溢出及异常输入按标准 expf 处理
 * @note        [-80, 80] 使用归约后的多项式, 其他输入使用标准函数
 */
HAPI float32_t fast_expf_rt(const float32_t x)
{
    if (!isfinite(x) || x < -80.0F || x > 80.0F) {
        return expf(x);
    }

    const int32_t exponent =
        (int32_t)(x * 1.4426950409F + (x >= 0.0F ? 0.5F : -0.5F));
    float32_t reduced = x - (float32_t)exponent * 0.693145751953125F;
    reduced -= (float32_t)exponent * 1.428606765330187e-6F;

    // e^x = 2^exponent * e^reduced, reduced 位于半个 ln(2) 范围内
    const float32_t polynomial =
        1.0F +
        reduced *
            (1.0F +
             reduced *
                 (0.5F +
                  reduced *
                      (0.1666666667F +
                       reduced *
                           (0.0416666667F +
                            reduced * (0.0083333333F +
                                       reduced * (0.0013888889F +
                                                  reduced * 0.0001984127F))))));
    return ldexpf(polynomial, exponent);
}

/**
 * @brief       计算浮点绝对值
 * @param[in] x 输入值
 * @return      计算结果
 */
HAPI float32_t fast_absf_rt(const float32_t x)
{
#ifdef _MSC_VER
    return fabsf(x);
#else
    return __builtin_fabsf(x);
#endif
}

/**
 * @brief       计算平方根
 * @param[in] x 输入值
 * @return      计算结果
 */
HAPI float32_t fast_sqrtf_rt(const float32_t x)
{
    return sqrtf(x);
}

/**
 * @brief       计算浮点余数
 * @param[in] x 输入值
 * @param[in] y 第二输入值
 * @return      计算结果
 */
HAPI float32_t fast_modf_rt(const float32_t x, const float32_t y)
{
    return fmodf(x, y);
}

/**
 * @brief         将单精度数据转换为半精度
 * @param[in] val 输入值
 * @return        按函数所述变换或计算得到的结果
 */
HAPI uint16_t fast_f32_to_f16(const float32_t val)
{
    union {
        float32_t val;
        uint32_t bits;
    } source = {val};

    const uint16_t sign = (uint16_t)((source.bits >> 16U) & 0x8000U);
    const uint32_t exp = (source.bits >> 23U) & 0xFFU;
    uint32_t mant = source.bits & 0x7FFFFFU;

    if (exp == 0xFFU) {
        return (uint16_t)(sign | (mant != 0U ? 0x7E00U : 0x7C00U));
    }

    int half_exp = (int)exp - 127 + 15;
    if (half_exp >= 31) {
        return (uint16_t)(sign | 0x7C00U);
    }

    if (half_exp <= 0) {
        if (half_exp < -10) {
            return sign;
        }

        mant |= 0x800000U;
        const uint32_t shift = (uint32_t)(14 - half_exp);
        uint32_t half_mant = mant >> shift;
        const uint32_t remainder = mant & ((1U << shift) - 1U);
        const uint32_t halfway = 1U << (shift - 1U);
        if (remainder > halfway ||
            (remainder == halfway && (half_mant & 1U) != 0U)) {
            ++half_mant;
        }
        return (uint16_t)(sign | half_mant);
    }

    uint16_t half = (uint16_t)(sign | ((uint16_t)half_exp << 10U) |
                               (uint16_t)(mant >> 13U));
    const uint32_t remainder = mant & 0x1FFFU;
    if (remainder > 0x1000U || (remainder == 0x1000U && (half & 1U) != 0U)) {
        ++half;
    }
    return half;
}

/**
 * @brief         将半精度数据转换为单精度
 * @param[in] val 输入值
 * @return        按函数所述变换或计算得到的结果
 */
HAPI float32_t fast_f16_to_f32(const uint16_t val)
{
    const uint32_t sign = ((uint32_t)val & 0x8000U) << 16U;
    uint32_t exp = ((uint32_t)val >> 10U) & 0x1FU;
    uint32_t mant = (uint32_t)val & 0x3FFU;
    uint32_t bits;

    if (exp == 0U) {
        if (mant == 0U) {
            bits = sign;
        } else {
            int normalized_exp = -14;
            while ((mant & 0x400U) == 0U) {
                mant <<= 1U;
                --normalized_exp;
            }
            mant &= 0x3FFU;
            bits = sign | ((uint32_t)(normalized_exp + 127) << 23U) |
                   (mant << 13U);
        }
    } else if (exp == 0x1FU) {
        bits = sign | 0x7F800000U | (mant << 13U);
    } else {
        bits = sign | ((exp + 112U) << 23U) | (mant << 13U);
    }

    union {
        uint32_t bits;
        float32_t val;
    } dest = {bits};

    return dest.val;
}

#ifdef __cplusplus
}
#endif

#endif
