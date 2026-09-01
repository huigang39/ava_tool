#ifndef LUT_H
#define LUT_H

#include "platdef.h"

#ifdef __cplusplus
extern "C" {
#endif

struct table {
    const float32_t *x; // 自变量数组(必须单调递增)
    const float32_t *y; // 因变量数组
    size_t len;         // 表长度
};

/**
 * @brief         按浮点索引执行线性插值
 * @param[in] y   查找表数据
 * @param[in] x   浮点索引
 * @param[in] len 表元素数量
 * @return        插值结果, 超出范围返回端点值, 无效输入返回 0
 */
float32_t lut_idx(const float32_t *y, float32_t x, size_t len);

/**
 * @brief         周期表线性插值
 * @param[in] y   周期查找表
 * @param[in] x   浮点索引
 * @param[in] len 周期表元素数量
 * @return        周期插值后的表值, 无效输入返回 0
 * @note          末点与首点连续插值, 索引周期为 len
 */
float32_t lut_idx_periodic(const float32_t *y, float32_t x, size_t len);

/**
 * @brief          在单调坐标轴中反查带小数的索引
 *                 坐标轴可以单调递增或单调递减 输入超出范围时钳位到首尾索引
 *                 例如返回 2.5F 表示 input 位于 axis[2] 与 axis[3] 的中点
 * @param[in] axis 单调递增或单调递减的坐标轴
 * @param[in] len  坐标轴长度
 * @param[in] in   待查询值
 * @return         插值后的浮点索引; 无效或单点坐标轴返回 0
 */
float32_t lut_binary_idx(const float32_t *axis, size_t len, float32_t in);

/**
 * @brief           查询输入数据在表中对应的 y 值(线性插值)
 * @param[in] table table 结构体
 * @param[in] in    待查询数据
 * @return          线性插值后的表值, 无效输入返回 0
 */
float32_t lut_binary(const struct table *table, float32_t in);

#ifdef __cplusplus
}
#endif

#endif
