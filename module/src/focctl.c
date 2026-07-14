#include "macrodef.h"
#include "mathdef.h"

#include "focdef.h"

/**
 * @brief 切换 FOC 控制模式时的操作
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_select_mode(foc_t *foc)
{
        DECL(foc, in, lo, out, tmp);

        /* 若当前控制模式与上一次控制模式相同, 则直接返回, 不做处理 */
        if (lo->e_mode == tmp->e_prev_mode)
                return;

        /* 参考 PVCT 变量置零, 避免出现指令残留的情况 */
        memset(&in->ref_pvct, 0, sizeof(in->ref_pvct));

        /* 位置/PD 控制模式将当前位置设置为参考位置, 避免出现瞬间回零阶跃的情况 */
        switch (lo->e_mode) {
                case FOC_MODE_POS:
                case FOC_MODE_PD:
                case FOC_MODE_RATCHET: {
                        in->ref_pvct.pos       = out->fdb_pvct.pos;
                        lo->ratchet_anchor_pos = out->fdb_pvct.pos;
                        lo->ratchet_detent_pos = out->fdb_pvct.pos;
                        lo->ratchet_idx        = 0;
                        break;
                }
                case FOC_MODE_HM: {
                        lo->pos_p.lo.prev_ref = out->fdb_pvct.pos;
                        break;
                }
                default:
                        break;
        }

        tmp->e_prev_mode = lo->e_mode;
}

/**
 * @brief 电压控制环
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_vol_ctl(foc_t *foc)
{
        ARG_UNUSED(foc);
}

/**
 * @brief 电流控制环
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_cur_ctl(foc_t *foc)
{
        DECL(foc, cfg, in, out, lo, tmp);
        RENAME(&lo->iq_pi, iq_pi);
        RENAME(&lo->id_pi, id_pi);

        f32 v_dq_max, vq_max;

        if (++tmp->freq_div_cnt.cur < cfg->ctl_cfg.freq_div.cur)
                return;

        tmp->freq_div_cnt.cur = 0;

        lo->ref_i_dq.q = in->ref_pvct.cur + lo->comp_i_dq.q;

        v_dq_max = in->v_bus / SQRT_3 * cfg->base_cfg.periph.f32_pwm_duty_max;

        /* d 轴电流环 */
        id_pi->cfg.out_max = id_pi->cfg.ki_out_max = v_dq_max;
        id_pi->cfg.out_min = id_pi->cfg.ki_out_min = -id_pi->cfg.out_max;

        lo->ffd_v_dq.d = -in->rotor.elec_omega * cfg->base_cfg.motor.lq * in->stator.i_dq.q * 0.7f;
        pid_parallel_exec_in(id_pi, lo->ref_i_dq.d, in->stator.i_dq.d, lo->ffd_v_dq.d);
        out->v_dq.d = id_pi->out.u;

        /* q 轴电流环: 使用 d 轴后剩余的电压圆半径, 避免 dq 合成电压超限 */
        vq_max             = (ABS(out->v_dq.d) < v_dq_max) ? SQRT(SQ(v_dq_max) - SQ(out->v_dq.d)) : 0.0f;
        iq_pi->cfg.out_max = iq_pi->cfg.ki_out_max = vq_max;
        iq_pi->cfg.out_min = iq_pi->cfg.ki_out_min = -iq_pi->cfg.out_max;

        lo->ffd_v_dq.q = in->rotor.elec_omega * cfg->base_cfg.motor.psi * 0.7f;
        pid_parallel_exec_in(iq_pi, lo->ref_i_dq.q, in->stator.i_dq.q, lo->ffd_v_dq.q);
        out->v_dq.q = iq_pi->out.u;
}

/**
 * @brief 力矩控制环
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_tor_ctl(foc_t *foc)
{
        DECL(foc, cfg, in, lo, out, tmp);

        if (++tmp->freq_div_cnt.tor < cfg->ctl_cfg.freq_div.tor)
                return;

        tmp->freq_div_cnt.tor = 0;

        if (cfg->sensor_cfg.tor_sensor_enable) {
                pid_parallel_exec_in(&lo->tor_pi, in->ref_pvct.tor, out->fdb_pvct.load_tor, in->ref_pvct.ffd_cur);
                in->ref_pvct.cur = lo->tor_pi.out.u;
        }
}

/**
 * @brief 速度控制环
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_vel_ctl(foc_t *foc)
{
        DECL(foc, cfg, in, lo, out, tmp);

        if (++tmp->freq_div_cnt.vel < cfg->ctl_cfg.freq_div.vel)
                return;

        tmp->freq_div_cnt.vel = 0;

        lo->vel_pi.cfg.out_max = lo->vel_pi.cfg.ki_out_max = cfg->ctl_cfg.vel_pi.out_max;
        lo->vel_pi.cfg.out_min = lo->vel_pi.cfg.ki_out_min = -lo->vel_pi.cfg.out_max;

        pid_parallel_exec_in(&lo->vel_pi,
                             CLAMP_RET(foc_vel_to_rads(in->ref_pvct.vel, cfg->base_cfg.e_vel_unit),
                                       -cfg->base_cfg.motor.vel_peak,
                                       cfg->base_cfg.motor.vel_peak),
                             foc_vel_to_rads(out->fdb_pvct.vel, cfg->base_cfg.e_vel_unit),
                             in->ref_pvct.ffd_tor);
        in->ref_pvct.cur = lo->vel_pi.out.u;
}

/**
 * @brief 位置控制环
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_pos_ctl(foc_t *foc)
{
        DECL(foc, cfg, in, lo, out, tmp);

        if (++tmp->freq_div_cnt.pos < cfg->ctl_cfg.freq_div.pos)
                return;

        tmp->freq_div_cnt.pos = 0;

        lo->pos_p.cfg.out_max = lo->pos_p.cfg.ki_out_max = cfg->ctl_cfg.pos_p.out_max;
        lo->pos_p.cfg.out_min = lo->pos_p.cfg.ki_out_min = -lo->pos_p.cfg.out_max;

        pid_parallel_exec_in(
            &lo->pos_p, in->ref_pvct.pos, out->fdb_pvct.pos, foc_vel_to_rads(in->ref_pvct.ffd_vel, cfg->base_cfg.e_vel_unit));
        in->ref_pvct.vel = foc_vel_from_rads(lo->pos_p.out.u, cfg->base_cfg.e_vel_unit);
}

/**
 * @brief PD 控制环
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_pd_ctl(foc_t *foc)
{
        DECL(foc, cfg, in, lo, out, tmp);

        if (++tmp->freq_div_cnt.pd < cfg->ctl_cfg.freq_div.pd)
                return;

        tmp->freq_div_cnt.pd = 0;

        lo->pos_vel_pd.cfg.out_max = lo->pos_vel_pd.cfg.ki_out_max = cfg->ctl_cfg.pos_vel_pd.out_max;
        lo->pos_vel_pd.cfg.out_min = lo->pos_vel_pd.cfg.ki_out_min = -lo->pos_vel_pd.cfg.out_max;

        pd_parallel_exec_in(&lo->pos_vel_pd,
                            in->ref_pvct.pos,
                            out->fdb_pvct.pos,
                            foc_vel_to_rads(in->ref_pvct.vel, cfg->base_cfg.e_vel_unit),
                            foc_vel_to_rads(out->fdb_pvct.vel, cfg->base_cfg.e_vel_unit),
                            in->ref_pvct.ffd_tor);
        in->ref_pvct.tor = lo->pos_vel_pd.out.u;
}

/**
 * @brief 棘轮控制环
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_ratchet_ctl(foc_t *foc)
{
        DECL(foc, cfg, in, lo, out, tmp);

        if (++tmp->freq_div_cnt.pd < cfg->ctl_cfg.freq_div.pd)
                return;

        tmp->freq_div_cnt.pd = 0;

        const f32 step_angle = cfg->ctl_cfg.ratchet.step_angle > 0.0f ? cfg->ctl_cfg.ratchet.step_angle : TAU / 12.0f;
        f32 switch_angle     = cfg->ctl_cfg.ratchet.switch_angle > 0.0f ? cfg->ctl_cfg.ratchet.switch_angle : step_angle * 0.5f;
        if (switch_angle >= step_angle)
                switch_angle = step_angle * 0.5f;

        const f32 delta_pos = out->fdb_pvct.pos - lo->ratchet_detent_pos;
        if (delta_pos >= switch_angle)
                lo->ratchet_idx += (i32)DOWN2INT((delta_pos - switch_angle) / step_angle) + 1;
        else if (delta_pos <= -switch_angle)
                lo->ratchet_idx -= (i32)DOWN2INT((-delta_pos - switch_angle) / step_angle) + 1;

        lo->ratchet_detent_pos = lo->ratchet_anchor_pos + (f32)lo->ratchet_idx * step_angle;
        in->ref_pvct.pos       = lo->ratchet_detent_pos;
        in->ref_pvct.vel       = 0.0f;

        const f32 kp      = cfg->ctl_cfg.ratchet.kp != 0.0f ? cfg->ctl_cfg.ratchet.kp : 1.0f;
        const f32 kd      = cfg->ctl_cfg.ratchet.kd;
        const f32 cur_max = cfg->ctl_cfg.ratchet.cur_max > 0.0f
                                ? cfg->ctl_cfg.ratchet.cur_max
                                : MIN(cfg->base_cfg.motor.cur_peak, cfg->base_cfg.periph.cur_max);
        const f32 pos_err = lo->ratchet_detent_pos - out->fdb_pvct.pos;
        const f32 vel     = foc_vel_to_rads(out->fdb_pvct.vel, cfg->base_cfg.e_vel_unit);
        const f32 ref_cur = kp * pos_err - kd * vel + in->ref_pvct.ffd_cur;

        in->ref_pvct.cur = CLAMP_RET(ref_cur, -cur_max, cur_max);
}

/**
 * @brief 出轴角度传感器回零
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_home_ctl(foc_t *foc)
{
        DECL(foc, cfg, in, lo, tmp);
        RENAME(in->rotor, rotor);

        if (++tmp->freq_div_cnt.pos < cfg->ctl_cfg.freq_div.pos)
                return;

        tmp->freq_div_cnt.pos = 0;

        lo->pos_p.cfg              = cfg->ctl_cfg.pos_p;
        lo->pos_p.cfg.ref_rate_max = cfg->base_cfg.home_vel;

        pid_parallel_exec_in(&lo->pos_p, 0.0f, rotor.outshaft_theta_total, 0.0f);
        in->ref_pvct.vel = foc_vel_from_rads(lo->pos_p.out.u, cfg->base_cfg.e_vel_unit);
}

/**
 * @brief 弱磁环 MODE 1 (上一环路为速度环)
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_flux_week_ctl(foc_t *foc)
{
        DECL(foc, cfg, in, lo, tmp);
        RENAME(in->rotor, rotor);
        RENAME(&lo->iq_pi, iq_pi);
        RENAME(&lo->id_pi, id_pi);

        if (++tmp->freq_div_cnt.flux_week < cfg->ctl_cfg.freq_div.flux_week)
                return;

        tmp->freq_div_cnt.flux_week = 0;

        in->stator.line_v_amp = SQRT(SQ(iq_pi->out.u_raw) + SQ(id_pi->out.u_raw)) * SQRT_3;

        lo->flux_week_pi.cfg.out_max = lo->flux_week_pi.cfg.ki_out_max = 0.0f;
        lo->flux_week_pi.cfg.out_min = lo->flux_week_pi.cfg.ki_out_min = -cfg->base_cfg.motor.cur_rated;

        pid_parallel_exec_in(&lo->flux_week_pi, in->v_bus * cfg->base_cfg.v_bus_rate, in->stator.line_v_amp, 0.0f);

        if (cfg->base_cfg.v_bus_rate != 0.0f)
                lo->ref_i_dq.d = lo->flux_week_pi.out.u;
}

/**
 * @brief 主动短路制动
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_asc_ctl(foc_t *foc)
{
        DECL(foc, cfg, out);

        cfg->func_cfg.f_set_pwm_status(PWM_CH_H, FALSE);
        cfg->func_cfg.f_set_pwm_status(PWM_CH_L, TRUE);

        u32_uvw_t uvw_pwm_cnt     = {0};
        out->svpwm.u32_pwm_duty.u = cfg->base_cfg.periph.pwm_full_cnt;
        out->svpwm.u32_pwm_duty.v = cfg->base_cfg.periph.pwm_full_cnt;
        out->svpwm.u32_pwm_duty.w = cfg->base_cfg.periph.pwm_full_cnt;
        cfg->func_cfg.f_set_pwm_duty(uvw_pwm_cnt, cfg->base_cfg.periph.pwm_full_cnt);
}
