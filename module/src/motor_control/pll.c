#include "mathdef.h"

#include "motor_control/pll.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

static inline void
pll_exec_core_rt(struct pll_filter *pll)
{
    DECL(pll, out, lo, tmp);

    /* 环路滤波器 */
    lo->ki_out     += tmp->ki_ts * lo->theta_err;
    out->omega      = lo->kp * lo->theta_err + lo->ki_out;
    out->lpf_omega  = tmp->lpf_alpha * out->omega + tmp->lpf_beta * out->lpf_omega;

    /* 压控振荡器 */
    out->theta += out->omega * tmp->inv_fs;
    WARP_TAU(out->theta, out->theta);
}

void
pll_init(struct pll_filter *pll, const struct pll_cfg pll_cfg)
{
    DECL(pll, cfg, lo, tmp);
    CFG_INIT(pll, pll_cfg);

    lo->kp         = 2.0F * cfg->wc * cfg->damp;
    lo->ki         = SQ(cfg->wc);
    lo->ffd_lpf_wc = 0.5F * cfg->lpf_wc;

    FS_INIT(cfg->fs, tmp);
    tmp->inv_fs = cfg->fs != 0.0F ? 1.0F / cfg->fs : 0.0F;
    tmp->ki_ts  = lo->ki * tmp->inv_fs;

    tmp->lpf_alpha = cfg->lpf_wc != 0.0F ? cfg->lpf_wc / (cfg->lpf_wc + cfg->fs) : 1.0F;
    tmp->lpf_beta  = 1.0F - tmp->lpf_alpha;

    tmp->ffd_lpf_alpha =
        lo->ffd_lpf_wc != 0.0F ? lo->ffd_lpf_wc / (lo->ffd_lpf_wc + cfg->fs) : 1.0F;
    tmp->ffd_lpf_beta = 1.0F - tmp->ffd_lpf_alpha;
}

void
pll_exec_rt(struct pll_filter *pll)
{
    pll_exec_core_rt(pll);
}

void
pll_exec_ab_in_rt(struct pll_filter *pll, const struct f32_ab ab)
{
    DECL(pll, in, out, lo);
    in->ab = ab;

    // PD 鉴相器
    lo->theta_err = in->ab.b * COS(out->theta) - in->ab.a * SIN(out->theta);

    pll_exec_core_rt(pll);
}

void
pll_exec_theta_err_in_rt(struct pll_filter *pll, const float32_t theta_err)
{
    DECL(pll, cfg, in, out, lo);
    // PD 鉴相器
    WARP_PI(lo->theta_err, theta_err);

    pll_exec_core_rt(pll);
}

void
pll_exec_theta_in_rt(struct pll_filter *pll, const float32_t theta)
{
    DECL(pll, in, out, lo, tmp);
    in->theta = theta;

    /* 前馈速度计算 */
    float32_t delta_theta;
    WARP_PI(delta_theta, in->theta - lo->prev_theta);
    lo->ffd_omega     = delta_theta * tmp->fs;
    lo->prev_theta    = in->theta;
    lo->lpf_ffd_omega = tmp->ffd_lpf_alpha * lo->ffd_omega + tmp->ffd_lpf_beta * lo->lpf_ffd_omega;

    /* 鉴相器 */
    WARP_PI(lo->theta_err, in->theta - out->theta);

    pll_exec_core_rt(pll);
}
