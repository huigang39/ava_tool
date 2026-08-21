#include "macrodef.h"
#include "mathdef.h"

#include "motor_control/foc.h"
#include "motor_control/focdef.h"

/**
 * @brief 出轴目标力矩转换为 q 轴前馈电流
 *
 * tor2cur
 * 使用力矩绝对值进行拟合,最终恢复目标力矩的符号.

 */
static float32_t
foc_tor_to_cur_rt(const struct foc *foc, const float32_t tor)
{
    const struct foc_cfg *cfg = &foc->cfg;

    if (tor == 0.0F) {
        return 0.0F;
    }

    return CPYSGN(
        poly_eval_rt(cfg->base_cfg.tor2cur, ARRAY_LEN(cfg->base_cfg.tor2cur) - 1, ABS(tor)), tor);
}

/**
 * @brief 切换 FOC 控制模式时的操作
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_select_mode_rt(struct foc *foc)
{
    DECL(foc, in, lo, out, tmp);

    /* 若当前控制模式与上一次控制模式相同, 则直接返回, 不做处理 */
    if (lo->e_mode == tmp->e_prev_mode) {
        return;
    }

    /* 参考 PVCT 变量置零, 避免出现指令残留的情况 */
    memset(&in->ref_pvct, 0, sizeof(in->ref_pvct));

    /* I/F、V/F 固定使用强拖电角度；退出开环模式时恢复原角度源 */
    switch (lo->e_mode) {
        case FOC_MODE_IF: {
            in->rotor.elec_theta_force = in->rotor.elec_theta;
            in->rotor.elec_omega_force = 0.0F;
            lo->if_ctl.vel_ref_limited = 0.0F;
            break;
        }
        case FOC_MODE_VF: {
            in->rotor.elec_theta_force = in->rotor.elec_theta;
            in->rotor.elec_omega_force = 0.0F;
            lo->vf_ctl.vel_ref_limited = 0.0F;
            lo->vf_ctl.volt_ref        = 0.0F;
            break;
        }
        default: {
            if ((tmp->e_prev_mode == FOC_MODE_IF || tmp->e_prev_mode == FOC_MODE_VF) &&
                lo->e_state != FOC_STATE_CALI && lo->e_elec_theta == FOC_ELEC_THETA_FORCE) {
                (void)foc_set_elec_theta(foc, tmp->e_prev_elec_theta);
            }
            break;
        }
    }

    switch (lo->e_mode) {
        case FOC_MODE_POS: {
            in->ref_pvct.pos      = out->fdb_pvct.pos;
            lo->pos_p.lo.prev_ref = out->fdb_pvct.pos;
            break;
        }
        case FOC_MODE_PD: {
            in->ref_pvct.pos = out->fdb_pvct.pos;
            in->ref_pvct.vel = 0.0F;
            break;
        }
        case FOC_MODE_HM: {
            lo->pos_p.lo.prev_ref = out->fdb_pvct.pos;
            break;
        }
        case FOC_MODE_RATCHET: {
            in->ref_pvct.pos           = out->fdb_pvct.pos;
            lo->ratchet_ctl.anchor_pos = out->fdb_pvct.pos;
            lo->ratchet_ctl.detent_pos = out->fdb_pvct.pos;
            lo->ratchet_ctl.idx        = 0;
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
foc_vol_ctl_rt(struct foc *foc)
{
    ARG_UNUSED(foc);
}

/**
 * @brief I/F 强制电角度更新
 *
 * 电流矢量以目标速度对应的电频率旋转。进入 I/F
 * 模式时从当前控制角度接续，

 * 避免模式切换造成电角度突跳。
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_if_theta_rt(struct foc *foc)
{
    DECL(foc, cfg, in, lo);

    const float32_t ref_vel =
        CLAMP_RET(foc_vel_to_rads_rt(in->ref_pvct.vel, cfg->base_cfg.e_vel_unit),
                  -cfg->base_cfg.motor.vel_peak,
                  cfg->base_cfg.motor.vel_peak);
    const float32_t ref_change_max =
        cfg->base_cfg.periph.pwm_freq > 0
            ? ABS(lo->if_ctl.vel_acc_max) / (float32_t)cfg->base_cfg.periph.pwm_freq
            : 0.0F;

    const float32_t prev_ref   = lo->if_ctl.vel_ref_limited;
    lo->if_ctl.vel_ref_limited = ref_vel;
    if (ref_change_max > 0.0F) {
        const float32_t ref_change = ref_vel - prev_ref;
        if (ref_change > ref_change_max) {
            lo->if_ctl.vel_ref_limited = prev_ref + ref_change_max;
        } else if (ref_change < -ref_change_max) {
            lo->if_ctl.vel_ref_limited = prev_ref - ref_change_max;
        }
    }

    /* 只更新强拖状态，不反写外部目标指令。 */
    in->rotor.elec_omega_force = OUTSHAFT2ELEC(
        lo->if_ctl.vel_ref_limited, cfg->base_cfg.reducer.outshaft_ratio, cfg->base_cfg.motor.npp);
    if (cfg->base_cfg.periph.pwm_freq > 0) {
        WARP_TAU(in->rotor.elec_theta_force,
                 in->rotor.elec_theta_force +
                     in->rotor.elec_omega_force / (float32_t)cfg->base_cfg.periph.pwm_freq);
    }
}

/**
 * @brief V/F 强制电角度和速度斜坡更新
 */
void
foc_vf_theta_rt(struct foc *foc)
{
    DECL(foc, cfg, in, lo);

    const float32_t ref_vel =
        CLAMP_RET(foc_vel_to_rads_rt(in->ref_pvct.vel, cfg->base_cfg.e_vel_unit),
                  -cfg->base_cfg.motor.vel_peak,
                  cfg->base_cfg.motor.vel_peak);
    const float32_t ref_change_max =
        cfg->base_cfg.periph.pwm_freq > 0
            ? ABS(lo->vf_ctl.vel_acc_max) / (float32_t)cfg->base_cfg.periph.pwm_freq
            : 0.0F;
    const float32_t prev_ref = lo->vf_ctl.vel_ref_limited;

    lo->vf_ctl.vel_ref_limited = ref_vel;
    if (ref_change_max > 0.0F) {
        const float32_t ref_change = ref_vel - prev_ref;
        if (ref_change > ref_change_max) {
            lo->vf_ctl.vel_ref_limited = prev_ref + ref_change_max;
        } else if (ref_change < -ref_change_max) {
            lo->vf_ctl.vel_ref_limited = prev_ref - ref_change_max;
        }
    }

    in->rotor.elec_omega_force = OUTSHAFT2ELEC(
        lo->vf_ctl.vel_ref_limited, cfg->base_cfg.reducer.outshaft_ratio, cfg->base_cfg.motor.npp);
    if (cfg->base_cfg.periph.pwm_freq > 0) {
        WARP_TAU(in->rotor.elec_theta_force,
                 in->rotor.elec_theta_force +
                     in->rotor.elec_omega_force / (float32_t)cfg->base_cfg.periph.pwm_freq);
    }
}

/**
 * @brief V/F 电压指令生成
 */
void
foc_vf_ctl_rt(struct foc *foc)
{
    DECL(foc, cfg, in, out, lo);

    const float32_t freq_hz     = ABS(in->rotor.elec_omega_force) / TAU;
    const float32_t volt_boost  = cfg->ctl_cfg.vf.volt_boost > 0.0F
                                      ? cfg->ctl_cfg.vf.volt_boost
                                      : cfg->base_cfg.motor.rs * cfg->base_cfg.motor.cur_rated;
    const float32_t volt_per_hz = cfg->ctl_cfg.vf.volt_per_hz > 0.0F
                                      ? cfg->ctl_cfg.vf.volt_per_hz
                                      : TAU * cfg->base_cfg.motor.psi;
    const float32_t inverter_volt_max =
        in->v_bus * DIV_1_BY_SQRT_3 * cfg->base_cfg.periph.f32_pwm_duty_max;
    const float32_t volt_max = cfg->ctl_cfg.vf.volt_max > 0.0F
                                   ? MIN(cfg->ctl_cfg.vf.volt_max, inverter_volt_max)
                                   : inverter_volt_max;

    lo->vf_ctl.volt_ref = freq_hz > 0.0F ? MIN(volt_boost + volt_per_hz * freq_hz, volt_max) : 0.0F;
    out->v_dq.d         = 0.0F;
    out->v_dq.q         = CPYSGN(lo->vf_ctl.volt_ref, in->rotor.elec_omega_force);
}

/**
 * @brief I/F 电流指令生成
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_if_ctl_rt(struct foc *foc)
{
    DECL(foc, cfg, in, lo);

    const float32_t cur_max = MIN(cfg->base_cfg.motor.cur_peak, cfg->base_cfg.periph.cur_max);
    lo->ref_i_dq.d          = CLAMP_RET(ABS(in->ref_pvct.cur), 0.0F, cur_max) + lo->comp_i_dq.d;
    lo->ref_i_dq.q          = lo->comp_i_dq.q;
}

/**
 * @brief 电流控制环
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_cur_ctl_rt(struct foc *foc)
{
    DECL(foc, cfg, in, out, lo, tmp);
    RENAME(&lo->iq_pi, iq_pi);
    RENAME(&lo->id_pi, id_pi);
    RENAME(&lo->iq_adrc, iq_adrc);
    RENAME(&lo->id_adrc, id_adrc);

    float32_t v_dq_max, vq_max;

    if (++tmp->freq_div_cnt.cur < cfg->ctl_cfg.freq_div.cur) {
        return;
    }

    tmp->freq_div_cnt.cur = 0;

    if (lo->e_mode != FOC_MODE_IF) {
        lo->ref_i_dq.q = in->ref_pvct.cur + lo->comp_i_dq.q;
    }

    v_dq_max = in->v_bus * DIV_1_BY_SQRT_3 * cfg->base_cfg.periph.f32_pwm_duty_max;

    /* d 轴电流环 */
    lo->ffd_v_dq.d = -in->rotor.elec_omega * cfg->base_cfg.motor.lq * in->stator.i_dq.q * 0.7F;
    if (cfg->ctl_cfg.e_cur_ctl == FOC_CTL_ADRC) {
        adrc_set_out_limit(id_adrc, v_dq_max, -v_dq_max);
        adrc_exec_in_rt(id_adrc, lo->ref_i_dq.d, in->stator.i_dq.d, lo->ffd_v_dq.d);
        out->v_dq.d = id_adrc->out.u;
    } else {
        pid_set_out_limit(id_pi, v_dq_max, v_dq_max, v_dq_max, -v_dq_max, -v_dq_max, -v_dq_max);
        pid_parallel_exec_in_rt(id_pi, lo->ref_i_dq.d, in->stator.i_dq.d, lo->ffd_v_dq.d);
        out->v_dq.d = id_pi->out.u;
    }

    /* q 轴电流环: 使用 d 轴后剩余的电压圆半径, 避免 dq 合成电压超限 */
    vq_max         = (ABS(out->v_dq.d) < v_dq_max) ? SQRT(SQ(v_dq_max) - SQ(out->v_dq.d)) : 0.0F;
    lo->ffd_v_dq.q = in->rotor.elec_omega * cfg->base_cfg.motor.psi * 0.7F;
    if (cfg->ctl_cfg.e_cur_ctl == FOC_CTL_ADRC) {
        adrc_set_out_limit(iq_adrc, vq_max, -vq_max);
        adrc_exec_in_rt(iq_adrc, lo->ref_i_dq.q, in->stator.i_dq.q, lo->ffd_v_dq.q);
        out->v_dq.q = iq_adrc->out.u;
    } else {
        pid_set_out_limit(iq_pi, vq_max, vq_max, vq_max, -vq_max, -vq_max, -vq_max);
        pid_parallel_exec_in_rt(iq_pi, lo->ref_i_dq.q, in->stator.i_dq.q, lo->ffd_v_dq.q);
        out->v_dq.q = iq_pi->out.u;
    }
}

/**
 * @brief 力矩控制环
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_tor_ctl_rt(struct foc *foc)
{
    DECL(foc, cfg, in, lo, out, tmp);
    float32_t cur_ff;

    if (++tmp->freq_div_cnt.tor < cfg->ctl_cfg.freq_div.tor) {
        return;
    }

    tmp->freq_div_cnt.tor = 0;

    /* 模型前馈为主,外部电流前馈用于附加补偿. */
    cur_ff = foc_tor_to_cur_rt(foc, in->ref_pvct.tor) + in->ref_pvct.ffd_cur;

    if (cfg->sensor_cfg.tor_sensor_enable) {
        /* PI 仅修正力矩模型,摩擦和负载扰动造成的残余误差. */
        pid_parallel_exec_in_rt(&lo->tor_pi, in->ref_pvct.tor, -out->fdb_pvct.load_tor, cur_ff);
        in->ref_pvct.cur = lo->tor_pi.out.u;
    } else {
        in->ref_pvct.cur = cur_ff;
        CLAMP(in->ref_pvct.cur, lo->tor_pi.cfg.out_min, lo->tor_pi.cfg.out_max);
    }
}

/**
 * @brief 速度控制环
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_vel_ctl_rt(struct foc *foc)
{
    DECL(foc, cfg, in, lo, out, tmp);

    if (++tmp->freq_div_cnt.vel < cfg->ctl_cfg.freq_div.vel) {
        return;
    }

    tmp->freq_div_cnt.vel = 0;

    const float32_t ref_vel =
        CLAMP_RET(foc_vel_to_rads_rt(in->ref_pvct.vel, cfg->base_cfg.e_vel_unit),
                  -cfg->base_cfg.motor.vel_peak,
                  cfg->base_cfg.motor.vel_peak);
    const float32_t fdb_vel = foc_vel_to_rads_rt(out->fdb_pvct.vel, cfg->base_cfg.e_vel_unit);

    if (cfg->ctl_cfg.e_vel_ctl == FOC_CTL_ADRC) {
        adrc_exec_in_rt(&lo->vel_adrc, ref_vel, fdb_vel, in->ref_pvct.ffd_cur);
        in->ref_pvct.cur = lo->vel_adrc.out.u;
    } else {
        pid_parallel_exec_in_rt(&lo->vel_pi, ref_vel, fdb_vel, in->ref_pvct.ffd_cur);
        in->ref_pvct.cur = lo->vel_pi.out.u;
    }
}

/**
 * @brief 位置控制环
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_pos_ctl_rt(struct foc *foc)
{
    DECL(foc, cfg, in, lo, out, tmp);

    if (++tmp->freq_div_cnt.pos < cfg->ctl_cfg.freq_div.pos) {
        return;
    }

    tmp->freq_div_cnt.pos = 0;

    pid_parallel_exec_in_rt(&lo->pos_p,
                            in->ref_pvct.pos,
                            out->fdb_pvct.pos,
                            foc_vel_to_rads_rt(in->ref_pvct.ffd_vel, cfg->base_cfg.e_vel_unit));
    in->ref_pvct.vel = foc_vel_from_rads_rt(lo->pos_p.out.u, cfg->base_cfg.e_vel_unit);
}

/**
 * @brief PD 控制环
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_pd_ctl_rt(struct foc *foc)
{
    DECL(foc, cfg, in, lo, out, tmp);

    if (++tmp->freq_div_cnt.pd < cfg->ctl_cfg.freq_div.pd) {
        return;
    }

    tmp->freq_div_cnt.pd = 0;

    pd_parallel_exec_in_rt(&lo->pos_vel_pd,
                           in->ref_pvct.pos,
                           out->fdb_pvct.pos,
                           foc_vel_to_rads_rt(in->ref_pvct.vel, cfg->base_cfg.e_vel_unit),
                           foc_vel_to_rads_rt(out->fdb_pvct.vel, cfg->base_cfg.e_vel_unit),
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
foc_ratchet_ctl_rt(struct foc *foc)
{
    DECL(foc, cfg, in, lo, out, tmp);

    if (++tmp->freq_div_cnt.pd < cfg->ctl_cfg.freq_div.pd) {
        return;
    }

    tmp->freq_div_cnt.pd = 0;

    const float32_t step_angle =
        cfg->ctl_cfg.ratchet.step_angle > 0.0F ? cfg->ctl_cfg.ratchet.step_angle : TAU / 12.0F;
    float32_t switch_angle = cfg->ctl_cfg.ratchet.switch_angle > 0.0F
                                 ? cfg->ctl_cfg.ratchet.switch_angle
                                 : step_angle * 0.5F;
    if (switch_angle >= step_angle) {
        switch_angle = step_angle * 0.5F;
    }

    const float32_t delta_pos = out->fdb_pvct.pos - lo->ratchet_ctl.detent_pos;
    if (delta_pos >= switch_angle)
        lo->ratchet_ctl.idx += (int32_t)DOWN2INT((delta_pos - switch_angle) / step_angle) + 1;
    else if (delta_pos <= -switch_angle)
        lo->ratchet_ctl.idx -= (int32_t)DOWN2INT((-delta_pos - switch_angle) / step_angle) + 1;

    lo->ratchet_ctl.detent_pos =
        lo->ratchet_ctl.anchor_pos + (float32_t)lo->ratchet_ctl.idx * step_angle;
    in->ref_pvct.pos = lo->ratchet_ctl.detent_pos;
    in->ref_pvct.vel = 0.0F;

    const float32_t kp      = cfg->ctl_cfg.ratchet.kp != 0.0F ? cfg->ctl_cfg.ratchet.kp : 1.0F;
    const float32_t kd      = cfg->ctl_cfg.ratchet.kd;
    const float32_t cur_max = cfg->ctl_cfg.ratchet.cur_max > 0.0F
                                  ? cfg->ctl_cfg.ratchet.cur_max
                                  : MIN(cfg->base_cfg.motor.cur_peak, cfg->base_cfg.periph.cur_max);
    const float32_t pos_err = lo->ratchet_ctl.detent_pos - out->fdb_pvct.pos;
    const float32_t vel     = foc_vel_to_rads_rt(out->fdb_pvct.vel, cfg->base_cfg.e_vel_unit);
    const float32_t ref_cur = kp * pos_err - kd * vel + in->ref_pvct.ffd_cur;

    in->ref_pvct.cur = CLAMP_RET(ref_cur, -cur_max, cur_max);
}

/**
 * @brief 出轴角度传感器回零
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_home_ctl_rt(struct foc *foc)
{
    DECL(foc, cfg, in, lo, tmp);
    RENAME(in->rotor, rotor);

    if (++tmp->freq_div_cnt.pos < cfg->ctl_cfg.freq_div.pos) {
        return;
    }

    tmp->freq_div_cnt.pos = 0;

    // TODO: 单独建一个回零模式 PID
    lo->pos_p.cfg              = cfg->ctl_cfg.pos_p;
    lo->pos_p.cfg.ref_rate_max = cfg->base_cfg.home_vel;

    pid_parallel_exec_in_rt(&lo->pos_p, 0.0F, rotor.outshaft_theta_total, 0.0F);
    in->ref_pvct.vel = foc_vel_from_rads_rt(lo->pos_p.out.u, cfg->base_cfg.e_vel_unit);
}

/**
 * @brief 弱磁环 MODE 1 (上一环路为速度环)
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_flux_week_ctl_rt(struct foc *foc)
{
    DECL(foc, cfg, in, lo, tmp);
    RENAME(in->rotor, rotor);
    RENAME(&lo->iq_pi, iq_pi);
    RENAME(&lo->id_pi, id_pi);
    RENAME(&lo->iq_adrc, iq_adrc);
    RENAME(&lo->id_adrc, id_adrc);

    if (++tmp->freq_div_cnt.flux_week < cfg->ctl_cfg.freq_div.flux_week) {
        return;
    }

    tmp->freq_div_cnt.flux_week = 0;

    if (cfg->ctl_cfg.e_cur_ctl == FOC_CTL_ADRC) {
        in->stator.line_v_amp = SQRT(SQ(iq_adrc->out.u_raw) + SQ(id_adrc->out.u_raw)) * SQRT_3;
    } else {
        in->stator.line_v_amp = SQRT(SQ(iq_pi->out.u_raw) + SQ(id_pi->out.u_raw)) * SQRT_3;
    }

    pid_set_out_limit(&lo->flux_week_pi,
                      0.0F,
                      0.0F,
                      0.0F,
                      -cfg->base_cfg.motor.cur_rated,
                      -cfg->base_cfg.motor.cur_rated,
                      -cfg->base_cfg.motor.cur_rated);

    pid_parallel_exec_in_rt(
        &lo->flux_week_pi, in->v_bus * cfg->base_cfg.v_bus_rate, in->stator.line_v_amp, 0.0F);

    if (cfg->base_cfg.v_bus_rate != 0.0F) {
        lo->ref_i_dq.d = lo->flux_week_pi.out.u;
    }
}

/**
 * @brief 主动短路制动
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_asc_ctl_rt(struct foc *foc)
{
    DECL(foc, cfg, out);

    cfg->func_cfg.f_set_pwm_status(PWM_CH_H, false);
    cfg->func_cfg.f_set_pwm_status(PWM_CH_L, true);

    struct u32_uvw uvw_pwm_cnt = {0};
    out->svpwm.u32_pwm_duty.u  = cfg->base_cfg.periph.pwm_full_cnt;
    out->svpwm.u32_pwm_duty.v  = cfg->base_cfg.periph.pwm_full_cnt;
    out->svpwm.u32_pwm_duty.w  = cfg->base_cfg.periph.pwm_full_cnt;
    cfg->func_cfg.f_set_pwm_duty(uvw_pwm_cnt, cfg->base_cfg.periph.pwm_full_cnt);
}
