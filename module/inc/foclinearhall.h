#ifndef LINEARHALL_H
#define LINEARHALL_H

#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef struct linearhall_cfg {
        f32 fs;             // 采样频率
        f32 sin_offset;     // 正弦信号偏移量
        f32 cos_offset;     // 余弦信号偏移量
        f32 sin_gain;       // 正弦信号增益
        f32 cos_gain;       // 余弦信号增益
        f32 amp_min;        // 最小幅值阈值
        f32 amp_max;        // 最大幅值阈值
        f32 amp_tolerance;  // 幅值容差(用于检查sin和cos幅值一致性)
        f32 theta_rate_max; // 最大角度变化率
        f32 fault_timeout;  // 故障超时时间
} linearhall_cfg_t;

typedef struct linearhall_in {
        f32 sin_raw;
        f32 cos_raw;
} linearhall_in_t;

typedef struct linearhall_out {
        f32 theta;    // 计算得到的角度
        f32 sin_norm; // 归一化后的正弦信号
        f32 cos_norm; // 归一化后的余弦信号
        f32 amp;      // 信号幅值
        u8  valid;    // 角度是否有效
} linearhall_out_t;

typedef struct linearhall_fault {
        u32 signal_lost      : 1; // 信号丢失
        u32 amp_too_low      : 1; // 幅值过低
        u32 amp_too_high     : 1; // 幅值过高
        u32 amp_mismatch     : 1; // sin/cos幅值不匹配
        u32 theta_rate_fault : 1; // 角度变化率异常
        u32 signal_invalid   : 1; // 信号无效(NaN或Inf)
} linearhall_fault_t;

typedef struct linearhall_lo {
        f32 prev_theta;
        f32 theta_rate;
        f32 fault_time;
        u32 fault_cnt;
        u32 valid_cnt;

        linearhall_fault_t fault;
} linearhall_lo_t;

typedef struct linearhall {
        linearhall_cfg_t cfg;
        linearhall_in_t  in;
        linearhall_out_t out;
        linearhall_lo_t  lo;
} linearhall_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief linearhall 结构体初始化
 *
 * @param linearhall     linearhall 结构体
 * @param linearhall_cfg linearhall 配置
 * @return               void
 */
void linearhall_init(linearhall_t *linearhall, linearhall_cfg_t linearhall_cfg);

/**
 * @brief linearhall 角度解算
 *
 * @param linearhall linearhall 结构体
 * @return           void
 */
void linearhall_exec(linearhall_t *linearhall);

/**
 * @brief linearhall 角度解算(带输入)
 *
 * @param linearhall linearhall 结构体
 * @param sin_raw    正弦值
 * @param cos_raw    余弦值
 * @return           void
 */
void linearhall_exec_in(linearhall_t *linearhall, f32 sin_raw, f32 cos_raw);

#ifdef __cplusplus
}
#endif

#endif // !LINEARHALL_H
