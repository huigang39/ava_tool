#include "iir.h"
#include "mathdef.h"

/**
 * @brief         检查滤波配置及采样频率
 * @param[in] cfg 滤波配置
 * @param[in] fs  采样频率, 单位: Hz
 * @return        配置有效返回 true
 */
static bool is_iir_config_valid(const struct iir_cfg *cfg, float32_t fs);

int iir_init(struct iir_filter *iir,
             const struct iir_cfg iir_cfg,
             const float32_t fs)
{
    if (!iir) {
        return MERR(MENULLPTR);
    }
    if (!is_iir_config_valid(&iir_cfg, fs)) {
        return MERR(MEINVAL);
    }
    CFG_READ(iir);
    DECL(iir, lo);

    memset(lo, 0, sizeof(*lo));
    CFG_INIT(iir, iir_cfg);

    lo->fs = fs;

    switch (cfg->order) {
    case IIR_1: {
        lo->fc = cfg->fh;
        lo->rc = 1.0F / HZ2RADS(lo->fc);
        lo->alpha = 1.0F / (1.0F + lo->rc * lo->fs);

        switch (cfg->type) {
        case IIR_LOWPASS: {
            lo->b0 = lo->alpha;
            lo->b1 = 0.0F;
            break;
        }
        case IIR_HIGHPASS: {
            lo->b0 = 1.0F - lo->alpha;
            lo->b1 = -(1.0F - lo->alpha);
            break;
        }
        default: {
            break;
        }
        }
        lo->norm_a1 = -(1.0F - lo->alpha);
        break;
    }
    case IIR_2: {
        lo->fc = (cfg->fh + cfg->fl) / 2.0F;
        lo->q = SQRT(cfg->fh * cfg->fl) / (cfg->fh - cfg->fl);
        lo->w0 = HZ2RADS(lo->fc) / lo->fs;
        lo->alpha = SIN(lo->w0) / (2.0F * lo->q);

        switch (cfg->type) {
        case IIR_LOWPASS: {
            lo->b0 = (1.0F - COS(lo->w0)) / 2.0F;
            lo->b1 = lo->b0 * 2.0F;
            lo->b2 = lo->b0;
            lo->a0 = 1.0F + lo->alpha;
            lo->a1 = -2.0F * COS(lo->w0);
            lo->a2 = 1.0F - lo->alpha;
            break;
        }
        case IIR_HIGHPASS: {
            lo->b0 = (1.0F + COS(lo->w0)) / 2.0F;
            lo->b1 = -lo->b0 * 2.0F;
            lo->b2 = lo->b0;
            lo->a0 = 1.0F + lo->alpha;
            lo->a1 = -2.0F * COS(lo->w0);
            lo->a2 = 1.0F - lo->alpha;
            break;
        }
        case IIR_BANDPASS: {
            lo->b0 = lo->alpha;
            lo->b1 = 0.0F;
            lo->b2 = -lo->alpha;
            lo->a0 = 1.0F + lo->alpha;
            lo->a1 = -2.0F * COS(lo->w0);
            lo->a2 = 1.0F - lo->alpha;
            break;
        }
        default: {
            break;
        }
        }
        // 归一化
        lo->norm_a0 = lo->b0 / lo->a0;
        lo->norm_a1 = lo->b1 / lo->a0;
        lo->norm_a2 = lo->b2 / lo->a0;
        lo->norm_a3 = lo->a1 / lo->a0;
        lo->norm_a4 = lo->a2 / lo->a0;
        break;
    }
    default: {
        break;
    }
    }

    return MOK;
}

int iir_exec(struct iir_filter *iir)
{
    if (!iir) {
        return MERR(MENULLPTR);
    }
    if (!is_iir_config_valid(&iir->cfg, iir->lo.fs)) {
        iir->out.y = 0.0F;
        return MERR(MESTATE);
    }
    CFG_READ(iir);
    DECL(iir, in, out, lo);

    switch (cfg->order) {
    case IIR_1: {
        out->y = lo->b0 * in->x + lo->b1 * lo->x1 - lo->norm_a1 * lo->y1;
        lo->x1 = in->x;
        lo->y1 = out->y;
        break;
    }
    case IIR_2: {
        out->y = lo->norm_a0 * in->x + lo->norm_a1 * lo->x1 +
                 lo->norm_a2 * lo->x2 - lo->norm_a3 * lo->y1 -
                 lo->norm_a4 * lo->y2;
        lo->x2 = lo->x1;
        lo->x1 = in->x;
        lo->y2 = lo->y1;
        lo->y1 = out->y;
        break;
    }
    default: {
        return MERR(MESTATE);
    }
    }
    return MOK;
}

int iir_exec_in(struct iir_filter *iir, const float32_t x)
{
    if (!iir) {
        return MERR(MENULLPTR);
    }
    DECL(iir, in);

    in->x = x;
    return iir_exec(iir);
}

static bool is_iir_config_valid(const struct iir_cfg *cfg, const float32_t fs)
{
    if (!is_f32_finite_rt(fs) || !is_f32_finite_rt(cfg->fh) ||
        !is_f32_finite_rt(cfg->fl) || fs <= 0.0F || cfg->fh <= 0.0F ||
        cfg->order < IIR_1 || cfg->order > IIR_2 || cfg->type < IIR_LOWPASS ||
        cfg->type > IIR_BANDPASS) {
        return false;
    }
    if (cfg->order == IIR_1) {
        return cfg->type != IIR_BANDPASS;
    }
    // 二阶滤波的频率边界不得超过 Nyquist
    return cfg->fl > 0.0F && cfg->fh > cfg->fl && cfg->fh <= fs * 0.5F;
}
