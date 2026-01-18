#include "fft.h"
#include "timeops.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
fft_init(fft_t *fft, const fft_cfg_t fft_cfg)
{
        CFG_INIT(fft, fft_cfg);
        DECL(fft, cfg, in, out, lo);

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
        fftwf_execute(lo->p);
        for (size_t i = 0; i < cfg->npoints / 2 + 1; i++)
                out->mag_buf[i] = SQRT(lo->buf[i][0] * lo->buf[i][0] + lo->buf[i][1] * lo->buf[i][1]);
        find_max(&out->mag_buf[1], cfg->npoints >> 1, &out->max_mag, &out->out_idx);
#elif defined(ARM_MATH)
        arm_hanning_f32(lo->buf, cfg->npoints);
        arm_rfft_fast_f32(&lo->s, in->buf, lo->buf, cfg->flag);
        arm_cmplx_mag_f32(lo->buf, out->mag_buf, cfg->npoints >> 1);
        arm_max_f32(&out->mag_buf[1], cfg->npoints >> 1, &out->max_mag, &out->out_idx);
#endif

        out->ft = (f32)out->out_idx * cfg->fs / (f32)cfg->npoints;

        lo->need_exec = 0;

        lo->elapsed_us = (u32)(get_mono_ts_us() - start_ts);
}

void
fft_exec_in(fft_t *fft, const f32 val)
{
        DECL(fft, lo);

        if (spsc_write(&lo->spsc, &val, sizeof(val)) == 0)
                lo->need_exec = 1;
}
