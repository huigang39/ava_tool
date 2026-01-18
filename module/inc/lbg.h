#ifndef LBG_H
#define LBG_H

#include "pll.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef struct lbg_cfg {
        f32 fs;
        f32 wc;
        f32 damp;

        motor_cfg_t motor;
} lbg_cfg_t;

typedef struct lbg_in {
        f32 theta;
        f32 elec_tor;
} lbg_in_t;

typedef struct lbg_out {
        f32 est_theta, est_omega;
        f32 est_load_tor;
        f32 sum_tor;
} lbg_out_t;

typedef struct lbg_lo {
        lbg_cfg_t cfg;
        u8        reset_flag;

        f32 g1;
        f32 kp, ki;

        f32 theta_err, mech_theta_err;
        f32 ki_out;
        f32 est_omega;
} lbg_lo_t;

typedef struct lbg_obs {
        lbg_cfg_t cfg;
        lbg_in_t  in;
        lbg_out_t out;
        lbg_lo_t  lo;
} lbg_obs_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

void lbg_init(lbg_obs_t *lbg, lbg_cfg_t lbg_cfg);
void lbg_exec(lbg_obs_t *lbg);
void lbg_exec_in(lbg_obs_t *lbg, f32 theta, f32 elec_tor);

#ifdef __cplusplus
}
#endif

#endif // !LBG_H
