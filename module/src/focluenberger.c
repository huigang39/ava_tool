#include "mathdef.h"

#include "focluenberger.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
luenberger_init(luenberger_obs_t *luenberger, const luenberger_cfg_t luenberger_cfg)
{
        DECL(luenberger, cfg, lo);
        CFG_INIT(luenberger, luenberger_cfg);

        lo->g1 = 2.0f * cfg->wc;
        lo->kp = 2.0f * SQ(cfg->wc) * cfg->motor->j * cfg->damp;
        lo->ki = SQ(cfg->wc) * cfg->wc * cfg->motor->j;
}

void
luenberger_exec(luenberger_obs_t *luenberger)
{
        DECL(luenberger, cfg, in, out, lo);
        CFG_CHECK(luenberger, luenberger_init);

        if (cfg->motor->j == 0.0f)
                return;

        /* 电角度 */
        WARP_PI(lo->theta_err, in->theta - out->est_theta);

        /* 机械角度 */
        lo->mech_theta_err = lo->theta_err / (f32)cfg->motor->npp;

        /* 负载力矩 */
        INTEGRATOR(lo->ki_out, lo->mech_theta_err, lo->ki, cfg->fs);
        CLAMP(lo->ki_out, -cfg->motor->tor_peak, cfg->motor->tor_peak);
        out->est_load_tor = -lo->ki_out;

        out->sum_tor = in->elec_tor + lo->kp * lo->mech_theta_err + lo->ki_out;

        INTEGRATOR(lo->est_omega, out->sum_tor, 1.0f / cfg->motor->j, cfg->fs);
        out->est_omega = lo->g1 * lo->mech_theta_err + lo->est_omega;

        INTEGRATOR(out->est_theta, out->est_omega, (f32)cfg->motor->npp, cfg->fs);
        WARP_TAU(out->est_theta, out->est_theta);
}

void
luenberger_exec_in(luenberger_obs_t *luenberger, const f32 theta, const f32 elec_tor)
{
        DECL(luenberger, in);

        if (!luenberger->tmp.is_reset) {
                luenberger->out.est_theta = theta;
                luenberger->tmp.is_reset  = TRUE;
        }

        in->theta    = theta;
        in->elec_tor = elec_tor;
        luenberger_exec(luenberger);
}
