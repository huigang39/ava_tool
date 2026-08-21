#ifndef SMO_H
#define SMO_H

#include "../pll.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

struct smo_cfg {
    float32_t fs;
    float32_t ks;
    float32_t es0;
    float32_t sample_volt_max_omega; // 全部使用端电压采样的最高电角速度 (rad/s)
    float32_t out_volt_min_omega;    // 全部使用输出电压的最低电角速度 (rad/s)

    struct pll_cfg pll;

    struct motor_cfg *motor;
};

struct smo_in {
    struct f32_ab i_ab, v_ab;
};

struct smo_out {
    float32_t est_theta;
    float32_t est_omega;
};

struct smo_lo {
    struct f32_ab est_i_ab;
    struct f32_ab est_i_ab_err;
    struct f32_ab est_emf_v_ab;

    struct pll_filter pll;
};

struct smo_obs {
    struct smo_cfg cfg;
    struct smo_in  in;
    struct smo_out out;
    struct smo_lo  lo;
};

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 滑膜观测器结构体初始化
 *
 * @param smo     滑膜观测器结构体
 * @param smo_cfg 滑膜观测器配置
 * @return        void
 */
void smo_init(struct smo_obs *smo, struct smo_cfg smo_cfg);

/**
 * @brief 滑膜观测器单次执行计算
 *
 * @param smo 滑膜观测器结构体
 * @return    void
 */
void smo_exec_rt(struct smo_obs *smo);

/**
 * @brief 滑膜观测器单次执行计算(带输入)
 *
 * @param smo  滑膜观测器结构体
 * @param i_ab alpha-beta 轴电流
 * @param v_ab alpha-beta 轴电压
 * @return     void
 */
void smo_exec_in_rt(struct smo_obs *smo, struct f32_ab i_ab, struct f32_ab v_ab);

#ifdef __cplusplus
}
#endif

#endif // !SMO_H
