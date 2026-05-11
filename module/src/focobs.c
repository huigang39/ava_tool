#include "mathdef.h"

#include "focdef.h"

/**
 * @brief 以 alpha-beta 轴电流为输入的观测器单次执行计算
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_obs_i_ab(foc_t *foc)
{
        DECL(foc, cfg, in, out, lo);
        RENAME(&lo->hfi, hfi);
        RENAME(&lo->smo, smo);

        if (cfg->obs_cfg.u_obs_flag.bit.hfi) {
                // if (lo->e_elec_theta != FOC_ELEC_THETA_SENSORLESS)
                //         return;

                if (hfi->lo.e_polar_idf != HFI_POLAR_IDF_FINISH)
                        lo->e_mode = FOC_MODE_CUR;

                hfi_exec_in(hfi, in->stator.i_ab);
                lo->ref_i_dq.d = hfi->out.id;

                in->rotor.elec_obs_theta = hfi->out.est_theta;
                in->rotor.elec_obs_omega = hfi->out.est_omega;
                return;
        }

        if (cfg->obs_cfg.u_obs_flag.bit.smo) {
                smo_exec_in(smo, in->stator.i_ab, in->stator.v_ab);

                in->rotor.elec_obs_theta = smo->out.est_theta;
                in->rotor.elec_obs_omega = smo->out.est_omega;
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
foc_obs_i_dq(foc_t *foc)
{
        DECL(foc, cfg, in, out, lo, tmp);
        RENAME(&lo->luenberger, luenberger);

        if (cfg->obs_cfg.u_obs_flag.bit.luenberger) {
                luenberger_exec_in(luenberger, in->rotor.elec_theta, out->fdb_pvct.elec_tor / cfg->base_cfg.outshaft_ratio);

                if (cfg->obs_cfg.enable_vel == 0.0f || out->fdb_pvct.vel > cfg->obs_cfg.enable_vel) {
                        lo->comp_i_dq.q = 0.0f;
                        return;
                }

                lo->comp_i_dq.q = CPYSGN(poly_eval(cfg->base_cfg.motor.tor2cur,
                                                   ARRAY_LEN(cfg->base_cfg.motor.tor2cur) - 1,
                                                   ABS(luenberger->out.est_load_tor)),
                                         lo->ref_i_dq.q);
        }

        if (cfg->obs_cfg.u_obs_flag.bit.rls) {
                f32 y    = 0.0f;
                f32 x[2] = {0.0f, 0.0f};

                rls_exec_in(&lo->rls, y, x);
        }
}

/**
 * @brief 以 d-q 轴电压为输入的观测器单次执行计算
 *
 * @param foc FOC 结构体
 * @return    void
 */
void
foc_obs_v_dq(foc_t *foc)
{
        DECL(foc, cfg, in, out, lo, tmp);
        RENAME(&lo->hfi, hfi)

        if (cfg->obs_cfg.u_obs_flag.bit.hfi)
                out->v_dq.d += hfi->out.vd;
}
