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

static const float32_t SIN_TABLE[] = {
    0.00000000F, // sin(0)
    0.17364818F, // sin(10)
    0.34202014F, // sin(20)
    0.50000000F, // sin(30)
    0.64278761F, // sin(40)
    0.76604444F, // sin(50)
    0.86602540F, // sin(60)
    0.93969262F, // sin(70)
    0.98480775F, // sin(80)
    1.00000000F  // sin(90)
};

static const float32_t COS_TABLE[] = {
    1.00000000F, // cos(0)
    0.99984770F, // cos(1)
    0.99939083F, // cos(2)
    0.99862953F, // cos(3)
    0.99756405F, // cos(4)
    0.99619470F, // cos(5)
    0.99452190F, // cos(6)
    0.99254615F, // cos(7)
    0.99026807F, // cos(8)
    0.98768834F  // cos(9)
};

HAPI float32_t
fast_sinf_rt(float32_t x)
{
    x = x * F32_RAD2DEG;

    /* 快速归一化到 [0, 360) */
    x = x - 360.0F * (int32_t)(x * (1.0F / 360.0F));
    if (x < 0.0F) {
        x += 360.0F;
    }

    int32_t sig = 0;
    if (x >= 180.0F) {
        sig = 1;
        x   = x - 180.0F;
    }

    x = (x > 90.0F) ? (180.0F - x) : x;

    const int32_t   a = (int32_t)(x * 0.1F);
    const float32_t b = x - 10.0F * a;

    const float32_t y = SIN_TABLE[a] * COS_TABLE[(int32_t)b] + b * F32_DEG2RAD * SIN_TABLE[9 - a];
    return (sig > 0) ? -y : y;
}

HAPI float32_t
fast_cosf_rt(const float32_t x)
{
    return fast_sinf_rt(x + 1.5707964F);
}

HAPI void
fast_sincosf_rt(float32_t x, float32_t *sin_out, float32_t *cos_out)
{
    const float32_t half_pi     = 1.5707963268F;
    const float32_t inv_half_pi = 0.6366197724F;

    int32_t quadrant  = (int32_t)(x * inv_half_pi + (x >= 0.0F ? 0.5F : -0.5F));
    x                -= (float32_t)quadrant * half_pi;

    const float32_t x2 = x * x;
    const float32_t sin_x =
        x * (1.0F + x2 * (-0.1666666667F + x2 * (0.0083333333F - x2 * 0.0001984127F)));
    const float32_t cos_x = 1.0F + x2 * (-0.5F + x2 * (0.0416666667F - x2 * 0.0013888889F));

    switch ((uint32_t)quadrant & 3U) {
        case 0:
            *sin_out = sin_x;
            *cos_out = cos_x;
            break;
        case 1:
            *sin_out = cos_x;
            *cos_out = -sin_x;
            break;
        case 2:
            *sin_out = -sin_x;
            *cos_out = -cos_x;
            break;
        default:
            *sin_out = -cos_x;
            *cos_out = sin_x;
            break;
    }
}

HAPI float32_t
fast_tanf_rt(const float32_t x)
{
    return fast_sinf_rt(x) / fast_cosf_rt(x);
}

HAPI float32_t
fast_expf_rt(float32_t x)
{
    /* 限制输入范围防止位模式溢出 */
    if (x > 88.0F)
        return 3.4028235e38F;
    if (x < -87.0F)
        return 0.0F;

    union {
        uint32_t  i;
        float32_t f;
    } v;
    v.i = (uint32_t)((1U << 23) * (1.442695F * x + 126.9349F));
    return v.f;
}

HAPI float32_t
fast_absf_rt(const float32_t x)
{
#ifdef _MSC_VER
    return fabsf(x);
#else
    return __builtin_fabsf(x);
#endif
}

HAPI float32_t
fast_sqrtf_rt(const float32_t x)
{
    return sqrtf(x);
}

HAPI float32_t
fast_modf_rt(const float32_t x, const float32_t y)
{
    return fmodf(x, y);
}

/* IEEE-754 binary16 storage. uint16_t is used because binary16 is not a
 * standard C scalar type
 * and is unavailable on some supported compilers. */
HAPI uint16_t
fast_f32_to_f16(const float32_t value)
{
    union {
        float32_t value;
        uint32_t  bits;
    } src               = {value};
    const uint16_t sign = (uint16_t)((src.bits >> 16U) & 0x8000U);
    const uint32_t exp  = (src.bits >> 23U) & 0xFFU;
    uint32_t       mant = src.bits & 0x7FFFFFU;

    if (exp == 0xFFU)
        return (uint16_t)(sign | (mant != 0U ? 0x7E00U : 0x7C00U));

    int half_exp = (int)exp - 127 + 15;
    if (half_exp >= 31)
        return (uint16_t)(sign | 0x7C00U);

    if (half_exp <= 0) {
        if (half_exp < -10)
            return sign;

        mant                     |= 0x800000U;
        const uint32_t shift      = (uint32_t)(14 - half_exp);
        uint32_t       half_mant  = mant >> shift;
        const uint32_t remainder  = mant & ((1U << shift) - 1U);
        const uint32_t halfway    = 1U << (shift - 1U);
        if (remainder > halfway || (remainder == halfway && (half_mant & 1U) != 0U))
            ++half_mant;
        return (uint16_t)(sign | half_mant);
    }

    uint16_t       half = (uint16_t)(sign | ((uint16_t)half_exp << 10U) | (uint16_t)(mant >> 13U));
    const uint32_t remainder = mant & 0x1FFFU;
    if (remainder > 0x1000U || (remainder == 0x1000U && (half & 1U) != 0U))
        ++half;
    return half;
}

HAPI float32_t
fast_f16_to_f32(const uint16_t value)
{
    const uint32_t sign = ((uint32_t)value & 0x8000U) << 16U;
    uint32_t       exp  = ((uint32_t)value >> 10U) & 0x1FU;
    uint32_t       mant = (uint32_t)value & 0x3FFU;
    uint32_t       bits;

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
            bits  = sign | ((uint32_t)(normalized_exp + 127) << 23U) | (mant << 13U);
        }
    } else if (exp == 0x1FU) {
        bits = sign | 0x7F800000U | (mant << 13U);
    } else {
        bits = sign | ((exp + 112U) << 23U) | (mant << 13U);
    }

    union {
        uint32_t  bits;
        float32_t value;
    } dst = {bits};
    return dst.value;
}

#ifdef __cplusplus
}
#endif

#endif // !FASTMATH_H
