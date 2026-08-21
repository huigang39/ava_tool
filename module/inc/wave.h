#ifndef WAVE_H
#define WAVE_H
#include <stddef.h>
#include <stdint.h>

#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

enum wave_type {
    WAVE_TYPE_SINE,     // 正弦波
    WAVE_TYPE_SQUARE,   // 方波
    WAVE_TYPE_TRIANGLE, // 三角波
};

struct wave_cfg {
    float32_t      fs;   // 采样频率
    enum wave_type type; // 波形类型

    float32_t freq;   // 目标波形频率
    float32_t amp;    // 目标幅度
    float32_t phase;  // 累加相位
    float32_t offset; // 目标直流偏移量
    float32_t duty;   // 目标占空比(0~1, 仅用于方波/三角波)

    float32_t smooth_factor; // 平滑系数 (0.0~1.0].1.0为瞬间跳变,0.001为极度平滑
};

struct wave_out {
    float32_t val;
};

struct wave_lo {
    float32_t phase_incr;

    float32_t curr_freq;
    float32_t curr_amp;
    float32_t curr_offset;
    float32_t curr_duty;

    enum wave_type last_type; // 记录上一次的波形类型
    float32_t      mix;       // 混合因子: 0.0(全旧) -> 1.0(全新)
    float32_t      prev_val;  // 记录旧波形的瞬时值
};

struct wave {
    struct wave_cfg cfg;
    struct wave_out out;
    struct wave_lo  lo;
};

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

void wave_init(struct wave *wave, const struct wave_cfg wave_cfg);
void wave_exec(struct wave *wave);

#ifdef __cplusplus
}
#endif

#endif // !WAVE_H