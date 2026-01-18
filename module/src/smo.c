#include "mathdef.h"

#include "smo.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
smo_init(smo_obs_t *smo, const smo_cfg_t smo_cfg)
{
        CFG_INIT(smo, smo_cfg);
        DECL(smo, cfg, lo);

        lo->pll.cfg.fs = cfg->fs;

        pll_init(&lo->pll, lo->pll.cfg);
}

/**
 * @brief 滑膜观测器
 *
 * @details
 *
 * $\theta_{r} = \arctan(\frac{-e_{s \alpha}}{e_{s \beta}})$
 *
 * @param smo
 */
void
smo_exec(smo_obs_t *smo)
{
        DECL(smo, cfg, in, out, lo);
        RENAME(&smo->lo.pll, pll);

        // 电流误差方程
        INTEGRATOR(lo->est_i_ab.a,
                   (in->v_ab.a - in->i_ab.a * cfg->motor.rs - lo->est_emf_v_ab.a) / (0.5f * (cfg->motor.ld + cfg->motor.lq)),
                   1.0f,
                   cfg->fs);

        INTEGRATOR(lo->est_i_ab.b,
                   (in->v_ab.b - in->i_ab.b * cfg->motor.rs - lo->est_emf_v_ab.b) / (0.5f * (cfg->motor.ld + cfg->motor.lq)),
                   1.0f,
                   cfg->fs);

        AB_SUB_VEC(lo->est_i_ab_err, lo->est_i_ab, in->i_ab);

        // 反电动势估算
        lo->est_emf_v_ab.a = (ABS(lo->est_i_ab_err.a) > cfg->es0) ? CPYSGN(cfg->ks, lo->est_i_ab_err.a)
                                                                  : (cfg->ks * lo->est_i_ab_err.a / cfg->es0);

        lo->est_emf_v_ab.b = (ABS(lo->est_i_ab_err.b) > cfg->es0) ? CPYSGN(cfg->ks, lo->est_i_ab_err.b)
                                                                  : (cfg->ks * lo->est_i_ab_err.b / cfg->es0);

        pll_exec_ab_in(&lo->pll, lo->est_emf_v_ab);
        out->est_omega = pll->out.lpf_omega;

        WARP_TAU(out->est_theta, ATAN2(-lo->est_emf_v_ab.a * SGN(out->est_omega), lo->est_emf_v_ab.b * SGN(out->est_omega)));
}

void
smo_exec_in(smo_obs_t *smo, const f32_ab_t i_ab, const f32_ab_t v_ab)
{
        DECL(smo, in);

        in->i_ab = i_ab;
        in->v_ab = v_ab;
        smo_exec(smo);
}
