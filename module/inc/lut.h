#ifndef LUT_H
#define LUT_H

#include "motor_control/types.h"

struct table {
    const float32_t *x;   // 自变量数组(必须单调递增)
    const float32_t *y;   // 因变量数组
    size_t           len; // 表长度
};

float32_t lut_idx(const float32_t *y, float32_t x, size_t len);

/**
 * @brief 查询输入数据在表中对应的 y 值(线性插值)
 *
 * @param table table 结构体
 * @param input 待查询数据
 * @return      float32_t
 */
float32_t lut_binary(const struct table *table, float32_t input);

struct f32_dq lut_interp_2d_dq(const float32_t *x_axis,
                               const float32_t *y_axis,
                               const float32_t *z_table,
                               size_t           x_len,
                               size_t           y_len,
                               float32_t        x_val,
                               float32_t        y_val);

#endif // LUT_H
