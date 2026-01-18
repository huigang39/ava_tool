#ifndef TYPEDEF_H
#define TYPEDEF_H

#include <stdint.h>

#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t u8;
typedef int8_t  i8;

typedef uint16_t u16;
typedef int16_t  i16;

typedef uint32_t u32;
typedef int32_t  i32;

//* on Windows, (unsigned long) is 32bit
typedef unsigned long long u64;
typedef signed long long   i64;

typedef _Float16 f16;
typedef float    f32;
typedef double   f64;

#ifdef MCU
typedef u32 usize;
typedef i32 isize;
#else
typedef u64 usize;
typedef i64 isize;
#endif

typedef struct u32_uvw {
        u32 u;
        u32 v;
        u32 w;
} u32_uvw_t;

typedef struct i32_uvw {
        i32 u;
        i32 v;
        i32 w;
} i32_uvw_t;

typedef struct f32_uvw {
        f32 u;
        f32 v;
        f32 w;
} f32_uvw_t;

typedef struct f32_ab {
        f32 a;
        f32 b;
} f32_ab_t;

typedef struct f32_dq {
        f32 d;
        f32 q;
} f32_dq_t;

typedef struct motor_cfg {
        u32 npp;
        f32 ld;
        f32 lq;
        f32 rs;
        f32 psi; // Wb
        f32 j;   // 转子惯量
        f32 vel_max, cur_rated, cur_max, tor_rated, tor_max;
        f32 cur2tor[4], tor2cur[4];
} motor_cfg_t;

HAPI void
void_null_func(void)
{
}

HAPI f32
f32_null_func(void)
{
        return 0.0f;
}

HAPI int
i32_null_func(void)
{
        return 0;
}

#ifdef __cplusplus
}
#endif

#endif // !TYPEDEF_H
