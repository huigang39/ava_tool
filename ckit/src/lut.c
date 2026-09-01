#include "mathdef.h"

#include "lut.h"

float32_t lut_idx(const float32_t *y, float32_t x, size_t len)
{
    if (y == NULL || len == 0U || !is_f32_finite_rt(x)) {
        return 0.0F;
    }
    if (x <= 0.0F || len == 1U) {
        return y[0];
    }
    if (x >= (float32_t)(len - 1U)) {
        return y[len - 1U];
    }

    const size_t idx = (size_t)x;
    const float32_t weight = x - (float32_t)idx;
    return y[idx] * (1.0F - weight) + y[idx + 1U] * weight;
}

float32_t lut_idx_periodic(const float32_t *y, float32_t x, const size_t len)
{
    if (y == NULL || len == 0U || !is_f32_finite_rt(x)) {
        return 0.0F;
    }
    if (len == 1U) {
        return y[0];
    }

    x = MOD(x, (float32_t)len);
    if (x < 0.0F) {
        x += (float32_t)len;
    }

    // 极小负数回绕后可能舍入到 len, 限制下标以免越界
    const size_t raw = (size_t)x;
    const size_t idx = raw < len ? raw : len - 1U;
    const size_t next = idx + 1U < len ? idx + 1U : 0U;
    const float32_t weight = x - (float32_t)idx;
    const float32_t y0 = y[idx];
    const float32_t y1 = y[next];
    return y0 * (1.0F - weight) + y1 * weight;
}

float32_t lut_binary_idx(const float32_t *axis,
                         const size_t len,
                         const float32_t in)
{
    if (axis == NULL || len < 2U || !is_f32_finite_rt(in)) {
        return 0.0F;
    }

    const size_t prev = len - 1U;
    const bool ascending = axis[prev] >= axis[0];

    if ((ascending && in <= axis[0]) || (!ascending && in >= axis[0])) {
        return 0.0F;
    }
    if ((ascending && in >= axis[prev]) || (!ascending && in <= axis[prev])) {
        return (float32_t)prev;
    }

    size_t left = 0U;    // 二分查找左边界索引
    size_t right = prev; // 二分查找右边界索引
    while (right - left > 1U) {
        const size_t mid = left + ((right - left) >> 1U);
        if ((ascending && in >= axis[mid]) || (!ascending && in <= axis[mid])) {
            left = mid;
        } else {
            right = mid;
        }
    }

    const float32_t interval = axis[right] - axis[left];
    if (interval == 0.0F) {
        return (float32_t)left;
    }
    return (float32_t)left + (in - axis[left]) / interval;
}

float32_t lut_binary(const struct table *table, const float32_t in)
{
    if (table == NULL || table->x == NULL || table->y == NULL ||
        table->len == 0U || !is_f32_finite_rt(in)) {
        return 0.0F;
    }
    const float32_t *x = table->x;
    const float32_t *y = table->y;
    size_t left = 0;               // 二分查找左边界索引
    size_t right = table->len - 1; // 二分查找右边界索引

    if (in <= x[0]) {
        return y[0];
    }

    if (in >= x[right]) {
        return y[right];
    }

    while (right - left > 1) {
        const size_t mid = left + ((right - left) >> 1U);
        if (in >= x[mid]) {
            left = mid;
        } else {
            right = mid;
        }
    }

    const float32_t dx = x[left + 1] - x[left];
    const float32_t ratio = (in - x[left]) / dx;
    return y[left] + ratio * (y[left + 1] - y[left]);
}
