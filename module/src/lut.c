#include "mathdef.h"

#include "lut.h"

f32
lut_idx(const f32 *y, f32 x, usize len)
{
        // 获取整数索引
        const u32 idx = (u32)x;

        // 边界处理
        if (idx >= len - 1)
                return y[len - 1];

        // 计算插值权重
        const f32 weight = x - (f32)idx;

        // 线性插值
        return y[idx] * (1.0f - weight) + y[idx + 1] * weight;
}

f32
lut_binary(const table_t *table, const f32 input)
{
        const f32 *x     = table->x;
        const f32 *y     = table->y;
        usize      left  = 0;
        usize      right = table->len - 1;

        if (input <= x[0])
                return y[0];

        if (input >= x[right])
                return y[right];

        while (right - left > 1) {
                const usize mid = (left + right) >> 1;
                if (input >= x[mid])
                        left = mid;
                else
                        right = mid;
        }

        const f32 dx    = x[left + 1] - x[left];
        const f32 ratio = (input - x[left]) / dx;
        return y[left] + ratio * (y[left + 1] - y[left]);
}

f32_dq_t
lut_interp_2d_dq(const f32 *x_axis, const f32 *y_axis, const f32 *z_table, usize x_len, usize y_len, f32 x_val, f32 y_val)
{
        // 1. X 轴二分查找及计算权重
        usize x_idx  = 0;
        f32   x_frac = 0.0f;
        if (x_val <= x_axis[0])
                x_idx = 0;
        else if (x_val >= x_axis[x_len - 1]) {
                x_idx  = x_len - 2; // 防止越界
                x_frac = 1.0f;
        } else {
                usize left = 0, right = x_len - 1;
                while (right - left > 1) {
                        const usize mid = (left + right) >> 1;
                        if (x_val >= x_axis[mid])
                                left = mid;
                        else
                                right = mid;
                }
                x_idx  = left;
                x_frac = (x_val - x_axis[x_idx]) / (x_axis[x_idx + 1] - x_axis[x_idx]);
        }

        // 2. Y 轴二分查找及计算权重
        usize y_idx  = 0;
        f32   y_frac = 0.0f;
        if (y_val <= y_axis[0])
                y_idx = 0;
        else if (y_val >= y_axis[y_len - 1]) {
                y_idx  = y_len - 2; // 防止越界
                y_frac = 1.0f;
        } else {
                usize left = 0, right = y_len - 1;
                while (right - left > 1) {
                        const usize mid = (left + right) >> 1;
                        if (y_val >= y_axis[mid])
                                left = mid;
                        else
                                right = mid;
                }
                y_idx  = left;
                y_frac = (y_val - y_axis[y_idx]) / (y_axis[y_idx + 1] - y_axis[y_idx]);
        }

        // 3. 双线性插值计算 (交织表 z_table: [d1, q1, d2, q2 ...])
        // 索引乘 2，因为每个网格点包含 d 和 q 两个 f32 数据
        const usize idx11 = (y_idx * x_len + x_idx) << 1;
        const usize idx12 = (y_idx * x_len + x_idx + 1) << 1;
        const usize idx21 = ((y_idx + 1) * x_len + x_idx) << 1;
        const usize idx22 = ((y_idx + 1) * x_len + x_idx + 1) << 1;

        f32_dq_t dq_out;

        // D 轴插值 (偏移为 0)
        const f32 d1 = z_table[idx11] + x_frac * (z_table[idx12] - z_table[idx11]);
        const f32 d2 = z_table[idx21] + x_frac * (z_table[idx22] - z_table[idx21]);
        dq_out.d     = d1 + y_frac * (d2 - d1);

        // Q 轴插值 (偏移为 1)
        const f32 q1 = z_table[idx11 + 1] + x_frac * (z_table[idx12 + 1] - z_table[idx11 + 1]);
        const f32 q2 = z_table[idx21 + 1] + x_frac * (z_table[idx22 + 1] - z_table[idx21 + 1]);
        dq_out.q     = q1 + y_frac * (q2 - q1);

        return dq_out;
}
