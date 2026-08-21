#include "mathdef.h"

#include "motor_control/observer/flux.h"

#define FLUX_MIN_MAG (1.0e-6F)

void
flux_init(struct flux_obs *flux, const struct flux_cfg flux_cfg)
{
    DECL(flux, cfg, lo);
    CFG_INIT(flux, flux_cfg);

    FS_INIT(cfg->fs, &cfg->pll);
    CFG_INIT(&lo->pll, cfg->pll);
    pll_init(&lo->pll, cfg->pll);
}

void
flux_exec_rt(struct flux_obs *flux)
{
    DECL(flux, cfg, in, out, lo);
    if (!cfg->motor || cfg->fs <= 0.0F)
        return;

    /* 电压模型加磁链幅值径向反馈, 避免纯积分器发生直流漂移 */
    const float32_t rotor_flux_mag =
        SQRT(SQ(out->est_rotor_flux_ab.a) + SQ(out->est_rotor_flux_ab.b));
    struct f32_ab correction = {0.0F, 0.0F};

    /* IPMSM 有功磁链幅值 psi_f + (Ld - Lq) * id */
    const float32_t i_d = in->i_ab.a * COS(lo->pll.out.theta) + in->i_ab.b * SIN(lo->pll.out.theta);
    lo->target_flux     = cfg->motor->psi + (cfg->motor->ld - cfg->motor->lq) * i_d;
    if (lo->target_flux < FLUX_MIN_MAG)
        lo->target_flux = FLUX_MIN_MAG;

    if (rotor_flux_mag > FLUX_MIN_MAG) {
        const float32_t gain = cfg->wc * (lo->target_flux - rotor_flux_mag) / rotor_flux_mag;
        correction.a         = gain * out->est_rotor_flux_ab.a;
        correction.b         = gain * out->est_rotor_flux_ab.b;
    }

    INTEGRATOR(out->est_stator_flux_ab.a,
               in->v_ab.a - cfg->motor->rs * in->i_ab.a + correction.a,
               1.0F,
               cfg->fs);
    INTEGRATOR(out->est_stator_flux_ab.b,
               in->v_ab.b - cfg->motor->rs * in->i_ab.b + correction.b,
               1.0F,
               cfg->fs);

    /* psi_active = psi_s - Lq * i_s, 其方向即转子 d 轴方向 */
    out->est_rotor_flux_ab.a = out->est_stator_flux_ab.a - cfg->motor->lq * in->i_ab.a;
    out->est_rotor_flux_ab.b = out->est_stator_flux_ab.b - cfg->motor->lq * in->i_ab.b;
    out->est_flux            = SQRT(SQ(out->est_rotor_flux_ab.a) + SQ(out->est_rotor_flux_ab.b));

    if (out->est_flux > FLUX_MIN_MAG) {
        const struct f32_ab unit_flux = {
            .a = out->est_rotor_flux_ab.a / out->est_flux,
            .b = out->est_rotor_flux_ab.b / out->est_flux,
        };
        pll_exec_ab_in_rt(&lo->pll, unit_flux);
    }

    out->est_omega = lo->pll.out.lpf_omega;
    WARP_TAU(out->est_theta, lo->pll.out.theta);
}

void
flux_exec_in_rt(struct flux_obs *flux, const struct f32_ab i_ab, const struct f32_ab v_ab)
{
    DECL(flux, in);
    in->i_ab = i_ab;
    in->v_ab = v_ab;
    flux_exec_rt(flux);
}
