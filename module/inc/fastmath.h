#ifndef FASTMATH_H
#define FASTMATH_H

#include <math.h>

#include "bitops.h"
#include "macrodef.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

#define f32_HOLLYST (0.017453292519943295769236907684886F)

static const f32 SIN_TABLE[] = {
    0.0f,                                // sin(0)
    0.17364817766693034885171662676931f, // sin(10)
    0.34202014332566873304409961468226f, // sin(20)
    0.5f,                                // sin(30)
    0.64278760968653932632264340990726f, // sin(40)
    0.76604444311897803520239265055542f, // sin(50)
    0.86602540378443864676372317075294f, // sin(60)
    0.93969262078590838405410927732473f, // sin(70)
    0.98480775301220805936674302458952f, // sin(80)
    1.0f                                 // sin(90)
};

static const f32 COS_TABLE[] = {
    1.0f,                                // cos(0)
    0.99984769515639123915701155881391f, // cos(10)
    0.99939082701909573000624344004393f, // cos(20)
    0.99862953475457387378449205843944f, // cos(30)
    0.99756405025982424761316268064426f, // cos(40)
    0.99619469809174553229501040247389f, // cos(50)
    0.99452189536827333692269194498057f, // cos(60)
    0.99254615164132203498006158933058f, // cos(70)
    0.99026806874157031508377486734485f, // cos(80)
    0.98768834059513772619004024769344f  // cos(90)
};

HAPI f32
fast_sinf(f32 x)
{
        x = x * 57.295779513082320876798154814105f;

        i32 sig = 0;
        if (x > 0.0f) {
                while (x >= 360.0f)
                        x = x - 360.0f;
        } else {
                while (x < 0.0f)
                        x = x + 360.0f;
        }

        if (x >= 180.0f) {
                sig = 1u;
                x   = x - 180.0f;
        }

        x = (x > 90.0f) ? (180.0f - x) : x;

        const i32 a = x * 0.1f;
        const f32 b = x - 10.0f * a;

        const f32 y = SIN_TABLE[a] * COS_TABLE[(i32)b] + b * f32_HOLLYST * SIN_TABLE[9u - a];
        return (sig > 0) ? -y : y;
}

HAPI f32
fast_cosf(const f32 x)
{
        return fast_sinf(x + 1.5707963267948966192313216916398f);
}

HAPI f32
fast_tanf(const f32 x)
{
        return fast_sinf(x) / fast_cosf(x);
}

HAPI f32
fast_expf(const f32 x)
{
        union {
                u32 i;
                f32 f;
        } v;
        v.i = (BIT(23)) * (1.4426950409f * x + 126.93490512f);
        return v.f;
}

HAPI f32
fast_absf(const f32 x)
{
        f32 y = x;
        if (x < 0.0f)
                y = -x;
        return y;
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
