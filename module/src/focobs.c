#include "mathdef.h"

#include "foc.h"

void foc_obs_i_ab(foc_t *foc);
void foc_obs_i_dq(foc_t *foc);
void foc_obs_v_dq(foc_t *foc);

void
foc_obs_i_ab(foc_t *foc)
{
        DECL(foc, cfg, in, out, lo);

        switch (cfg->obs_cfg.low) {
                case FOC_OBS_HFI: {
                        if (lo->e_theta != FOC_THETA_SENSORLESS || ABS(lo->fdb_pvct.vel) > cfg->obs_cfg.switch_vel)
                                break;

                        RENAME(&lo->hfi, hfi);

                        hfi_exec_in(hfi, in->i_ab);
                        in->rotor.obs_theta = hfi->out.est_theta;
                        in->rotor.obs_omega = hfi->out.est_omega;
                        lo->ref_i_dq.d      = hfi->out.id;

                        if (hfi->lo.e_polar_idf != HFI_POLAR_IDF_FINISH)
                                lo->e_mode = FOC_MODE_CUR;

                        break;
                }
                default:
                        break;
        }

        switch (cfg->obs_cfg.high) {
                case FOC_OBS_SMO: {
                        RENAME(&lo->smo, smo);

                        smo_exec_in(smo, in->i_ab, out->v_ab);
                        if (ABS(lo->fdb_pvct.vel) < cfg->obs_cfg.switch_vel)
                                break;

                        in->rotor.obs_theta = smo->out.est_theta;
                        in->rotor.obs_omega = smo->out.est_omega;
                        break;
                }
                default:
                        break;
        }
}

void
foc_obs_i_dq(foc_t *foc)
{
        DECL(foc, cfg, in, out, lo);

        switch (cfg->obs_cfg.load_tor) {
                case FOC_OBS_LBG: {
                        RENAME(&lo->lbg, lbg);

                        lbg_exec_in(lbg, in->rotor.theta, lo->fdb_pvct.elec_tor / cfg->base_cfg.outshaft_ratio);
                        lo->fdb_pvct.load_tor = lbg->out.est_load_tor;
                        if (lo->e_theta != FOC_THETA_SENSOR || ABS(lo->fdb_pvct.vel) > cfg->obs_cfg.enable_vel)
                                break;

                        lo->comp_i_dq.q = CPYSGN(poly_eval(cfg->base_cfg.motor.tor2cur,
                                                           ARRAY_LEN(cfg->base_cfg.motor.tor2cur) - 1,
                                                           ABS(lo->fdb_pvct.load_tor)),
                                                 lo->ref_i_dq.q);
                }
                default:
                        break;
        }
}

void
foc_obs_v_dq(foc_t *foc)
{
        DECL(foc, cfg, out, lo);

        switch (cfg->obs_cfg.low) {
                case FOC_OBS_HFI: {
                        RENAME(&lo->hfi, hfi)

                        out->v_dq.d += hfi->out.vd;
                        break;
                }
                default:
                        break;
        }
}
