#include "mathdef.h"

#include "motor_control/observer/luenberger.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
luenberger_init(struct luenberger_obs *luenberger, const struct luenberger_cfg luenberger_cfg)
{
    DECL(luenberger, cfg, lo);
    CFG_INIT(luenberger, luenberger_cfg);

    lo->g1 = 2.0F * cfg->wc;
    lo->kp = 2.0F * SQ(cfg->wc) * cfg->motor->j * cfg->damp;
    lo->ki = SQ(cfg->wc) * cfg->wc * cfg->motor->j;
}

void
luenberger_exec_rt(struct luenberger_obs *luenberger)
{
    DECL(luenberger, cfg, in, out, lo);
    if (cfg->motor->j == 0.0F)
        return;

    /* 电角度 */
    WARP_PI(lo->theta_err, in->theta - out->est_theta);

    /* 机械角度 */
    lo->mech_theta_err = lo->theta_err / (float32_t)cfg->motor->npp;

    /* 负载力矩 */
    INTEGRATOR(lo->ki_out, lo->mech_theta_err, lo->ki, cfg->fs);
    CLAMP(lo->ki_out, -cfg->motor->tor_peak, cfg->motor->tor_peak);
    out->est_load_tor = -lo->ki_out;

    out->sum_tor = in->elec_tor + lo->kp * lo->mech_theta_err + lo->ki_out;

    INTEGRATOR(lo->est_omega, out->sum_tor, 1.0F / cfg->motor->j, cfg->fs);
    out->est_omega = lo->g1 * lo->mech_theta_err + lo->est_omega;

    INTEGRATOR(out->est_theta, out->est_omega, (float32_t)cfg->motor->npp, cfg->fs);
    WARP_TAU(out->est_theta, out->est_theta);
}

void
luenberger_exec_in_rt(struct luenberger_obs *luenberger,
                      const float32_t        theta,
                      const float32_t        elec_tor)
{
    DECL(luenberger, in);

    if (!luenberger->tmp.is_reset) {
        luenberger->out.est_theta = theta;
        luenberger->tmp.is_reset  = true;
    }

    in->theta    = theta;
    in->elec_tor = elec_tor;
    luenberger_exec_rt(luenberger);
}
