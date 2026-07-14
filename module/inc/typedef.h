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

#if COMPILER(MSVC) && !COMPILER(CLANG)
typedef u16 f16; // MSVC doesn't support _Float16
#else
typedef _Float16 f16;
#endif
typedef float  f32;
typedef double f64;

#if OS(NONE)
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
        u32 npp;       // 极对数
        f32 ld;        // d 轴电感 (H)
        f32 lq;        // q 轴电感 (H)
        f32 rs;        // 定子相电阻 (Ω)
        f32 psi;       // 转子磁链 (Wb)
        f32 j;         // 转子转动惯量 (kg·m²)
        f32 vel_rated; // 额定转速 (rad/s)
        f32 vel_peak;  // 峰值转速 (rad/s)
        f32 cur_rated; // 额定电流 (A)
        f32 cur_peak;  // 峰值电流 (A)
        f32 tor_rated; // 额定扭矩 (N·m)
        f32 tor_peak;  // 峰值扭矩 (N·m)
} motor_cfg_t;

typedef struct reducer_cfg {
        f32 outshaft_ratio; // 减速比
} reducer_cfg_t;

// 单路注入 ADC 采样来源
typedef struct adc_src {
        void *adc;  // 所属 ADC 句柄 (HAL 句柄, NULL=未使用)
        u8    rank; // 注入通道序号: 1=JDR1, 2=JDR2, 3=JDR3, 4=JDR4
} adc_src_t;

typedef enum foc_sensor {
        FOC_SENSOR_NONE,
        FOC_SENSOR_ELEC, // 电角度编码器
        FOC_SENSOR_MECH, // 机械角度编码器
} foc_sensor_e;

// 故障标志位 (g_check 故障状态, 同时复用为 foc_base_cfg_t.check_fault_enable 检测使能位)
typedef union check_fault {
        u32 val;
        struct {
                u32 cur_offset           : 1;
                u32 elec_theta_offset    : 1;
                u32 comm_shm             : 1;
                u32 param_sync           : 1;
                u32 under_vbus           : 1;
                u32 over_vbus            : 1;
                u32 over_cur             : 1;
                u32 over_coil_temp       : 1;
                u32 over_inverter_temp   : 1;
                u32 over_load            : 1;
                u32 loss_motor_sensor    : 1;
                u32 loss_outshaft_sensor : 1;
                u32 loss_phase           : 1;
                u32 sensor_cali          : 1;
        } bit;
} check_fault_u;

typedef struct periph_cfg {
        /* ADC */
        u32 adc_full_cnt;       // ADC 满量程计数值
        f32 cur_max;            // 电流采样量程 (A)
        f32 vbus_max;           // 母线电压采样量程 (V)
        f32 power_off_vbus_min; // 掉电检测阈值 (V), 用于 ADC 看门狗及软件欠压判断
        struct {                // NTC 分压电阻 (Ω)
                f32 inverter;   // 逆变器 NTC 分压电阻
                f32 stator_uv;  // 定子 UV 相 NTC 分压电阻
                f32 stator_vw;  // 定子 VW 相 NTC 分压电阻
        } ntc_r_divider;

        struct {                         // ADC 采样来源映射, 与 foc_adc_raw_t 一一对应 (换板只改此处)
                adc_src_t i_uvw[3];      // 三相电流 U/V/W
                adc_src_t v_uvw[3];      // 三相端电压 U/V/W
                adc_src_t v_bus;         // 母线电压
                adc_src_t inverter_ntc;  // 逆变器 NTC
                adc_src_t stator_ntc_uv; // 绕组 UV 相 NTC
                adc_src_t stator_ntc_vw; // 绕组 VW 相 NTC
        } adc_inject_map;

        f32 adc2cur;  // ADC 原始值 -> 电流 (A) 系数
        f32 adc2vbus; // ADC 原始值 -> 母线电压 (V) 系数
        f32 adc2volt; // ADC 原始值 -> 端电压 (V) 系数
        f32 volt_tau; // 电压采样一阶滤波时间常数 (s)

        /* DRV */
        u32 drv_type;
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

        /* 传感器和串口 */
        foc_sensor_e e_motor_sensor;      // 电机轴编码器类型
        u32          theta_uart_baudrate; // 角度传感器串口波特率
        u32          tor_uart_baudrate;   // 转矩传感器串口波特率

        /* 外设句柄 (ADC 句柄见 adc_inject_map) */
        struct {
                void *pwm;        // HRTIM 句柄
                void *timer;      // LPTIM 句柄 (时间戳)
                void *drv_spi;    // 栅极驱动 SPI 句柄
                void *theta_uart; // 角度传感器串口句柄
                void *tor_uart;   // 转矩传感器串口句柄
                void *log_uart;   // 日志串口句柄
                struct {
                        void *port; // 驱动使能 GPIO 端口 (GPIO_TypeDef *)
                        u16   pin;  // 驱动使能 GPIO 引脚
                } drv_en;
        } handle;
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
