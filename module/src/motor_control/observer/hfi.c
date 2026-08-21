#include "mathdef.h"
#include "timeops.h"

#include "motor_control/observer/hfi.h"

/* -------------------------------------------------------------------------- */
/*                                  内部函数                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 高频注入极性辨识
 *
 * @param hfi 高频注入结构体
 * @return    void
 */
static void
hfi_polar_idf_rt(struct hfi_obs *hfi)
{
    DECL(hfi, cfg, out, lo);

    switch (lo->e_polar_idf) {
        case HFI_POLAR_IDF_READY: {
            if (lo->polar_cnt++ > MS2CNT(cfg->polar_idf_ms, cfg->fs) / 3.0F * 1)
                lo->e_polar_idf = HFI_POLAR_IDF_POSITIVE;

            break;
        }
        case HFI_POLAR_IDF_POSITIVE: {
            out->id = cfg->hfi_id;

            if (lo->polar_cnt > MS2CNT(cfg->polar_idf_ms, cfg->fs) / 3.0F * 1.5F)
                lo->id_pos += ABS(lo->lpf_i_dq.d);

            if (lo->polar_cnt++ > MS2CNT(cfg->polar_idf_ms, cfg->fs) / 3.0F * 2)
                lo->e_polar_idf = HFI_POLAR_IDF_NEGATIVE;

            break;
        }
        case HFI_POLAR_IDF_NEGATIVE: {
            out->id = -cfg->hfi_id;

            if (lo->polar_cnt > MS2CNT(cfg->polar_idf_ms, cfg->fs) / 3.0F * 2.5F)
                lo->id_neg += ABS(lo->lpf_i_dq.d);

            if (lo->polar_cnt++ > MS2CNT(cfg->polar_idf_ms, cfg->fs) / 3.0F * 3) {
                lo->polar_offset = (ABS(lo->id_pos) > ABS(lo->id_neg)) ? 0.0F : PI;
                lo->e_polar_idf  = HFI_POLAR_IDF_FINISH;
            }
            break;
        }
        case HFI_POLAR_IDF_FINISH: {
            out->id       = 0.0F;
            lo->id_pos    = 0.0F;
            lo->id_neg    = 0.0F;
            lo->polar_cnt = 0;
            return;
        }
        default:
            break;
    }
}

/**
 * @brief 脉振正弦高频注入
 *
 * @param hfi 高频注入结构体
 * @return    void
 */
static void
hfi_exec_pulsating_sine_rt(struct hfi_obs *hfi)
{
    DECL(hfi, cfg, in, out, lo);
    RENAME(&lo->pll, pll);
    RENAME(&lo->id_bpf, id_bpf);
    RENAME(&lo->iq_bpf, iq_bpf);

    // pll
    pll_exec_theta_err_in_rt(pll, lo->lpf_i_dq.q);
    WARP_TAU(out->est_theta, pll->out.theta + lo->polar_offset);
    out->est_omega = pll->out.lpf_omega;

    /* 当前采样电流变换到估计 dq 坐标系。 */
    lo->est_i_dq = park_rt(in->i_ab, pll->out.theta);

    /*
     * 当前电流采样对应当前保持的注入相位。d/q 两路必须使用同一个
     * 同步解调参考，不能在两路解调之间提前更新 hfi_theta。
     */
    const float32_t demod_sin = SIN(lo->hfi_theta);

    iir_exec_in(id_bpf, lo->est_i_dq.d);
    iir_exec_in(iq_bpf, lo->est_i_dq.q);

    lo->hfi_i_dq.d = id_bpf->out.y * demod_sin;
    lo->hfi_i_dq.q = iq_bpf->out.y * demod_sin;

    LOWPASS(lo->lpf_i_dq.d, lo->hfi_i_dq.d, cfg->lpf_wc_dq.d, cfg->fs);
    LOWPASS(lo->lpf_i_dq.q, lo->hfi_i_dq.q, cfg->lpf_wc_dq.q, cfg->fs);

    /* 极性辨识使用本周期更新后的 d 轴解调结果。 */
    hfi_polar_idf_rt(hfi);

    /* 解调完成后再生成下一拍 d 轴高频电压。 */
    INTEGRATOR(lo->hfi_theta, HZ2RADS(cfg->fi), 1.0F, cfg->fs);
    WARP_TAU(lo->hfi_theta, lo->hfi_theta);
    out->vd = cfg->hfi_vd * COS(lo->hfi_theta + lo->polar_offset);
}

/**
 * @brief 脉振方波高频注入
 *
 * 在估计 d 轴注入 fs/2 方波电压 ±hfi_vd, 通过相邻两拍 q 轴电流差分
 * 与符号解调得到位置误差信号 ∝ sin(2Δθ), 送入 PLL.
 *
 * @param hfi 高频注入结构体
 * @return    void
 */
static void
hfi_exec_pulsating_square_rt(struct hfi_obs *hfi)
{
    DECL(hfi, cfg, in, out, lo);
    RENAME(&lo->pll, pll);

    /* PLL */
    pll_exec_theta_err_in_rt(pll, lo->lpf_i_dq.q);
    WARP_TAU(out->est_theta, pll->out.theta + lo->polar_offset);
    out->est_omega = pll->out.lpf_omega;

    /* 当前采样电流变换到估计 dq 坐标系 */
    lo->est_i_dq = park_rt(in->i_ab, pll->out.theta);

    /*
     * 当前 i[k] 主要对应上一控制周期施加的 HFI 电压，
     * 因此必须使用上一拍保持的 sq_sign 解调，
     * 不能提前翻转 sign。
     */
    const float32_t delta_id = lo->est_i_dq.d - lo->prev_est_i_dq.d;
    const float32_t delta_iq = lo->est_i_dq.q - lo->prev_est_i_dq.q;

    lo->prev_est_i_dq = lo->est_i_dq;

    lo->hfi_i_dq.d = (float32_t)lo->sq_sign * delta_id;
    lo->hfi_i_dq.q = (float32_t)lo->sq_sign * delta_iq;

    LOWPASS(lo->lpf_i_dq.d, lo->hfi_i_dq.d, cfg->lpf_wc_dq.d, cfg->fs);
    LOWPASS(lo->lpf_i_dq.q, lo->hfi_i_dq.q, cfg->lpf_wc_dq.q, cfg->fs);

    /*
     * 解调完上一拍响应之后，再生成下一拍注入电压。
     */
    lo->sq_sign = (lo->sq_sign >= 0) ? DIR_REVERSE : DIR_FORWARD;

    const float32_t polar_sign = COS(lo->polar_offset);

    out->vd = polar_sign * (float32_t)lo->sq_sign * cfg->hfi_vd;

    /* 极性辨识 */
    hfi_polar_idf_rt(hfi);
}

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
hfi_init(struct hfi_obs *hfi, const struct hfi_cfg hfi_cfg)
{
    DECL(hfi, cfg, lo);

    CFG_INIT(hfi, hfi_cfg);

    CFG_INIT(&lo->pll, cfg->pll);
    CFG_INIT(&lo->id_bpf, cfg->id_bpf);
    CFG_INIT(&lo->iq_bpf, cfg->iq_bpf);

    FS_INIT(cfg->fs, &cfg->pll, &cfg->id_bpf, &cfg->iq_bpf);

    pll_init(&lo->pll, cfg->pll);
    iir_init(&lo->id_bpf, cfg->id_bpf);
    iir_init(&lo->iq_bpf, cfg->iq_bpf);

    lo->sq_sign = DIR_FORWARD;
}

void
hfi_exec_rt(struct hfi_obs *hfi)
{
    DECL(hfi, cfg, out, lo);

    /* 告警模式只生成注入电压，不执行 PLL、解调和极性辨识 */
    if (cfg->alarm_enable) {
        INTEGRATOR(lo->alarm_theta, HZ2RADS(cfg->alarm_fi), 1.0F, cfg->fs);
        WARP_TAU(lo->alarm_theta, lo->alarm_theta);

        out->id = 0.0F;
        switch (cfg->e_type) {
            case HFI_TYPE_PULSATING_SQUARE:
            case HFI_TYPE_ROTATING_SQUARE: {
                out->vd = COS(lo->alarm_theta) >= 0.0F ? cfg->alarm_vd : -cfg->alarm_vd;
                break;
            }
            default: {
                out->vd = cfg->alarm_vd * COS(lo->alarm_theta);
                break;
            }
        }
        return;
    }

    switch (cfg->e_type) {
        case HFI_TYPE_PULSATING_SINE: {
            hfi_exec_pulsating_sine_rt(hfi);
            break;
        }
        case HFI_TYPE_PULSATING_SQUARE: {
            hfi_exec_pulsating_square_rt(hfi);
            break;
        }
        default:
            break;
    }
}

void
hfi_exec_in_rt(struct hfi_obs *hfi, const struct f32_ab i_ab)
{
    DECL(hfi, in);

    in->i_ab = i_ab;
    hfi_exec_rt(hfi);
}

void
hfi_alarm_set(struct hfi_obs *hfi, const uint32_t enable)
{
    hfi->cfg.alarm_enable = !!enable;
}
