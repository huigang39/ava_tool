#ifndef FFT_H
#define FFT_H

#if defined(__linux__) || defined(_WIN32)
#include "fftw3.h"
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

typedef enum fft_size {
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
} fft_size_e;

typedef struct fft_cfg {
        f32   fs;
        u8    flag;
        usize npoints;
        f32  *buf;
        f32  *in_buf;
#if defined(__linux__) || defined(_WIN32)
        fftwf_complex *out_buf;
#else
        f32 *out_buf;
#endif
        f32 *mag_buf;
} fft_cfg_t;

typedef struct fft_in {
        f32 *buf;
} fft_in_t;

typedef struct fft_out {
        f32   fr;
        f32   ft;
        usize out_idx;
        f32  *mag_buf;
        f32   max_mag;
} fft_out_t;

typedef struct fft_lo {
        u32    elapsed_us;
        spsc_t spsc;
        u8     need_exec;
#if defined(__linux__) || defined(_WIN32)
        fftwf_plan     p;
        fftwf_complex *buf;
#elif defined(ARM_MATH)
        arm_rfft_fast_instance_f32 s;
        f32                       *buf;
#else
        f32 *buf;
#endif
} fft_lo_t;

typedef struct fft {
        fft_cfg_t cfg;
        fft_in_t  in;
        fft_out_t out;
        fft_lo_t  lo;
} fft_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

void fft_init(fft_t *fft, fft_cfg_t fft_cfg);
void fft_destroy(fft_t *fft);
void fft_exec(fft_t *fft);
void fft_exec_in(fft_t *fft, f32 val);

#ifdef __cplusplus
}
#endif

#endif // !FFT_H
