#include "mathdef.h"

#include "motor_control/controller/adrc.h"

/* -------------------------------------------------------------------------- */
/*                                  内部函数                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief adrc 扰动估测器运算
 *
 * @param adrc adrc 结构体
 * @return     NULL
 */
static void
adrc_eso_update_rt(struct adrc_ctl *adrc)
{
    DECL(adrc, cfg, in, out, lo);

    lo->err = lo->x_hat - in->fdb;

    /* 一阶对象 LESO: x_dot=d+b0*u，d_dot≈0。两个状态使用同一拍误差显式离散。 */
    const float32_t x_dot  = lo->d_hat - lo->beta1 * lo->err + cfg->b0 * lo->plant_u;
    const float32_t d_dot  = -lo->beta2 * lo->err;
    lo->x_hat             += lo->inv_fs * x_dot;
    lo->d_hat_raw         += lo->inv_fs * d_dot;
    LOWPASS(lo->d_hat, lo->d_hat_raw, cfg->d_lpf_wc, cfg->fs);
}

/**
 * @brief adrc 控制率运算
 *
 * @param adrc adrc 结构体
 * @return     NULL
 */
static void
adrc_ctl_update_rt(struct adrc_ctl *adrc)
{
    DECL(adrc, cfg, in, out, lo);

    lo->ref_limited = in->ref;
    if (cfg->ref_rate_max > 0.0F) {
        const float32_t ref_change_max = cfg->ref_rate_max * lo->inv_fs;
        lo->ref_change                 = in->ref - lo->prev_ref;
        lo->ref_limited =
            lo->prev_ref + (CLAMP_RET(lo->ref_change, -ref_change_max, ref_change_max));
    }
    lo->prev_ref = lo->ref_limited;

    out->u0     = lo->kp * (lo->ref_limited - lo->x_hat);
    out->d_comp = -lo->d_hat * lo->inv_b0;
    if (cfg->d_comp_max > 0.0F) {
        out->d_comp = CLAMP_RET(out->d_comp, -cfg->d_comp_max, cfg->d_comp_max);
    }
    out->u_raw = out->u0 * lo->inv_b0 + out->d_comp + in->ffd;
    out->u     = CLAMP_RET(out->u_raw, cfg->out_min, cfg->out_max);
    if (cfg->out_rate_max > 0.0F) {
        const float32_t out_change_max = cfg->out_rate_max * lo->inv_fs;
        const float32_t out_change =
            CLAMP_RET(out->u - lo->prev_u, -out_change_max, out_change_max);
        out->u = lo->prev_u + out_change;
    }
    lo->prev_u  = out->u;
    lo->plant_u = out->u - in->ffd;
}

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
adrc_init(struct adrc_ctl *adrc, const struct adrc_cfg adrc_cfg)
{
    DECL(adrc, cfg, lo);
    CFG_INIT(adrc, adrc_cfg);

    FS_INIT(ABS(cfg->fs), cfg);
    cfg->wc           = ABS(cfg->wc);
    cfg->wo           = ABS(cfg->wo);
    cfg->ref_rate_max = ABS(cfg->ref_rate_max);
    cfg->d_lpf_wc     = ABS(cfg->d_lpf_wc);
    cfg->d_comp_max   = ABS(cfg->d_comp_max);
    cfg->out_rate_max = ABS(cfg->out_rate_max);

    /* 显式欧拉离散下限制归一化带宽，避免 LESO 将采样噪声放大成高频控制量。 */
    if (cfg->fs > 0.0F) {
        cfg->wc       = MIN(cfg->wc, 0.2F * cfg->fs);
        cfg->wo       = MIN(cfg->wo, 0.3F * cfg->fs);
        cfg->d_lpf_wc = MIN(cfg->d_lpf_wc, 0.3F * cfg->fs);
    }

    lo->inv_fs = cfg->fs > 0.0F && f32_is_finite_rt(cfg->fs) ? 1.0F / cfg->fs : 0.0F;
    lo->inv_b0 = cfg->b0 != 0.0F && f32_is_finite_rt(cfg->b0) ? 1.0F / cfg->b0 : 0.0F;
    lo->kp     = cfg->wc;
    lo->beta1  = 2.0F * cfg->wo;
    lo->beta2  = SQ(cfg->wo);

    adrc_reset(adrc);
}

void
adrc_reset(struct adrc_ctl *adrc)
{
    RESET(adrc, in, out);
    adrc->lo.x_hat       = 0.0F;
    adrc->lo.d_hat_raw   = 0.0F;
    adrc->lo.d_hat       = 0.0F;
    adrc->lo.err         = 0.0F;
    adrc->lo.plant_u     = 0.0F;
    adrc->lo.prev_u      = 0.0F;
    adrc->lo.ref_change  = 0.0F;
    adrc->lo.ref_limited = 0.0F;
    adrc->lo.prev_ref    = 0.0F;
    adrc->lo.initialized = false;
}

void
adrc_set_out_limit(struct adrc_ctl *adrc, const float32_t out_max, const float32_t out_min)
{
    adrc->cfg.out_max = MAX(out_max, out_min);
    adrc->cfg.out_min = MIN(out_max, out_min);
}

void
adrc_exec_rt(struct adrc_ctl *adrc)
{
    DECL(adrc, in, out, lo);

    if (!f32_is_finite_rt(in->ref) || !f32_is_finite_rt(in->fdb) || !f32_is_finite_rt(in->ffd) ||
        lo->inv_fs == 0.0F || lo->inv_b0 == 0.0F) {
        out->u_raw = out->u = 0.0F;
        lo->plant_u         = 0.0F;
        lo->prev_u          = 0.0F;
        lo->initialized     = false;
        return;
    }

    if (!lo->initialized) {
        lo->x_hat       = in->fdb;
        lo->d_hat       = 0.0F;
        lo->prev_ref    = in->fdb;
        lo->initialized = true;
    } else {
        adrc_eso_update_rt(adrc);
    }

    adrc_ctl_update_rt(adrc);
}

void
adrc_exec_in_rt(struct adrc_ctl *adrc,
                const float32_t  ref,
                const float32_t  fdb,
                const float32_t  ffd)
{
    DECL(adrc, in);

    in->ref = ref;
    in->fdb = fdb;
    in->ffd = ffd;
    adrc_exec_rt(adrc);
}
