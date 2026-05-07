#include "mathdef.h"

#include "pll.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
pll_init(pll_filter_t *pll, const pll_cfg_t pll_cfg)
{
        DECL(pll, cfg, lo);
        CFG_INIT(pll, pll_cfg);

        lo->kp         = 2.0f * cfg->wc * cfg->damp;
        lo->ki         = SQ(cfg->wc);
        lo->ffd_lpf_wc = 0.5f * cfg->lpf_wc;
}

void
pll_exec(pll_filter_t *pll)
{
        DECL(pll, cfg, in, out, lo);
        CFG_CHECK(pll, pll_init);

        // LF 环路滤波器
        INTEGRATOR(lo->ki_out, lo->theta_err, lo->ki, cfg->fs);
        out->omega = lo->kp * lo->theta_err + lo->ki_out;
        LOWPASS(out->lpf_omega, out->omega, cfg->lpf_wc, cfg->fs);

        // VCO 压控振荡器
        INTEGRATOR(out->theta, out->omega, 1.0f, cfg->fs);
        WARP_TAU(out->theta, out->theta);
}

void
pll_exec_ab_in(pll_filter_t *pll, const f32_ab_t ab)
{
        DECL(pll, in, out, lo);

        in->ab = ab;

        // PD 鉴相器
        lo->theta_err = in->ab.b * COS(out->theta) - in->ab.a * SIN(out->theta);

        pll_exec(pll);
}

void
pll_exec_theta_err_in(pll_filter_t *pll, const f32 theta_err)
{
        DECL(pll, cfg, in, out, lo);

        // PD 鉴相器
        WARP_PI(lo->theta_err, theta_err);

        pll_exec(pll);
}

void
pll_exec_theta_in(pll_filter_t *pll, const f32 theta)
{
        DECL(pll, cfg, in, out, lo);

        in->theta = theta;

        // 前馈速度计算
        THETA_DERIVATIVE(lo->ffd_omega, in->theta, lo->prev_theta, 1.0f, cfg->fs);
        LOWPASS(lo->lpf_ffd_omega, lo->ffd_omega, lo->ffd_lpf_wc, cfg->fs);

        // PD 鉴相器
        WARP_PI(lo->theta_err, in->theta - out->theta);

        pll_exec(pll);
}
