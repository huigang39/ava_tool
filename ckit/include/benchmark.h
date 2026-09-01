#ifndef BENCHMARK_H
#define BENCHMARK_H

#ifdef ARM_MATH
#include "arm_math.h"
#endif
#include <stddef.h>
#include <stdint.h>

#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DWT_INIT()                                      \
    do {                                                \
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; \
        DWT->CYCCNT = 0;                                \
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;            \
    } while (0)

/**
 * @brief  读取当前周期计数
 * @return 当前硬件周期计数, 不支持的平台返回 0
 */
HAPI uint32_t benchmark_read_cycle_cnt_rt(void)
{
#if defined(__arm__) || defined(__thumb__) || defined(__ARM_ARCH)
    return *(volatile const uint32_t *)0xE0001004UL;
#else
    return 0;
#endif
}

#define MEASURE_TIME(total_cycle_cnt, iter_cnt_max, code) \
    do {                                                  \
        uint32_t __start, __end, __single;                \
        (total_cycle_cnt) = 0;                            \
        for (uint32_t i = 0; i < (iter_cnt_max); i++) {   \
            __start = benchmark_read_cycle_cnt_rt();      \
            {code};                                       \
            __end = benchmark_read_cycle_cnt_rt();        \
            __single = __end - __start;                   \
            (total_cycle_cnt) += __single;                \
        }                                                 \
    } while (0)

#define CYCCNT2US(cycle_cnt) \
    ((cycle_cnt) * (1.0F / (SystemCoreClock / M(1.0F))))

struct benchmark {
    // 测试标识
    const char *name;

    // 周期计数
    uint32_t total_cycle_cnt;  // 累计执行周期数
    uint32_t single_cycle_cnt; // 单次执行周期数

    // 耗时统计
    float32_t total_exec_us;  // 累计耗时, 单位: us
    float32_t single_exec_us; // 单次耗时, 单位: us

    union {
        float32_t float_val; // 浮点计算结果
        int32_t int_val;     // 整数计算结果
    } out;
};

#define TEST_OP(result, iter_cnt_max, op_name, op_code)                  \
    do {                                                                 \
        result.name = op_name;                                           \
        MEASURE_TIME(result.total_cycle_cnt, iter_cnt_max, op_code);     \
        result.single_cycle_cnt = result.total_cycle_cnt / iter_cnt_max; \
        result.total_exec_us = CYCCNT2US(result.total_cycle_cnt);        \
        result.single_exec_us = result.total_exec_us / iter_cnt_max;     \
    } while (0)

// 整数运算
#define TEST_INT_ADD(result, iter_cnt_max)         \
    do {                                           \
        volatile int __a = 123456, __b = 789;      \
        TEST_OP(result, iter_cnt_max, "int_add", { \
            result.out.int_val = __a + __b;        \
        });                                        \
    } while (0)

#define TEST_INT_MUL(result, iter_cnt_max)         \
    do {                                           \
        volatile int __a = 123456, __b = 789;      \
        TEST_OP(result, iter_cnt_max, "int_mul", { \
            result.out.int_val = __a * __b;        \
        });                                        \
    } while (0)

#define TEST_INT_DIV(result, iter_cnt_max)         \
    do {                                           \
        volatile int __a = 123456, __b = 789;      \
        TEST_OP(result, iter_cnt_max, "int_div", { \
            result.out.int_val = __a / __b;        \
        });                                        \
    } while (0)

// 浮点运算
#define TEST_FLOAT_ADD(result, iter_cnt_max)            \
    do {                                                \
        volatile float32_t __a = 123.456F, __b = 7.89F; \
        TEST_OP(result, iter_cnt_max, "float_add", {    \
            result.out.float_val = __a + __b;           \
        });                                             \
    } while (0)

#define TEST_FLOAT_MUL(result, iter_cnt_max)            \
    do {                                                \
        volatile float32_t __a = 123.456F, __b = 7.89F; \
        TEST_OP(result, iter_cnt_max, "float_mul", {    \
            result.out.float_val = __a * __b;           \
        });                                             \
    } while (0)

#define TEST_FLOAT_DIV(result, iter_cnt_max)            \
    do {                                                \
        volatile float32_t __a = 123.456F, __b = 7.89F; \
        TEST_OP(result, iter_cnt_max, "float_div", {    \
            result.out.float_val = __a / __b;           \
        });                                             \
    } while (0)

// 三角函数
#define TEST_SINF(result, iter_cnt_max)         \
    do {                                        \
        volatile float32_t __x = 0.785398F;     \
        TEST_OP(result, iter_cnt_max, "sinf", { \
            result.out.float_val = sinf(__x);   \
        });                                     \
    } while (0)

#define TEST_COSF(result, iter_cnt_max)         \
    do {                                        \
        volatile float32_t __x = 0.785398F;     \
        TEST_OP(result, iter_cnt_max, "cosf", { \
            result.out.float_val = cosf(__x);   \
        });                                     \
    } while (0)

#define TEST_TANF(result, iter_cnt_max)         \
    do {                                        \
        volatile float32_t __x = 0.785398F;     \
        TEST_OP(result, iter_cnt_max, "tanf", { \
            result.out.float_val = tanf(__x);   \
        });                                     \
    } while (0)

#ifdef ARM_MATH
#define TEST_ARM_SINF(result, iter_cnt_max)          \
    do {                                             \
        volatile float32_t __x = 0.785398F;          \
        TEST_OP(result, iter_cnt_max, "arm_sinf", {  \
            result.out.float_val = arm_sin_f32(__x); \
        });                                          \
    } while (0)

#define TEST_ARM_COSF(result, iter_cnt_max)          \
    do {                                             \
        volatile float32_t __x = 0.785398F;          \
        TEST_OP(result, iter_cnt_max, "arm_cosf", {  \
            result.out.float_val = arm_cos_f32(__x); \
        });                                          \
    } while (0)
#else
#define TEST_ARM_SINF(result, iter_cnt_max)
#define TEST_ARM_COSF(result, iter_cnt_max)
#endif

#define TEST_FAST_SINF(result, iter_cnt_max)          \
    do {                                              \
        volatile float32_t __x = 0.785398F;           \
        TEST_OP(result, iter_cnt_max, "fast_sinf", {  \
            result.out.float_val = fast_sinf_rt(__x); \
        });                                           \
    } while (0)

#define TEST_FAST_COSF(result, iter_cnt_max)          \
    do {                                              \
        volatile float32_t __x = 0.785398F;           \
        TEST_OP(result, iter_cnt_max, "fast_cosf", {  \
            result.out.float_val = fast_cosf_rt(__x); \
        });                                           \
    } while (0)

#define TEST_FAST_TANF(result, iter_cnt_max)          \
    do {                                              \
        volatile float32_t __x = 0.785398F;           \
        TEST_OP(result, iter_cnt_max, "fast_tanf", {  \
            result.out.float_val = fast_tanf_rt(__x); \
        });                                           \
    } while (0)

#define TEST_ATAN2F_QUAD1(result, iter_cnt_max)         \
    do {                                                \
        volatile float32_t __y = 1.0F, __x = 1.0F;      \
        TEST_OP(result, iter_cnt_max, "atan2f_quad1", { \
            result.out.float_val = atan2f(__y, __x);    \
        });                                             \
    } while (0)

#define TEST_ATAN2F_QUAD2(result, iter_cnt_max)         \
    do {                                                \
        volatile float32_t __y = 1.0F, __x = -1.0F;     \
        TEST_OP(result, iter_cnt_max, "atan2f_quad2", { \
            result.out.float_val = atan2f(__y, __x);    \
        });                                             \
    } while (0)

#define TEST_ATAN2F_QUAD3(result, iter_cnt_max)         \
    do {                                                \
        volatile float32_t __y = -1.0F, __x = -1.0F;    \
        TEST_OP(result, iter_cnt_max, "atan2f_quad3", { \
            result.out.float_val = atan2f(__y, __x);    \
        });                                             \
    } while (0)

#define TEST_ATAN2F_QUAD4(result, iter_cnt_max)         \
    do {                                                \
        volatile float32_t __y = -1.0F, __x = 1.0F;     \
        TEST_OP(result, iter_cnt_max, "atan2f_quad4", { \
            result.out.float_val = atan2f(__y, __x);    \
        });                                             \
    } while (0)

// 其他数学函数
#define TEST_SQRTF(result, iter_cnt_max)         \
    do {                                         \
        volatile float32_t __x = 2.0F;           \
        TEST_OP(result, iter_cnt_max, "sqrtf", { \
            result.out.float_val = sqrtf(__x);   \
        });                                      \
    } while (0)

#define TEST_LOGF(result, iter_cnt_max)         \
    do {                                        \
        volatile float32_t __x = 2.0F;          \
        TEST_OP(result, iter_cnt_max, "logf", { \
            result.out.float_val = logf(__x);   \
        });                                     \
    } while (0)

#define TEST_EXPF(result, iter_cnt_max)         \
    do {                                        \
        volatile float32_t __x = 2.0F;          \
        TEST_OP(result, iter_cnt_max, "expf", { \
            result.out.float_val = expf(__x);   \
        });                                     \
    } while (0)

#define TEST_FAST_EXPF(result, iter_cnt_max)          \
    do {                                              \
        volatile float32_t __x = 2.0F;                \
        TEST_OP(result, iter_cnt_max, "fast_expf", {  \
            result.out.float_val = fast_expf_rt(__x); \
        });                                           \
    } while (0)

#define TEST_F16_TO_F32(result, iter_cnt_max)              \
    do {                                                   \
        volatile uint16_t __x = fast_f32_to_f16(114.514F); \
        TEST_OP(result, iter_cnt_max, "fast_f16_to_f32", { \
            result.out.float_val = fast_f16_to_f32(__x);   \
        });                                                \
    } while (0)

#define TEST_F32_TO_F16(result, iter_cnt_max)              \
    do {                                                   \
        volatile float32_t __x = 114.514F;                 \
        TEST_OP(result, iter_cnt_max, "fast_f32_to_f16", { \
            result.out.float_val = fast_f32_to_f16(__x);   \
        });                                                \
    } while (0)

#define RUN_MATH_BENCHMARK(result, iter_cnt_max)        \
    do {                                                \
        volatile uint32_t __idx = 0;                    \
        TEST_INT_ADD(result[__idx], iter_cnt_max);      \
        __idx++;                                        \
        TEST_INT_MUL(result[__idx], iter_cnt_max);      \
        __idx++;                                        \
        TEST_INT_DIV(result[__idx], iter_cnt_max);      \
        __idx++;                                        \
        TEST_FLOAT_ADD(result[__idx], iter_cnt_max);    \
        __idx++;                                        \
        TEST_FLOAT_MUL(result[__idx], iter_cnt_max);    \
        __idx++;                                        \
        TEST_FLOAT_DIV(result[__idx], iter_cnt_max);    \
        __idx++;                                        \
        TEST_SINF(result[__idx], iter_cnt_max);         \
        __idx++;                                        \
        TEST_COSF(result[__idx], iter_cnt_max);         \
        __idx++;                                        \
        TEST_TANF(result[__idx], iter_cnt_max);         \
        __idx++;                                        \
        TEST_ARM_SINF(result[__idx], iter_cnt_max);     \
        __idx++;                                        \
        TEST_ARM_COSF(result[__idx], iter_cnt_max);     \
        __idx++;                                        \
        TEST_FAST_SINF(result[__idx], iter_cnt_max);    \
        __idx++;                                        \
        TEST_FAST_COSF(result[__idx], iter_cnt_max);    \
        __idx++;                                        \
        TEST_FAST_TANF(result[__idx], iter_cnt_max);    \
        __idx++;                                        \
        TEST_ATAN2F_QUAD1(result[__idx], iter_cnt_max); \
        __idx++;                                        \
        TEST_ATAN2F_QUAD2(result[__idx], iter_cnt_max); \
        __idx++;                                        \
        TEST_ATAN2F_QUAD3(result[__idx], iter_cnt_max); \
        __idx++;                                        \
        TEST_ATAN2F_QUAD4(result[__idx], iter_cnt_max); \
        __idx++;                                        \
        TEST_SQRTF(result[__idx], iter_cnt_max);        \
        __idx++;                                        \
        TEST_LOGF(result[__idx], iter_cnt_max);         \
        __idx++;                                        \
        TEST_EXPF(result[__idx], iter_cnt_max);         \
        __idx++;                                        \
        TEST_FAST_EXPF(result[__idx], iter_cnt_max);    \
        __idx++;                                        \
        TEST_F16_TO_F32(result[__idx], iter_cnt_max);   \
        __idx++;                                        \
        TEST_F32_TO_F16(result[__idx], iter_cnt_max);   \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif
