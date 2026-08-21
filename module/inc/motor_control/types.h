#ifndef MOTOR_CONTROL_TYPES_H
#define MOTOR_CONTROL_TYPES_H
#include <stddef.h>
#include <stdint.h>

#include "macrodef.h"

struct u32_uvw {
    uint32_t u;
    uint32_t v;
    uint32_t w;
};

struct i32_uvw {
    int32_t u;
    int32_t v;
    int32_t w;
};

struct f32_uvw {
    float32_t u;
    float32_t v;
    float32_t w;
};

struct f32_ab {
    float32_t a;
    float32_t b;
};

struct f32_dq {
    float32_t d;
    float32_t q;
};

enum dir {
    DIR_NONE    = 0,
    DIR_FORWARD = 1,
    DIR_REVERSE = -1,
};

struct motor_cfg {
    uint32_t  npp;       // 极对数
    float32_t ld;        // d 轴电感 (H)
    float32_t lq;        // q 轴电感 (H)
    float32_t rs;        // 定子相电阻 (Ω)
    float32_t psi;       // 转子磁链 (Wb)
    float32_t j;         // 转子转动惯量 (kg·m²)
    float32_t vel_rated; // 额定转速 (rad/s)
    float32_t vel_peak;  // 峰值转速 (rad/s)
    float32_t cur_rated; // 额定电流 (A)
    float32_t cur_peak;  // 峰值电流 (A)
    float32_t tor_rated; // 额定扭矩 (N·m)
    float32_t tor_peak;  // 峰值扭矩 (N·m)
};

struct reducer_cfg {
    float32_t outshaft_ratio; // 减速比
};

// 单路 ADC 采样来源
struct adc_src {
    void   *adc;  // 所属 ADC 句柄 (HAL 句柄, NULL=未使用)
    uint8_t rank; // 转换序列中的 Rank
};

enum foc_sensor {
    FOC_SENSOR_NONE,
    FOC_SENSOR_ELEC, // 电角度编码器
    FOC_SENSOR_MECH, // 机械角度编码器
};

struct periph_cfg {
    /* ADC */
    uint32_t  adc_full_cnt;       // ADC 满量程计数值
    float32_t cur_max;            // 电流采样量程 (A)
    float32_t vbus_max;           // 母线电压采样量程 (V)
    float32_t power_off_vbus_min; // 掉电检测阈值 (V), 用于 ADC 看门狗及软件欠压判断
    float32_t v_supply;           // NTC 分压电路供电电压 (V)
    struct {                      // NTC 分压电阻 (Ω)
        float32_t inverter;       // 逆变器 NTC 分压电阻
        float32_t stator_uv;      // 定子 UV 相 NTC 分压电阻
        float32_t stator_vw;      // 定子 VW 相 NTC 分压电阻
    } ntc_r_divider;

    struct {                          // ADC 采样来源映射 (换板只改此处)
        struct adc_src i_uvw[3];      // 三相电流 U/V/W
        struct adc_src v_uvw[3];      // 三相端电压 U/V/W
        struct adc_src v_bus;         // 母线电压
        struct adc_src inverter_ntc;  // 逆变器 NTC,规则组
        struct adc_src stator_ntc_uv; // 绕组 UV 相 NTC,规则组
        struct adc_src stator_ntc_vw; // 绕组 VW 相 NTC,规则组
        struct adc_src mcu_temp;      // MCU 内部温度,规则组
        struct adc_src vrefint;       // MCU 内部参考电压,规则组
    } adc_map;

    float32_t adc2cur;  // ADC 原始值 -> 电流 (A) 系数
    float32_t adc2vbus; // ADC 原始值 -> 母线电压 (V) 系数
    float32_t adc2volt; // ADC 原始值 -> 端电压 (V) 系数
    float32_t volt_tau; // 电压采样一阶滤波时间常数 (s)

    /* DRV */
    uint32_t drv_type;
    uint32_t cur_gain;   // 电流采样运放增益档位
    uint32_t idriven_hs; // 上桥拉低驱动电流档位
    uint32_t idrivep_hs; // 上桥拉高驱动电流档位
    uint32_t idriven_ls; // 下桥拉低驱动电流档位
    uint32_t idrivep_ls; // 下桥拉高驱动电流档位

    /* PWM */
    uint32_t  timer_freq;       // 定时器时钟频率 (Hz)
    uint32_t  pwm_freq;         // PWM 载波频率 (Hz)
    uint32_t  pwm_full_cnt;     // PWM 计数器周期值
    float32_t f32_pwm_duty_min; // PWM 占空比下限
    float32_t f32_pwm_duty_max; // PWM 占空比上限

    /* 传感器串口 */
    uint32_t theta_uart_baudrate; // 角度传感器串口波特率
    uint32_t tor_uart_baudrate;   // 转矩传感器串口波特率

    /* 外设句柄 (ADC 句柄见 adc_map) */
    struct {
        void *pwm;            // HRTIM 句柄
        void *timer;          // LPTIM 句柄 (时间戳)
        void *drv_spi;        // 栅极驱动 SPI 句柄
        void *motor_theta;    // 电机轴角度传感器外设句柄
        void *outshaft_theta; // 出轴角度传感器外设句柄
        void *tor_uart;       // 转矩传感器串口句柄
        void *log_uart;       // 日志串口句柄
        struct {
            void    *port; // 驱动使能 GPIO 端口 (GPIO_TypeDef *)
            uint16_t pin;  // 驱动使能 GPIO 引脚
        } drv_en;
    } handle;
};

#endif // !MOTOR_CONTROL_TYPES_H
