#include "rls.h"

#include "mathdef.h"

/**
 * @brief         判断数值是否为有限浮点数
 * @param[in] val 参数原始值
 */
static uint8_t is_rls_value_valid(const float32_t val);

#define RLS_DENOMINATOR_MIN (1.0e-12F)
#define RLS_VALUE_MAX (1.0e30F)

#if defined(__clang__)
#define RLS_COMPACT __attribute__((minsize))
#else
#define RLS_COMPACT
#endif

RLS_COMPACT
void rls_init(struct rls_obs *rls, const struct rlf_cfg rls_cfg)
{
    CFG_READ(rls);
    DECL(rls, lo);
    CFG_INIT(rls, rls_cfg);

    lo->eff_cfg = rls_cfg;

    if (lo->eff_cfg.order == 0) {
        lo->eff_cfg.order = 1;
    }
    if (lo->eff_cfg.order > MAX_ORDER) {
        lo->eff_cfg.order = MAX_ORDER;
    }

    if (!is_rls_value_valid(lo->eff_cfg.lambda) || lo->eff_cfg.lambda <= 0.0F ||
        lo->eff_cfg.lambda > 1.0F) {
        lo->eff_cfg.lambda = 1.0F;
    }
    if (!is_rls_value_valid(lo->eff_cfg.delta) || lo->eff_cfg.delta <= 0.0F) {
        lo->eff_cfg.delta = 1.0F;
    }

    rls_reset(rls);
}

RLS_COMPACT
void rls_reset(struct rls_obs *rls)
{
    CFG_READ(rls);
    DECL(rls, in, out, lo);

    const struct rlf_cfg eff_cfg = lo->eff_cfg;
    memset(in, 0, sizeof(*in));
    memset(out, 0, sizeof(*out));
    memset(lo, 0, sizeof(*lo));
    lo->eff_cfg = eff_cfg;
    for (uint32_t i = 0; i < MAX_ORDER; i++) {
        lo->p[i][i] = eff_cfg.delta;
    }
}

/**
 * @brief 限制矩阵循环展开, 避免超过固件 Flash 分区
 */
RLS_COMPACT
void rls_exec(struct rls_obs *rls)
{
    CFG_READ(rls);
    DECL(rls, in, out, lo);

    if (!is_rls_value_valid(in->y)) {
        return;
    }
    for (uint32_t i = 0; i < lo->eff_cfg.order; i++) {
        if (!is_rls_value_valid(in->x[i])) {
            return;
        }
    }

    lo->y_hat = 0.0F;
    for (uint32_t i = 0; i < lo->eff_cfg.order; i++) {
        lo->y_hat += out->w[i] * in->x[i];
    }

    lo->err = in->y - lo->y_hat;

    for (uint32_t i = 0; i < lo->eff_cfg.order; i++) {
        lo->px[i] = 0.0F;
        for (uint32_t j = 0; j < lo->eff_cfg.order; j++) {
            lo->px[i] += lo->p[i][j] * in->x[j];
        }
    }

    lo->denominator = lo->eff_cfg.lambda;
    for (uint32_t i = 0; i < lo->eff_cfg.order; i++) {
        lo->denominator += in->x[i] * lo->px[i];
    }

    if (!is_rls_value_valid(lo->denominator) ||
        lo->denominator <= RLS_DENOMINATOR_MIN) {
        rls_reset(rls);
        return;
    }

    for (uint32_t i = 0; i < lo->eff_cfg.order; i++) {
        lo->k[i] = lo->px[i] / lo->denominator;
    }

    for (uint32_t i = 0; i < lo->eff_cfg.order; i++) {
        out->w[i] += lo->k[i] * lo->err;
    }

    for (uint32_t j = 0; j < lo->eff_cfg.order; j++) {
        lo->xtp[j] = 0.0F;
        for (uint32_t i = 0; i < lo->eff_cfg.order; i++) {
            lo->xtp[j] += in->x[i] * lo->p[i][j];
        }
    }

    for (uint32_t i = 0; i < lo->eff_cfg.order; i++) {
        for (uint32_t j = 0; j < lo->eff_cfg.order; j++) {
            lo->gain_covariance_product[i][j] = lo->k[i] * lo->xtp[j];
            lo->p[i][j] = (lo->p[i][j] - lo->gain_covariance_product[i][j]) /
                          lo->eff_cfg.lambda;
        }
    }

    for (uint32_t i = 0; i < lo->eff_cfg.order; i++) {
        if (!is_rls_value_valid(out->w[i])) {
            rls_reset(rls);
            return;
        }
        for (uint32_t j = i; j < lo->eff_cfg.order; j++) {
            if (!is_rls_value_valid(lo->p[i][j]) ||
                !is_rls_value_valid(lo->p[j][i])) {
                rls_reset(rls);
                return;
            }
            const float32_t p_sym = 0.5F * (lo->p[i][j] + lo->p[j][i]);
            lo->p[i][j] = lo->p[j][i] = p_sym;
        }
    }
}

RLS_COMPACT
void rls_exec_in(struct rls_obs *rls, const float32_t y, const float32_t *x)
{
    CFG_READ(rls);
    DECL(rls, in, lo);

    in->y = y;
    for (uint32_t i = 0; i < lo->eff_cfg.order; i++) {
        in->x[i] = x[i];
    }

    rls_exec(rls);
}

static uint8_t is_rls_value_valid(const float32_t val)
{
    return val == val && ABS(val) <= RLS_VALUE_MAX;
}
