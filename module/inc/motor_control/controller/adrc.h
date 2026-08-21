#ifndef ADRC_H
#define ADRC_H
#include <stddef.h>
#include <stdint.h>

#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

struct adrc_cfg {
    float32_t fs;            // 采样频率 (Hz)
    float32_t b0;            // 被控对象输入增益估计
    float32_t wc;            // 控制器带宽 (rad/s)
    float32_t wo;            // LESO 带宽 (rad/s)
    float32_t out_min;       // 输出下限
    float32_t out_max;       // 输出上限
    float32_t ref_rate_max;  // 参考值最大变化率 (单位/s，0 表示不限速)
    float32_t d_lpf_wc;      // 扰动估计低通截止频率 (rad/s，0 表示不滤波)
    float32_t d_comp_max;    // 扰动补偿输出绝对值上限 (0 表示不限幅)
    float32_t out_rate_max;  // 最终输出最大变化率 (单位/s，0 表示不限速)
    float32_t d_comp_gain;   // 外部输出限幅的扰动补偿比例系数
    float32_t out_rate_gain; // 外部输出限幅的输出变化率比例系数 (1/s)
};

struct adrc_in {
    float32_t ref;
    float32_t fdb;
    float32_t ffd;
};

struct adrc_out {
    float32_t u0;     // 状态误差反馈输出
    float32_t d_comp; // 限幅后的扰动补偿输出
    float32_t u_raw;  // 扰动补偿、前馈叠加后的未限幅输出
    float32_t u;      // 最终限幅输出
};

struct adrc_lo {
    float32_t x_hat;     // 被控状态估计
    float32_t d_hat_raw; // LESO 原始总扰动估计
    float32_t d_hat;     // 低通后的总扰动估计
    float32_t err;       // LESO 观测误差 x_hat-fdb
    float32_t beta1;     // LESO 一阶增益 2*wo
    float32_t beta2;     // LESO 二阶增益 wo^2
    float32_t kp;        // 状态误差反馈增益 wc
    float32_t inv_fs;
    float32_t inv_b0;
    float32_t plant_u; // 上一拍实际施加的 ADRC 分量（总输出减前馈）
    float32_t prev_u;
    float32_t ref_change;
    float32_t ref_limited;
    float32_t prev_ref;
    uint8_t   initialized;
};

struct adrc_ctl {
    struct adrc_cfg cfg;
    struct adrc_in  in;
    struct adrc_out out;
    struct adrc_lo  lo;
};

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief adrc 结构体初始化
 *
 * @param adrc     adrc 结构体
 * @param adrc_cfg adrc 配置
 * @return         void
 */
void adrc_init(struct adrc_ctl *adrc, struct adrc_cfg adrc_cfg);

/** 清除 ADRC 运行状态，保留配置和离散化参数。 */
void adrc_reset(struct adrc_ctl *adrc);

/** 设置控制器最终输出上下限。 */
void adrc_set_out_limit(struct adrc_ctl *adrc, float32_t out_max, float32_t out_min);

/**
 * @brief adrc 运算
 *
 * @param adrc adrc 结构体
 * @return     void
 */
void adrc_exec_rt(struct adrc_ctl *adrc);

/**
 * @brief adrc 运算(带输入)
 *
 * @param adrc adrc 结构体
 * @param ref  参考值
 * @param fdb  反馈值
 * @param ffd  前馈值
 * @return     void
 */
void adrc_exec_in_rt(struct adrc_ctl *adrc, float32_t ref, float32_t fdb, float32_t ffd);

#ifdef __cplusplus
}
#endif

#endif // !ADRC_H
