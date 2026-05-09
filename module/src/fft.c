#include "fft.h"
#include "timeops.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
fft_init(fft_t *fft, const fft_cfg_t fft_cfg)
{
        DECL(fft, cfg, in, out, lo);
        CFG_INIT(fft, fft_cfg);

        spsc_init(&lo->spsc, cfg->buf, cfg->npoints * sizeof(f32), SPSC_POLICY_REJECT);

        in->buf      = cfg->in_buf;
        lo->buf      = cfg->out_buf;
        out->mag_buf = cfg->mag_buf;

        out->fr = cfg->fs / (f32)cfg->npoints;

#if defined(__linux__) || defined(_WIN32)
        lo->p = fftwf_plan_dft_r2c_1d(cfg->npoints, in->buf, lo->buf, FFTW_ESTIMATE);
#elif defined(ARM_MATH)
        arm_rfft_fast_init_f32(&lo->s, (u16)cfg->npoints);
#endif
}

void
fft_destroy(fft_t *fft)
{
        DECL(fft, lo);

#if defined(__linux__) || defined(_WIN32)
        fftwf_destroy_plan(lo->p);
#endif
}

void
fft_exec(fft_t *fft)
{
        DECL(fft, cfg, in, out, lo);

        if (!lo->need_exec)
                return;

        const u64 start_ts = get_mono_ts_us();

        memcpy(in->buf, lo->spsc.buf, lo->spsc.cap);

#if defined(__linux__) || defined(_WIN32)
        if (cfg->e_window != FFT_WINDOW_NONE) {
                for (size_t i = 0; i < cfg->npoints; i++) {
                        float w     = 1.0f;
                        float phase = 2.0f * PI * i / (cfg->npoints - 1);
                        switch (cfg->e_window) {
                                case FFT_WINDOW_HANNING: {
                                        w = 0.5f * (1.0f - COS(phase));
                                        break;
                                }
                                case FFT_WINDOW_HAMMING: {
                                        w = 0.54f - 0.46f * COS(phase);
                                        break;
                                }
                                case FFT_WINDOW_BLACKMAN: {
                                        w = 0.42f - 0.5f * COS(phase) + 0.08f * COS(2.0f * phase);
                                        break;
                                }
                                default:
                                        break;
                        }
                        in->buf[i] *= w;
                }
        }
        fftwf_execute(lo->p);
        for (size_t i = 0; i < cfg->npoints / 2 + 1; i++)
                out->mag_buf[i] = SQRT(lo->buf[i][0] * lo->buf[i][0] + lo->buf[i][1] * lo->buf[i][1]);

        find_max(&out->mag_buf[1], cfg->npoints >> 1, &out->max_mag, &out->out_idx);
#elif defined(ARM_MATH)
        if (cfg->e_window != FFT_WINDOW_NONE) {
                switch (cfg->e_window) {
                        case FFT_WINDOW_HANNING: {
                                arm_hanning_f32(lo->buf, cfg->npoints);
                                break;
                        }
                        case FFT_WINDOW_HAMMING: {
                                arm_hamming_f32(lo->buf, cfg->npoints);
                                break;
                        }
                        case FFT_WINDOW_BLACKMAN: {
                                arm_blackman_harris_92db_f32(lo->buf, cfg->npoints);
                                break;
                        }
                        default:
                                break;
                }
        }
        arm_rfft_fast_f32(&lo->s, in->buf, lo->buf, cfg->flag);
        arm_cmplx_mag_f32(lo->buf, out->mag_buf, cfg->npoints >> 1);
        arm_max_f32(&out->mag_buf[1], cfg->npoints >> 1, &out->max_mag, &out->out_idx);
#endif

        out->ft = (f32)out->out_idx * cfg->fs / (f32)cfg->npoints;

        lo->need_exec  = 0;
        lo->elapsed_us = (u32)(get_mono_ts_us() - start_ts);
}

void
fft_exec_in(fft_t *fft, const f32 val)
{
        DECL(fft, lo);

        if (spsc_write(&lo->spsc, &val, sizeof(val)) == 0)
                lo->need_exec = 1;
}
