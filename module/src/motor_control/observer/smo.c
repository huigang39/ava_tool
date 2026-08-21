#include "mathdef.h"

#include "motor_control/observer/smo.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
smo_init(struct smo_obs *smo, const struct smo_cfg smo_cfg)
{
    DECL(smo, cfg, lo);
    CFG_INIT(smo, smo_cfg);
    CFG_INIT(&lo->pll, cfg->pll);

    FS_INIT(cfg->fs, &cfg->pll);
    pll_init(&lo->pll, cfg->pll);
}

void
smo_exec_rt(struct smo_obs *smo)
{
    DECL(smo, cfg, in, out, lo);
    RENAME(&smo->lo.pll, pll);

    /* 电流误差方程 */
    INTEGRATOR(lo->est_i_ab.a,
               (in->v_ab.a - in->i_ab.a * cfg->motor->rs - lo->est_emf_v_ab.a) /
                   (0.5F * (cfg->motor->ld + cfg->motor->lq)),
               1.0F,
               cfg->fs);

    INTEGRATOR(lo->est_i_ab.b,
               (in->v_ab.b - in->i_ab.b * cfg->motor->rs - lo->est_emf_v_ab.b) /
                   (0.5F * (cfg->motor->ld + cfg->motor->lq)),
               1.0F,
               cfg->fs);

    AB_SUB_VEC(lo->est_i_ab_err, lo->est_i_ab, in->i_ab);

    /* 反电动势估算 */
    lo->est_emf_v_ab.a = ABS(lo->est_i_ab_err.a) > cfg->es0
                             ? CPYSGN(cfg->ks, lo->est_i_ab_err.a)
                             : cfg->ks * lo->est_i_ab_err.a / cfg->es0;

    lo->est_emf_v_ab.b = ABS(lo->est_i_ab_err.b) > cfg->es0
                             ? CPYSGN(cfg->ks, lo->est_i_ab_err.b)
                             : cfg->ks * lo->est_i_ab_err.b / cfg->es0;

    pll_exec_ab_in_rt(&lo->pll, lo->est_emf_v_ab);
    out->est_omega = pll->out.lpf_omega;
    WARP_TAU(out->est_theta, pll->out.theta);
}

void
smo_exec_in_rt(struct smo_obs *smo, const struct f32_ab i_ab, const struct f32_ab v_ab)
{
    DECL(smo, in);

    in->i_ab = i_ab;
    in->v_ab = v_ab;
    smo_exec_rt(smo);
}
