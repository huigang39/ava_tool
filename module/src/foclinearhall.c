#include "mathdef.h"

#include "foclinearhall.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
linearhall_init(linearhall_t *linearhall, const linearhall_cfg_t linearhall_cfg)
{
        DECL(linearhall, lo);
        CFG_INIT(linearhall, linearhall_cfg);

        lo->prev_theta = 0.0f;
        lo->theta_rate = 0.0f;
        lo->fault_time = 0.0f;
        lo->fault_cnt  = 0;
        lo->valid_cnt  = 0;
        memset(&lo->fault, 0, sizeof(lo->fault));

        RESET(linearhall, out);
}

void
linearhall_exec(linearhall_t *linearhall)
{
        DECL(linearhall, cfg, in, out, lo);

        f32 sin_processed, cos_processed;
        f32 sin_amp, cos_amp, norm_amp;
        f32 amp_diff, theta_diff;

        // 清除故障标志
        memset(&lo->fault, 0, sizeof(lo->fault));

        // 1. 信号有效性检查
        if (IS_NAN(in->sin_raw) || IS_NAN(in->cos_raw) || isinf(in->sin_raw) || isinf(in->cos_raw)) {
                lo->fault.signal_invalid  = 1;
                out->valid                = 0;
                lo->fault_time           += 1.0f / cfg->fs;
                goto fault;
        }

        // 2. 信号预处理：去除偏移并应用增益
        sin_processed = (in->sin_raw - cfg->sin_offset) * cfg->sin_gain;
        cos_processed = (in->cos_raw - cfg->cos_offset) * cfg->cos_gain;

        // 3. 计算信号幅值
        out->amp = SQRT(SQ(sin_processed) + SQ(cos_processed));

        // 4. 幅值检查
        if (out->amp < cfg->amp_min) {
                lo->fault.amp_too_low  = 1;
                out->valid             = 0;
                lo->fault_time        += 1.0f / cfg->fs;
                goto fault;
        }

        if (out->amp > cfg->amp_max) {
                lo->fault.amp_too_high  = 1;
                out->valid              = 0;
                lo->fault_time         += 1.0f / cfg->fs;
                goto fault;
        }

        // 5. 信号丢失检查（幅值接近零）
        if (out->amp < cfg->amp_min * 0.1f) {
                lo->fault.signal_lost  = 1;
                out->valid             = 0;
                lo->fault_time        += 1.0f / cfg->fs;
                goto fault;
        }

        // 6. 归一化信号
        norm_amp      = 1.0f / out->amp;
        out->sin_norm = sin_processed * norm_amp;
        out->cos_norm = cos_processed * norm_amp;

        // 7. 幅值一致性检查（sin和cos的幅值应该接近）
        sin_amp  = ABS(sin_processed);
        cos_amp  = ABS(cos_processed);
        amp_diff = ABS(sin_amp - cos_amp);
        if (amp_diff > cfg->amp_tolerance)
                lo->fault.amp_mismatch = 1;

        // 8. 计算角度
        WARP_TAU(out->theta, ATAN2(out->sin_norm, out->cos_norm));

        // 9. 角度变化率检查
        WARP_PI(theta_diff, out->theta - lo->prev_theta);
        lo->theta_rate = ABS(theta_diff) * cfg->fs;

        if (lo->theta_rate > cfg->theta_rate_max) {
                lo->fault.theta_rate_fault  = 1;
                out->valid                  = 0;
                lo->fault_time             += 1.0f / cfg->fs;
                goto fault;
        }

        // 10. 所有检查通过，角度有效
        out->valid     = 1;
        lo->prev_theta = out->theta;
        lo->fault_time = 0.0f;
        lo->valid_cnt++;

fault:
        if (!out->valid) {
                lo->fault_cnt++;
                if (lo->fault_time > cfg->fault_timeout)
                        out->theta = lo->prev_theta;

        } else
                lo->fault_cnt = 0;
}

void
linearhall_exec_in(linearhall_t *linearhall, const f32 sin_raw, const f32 cos_raw)
{
        DECL(linearhall, in);

        in->sin_raw = sin_raw;
        in->cos_raw = cos_raw;
        linearhall_exec(linearhall);
}
