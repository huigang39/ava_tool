#ifndef IIR_H
#define IIR_H
#include <stddef.h>
#include <stdint.h>

#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

enum iir_order {
    IIR_1 = 1,
    IIR_2 = 2,
};

enum iir_type {
    IIR_LOWPASS,
    IIR_HIGHPASS,
    IIR_BANDPASS,
};

struct iif_cfg {
    float32_t      fs;
    float32_t      fh, fl;
    enum iir_order order;
    enum iir_type  type;
};

struct iir_in {
    float32_t x;
};

struct iir_out {
    float32_t y;
};

struct iir_lo {
    float32_t fc;
    float32_t rc;
    float32_t alpha;
    float32_t w0, q;
    float32_t x1, x2, y1, y2;
    float32_t b0, b1, b2;
    float32_t a0, a1, a2;
    float32_t norm_a0, norm_a1, norm_a2, norm_a3, norm_a4;
};

struct iir_filter {
    struct iif_cfg cfg;
    struct iir_in  in;
    struct iir_out out;
    struct iir_lo  lo;
};

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief IIR 滤波器结构体初始化
 *
 * @param iir     IIR 滤波器结构体
 * @param iir_cfg IIR 滤波器配置
 * @return        int 状态码
 */
int iir_init(struct iir_filter *iir, struct iif_cfg iir_cfg);

/**
 * @brief IIR 滤波器单次执行器计算
 *
 * @param iir IIR 滤波器结构体
 * @return    void
 */
void iir_exec(struct iir_filter *iir);

/**
 * @brief IIR 滤波器单次执行器计算(带输入)
 *
 * @param iir IIR 滤波器结构体
 * @param x   待计算的数据
 * @return    void
 */
void iir_exec_in(struct iir_filter *iir, float32_t x);

#ifdef __cplusplus
}
#endif

#endif // !IIR_H
