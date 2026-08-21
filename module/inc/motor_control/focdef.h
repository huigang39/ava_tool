#ifndef FOCDEF_H
#define FOCDEF_H

#include "mathdef.h"
#include "pll.h"
#include "rls.h"
#include "types.h"

#include "controller/adrc.h"
#include "controller/pid.h"

#include "observer/flux.h"
#include "observer/hfi.h"
#include "observer/luenberger.h"
#include "observer/smo.h"

#ifdef __cplusplus
extern "C" {
#endif

struct foc_adc {
    struct {
        struct i32_uvw i_uvw; // 三相电流 ADC 原始值
        struct i32_uvw v_uvw; // 三相电压 ADC 原始值
        int32_t        v_bus; // 母线电压 ADC 原始值
        int32_t        inverter_ntc;
        struct {
            int32_t uv;
            int32_t vw;
        } stator_ntc;
        int32_t mcu_temp;
        int32_t vrefint;
    } raw;

    struct {
        float32_t      vdda;  // ADC 参考电压 (V)
        struct f32_uvw i_uvw; // 三相电流采样引脚电压 (V)
        struct f32_uvw v_uvw; // 三相端电压采样引脚电压 (V)
        float32_t      v_bus; // 母线电压采样引脚电压 (V)
        float32_t      inverter_ntc;
        struct {
            float32_t uv;
            float32_t vw;
        } stator_ntc;
        float32_t mcu_temp;
        float32_t vrefint;
    } volt;
};

struct foc_svpwm {
    float32_t      v_max;            // 三相最大电压
    float32_t      v_min;            // 三相最小电压
    float32_t      v_avg;            // 三相最大最小之和的均值电压
    struct f32_uvw f32_pwm_duty_raw; // PWM 占空比原始值 (百分比) (未中点平移)
    struct f32_uvw f32_pwm_duty;     // PWM 占空比 (百分比)
    struct u32_uvw u32_pwm_duty;     // PWM 占空比计数值
};

enum foc_pwm_ch {
    PWM_CH_UH,
    PWM_CH_UL,

    PWM_CH_VH,
    PWM_CH_VL,

    PWM_CH_WH,
    PWM_CH_WL,

    PWM_CH_H,
    PWM_CH_L,

    PWM_CH_ALL,
};

enum foc_theta {
    FOC_ELEC_THETA_NONE,
    FOC_ELEC_THETA_FORCE,        // 强拖角度
    FOC_ELEC_THETA_SENSOR,       // 有感角度
    FOC_ELEC_THETA_SENSORLESS,   // 无感角度
    FOC_ELEC_THETA_SENSORFUSION, // 融合角度
};

/* NTC 型号枚举 (foc_ntc_e) 由 driver 层 ntc.h 定义                            */
/* 后备/驱动/角度传感器/扭矩传感器选项枚举由 user 层 param_cfg.h 定义           */
/* 本模块仅以 uint32_t 不透明值保存这些选择, 由 driver/user 的 resolver 负责解释     */

typedef union foc_obs_flag {
    uint32_t val;
    struct {
        uint32_t smo        : 1; // 滑模观测器
        uint32_t hfi        : 1; // 高频注入
        uint32_t luenberger : 1; // 龙伯格观测器
        uint32_t rls        : 1; // 参数辨识
        uint32_t flux       : 1; // 磁链观测器
        uint32_t reserved   : 27;
    } bit;
} foc_obs_flag_u;

enum foc_obs_switch_state {
    FOC_OBS_SWITCH_HFI,
    FOC_OBS_SWITCH_HFI_TO_FLUX,
    FOC_OBS_SWITCH_FLUX,
    FOC_OBS_SWITCH_FLUX_TO_HFI,
};

struct foc_obs_cfg {
    foc_obs_flag_u u_obs_flag;
    float32_t      switch_vel;           // HFI/高速观测器切换中心电角速度 (rad/s)
    float32_t      switch_band_vel;      // HFI/高速观测器过渡带宽度 (rad/s)
    uint32_t       switch_time_ms;       // HFI/高速观测器输出交接时间 (ms)
    uint32_t       switch_ready_ms;      // 高速观测器稳定等待时间 (ms)
    float32_t      switch_align_wc;      // 两观测器角度基准对齐带宽 (rad/s)
    float32_t      switch_theta_err_max; // 允许交接的最大对齐残差 (rad)
    float32_t      enable_vel;           // 启用速度阈值

    struct pll_cfg        omega_pll;    // 速度锁相环参数
    struct rlf_cfg        mech_acc_rls; // 机械加速度 RLS 参数
    struct luenberger_cfg luenberger;   // 龙伯格观测器参数
    struct hfi_cfg        hfi;          // 高频注入参数
    struct smo_cfg        smo;          // 滑模观测器参数
    struct flux_cfg       flux;         // 磁链观测器参数
    struct rlf_cfg        rls;
};

typedef union foc_cali_flag {
    uint32_t val;
    struct {
        uint32_t inductance               : 1; // 电感离线辨识
        uint32_t resistance               : 1; // 电阻离线辨识
        uint32_t nonlinear_elec_theta     : 1; // 电角度非线性误差校准标志
        uint32_t nonlinear_outshaft_theta : 1; // 出轴角度非线性误差校准标志
        uint32_t offset_elec_theta        : 1; // 电角度偏置校准标志
        uint32_t reserved                 : 26;
    } bit;
} foc_cali_flag_u;

enum foc_cali_state {
    FOC_CALI_STATE_INIT,   // 初始化
    FOC_CALI_STATE_CW,     // 正转
    FOC_CALI_STATE_CCW,    // 反转
    FOC_CALI_STATE_FINISH, // 结束
};

enum foc_update_state {
    FOC_STATE_DISABLE, // 失能
    FOC_STATE_ENABLE,  // 使能
    FOC_STATE_CALI,    // 校准
    FOC_STATE_FAULT,   // 故障
};

enum foc_mode {
    FOC_MODE_NONE,
    FOC_MODE_VOL,     // 电压模式
    FOC_MODE_CUR,     // 电流模式
    FOC_MODE_TOR,     // 力矩模式
    FOC_MODE_PD,      // PD 模式
    FOC_MODE_VEL,     // 速度模式
    FOC_MODE_POS,     // 位置模式
    FOC_MODE_HM,      // 回零模式
    FOC_MODE_RATCHET, // 棘轮模式
    FOC_MODE_ASC,     // 主动短路制动模式
    FOC_MODE_IF,      // I/F 模式
    FOC_MODE_VF,      // V/F 模式
};

enum foc_vel_unit {
    FOC_VEL_UNIT_RADS, // rad/s
    FOC_VEL_UNIT_DEGS, // deg/s
    FOC_VEL_UNIT_RPM,  // rpm
    FOC_VEL_UNIT_HZ,   // hz
};

enum foc_ctl_type {
    FOC_CTL_PID = 0,
    FOC_CTL_ADRC,
};

static inline float32_t
foc_vel_to_rads_rt(const float32_t vel, const enum foc_vel_unit e_unit)
{
    switch (e_unit) {
        case FOC_VEL_UNIT_DEGS:
            return DEG2RAD(vel);
        case FOC_VEL_UNIT_RPM:
            return RPM2RADS(vel);
        case FOC_VEL_UNIT_HZ:
            return HZ2RADS(vel);
        default:
            return vel;
    }
}

static inline float32_t
foc_vel_from_rads_rt(const float32_t vel, const enum foc_vel_unit e_unit)
{
    switch (e_unit) {
        case FOC_VEL_UNIT_DEGS:
            return RAD2DEG(vel);
        case FOC_VEL_UNIT_RPM:
            return RADS2RPM(vel);
        case FOC_VEL_UNIT_HZ:
            return RADS2HZ(vel);
        default:
            return vel;
    }
}

struct foc_ref_pvct {
    float32_t pos;     // 位置 (出轴)
    float32_t vel;     // 速度 (出轴)
    float32_t cur;     // q 轴电流
    float32_t tor;     // 目标转矩 (出轴)
    float32_t ffd_vel; // 前馈速度 (出轴)
    float32_t ffd_cur; // 前馈 q 轴电流
    float32_t ffd_tor; // 前馈力矩 (出轴)
};

struct foc_fdb_pvct {
    float32_t pos;      // 位置 (出轴)
    float32_t vel;      // 速度 (出轴)
    float32_t cur;      // q 轴电流
    float32_t elec_tor; // 电磁转矩 (出轴)
    float32_t load_tor; // 负载转矩 (出轴)
};

struct foc_stator {
    uint32_t       is_init;         // 初始化标志
    struct f32_uvw cur_offset;      // 电流偏置 (上电自动校准)
    struct f32_uvw volt_offset;     // 端电压通道差分偏置 (上电自动校准)
    struct f32_uvw f32_i_uvw_raw;   // 原始三相相电流
    struct f32_uvw f32_i_uvw;       // 三相相电流
    struct f32_uvw f32_v_uvw_raw;   // 未扣除通道偏置的三相端电压
    struct f32_uvw f32_v_uvw;       // 三相端电压
    struct f32_uvw f32_line_v_uvw;  // 三相线电压
    float32_t      v_n;             // 共模电压
    struct f32_uvw f32_phase_v_uvw; // 三相相电压
    struct f32_ab  i_ab;            // alpha-beta 轴电流
    struct f32_ab  v_ab;            // alpha-beta 轴电压
    struct f32_dq  i_dq;            // d-q 轴电流
    struct f32_dq  v_dq;            // d-q 轴电压
    float32_t      i_s;             // 电流矢量幅值
    float32_t      v_s;             // 电压矢量幅值
    float32_t      line_v_amp;
};

struct foc_store_theta {
    enum dir  motor_sensor_dir;      // 电机轴编码器方向
    enum dir  outshaft_sensor_dir;   // 出轴编码器方向
    float32_t elec_theta_offset;     // 电角度偏置
    float32_t outshaft_theta_offset; // 出轴角度偏置 (标零用)
    float32_t tor_offset;            // 扭矩传感器零偏 (标零用)
};

struct foc_rotor {
    uint32_t               is_init;                          // 初始化标志
    float32_t              motor_theta_start;                // 电机轴初始相对角度
    float32_t              outshaft_theta_start;             // 出轴初始相对角度
    float32_t              motor_theta_total_wrap_offset;    // 电机轴总角度整圈基准修正
    float32_t              outshaft_theta_total_wrap_offset; // 出轴总角度整圈基准修正
    struct foc_store_theta sensor;                           // 保存的传感器校准数据
    // 电机轴编码器角度
    int32_t   motor_cycle_cnt;        // 电机轴圈数计数
    float32_t motor_theta_raw;        // 电机轴角度原始值 (可能是电角度或机械角度)
    float32_t motor_theta_comp;       // 电机轴角度补偿值
    float32_t motor_theta;            // 电机轴角度
    float32_t motor_theta_prev;       // 上一周期电机轴角度
    float32_t motor_theta_total;      // 总电机轴角度
    float32_t motor_omega;            // 电机轴角速度
    float32_t motor_theta_elec;       // 从电机轴角度换算的电角度
    float32_t motor_omega_elec;       // 从电机轴角速度换算的电角速度
    float32_t motor_theta_mech;       // 从电机轴角度换算的机械角度
    float32_t motor_theta_mech_total; // 从电机轴角度换算的总机械角度
    float32_t motor_omega_mech;
    // 出轴轴编码器角度
    int32_t   outshaft_cycle_cnt;       // 出轴圈数计数
    float32_t outshaft_theta_raw;       // 出轴角度原始值
    float32_t outshaft_theta_comp;      // 出轴角度补偿值
    float32_t outshaft_theta;           // 出轴角度
    float32_t outshaft_theta_prev;      // 上一周期出轴角度
    float32_t outshaft_theta_total;     // 总出轴角度
    float32_t outshaft_omega;           // 出轴角速度
    float32_t outshaft_theta_est;       // 从电机轴机械角度换算的出轴角度
    float32_t outshaft_theta_total_est; // 从电机轴总机械角度换算的出轴总角度
    float32_t outshaft_omega_est;       // 从电机轴机械角速度换算的出轴角速度
    float32_t outshaft_theta_err;       // outshaft_est_theta 与 outshaft_theta 的差值
    float32_t outshaft_theta_err_pp;    // outshaft_theta_err 的峰峰值
    // 机械角度
    float32_t mech_theta;       // 机械角度
    float32_t mech_theta_total; // 总机械角度
    float32_t mech_omega;       // 机械角速度
    float32_t mech_acc;         // 机械角加速度
    // 电角度
    int32_t   elec_cycle_cnt;     // 电角度圈数
    float32_t elec_theta;         // 电角度
    float32_t elec_theta_prev;    // 上一周期电角度
    float32_t elec_theta_comp;    // 电角度补偿值
    float32_t elec_theta_total;   // 总电角度
    float32_t elec_omega;         // 电角速度
    float32_t elec_theta_obs;     // 观测电角度
    float32_t elec_omega_obs;     // 观测电角速度
    float32_t elec_theta_obs_err; // elec_theta_obs 与 motor_theta_elec 的差值
    float32_t elec_theta_force;   // 强拖电角度
    float32_t elec_omega_force;   // 强拖电角速度
};

struct foc_temp {
    float32_t mcu;      // MCU 芯片温度
    float32_t inverter; // 逆变器温度
    struct {
        float32_t uv; // 绕组 UV 相温度
        float32_t vw; // 绕组 VW 相温度
    } stator;
    struct {
        uint32_t inverter  : 2; // MOS NTC 状态，见 enum ntc_status
        uint32_t stator_uv : 2; // UV 绕组 NTC 状态，见 enum ntc_status
        uint32_t stator_vw : 2; // VW 绕组 NTC 状态，见 enum ntc_status
        uint32_t reserved  : 26;
    } ntc_abnormal;
};

struct foc_store_table {
    uint8_t fw[SIZE_1KB];             // 弱磁表
    uint8_t elec_theta[SIZE_1KB];     // 电角度非线性误差表
    uint8_t outshaft_theta[SIZE_1KB]; // 出轴角度非线性误差表
};

struct foc_store {
    struct foc_store_theta sensor; // 传感器校准数据
    struct foc_store_table table;  // 表格数据
};

struct foc_freq_div {
    uint32_t motor_sensor;    // 电机轴编码器
    uint32_t outshaft_sensor; // 出轴编码器
    uint32_t tor_sensor;      // 扭矩传感器
    uint32_t cur;             // 电流环
    uint32_t flux_week;       // 弱磁环
    uint32_t tor;             // 力矩环
    uint32_t pd;              // PD 环
    uint32_t vel;             // 速度环
    uint32_t pos;             // 位置环
};

struct foc_ratchet_cfg {
    float32_t step_angle;   // 棘轮档位间隔 (出轴, rad)
    float32_t switch_angle; // 过档阈值 (出轴, rad; <=0 时使用 step_angle / 2)
    float32_t kp;           // 档位吸附刚度 (A/rad)
    float32_t kd;           // 阻尼 (A/(rad/s))
    float32_t cur_max;      // 输出 q 轴电流限幅 (A; <=0 时使用电机/采样限流)
};

struct foc_vf_cfg {
    float32_t volt_boost;  // 低速电压补偿 (相电压峰值, V; <=0 时自动估算)
    float32_t volt_per_hz; // V/F 斜率 (相电压峰值 V/Hz; <=0 时自动估算)
    float32_t volt_max;    // 最大相电压峰值 (V; <=0 时使用逆变器允许值)
};

struct foc_type_cfg {
    uint32_t actuator;
    uint32_t periph;
    uint32_t motor;
    uint32_t reducer;
    uint32_t backup;
    uint32_t motor_theta_sensor;
    uint32_t outshaft_sensor;
    uint32_t tor_sensor;
    struct {
        uint32_t inverter;
        uint32_t stator_uv;
        uint32_t stator_vw;
    } ntc;
};

struct foc_base_cfg {
    struct motor_cfg   motor;             // 电机参数
    struct reducer_cfg reducer;           // 减速机参数
    struct periph_cfg  periph;            // 硬件/外设参数
    enum dir           dir;               // 执行器方向
    enum foc_vel_unit  e_vel_unit;        // 速度单位 (默认 rad/s)
    float32_t          acc_max;           // 最大加速度 (出轴)
    float32_t          home_vel;          // 回零速度 (出轴)
    float32_t          v_bus_rate;        // 母线电压利用率
    float32_t          cur2tor[4];        // 电流->扭矩 多项式拟合系数
    float32_t          tor2cur[4];        // 扭矩->电流 多项式拟合系数
    uint32_t           self_check_enable; // 启用上电自检
};

struct foc_cali_cfg {
    foc_cali_flag_u u_cali_flag;
    uint32_t        step_delay_ms;                // 相邻校准任务之间的非阻塞延时 (ms)
    float32_t       force_id;                     // 强拖电流
    float32_t       nonlinear_theta_cali_acc_max; // 非线性校准最大加速度 (出轴, rad/s^2)
    float32_t
        nonlinear_motor_theta_cali_motor_vel;  // 电机轴角度非线性误差校准速度 (电机轴机械, rad/s)
    float32_t offset_elec_theta_cali_elec_vel; // 电角度偏置校准速度 (电角速度, rad/s)
    float32_t
        nonlinear_outshaft_theta_cali_outshaft_vel; // 出轴角度非线性误差校准速度 (出轴, rad/s)
};

enum foc_phase {
    FOC_PHASE_NONE,
    FOC_PHASE_U,
    FOC_PHASE_V,
    FOC_PHASE_W,
};

enum foc_stator_ntc {
    FOC_STATOR_NTC_NONE,
    FOC_STATOR_NTC_UV,
    FOC_STATOR_NTC_VW,
    FOC_STATOR_NTC_BOTH,
};

struct foc_sensor_cfg {
    enum foc_sensor     e_motor_sensor;          // 电机轴编码器输出角度类型
    enum foc_theta      e_elec_theta;            // 电角度源
    enum foc_phase      e_loss_phase_cur;        // 缺失的某相电流
    enum foc_stator_ntc e_stator_ntc;            // 启用的绕组 NTC
    uint32_t            outshaft_sensor_enable;  // 启用出轴编码器
    uint32_t outshaft_theta_pos_loop_enable;     // 启用位置全闭环 (用出轴编码器实测角度作位置反馈,
                                                 // 否则用电机轴换算的估计值)
    uint32_t  swap_motor_outshaft_sensor_enable; // 交换电机轴和出轴编码器接口
    uint32_t  swap_theta_tor_uart_enable;        // 交换角度传感器和力矩传感器串口接口
    uint32_t  motor_sensor_comp_enable;          // 启用电机轴编码器非线性补偿
    uint32_t  outshaft_sensor_comp_enable;       // 启用出轴编码器非线性补偿
    uint32_t  tor_sensor_enable;                 // 启用力矩传感器
    uint32_t  terminal_volt_sample_enable;       // 启用端电压采样
    uint32_t  theta_sensor_timeout_retry_ms;     // 角度传感器无响应时的查询重试间隔 (ms)
    uint32_t  tor_sensor_timeout_retry_ms;       // 力矩传感器无响应时的启动命令重试间隔 (ms)
    float32_t motor_theta_delay_comp_cycle;      // 电机轴角度补偿增益
    float32_t pwm_elec_theta_delay_comp_cycle;   // 电角度补偿增益
};

struct foc_ctl_cfg {
    struct foc_freq_div    freq_div;     // 分频系数
    enum foc_ctl_type      e_cur_ctl;    // 电流环控制器类型
    enum foc_ctl_type      e_vel_ctl;    // 速度环控制器类型
    float32_t              cur_wc;       // 电流环带宽
    struct pid_cfg         id_pi;        // d 轴电流环 PI 控制器参数
    struct pid_cfg         iq_pi;        // q 轴电流环 PI 控制器参数
    struct adrc_cfg        id_adrc;      // d 轴电流环 ADRC 参数
    struct adrc_cfg        iq_adrc;      // q 轴电流环 ADRC 参数
    struct pid_cfg         tor_pi;       // 力矩环 PI 控制器参数
    struct pid_cfg         flux_week_pi; // 弱磁环 PI 控制器参数
    struct pid_cfg         pos_vel_pd;   // PD 环 PD 控制器参数
    struct pid_cfg         vel_pi;       // 速度环 PI 控制器参数
    struct adrc_cfg        vel_adrc;     // 速度环 ADRC 参数
    struct pid_cfg         pos_p;        // 位置环 P 控制器参数
    struct foc_ratchet_cfg ratchet;      // 棘轮模式参数
    struct foc_vf_cfg      vf;           // V/F 模式参数
};

struct foc_comm {
    void    *periph; // 外设
    bool     busy;   // 忙碌标志
    bool     frame_ready;
    uint64_t tx_tick;           // 发送时 FOC 运行计数
    uint64_t rx_tick;           // 接收时 FOC 运行计数
    uint32_t elapsed_tick;      // 编码器为收发延迟，力矩传感器为相邻接收周期
    uint64_t tx_ts_us;          // 发送时间戳 (us)
    uint64_t rx_ts_us;          // 接收时间戳 (us)
    uint32_t elapsed_us;        // 与 elapsed_tick 对应的微秒间隔
    uint32_t timeout_cnt;       // 接收超时计数 (FOC 周期内未收到响应)
    uint32_t timeout_total_cnt; // 累计超时/丢包计数 (收到响应后不清零)
    uint32_t verify_err_cnt;    // 校验失败计数 (收到了但数据损坏)
    uint32_t errcode;           // 错误码
};

typedef int (*foc_store_f)(void);
typedef int (*foc_load_f)(void);

struct foc;

typedef int (*foc_init_f)(void);
typedef void (*foc_update_adc_f)(struct foc *foc);
typedef int (*foc_trigger_f)(void);
typedef float32_t (*foc_get_f)(void);

typedef int (*foc_set_status_f)(uint8_t status);
typedef int (*foc_set_pwm_status_f)(enum foc_pwm_ch pwm_ch, uint8_t status);
typedef void (*foc_set_pwm_duty_f)(struct u32_uvw u32_pwm_duty, uint32_t pwm_full_cnt);

struct foc_cfg;
typedef void (*foc_type_resolve_f)(struct foc_cfg *foc_cfg);

struct foc_func_cfg {
    foc_type_resolve_f f_type_resolve; // 解析回调

    foc_load_f  f_load;  // 数据加载
    foc_store_f f_store; // 数据保存

    foc_init_f f_init_periph;          // 外设初始化
    foc_init_f f_init_motor_sensor;    // 电机轴编码器初始化
    foc_init_f f_init_outshaft_sensor; // 出轴编码器初始化
    foc_init_f f_init_tor_sensor;      // 扭矩传感器初始化

    foc_set_status_f     f_set_irq_status; // 中断状态设置
    foc_set_pwm_status_f f_set_pwm_status; // PWM 状态设置
    foc_set_status_f     f_set_drv_status; // 预驱状态设置

    foc_update_adc_f f_update_adc;             // 更新高速 ADC 数据
    foc_get_f        f_get_tor;                // 扭矩获取
    foc_trigger_f    f_trigger_motor_theta;    // 电机轴编码器触发
    foc_get_f        f_get_motor_theta;        // 电机轴角度获取
    foc_trigger_f    f_trigger_outshaft_theta; // 出轴编码器触发
    foc_get_f        f_get_outshaft_theta;     // 出轴角度获取

    foc_set_pwm_duty_f f_set_pwm_duty; // PWM 占空比设置
};

typedef union foc_err {
    uint32_t val;
    struct {
        uint32_t null_ptr                : 1; // 空指针
        uint32_t resistance_cali_timeout : 1; // 电阻辨识升压超时
    } bit;
} foc_err_u;

struct foc_cfg {
    struct foc_type_cfg   type_cfg;   // 类型参数
    struct foc_base_cfg   base_cfg;   // 基础参数
    struct foc_sensor_cfg sensor_cfg; // 传感器参数
    struct foc_ctl_cfg    ctl_cfg;    // 控制参数
    struct foc_obs_cfg    obs_cfg;    // 观测器参数
    struct foc_cali_cfg   cali_cfg;   // 校准参数
    struct foc_func_cfg   func_cfg;   // 函数参数 (由 func_opt 经 f_resolve 解析得到)
};

struct foc_in {
    struct foc_adc      adc;      // ADC 采样数据
    struct foc_stator   stator;   // 定子相关数据 (电流/电压)
    struct foc_rotor    rotor;    // 转子相关数据 (角度)
    struct foc_temp     temp;     // 温度相关数据 (MCU/逆变器/绕组)
    float32_t           i_bus;    // 母线电流
    float32_t           v_bus;    // 母线电压
    float32_t           load_tor; // 转矩
    struct foc_ref_pvct ref_pvct; // 目标 PVCT
};

struct foc_out {
    struct f32_dq       v_dq;         // d-q 轴电压
    struct f32_ab       v_ab;         // alpha-beta 轴电压
    struct f32_ab       v_ab_sv;      // 标幺 alpha-beta 轴电压
    struct f32_uvw      f32_v_uvw;    // 目标三相电压
    struct f32_uvw      f32_v_uvw_sv; // 标幺目标三相电压
    struct foc_svpwm    svpwm;        // SVPWM 相关数据
    struct foc_fdb_pvct fdb_pvct;     // 反馈 PVCT
};

struct foc_stage_time {
    uint32_t last_cyccnt;
    uint32_t max_cyccnt;
};

struct foc_state_time {
    uint32_t last_cyccnt;
    uint32_t max_cyccnt;

    struct foc_stage_time transform;
    struct foc_stage_time observer;
    struct foc_stage_time control;
    struct foc_stage_time svpwm;
};

struct foc_exec_time {
    uint32_t last_us;
    uint32_t max_us;

    struct foc_stage_time total;
    struct foc_stage_time self_check;
    struct foc_stage_time adc;
    struct foc_stage_time theta;
    struct foc_stage_time tor;
    struct foc_state_time state;
    struct foc_stage_time fdb;
};

struct foc_if_ctl {
    float32_t vel_acc_max;     // I/F 速度斜坡最大加速度 (出轴, rad/s^2)
    float32_t vel_ref_limited; // I/F 斜坡后的速度指令 (出轴, rad/s)
};

struct foc_vf_ctl {
    float32_t vel_acc_max;     // V/F 速度斜坡最大加速度 (出轴, rad/s^2)
    float32_t vel_ref_limited; // V/F 斜坡后的速度指令 (出轴, rad/s)
    float32_t volt_ref;        // V/F 输出相电压峰值 (V)
};

/* 参数辨识结果。配置参数是辨识初值/标称值，辨识结果统一保存在运行态 lo 中。 */
struct foc_identify {
    float32_t rs;             // 定子相电阻 (Ω)
    float32_t ld;             // d 轴电感 (H)
    float32_t lq;             // q 轴电感 (H)
    float32_t psi;            // 转子磁链 (Wb)
    float32_t j;              // 电机轴等效惯量 (kg*m^2)
    float32_t friction;       // 电机轴粘性摩擦系数 (N*m*s/rad)
    float32_t outshaft_ratio; // 减速比
};

struct foc_ratchet_ctl {
    float32_t anchor_pos; // 棘轮基准位置 (出轴)
    float32_t detent_pos; // 当前吸附档位位置 (出轴)
    int32_t   idx;        // 当前棘轮档位编号
};

struct foc_lo {
    uint64_t             tick;      // FOC 运行计数
    struct foc_exec_time exec_time; // 执行耗时统计

    uint32_t         is_ready;
    struct foc_store store; // 保存的数据

    foc_err_u       u_err;       // 错误码
    foc_cali_flag_u u_cali_flag; // 校准标志

    enum foc_update_state e_state;      // FOC 状态
    enum foc_mode         e_mode;       // 控制模式
    enum foc_theta        e_elec_theta; // 电角度源
    enum foc_cali_state   e_cali_state; // 校准状态

    struct foc_comm motor_theta_sensor_comm;    // 电机轴编码器通信状态数据
    struct foc_comm outshaft_theta_sensor_comm; // 出轴编码器通信状态数据
    struct foc_comm tor_sensor_comm;            // 扭矩传感器通信状态数据

    struct pid_ctl id_pi;        // d 轴电流环 PI 控制器
    struct pid_ctl iq_pi;        // q 轴电流环 PI 控制器
    struct pid_ctl tor_pi;       // 力矩环 PI 控制器
    struct pid_ctl flux_week_pi; // 弱磁环 PI 控制器
    struct pid_ctl pos_vel_pd;   // PD 环 PD 控制器
    struct pid_ctl vel_pi;       // 速度环 PI 控制器
    struct pid_ctl pos_p;        // 位置环 P 控制器

    struct adrc_ctl id_adrc;  // d 轴电流环 ADRC
    struct adrc_ctl iq_adrc;  // q 轴电流环 ADRC
    struct adrc_ctl vel_adrc; // 速度环 ADRC

    struct pll_filter         omega_pll;            // 速度锁相环
    struct rls_obs            mech_acc_rls;         // 机械加速度 RLS
    struct luenberger_obs     luenberger;           // 龙伯格观测器
    struct smo_obs            smo;                  // 滑模观测器
    struct flux_obs           flux;                 // 磁链观测器
    struct hfi_obs            hfi;                  // 高频注入
    enum foc_obs_switch_state e_obs_switch;         // HFI/flux 交接状态
    float32_t                 hfi_weight;           // HFI 输出权重 [0, 1]
    float32_t                 flux_weight;          // flux 输出权重 [0, 1]
    float32_t                 flux_theta_offset;    // flux 相对 HFI 的角度校正
    float32_t                 obs_switch_theta_err; // 校正后 flux 与 HFI 的角度残差
    uint32_t                  obs_switch_ready_cnt;
    uint8_t                   flux_theta_aligned;
    uint8_t                   hfi_injection_enable;
    struct rls_obs            rls;      // 递推最小二乘观测器
    struct foc_identify       identify; // 参数辨识结果

    struct f32_dq ref_i_dq;  // 目标 d-q 轴电流
    struct f32_dq comp_i_dq; // 补偿 d-q 轴电流
    struct f32_dq ffd_v_dq;  // 前馈 d-q 轴电压

    struct foc_ratchet_ctl ratchet_ctl; // 棘轮模式状态
    struct foc_if_ctl      if_ctl;      // I/F 控制状态
    struct foc_vf_ctl      vf_ctl;      // V/F 控制状态
};

struct foc_tmp {
    struct foc_freq_div freq_div_cnt; // 分频计数

    uint32_t       cur_offset_cali_cnt;                     // 电流偏置校准计数
    uint32_t       offset_cali_settle_cnt;                  // 电流/端电压偏置校准建立计数
    struct f32_uvw cur_offset_sum;                          // 电流偏置值总和
    struct f32_uvw volt_offset_sum;                         // 端电压采样值总和
    int32_t        elec_theta_dir_cali_cnt;                 // 电角度方向校准计数
    uint32_t       elec_theta_offset_cali_cycle_cnt;        // 电角度偏置校准圈数计数
    uint32_t       elec_theta_offset_cali_sample_delay_cnt; // 电角度校准采样延时计数
    float32_t      elec_theta_offset_sum;                   // 电角度偏置总和
    int32_t        outshaft_theta_dir_cali_cnt;             // 出轴编码器方向校准计数
    float32_t      prev_outshaft_theta_raw;                 // 出轴编码器上一周期原始角度

    enum foc_update_state e_prev_state;           // 上一个 FOC 状态
    enum foc_mode         e_prev_mode;            // 上一个控制模式
    enum foc_theta        e_prev_elec_theta;      // 上一个电角度源
    enum foc_mode         e_cali_prev_mode;       // 进入校准状态前的控制模式
    enum foc_theta        e_cali_prev_elec_theta; // 进入校准状态前的电角度源

    int32_t   cali_cnt;
    int32_t   cali_step_delay_cnt;
    int32_t   resistance_cali_timeout_cnt;
    float32_t lpf_is;
    float32_t lpf_vd;

    foc_obs_flag_u u_prev_obs_flag;

    struct f32_dq prev_i_dq;
    struct f32_dq prev_v_dq;

    float32_t inv_motor_npp;
    float32_t inv_outshaft_ratio;
    float32_t motor_theta_comp_gain;
    float32_t pwm_theta_comp_gain;
    float32_t prev_mech_omega;
    uint8_t   mech_acc_rls_valid;
    float32_t prev_outshaft_theta_total_est;
    uint8_t   outshaft_theta_est_valid;
};

struct foc {
    struct foc_cfg cfg; // 参数
    struct foc_in  in;  // 输入数据
    struct foc_out out; // 输出数据
    struct foc_lo  lo;  // 本地数据
    struct foc_tmp tmp; // 临时数据
};

/* 计算电流环 Kp */
#define CUR_KP(kp, wc, ldq)      \
    do {                         \
        if ((wc) != 0.0F)        \
            (kp) = (wc) * (ldq); \
    } while (0);

/* 计算电流环 Ki */
#define CUR_KI(ki, wc, rs)      \
    do {                        \
        if ((wc) != 0.0F)       \
            (ki) = (wc) * (rs); \
    } while (0);

#ifdef __cplusplus
}
#endif

#endif // !FOCDEF_H
