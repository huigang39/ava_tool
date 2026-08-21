#include "mathdef.h"

#include "lut.h"

float32_t
lut_idx(const float32_t *y, float32_t x, size_t len)
{
    // 获取整数索引
    const uint32_t idx = (uint32_t)x;

    // 边界处理
    if (idx >= len - 1)
        return y[len - 1];

    // 计算插值权重
    const float32_t weight = x - (float32_t)idx;

    // 线性插值
    return y[idx] * (1.0F - weight) + y[idx + 1] * weight;
}

float32_t
lut_binary(const struct table *table, const float32_t input)
{
    const float32_t *x     = table->x;
    const float32_t *y     = table->y;
    size_t           left  = 0;
    size_t           right = table->len - 1;

    if (input <= x[0])
        return y[0];

    if (input >= x[right])
        return y[right];

    while (right - left > 1) {
        const size_t mid = (left + right) >> 1;
        if (input >= x[mid])
            left = mid;
        else
            right = mid;
    }

    const float32_t dx    = x[left + 1] - x[left];
    const float32_t ratio = (input - x[left]) / dx;
    return y[left] + ratio * (y[left + 1] - y[left]);
}

struct f32_dq
lut_interp_2d_dq(const float32_t *x_axis,
                 const float32_t *y_axis,
                 const float32_t *z_table,
                 size_t           x_len,
                 size_t           y_len,
                 float32_t        x_val,
                 float32_t        y_val)
{
    size_t    x_idx  = 0;
    float32_t x_frac = 0.0F;
    if (x_val <= x_axis[0])
        x_idx = 0;
    else if (x_val >= x_axis[x_len - 1]) {
        x_idx  = x_len - 2;
        x_frac = 1.0F;
    } else {
        size_t left = 0, right = x_len - 1;
        while (right - left > 1) {
            const size_t mid = (left + right) >> 1;
            if (x_val >= x_axis[mid])
                left = mid;
            else
                right = mid;
        }
        x_idx  = left;
        x_frac = (x_val - x_axis[x_idx]) / (x_axis[x_idx + 1] - x_axis[x_idx]);
    }

    size_t    y_idx  = 0;
    float32_t y_frac = 0.0F;
    if (y_val <= y_axis[0])
        y_idx = 0;
    else if (y_val >= y_axis[y_len - 1]) {
        y_idx  = y_len - 2;
        y_frac = 1.0F;
    } else {
        size_t left = 0, right = y_len - 1;
        while (right - left > 1) {
            const size_t mid = (left + right) >> 1;
            if (y_val >= y_axis[mid])
                left = mid;
            else
                right = mid;
        }
        y_idx  = left;
        y_frac = (y_val - y_axis[y_idx]) / (y_axis[y_idx + 1] - y_axis[y_idx]);
    }

    // 每个网格点交错存储 d/q 值.
    const size_t idx11 = (y_idx * x_len + x_idx) << 1;
    const size_t idx12 = (y_idx * x_len + x_idx + 1) << 1;
    const size_t idx21 = ((y_idx + 1) * x_len + x_idx) << 1;
    const size_t idx22 = ((y_idx + 1) * x_len + x_idx + 1) << 1;

    struct f32_dq dq_out;

    const float32_t d1 = z_table[idx11] + x_frac * (z_table[idx12] - z_table[idx11]);
    const float32_t d2 = z_table[idx21] + x_frac * (z_table[idx22] - z_table[idx21]);
    dq_out.d           = d1 + y_frac * (d2 - d1);

    const float32_t q1 = z_table[idx11 + 1] + x_frac * (z_table[idx12 + 1] - z_table[idx11 + 1]);
    const float32_t q2 = z_table[idx21 + 1] + x_frac * (z_table[idx22 + 1] - z_table[idx21 + 1]);
    dq_out.q           = q1 + y_frac * (q2 - q1);

    return dq_out;
}
