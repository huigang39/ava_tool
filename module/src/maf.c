#include "maf.h"

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

void
maf_init(maf_filter_t *maf, const maf_cfg_t maf_cfg)
{
        CFG_INIT(maf, maf_cfg);
        DECL(maf, cfg, lo);

        spsc_init(&lo->spsc, cfg->buf, cfg->cap, SPSC_POLICY_REJECT);
}

void
maf_exec(maf_filter_t *maf)
{
        DECL(maf, cfg, in, lo);

        f32 prev_x;
        spsc_read(&lo->spsc, &prev_x, sizeof(prev_x));
        lo->sum -= prev_x;

        spsc_write(&lo->spsc, &in->x, sizeof(in->x));

        lo->sum += in->x;
        lo->sum /= (f32)cfg->cap;
}

void
maf_exec_in(maf_filter_t *maf, const f32 x)
{
        DECL(maf, in);

        in->x = x;
        maf_exec(maf);
}
