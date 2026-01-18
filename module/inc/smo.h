#ifndef SMO_H
#define SMO_H

#include "pll.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef struct smo_cfg {
        f32 fs;
        f32 ks;
        f32 es0;

        motor_cfg_t motor;
} smo_cfg_t;

typedef struct smo_in {
        f32_ab_t i_ab, v_ab;
} smo_in_t;

typedef struct smo_out {
        f32 est_theta;
        f32 est_omega;
} smo_out_t;

typedef struct smo_lo {
        f32_ab_t     est_i_ab;
        f32_ab_t     est_i_ab_err;
        f32_ab_t     est_emf_v_ab;
        pll_filter_t pll;
} smo_lo_t;

typedef struct smo_obs {
        smo_cfg_t cfg;
        smo_in_t  in;
        smo_out_t out;
        smo_lo_t  lo;
} smo_obs_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

void smo_init(smo_obs_t *smo, smo_cfg_t smo_cfg);
void smo_exec(smo_obs_t *smo);
void smo_exec_in(smo_obs_t *smo, f32_ab_t i_ab, f32_ab_t v_ab);

#ifdef __cplusplus
}
#endif

#endif // !SMO_H
