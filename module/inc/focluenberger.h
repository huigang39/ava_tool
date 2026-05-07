#ifndef LUENBERGER_H
#define LUENBERGER_H

#include "pll.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef struct luenberger_cfg {
        f32 fs;
        f32 wc;
        f32 damp;

        motor_cfg_t *motor;
} luenberger_cfg_t;

typedef struct luenberger_in {
        f32 theta;
        f32 elec_tor;
} luenberger_in_t;

typedef struct luenberger_out {
        f32 est_theta, est_omega;
        f32 est_load_tor;
        f32 sum_tor;
} luenberger_out_t;

typedef struct luenberger_lo {
        luenberger_cfg_t cfg;

        f32 g1;
        f32 kp, ki;

        f32 theta_err, mech_theta_err;
        f32 ki_out;
        f32 est_omega;
} luenberger_lo_t;

typedef struct luenberger_tmp {
        u8 is_reset;
} luenberger_tmp_t;

typedef struct luenberger_obs {
        luenberger_cfg_t cfg;
        luenberger_in_t  in;
        luenberger_out_t out;
        luenberger_lo_t  lo;
        luenberger_tmp_t tmp;
} luenberger_obs_t;

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
void luenberger_init(luenberger_obs_t *luenberger, luenberger_cfg_t luenberger_cfg);

/**
 * @brief 龙伯格观测器单次执行计算
 *
 * @param luenberger 龙伯格观测器结构体
 * @return           void
 */
void luenberger_exec(luenberger_obs_t *luenberger);

/**
 * @brief 龙伯格观测器单次执行计算(带输入)
 *
 * @param luenberger 龙伯格观测器结构体
 * @param theta      电气角度
 * @param elec_tor   电磁转矩
 * @return           void
 */
void luenberger_exec_in(luenberger_obs_t *luenberger, f32 theta, f32 elec_tor);

#ifdef __cplusplus
}
#endif

#endif // !LUENBERGER_H
