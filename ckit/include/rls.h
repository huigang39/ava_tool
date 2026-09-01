#ifndef RLS_H
#define RLS_H
#include <stddef.h>
#include <stdint.h>

#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_ORDER (4)

struct rlf_cfg {
    // 模型阶数
    uint32_t order; // 算法阶数

    // 递推与初始协方差参数
    float32_t lambda; // 遗忘因子
    float32_t delta;  // 初始协方差系数
};

struct rls_in {
    float32_t y;            // 算法输出
    float32_t x[MAX_ORDER]; // 算法输入
};

struct rlf_out {
    float32_t w[MAX_ORDER]; // 当前估算的参数向量
};

struct rls_lo {
    // 生效配置
    struct rlf_cfg eff_cfg; // 校正后的生效配置

    // 误差与预测结果
    float32_t err;         // 当前预测误差
    float32_t denominator; // 增益计算分母
    float32_t y_hat;       // 当前预测输出

    // 协方差与递推工作区
    float32_t p[MAX_ORDER][MAX_ORDER];
    float32_t px[MAX_ORDER]; // 协方差矩阵与输入向量的乘积
    float32_t k[MAX_ORDER];  // 当前更新增益向量
    float32_t gain_covariance_product[MAX_ORDER][MAX_ORDER];
    float32_t xtp[MAX_ORDER]; // 输入转置与协方差矩阵的乘积
};

struct rls_obs {
    struct rlf_cfg cfg; // 配置参数
    struct rls_in in;   // 输入数据
    struct rlf_out out; // 输出结果
    struct rls_lo lo;   // 运行状态
};

/**
 * @brief                 初始化递推最小二乘估计器
 * @param[in,out] rls     有效的估计器对象
 * @param[in]     rls_cfg 阶数, 遗忘因子与初始协方差配置
 */
void rls_init(struct rls_obs *rls, struct rlf_cfg rls_cfg);

/**
 * @brief             按生效配置复位估计器历史状态
 * @param[in,out] rls 已初始化的估计器对象
 */
void rls_reset(struct rls_obs *rls);

/**
 * @brief             根据当前输入执行一次参数估计
 * @param[in,out] rls 已初始化的估计器对象
 */
void rls_exec(struct rls_obs *rls);

/**
 * @brief             写入观测值和回归向量并执行估计
 * @param[in,out] rls 已初始化的估计器对象
 * @param[in]     ref 观测值
 * @param[in]     x   回归向量, 长度至少为生效阶数
 */
void rls_exec_in(struct rls_obs *rls, float32_t ref, const float32_t *x);

#ifdef __cplusplus
}
#endif

#endif
