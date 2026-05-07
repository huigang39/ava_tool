#ifndef RLS_H
#define RLS_H

#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  宏/表定义                                 */
/* -------------------------------------------------------------------------- */

#define MAX_ORDER (4)

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef struct rlf_cfg {
        u32 order;
        f32 lambda;
        f32 delta;
} rls_cfg_t;

typedef struct rls_in {
        f32 y;
        f32 x[MAX_ORDER];
} rls_in_t;

typedef struct rlf_out {
        f32 w[MAX_ORDER];
} rls_out_t;

typedef struct rls_lo {
        f32 err;
        f32 denom;
        f32 y_hat;
        f32 p[MAX_ORDER][MAX_ORDER];
        f32 px[MAX_ORDER];
        f32 k[MAX_ORDER];
        f32 temp[MAX_ORDER][MAX_ORDER];
        f32 xtp[MAX_ORDER];
} rls_lo_t;

typedef struct rls_obs {
        rls_cfg_t cfg;
        rls_in_t  in;
        rls_out_t out;
        rls_lo_t  lo;
} rls_obs_t;

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void rls_init(rls_obs_t *rls, rls_cfg_t rls_cfg);
void rls_exec(rls_obs_t *rls);
void rls_exec_in(rls_obs_t *rls, f32 ref, const f32 *x);

#ifdef __cplusplus
}
#endif

#endif // !RLS_H
