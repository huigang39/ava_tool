#include "rls.h"

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

void
rls_init(rls_filter_t *rls, const rls_cfg_t rls_cfg)
{
        CFG_INIT(rls, rls_cfg);
        DECL(rls, cfg);
}

void
rls_exec(rls_filter_t *rls)
{
        DECL(rls, cfg, in, out, lo);

        for (u32 i = cfg->order - 1; i > 0; i--)
                lo->x[i] = lo->x[i - 1];

        lo->x[0] = in->x;

        for (u32 i = 0; i < cfg->order; i++)
                out->y_hat += lo->w[i] * lo->x[i];

        lo->err = in->ref - out->y_hat;

        for (u32 i = 0; i < cfg->order; i++) {
                for (u32 j = 0; j < cfg->order; j++)
                        lo->px[i] += lo->p[i][j] * lo->x[j];
        }

        for (u32 i = 0; i < cfg->order; i++)
                lo->denom += lo->x[i] * lo->px[i];

        for (u32 i = 0; i < cfg->order; i++)
                lo->k[i] = lo->px[i] / lo->denom;

        for (u32 i = 0; i < cfg->order; i++)
                lo->w[i] += lo->k[i] * lo->err;

        for (u32 j = 0; j < cfg->order; j++) {
                for (u32 i = 0; i < cfg->order; i++)
                        lo->xtp[j] += lo->x[i] * lo->p[i][j];
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
rls_exec_in(rls_filter_t *rls, const f32 ref, const f32 x)
{
        DECL(rls, in);

        in->ref = ref;
        in->x   = x;
        rls_exec(rls);
}
