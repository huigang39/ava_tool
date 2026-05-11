#ifndef FOCDEF_H
#define FOCDEF_H

#include "pid.h"
#include "pll.h"
#include "rls.h"
#include "typedef.h"

#include "fochfi.h"
#include "focluenberger.h"
#include "focsmo.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct foc_adc_raw {
        i32_uvw_t i32_i_uvw;    // 三相电流 ADC 原始值
        i32_uvw_t i32_v_uvw;    // 三相电压 ADC 原始值
        i32       i32_v_bus;    // 母线电压 ADC 原始值
        i32       inverter_ntc; // 逆变器 NTC 温度 ADC 原始值
        struct {
                i32 uv; // 绕组 UV 相温度 ADC 原始值
                i32 vw; // 绕组 VW 相温度 ADC 原始值
        } stator_ntc;
} foc_adc_raw_t;

typedef struct foc_svpwm {
        f32       v_max;            // 三相最大电压
        f32       v_min;            // 三相最小电压
        f32       v_avg;            // 三相最大最小之和的均值电压
        f32_uvw_t f32_pwm_duty_raw; // PWM 占空比原始值 (百分比) (未中点平移)
        f32_uvw_t f32_pwm_duty;     // PWM 占空比 (百分比)
        u32_uvw_t u32_pwm_duty;     // PWM 占空比计数值
} foc_svpwm_t;

typedef enum foc_pwm_ch : u32 {
        PWM_CH_UH,
        PWM_CH_UL,

        PWM_CH_VH,
        PWM_CH_VL,

        PWM_CH_WH,
        PWM_CH_WL,

        PWM_CH_H,
        PWM_CH_L,

        PWM_CH_ALL,
} foc_pwm_ch_e;

typedef enum foc_sensor : u32 {
        FOC_SENSOR_NONE,
        FOC_SENSOR_ELEC, // 电角度编码器
        FOC_SENSOR_MECH, // 机械角度编码器
} foc_sensor_e;

typedef enum foc_theta : u32 {
        FOC_ELEC_THETA_NONE,
        FOC_ELEC_THETA_FORCE,        // 强拖角度
        FOC_ELEC_THETA_SENSOR,       // 有感角度
        FOC_ELEC_THETA_SENSORLESS,   // 无感角度
        FOC_ELEC_THETA_SENSORFUSION, // 融合角度
} foc_elec_theta_e;

typedef union foc_obs_flag {
        u32 val;
        struct {
                u32 smo        : 1; // 启用滑模观测器
                u32 hfi        : 1; // 启用高频注入
                u32 luenberger : 1; // 启用龙伯格观测器
                u32 rls        : 1; // 启用参数辨识
                u32 reserved   : 28;
        } bit;
} foc_obs_flag_u;

typedef struct foc_obs_cfg {
        foc_obs_flag_u u_obs_flag;
        f32            switch_vel; // 切换速度阈值
        f32            enable_vel; // 启用速度阈值

        pll_cfg_t        omega_pll;  // 速度锁相环参数
        luenberger_cfg_t luenberger; // 龙伯格观测器参数
        hfi_cfg_t        hfi;        // 高频注入参数
        smo_cfg_t        smo;        // 滑模观测器参数
        rls_cfg_t        rls;
} foc_obs_cfg_t;

typedef union foc_cali_flag {
        u32 val;
        struct {
                u32 nonlinear_elec_theta     : 1; // 电角度非线性误差校准标志
                u32 offset_elec_theta        : 1; // 电角度偏置校准标志
                u32 nonlinear_outshaft_theta : 1; // 出轴角度非线性误差校准标志
                u32 reserved                 : 28;
        } bit;
} foc_cali_flag_u;

typedef enum foc_cali_state : u32 {
        FOC_CALI_STATE_INIT,   // 初始化
        FOC_CALI_STATE_CW,     // 正转
        FOC_CALI_STATE_CCW,    // 反转
        FOC_CALI_STATE_FINISH, // 结束
} foc_cali_state_e;

typedef enum foc_select_state : u32 {
        FOC_STATE_DISABLE, // 失能
        FOC_STATE_ENABLE,  // 使能
        FOC_STATE_CALI,    // 校准
        FOC_STATE_FAULT,   // 故障
} foc_state_e;

typedef enum foc_mode : u32 {
        FOC_MODE_NONE,
        FOC_MODE_VOL, // 电压模式
        FOC_MODE_CUR, // 电流模式
        FOC_MODE_TOR, // 力矩模式
        FOC_MODE_PD,  // PD 模式
        FOC_MODE_VEL, // 速度模式
        FOC_MODE_POS, // 位置模式
        FOC_MODE_HM,  // 回零模式
        FOC_MODE_ASC, // 主动短路制动模式
} foc_mode_e;

typedef struct foc_ref_pvct {
        f32 pos;     // 位置 (出轴)
        f32 vel;     // 速度 (出轴)
        f32 cur;     // q 轴电流
        f32 tor;     // 目标转矩 (出轴)
        f32 ffd_vel; // 前馈速度 (出轴)
        f32 ffd_cur; // 前馈 q 轴电流
        f32 ffd_tor; // 前馈力矩 (出轴)
} foc_ref_pvct_t;

typedef struct foc_fdb_pvct {
        f32 pos;      // 位置 (出轴)
        f32 vel;      // 速度 (出轴)
        f32 cur;      // q 轴电流
        f32 elec_tor; // 电磁转矩 (出轴)
        f32 load_tor; // 负载转矩 (出轴)
} foc_fdb_pvct_t;

typedef struct foc_stator {
        u32       is_init;        // 初始化标志
        f32_uvw_t cur_offset;     // 电流偏置 (上电自动校准)
        f32_uvw_t f32_i_uvw_raw;  // 原始三相相电流
        f32_uvw_t f32_i_uvw;      // 三相相电流
        f32_uvw_t f32_v_uvw;      // 三相端电压
        f32_uvw_t f32_line_v_uvw; // 三相线电压
        f32_ab_t  i_ab;           // alpha-beta 轴电流
        f32_ab_t  v_ab;           // alpha-beta 轴电压
        f32_dq_t  i_dq;           // d-q 轴电流
        f32_dq_t  v_dq;           // d-q 轴电压
        f32       i_s;            // 电流矢量幅值
        f32       v_s;            // 电压矢量幅值
        f32       line_v_amp;
} foc_stator_t;

typedef struct foc_store_theta {
        dir_e motor_sensor_dir;      // 电机轴编码器方向
        dir_e outshaft_sensor_dir;   // 出轴编码器方向
        f32   elec_offset_theta;     // 电角度偏置
        f32   outshaft_offset_theta; // 出轴角度偏置 (标零用)
} foc_store_sensor_t;

typedef struct foc_rotor {
        u32                is_init;             // 初始化标志
        f32                motor_init_theta;    // 电机轴编码器角度 (上电时刻)
        f32                outshaft_init_theta; // 出轴编码器角度 (上电时刻)
        f32                motor_offset_theta;
        f32                outshaft_offset_theta;
        foc_store_sensor_t sensor; // 保存的传感器校准数据
        // 电机轴编码器角度
        i32 motor_cycle_cnt;        // 电机轴圈数计数
        f32 motor_theta_raw;        // 电机轴角度原始值 (可能是电角度或机械角度)
        f32 motor_comp_theta;       // 电机轴角度补偿值
        f32 motor_theta;            // 电机轴角度
        f32 motor_prev_theta;       // 上一周期电机轴角度
        f32 motor_total_theta;      // 总电机轴角度
        f32 motor_omega;            // 电机轴角速度
        f32 motor_elec_theta;       // 从电机轴角度换算的电角度
        f32 motor_elec_omega;       // 从电机轴角速度换算的电角速度
        f32 motor_mech_theta;       // 从电机轴角度换算的机械角度
        f32 motor_mech_total_theta; // 从电机轴角度换算的总机械角度
        f32 motor_mech_omega;
        // 出轴轴编码器角度
        i32 outshaft_cycle_cnt;       // 出轴圈数计数
        f32 outshaft_theta_raw;       // 出轴角度原始值
        f32 outshaft_comp_theta;      // 出轴角度补偿值
        f32 outshaft_theta;           // 出轴角度
        f32 outshaft_prev_theta;      // 上一周期出轴角度
        f32 outshaft_total_theta;     // 总出轴角度
        f32 outshaft_omega;           // 出轴角速度
        f32 outshaft_est_theta;       // 从电机轴机械角度换算的出轴角度
        f32 outshaft_est_total_theta; // 从电机轴总机械角度换算的出轴总角度
        f32 outshaft_est_omega;       // 从电机轴机械角速度换算的出轴角速度
        f32 outshaft_theta_err;       // outshaft_est_theta 与 outshaft_theta 的差值
        f32 outshaft_theta_err_pp;    // outshaft_theta_err 的峰峰值
        // 机械角度
        f32 mech_theta;       // 机械角度
        f32 mech_total_theta; // 总机械角度
        f32 mech_omega;       // 机械角速度
        f32 mech_prev_omega;  // 上一周期机械角速度
        f32 mech_acc;         // 机械角加速度
        // 电角度
        i32 elec_cycle_cnt;        // 电角度圈数
        f32 elec_theta;            // 电角度
        f32 elec_prev_theta;       // 上一周期电角度
        f32 elec_comp_theta;       // 电角度补偿值
        f32 elec_total_theta;      // 总电角度
        f32 elec_omega;            // 电角速度
        f32 elec_obs_theta;        // 观测电角度
        f32 elec_obs_omega;        // 观测电角速度
        f32 elec_fusion_theta_err; // elec_obs_theta 与 elec_theta 的差值
        f32 elec_force_theta;      // 强拖电角度
        f32 elec_force_omega;      // 强拖电角速度
} foc_rotor_t;

typedef struct foc_temp {
        f32 inverter; // 逆变器温度
        struct {
                f32 uv; // 绕组 UV 相温度
                f32 vw; // 绕组 VW 相温度
        } stator;
} foc_temp_t;

typedef struct foc_store_table {
        u8 fw[SIZE_1KB];             // 弱磁表
        u8 elec_theta[SIZE_1KB];     // 电角度非线性误差表
        u8 outshaft_theta[SIZE_1KB]; // 出轴角度非线性误差表
} foc_store_table_t;

typedef struct foc_store {
        foc_store_sensor_t sensor; // 传感器校准数据
        foc_store_table_t  table;  // 表格数据
} foc_store_t;

typedef struct foc_freq_div {
        f32 motor_sensor;    // 电机轴编码器
        f32 outshaft_sensor; // 出轴编码器
        f32 tor_sensor;      // 扭矩传感器
        f32 cur;             // 电流环
        f32 flux_week;       // 弱磁环
        f32 tor;             // 力矩环
        f32 pd;              // PD 环
        f32 vel;             // 速度环
        f32 pos;             // 位置环
} foc_freq_div_t;

typedef struct foc_base_cfg {
        f32          fs;             // FOC 运行频率 (定时器中断频率)
        motor_cfg_t  motor;          // 电机参数
        periph_cfg_t periph;         // 硬件/外设参数
        dir_e        dir;            // 执行器方向
        f32          outshaft_ratio; // 减速比
        f32          acc_max;        // 最大加速度 (出轴)
        f32          home_vel;       // 回零速度 (出轴)
        f32          v_bus_rate;     // 母线电压利用率
        u32          self_check_enable;
} foc_base_cfg_t;

typedef struct foc_cali_cfg {
        f32 force_id;                          // 强拖电流
        f32 elec_theta_nonlinear_cali_vel;     // 电角度非线性误差校准速度 (出轴), 如果设为 0 将不会校准此项
        f32 elec_theta_offset_cali_vel;        // 电角度偏置校准速度 (出轴)
        f32 outshaft_theta_nonlinear_cali_vel; // 出轴非线性误差校准速度 (出轴), 如果设为 0 将不会校准此项
} foc_cali_cfg_t;

typedef struct foc_sensor_cfg {
        foc_sensor_e     e_sensor;                     // 编码器类型
        foc_elec_theta_e e_elec_theta;                 // 电角度源
        u32              theta_uart_baudrate;          // 角度传感器串口波特率
        u32              tor_uart_baudrate;            // 转矩传感器串口波特率
        u32              outshaft_sensor_enable;       // 启用出轴编码器
        u32              motor_sensor_comp_enable;     // 启用电机轴编码器非线性补偿
        u32              outshaft_sensor_comp_enable;  // 启用出轴编码器非线性补偿
        u32              tor_sensor_enable;            // 启用力矩传感器
        u32              terminal_volt_sample_enable;  // 启用端电压采样
        f32              motor_theta_delay_comp_cycle; // 电机轴角度补偿增益
        f32              elec_theta_delay_comp_cycle;  // 电角度补偿增益
} foc_sensor_cfg_t;

typedef struct foc_ctl_cfg {
        foc_freq_div_t div;          // 分频系数
        f32            cur_wc;       // 电流环带宽
        pid_cfg_t      id_pi;        // d 轴电流环 PI 控制器参数
        pid_cfg_t      iq_pi;        // q 轴电流环 PI 控制器参数
        pid_cfg_t      tor_pi;       // 力矩环 PI 控制器参数
        pid_cfg_t      flux_week_pi; // 弱磁环 PI 控制器参数
        pid_cfg_t      pos_vel_pd;   // PD 环 PD 控制器参数
        pid_cfg_t      vel_pi;       // 速度环 PI 控制器参数
        pid_cfg_t      pos_p;        // 位置环 P 控制器参数
} foc_ctl_cfg_t;

typedef struct foc_comm {
        void *periph;       // 外设
        u8    busy;         // 忙碌标志
        u64   tx_tick;      // 发送时 FOC 运行计数
        u64   rx_tick;      // 接收时 FOC 运行计数
        u32   elapsed_tick; // 单次收发消耗的计数值
        u32   rx_err_cnt;   // FOC 周期内未接收到值产生的错误计数
        u32   errcode;      // 错误码
} foc_comm_t;

typedef int (*foc_store_f)(void);
typedef int (*foc_load_f)(void);

typedef int (*foc_init_f)(void);
typedef foc_adc_raw_t (*foc_get_adc_raw_f)(void);
typedef int (*foc_trigger_f)(void);
typedef f32 (*foc_get_f)(void);

typedef int (*foc_set_status_f)(u8 status);
typedef int (*foc_set_pwm_status_f)(foc_pwm_ch_e pwm_ch, u8 status);
typedef void (*foc_set_pwm_duty_f)(u32_uvw_t u32_pwm_duty, u32 pwm_full_cnt);

typedef struct foc_func_cfg {
        foc_load_f  f_load;  // 数据加载
        foc_store_f f_store; // 数据保存

        foc_init_f f_init_periph;          // 外设初始化
        foc_init_f f_init_motor_sensor;    // 电机轴编码器初始化
        foc_init_f f_init_outshaft_sensor; // 出轴编码器初始化
        foc_init_f f_init_tor_sensor;      // 扭矩传感器初始化

        foc_set_status_f     f_set_irq_status; // 中断状态设置
        foc_set_pwm_status_f f_set_pwm_status; // PWM 状态设置
        foc_set_status_f     f_set_drv_status; // 预驱状态设置

        foc_get_adc_raw_f f_get_adc_raw;            // ADC 原始值获取
        foc_get_f         f_get_tor;                // 扭矩获取
        foc_trigger_f     f_trigger_motor_theta;    // 电机轴编码器触发
        foc_get_f         f_get_motor_theta;        // 电机轴角度获取
        foc_trigger_f     f_trigger_outshaft_theta; // 出轴编码器触发
        foc_get_f         f_get_outshaft_theta;     // 出轴角度获取

        foc_set_pwm_duty_f f_set_pwm_duty; // PWM 占空比设置
} foc_func_cfg_t;

typedef union foc_err {
        u32 val;
        struct {
                u32 null_ptr : 1; // 空指针
        } bit;
} foc_err_u;

typedef struct foc_cfg {
        foc_base_cfg_t   base_cfg;   // 基础参数
        foc_sensor_cfg_t sensor_cfg; // 传感器参数
        foc_ctl_cfg_t    ctl_cfg;    // 控制参数
        foc_obs_cfg_t    obs_cfg;    // 观测器参数
        foc_cali_cfg_t   cali_cfg;   // 校准参数
        foc_func_cfg_t   func_cfg;   // 函数参数
} foc_cfg_t;

typedef struct foc_in {
        foc_adc_raw_t  adc_raw;  // ADC 原始值
        foc_stator_t   stator;   // 定子相关数据 (电流/电压)
        foc_rotor_t    rotor;    // 转子相关数据 (角度)
        foc_temp_t     temp;     // 温度相关数据 (逆变器/绕组)
        f32            i_bus;    // 母线电流
        f32            v_bus;    // 母线电压
        f32            load_tor; // 转矩
        foc_ref_pvct_t ref_pvct; // 目标 PVCT
} foc_in_t;

typedef struct foc_out {
        f32_dq_t       v_dq;         // d-q 轴电压
        f32_ab_t       v_ab;         // alpha-beta 轴电压
        f32_ab_t       v_ab_sv;      // 标幺 alpha-beta 轴电压
        f32_uvw_t      f32_v_uvw;    // 目标三相电压
        f32_uvw_t      f32_v_uvw_sv; // 标幺目标三相电压
        foc_svpwm_t    svpwm;        // SVPWM 相关数据
        foc_fdb_pvct_t fdb_pvct;     // 反馈 PVCT
} foc_out_t;

typedef struct foc_lo {
        u32 elapsed_us_max; // 最大耗时 (微秒)
        u64 tick;           // FOC 运行计数

        u32         is_ready;
        foc_store_t store; // 保存的数据

        foc_err_u       u_err;       // 错误码
        foc_cali_flag_u u_cali_flag; // 校准标志

        foc_state_e      e_state;      // FOC 状态
        foc_cali_state_e e_cali_state; // 校准状态
        foc_mode_e       e_mode;       // 控制模式
        foc_elec_theta_e e_elec_theta; // 电角度源

        foc_comm_t motor_sensor_comm;    // 电机轴编码器通信状态数据
        foc_comm_t outshaft_sensor_comm; // 出轴编码器通信状态数据
        foc_comm_t tor_sensor_comm;      // 扭矩传感器通信状态数据

        pid_ctl_t id_pi;        // d 轴电流环 PI 控制器
        pid_ctl_t iq_pi;        // q 轴电流环 PI 控制器
        pid_ctl_t tor_pi;       // 力矩环 PI 控制器
        pid_ctl_t flux_week_pi; // 弱磁环 PI 控制器
        pid_ctl_t pos_vel_pd;   // PD 环 PD 控制器
        pid_ctl_t vel_pi;       // 速度环 PI 控制器
        pid_ctl_t pos_p;        // 位置环 P 控制器

        pll_filter_t     omega_pll;  // 速度锁相环
        luenberger_obs_t luenberger; // 龙伯格观测器
        smo_obs_t        smo;        // 滑模观测器
        hfi_obs_t        hfi;        // 高频注入
        rls_obs_t        rls;

        f32_dq_t ref_i_dq;  // 目标 d-q 轴电流
        f32_dq_t comp_i_dq; // 补偿 d-q 轴电流
        f32_dq_t ffd_v_dq;  // 前馈 d-q 轴电压
} foc_lo_t;

typedef struct foc_tmp {
        u32            elapsed_us;      // FOC 耗时 (微秒)
        u32            prev_elapsed_us; // 上一周期 FOC 耗时 (微秒)
        foc_freq_div_t freq_div_cnt;    // 分频计数

        u32       cur_offset_cali_cnt;                     // 电流偏置校准计数
        f32_uvw_t cur_offset_sum;                          // 电流偏置值总和
        i32       elec_theta_dir_cali_cnt;                 // 电角度方向校准计数
        u32       elec_theta_offset_cali_cycle_cnt;        // 电角度偏置校准圈数计数
        u32       elec_theta_offset_cali_sample_delay_cnt; // 电角度校准采样延时计数
        f32       elec_theta_offset_sum;                   // 电角度偏置总和

        foc_state_e      e_prev_state;      // 上一个 FOC 状态
        foc_mode_e       e_prev_mode;       // 上一个控制模式
        foc_elec_theta_e e_prev_elec_theta; // 上一个电角度源

        foc_obs_flag_u u_prev_obs_flag;

        f32_dq_t prev_i_dq;
        f32_dq_t prev_v_dq;
} foc_tmp_t;

typedef struct foc {
        foc_cfg_t cfg; // 参数
        foc_in_t  in;  // 输入数据
        foc_out_t out; // 输出数据
        foc_lo_t  lo;  // 本地数据
        foc_tmp_t tmp; // 临时数据
} foc_t;

/* 计算电流环 Kp */
#define CUR_KP(kp, wc, ldq)                  \
        do {                                 \
                if ((wc) != 0.0F)            \
                        (kp) = (wc) * (ldq); \
        } while (0);

/* 计算电流环 Kd */
#define CUR_KI(ki, wc, rs)                  \
        do {                                \
                if ((wc) != 0.0F)           \
                        (ki) = (wc) * (rs); \
        } while (0);

#ifdef __cplusplus
}
#endif

#endif // !FOCDEF_H
