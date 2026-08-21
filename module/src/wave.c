#include "wave.h"
#include "mathdef.h"

/* -------------------------------------------------------------------------- */
/*                                  内部函数                                  */
/* -------------------------------------------------------------------------- */

static float32_t
wave_calc_val(enum wave_type type, float32_t phase, float32_t amp, float32_t offset, float32_t duty)
{
    float32_t val = 0.0F;
    switch (type) {
        case WAVE_TYPE_SINE:
            val = amp * SIN(phase) + offset;
            break;
        case WAVE_TYPE_SQUARE:
            val = (phase < (duty * TAU)) ? (amp + offset) : (-amp + offset);
            break;
        case WAVE_TYPE_TRIANGLE: {
            float32_t rise_end = duty * TAU;
            if (phase < rise_end)
                val = -amp + 2.0F * amp * (phase / rise_end);
            else
                val = amp - 2.0F * amp * ((phase - rise_end) / (TAU - rise_end));

            val += offset;
            break;
        }
        default:
            break;
    }
    return val;
}

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
wave_init(struct wave *wave, const struct wave_cfg wave_cfg)
{
    CFG_INIT(wave, wave_cfg);

    /* 初始化时,让当前值直接等于目标值,防止启动时发生从 0 开始的阶跃 */
    wave->lo.curr_freq   = wave->cfg.freq;
    wave->lo.curr_amp    = wave->cfg.amp;
    wave->lo.curr_offset = wave->cfg.offset;
    wave->lo.curr_duty   = wave->cfg.duty;
}

void
wave_exec(struct wave *wave)
{
    DECL(wave, cfg, out, lo);

    float32_t alpha =
        (cfg->smooth_factor <= 0.0F || cfg->smooth_factor > 1.0F) ? 1.0F : cfg->smooth_factor;
    lo->curr_freq   += (cfg->freq - lo->curr_freq) * alpha;
    lo->curr_amp    += (cfg->amp - lo->curr_amp) * alpha;
    lo->curr_offset += (cfg->offset - lo->curr_offset) * alpha;
    lo->curr_duty   += (cfg->duty - lo->curr_duty) * alpha;

    // 上一次过渡完成后才能开始新的波形混合.
    if (cfg->type != lo->last_type && lo->mix >= 1.0F)
        lo->mix = 0.0F;

    float32_t val_next =
        wave_calc_val(cfg->type, cfg->phase, lo->curr_amp, lo->curr_offset, lo->curr_duty);

    if (lo->mix < 1.0F) {
        float32_t val_prev =
            wave_calc_val(lo->last_type, cfg->phase, lo->curr_amp, lo->curr_offset, lo->curr_duty);

        out->val = val_prev * (1.0F - lo->mix) + val_next * lo->mix;

        lo->mix += alpha;

        if (lo->mix >= 1.0F) {
            lo->mix       = 1.0F;
            lo->last_type = cfg->type;
        }
    } else {
        out->val      = val_next;
        lo->last_type = cfg->type;
    }

    lo->phase_incr  = TAU * lo->curr_freq / cfg->fs;
    cfg->phase     += lo->phase_incr;
    WARP_TAU(cfg->phase, cfg->phase);
}
