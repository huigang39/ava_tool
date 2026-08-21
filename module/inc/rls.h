#ifndef RLS_H
#define RLS_H
#include <stddef.h>
#include <stdint.h>

#include "macrodef.h"

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

struct rlf_cfg {
    uint32_t  order;
    float32_t lambda;
    float32_t delta;
};

struct rls_in {
    float32_t y;
    float32_t x[MAX_ORDER];
};

struct rlf_out {
    float32_t w[MAX_ORDER];
};

struct rls_lo {
    float32_t err;
    float32_t denom;
    float32_t y_hat;
    float32_t p[MAX_ORDER][MAX_ORDER];
    float32_t px[MAX_ORDER];
    float32_t k[MAX_ORDER];
    float32_t temp[MAX_ORDER][MAX_ORDER];
    float32_t xtp[MAX_ORDER];
};

struct rls_obs {
    struct rlf_cfg cfg;
    struct rls_in  in;
    struct rlf_out out;
    struct rls_lo  lo;
};

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void rls_init(struct rls_obs *rls, struct rlf_cfg rls_cfg);
void rls_reset(struct rls_obs *rls);
void rls_exec(struct rls_obs *rls);
void rls_exec_in(struct rls_obs *rls, float32_t ref, const float32_t *x);

#ifdef __cplusplus
}
#endif

#endif // !RLS_H
