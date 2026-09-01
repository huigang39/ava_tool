#ifndef WAVE_H
#define WAVE_H
#include <stddef.h>
#include <stdint.h>

#include "errdef.h"
#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

enum wave_type {
    WAVE_TYPE_SINE,     // 正弦波
    WAVE_TYPE_SQUARE,   // 方波
    WAVE_TYPE_TRIANGLE, // 三角波
};

struct wave_cfg {
    // 采样与波形类型
    float32_t fs;        // 采样频率
    enum wave_type type; // 波形类型

    // 目标波形参数
    float32_t freq;   // 目标波形频率
    float32_t amp;    // 目标幅度
    float32_t phase;  // 初始相位
    float32_t offset; // 目标直流偏移量
    float32_t duty;   // 目标占空比(0~1, 仅用于方波/三角波)

    // 参数平滑
    float32_t smooth_factor; // 平滑系数 (0.0~1.0].1.0为瞬间跳变,0.001为极度平滑
};

struct wave_out {
    float32_t val; // 当前波形输出
};

struct wave_lo {
    // 波形切换状态
    enum wave_type prev_type; // 最近一次已完成过渡的波形类型
    float32_t mix;            // 旧波形到当前波形的混合比例, 范围: 0 到 1

    // 相位累积
    float32_t phase;           // 当前波形相位, 单位: rad
    float32_t phase_increment; // 每次执行的相位增量, 单位: rad

    // 当前平滑参数
    float32_t curr_freq;   // 平滑后的频率, 单位: Hz
    float32_t curr_amp;    // 平滑后的幅值
    float32_t curr_offset; // 平滑后的直流偏置
    float32_t curr_duty;   // 平滑后的占空比
};

struct wave {
    struct wave_cfg cfg; // 配置参数
    struct wave_out out; // 输出结果
    struct wave_lo lo;   // 运行状态
};

/**
 * @brief                  初始化波形发生器
 * @param[in,out] wave     波形发生器对象
 * @param[in]     wave_cfg 波形及采样配置
 * @return                 成功返回 0, 失败返回负错误码
 */
int wave_init(struct wave *wave, const struct wave_cfg wave_cfg);

/**
 * @brief              推进一个采样周期并更新输出
 * @param[in,out] wave 已初始化的波形发生器
 * @return             成功返回 0, 失败返回负错误码
 */
int wave_exec(struct wave *wave);

#ifdef __cplusplus
}
#endif

#endif
