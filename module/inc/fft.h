#ifndef FFT_H
#define FFT_H

#include "platdef.h"

#if OS(HOSTED)
#include "fftw3.h"
#define FFT_HAS_FFTW3 1
#elif defined(ARM_MATH)
#include "arm_const_structs.h"
#include "arm_math.h"
#endif

#include "mathdef.h"
#include "spsc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

enum fft_len {
    FFT_POINTS_32      = 32,
    FFT_POINTS_64      = 64,
    FFT_POINTS_128     = 128,
    FFT_POINTS_256     = 256,
    FFT_POINTS_512     = 512,
    FFT_POINTS_1024    = 1024,
    FFT_POINTS_2048    = 2048,
    FFT_POINTS_4096    = 4096,
    FFT_POINTS_8192    = 8192,
    FFT_POINTS_16384   = 16384,
    FFT_POINTS_32768   = 32768,
    FFT_POINTS_65536   = 65536,
    FFT_POINTS_131072  = 131072,
    FFT_POINTS_262144  = 262144,
    FFT_POINTS_524288  = 524288,
    FFT_POINTS_1048576 = 1048576,
};

enum fft_window {
    FFT_WINDOW_NONE,
    FFT_WINDOW_HANNING,
    FFT_WINDOW_HAMMING,
    FFT_WINDOW_BLACKMAN,
};

struct fft_cfg {
    float32_t       fs;
    uint8_t         flag;
    enum fft_window e_window;
    size_t          npoints;
    float32_t      *buf;
    float32_t      *in_buf;
#if defined(FFT_HAS_FFTW3)
    fftwf_complex *out_buf;
#else
    float32_t *out_buf;
#endif
    float32_t *mag_buf;
};

struct fft_in {
    float32_t *buf;
};

struct fft_out {
    float32_t  fr;
    float32_t  ft;
    size_t     out_idx;
    float32_t *mag_buf;
    float32_t  max_mag;
};

struct fft_lo {
    uint32_t    elapsed_us;
    struct spsc spsc;
    uint8_t     need_exec;
#if defined(FFT_HAS_FFTW3)
    fftwf_plan     p;
    fftwf_complex *buf;
#elif defined(ARM_MATH)
    arm_rfft_fast_instance_f32 s;
    float32_t                 *buf;
#else
    float32_t *buf;
#endif
};

struct fft {
    struct fft_cfg cfg;
    struct fft_in  in;
    struct fft_out out;
    struct fft_lo  lo;
};

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief FFT 结构体初始化
 *
 * @param fft     FFT 结构体
 * @param fft_cfg FFT 配置
 */
void fft_init(struct fft *fft, struct fft_cfg fft_cfg);

/**
 * @brief FFT 结构体中的 fftwf_plan 类型变量销毁(仅 linux/win 平台)
 *
 * @param fft FFT 结构体
 * @return    void
 */
void fft_destroy(struct fft *fft);

/**
 * @brief FFT 单次执行计算
 *
 * @param fft FFT 结构体
 * @return    void
 */
void fft_exec(struct fft *fft);

/**
 * @brief FFT 单次执行计算(带输入)
 *
 * @param fft FFT 结构体
 * @param val 待计算的数据
 * @return    void
 */
void fft_exec_in(struct fft *fft, float32_t val);

#ifdef __cplusplus
}
#endif

#endif // !FFT_H
