#include "rls.h"

#include "mathdef.h"

#define RLS_DENOM_MIN (1.0e-12f)
#define RLS_VALUE_MAX (1.0e30f)

static u8
rls_is_finite(const f32 val)
{
        return val == val && ABS(val) <= RLS_VALUE_MAX;
}

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

void
rls_init(rls_obs_t *rls, const rls_cfg_t rls_cfg)
{
        DECL(rls, cfg);
        CFG_INIT(rls, rls_cfg);

        if (cfg->order == 0)
                cfg->order = 1;
        if (cfg->order > MAX_ORDER)
                cfg->order = MAX_ORDER;

        if (!rls_is_finite(cfg->lambda) || cfg->lambda <= 0.0f || cfg->lambda > 1.0f)
                cfg->lambda = 1.0f;
        if (!rls_is_finite(cfg->delta) || cfg->delta <= 0.0f)
                cfg->delta = 1.0f;

        rls_reset(rls);
}

void
rls_reset(rls_obs_t *rls)
{
        DECL(rls, cfg, in, out, lo);

        for (u32 i = 0; i < MAX_ORDER; i++) {
                in->x[i]  = 0.0f;
                out->w[i] = 0.0f;
                lo->px[i] = lo->k[i] = lo->xtp[i] = 0.0f;
                for (u32 j = 0; j < MAX_ORDER; j++) {
                        if (i == j)
                                lo->p[i][j] = cfg->delta;
                        else
                                lo->p[i][j] = 0.0f;
                        lo->temp[i][j] = 0.0f;
                }
        }
        in->y = lo->err = lo->denom = lo->y_hat = 0.0f;
}

void
rls_exec(rls_obs_t *rls)
{
        DECL(rls, cfg, in, out, lo);

        if (!rls_is_finite(in->y))
                return;
        for (u32 i = 0; i < cfg->order; i++) {
                if (!rls_is_finite(in->x[i]))
                        return;
        }

        lo->y_hat = 0.0f;
        for (u32 i = 0; i < cfg->order; i++)
                lo->y_hat += out->w[i] * in->x[i];

        lo->err = in->y - lo->y_hat;

        for (u32 i = 0; i < cfg->order; i++) {
                lo->px[i] = 0.0f;
                for (u32 j = 0; j < cfg->order; j++)
                        lo->px[i] += lo->p[i][j] * in->x[j];
        }

        lo->denom = cfg->lambda;
        for (u32 i = 0; i < cfg->order; i++)
                lo->denom += in->x[i] * lo->px[i];

        if (!rls_is_finite(lo->denom) || lo->denom <= RLS_DENOM_MIN) {
                rls_reset(rls);
                return;
        }

        for (u32 i = 0; i < cfg->order; i++)
                lo->k[i] = lo->px[i] / lo->denom;

        for (u32 i = 0; i < cfg->order; i++)
                out->w[i] += lo->k[i] * lo->err;

        for (u32 j = 0; j < cfg->order; j++) {
                lo->xtp[j] = 0.0f;
                for (u32 i = 0; i < cfg->order; i++)
                        lo->xtp[j] += in->x[i] * lo->p[i][j];
        }

        for (u32 i = 0; i < cfg->order; i++) {
                for (u32 j = 0; j < cfg->order; j++)
                        lo->temp[i][j] = lo->k[i] * lo->xtp[j];
        }

        for (u32 i = 0; i < cfg->order; i++) {
                for (u32 j = 0; j < cfg->order; j++)
                        lo->p[i][j] = (lo->p[i][j] - lo->temp[i][j]) / cfg->lambda;
        }

        for (u32 i = 0; i < cfg->order; i++) {
                if (!rls_is_finite(out->w[i])) {
                        rls_reset(rls);
                        return;
                }
                for (u32 j = i; j < cfg->order; j++) {
                        if (!rls_is_finite(lo->p[i][j]) || !rls_is_finite(lo->p[j][i])) {
                                rls_reset(rls);
                                return;
                        }
                        const f32 p_sym = 0.5f * (lo->p[i][j] + lo->p[j][i]);
                        lo->p[i][j] = lo->p[j][i] = p_sym;
                }
        }
}

void
rls_exec_in(rls_obs_t *rls, const f32 y, const f32 *x)
{
        DECL(rls, cfg, in);

        in->y = y;
        for (u32 i = 0; i < cfg->order; i++)
                in->x[i] = x[i];

        rls_exec(rls);
}
