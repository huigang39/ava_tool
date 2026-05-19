#ifndef FOC_H
#define FOC_H

#include "focdef.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FOC 结构体变量初始化
 *
 * @param foc     FOC 结构体
 * @param foc_cfg FOC 配置
 * @return        void
 */
void foc_init(foc_t *foc, foc_cfg_t foc_cfg);

/**
 * @brief FOC 单次执行计算
 *
 * @param foc FOC 结构体
 * @return    void
 */
void foc_exec(foc_t *foc);

/**
 * @brief FOC 校准
 *
 * @param foc FOC 结构体
 * @return    i32
 */
i32 foc_cali(foc_t *foc);

/**
 * @brief 判断 FOC 是否准备好
 *
 * @param foc FOC 结构体
 * @return    u32
 */
u32 foc_is_ready(const foc_t *foc);

i32         foc_set_state(foc_t *foc, const foc_state_e e_state);
foc_state_e foc_get_state(const foc_t *foc);

i32        foc_set_mode(foc_t *foc, const foc_mode_e e_mode);
foc_mode_e foc_get_mode(const foc_t *foc);

i32              foc_set_elec_theta(foc_t *foc, const foc_elec_theta_e e_elec_theta);
foc_elec_theta_e foc_get_elec_theta(const foc_t *foc);

i32 foc_nonlinear_elec_theta_cali(foc_t *foc);

foc_temp_t foc_get_temp(const foc_t *foc);
f32        foc_get_vbus(const foc_t *foc);

foc_store_t foc_get_store(const foc_t *foc);

foc_ref_pvct_t foc_get_ref_pvct(foc_t *foc);

i32 foc_set_outshaft_zero(foc_t *foc);
i32 foc_set_tor_zero(foc_t *foc);

/**
 * @brief 检测 foc->cfg.func_opt 自上次 apply 以来是否变化, 变化则在安全状态下
 *        (FOC_STATE_DISABLE) 自动 apply, 用于支持运行时实时改写函数选择器
 *
 * @param foc FOC 结构体
 * @return    void
 */
void foc_sync_func_opt(foc_t *foc);

/**
 * @brief 设置 FOC 校准标志
 *
 * @param foc FOC 结构体
 * @return    void
 */
void foc_set_cali_flag(foc_t *foc);

/**
 * @brief 设置目标 PVCT
 *
 * @param foc      FOC 结构体
 * @param ref_pvct 目标 PVCT(位置、速度、电流、力矩)
 * @return         void
 */
void foc_set_pvct(foc_t *foc, foc_ref_pvct_t ref_pvct);

/**
 * @brief 获取反馈 PVCT
 *
 * @param foc FOC 结构体
 * @return    foc_fdb_pvct_t
 */
foc_fdb_pvct_t foc_get_fdb_pvct(foc_t *foc);

/**
 * @brief 设置强拖速度
 *
 * @param foc       FOC 结构体
 * @param ref_vel   目标强拖速度
 * @param exec_freq 函数调用频率
 * @return          void
 */
void foc_set_force_vel_ref(foc_t *foc, f32 ref_id, f32 ref_vel, f32 exec_freq);

#ifdef __cplusplus
}
#endif

#endif // !FOC_H
