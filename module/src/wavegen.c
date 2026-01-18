#include "wavegen.h"
#include "mathdef.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
wave_init(wave_t *wave, const wave_cfg_t wave_cfg)
{
        CFG_INIT(wave, wave_cfg);
}

void
wave_exec(wave_t *wave)
{
        DECL(wave, cfg, out, lo);

        lo->phase_incr = TAU * cfg->wave_freq / cfg->fs;

        switch (cfg->type) {
                case WAVE_TYPE_SINE: {
                        out->val = cfg->amp * SIN(cfg->phase) + cfg->offset;
                        break;
                }
                case WAVE_TYPE_SQUARE: {
                        if (cfg->phase < (cfg->duty * TAU))
                                out->val = cfg->amp + cfg->offset;
                        else
                                out->val = -cfg->amp + cfg->offset;
                        break;
                }
                default:
                        break;
        }

        cfg->phase += lo->phase_incr;
        WARP_TAU(cfg->phase, cfg->phase);
}
