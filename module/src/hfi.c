#include "clarkepark.h"
#include "mathdef.h"

#include "hfi.h"

/* -------------------------------------------------------------------------- */
/*                                  内部函数                                  */
/* -------------------------------------------------------------------------- */

static void
hfi_polar_idf(hfi_obs_t *hfi)
{
        DECL(hfi, cfg, out, lo);

        switch (lo->e_polar_idf) {
                case HFI_POLAR_IDF_READY: {
                        if (lo->polar_cnt == cfg->polar_cnt_max * 1)
                                lo->e_polar_idf = HFI_POLAR_IDF_POSITIVE;
                        break;
                }
                case HFI_POLAR_IDF_POSITIVE: {
                        out->id     = cfg->hfi_id;
                        lo->id_pos += ABS(lo->lpf_i_dq.d);
                        if (lo->polar_cnt == cfg->polar_cnt_max * 2)
                                lo->e_polar_idf = HFI_POLAR_IDF_NEGATIVE;
                        break;
                }
                case HFI_POLAR_IDF_NEGATIVE: {
                        out->id     = -cfg->hfi_id;
                        lo->id_neg += ABS(lo->lpf_i_dq.d);
                        if (lo->polar_cnt == cfg->polar_cnt_max * 3) {
                                lo->polar_offset = (ABS(lo->id_pos) > ABS(lo->id_neg)) ? 0.0f : PI;
                                lo->e_polar_idf  = HFI_POLAR_IDF_FINISH;
                        }
                        break;
                }
                case HFI_POLAR_IDF_FINISH: {
                        out->id       = 0.0f;
                        lo->polar_cnt = 0;
                        return;
                }
                default:
                        break;
        }

        lo->polar_cnt++;
}

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
hfi_init(hfi_obs_t *hfi, const hfi_cfg_t hfi_cfg)
{
        CFG_INIT(hfi, hfi_cfg);
        DECL(hfi, cfg, lo);

        lo->pll.cfg.fs = lo->id_bpf.cfg.fs = lo->iq_bpf.cfg.fs = cfg->fs;

        pll_init(&lo->pll, lo->pll.cfg);
        iir_init(&lo->id_bpf, lo->id_bpf.cfg);
        iir_init(&lo->iq_bpf, lo->iq_bpf.cfg);
}

void
hfi_exec(hfi_obs_t *hfi)
{
        DECL(hfi, cfg, in, out, lo);
        CFG_CHECK(hfi, hfi_init);

        // pll
        RENAME(&lo->pll, pll);
        pll_exec_theta_err_in(pll, lo->lpf_i_dq.q);
        WARP_TAU(out->est_theta, pll->out.theta + lo->polar_offset);
        out->est_omega = pll->out.lpf_omega;

        // park变换
        lo->est_i_dq = park(in->i_ab, pll->out.theta);

        RENAME(&lo->id_bpf, id_bpf);
        iir_exec_in(id_bpf, lo->est_i_dq.d);
        lo->hfi_i_dq.d = id_bpf->out.y * SIN(lo->hfi_theta);
        LOWPASS(lo->lpf_i_dq.d, lo->hfi_i_dq.d, cfg->lpf_wc_dq.d, cfg->fs);

        // 生成d轴高频电压
        INTEGRATOR(lo->hfi_theta, HZ2RADS(cfg->fi), 1.0f, cfg->fs);
        WARP_TAU(lo->hfi_theta, lo->hfi_theta);
        out->vd = cfg->hfi_vd * COS(lo->hfi_theta + lo->polar_offset);

        // 极性辨识
        hfi_polar_idf(hfi);

        RENAME(&lo->iq_bpf, iq_bpf);
        iir_exec_in(iq_bpf, lo->est_i_dq.q);
        lo->hfi_i_dq.q = iq_bpf->out.y * SIN(lo->hfi_theta);
        LOWPASS(lo->lpf_i_dq.q, lo->hfi_i_dq.q, cfg->lpf_wc_dq.q, cfg->fs);
}

void
hfi_exec_in(hfi_obs_t *hfi, const f32_ab_t i_ab)
{
        DECL(hfi, in);

        in->i_ab = i_ab;
        hfi_exec(hfi);
}
