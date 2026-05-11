#include "fir.h"
#include "macrodef.h"
#include "mathdef.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

i32
fir_init(fir_filter_t *fir, const fir_cfg_t fir_cfg)
{
        DECL(fir, cfg, lo);
        ARG_CHECK(fir);
        CFG_INIT(fir, fir_cfg);

        switch (cfg->order) {
                case FIR_1: {
                        switch (cfg->type) {
                                case FIR_LOWPASS: {
                                        lo->b0 = 0.5f;
                                        lo->b1 = 0.5f;
                                        break;
                                }
                                case FIR_HIGHPASS: {
                                        lo->b0 = 0.5f;
                                        lo->b1 = -0.5f;
                                        break;
                                }
                                default:
                                        break;
                        }
                        break;
                }
                case FIR_2: {
                        switch (cfg->type) {
                                case FIR_LOWPASS: {
                                        lo->b0 = 1.0f / 3.0f;
                                        lo->b1 = 1.0f / 3.0f;
                                        lo->b2 = 1.0f / 3.0f;
                                        break;
                                }
                                case FIR_HIGHPASS: {
                                        lo->b0 = 0.5f;
                                        lo->b1 = 0.0f;
                                        lo->b2 = -0.5f;
                                        break;
                                }
                                case FIR_BANDPASS: {
                                        lo->fc = (cfg->fh + cfg->fl) / 2.0f;
                                        lo->w0 = TAU * lo->fc / cfg->fs;
                                        lo->k  = SIN(lo->w0) / 2.0f;
                                        lo->b0 = lo->k;
                                        lo->b1 = 0.0f;
                                        lo->b2 = -lo->k;
                                        break;
                                }
                                default:
                                        break;
                        }
                        break;
                }
                case FIR_3: {
                        switch (cfg->type) {
                                case FIR_LOWPASS: {
                                        lo->b0 = 0.25f;
                                        lo->b1 = 0.25f;
                                        lo->b2 = 0.25f;
                                        lo->b3 = 0.25f;
                                        break;
                                }
                                case FIR_HIGHPASS: {
                                        lo->b0 = 0.25f;
                                        lo->b1 = -0.25f;
                                        lo->b2 = -0.25f;
                                        lo->b3 = 0.25f;
                                        break;
                                }
                                case FIR_BANDPASS: {
                                        lo->fc = (cfg->fh + cfg->fl) / 2.0f;
                                        lo->w0 = TAU * lo->fc / cfg->fs;
                                        lo->k  = SIN(lo->w0) / 2.0f;
                                        lo->b0 = lo->k;
                                        lo->b1 = -lo->k;
                                        lo->b2 = -lo->k;
                                        lo->b3 = lo->k;
                                        break;
                                }
                                default:
                                        break;
                        }
                        break;
                }
                default:
                        break;
        }

        return 0;
}

void
fir_exec(fir_filter_t *fir)
{
        DECL(fir, cfg, in, out, lo);
        CFG_CHECK(fir, fir_init);

        switch (cfg->order) {
                case FIR_1: {
                        out->y = lo->b0 * in->x + lo->b1 * lo->x1;
                        lo->x1 = in->x;
                        break;
                }
                case FIR_2: {
                        out->y = lo->b0 * in->x + lo->b1 * lo->x1 + lo->b2 * lo->x2;
                        lo->x2 = lo->x1;
                        lo->x1 = in->x;
                        break;
                }
                case FIR_3: {
                        out->y = lo->b0 * in->x + lo->b1 * lo->x1 + lo->b2 * lo->x2 + lo->b3 * lo->x3;
                        lo->x3 = lo->x2;
                        lo->x2 = lo->x1;
                        lo->x1 = in->x;
                        break;
                }
                default:
                        break;
        }
}

void
fir_exec_in(fir_filter_t *fir, const f32 x)
{
        DECL(fir, in);

        in->x = x;
        fir_exec(fir);
}
