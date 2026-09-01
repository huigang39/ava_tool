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

enum fft_len {
    FFT_POINTS_32 = 32,           // 32 点 FFT
    FFT_POINTS_64 = 64,           // 64 点 FFT
    FFT_POINTS_128 = 128,         // 128 点 FFT
    FFT_POINTS_256 = 256,         // 256 点 FFT
    FFT_POINTS_512 = 512,         // 512 点 FFT
    FFT_POINTS_1024 = 1024,       // 1024 点 FFT
    FFT_POINTS_2048 = 2048,       // 2048 点 FFT
    FFT_POINTS_4096 = 4096,       // 4096 点 FFT
    FFT_POINTS_8192 = 8192,       // 8192 点 FFT
    FFT_POINTS_16384 = 16384,     // 16384 点 FFT
    FFT_POINTS_32768 = 32768,     // 32768 点 FFT
    FFT_POINTS_65536 = 65536,     // 65536 点 FFT
    FFT_POINTS_131072 = 131072,   // 131072 点 FFT
    FFT_POINTS_262144 = 262144,   // 262144 点 FFT
    FFT_POINTS_524288 = 524288,   // 524288 点 FFT
    FFT_POINTS_1048576 = 1048576, // 1048576 点 FFT
};

enum fft_window {
    FFT_WINDOW_NONE,     // 不加窗
    FFT_WINDOW_HANNING,  // Hann 窗
    FFT_WINDOW_HAMMING,  // Hamming 窗
    FFT_WINDOW_BLACKMAN, // Blackman 窗
};

struct fft_cfg {
    // 采样与变换配置
    float32_t fs;           // 采样频率, 单位: Hz
    uint8_t flag;           // 0 为正变换, 1 为 CMSIS 逆变换; FFTW 只支持正变换
    enum fft_window window; // FFT 窗函数
    uint32_t point_cnt;     // 变换点数 N, 必须为 2 的幂; CMSIS 支持 32 至 4096

    // 输入输出及工作缓冲区
    float32_t *buf;    // 采样队列存储, 容量为 N 个 float32_t
    float32_t *in_buf; // 变换输入, 容量为 N 个 float32_t
#if defined(FFT_HAS_FFTW3)
    fftwf_complex *out_buf; // 频谱输出, 容量为 N / 2 + 1 个复数
#else
    float32_t *out_buf; // CMSIS 输出, 容量为 N 个 float32_t
#endif
    float32_t *
        magnitude_buf; // 频谱幅值, 容量为 N / 2 + 1 个 float32_t, 含 DC 和 Nyquist
};

struct fft_in {
    float32_t *buf; // 算法或通信存储缓冲区
};

struct fft_out {
    // 频率分辨率与周期
    float32_t fr; // 频率分辨率, 单位: Hz
    float32_t ft; // 变换结果频率, 单位: Hz

    // 频谱输出
    uint32_t out_idx;         // 频谱输出索引
    float32_t *magnitude_buf; // 频谱幅值缓冲区
    float32_t max_mag;        // 最大频谱幅值
};

struct fft_lo {
    // 执行时序
    uint32_t exec_us; // 最近变换耗时, 单位: us

    // 采样队列与执行状态
    struct spsc spsc;     // 共享通信队列
    uint8_t exec_pending; // 已有完整数据等待变换
#if defined(FFT_HAS_FFTW3)
    fftwf_plan p;
    fftwf_complex *buf; // 算法或通信存储缓冲区
#elif defined(ARM_MATH)
    arm_rfft_fast_instance_f32 s;
    float32_t *buf; // 算法或通信存储缓冲区
#else
    float32_t *buf; // 算法或通信存储缓冲区
#endif
};

struct fft {
    struct fft_cfg cfg; // 配置参数
    struct fft_in in;   // 输入数据
    struct fft_out out; // 输出结果
    struct fft_lo lo;   // 运行状态
};

/**
 * @brief                 FFT 结构体初始化
 * @param[in,out] fft     FFT 结构体
 * @param[in]     fft_cfg FFT 配置
 * @return                成功返回 0, 失败返回负错误码
 */
int fft_init(struct fft *fft, struct fft_cfg fft_cfg);

/**
 * @brief             释放变换计划并停止采样
 * @param[in,out] fft FFT 结构体
 * @return            成功返回 0, 失败返回负错误码
 */
int fft_destroy(struct fft *fft);

/**
 * @brief             变换一组已采集的数据
 * @param[in,out] fft FFT 结构体
 * @return            成功返回 0, 数据未满返回 -MEAGAIN, 失败返回负错误码
 * @note              正变换峰值搜索跳过 DC, 包含 Nyquist; CMSIS 逆变换保留时域输出, 频谱和峰值清零
 */
int fft_exec(struct fft *fft);

/**
 * @brief             追加一个输入样本, 数据满后等待 fft_exec
 * @param[in,out] fft FFT 结构体
 * @param[in]     val 待计算的数据
 * @return            成功返回 0, 等待变换时返回 -MEBUSY, 失败返回负错误码
 */
int fft_exec_in(struct fft *fft, float32_t val);

#ifdef __cplusplus
}
#endif

#endif
