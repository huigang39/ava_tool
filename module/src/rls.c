#include "rls.h"

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

void
rls_init(rls_obs_t *rls, const rls_cfg_t rls_cfg)
{
        DECL(rls, cfg, out, lo);
        CFG_INIT(rls, rls_cfg);

        if (cfg->order > MAX_ORDER)
                cfg->order = MAX_ORDER;

        for (u32 i = 0; i < cfg->order; i++) {
                for (u32 j = 0; j < cfg->order; j++) {
                        if (i == j)
                                lo->p[i][j] = cfg->delta;
                        else
                                lo->p[i][j] = 0.0f;
                }
                out->w[i] = 0.0f;
        }
}

void
rls_exec(rls_obs_t *rls)
{
        DECL(rls, cfg, in, out, lo);

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
