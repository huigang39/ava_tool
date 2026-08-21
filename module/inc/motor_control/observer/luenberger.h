#ifndef LUENBERGER_H
#define LUENBERGER_H

#include "../pll.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

struct luenberger_cfg {
    float32_t fs;
    float32_t wc;
    float32_t damp;

    struct motor_cfg *motor;
};

struct luenberger_in {
    float32_t theta;
    float32_t elec_tor;
};

struct luenberger_out {
    float32_t est_theta, est_omega;
    float32_t est_load_tor;
    float32_t sum_tor;
};

struct luenberger_lo {
    float32_t g1;
    float32_t kp, ki;

    float32_t theta_err, mech_theta_err;
    float32_t ki_out;
    float32_t est_omega;
};

struct luenberger_tmp {
    uint8_t is_reset;
};

struct luenberger_obs {
    struct luenberger_cfg cfg;
    struct luenberger_in  in;
    struct luenberger_out out;
    struct luenberger_lo  lo;
    struct luenberger_tmp tmp;
};

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 龙伯格观测器结构体初始化
 *
 * @param luenberger     龙伯格观测器结构体
 * @param luenberger_cfg 龙伯格观测器配置
 * @return               void
 */
void luenberger_init(struct luenberger_obs *luenberger, struct luenberger_cfg luenberger_cfg);

/**
 * @brief 龙伯格观测器单次执行计算
 *
 * @param luenberger 龙伯格观测器结构体
 * @return           void
 */
void luenberger_exec_rt(struct luenberger_obs *luenberger);

/**
 * @brief 龙伯格观测器单次执行计算(带输入)
 *
 * @param luenberger 龙伯格观测器结构体
 * @param theta      电气角度
 * @param elec_tor   电磁转矩
 * @return           void
 */
void luenberger_exec_in_rt(struct luenberger_obs *luenberger, float32_t theta, float32_t elec_tor);

#ifdef __cplusplus
}
#endif

#endif // !LUENBERGER_H
