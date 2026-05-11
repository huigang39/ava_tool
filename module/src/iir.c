#include "iir.h"
#include "mathdef.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

i32
iir_init(iir_filter_t *iir, const iir_cfg_t iir_cfg)
{
        DECL(iir, cfg, lo);
        ARG_CHECK(iir);
        CFG_INIT(iir, iir_cfg);

        switch (cfg->order) {
                case IIR_1: {
                        lo->fc    = cfg->fh;
                        lo->rc    = 1.0f / HZ2RADS(lo->fc);
                        lo->alpha = 1.0f / (1.0f + lo->rc * cfg->fs);

                        switch (cfg->type) {
                                case IIR_LOWPASS: {
                                        lo->b0 = lo->alpha;
                                        lo->b1 = 0.0f;
                                        break;
                                }
                                case IIR_HIGHPASS: {
                                        lo->b0 = 1.0f - lo->alpha;
                                        lo->b1 = -(1.0f - lo->alpha);
                                        break;
                                }
                                default:
                                        break;
                        }
                        lo->norm_a1 = -(1.0f - lo->alpha);
                        break;
                }
                case IIR_2: {
                        lo->fc    = (cfg->fh + cfg->fl) / 2.0f;
                        lo->q     = SQRT(cfg->fh * cfg->fl) / (cfg->fh - cfg->fl);
                        lo->w0    = HZ2RADS(lo->fc) / cfg->fs;
                        lo->alpha = SIN(lo->w0) / (2.0f * lo->q);

                        switch (cfg->type) {
                                case IIR_LOWPASS: {
                                        lo->b0 = (1.0f - COS(lo->w0)) / 2.0f;
                                        lo->b1 = lo->b0 * 2.0f;
                                        lo->b2 = lo->b0;
                                        lo->a0 = 1.0f + lo->alpha;
                                        lo->a1 = -2.0f * COS(lo->w0);
                                        lo->a2 = 1.0f - lo->alpha;
                                        break;
                                }
                                case IIR_HIGHPASS: {
                                        lo->b0 = (1.0f + COS(lo->w0)) / 2.0f;
                                        lo->b1 = -lo->b0 * 2.0f;
                                        lo->b2 = lo->b0;
                                        lo->a0 = 1.0f + lo->alpha;
                                        lo->a1 = -2.0f * COS(lo->w0);
                                        lo->a2 = 1.0f - lo->alpha;
                                        break;
                                }
                                case IIR_BANDPASS: {
                                        lo->b0 = lo->alpha;
                                        lo->b1 = 0.0f;
                                        lo->b2 = -lo->alpha;
                                        lo->a0 = 1.0f + lo->alpha;
                                        lo->a1 = -2.0f * COS(lo->w0);
                                        lo->a2 = 1.0f - lo->alpha;
                                        break;
                                }
                                default:
                                        break;
                        }
                        // 归一化
                        lo->norm_a0 = lo->b0 / lo->a0;
                        lo->norm_a1 = lo->b1 / lo->a0;
                        lo->norm_a2 = lo->b2 / lo->a0;
                        lo->norm_a3 = lo->a1 / lo->a0;
                        lo->norm_a4 = lo->a2 / lo->a0;
                        break;
                }
                default:
                        break;
        }

        return 0;
}

void
iir_exec(iir_filter_t *iir)
{
        DECL(iir, cfg, in, out, lo);
        CFG_CHECK(iir, iir_init);

        switch (cfg->order) {
                case IIR_1: {
                        out->y = lo->b0 * in->x + lo->b1 * lo->x1 - lo->norm_a1 * lo->y1;
                        lo->x1 = in->x;
                        lo->y1 = out->y;
                        break;
                }
                case IIR_2: {
                        out->y = lo->norm_a0 * in->x + lo->norm_a1 * lo->x1 + lo->norm_a2 * lo->x2 - lo->norm_a3 * lo->y1 -
                                 lo->norm_a4 * lo->y2;
                        lo->x2 = lo->x1;
                        lo->x1 = in->x;
                        lo->y2 = lo->y1;
                        lo->y1 = out->y;
                        break;
                }
                default:
                        break;
        }
}

void
iir_exec_in(iir_filter_t *iir, const f32 x)
{
        DECL(iir, in);

        in->x = x;
        iir_exec(iir);
}
