#ifndef IIR_H
#define IIR_H
#include <stddef.h>
#include <stdint.h>

#include "errdef.h"
#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

enum iir_order {
    IIR_1 = 1, // 一阶 IIR
    IIR_2 = 2, // 二阶 IIR
};

enum iir_type {
    IIR_LOWPASS,  // 低通滤波
    IIR_HIGHPASS, // 高通滤波
    IIR_BANDPASS, // 带通滤波
};

struct iir_cfg {
    // 截止频率
    float32_t fh; // 上截止频率, 单位: Hz
    float32_t fl; // 下截止频率, 单位: Hz

    // 滤波类型与阶数
    enum iir_order order; // 滤波器阶数
    enum iir_type type;   // 处理类型
};

struct iir_in {
    float32_t x; // 算法输入
};

struct iir_out {
    float32_t y; // 算法输出
};

struct iir_lo {
    // 采样频率与派生参数
    float32_t fs;    // 采样频率, 单位: Hz
    float32_t fc;    // 滤波中心或截止频率, 单位: Hz
    float32_t rc;    // 一阶滤波时间常数, 单位: s
    float32_t alpha; // 滤波系数
    float32_t w0;    // 归一化角频率, 单位: rad/sample
    float32_t q;     // 品质因数

    // 输入输出历史
    float32_t x1; // 前 1 个周期的输入值
    float32_t x2; // 前 2 个周期的输入值
    float32_t y1; // 上一周期输出值
    float32_t y2; // 前 2 个周期的输出值

    // 差分方程系数
    float32_t b0; // 输入延迟 0 个周期的系数
    float32_t b1; // 输入延迟 1 个周期的系数
    float32_t b2; // 输入延迟 2 个周期的系数
    float32_t a0; // 输出归一化基准系数
    float32_t a1; // 输出延迟 1 个周期的反馈系数
    float32_t a2; // 输出延迟 2 个周期的反馈系数

    // 归一化系数
    float32_t norm_a0; // 二阶滤波输入系数 b0/a0
    float32_t norm_a1; // 一阶反馈系数或二阶输入系数 b1/a0
    float32_t norm_a2; // 二阶滤波输入系数 b2/a0
    float32_t norm_a3; // 二阶滤波反馈系数 a1/a0
    float32_t norm_a4; // 二阶滤波反馈系数 a2/a0
};

struct iir_filter {
    struct iir_cfg cfg; // 配置参数
    struct iir_in in;   // 输入数据
    struct iir_out out; // 输出结果
    struct iir_lo lo;   // 运行状态
};

/**
 * @brief                 IIR 滤波器结构体初始化
 * @param[in,out] iir     IIR 滤波器结构体
 * @param[in]     iir_cfg IIR 滤波器配置
 * @param[in]     fs      采样频率, 单位: Hz
 * @return                成功返回 0, 失败返回负错误码
 * @note                  二阶滤波的上截止频率不能超过采样频率的一半
 */
int iir_init(struct iir_filter *iir, struct iir_cfg iir_cfg, float32_t fs);

/**
 * @brief             IIR 滤波器单次滤波计算
 * @param[in,out] iir IIR 滤波器结构体
 * @return            成功返回 0, 失败返回负错误码
 */
int iir_exec(struct iir_filter *iir);

/**
 * @brief             IIR 滤波器单次滤波计算(带输入)
 * @param[in,out] iir IIR 滤波器结构体
 * @param[in]     x   待计算的数据
 * @return            成功返回 0, 失败返回负错误码
 */
int iir_exec_in(struct iir_filter *iir, float32_t x);

#ifdef __cplusplus
}
#endif

#endif
