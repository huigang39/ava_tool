#ifndef FIR_H
#define FIR_H

#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef enum fir_order {
        FIR_1 = 1,
        FIR_2 = 2,
        FIR_3 = 3,
} fir_order_e;

typedef enum fir_type {
        FIR_LOWPASS,
        FIR_HIGHPASS,
        FIR_BANDPASS,
} fir_type_e;

typedef struct fir_cfg {
        f32         fs;
        f32         fh, fl;
        fir_order_e order;
        fir_type_e  type;
} fir_cfg_t;

typedef struct fir_in {
        f32 x;
} fir_in_t;

typedef struct fir_out {
        f32 y;
} fir_out_t;

typedef struct fir_lo {
        fir_cfg_t cfg;

        f32 fc;
        f32 w0, k;
        f32 b0, b1, b2, b3;
        f32 x1, x2, x3;
} fir_lo_t;

typedef struct fir_filter {
        fir_cfg_t cfg;
        fir_in_t  in;
        fir_out_t out;
        fir_lo_t  lo;
} fir_filter_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

int  fir_init(fir_filter_t *fir, fir_cfg_t fir_cfg);
void fir_exec(fir_filter_t *fir);
void fir_exec_in(fir_filter_t *fir, f32 x);

#ifdef __cplusplus
}
#endif

#endif // !FIR_H
