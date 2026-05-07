#ifndef LUT_H
#define LUT_H

#include "typedef.h"

typedef struct table {
        const f32 *x;   // 自变量数组(必须单调递增)
        const f32 *y;   // 因变量数组
        usize      len; // 表长度
} table_t;

f32 lut_idx(const f32 *y, f32 x, usize len);

/**
 * @brief 查询输入数据在表中对应的 y 值(线性插值)
 *
 * @param table table 结构体
 * @param input 待查询数据
 * @return      f32
 */
f32 lut_binary(const table_t *table, f32 input);

f32_dq_t
lut_interp_2d_dq(const f32 *x_axis, const f32 *y_axis, const f32 *z_table, usize x_len, usize y_len, f32 x_val, f32 y_val);

#endif // LUT_H
