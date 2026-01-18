#ifndef MAF_H
#define MAF_H

#include "spsc.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef struct maf_cfg {
        f32 *buf;
        u32  cap;
} maf_cfg_t;

typedef struct maf_in {
        f32 x;
} maf_in_t;

typedef struct maf_out {
        f32 y;
} maf_out_t;

typedef struct maf_lo {
        spsc_t spsc;
        f64    sum;
} maf_lo_t;

typedef struct maf_filter {
        maf_cfg_t cfg;
        maf_in_t  in;
        maf_out_t out;
        maf_lo_t  lo;
} maf_filter_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

void maf_init(maf_filter_t *maf, maf_cfg_t maf_cfg);
void maf_exec(maf_filter_t *maf);
void maf_exec_in(maf_filter_t *maf, f32 x);

#ifdef __cplusplus
}
#endif

#endif // !MAF_H
