#include "wave.h"
#include "mathdef.h"

/**
 * @brief         检查波形配置
 * @param[in] cfg 波形配置
 * @return        配置有效返回 true
 */
static bool is_wave_config_valid(const struct wave_cfg *cfg);

/**
 * @brief            根据波形类型和相位计算输出
 * @param[in] type   波形类型
 * @param[in] phase  波形相位
 * @param[in] amp    波形幅值
 * @param[in] offset 数据偏移
 * @param[in] duty   占空比
 */
static float32_t wave_calc_val(enum wave_type type,
                               float32_t phase,
                               float32_t amp,
                               float32_t offset,
                               float32_t duty);

int wave_init(struct wave *wave, const struct wave_cfg wave_cfg)
{
    if (!wave) {
        return MERR(MENULLPTR);
    }
    if (!is_wave_config_valid(&wave_cfg)) {
        return MERR(MEINVAL);
    }
    memset(&wave->lo, 0, sizeof(wave->lo));
    memset(&wave->out, 0, sizeof(wave->out));
    CFG_INIT(wave, wave_cfg);

    // 初始值直接使用配置, 不从 0 开始渐变
    wave->lo.phase = wave_cfg.phase;
    wave->lo.curr_freq = wave->cfg.freq;
    wave->lo.curr_amp = wave->cfg.amp;
    wave->lo.curr_offset = wave->cfg.offset;
    wave->lo.curr_duty = wave->cfg.duty;
    wave->lo.prev_type = wave->cfg.type;
    wave->lo.mix = 1.0F;
    return MOK;
}

int wave_exec(struct wave *wave)
{
    if (!wave) {
        return MERR(MENULLPTR);
    }
    if (!is_wave_config_valid(&wave->cfg) ||
        !is_f32_finite_rt(wave->lo.phase) ||
        !is_f32_finite_rt(wave->lo.curr_freq) ||
        !is_f32_finite_rt(wave->lo.curr_amp) ||
        !is_f32_finite_rt(wave->lo.curr_offset) ||
        !is_f32_finite_rt(wave->lo.curr_duty) ||
        !is_f32_finite_rt(wave->lo.mix)) {
        wave->out.val = 0.0F;
        return MERR(MESTATE);
    }
    CFG_READ(wave);
    DECL(wave, out, lo);

    float32_t alpha = (cfg->smooth_factor <= 0.0F || cfg->smooth_factor > 1.0F)
                          ? 1.0F
                          : cfg->smooth_factor;
    lo->curr_freq += (cfg->freq - lo->curr_freq) * alpha;
    lo->curr_amp += (cfg->amp - lo->curr_amp) * alpha;
    lo->curr_offset += (cfg->offset - lo->curr_offset) * alpha;
    lo->curr_duty += (cfg->duty - lo->curr_duty) * alpha;

    // 上一次过渡完成后才能开始新的波形混合
    if (cfg->type != lo->prev_type && lo->mix >= 1.0F) {
        lo->mix = 0.0F;
    }

    float32_t val_next = wave_calc_val(
        cfg->type, lo->phase, lo->curr_amp, lo->curr_offset, lo->curr_duty);

    if (lo->mix < 1.0F) {
        float32_t val_prev = wave_calc_val(lo->prev_type,
                                           lo->phase,
                                           lo->curr_amp,
                                           lo->curr_offset,
                                           lo->curr_duty);

        out->val = val_prev * (1.0F - lo->mix) + val_next * lo->mix;

        lo->mix += alpha;

        if (lo->mix >= 1.0F) {
            lo->mix = 1.0F;
            lo->prev_type = cfg->type;
        }
    } else {
        out->val = val_next;
        lo->prev_type = cfg->type;
    }

    lo->phase_increment = TAU * lo->curr_freq / cfg->fs;
    lo->phase += lo->phase_increment;
    if (!is_f32_finite_rt(out->val) || !is_f32_finite_rt(lo->phase)) {
        out->val = 0.0F;
        return MERR(MEOVERFLOW);
    }
    WARP_TAU(lo->phase, lo->phase);
    return MOK;
}

static bool is_wave_config_valid(const struct wave_cfg *cfg)
{
    return is_f32_finite_rt(cfg->fs) && is_f32_finite_rt(cfg->freq) &&
           is_f32_finite_rt(cfg->amp) && is_f32_finite_rt(cfg->phase) &&
           is_f32_finite_rt(cfg->offset) && is_f32_finite_rt(cfg->duty) &&
           is_f32_finite_rt(cfg->smooth_factor) && cfg->fs > 0.0F &&
           cfg->freq >= 0.0F && cfg->type >= WAVE_TYPE_SINE &&
           cfg->type <= WAVE_TYPE_TRIANGLE &&
           (cfg->type != WAVE_TYPE_TRIANGLE ||
            (cfg->duty > 0.0F && cfg->duty < 1.0F));
}

static float32_t wave_calc_val(enum wave_type type,
                               float32_t phase,
                               float32_t amp,
                               float32_t offset,
                               float32_t duty)
{
    float32_t val = 0.0F;
    switch (type) {
    case WAVE_TYPE_SINE: {
        val = amp * SIN(phase) + offset;
        break;
    }
    case WAVE_TYPE_SQUARE: {
        val = (phase < (duty * TAU)) ? (amp + offset) : (-amp + offset);
        break;
    }
    case WAVE_TYPE_TRIANGLE: {
        float32_t rise_end = duty * TAU;
        if (phase < rise_end) {
            val = -amp + 2.0F * amp * (phase / rise_end);
        } else {
            val = amp - 2.0F * amp * ((phase - rise_end) / (TAU - rise_end));
        }

        val += offset;
        break;
    }
    default: {
        break;
    }
    }
    return val;
}
