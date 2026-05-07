#include "mathdef.h"
#include "timeops.h"

#include "fochfi.h"

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
hfi_polar_idf(hfi_obs_t *hfi)
{
        DECL(hfi, cfg, out, lo);

        switch (lo->e_polar_idf) {
                case HFI_POLAR_IDF_READY: {
                        if (lo->polar_cnt++ > MS2CNT(cfg->polar_idf_ms, cfg->fs) / 3.0f * 1)
                                lo->e_polar_idf = HFI_POLAR_IDF_POSITIVE;

                        break;
                }
                case HFI_POLAR_IDF_POSITIVE: {
                        out->id = cfg->hfi_id;

                        if (lo->polar_cnt > MS2CNT(cfg->polar_idf_ms, cfg->fs) / 3.0f * 1.5f)
                                lo->id_pos += ABS(lo->lpf_i_dq.d);

                        if (lo->polar_cnt++ > MS2CNT(cfg->polar_idf_ms, cfg->fs) / 3.0f * 2)
                                lo->e_polar_idf = HFI_POLAR_IDF_NEGATIVE;

                        break;
                }
                case HFI_POLAR_IDF_NEGATIVE: {
                        out->id = -cfg->hfi_id;

                        if (lo->polar_cnt > MS2CNT(cfg->polar_idf_ms, cfg->fs) / 3.0f * 2.5f)
                                lo->id_neg += ABS(lo->lpf_i_dq.d);

                        if (lo->polar_cnt++ > MS2CNT(cfg->polar_idf_ms, cfg->fs) / 3.0f * 3) {
                                lo->polar_offset = (ABS(lo->id_pos) > ABS(lo->id_neg)) ? 0.0f : PI;
                                lo->e_polar_idf  = HFI_POLAR_IDF_FINISH;
                        }
                        break;
                }
                case HFI_POLAR_IDF_FINISH: {
                        out->id       = 0.0f;
                        lo->id_pos    = 0.0f;
                        lo->id_neg    = 0.0f;
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
hfi_exec_pulsating_sine(hfi_obs_t *hfi)
{
        DECL(hfi, cfg, in, out, lo);
        RENAME(&lo->pll, pll);
        RENAME(&lo->id_bpf, id_bpf);
        RENAME(&lo->iq_bpf, iq_bpf);

        // pll
        pll_exec_theta_err_in(pll, lo->lpf_i_dq.q);
        WARP_TAU(out->est_theta, pll->out.theta + lo->polar_offset);
        out->est_omega = pll->out.lpf_omega;

        // park 变换
        lo->est_i_dq = park(in->i_ab, pll->out.theta);

        iir_exec_in(id_bpf, lo->est_i_dq.d);
        lo->hfi_i_dq.d = id_bpf->out.y * SIN(lo->hfi_theta);
        LOWPASS(lo->lpf_i_dq.d, lo->hfi_i_dq.d, cfg->lpf_wc_dq.d, cfg->fs);

        // 生成 d 轴高频电压
        INTEGRATOR(lo->hfi_theta, HZ2RADS(cfg->fi), 1.0f, cfg->fs);
        WARP_TAU(lo->hfi_theta, lo->hfi_theta);
        out->vd = cfg->hfi_vd * COS(lo->hfi_theta + lo->polar_offset);

        // 极性辨识
        hfi_polar_idf(hfi);

        iir_exec_in(iq_bpf, lo->est_i_dq.q);
        lo->hfi_i_dq.q = iq_bpf->out.y * SIN(lo->hfi_theta);
        LOWPASS(lo->lpf_i_dq.q, lo->hfi_i_dq.q, cfg->lpf_wc_dq.q, cfg->fs);
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
hfi_exec_pulsating_square(hfi_obs_t *hfi)
{
        DECL(hfi, cfg, in, out, lo);
        RENAME(&lo->pll, pll);

        // pll
        pll_exec_theta_err_in(pll, lo->lpf_i_dq.q);
        WARP_TAU(out->est_theta, pll->out.theta + lo->polar_offset);
        out->est_omega = pll->out.lpf_omega;

        // park 变换
        lo->est_i_dq = park(in->i_ab, pll->out.theta);

        // 翻转方波符号 (fs/2 方波)
        lo->sq_sign = (lo->sq_sign >= 0) ? DIR_REVERSE : DIR_FORWARD;

        // 解调: sign[k] * (i[k] - i[k-1])
        const f32 delta_id = lo->est_i_dq.d - lo->prev_est_i_dq.d;
        const f32 delta_iq = lo->est_i_dq.q - lo->prev_est_i_dq.q;
        lo->prev_est_i_dq  = lo->est_i_dq;

        lo->hfi_i_dq.d = (f32)lo->sq_sign * delta_id;
        lo->hfi_i_dq.q = (f32)lo->sq_sign * delta_iq;
        LOWPASS(lo->lpf_i_dq.d, lo->hfi_i_dq.d, cfg->lpf_wc_dq.d, cfg->fs);
        LOWPASS(lo->lpf_i_dq.q, lo->hfi_i_dq.q, cfg->lpf_wc_dq.q, cfg->fs);

        // 生成 d 轴方波电压, polar_offset == PI 时翻转极性
        const f32 polar_sign = COS(lo->polar_offset);
        out->vd              = polar_sign * (f32)lo->sq_sign * cfg->hfi_vd;

        // 极性辨识
        hfi_polar_idf(hfi);
}

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
hfi_init(hfi_obs_t *hfi, const hfi_cfg_t hfi_cfg)
{
        DECL(hfi, cfg, lo);
        CFG_INIT(hfi, hfi_cfg);
        CFG_INIT(&lo->pll, cfg->pll);
        CFG_INIT(&lo->id_bpf, cfg->id_bpf);
        CFG_INIT(&lo->iq_bpf, cfg->iq_bpf);

        cfg->pll.fs = cfg->id_bpf.fs = cfg->iq_bpf.fs = cfg->fs;
        pll_init(&lo->pll, cfg->pll);
        iir_init(&lo->id_bpf, cfg->id_bpf);
        iir_init(&lo->iq_bpf, cfg->iq_bpf);

        lo->sq_sign = 1;
}

void
hfi_exec(hfi_obs_t *hfi)
{
        DECL(hfi, cfg);
        CFG_CHECK(hfi, hfi_init);

        switch (cfg->e_type) {
                case HFI_TYPE_PULSATING_SINE: {
                        hfi_exec_pulsating_sine(hfi);
                        break;
                }
                case HFI_TYPE_PULSATING_SQUARE: {
                        hfi_exec_pulsating_square(hfi);
                        break;
                }
                default:
                        break;
        }
}

void
hfi_exec_in(hfi_obs_t *hfi, const f32_ab_t i_ab)
{
        DECL(hfi, in);

        in->i_ab = i_ab;
        hfi_exec(hfi);
}
