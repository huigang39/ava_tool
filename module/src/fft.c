#include "fft.h"
#include "timeops.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
fft_init(struct fft *fft, const struct fft_cfg fft_cfg)
{
    DECL(fft, cfg, in, out, lo);
    CFG_INIT(fft, fft_cfg);

    spsc_init(&lo->spsc, cfg->buf, cfg->npoints * sizeof(float32_t), SPSC_POLICY_REJECT);

    in->buf      = cfg->in_buf;
    lo->buf      = cfg->out_buf;
    out->mag_buf = cfg->mag_buf;

    out->fr = cfg->fs / (float32_t)cfg->npoints;

#if defined(FFT_HAS_FFTW3)
    lo->p = fftwf_plan_dft_r2c_1d((int)cfg->npoints, in->buf, lo->buf, FFTW_ESTIMATE);
#elif defined(ARM_MATH)
    arm_rfft_fast_init_f32(&lo->s, (uint16_t)cfg->npoints);
#endif
}

void
fft_destroy(struct fft *fft)
{
    DECL(fft, lo);

#if defined(FFT_HAS_FFTW3)
    fftwf_destroy_plan(lo->p);
#endif
}

void
fft_exec(struct fft *fft)
{
    DECL(fft, cfg, in, out, lo);

    if (!lo->need_exec)
        return;

    const uint64_t start_ts = get_mono_ts_us();

    memcpy(in->buf, lo->spsc.buf, lo->spsc.cap);

#if defined(FFT_HAS_FFTW3)
    if (cfg->e_window != FFT_WINDOW_NONE) {
        for (size_t i = 0; i < cfg->npoints; i++) {
            float32_t w     = 1.0F;
            float32_t phase = 2.0F * PI * i / (cfg->npoints - 1);
            switch (cfg->e_window) {
                case FFT_WINDOW_HANNING: {
                    w = 0.5F * (1.0F - COS(phase));
                    break;
                }
                case FFT_WINDOW_HAMMING: {
                    w = 0.54F - 0.46F * COS(phase);
                    break;
                }
                case FFT_WINDOW_BLACKMAN: {
                    w = 0.42F - 0.5F * COS(phase) + 0.08F * COS(2.0F * phase);
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

    out->ft = (float32_t)out->out_idx * cfg->fs / (float32_t)cfg->npoints;

    lo->need_exec  = 0;
    lo->elapsed_us = (uint32_t)(get_mono_ts_us() - start_ts);
}

void
fft_exec_in(struct fft *fft, const float32_t val)
{
    DECL(fft, lo);

    if (spsc_write(&lo->spsc, &val, sizeof(val)) == 0)
        lo->need_exec = 1;
}
