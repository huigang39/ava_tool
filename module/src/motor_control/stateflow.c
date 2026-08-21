#include "benchmark.h"
#include "mathdef.h"
#include <stddef.h>
#include <stdint.h>

#include "macrodef.h"

#include "motor_control/foc.h"
#include "motor_control/focdef.h"
#include <string.h>

extern void foc_obs_i_ab_rt(struct foc *foc);
extern void foc_obs_i_dq_rt(struct foc *foc);
extern void foc_obs_v_dq_rt(struct foc *foc);
extern void foc_select_mode_rt(struct foc *foc);

extern void foc_vol_ctl_rt(struct foc *foc);
extern void foc_if_theta_rt(struct foc *foc);
extern void foc_if_ctl_rt(struct foc *foc);
extern void foc_vf_theta_rt(struct foc *foc);
extern void foc_vf_ctl_rt(struct foc *foc);
extern void foc_cur_ctl_rt(struct foc *foc);
extern void foc_tor_ctl_rt(struct foc *foc);
extern void foc_vel_ctl_rt(struct foc *foc);
extern void foc_flux_week_ctl_rt(struct foc *foc);
extern void foc_pos_ctl_rt(struct foc *foc);
extern void foc_pd_ctl_rt(struct foc *foc);
extern void foc_home_ctl_rt(struct foc *foc);
extern void foc_ratchet_ctl_rt(struct foc *foc);
extern void foc_asc_ctl_rt(struct foc *foc);

void foc_disable_rt(struct foc *foc);
void foc_enable_rt(struct foc *foc);
void foc_fault_rt(struct foc *foc);

static void foc_svpwm_rt(struct foc *foc);

static inline void
foc_update_detail_time_rt(struct foc_stage_time *time, const uint32_t elapsed)
{
    time->last_cyccnt = elapsed;
    if (elapsed > time->max_cyccnt)
        time->max_cyccnt = elapsed;
}

static inline void
foc_clear_state_detail_last_rt(struct foc_state_time *time)
{
    time->transform.last_cyccnt = 0;
    time->observer.last_cyccnt  = 0;
    time->control.last_cyccnt   = 0;
    time->svpwm.last_cyccnt     = 0;
}

static inline void
foc_commit_state_detail_rt(struct foc_state_time *time,
                           const uint32_t         transform_cyccnt,
                           const uint32_t         observer_cyccnt,
                           const uint32_t         control_cyccnt,
                           const uint32_t         svpwm_cyccnt)
{
    foc_update_detail_time_rt(&time->transform, transform_cyccnt);
    foc_update_detail_time_rt(&time->observer, observer_cyccnt);
    foc_update_detail_time_rt(&time->control, control_cyccnt);
    foc_update_detail_time_rt(&time->svpwm, svpwm_cyccnt);
}

void
foc_update_state_rt(struct foc *foc)
{
    DECL(foc, lo);

    if (lo->e_state != FOC_STATE_ENABLE)
        foc_clear_state_detail_last_rt(&lo->exec_time.state);

    switch (lo->e_state) {
        case FOC_STATE_DISABLE: {
            foc_disable_rt(foc);
            break;
        }
        case FOC_STATE_ENABLE: {
            foc_enable_rt(foc);
            break;
        }
        case FOC_STATE_CALI: {
            foc_cali(foc);
            break;
        }
        case FOC_STATE_FAULT: {
            foc_fault_rt(foc);
            break;
        }
        default:
            break;
    }
}

/**
 * @brief 选择 FOC 电气角度源
 *
 * @param foc FOC 结构体
 * @return    void
 */
static void
foc_select_theta_rt(struct foc *foc)
{
    DECL(foc, cfg, in, lo);

    switch (lo->e_elec_theta) {
        case FOC_ELEC_THETA_FORCE: {
            in->rotor.elec_theta = in->rotor.elec_theta_force;
            in->rotor.elec_omega = in->rotor.elec_omega_force;
            break;
        }
        case FOC_ELEC_THETA_SENSOR: {
            WARP_TAU(in->rotor.elec_theta,
                     in->rotor.motor_theta_elec - in->rotor.sensor.elec_theta_offset);
            in->rotor.elec_omega = in->rotor.motor_omega_elec;
            break;
        }
        case FOC_ELEC_THETA_SENSORLESS: {
            in->rotor.elec_theta = in->rotor.elec_theta_obs;
            in->rotor.elec_omega = in->rotor.elec_omega_obs;
            break;
        }
        case FOC_ELEC_THETA_SENSORFUSION:
            break;
        default:
            break;
    }

    /* 观测器电角度和传感器电角度误差计算 */
    WARP_PI(in->rotor.elec_theta_obs_err,
            in->rotor.motor_theta_elec - in->rotor.sensor.elec_theta_offset -
                in->rotor.elec_theta_obs);

    CYCLE_CNT(in->rotor.elec_cycle_cnt, in->rotor.elec_theta, in->rotor.elec_theta_prev);
    in->rotor.elec_theta_total =
        (float32_t)in->rotor.elec_cycle_cnt * (float32_t)TAU + in->rotor.elec_theta;
}

void
foc_fault_rt(struct foc *foc)
{
    DECL(foc, cfg);

    cfg->func_cfg.f_set_pwm_status(PWM_CH_ALL, false);
}

/**
 * @brief FOC 失能状态
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_disable_rt(struct foc *foc)
{
    DECL(foc, cfg, in, out, lo, tmp);

    cfg->func_cfg.f_set_pwm_status(PWM_CH_ALL, false);

    memset(&out->v_dq, 0, sizeof(out->v_dq));
    memset(&out->v_ab, 0, sizeof(out->v_ab));
    memset(&out->v_ab_sv, 0, sizeof(out->v_ab_sv));
    memset(&out->f32_v_uvw, 0, sizeof(out->f32_v_uvw));
    memset(&out->f32_v_uvw_sv, 0, sizeof(out->f32_v_uvw_sv));
    memset(&out->svpwm, 0, sizeof(out->svpwm));

    in->stator.i_s = 0.0F;
    memset(&in->stator.i_ab, 0, sizeof(in->stator.i_ab));
    memset(&in->stator.i_dq, 0, sizeof(in->stator.i_dq));

    memset(&lo->comp_i_dq, 0, sizeof(lo->comp_i_dq));
    memset(&lo->ref_i_dq, 0, sizeof(lo->ref_i_dq));
    memset(&lo->ffd_v_dq, 0, sizeof(lo->ffd_v_dq));

    RESET(&lo->smo, out);
    RESET(&lo->hfi, out);
    RESET(&lo->luenberger, in, out, lo);

    RESET(&lo->id_pi, in, out, lo);
    RESET(&lo->iq_pi, in, out, lo);
    RESET(&lo->tor_pi, in, out, lo);
    RESET(&lo->flux_week_pi, in, out, lo);
    RESET(&lo->pos_vel_pd, in, out, lo);
    RESET(&lo->vel_pi, in, out, lo);
    RESET(&lo->pos_p, in, out, lo);
    adrc_reset(&lo->id_adrc);
    adrc_reset(&lo->iq_adrc);
    adrc_reset(&lo->vel_adrc);

    /* 校准中止或正常结束后恢复运行速度环的加速度限制。 */
    lo->vel_pi.cfg.ref_rate_max = ABS(cfg->base_cfg.acc_max);
    lo->vel_pi.cfg.ref_change_max =
        lo->vel_pi.cfg.fs > 0.0F ? lo->vel_pi.cfg.ref_rate_max / lo->vel_pi.cfg.fs : 0.0F;
    lo->vel_adrc.cfg.ref_rate_max = ABS(cfg->base_cfg.acc_max);

    lo->hfi.lo.e_polar_idf = HFI_POLAR_IDF_READY;
}

/**
 * @brief FOC 使能状态
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_enable_rt(struct foc *foc)
{
    DECL(foc, cfg, in, out, lo, tmp);
    uint32_t transform_cyccnt = 0;
    uint32_t observer_cyccnt  = 0;
    uint32_t control_cyccnt   = 0;
    uint32_t svpwm_cyccnt     = 0;
    uint32_t mark_cyccnt;
    uint32_t next_cyccnt;

    foc_clear_state_detail_last_rt(&lo->exec_time.state);

    /* 偏置校准期间由 foc_stator_init 保持三相下管导通,不能被 SVPWM 覆盖. */
    if (!in->stator.is_init)
        return;

    cfg->func_cfg.f_set_pwm_status(PWM_CH_ALL, true);
    mark_cyccnt = benchmark_read_cyccnt_rt();

    /* clarke 变换 */
    in->stator.i_ab = clarke_amp_rt(in->stator.f32_i_uvw);
    if (cfg->sensor_cfg.terminal_volt_sample_enable)
        in->stator.v_ab = clarke_amp_rt(in->stator.f32_v_uvw);

    /* 无感观测器(iab) */
    next_cyccnt       = benchmark_read_cyccnt_rt();
    transform_cyccnt += next_cyccnt - mark_cyccnt;
    mark_cyccnt       = next_cyccnt;
    foc_obs_i_ab_rt(foc);

    /* 模式切换可能同时改变电角度源，必须在本周期 Park 变换前完成。 */
    foc_select_mode_rt(foc);

    /* I/F 模式在坐标变换前更新强制电角度。 */
    if (lo->e_mode == FOC_MODE_IF)
        foc_if_theta_rt(foc);
    else if (lo->e_mode == FOC_MODE_VF)
        foc_vf_theta_rt(foc);

    /* 电角度源选择 */
    next_cyccnt      = benchmark_read_cyccnt_rt();
    observer_cyccnt += next_cyccnt - mark_cyccnt;
    mark_cyccnt      = next_cyccnt;
    foc_select_theta_rt(foc);

    float32_t sin_theta;
    float32_t cos_theta;
    fast_sincosf_rt(in->rotor.elec_theta, &sin_theta, &cos_theta);

    /* park 变换 */
    in->stator.i_dq = park_sincos_rt(in->stator.i_ab, sin_theta, cos_theta);
    in->stator.i_s  = SQRT(SQ(in->stator.i_dq.d) + SQ(in->stator.i_dq.q));

    if (cfg->sensor_cfg.terminal_volt_sample_enable) {
        in->stator.v_dq          = park_sincos_rt(in->stator.v_ab, sin_theta, cos_theta);
        in->stator.v_s           = SQRT(SQ(in->stator.v_dq.d) + SQ(in->stator.v_dq.q));
        const float32_t inv_vbus = 1.0F / in->v_bus;
        in->i_bus                = (float32_t)(DIV_3_BY_2 *
                                (in->stator.i_dq.d * in->stator.v_dq.d +
                                 in->stator.i_dq.q * in->stator.v_dq.q) *
                                inv_vbus);
    } else {
        in->stator.v_s = 0.0F;
        in->i_bus      = 0.0F;
    }

    /* 无感观测器(idq) */
    next_cyccnt       = benchmark_read_cyccnt_rt();
    transform_cyccnt += next_cyccnt - mark_cyccnt;
    mark_cyccnt       = next_cyccnt;
    foc_obs_i_dq_rt(foc);

    /* 控制模式选择 */
    next_cyccnt          = benchmark_read_cyccnt_rt();
    observer_cyccnt     += next_cyccnt - mark_cyccnt;
    mark_cyccnt          = next_cyccnt;
    uint8_t asc_enabled  = false;
    switch (lo->e_mode) {
        case FOC_MODE_VOL: {
            foc_vol_ctl_rt(foc);
            break;
        }
        case FOC_MODE_CUR: {
            foc_cur_ctl_rt(foc);
            foc_vol_ctl_rt(foc);
            break;
        }
        case FOC_MODE_IF: {
            foc_if_ctl_rt(foc);
            foc_cur_ctl_rt(foc);
            foc_vol_ctl_rt(foc);
            break;
        }
        case FOC_MODE_VF: {
            foc_vf_ctl_rt(foc);
            foc_vol_ctl_rt(foc);
            break;
        }
        case FOC_MODE_TOR: {
            foc_tor_ctl_rt(foc);
            foc_cur_ctl_rt(foc);
            foc_vol_ctl_rt(foc);
            break;
        }
        case FOC_MODE_VEL: {
            foc_vel_ctl_rt(foc);
            foc_flux_week_ctl_rt(foc);
            foc_cur_ctl_rt(foc);
            foc_vol_ctl_rt(foc);
            break;
        }
        case FOC_MODE_POS: {
            foc_pos_ctl_rt(foc);
            foc_vel_ctl_rt(foc);
            foc_cur_ctl_rt(foc);
            foc_vol_ctl_rt(foc);
            break;
        }
        case FOC_MODE_PD: {
            foc_pd_ctl_rt(foc);
            foc_tor_ctl_rt(foc);
            foc_cur_ctl_rt(foc);
            foc_vol_ctl_rt(foc);
            break;
        }
        case FOC_MODE_HM: {
            foc_home_ctl_rt(foc);
            foc_vel_ctl_rt(foc);
            foc_cur_ctl_rt(foc);
            foc_vol_ctl_rt(foc);
            break;
        }
        case FOC_MODE_RATCHET: {
            foc_ratchet_ctl_rt(foc);
            foc_cur_ctl_rt(foc);
            foc_vol_ctl_rt(foc);
            break;
        }
        case FOC_MODE_ASC: {
            foc_asc_ctl_rt(foc);
            asc_enabled = true;
            break;
        }
        default:
            break;
    }

    next_cyccnt     = benchmark_read_cyccnt_rt();
    control_cyccnt += next_cyccnt - mark_cyccnt;
    if (asc_enabled) {
        foc_commit_state_detail_rt(
            &lo->exec_time.state, transform_cyccnt, observer_cyccnt, control_cyccnt, svpwm_cyccnt);
        return;
    }
    mark_cyccnt = next_cyccnt;

    /* 无感观测器(vdq) */
    foc_obs_v_dq_rt(foc);

    /* 补偿 PWM 采样造成的延迟 */
    next_cyccnt              = benchmark_read_cyccnt_rt();
    observer_cyccnt         += next_cyccnt - mark_cyccnt;
    mark_cyccnt              = next_cyccnt;
    float32_t inv_sin_theta  = sin_theta;
    float32_t inv_cos_theta  = cos_theta;
    if (lo->e_elec_theta != FOC_ELEC_THETA_FORCE) {
        in->rotor.elec_theta_comp = tmp->pwm_theta_comp_gain * in->rotor.elec_omega;
        WARP_TAU(in->rotor.elec_theta, in->rotor.elec_theta + in->rotor.elec_theta_comp);
        fast_sincosf_rt(in->rotor.elec_theta, &inv_sin_theta, &inv_cos_theta);
    }

    /* 反 park 变换 */
    out->v_ab = inv_park_sincos_rt(out->v_dq, inv_sin_theta, inv_cos_theta);

    /* 标幺 */
    const float32_t inv_vbus = 1.0F / in->v_bus;
    out->v_ab_sv.a           = out->v_ab.a * inv_vbus;
    out->v_ab_sv.b           = out->v_ab.b * inv_vbus;

    /* 调制发波 */
    next_cyccnt       = benchmark_read_cyccnt_rt();
    transform_cyccnt += next_cyccnt - mark_cyccnt;
    mark_cyccnt       = next_cyccnt;
    foc_svpwm_rt(foc);
    cfg->func_cfg.f_set_pwm_duty(out->svpwm.u32_pwm_duty, cfg->base_cfg.periph.pwm_full_cnt);

    next_cyccnt   = benchmark_read_cyccnt_rt();
    svpwm_cyccnt += next_cyccnt - mark_cyccnt;
    foc_commit_state_detail_rt(
        &lo->exec_time.state, transform_cyccnt, observer_cyccnt, control_cyccnt, svpwm_cyccnt);
}

/**
 * @brief SVPWM 单次执行计算
 *
 * @param foc FOC 结构体
 * @return    void
 */
static void
foc_svpwm_rt(struct foc *foc)
{
    DECL(foc, cfg, in, out);

    out->f32_v_uvw_sv = inv_clarke_rt(out->v_ab_sv);

    if (out->f32_v_uvw_sv.u > out->f32_v_uvw_sv.v) {
        out->svpwm.v_max = out->f32_v_uvw_sv.u;
        out->svpwm.v_min = out->f32_v_uvw_sv.v;
    } else {
        out->svpwm.v_max = out->f32_v_uvw_sv.v;
        out->svpwm.v_min = out->f32_v_uvw_sv.u;
    }
    if (out->f32_v_uvw_sv.w < out->svpwm.v_min)
        out->svpwm.v_min = out->f32_v_uvw_sv.w;
    else if (out->f32_v_uvw_sv.w > out->svpwm.v_max)
        out->svpwm.v_max = out->f32_v_uvw_sv.w;

    out->svpwm.v_avg = (out->svpwm.v_max + out->svpwm.v_min) * 0.5F;

    UVW_SUB(out->svpwm.f32_pwm_duty_raw, out->f32_v_uvw_sv, out->svpwm.v_avg);
    UVW_ADD(out->svpwm.f32_pwm_duty, out->svpwm.f32_pwm_duty_raw, 0.5F);
    UVW_CLAMP(out->svpwm.f32_pwm_duty,
              cfg->base_cfg.periph.f32_pwm_duty_min,
              cfg->base_cfg.periph.f32_pwm_duty_max);

    UVW_MUL(out->f32_v_uvw, out->svpwm.f32_pwm_duty, in->v_bus);

    out->svpwm.u32_pwm_duty.u =
        (uint32_t)(out->svpwm.f32_pwm_duty.u * cfg->base_cfg.periph.pwm_full_cnt);
    out->svpwm.u32_pwm_duty.v =
        (uint32_t)(out->svpwm.f32_pwm_duty.v * cfg->base_cfg.periph.pwm_full_cnt);
    out->svpwm.u32_pwm_duty.w =
        (uint32_t)(out->svpwm.f32_pwm_duty.w * cfg->base_cfg.periph.pwm_full_cnt);
}
