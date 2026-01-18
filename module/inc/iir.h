#ifndef IIR_H
#define IIR_H

#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef enum iir_order {
        IIR_1 = 1,
        IIR_2 = 2,
} iir_order_e;

typedef enum iir_type {
        IIR_LOWPASS,
        IIR_HIGHPASS,
        IIR_BANDPASS,
} iir_type_e;

typedef struct iif_cfg {
        f32         fs;
        f32         fh, fl;
        iir_order_e order;
        iir_type_e  type;
} iir_cfg_t;

typedef struct iir_in {
        f32 x;
} iir_in_t;

typedef struct iir_out {
        f32 y;
} iir_out_t;

typedef struct iir_lo {
        iir_cfg_t cfg;

        f32 fc;
        f32 rc;
        f32 alpha;
        f32 w0, q;
        f32 x1, x2, y1, y2;
        f32 b0, b1, b2;
        f32 a0, a1, a2;
        f32 norm_a0, norm_a1, norm_a2, norm_a3, norm_a4;
} iir_lo_t;

typedef struct iir_filter {
        iir_cfg_t cfg;
        iir_in_t  in;
        iir_out_t out;
        iir_lo_t  lo;
} iir_filter_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

int  iir_init(iir_filter_t *iir, iir_cfg_t iir_cfg);
void iir_exec(iir_filter_t *iir);
void iir_exec_in(iir_filter_t *iir, f32 x);

#ifdef __cplusplus
}
#endif

#endif // !IIR_H
