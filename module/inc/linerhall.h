#ifndef LINERHALL_H
#define LINERHALL_H

#include "mathdef.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  类型定义                                  */
/* -------------------------------------------------------------------------- */

typedef struct linerhall_cfg {
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
} linerhall_cfg_t;

typedef struct linerhall_in {
        f32 sin_raw;
        f32 cos_raw;
} linerhall_in_t;

typedef struct linerhall_out {
        f32 theta;    // 计算得到的角度
        f32 sin_norm; // 归一化后的正弦信号
        f32 cos_norm; // 归一化后的余弦信号
        f32 amp;      // 信号幅值
        u8  valid;    // 角度是否有效
} linerhall_out_t;

typedef struct linerhall_fault {
        u32 signal_lost      : 1; // 信号丢失
        u32 amp_too_low      : 1; // 幅值过低
        u32 amp_too_high     : 1; // 幅值过高
        u32 amp_mismatch     : 1; // sin/cos幅值不匹配
        u32 theta_rate_fault : 1; // 角度变化率异常
        u32 signal_invalid   : 1; // 信号无效(NaN或Inf)
} linerhall_fault_t;

typedef struct linerhall_lo {
        f32 prev_theta;
        f32 theta_rate;
        f32 fault_time;
        u32 fault_cnt;
        u32 valid_cnt;

        linerhall_fault_t fault;
} linerhall_lo_t;

typedef struct linerhall {
        linerhall_cfg_t cfg;
        linerhall_in_t  in;
        linerhall_out_t out;
        linerhall_lo_t  lo;
} linerhall_t;

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief linerhall 结构体初始化
 *
 * @param linerhall     linerhall 结构体
 * @param linerhall_cfg linerhall 配置
 * @return              NULL
 */
void linerhall_init(linerhall_t *linerhall, linerhall_cfg_t linerhall_cfg);

/**
 * @brief linerhall 角度解算
 *
 * @param linerhall linerhall 结构体
 * @return          NULL
 */
void linerhall_exec(linerhall_t *linerhall);

/**
 * @brief linerhall 角度解算(带输入)
 *
 * @param linerhall linerhall 结构体
 * @param sin_raw   正弦值
 * @param cos_raw   余弦值
 * @return          NULL
 */
void linerhall_exec_in(linerhall_t *linerhall, f32 sin_raw, f32 cos_raw);

#ifdef __cplusplus
}
#endif

#endif // !LINERHALL_H
