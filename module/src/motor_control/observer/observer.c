#include "mathdef.h"
#include "motor_control/foc.h"

#include "motor_control/focdef.h"

#define RLS_INERTIA_MIN_ACC   (1.0F)
#define RLS_INERTIA_MIN_OMEGA (1.0F)

static struct f32_ab
foc_obs_v_ab_rt(const struct foc *foc,
                const float32_t   sample_volt_max_omega,
                const float32_t   out_volt_min_omega)
{
    /* 没有端电压采样时使用控制器输出电压. */
    if (!foc->cfg.sensor_cfg.terminal_volt_sample_enable)
        return foc->out.v_ab;

    const float32_t omega            = ABS(foc->in.rotor.elec_omega);
    const float32_t sample_max_omega = MAX(sample_volt_max_omega, 0.0F);
    const float32_t out_min_omega    = MAX(out_volt_min_omega, sample_max_omega);

    float32_t out_weight;
    if (omega <= sample_max_omega)
        out_weight = 0.0F;
    else if (omega >= out_min_omega || out_min_omega == sample_max_omega)
        out_weight = 1.0F;
    else
        out_weight = (omega - sample_max_omega) / (out_min_omega - sample_max_omega);

    const float32_t sample_weight = 1.0F - out_weight;
    return (struct f32_ab){
        .a = sample_weight * foc->in.stator.v_ab.a + out_weight * foc->out.v_ab.a,
        .b = sample_weight * foc->in.stator.v_ab.b + out_weight * foc->out.v_ab.b,
    };
}

static float32_t
foc_obs_flux_theta_rt(const struct foc *foc)
{
    float32_t theta;
    WARP_TAU(theta, foc->lo.flux.out.est_theta + foc->lo.flux_theta_offset);
    return theta;
}

static float32_t
foc_obs_align_flux_rt(struct foc *foc, const uint8_t capture)
{
    DECL(foc, cfg, lo);

    float32_t theta_err;
    WARP_PI(theta_err, lo->hfi.out.est_theta - foc_obs_flux_theta_rt(foc));

    const float32_t fs    = cfg->ctl_cfg.iq_pi.fs;
    const float32_t wc    = MAX(cfg->obs_cfg.switch_align_wc, 0.0F);
    const float32_t alpha = capture || wc == 0.0F || fs <= 0.0F ? 1.0F : wc / (wc + fs);
    WARP_PI(lo->flux_theta_offset, lo->flux_theta_offset + alpha * theta_err);

    WARP_PI(theta_err, lo->hfi.out.est_theta - foc_obs_flux_theta_rt(foc));
    lo->obs_switch_theta_err = theta_err;
    return theta_err;
}

static float32_t
foc_obs_switch_step_rt(const struct foc *foc)
{
    const float32_t fs      = foc->cfg.ctl_cfg.iq_pi.fs;
    const float32_t time_ms = (float32_t)foc->cfg.obs_cfg.switch_time_ms;
    if (fs <= 0.0F || time_ms <= 0.0F)
        return 1.0F;
    return MIN(1000.0F / (time_ms * fs), 1.0F);
}

static void
foc_obs_sync_hfi_rt(struct foc *foc, const float32_t theta, const float32_t omega)
{
    RENAME(&foc->lo.hfi, hfi);
    RENAME(&hfi->lo.pll, pll);

    WARP_TAU(pll->out.theta, theta - hfi->lo.polar_offset);
    pll->out.omega     = omega;
    pll->out.lpf_omega = omega;
    pll->lo.ki_out     = omega;
    pll->lo.theta_err  = 0.0F;

    hfi->out.est_theta    = theta;
    hfi->out.est_omega    = omega;
    hfi->lo.lpf_i_dq.q    = 0.0F;
    hfi->lo.hfi_i_dq.q    = 0.0F;
    hfi->lo.est_i_dq      = park_rt(foc->in.stator.i_ab, pll->out.theta);
    hfi->lo.prev_est_i_dq = hfi->lo.est_i_dq;
}

/**
 * @brief 以 alpha-beta 轴电流为输入的观测器单次执行计算
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_obs_i_ab_rt(struct foc *foc)
{
    DECL(foc, cfg, in, out, lo);
    RENAME(&lo->hfi, hfi);
    RENAME(&lo->smo, smo);
    RENAME(&lo->flux, flux);

    const uint32_t hfi_enable  = cfg->obs_cfg.u_obs_flag.bit.hfi;
    const uint32_t flux_enable = cfg->obs_cfg.u_obs_flag.bit.flux;
    const uint8_t  open_loop   = lo->e_mode == FOC_MODE_IF || lo->e_mode == FOC_MODE_VF;

    /* 正常 HFI 观测优先，禁止告警音暂停 PLL、解调和极性辨识。 */
    if (hfi_enable && hfi->cfg.alarm_enable)
        hfi_alarm_set(hfi, false);

    if (hfi->cfg.alarm_enable) {
        lo->hfi_injection_enable = true;
        hfi_exec_in_rt(hfi, in->stator.i_ab);
        return;
    }

    if (flux_enable) {
        flux_exec_in_rt(flux,
                        in->stator.i_ab,
                        foc_obs_v_ab_rt(foc,
                                        cfg->obs_cfg.flux.sample_volt_max_omega,
                                        cfg->obs_cfg.flux.out_volt_min_omega));
    }

    if (hfi_enable && flux_enable && cfg->obs_cfg.switch_vel > 0.0F) {
        const float32_t band = MAX(cfg->obs_cfg.switch_band_vel, 0.0F);
        const float32_t low  = MAX(cfg->obs_cfg.switch_vel - 0.5F * band, 0.0F);
        const float32_t high = MAX(cfg->obs_cfg.switch_vel + 0.5F * band, low);
        const float32_t step = foc_obs_switch_step_rt(foc);
        const uint32_t  ready_cnt =
            (uint32_t)(0.001F * (float32_t)cfg->obs_cfg.switch_ready_ms * cfg->ctl_cfg.iq_pi.fs);

        if ((uint32_t)lo->e_obs_switch > (uint32_t)FOC_OBS_SWITCH_FLUX_TO_HFI) {
            lo->e_obs_switch = FOC_OBS_SWITCH_HFI;
            lo->flux_weight  = 0.0F;
        }

        float32_t speed = lo->e_obs_switch == FOC_OBS_SWITCH_FLUX ||
                                  lo->e_obs_switch == FOC_OBS_SWITCH_FLUX_TO_HFI
                              ? ABS(f32_finite_or_rt(flux->out.est_omega, 0.0F))
                              : ABS(f32_finite_or_rt(hfi->out.est_omega, 0.0F));

        /* 降速进入交接区时，先把 HFI PLL 同步到当前校正后的 flux 角度。 */
        if (lo->e_obs_switch == FOC_OBS_SWITCH_FLUX && speed <= high) {
            foc_obs_sync_hfi_rt(foc, foc_obs_flux_theta_rt(foc), flux->out.est_omega);
            lo->e_obs_switch         = FOC_OBS_SWITCH_FLUX_TO_HFI;
            lo->hfi_injection_enable = true;
        }

        lo->hfi_injection_enable = lo->e_obs_switch != FOC_OBS_SWITCH_FLUX;
        if (lo->hfi_injection_enable) {
            /* 交接结束前保持完整注入幅值，避免 HFI 因信噪比下降提前丢锁。 */
            if (!open_loop && hfi->lo.e_polar_idf != HFI_POLAR_IDF_FINISH)
                foc_set_mode(foc, FOC_MODE_CUR);
            hfi_exec_in_rt(hfi, in->stator.i_ab);
            if (!open_loop)
                lo->ref_i_dq.d = hfi->out.id;
        } else {
            hfi->out.id = 0.0F;
            hfi->out.vd = 0.0F;
            if (!open_loop)
                lo->ref_i_dq.d = 0.0F;
        }

        speed = lo->e_obs_switch == FOC_OBS_SWITCH_FLUX ||
                        lo->e_obs_switch == FOC_OBS_SWITCH_FLUX_TO_HFI
                    ? ABS(f32_finite_or_rt(flux->out.est_omega, 0.0F))
                    : ABS(f32_finite_or_rt(hfi->out.est_omega, 0.0F));

        switch (lo->e_obs_switch) {
            case FOC_OBS_SWITCH_HFI: {
                lo->flux_weight = 0.0F;
                if (speed < low) {
                    lo->flux_theta_aligned   = false;
                    lo->obs_switch_ready_cnt = 0U;
                    break;
                }

                const float32_t align_err = foc_obs_align_flux_rt(foc, !lo->flux_theta_aligned);
                lo->flux_theta_aligned    = true;
                if (cfg->obs_cfg.switch_theta_err_max > 0.0F &&
                    ABS(align_err) > cfg->obs_cfg.switch_theta_err_max) {
                    lo->obs_switch_ready_cnt = 0U;
                    break;
                }

                if (speed >= high) {
                    if (lo->obs_switch_ready_cnt < ready_cnt)
                        lo->obs_switch_ready_cnt++;
                    else
                        lo->e_obs_switch = FOC_OBS_SWITCH_HFI_TO_FLUX;
                } else
                    lo->obs_switch_ready_cnt = 0U;
                break;
            }
            case FOC_OBS_SWITCH_HFI_TO_FLUX: {
                const float32_t align_err = foc_obs_align_flux_rt(foc, false);
                if (speed < low || (cfg->obs_cfg.switch_theta_err_max > 0.0F &&
                                    ABS(align_err) > cfg->obs_cfg.switch_theta_err_max)) {
                    lo->e_obs_switch         = FOC_OBS_SWITCH_HFI;
                    lo->flux_weight          = 0.0F;
                    lo->obs_switch_ready_cnt = 0U;
                    break;
                }

                lo->flux_weight = MIN(lo->flux_weight + step, 1.0F);
                if (lo->flux_weight >= 1.0F) {
                    lo->e_obs_switch         = FOC_OBS_SWITCH_FLUX;
                    lo->hfi_injection_enable = false;
                }
                break;
            }
            case FOC_OBS_SWITCH_FLUX: {
                lo->flux_weight = 1.0F;
                break;
            }
            case FOC_OBS_SWITCH_FLUX_TO_HFI: {
                if (speed > high) {
                    lo->e_obs_switch         = FOC_OBS_SWITCH_FLUX;
                    lo->flux_weight          = 1.0F;
                    lo->hfi_injection_enable = false;
                    break;
                }

                lo->flux_weight = MAX(lo->flux_weight - step, 0.0F);
                if (lo->flux_weight <= 0.0F) {
                    lo->e_obs_switch         = FOC_OBS_SWITCH_HFI;
                    lo->flux_theta_aligned   = false;
                    lo->obs_switch_ready_cnt = 0U;
                }
                break;
            }
            default:
                break;
        }

        lo->hfi_weight             = 1.0F - lo->flux_weight;
        const float32_t flux_theta = foc_obs_flux_theta_rt(foc);
        if (lo->flux_weight <= 0.0F) {
            in->rotor.elec_theta_obs = hfi->out.est_theta;
            in->rotor.elec_omega_obs = hfi->out.est_omega;
        } else if (lo->flux_weight >= 1.0F) {
            in->rotor.elec_theta_obs = flux_theta;
            in->rotor.elec_omega_obs = flux->out.est_omega;
        } else {
            float32_t theta_err;
            WARP_PI(theta_err, flux_theta - hfi->out.est_theta);
            WARP_TAU(in->rotor.elec_theta_obs, hfi->out.est_theta + lo->flux_weight * theta_err);
            in->rotor.elec_omega_obs =
                lo->hfi_weight * hfi->out.est_omega + lo->flux_weight * flux->out.est_omega;
        }
        return;
    }

    lo->flux_weight          = flux_enable ? 1.0F : 0.0F;
    lo->hfi_weight           = hfi_enable ? 1.0F : 0.0F;
    lo->hfi_injection_enable = hfi_enable;
    if (hfi_enable) {
        if (!open_loop && hfi->lo.e_polar_idf != HFI_POLAR_IDF_FINISH)
            foc_set_mode(foc, FOC_MODE_CUR);
        hfi_exec_in_rt(hfi, in->stator.i_ab);
        if (!open_loop)
            lo->ref_i_dq.d = hfi->out.id;
        in->rotor.elec_theta_obs = hfi->out.est_theta;
        in->rotor.elec_omega_obs = hfi->out.est_omega;
        return;
    }

    /* SMO 与 flux 同时开启属于配置冲突，角度输出默认优先使用 flux。 */
    if (flux_enable) {
        in->rotor.elec_theta_obs = flux->out.est_theta;
        in->rotor.elec_omega_obs = flux->out.est_omega;
        return;
    }

    if (cfg->obs_cfg.u_obs_flag.bit.smo) {
        smo_exec_in_rt(smo,
                       in->stator.i_ab,
                       foc_obs_v_ab_rt(foc,
                                       cfg->obs_cfg.smo.sample_volt_max_omega,
                                       cfg->obs_cfg.smo.out_volt_min_omega));

        in->rotor.elec_theta_obs = smo->out.est_theta;
        in->rotor.elec_omega_obs = smo->out.est_omega;
        return;
    }
}

/**
 * @brief 以 d-q 轴电流为输入的观测器单次执行计算
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_obs_i_dq_rt(struct foc *foc)
{
    DECL(foc, cfg, in, out, lo, tmp);
    RENAME(&lo->luenberger, luenberger);

    if (cfg->obs_cfg.u_obs_flag.bit.luenberger) {
        luenberger_exec_in_rt(
            luenberger, in->rotor.elec_theta, out->fdb_pvct.elec_tor * tmp->inv_outshaft_ratio);

        if (cfg->obs_cfg.enable_vel == 0.0F || out->fdb_pvct.vel > cfg->obs_cfg.enable_vel) {
            lo->comp_i_dq.q = 0.0F;
            return;
        }

        lo->comp_i_dq.q = CPYSGN(poly_eval_rt(cfg->base_cfg.tor2cur,
                                              ARRAY_LEN(cfg->base_cfg.tor2cur) - 1,
                                              ABS(luenberger->out.est_load_tor)),
                                 lo->ref_i_dq.q);
    }

    if (cfg->obs_cfg.u_obs_flag.bit.rls) {
        const float32_t ratio     = cfg->base_cfg.reducer.outshaft_ratio;
        const float32_t motor_tor = CPYSGN(poly_eval_rt(cfg->base_cfg.cur2tor,
                                                        ARRAY_LEN(cfg->base_cfg.cur2tor) - 1,
                                                        ABS(in->stator.i_dq.q)),
                                           in->stator.i_dq.q);
        const float32_t load_tor =
            cfg->sensor_cfg.tor_sensor_enable && ratio != 0.0F ? in->load_tor / ratio : 0.0F;
        const float32_t y    = motor_tor - load_tor;
        const float32_t x[2] = {in->rotor.mech_acc, in->rotor.mech_omega};

        /* 无有效激励时不更新,也不执行遗忘 */
        if (lo->rls.cfg.order == 2 && ABS(x[0]) >= RLS_INERTIA_MIN_ACC &&
            ABS(x[1]) >= RLS_INERTIA_MIN_OMEGA) {
            rls_exec_in(&lo->rls, y, x);

            if (lo->rls.out.w[0] > 0.0F)
                lo->identify.j = lo->rls.out.w[0];
            if (lo->rls.out.w[1] >= 0.0F)
                lo->identify.friction = lo->rls.out.w[1];
        }
    }
}

/**
 * @brief 以 d-q 轴电压为输入的观测器单次执行计算
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_obs_v_dq_rt(struct foc *foc)
{
    DECL(foc, cfg, in, out, lo, tmp);
    RENAME(&lo->hfi, hfi)

    /* 交接完成前维持完整 HFI 注入，flux 完全接管后再关闭。 */
    if (hfi->cfg.alarm_enable)
        out->v_dq.d += hfi->out.vd;
    else if (cfg->obs_cfg.u_obs_flag.bit.hfi && lo->hfi_injection_enable)
        out->v_dq.d += hfi->out.vd;
}
