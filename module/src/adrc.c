#include "adrc.h"

/* -------------------------------------------------------------------------- */
/*                                  内部函数                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief adrc 扰动估测器运算
 *
 * @param adrc adrc 结构体
 * @return     NULL
 */
static void
adrc_eso_update(adrc_ctl_t *adrc)
{
        // 更新状态估计（基于当前控制输入和扰动估计）
        adrc->lo.x_hat += adrc->cfg.fs * (adrc->out.u - adrc->cfg.k1 * (adrc->in.fdb - adrc->lo.x_hat));

        // 更新扰动估计（基于输出误差）
        adrc->lo.d_hat += adrc->cfg.fs * (-adrc->cfg.gain * (adrc->in.fdb - adrc->lo.x_hat));
}

/**
 * @brief adrc 控制率运算
 *
 * @param adrc adrc 结构体
 * @return     NULL
 */
static void
adrc_ctl_update(adrc_ctl_t *adrc)
{
        // 计算状态误差和输出误差
        const f32 state_err  = adrc->in.fdb - adrc->lo.x_hat;
        const f32 output_err = adrc->in.ref - adrc->in.fdb;

        // 基于扰动估计的控制律
        adrc->out.u = -adrc->cfg.k1 * state_err - adrc->cfg.k2 * output_err - adrc->lo.d_hat;
}

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
adrc_init(adrc_ctl_t *adrc, const adrc_cfg_t adrc_cfg)
{
        CFG_INIT(adrc, adrc_cfg);
}

void
adrc_exec(adrc_ctl_t *adrc)
{
        adrc_eso_update(adrc);
        adrc_ctl_update(adrc);
}

void
adrc_exec_in(adrc_ctl_t *adrc, const f32 ref, const f32 fdb)
{
        DECL(adrc, in);

        in->ref = ref;
        in->fdb = fdb;
        adrc_exec(adrc);
}
