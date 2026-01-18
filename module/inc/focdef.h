#ifndef FOCDEF_H
#define FOCDEF_H

#include "hfi.h"
#include "lbg.h"
#include "pid.h"
#include "pll.h"
#include "smo.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FOC_COIL_NTC_NUM                       (2)
#define FOC_NONLINEAR_THETA_POINT_NUM          ((u32)((SIZE_1KB - sizeof(u32)) / sizeof(f16)))
#define FOC_NONLINEAR_OUTSHAFT_THETA_POINT_NUM ((u32)((SIZE_1KB - sizeof(u32)) / sizeof(f16)))

typedef struct adc_raw {
        i32_uvw_t i32_i_uvw;
        i32       i32_v_bus;
        i32       inverter_ntc;
        i32       coil_ntc[FOC_COIL_NTC_NUM];
} adc_raw_t;

typedef struct svpwm {
        f32       v_max, v_min, v_avg;
        f32_uvw_t f32_pwm_duty;
        u32_uvw_t u32_pwm_duty;
} svpwm_t;

typedef struct periph_cfg {
        /* ADC */
        u32 adc_full_cnt;
        f32 cur_max, vbus_max;
        f32 adc2cur, adc2vbus;
        u32 cur_gain;
        f32 cur_offset;

        /* PWM */
        u32 timer_freq;
        u32 pwm_freq;
        u32 pwm_full_cnt;
        f32 mi;
        f32 f32_pwm_min, f32_pwm_max;
} periph_cfg_t;

typedef enum pwm_ch : u32 {
        PWM_CH_UH,
        PWM_CH_UL,

        PWM_CH_VH,
        PWM_CH_VL,

        PWM_CH_WH,
        PWM_CH_WL,

        PWM_CH_H,
        PWM_CH_L,

        PWM_CH_ALL,
} pwm_ch_e;

typedef enum foc_sensor : u32 {
        FOC_SENSOR_NONE,
        FOC_SENSOR_ELEC,
        FOC_SENSOR_MECH,
} foc_sensor_e;

typedef enum foc_theta : u32 {
        FOC_THETA_NONE,
        FOC_THETA_FORCE,
        FOC_THETA_SENSOR,
        FOC_THETA_SENSORLESS,
        FOC_THETA_SENSORFUSION,
} foc_theta_e;

typedef enum foc_obs : u32 {
        FOC_OBS_NONE,
        FOC_OBS_HFI,
        FOC_OBS_SMO,
        FOC_OBS_LBG,
} foc_obs_e;

typedef struct foc_obs_cfg {
        f32       switch_vel;
        foc_obs_e low;
        foc_obs_e high;

        f32       enable_vel;
        foc_obs_e load_tor;
} foc_obs_cfg_t;

/* -------------------------------------------------------------------------- */
/*                                 CALIBRATION                                */
/* -------------------------------------------------------------------------- */

typedef union foc_cali_flag {
        u32 value;
        struct {
                u32 offset_adc               : 1;
                u32 offset_theta             : 1;
                u32 offset_outshaft_theta    : 1;
                u32 nonlinear_theta          : 1;
                u32 nonlinear_outshaft_theta : 1;
                u32 reserved                 : 27;
        } bits;
} foc_cali_flag_u;

typedef enum foc_cali_state : u32 {
        FOC_CALI_NONE,

        // ADC偏置校准
        FOC_OFFSET_CALI_ADC_INIT,
        FOC_OFFSET_CALI_ADC_SAMPING,
        FOC_OFFSET_CALI_ADC_FINISH,

        // 电气角度传感器零位偏置
        FOC_OFFSET_CALI_THETA_INIT,
        FOC_OFFSET_CALI_THETA_CW,
        FOC_OFFSET_CALI_THETA_CCW,
        FOC_OFFSET_CALI_THETA_FINISH,

        // 出轴角度传感器零位偏置
        FOC_OFFSET_CALI_OUTSHAFT_THETA_INIT,
        FOC_OFFSET_CALI_OUTSHAFT_THETA_FINISH,

        // 电气角度传感器非线性补偿
        FOC_NONLINEAR_CALI_THETA_INIT,
        FOC_NONLINEAR_CALI_THETA_CW,
        FOC_NONLINEAR_CALI_THETA_CCW,
        FOC_NONLINEAR_CALI_THETA_FINISH,

        // 出轴角度传感器非线性补偿
        FOC_NONLINEAR_CALI_OUTSHAFT_THETA_INIT,
        FOC_NONLINEAR_CALI_OUTSHAFT_THETA_CW,
        FOC_NONLINEAR_CALI_OUTSHAFT_THETA_CCW,
        FOC_NONLINEAR_CALI_OUTSHAFT_THETA_FINISH,

        FOC_CALI_FINISH,
} foc_cali_state_e;

typedef enum foc_state : u32 {
        FOC_STATE_NONE,
        FOC_STATE_CALI,
        FOC_STATE_READY,
        FOC_STATE_DISABLE,
        FOC_STATE_ENABLE,
} foc_state_e;

typedef enum foc_mode : u32 {
        FOC_MODE_NONE,
        FOC_MODE_VOL,
        FOC_MODE_CUR,
        FOC_MODE_VEL,
        FOC_MODE_POS,
        FOC_MODE_PD,
        FOC_MODE_ASC,
} foc_mode_e;

typedef enum foc_dir : i32 {
        FOC_DIR_FORWARD = 1,
        FOC_DIR_REVERSE = -1,
} foc_dir_e;

typedef struct foc_ref_pvct {
        f32 pos;
        f32 vel;
        f32 cur;
        f32 elec_tor;
        f32 ffd_vel;
        f32 ffd_cur;
        f32 ffd_tor;
} foc_ref_pvct_t;

typedef struct foc_fdb_pvct {
        f32 pos;
        f32 vel;
        f32 cur;
        f32 elec_tor, load_tor;
} foc_fdb_pvct_t;

typedef struct foc_rotor {
        // 电机角度
        f32 motor_theta_raw, motor_comp_theta;
        f32 motor_theta, motor_theta_offset, motor_omega;
        f32 elec_theta, elec_omega;
        f32 mech_theta, mech_total_theta, mech_omega;
        // 出轴角度
        i32 outshaft_cycle_cnt;
        f32 outshaft_theta_raw, outshaft_comp_theta;
        f32 outshaft_theta, prev_outshaft_theta, outshaft_theta_offset, outshaft_total_theta, outshaft_omega;
        f32 est_outshaft_theta, est_outshaft_total_theta, est_outshaft_omega;
        f32 outshaft_theta_err, outshaft_theta_err_max, outshaft_theta_err_min, outshaft_theta_err_pp;
        // 电气角度
        i32 theta_cycle_cnt;
        f32 theta, prev_theta, comp_theta, total_theta, omega;
        f32 obs_theta, obs_omega, fusion_theta_err;
        f32 force_theta, force_omega;
} foc_rotor_t;

typedef enum foc_coil_ntc {
        FOC_COIL_NTC_0,
        FOC_COIL_NTC_1,
} foc_coil_ntc_e;

typedef struct foc_temp {
        f32 inverter;
        f32 coil[FOC_COIL_NTC_NUM];
} foc_temp_t;

typedef struct foc_offset {
        adc_raw_t adc;
        f32       theta;
        f32       outshaft_theta;
} foc_offset_t;

typedef struct foc_nonlinear {
        f16 theta[FOC_NONLINEAR_THETA_POINT_NUM];
        f16 outshaft_theta[FOC_NONLINEAR_OUTSHAFT_THETA_POINT_NUM];
} foc_nonlinear_t;

typedef struct foc_store_info {
        u32             crc;
        u32             ver;
        foc_cali_flag_u cali_flag;
} foc_store_info_t;

typedef struct foc_store {
        foc_store_info_t info;
        foc_offset_t     offset;
        foc_nonlinear_t  nonlinear;
} foc_store_t;

typedef struct foc_store_addr {
        u32 info;
        u32 offset;
        u32 nonlinear;
} foc_store_addr_t;

typedef struct foc_cali_cnt {
        u32 offset_adc;
        u32 offset_theta_cycle, offset_theta_sample;
} foc_cali_cnt_t;

typedef struct foc_freq_div {
        u32 cur;
        u32 vel;
        u32 pos;
        u32 pd;
} foc_freq_div_t;

typedef struct foc_base_cfg {
        f32              exec_freq;
        foc_store_addr_t store;
        i32              dir;
        f32              outshaft_ratio;
        f32              acc_max;
        motor_cfg_t      motor;
        periph_cfg_t     periph;
} foc_base_cfg_t;

typedef struct foc_cali_cfg {
        foc_cali_cnt_t cnt;
        f32            force_id;
        f32            force_omega;
        f32            offset_theta_lpf_wc, nonlinear_theta_lpf_wc, nonlinear_outshaft_theta_lpf_wc;
        i32            nonlinear_theta_cycle, nonlinear_outshaft_theta_cycle;
        f32            nonlinear_theta_vel, nonlinear_outshaft_theta_vel;
} foc_cali_cfg_t;

typedef struct foc_force_cfg {
        f32 id;
} foc_force_cfg_t;

typedef struct foc_sensor_cfg {
        foc_sensor_e e_sensor;
        u32          outshaft_sensor_enable;
        u32          outshaft_sensor_comp_enable;
        foc_dir_e    theta_dir, outshaft_theta_dir;
        f32          motor_theta_comp_gain;
        f32          theta_comp_gain;
} foc_sensor_cfg_t;

typedef struct foc_ctl_cfg {
        foc_freq_div_t div;
        pid_cfg_t      cur, vel, pos, pd;
} foc_ctl_cfg_t;

typedef int (*foc_store_f)(void *dst, void *src, usize size);
typedef int (*foc_load_f)(void *dst, void *src, usize size);

typedef int (*foc_init_periph_f)(void);
typedef adc_raw_t (*foc_get_adc_f)(void);
typedef f32 (*foc_get_theta_f)(void);

typedef void (*foc_set_pwm_duty_f)(u32 pwm_full_cnt, u32_uvw_t u32_pwm_duty);
typedef void (*foc_set_pwm_status_f)(pwm_ch_e pwm_ch, u8 enable);
typedef void (*foc_set_drv_status_f)(u8 enable);

typedef struct {
        foc_load_f  f_load;
        foc_store_f f_store;

        foc_init_periph_f f_init_periph;
        foc_init_periph_f f_init_motor_theta_sensor;
        foc_init_periph_f f_init_outshaft_theta_sensor;

        foc_get_adc_f   f_get_adc;
        foc_get_theta_f f_get_motor_theta;
        foc_get_theta_f f_get_outshaft_theta;

        foc_set_pwm_duty_f   f_set_pwm_duty;
        foc_set_pwm_status_f f_set_pwm_status;
        foc_set_drv_status_f f_set_drv_status;
} foc_func_cfg_t;

typedef union foc_error {
        u32 value;
        struct {
                u32 null_ptr : 1;
        } bits;
} foc_error_u;

typedef struct {
        foc_base_cfg_t   base_cfg;
        foc_cali_cfg_t   cali_cfg;
        foc_force_cfg_t  force_cfg;
        foc_sensor_cfg_t sensor_cfg;
        foc_ctl_cfg_t    ctl_cfg;
        foc_obs_cfg_t    obs_cfg;
        foc_func_cfg_t   func_cfg;
} foc_cfg_t;

typedef struct {
        adc_raw_t   adc_raw;
        f32_uvw_t   f32_i_uvw;
        f32_ab_t    i_ab;
        f32_dq_t    i_dq;
        foc_rotor_t rotor;
        f32         v_bus;
        foc_temp_t  temp;
} foc_in_t;

typedef struct {
        f32_dq_t  v_dq;
        f32_ab_t  v_ab;
        f32_ab_t  v_ab_sv;
        f32_uvw_t f32_v_uvw;
        svpwm_t   svpwm;
} foc_out_t;

typedef struct {
        f32 elapsed_us;
        u32 exec_cnt;

        foc_error_u u_error;

        foc_store_t store;

        foc_freq_div_t freq_div;
        foc_cali_cnt_t cali_cnt;

        foc_cali_flag_u  cali_flag;
        foc_cali_state_e e_cali_state;

        foc_state_e e_state, e_prev_state;
        foc_mode_e  e_mode, e_prev_mode;
        foc_theta_e e_theta;

        pid_ctl_t id_pid, iq_pid;
        pid_ctl_t vel_pid, pos_pid, pd_pid;

        pll_filter_t pll;
        smo_obs_t    smo;
        hfi_obs_t    hfi;
        lbg_obs_t    lbg;

        f32_dq_t ref_i_dq, comp_i_dq;
        f32_dq_t ffd_v_dq;

        foc_ref_pvct_t ref_pvct;
        foc_fdb_pvct_t fdb_pvct;
} foc_lo_t;

typedef struct {
        foc_cfg_t cfg;
        foc_in_t  in;
        foc_out_t out;
        foc_lo_t  lo;
} foc_t;

#define CUR_KP(wc, ld)          ((wc) * (ld))
#define CUR_KI(wc, rs)          ((wc) * (rs))

#define VEL_KP(wc, psi, npp, j) ((wc) * (j) / (1.5F * (npp) * (psi)))
#define VEL_KI(wc, psi, npp, j) ((wc) * VEL_KP((wc), (psi), (npp), (j)))

#ifdef __cplusplus
}
#endif

#endif // !FOCDEF_H
