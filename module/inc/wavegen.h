#ifndef WAVEGEN_H
#define WAVEGEN_H

#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef enum wave_type {
        WAVE_TYPE_SINE,   // 正弦波
        WAVE_TYPE_SQUARE, // 方波
} wave_type_t;

typedef struct wave_cfg {
        f32         fs;        // 采样频率
        f32         wave_freq; // 波形频率
        wave_type_t type;      // 波形类型
        f32         amp;       // 幅度
        f32         phase;     // 初始相位
        f32         offset;    // 直流偏移量
        f32         duty;      // 占空比(0~1, 仅用于方波)
} wave_cfg_t;

typedef struct wave_out {
        f32 val;
} wave_out_t;

typedef struct wave_lo {
        f32 phase_incr;
} wave_lo_t;

typedef struct wave {
        wave_cfg_t cfg;
        wave_out_t out;
        wave_lo_t  lo;
} wave_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

void wave_init(wave_t *wave, wave_cfg_t wave_cfg);
void wave_exec(wave_t *wave);

#ifdef __cplusplus
}
#endif

#endif // !WAVEGEN_H
