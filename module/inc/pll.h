#ifndef PLL_H
#define PLL_H

#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef struct pll_cfg {
        f32 fs;
        f32 wc, damp;
        f32 lpf_wc;
} pll_cfg_t;

typedef struct pll_in {
        f32_ab_t ab;
        f32      theta;
} pll_in_t;

typedef struct pll_out {
        f32 theta;
        f32 omega, lpf_omega;
} pll_out_t;

typedef struct pll_lo {
        pll_cfg_t cfg;

        f32 ffd_lpf_wc;
        f32 kp, ki;

        f32 ki_out;
        f32 prev_theta;
        f32 theta_err;
        f32 ffd_omega, lpf_ffd_omega;
} pll_lo_t;

typedef struct pll_filter {
        pll_cfg_t cfg;
        pll_in_t  in;
        pll_out_t out;
        pll_lo_t  lo;
} pll_filter_t;

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief PLL 结构体初始化
 *
 * @param pll     PLL 结构体
 * @param pll_cfg PLL 配置
 * @return        void
 */
void pll_init(pll_filter_t *pll, pll_cfg_t pll_cfg);

/**
 * @brief PLL 单次执行计算
 *
 * @param pll PLL 结构体
 * @return    void
 */
void pll_exec(pll_filter_t *pll);

/**
 * @brief PLL 单次执行计算(带输入)
 *
 * @param pll PLL 结构体
 * @param ab  正交信号
 * @return    void
 */
void pll_exec_ab_in(pll_filter_t *pll, f32_ab_t ab);

/**
 * @brief PLL 单次执行计算(带输入)
 *
 * @param pll       PLL 结构体
 * @param theta_err 角度误差
 * @return          void
 */
void pll_exec_theta_err_in(pll_filter_t *pll, f32 theta_err);

/**
 * @brief PLL 单次执行计算(带输入)
 *
 * @param pll       PLL 结构体
 * @param theta     角度
 * @return          void
 */
void pll_exec_theta_in(pll_filter_t *pll, f32 theta);

#ifdef __cplusplus
}
#endif

#endif // !PLL_H
