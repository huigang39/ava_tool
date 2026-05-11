#ifndef FASTMATH_H
#define FASTMATH_H

#include <math.h>

#include "macrodef.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

#define F32_DEG2RAD (0.01745329f)
#define F32_RAD2DEG (57.29578f)

static const f32 SIN_TABLE[] = {
    0.00000000f, // sin(0)
    0.17364818f, // sin(10)
    0.34202014f, // sin(20)
    0.50000000f, // sin(30)
    0.64278761f, // sin(40)
    0.76604444f, // sin(50)
    0.86602540f, // sin(60)
    0.93969262f, // sin(70)
    0.98480775f, // sin(80)
    1.00000000f  // sin(90)
};

static const f32 COS_TABLE[] = {
    1.00000000f, // cos(0)
    0.99984770f, // cos(1)
    0.99939083f, // cos(2)
    0.99862953f, // cos(3)
    0.99756405f, // cos(4)
    0.99619470f, // cos(5)
    0.99452190f, // cos(6)
    0.99254615f, // cos(7)
    0.99026807f, // cos(8)
    0.98768834f  // cos(9)
};

HAPI f32
fast_sinf(f32 x)
{
        x = x * F32_RAD2DEG;

        /* 快速归一化到 [0, 360) */
        x = x - 360.0f * (i32)(x * (1.0f / 360.0f));
        if (x < 0.0f) {
                x += 360.0f;
        }

        i32 sig = 0;
        if (x >= 180.0f) {
                sig = 1;
                x   = x - 180.0f;
        }

        x = (x > 90.0f) ? (180.0f - x) : x;

        const i32 a = (i32)(x * 0.1f);
        const f32 b = x - 10.0f * a;

        const f32 y = SIN_TABLE[a] * COS_TABLE[(i32)b] + b * F32_DEG2RAD * SIN_TABLE[9 - a];
        return (sig > 0) ? -y : y;
}

HAPI f32
fast_cosf(const f32 x)
{
        return fast_sinf(x + 1.5707964f);
}

HAPI f32
fast_tanf(const f32 x)
{
        return fast_sinf(x) / fast_cosf(x);
}

HAPI f32
fast_expf(f32 x)
{
        /* 限制输入范围防止位模式溢出 */
        if (x > 88.0f)
                return 3.4028235e38f;
        if (x < -87.0f)
                return 0.0f;

        union {
                u32 i;
                f32 f;
        } v;
        v.i = (u32)((1U << 23) * (1.442695f * x + 126.9349f));
        return v.f;
}

HAPI f32
fast_absf(const f32 x)
{
#ifdef _MSC_VER
        return fabsf(x);
#else
        return __builtin_fabsf(x);
#endif
}

HAPI f32
fast_sqrtf(const f32 x)
{
        return sqrtf(x);
}

HAPI f32
fast_modf(const f32 x, const f32 y)
{
        return fmodf(x, y);
}

HAPI f16
fast_f32_to_f16(const f32 f32_val)
{
        return (f16)f32_val;
}

HAPI f32
fast_f16_to_f32(const f16 f16_val)
{
        return (f32)f16_val;
}

#ifdef __cplusplus
}
#endif

#endif // !FASTMATH_H
