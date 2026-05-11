#ifndef TYPEDEF_H
#define TYPEDEF_H

#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char u8;
typedef signed char   i8;

typedef unsigned short u16;
typedef signed short   i16;

typedef unsigned int u32;
typedef signed int   i32;

typedef unsigned long long u64;
typedef signed long long   i64;

#if defined(_MSC_VER) && !defined(__clang__)
typedef u16 f16; // MSVC doesn't support _Float16
#else
typedef _Float16 f16;
#endif
typedef float  f32;
typedef double f64;

#ifdef MCU
typedef u32 usize;
typedef i32 isize;
#else
typedef u64 usize;
typedef i64 isize;
#endif

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
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

typedef enum dir {
        DIR_NONE    = 0,
        DIR_FORWARD = 1,
        DIR_REVERSE = -1,
} dir_e;

typedef struct motor_cfg {
        u32 npp;
        f32 ld;
        f32 lq;
        f32 rs;
        f32 psi; // Wb
        f32 j;   // 转子惯量
        f32 vel_rated;
        f32 vel_peak;
        f32 cur_rated;
        f32 cur_peak;
        f32 tor_rated;
        f32 tor_peak;
        f32 cur2tor[4], tor2cur[4];
} motor_cfg_t;

typedef struct periph_cfg {
        /* ADC */
        u32 adc_full_cnt;
        f32 cur_max, vbus_max;
        f32 adc2cur, adc2vbus, adc2volt;
        f32 volt_tau;
        u32 cur_gain;

        /* PWM */
        u32 timer_freq;
        u32 pwm_freq;
        u32 pwm_full_cnt;
        f32 f32_pwm_duty_min;
        f32 f32_pwm_duty_max;
} periph_cfg_t;

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
