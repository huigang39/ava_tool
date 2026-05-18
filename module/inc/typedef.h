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
        u32 npp;        // 极对数
        f32 ld;         // d 轴电感 (H)
        f32 lq;         // q 轴电感 (H)
        f32 rs;         // 定子相电阻 (Ω)
        f32 psi;        // 转子磁链 (Wb)
        f32 j;          // 转子转动惯量 (kg·m²)
        f32 vel_rated;  // 额定转速 (rad/s)
        f32 vel_peak;   // 峰值转速 (rad/s)
        f32 cur_rated;  // 额定电流 (A)
        f32 cur_peak;   // 峰值电流 (A)
        f32 tor_rated;  // 额定扭矩 (N·m)
        f32 tor_peak;   // 峰值扭矩 (N·m)
        f32 cur2tor[4]; // 电流->扭矩 多项式拟合系数
        f32 tor2cur[4]; // 扭矩->电流 多项式拟合系数
} motor_cfg_t;

typedef struct periph_cfg {
        /* ADC */
        u32 adc_full_cnt;      // ADC 满量程计数值
        f32 cur_max;           // 电流采样量程 (A)
        f32 vbus_max;          // 母线电压采样量程 (V)
        struct {               // NTC 分压电阻 (Ω)
                f32 inverter;  // 逆变器 NTC 分压电阻
                f32 stator_uv; // 定子 UV 相 NTC 分压电阻
                f32 stator_vw; // 定子 VW 相 NTC 分压电阻
        } ntc_r_divider;

        f32 adc2cur;  // ADC 原始值 -> 电流 (A) 系数
        f32 adc2vbus; // ADC 原始值 -> 母线电压 (V) 系数
        f32 adc2volt; // ADC 原始值 -> 通用电压 (V) 系数
        f32 volt_tau; // 电压采样一阶滤波时间常数 (s)

        /* DRV */
        u32 cur_gain;   // 电流采样运放增益档位
        u32 idriven_hs; // 上桥拉低驱动电流档位
        u32 idrivep_hs; // 上桥拉高驱动电流档位
        u32 idriven_ls; // 下桥拉低驱动电流档位
        u32 idrivep_ls; // 下桥拉高驱动电流档位

        /* PWM */
        u32 timer_freq;       // 定时器时钟频率 (Hz)
        u32 pwm_freq;         // PWM 载波频率 (Hz)
        u32 pwm_full_cnt;     // PWM 计数器周期值
        f32 f32_pwm_duty_min; // PWM 占空比下限
        f32 f32_pwm_duty_max; // PWM 占空比上限
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
