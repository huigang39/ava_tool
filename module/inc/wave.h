#ifndef WAVE_H
#define WAVE_H

#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef enum wave_type {
        WAVE_TYPE_SINE,     // 正弦波
        WAVE_TYPE_SQUARE,   // 方波
        WAVE_TYPE_TRIANGLE, // 三角波
} wave_type_t;

typedef struct wave_cfg {
        f32         fs;   // 采样频率
        wave_type_t type; // 波形类型

        f32 freq;   // 目标波形频率
        f32 amp;    // 目标幅度
        f32 phase;  // 累加相位
        f32 offset; // 目标直流偏移量
        f32 duty;   // 目标占空比(0~1, 仅用于方波/三角波)

        f32 smooth_factor; // 平滑系数 (0.0~1.0]。1.0为瞬间跳变，0.001为极度平滑
} wave_cfg_t;

typedef struct wave_out {
        f32 val;
} wave_out_t;

typedef struct wave_lo {
        f32 phase_incr;

        f32 curr_freq;
        f32 curr_amp;
        f32 curr_offset;
        f32 curr_duty;

        wave_type_t last_type; // 记录上一次的波形类型
        f32         mix;       // 混合因子: 0.0(全旧) -> 1.0(全新)
        f32         prev_val;  // 记录旧波形的瞬时值
} wave_lo_t;

typedef struct wave {
        wave_cfg_t cfg;
        wave_out_t out;
        wave_lo_t  lo;
} wave_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

void wave_init(wave_t *wave, const wave_cfg_t wave_cfg);
void wave_exec(wave_t *wave);

#ifdef __cplusplus
}
#endif

#endif // !WAVE_H