#ifndef FIR_H
#define FIR_H
#include <stddef.h>
#include <stdint.h>

#include "errdef.h"
#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

enum fir_order {
    FIR_1 = 1, // 一阶 FIR
    FIR_2 = 2, // 二阶 FIR
    FIR_3 = 3, // 三阶 FIR
};

enum fir_type {
    FIR_LOWPASS,  // 低通滤波
    FIR_HIGHPASS, // 高通滤波
    FIR_BANDPASS, // 带通滤波
};

struct fir_cfg {
    // 采样与截止频率
    float32_t fs; // 采样频率, 单位: Hz
    float32_t fh; // 上截止频率, 单位: Hz
    float32_t fl; // 下截止频率, 单位: Hz

    // 滤波类型与阶数
    enum fir_order order; // 算法阶数
    enum fir_type type;   // 处理类型
};

struct fir_in {
    float32_t x; // 算法输入
};

struct fir_out {
    float32_t y; // 算法输出
};

struct fir_lo {
    // 频率与系数计算
    float32_t fc; // 滤波中心或截止频率, 单位: Hz
    float32_t w0; // 归一化角频率, 单位: rad/sample
    float32_t k;  // 带通滤波系数计算中间值

    // 滤波系数
    float32_t b0; // 输入延迟 0 个周期的系数
    float32_t b1; // 输入延迟 1 个周期的系数
    float32_t b2; // 输入延迟 2 个周期的系数
    float32_t b3; // 输入延迟 3 个周期的系数

    // 输入历史
    float32_t x1; // 前 1 个周期的输入值
    float32_t x2; // 前 2 个周期的输入值
    float32_t x3; // 前 3 个周期的输入值
};

struct fir_filter {
    struct fir_cfg cfg; // 配置参数
    struct fir_in in;   // 输入数据
    struct fir_out out; // 输出结果
    struct fir_lo lo;   // 运行状态
};

/**
 * @brief                 FIR 滤波器结构体初始化
 * @param[in,out] fir     FIR 滤波器结构体
 * @param[in]     fir_cfg FIR 滤波器配置
 * @return                成功返回 0, 失败返回负错误码
 */
int fir_init(struct fir_filter *fir, struct fir_cfg fir_cfg);

/**
 * @brief             FIR 滤波器单次滤波计算
 * @param[in,out] fir FIR 滤波器结构体
 * @return            成功返回 0, 失败返回负错误码
 */
int fir_exec(struct fir_filter *fir);

/**
 * @brief             FIR 滤波器单次滤波计算(带输入)
 * @param[in,out] fir FIR 滤波器结构体
 * @param[in]     x   待计算的数据
 * @return            成功返回 0, 失败返回负错误码
 */
int fir_exec_in(struct fir_filter *fir, float32_t x);

#ifdef __cplusplus
}
#endif

#endif
