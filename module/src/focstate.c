#include "focdef.h"
#include "mathdef.h"

extern void foc_cali(foc_t *foc);

extern void foc_obs_i_ab(foc_t *foc);
extern void foc_obs_i_dq(foc_t *foc);
extern void foc_obs_v_dq(foc_t *foc);
extern void foc_select_mode(foc_t *foc);

extern void foc_vol_ctl(foc_t *foc);
extern void foc_cur_ctl(foc_t *foc);
extern void foc_tor_ctl(foc_t *foc);
extern void foc_vel_ctl(foc_t *foc);
extern void foc_flux_week_ctl(foc_t *foc);
extern void foc_pos_ctl(foc_t *foc);
extern void foc_pd_ctl(foc_t *foc);
extern void foc_home_ctl(foc_t *foc);
extern void foc_asc_ctl(foc_t *foc);

void foc_disable(foc_t *foc);
void foc_enable(foc_t *foc);
void foc_fault(foc_t *foc);

static void foc_svpwm(foc_t *foc);

void
foc_select_state(foc_t *foc)
{
        DECL(foc, lo);

        if (!foc->in.stator.is_init || !foc->in.rotor.is_init)
                lo->e_state = FOC_STATE_DISABLE;

        switch (lo->e_state) {
                case FOC_STATE_DISABLE: {
                        foc_disable(foc);
                        break;
                }
                case FOC_STATE_ENABLE: {
                        foc_enable(foc);
                        break;
                }
                case FOC_STATE_CALI: {
                        foc_cali(foc);
                        break;
                }
                case FOC_STATE_FAULT: {
                        foc_fault(foc);
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
foc_select_theta(foc_t *foc)
{
        DECL(foc, cfg, in, lo);

        switch (lo->e_elec_theta) {
                case FOC_ELEC_THETA_FORCE: {
                        in->rotor.elec_theta = in->rotor.elec_force_theta;
                        in->rotor.elec_omega = in->rotor.elec_force_omega;
                        break;
                }
                case FOC_ELEC_THETA_SENSOR: {
                        WARP_TAU(in->rotor.elec_theta, in->rotor.motor_elec_theta - in->rotor.sensor.elec_offset_theta);
                        in->rotor.elec_omega = in->rotor.motor_elec_omega;
                        break;
                }
                case FOC_ELEC_THETA_SENSORLESS: {
                        in->rotor.elec_theta = in->rotor.elec_obs_theta;
                        in->rotor.elec_omega = in->rotor.elec_obs_omega;
                        break;
                }
                case FOC_ELEC_THETA_SENSORFUSION:
                        break;
                default:
                        break;
        }

        /* 观测器电角度和传感器电角度误差计算 */
        WARP_PI(in->rotor.elec_fusion_theta_err,
                in->rotor.motor_elec_theta - in->rotor.sensor.elec_offset_theta - in->rotor.elec_obs_theta);

        CYCLE_CNT(in->rotor.elec_cycle_cnt, in->rotor.elec_theta, in->rotor.elec_prev_theta);
        in->rotor.elec_total_theta = (f32)in->rotor.elec_cycle_cnt * TAU + in->rotor.elec_theta;
}

void
foc_fault(foc_t *foc)
{
        DECL(foc, cfg);

        cfg->func_cfg.f_set_pwm_status(PWM_CH_ALL, FALSE);
}

/**
 * @brief FOC 失能状态
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_disable(foc_t *foc)
{
        DECL(foc, cfg, in, lo, tmp);

        cfg->func_cfg.f_set_pwm_status(PWM_CH_ALL, FALSE);

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

        lo->hfi.lo.e_polar_idf = HFI_POLAR_IDF_READY;
}

/**
 * @brief FOC 使能状态
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_enable(foc_t *foc)
{
        DECL(foc, cfg, in, out, lo);

        cfg->func_cfg.f_set_pwm_status(PWM_CH_ALL, TRUE);

        /* clarke 变换 */
        in->stator.i_ab = clarke_amp(in->stator.f32_i_uvw);
        in->stator.v_ab = clarke_amp(in->stator.f32_v_uvw);

        /* 无感观测器(iab) */
        foc_obs_i_ab(foc);

        /* 电角度源选择 */
        foc_select_theta(foc);

        /* park 变换 */
        in->stator.i_dq = park(in->stator.i_ab, in->rotor.elec_theta);
        in->stator.i_s  = SQRT(SQ(in->stator.i_dq.d) + SQ(in->stator.i_dq.q));

        in->stator.v_dq = park(in->stator.v_ab, in->rotor.elec_theta);
        in->stator.v_s  = SQRT(SQ(in->stator.v_dq.d) + SQ(in->stator.v_dq.q));

        in->i_bus = DIV_3_BY_2 * (in->stator.i_dq.d * in->stator.v_dq.d + in->stator.i_dq.q * in->stator.v_dq.q) / in->v_bus;

        /* 无感观测器(idq) */
        foc_obs_i_dq(foc);

        /* 控制模式选择 */
        foc_select_mode(foc);

        switch (lo->e_mode) {
                case FOC_MODE_VOL: {
                        foc_vol_ctl(foc);
                        break;
                }
                case FOC_MODE_CUR: {
                        foc_cur_ctl(foc);
                        foc_vol_ctl(foc);
                        break;
                }
                case FOC_MODE_TOR: {
                        foc_tor_ctl(foc);
                        foc_cur_ctl(foc);
                        foc_vol_ctl(foc);
                        break;
                }
                case FOC_MODE_VEL: {
                        foc_vel_ctl(foc);
                        foc_flux_week_ctl(foc);
                        foc_cur_ctl(foc);
                        foc_vol_ctl(foc);
                        break;
                }
                case FOC_MODE_POS: {
                        foc_pos_ctl(foc);
                        foc_vel_ctl(foc);
                        foc_cur_ctl(foc);
                        foc_vol_ctl(foc);
                        break;
                }
                case FOC_MODE_PD: {
                        foc_pd_ctl(foc);
                        foc_cur_ctl(foc);
                        foc_vol_ctl(foc);
                        break;
                }
                case FOC_MODE_HM: {
                        foc_home_ctl(foc);
                        foc_vel_ctl(foc);
                        foc_cur_ctl(foc);
                        foc_vol_ctl(foc);
                        break;
                }
                case FOC_MODE_ASC: {
                        foc_asc_ctl(foc);
                        return;
                }
                default:
                        break;
        }

        /* 无感观测器(vdq) */
        foc_obs_v_dq(foc);

        /* 补偿 PWM 采样造成的延迟 */
        if (lo->e_elec_theta != FOC_ELEC_THETA_FORCE) {
                in->rotor.elec_comp_theta =
                    cfg->sensor_cfg.elec_theta_delay_comp_cycle * in->rotor.elec_omega / cfg->base_cfg.fs;
                WARP_TAU(in->rotor.elec_theta, in->rotor.elec_theta + in->rotor.elec_comp_theta);
        }

        /* 反 park 变换 */
        out->v_ab = inv_park(out->v_dq, in->rotor.elec_theta);

        /* 标幺 */
        out->v_ab_sv.a = out->v_ab.a / in->v_bus;
        out->v_ab_sv.b = out->v_ab.b / in->v_bus;

        /* 调制发波 */
        foc_svpwm(foc);
        cfg->func_cfg.f_set_pwm_duty(out->svpwm.u32_pwm_duty, cfg->base_cfg.periph.pwm_full_cnt);
}

/**
 * @brief SVPWM 单次执行计算
 *
 * @param foc FOC 结构体
 * @return    void
 */
static void
foc_svpwm(foc_t *foc)
{
        DECL(foc, cfg, in, out);

        out->f32_v_uvw_sv = inv_clarke(out->v_ab_sv);

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

        out->svpwm.v_avg = (out->svpwm.v_max + out->svpwm.v_min) * 0.5f;

        UVW_SUB(out->svpwm.f32_pwm_duty_raw, out->f32_v_uvw_sv, out->svpwm.v_avg);
        UVW_ADD(out->svpwm.f32_pwm_duty, out->svpwm.f32_pwm_duty_raw, 0.5f);
        UVW_CLAMP(out->svpwm.f32_pwm_duty, cfg->base_cfg.periph.f32_pwm_duty_min, cfg->base_cfg.periph.f32_pwm_duty_max);

        UVW_MUL(out->f32_v_uvw, out->svpwm.f32_pwm_duty, in->v_bus);

        UVW_MUL(out->svpwm.u32_pwm_duty, out->svpwm.f32_pwm_duty, cfg->base_cfg.periph.pwm_full_cnt);
}
