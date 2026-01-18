#ifndef ADRC_H
#define ADRC_H

#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef struct {
        f32 fs;   // 采样频率
        f32 k1;   // 状态反馈增益
        f32 k2;   // 输出反馈增益
        f32 gain; // 扰动估计增益
} adrc_cfg_t;

typedef struct {
        f32 ref;
        f32 fdb;
} adrc_in_t;

typedef struct {
        f32 u; // 控制输入
} adrc_out_t;

typedef struct {
        f32 x_hat; // 状态估计
        f32 d_hat; // 扰动估计
} adrc_lo_t;

typedef struct {
        adrc_cfg_t cfg;
        adrc_in_t  in;
        adrc_out_t out;
        adrc_lo_t  lo;
} adrc_ctl_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief adrc 结构体初始化
 *
 * @param adrc     adrc 结构体
 * @param adrc_cfg adrc 配置
 * @return         NULL
 */
void adrc_init(adrc_ctl_t *adrc, adrc_cfg_t adrc_cfg);

/**
 * @brief adrc 运算
 *
 * @param adrc adrc 结构体
 * @return     NULL
 */
void adrc_exec(adrc_ctl_t *adrc);

/**
 * @brief adrc 运算(带输入)
 *
 * @param adrc adrc 结构体
 * @param ref  参考值
 * @param fdb  反馈值
 * @return     NULL
 */
void adrc_exec_in(adrc_ctl_t *adrc, f32 ref, f32 fdb);

#ifdef __cplusplus
}
#endif

#endif // !ADRC_H
