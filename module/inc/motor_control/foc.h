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
void foc_init(struct foc *foc, struct foc_cfg foc_cfg);

/**
 * @brief FOC 单次执行计算
 *
 * @param foc FOC 结构体
 * @return    void
 */
void foc_exec_rt(struct foc *foc);

/**
 * @brief FOC 校准
 *
 * @param foc FOC 结构体
 * @return    int32_t
 */
int foc_cali(struct foc *foc);

/**
 * @brief 判断 FOC 是否准备好
 *
 * @param foc FOC 结构体
 * @return    uint32_t
 */
uint32_t foc_is_ready(const struct foc *foc);

int                   foc_set_state(struct foc *foc, const enum foc_update_state e_state);
enum foc_update_state foc_get_state(const struct foc *foc);

int           foc_set_mode(struct foc *foc, const enum foc_mode e_mode);
enum foc_mode foc_get_mode(const struct foc *foc);

int            foc_set_elec_theta(struct foc *foc, const enum foc_theta e_elec_theta);
enum foc_theta foc_get_elec_theta(const struct foc *foc);

int foc_nonlinear_elec_theta_cali(struct foc *foc);

struct foc_temp foc_get_temp(const struct foc *foc);
float32_t       foc_get_vbus(const struct foc *foc);

struct foc_store foc_get_store(const struct foc *foc);

struct foc_ref_pvct foc_get_ref_pvct(struct foc *foc);

int foc_set_outshaft_zero(struct foc *foc);
int foc_set_tor_zero(struct foc *foc);

/**
 * @brief 设置 FOC 校准标志
 *
 * @param foc FOC 结构体
 * @return    void
 */
void foc_set_cali_flag(struct foc *foc);

/**
 * @brief 设置目标 PVCT
 *
 * @param foc      FOC 结构体
 * @param ref_pvct 目标 PVCT(位置,速度,电流,力矩)
 * @return         void
 */
void foc_set_pvct(struct foc *foc, struct foc_ref_pvct ref_pvct);

/**
 * @brief 获取反馈 PVCT
 *
 * @param foc FOC 结构体
 * @return    struct foc_fdb_pvct
 */
struct foc_fdb_pvct foc_get_fdb_pvct(struct foc *foc);

#ifdef __cplusplus
}
#endif

#endif // !FOC_H
