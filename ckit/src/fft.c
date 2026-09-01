#include "fft.h"
#include "timeops.h"

#include <limits.h>

#if defined(FFT_HAS_FFTW3) || defined(ARM_MATH)
/**
 * @brief             将配置的窗函数乘到输入样本
 * @param[in,out] fft FFT 对象
 */
static void fft_apply_window(struct fft *fft);
#endif

int fft_init(struct fft *fft, const struct fft_cfg fft_cfg)
{
    if (!fft || !fft_cfg.buf || !fft_cfg.in_buf || !fft_cfg.out_buf ||
        !fft_cfg.magnitude_buf) {
        return MERR(MENULLPTR);
    }
    if (fft_cfg.point_cnt < 2U || !IS_POWER_OF_2(fft_cfg.point_cnt) ||
        !is_f32_finite_rt(fft_cfg.fs) || fft_cfg.fs <= 0.0F ||
        fft_cfg.flag > 1U || fft_cfg.window < FFT_WINDOW_NONE ||
        fft_cfg.window > FFT_WINDOW_BLACKMAN) {
        return MERR(MEINVAL);
    }
#if defined(FFT_HAS_FFTW3)
    if (fft_cfg.point_cnt > INT_MAX) {
        return MERR(MEOVERFLOW);
    }
    if (fft_cfg.flag != 0U) {
        return MERR(MENOTSUP);
    }
#elif defined(ARM_MATH)
    if (fft_cfg.point_cnt < 32U || fft_cfg.point_cnt > 4096U) {
        return MERR(MEINVAL);
    }
#else
    return MERR(MENOTSUP);
#endif
#if SIZE_MAX <= UINT32_MAX
    if (fft_cfg.point_cnt > SIZE_MAX / sizeof(float32_t)) {
        return MERR(MEOVERFLOW);
    }
#endif

    DECL(fft, cfg, in, out, lo);

    memset(lo, 0, sizeof(*lo));
    CFG_INIT(fft, fft_cfg);

    const int spsc_ret = spsc_init(&lo->spsc,
                                   cfg->buf,
                                   cfg->point_cnt * sizeof(float32_t),
                                   SPSC_POLICY_REJECT);
    if (spsc_ret < 0) {
        return spsc_ret;
    }

    in->buf = cfg->in_buf;
    lo->buf = cfg->out_buf;
    out->magnitude_buf = cfg->magnitude_buf;

    out->fr = cfg->fs / (float32_t)cfg->point_cnt;

#if defined(FFT_HAS_FFTW3)
    lo->p = fftwf_plan_dft_r2c_1d(
        (int)cfg->point_cnt, in->buf, lo->buf, FFTW_ESTIMATE);
    if (!lo->p) {
        lo->buf = NULL;
        return MERR(MEALLOC);
    }
#elif defined(ARM_MATH)
    if (arm_rfft_fast_init_f32(&lo->s, (uint16_t)cfg->point_cnt) !=
        ARM_MATH_SUCCESS) {
        lo->buf = NULL;
        return MERR(MEINVAL);
    }
#endif
    return MOK;
}

int fft_destroy(struct fft *fft)
{
    if (!fft) {
        return MERR(MENULLPTR);
    }
    DECL(fft, lo);

#if defined(FFT_HAS_FFTW3)
    if (!lo->p) {
        return MERR(MESTATE);
    }
    fftwf_destroy_plan(lo->p);
    lo->p = NULL;
#endif
    lo->spsc.buf = NULL;
    lo->buf = NULL;
    lo->exec_pending = 0U;
    return MOK;
}

int fft_exec(struct fft *fft)
{
    if (!fft) {
        return MERR(MENULLPTR);
    }
    DECL(fft, cfg, in, out, lo);

    if (!lo->buf || !in->buf || !out->magnitude_buf ||
        lo->spsc.cap != (size_t)cfg->point_cnt * sizeof(float32_t)) {
        return MERR(MESTATE);
    }
#if defined(FFT_HAS_FFTW3)
    if (!lo->p) {
        return MERR(MESTATE);
    }
#endif
    if (!lo->exec_pending) {
        return MERR(MEAGAIN);
    }

    if (spsc_read(&lo->spsc, in->buf, lo->spsc.cap) != lo->spsc.cap) {
        return MERR(MESTATE);
    }

    const uint64_t start_ts = get_mono_ts_us();

#if defined(FFT_HAS_FFTW3)
    fft_apply_window(fft);
    fftwf_execute(lo->p);
    for (uint32_t i = 0; i < cfg->point_cnt / 2 + 1; i++) {
        out->magnitude_buf[i] =
            SQRT(lo->buf[i][0] * lo->buf[i][0] + lo->buf[i][1] * lo->buf[i][1]);
    }

    size_t max_idx = 0U;
    find_max(
        &out->magnitude_buf[1], cfg->point_cnt >> 1U, &out->max_mag, &max_idx);
    // find_max() 返回相对于 magnitude_buf[1] 的下标, 换算回完整频谱下标
    out->out_idx = (uint32_t)max_idx + 1U;
#elif defined(ARM_MATH)
    if (cfg->flag == 0U) {
        fft_apply_window(fft);
    }
    arm_rfft_fast_f32(&lo->s, in->buf, lo->buf, cfg->flag);
    if (cfg->flag == 0U) {
        const uint32_t half = cfg->point_cnt >> 1U;
        out->magnitude_buf[0] = ABS(lo->buf[0]);
        out->magnitude_buf[half] = ABS(lo->buf[1]);
        arm_cmplx_mag_f32(lo->buf + 2U, out->magnitude_buf + 1U, half - 1U);
        arm_max_f32(
            out->magnitude_buf + 1U, half, &out->max_mag, &out->out_idx);
        out->out_idx++;
    } else {
        // 逆变换输出是时域样本, 不生成频谱或峰值频率
        memset(out->magnitude_buf,
               0,
               ((size_t)cfg->point_cnt / 2U + 1U) * sizeof(float32_t));
        out->max_mag = 0.0F;
        out->out_idx = 0U;
    }
#endif

    out->ft = (float32_t)out->out_idx * cfg->fs / (float32_t)cfg->point_cnt;

    lo->exec_pending = 0;
    lo->exec_us = (uint32_t)(get_mono_ts_us() - start_ts);
    return MOK;
}

int fft_exec_in(struct fft *fft, const float32_t val)
{
    if (!fft) {
        return MERR(MENULLPTR);
    }
    DECL(fft, lo);

    if (!lo->buf || !lo->spsc.buf || lo->spsc.cap == 0U) {
        return MERR(MESTATE);
    }
    if (!is_f32_finite_rt(val)) {
        return MERR(MEINVAL);
    }
    if (lo->exec_pending) {
        return MERR(MEBUSY);
    }
    if (spsc_write(&lo->spsc, &val, sizeof(val)) != sizeof(val)) {
        return MERR(MEOVERFLOW);
    }
    if (is_spsc_full(&lo->spsc)) {
        lo->exec_pending = 1;
    }
    return MOK;
}

#if defined(FFT_HAS_FFTW3) || defined(ARM_MATH)
static void fft_apply_window(struct fft *fft)
{
    DECL(fft, cfg, in);

    if (cfg->window == FFT_WINDOW_NONE) {
        return;
    }
    for (uint32_t i = 0U; i < cfg->point_cnt; i++) {
        const float32_t phase =
            TAU * (float32_t)i / (float32_t)(cfg->point_cnt - 1U);
        float32_t w = 1.0F;
        switch (cfg->window) {
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
        default: {
            break;
        }
        }
        in->buf[i] *= w;
    }
}
#endif
