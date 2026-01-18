#include "clarkepark.h"
#include "mathdef.h"

#include "foc.h"

extern void foc_obs_i_ab(foc_t *foc);
extern void foc_obs_i_dq(foc_t *foc);
extern void foc_obs_v_dq(foc_t *foc);
extern void foc_select_mode(foc_t *foc);

extern void foc_vol_ctl(foc_t *foc);
extern void foc_cur_ctl(foc_t *foc);
extern void foc_vel_ctl(foc_t *foc);
extern void foc_pos_ctl(foc_t *foc);
extern void foc_pd_ctl(foc_t *foc);

void foc_svpwm(foc_t *foc);
void foc_select_theta(foc_t *foc);
void foc_ready(foc_t *foc);
void foc_disable(foc_t *foc);
void foc_enable(foc_t *foc);

void
foc_svpwm(foc_t *foc)
{
        DECL(foc, cfg, out);

        out->f32_v_uvw = inv_clarke(out->v_ab_sv);

        if (out->f32_v_uvw.u > out->f32_v_uvw.v) {
                out->svpwm.v_max = out->f32_v_uvw.u;
                out->svpwm.v_min = out->f32_v_uvw.v;
        } else {
                out->svpwm.v_max = out->f32_v_uvw.v;
                out->svpwm.v_min = out->f32_v_uvw.u;
        }
        if (out->f32_v_uvw.w < out->svpwm.v_min)
                out->svpwm.v_min = out->f32_v_uvw.w;
        else if (out->f32_v_uvw.w > out->svpwm.v_max)
                out->svpwm.v_max = out->f32_v_uvw.w;

        out->svpwm.v_avg = 0.5f * (out->svpwm.v_max + out->svpwm.v_min);
        UVW_ADD(out->svpwm.f32_pwm_duty, out->f32_v_uvw, -out->svpwm.v_avg);

        UVW_ADD(out->svpwm.f32_pwm_duty, out->svpwm.f32_pwm_duty, 0.5f);
        UVW_CLAMP(out->svpwm.f32_pwm_duty, cfg->base_cfg.periph.f32_pwm_min, cfg->base_cfg.periph.f32_pwm_max);
        UVW_MUL(out->svpwm.u32_pwm_duty, out->svpwm.f32_pwm_duty, cfg->base_cfg.periph.pwm_full_cnt);
}

void
foc_select_theta(foc_t *foc)
{
        DECL(foc, cfg, in, lo);

        switch (lo->e_theta) {
                case FOC_THETA_FORCE: {
                        in->rotor.theta = in->rotor.force_theta;
                        in->rotor.omega = in->rotor.force_omega;
                        break;
                }
                case FOC_THETA_SENSOR: {
                        WARP_TAU(in->rotor.theta, in->rotor.elec_theta - lo->store.offset.theta);
                        in->rotor.omega = in->rotor.elec_omega;
                        break;
                }
                case FOC_THETA_SENSORLESS: {
                        in->rotor.theta = in->rotor.obs_theta;
                        in->rotor.omega = in->rotor.obs_omega;
                        break;
                }
                case FOC_THETA_SENSORFUSION:
                        break;
                default:
                        break;
        }
}

void
foc_ready(foc_t *foc)
{
        ARG_UNUSED(foc);
}

void
foc_disable(foc_t *foc)
{
        DECL(foc, cfg, in);

        cfg->func_cfg.f_set_pwm_status(PWM_CH_ALL, 0);

        memset(&in->i_ab, 0, sizeof(in->i_ab));
        memset(&in->i_dq, 0, sizeof(in->i_dq));

        RESET(foc, out);
        RESET(&foc->lo.smo, out);
        RESET(&foc->lo.hfi, out);

        RESET(&foc->lo.lbg, out, lo);
        RESET(&foc->lo.id_pid, out, lo);
        RESET(&foc->lo.iq_pid, out, lo);
        RESET(&foc->lo.vel_pid, out, lo);
        RESET(&foc->lo.pos_pid, out, lo);
        RESET(&foc->lo.pd_pid, out, lo);
}

void
foc_enable(foc_t *foc)
{
        DECL(foc, cfg, in, out, lo);

        cfg->func_cfg.f_set_pwm_status(PWM_CH_ALL, 1);

        // clarke变换
        in->i_ab = clarke(in->f32_i_uvw, cfg->base_cfg.periph.mi);

        // 无感观测器(iab)
        foc_obs_i_ab(foc);

        // 电角度源选择
        foc_select_theta(foc);

        // 帕克变换
        in->i_dq = park(in->i_ab, in->rotor.theta);

        // 无感观测器(idq)
        foc_obs_i_dq(foc);

        // 控制模式选择
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
                case FOC_MODE_VEL: {
                        foc_vel_ctl(foc);
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
                case FOC_MODE_ASC: {
                        foc_cur_ctl(foc);
                        foc_vol_ctl(foc);
                        break;
                }
                default:
                        break;
        }

        // 无感观测器(vdq)
        foc_obs_v_dq(foc);

        // 观测器电角度和传感器电角度误差计算
        WARP_PI(in->rotor.fusion_theta_err, in->rotor.elec_theta - in->rotor.obs_theta);

        // 补偿 PWM 采样造成的延迟
        in->rotor.comp_theta = cfg->sensor_cfg.theta_comp_gain * in->rotor.omega / cfg->base_cfg.exec_freq;
        WARP_TAU(in->rotor.theta, in->rotor.theta + in->rotor.comp_theta);

        // 反park变换
        out->v_ab = inv_park(out->v_dq, in->rotor.theta);

        // 标幺
        out->v_ab_sv.a = out->v_ab.a / in->v_bus;
        out->v_ab_sv.b = out->v_ab.b / in->v_bus;

        // 调制发波
        foc_svpwm(foc);
        cfg->func_cfg.f_set_pwm_duty(cfg->base_cfg.periph.pwm_full_cnt, out->svpwm.u32_pwm_duty);
}
