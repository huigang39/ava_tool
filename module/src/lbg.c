#include "mathdef.h"

#include "lbg.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
lbg_init(lbg_obs_t *lbg, const lbg_cfg_t lbg_cfg)
{
        CFG_INIT(lbg, lbg_cfg);
        DECL(lbg, cfg, lo);

        lo->g1 = 2.0f * cfg->wc;
        lo->kp = 2.0f * SQ(cfg->wc) * cfg->motor.j * cfg->damp;
        lo->ki = SQ(cfg->wc) * cfg->wc * cfg->motor.j;
}

void
lbg_exec(lbg_obs_t *lbg)
{
        CFG_CHECK(lbg, lbg_init);
        DECL(lbg, cfg, in, out, lo);

        // 电角度
        WARP_PI(lo->theta_err, in->theta - out->est_theta);

        // 机械角度
        lo->mech_theta_err = lo->theta_err / (f32)cfg->motor.npp;

        // 负载力矩
        INTEGRATOR(lo->ki_out, lo->mech_theta_err, lo->ki, cfg->fs);
        CLAMP(lo->ki_out, -cfg->motor.tor_max, cfg->motor.tor_max);
        out->est_load_tor = -lo->ki_out;

        out->sum_tor = in->elec_tor + lo->kp * lo->mech_theta_err + lo->ki_out;

        INTEGRATOR(lo->est_omega, out->sum_tor, 1.0f / cfg->motor.j, cfg->fs);
        out->est_omega = lo->g1 * lo->mech_theta_err + lo->est_omega;

        INTEGRATOR(out->est_theta, out->est_omega, (f32)cfg->motor.npp, cfg->fs);
        WARP_TAU(out->est_theta, out->est_theta);
}

void
lbg_exec_in(lbg_obs_t *lbg, const f32 theta, const f32 elec_tor)
{
        DECL(lbg, in);

        if (!lbg->lo.reset_flag) {
                lbg->out.est_theta = theta;
                lbg->lo.reset_flag = 1;
        }

        in->theta    = theta;
        in->elec_tor = elec_tor;
        lbg_exec(lbg);
}
