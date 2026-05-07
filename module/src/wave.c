#include "wave.h"
#include "mathdef.h"

/* -------------------------------------------------------------------------- */
/*                                  内部函数                                  */
/* -------------------------------------------------------------------------- */

static f32
wave_calc_val(wave_type_t type, f32 phase, f32 amp, f32 offset, f32 duty)
{
        f32 val = 0.0f;
        switch (type) {
                case WAVE_TYPE_SINE:
                        val = amp * SIN(phase) + offset;
                        break;
                case WAVE_TYPE_SQUARE:
                        val = (phase < (duty * TAU)) ? (amp + offset) : (-amp + offset);
                        break;
                case WAVE_TYPE_TRIANGLE: {
                        f32 rise_end = duty * TAU;
                        if (phase < rise_end)
                                val = -amp + 2.0f * amp * (phase / rise_end);
                        else
                                val = amp - 2.0f * amp * ((phase - rise_end) / (TAU - rise_end));

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
wave_init(wave_t *wave, const wave_cfg_t wave_cfg)
{
        CFG_INIT(wave, wave_cfg);

        /* 初始化时，让当前值直接等于目标值，防止启动时发生从 0 开始的阶跃 */
        wave->lo.curr_freq   = wave->cfg.freq;
        wave->lo.curr_amp    = wave->cfg.amp;
        wave->lo.curr_offset = wave->cfg.offset;
        wave->lo.curr_duty   = wave->cfg.duty;
}

void
wave_exec(wave_t *wave)
{
        DECL(wave, cfg, out, lo);

        /* 1. 参数平滑 */
        f32 alpha        = (cfg->smooth_factor <= 0.0f || cfg->smooth_factor > 1.0f) ? 1.0f : cfg->smooth_factor;
        lo->curr_freq   += (cfg->freq - lo->curr_freq) * alpha;
        lo->curr_amp    += (cfg->amp - lo->curr_amp) * alpha;
        lo->curr_offset += (cfg->offset - lo->curr_offset) * alpha;
        lo->curr_duty   += (cfg->duty - lo->curr_duty) * alpha;

        /* 2. 类型切换检测: 只有当新目标和“当前正在运行的类型”不同，且不在混合中时，触发混合 */
        if (cfg->type != lo->last_type && lo->mix >= 1.0f)
                lo->mix = 0.0f; // 开启混合

        /* 3. 计算新旧输出 */
        f32 val_next = wave_calc_val(cfg->type, cfg->phase, lo->curr_amp, lo->curr_offset, lo->curr_duty);

        if (lo->mix < 1.0f) {
                /* 正在过渡: 计算旧波形值 */
                f32 val_prev = wave_calc_val(lo->last_type, cfg->phase, lo->curr_amp, lo->curr_offset, lo->curr_duty);

                /* 线性插值混合 */
                out->val = val_prev * (1.0f - lo->mix) + val_next * lo->mix;

                /* 推进混合进度 */
                lo->mix += alpha;

                /* 只有混合完成了，才把当前类型标记为目标类型 */
                if (lo->mix >= 1.0f) {
                        lo->mix       = 1.0f;
                        lo->last_type = cfg->type;
                }
        } else {
                /* 稳定期: 直接输出 */
                out->val      = val_next;
                lo->last_type = cfg->type; // 确保同步
        }

        /* 4. 相位累加 */
        lo->phase_incr  = TAU * lo->curr_freq / cfg->fs;
        cfg->phase     += lo->phase_incr;
        WARP_TAU(cfg->phase, cfg->phase);
}
