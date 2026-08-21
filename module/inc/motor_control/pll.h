#ifndef PLL_H
#define PLL_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

struct pll_cfg {
    float32_t fs;
    float32_t wc, damp;
    float32_t lpf_wc;
};

struct pll_in {
    struct f32_ab ab;
    float32_t     theta;
};

struct pll_out {
    float32_t theta;
    float32_t omega, lpf_omega;
};

struct pll_lo {
    float32_t ffd_lpf_wc;
    float32_t kp, ki;

    float32_t ki_out;
    float32_t prev_theta;
    float32_t theta_err;
    float32_t ffd_omega, lpf_ffd_omega;
};

struct pll_tmp {
    float32_t fs;
    float32_t inv_fs;
    float32_t ki_ts;
    float32_t lpf_alpha;
    float32_t lpf_beta;
    float32_t ffd_lpf_alpha;
    float32_t ffd_lpf_beta;
};

struct pll_filter {
    struct pll_cfg cfg;
    struct pll_in  in;
    struct pll_out out;
    struct pll_lo  lo;
    struct pll_tmp tmp;
};

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
void pll_init(struct pll_filter *pll, struct pll_cfg pll_cfg);

/**
 * @brief PLL 单次执行计算
 *
 * @param pll PLL 结构体
 * @return    void
 */
void pll_exec_rt(struct pll_filter *pll);

/**
 * @brief PLL 单次执行计算(带输入)
 *
 * @param pll PLL 结构体
 * @param ab  正交信号
 * @return    void
 */
void pll_exec_ab_in_rt(struct pll_filter *pll, struct f32_ab ab);

/**
 * @brief PLL 单次执行计算(带输入)
 *
 * @param pll       PLL 结构体
 * @param theta_err 角度误差
 * @return          void
 */
void pll_exec_theta_err_in_rt(struct pll_filter *pll, float32_t theta_err);

/**
 * @brief PLL 单次执行计算(带输入)
 *
 * @param pll       PLL 结构体
 * @param theta     角度
 * @return          void
 */
void pll_exec_theta_in_rt(struct pll_filter *pll, float32_t theta);

#ifdef __cplusplus
}
#endif

#endif // !PLL_H
