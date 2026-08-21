#ifndef FIR_H
#define FIR_H
#include <stddef.h>
#include <stdint.h>

#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

enum fir_order {
    FIR_1 = 1,
    FIR_2 = 2,
    FIR_3 = 3,
};

enum fir_type {
    FIR_LOWPASS,
    FIR_HIGHPASS,
    FIR_BANDPASS,
};

struct fir_cfg {
    float32_t      fs;
    float32_t      fh, fl;
    enum fir_order order;
    enum fir_type  type;
};

struct fir_in {
    float32_t x;
};

struct fir_out {
    float32_t y;
};

struct fir_lo {
    float32_t fc;
    float32_t w0, k;
    float32_t b0, b1, b2, b3;
    float32_t x1, x2, x3;
};

struct fir_filter {
    struct fir_cfg cfg;
    struct fir_in  in;
    struct fir_out out;
    struct fir_lo  lo;
};

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief FIR 滤波器结构体初始化
 *
 * @param fir     FIR 滤波器结构体
 * @param fir_cfg FIR 滤波器配置
 * @return        int 状态码
 */
int fir_init(struct fir_filter *fir, struct fir_cfg fir_cfg);

/**
 * @brief FIR 滤波器单次执行器计算
 *
 * @param fir FIR 滤波器结构体
 * @return    void
 */
void fir_exec(struct fir_filter *fir);

/**
 * @brief FIR 滤波器单次执行器计算(带输入)
 *
 * @param fir FIR 滤波器结构体
 * @param x   待计算的数据
 * @return    void
 */
void fir_exec_in(struct fir_filter *fir, float32_t x);

#ifdef __cplusplus
}
#endif

#endif // !FIR_H
