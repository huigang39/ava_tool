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
        DWT->CYCCNT       = 0;                          \
        DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;     \
    } while (0)

static inline uint32_t
benchmark_read_cyccnt_rt(void)
{
#if defined(__arm__) || defined(__thumb__) || defined(__ARM_ARCH)
    return *(volatile const uint32_t *)0xE0001004UL;
#else
    return 0;
#endif
}

#define MEASURE_TIME(total_cyccnt, iter_cnt_max, code)    \
    do {                                                  \
        uint32_t __start, __end, __single;                \
        (total_cyccnt) = 0;                               \
        for (uint32_t i = 0; i < (iter_cnt_max); i++) {   \
            __start = benchmark_read_cyccnt_rt();         \
            {code};                                       \
            __end           = benchmark_read_cyccnt_rt(); \
            __single        = __end - __start;            \
            (total_cyccnt) += __single;                   \
        }                                                 \
    } while (0)

#define CYCCNT2US(cyccnt) ((cyccnt) * (1.0F / (HAL_RCC_GetSysClockFreq() / M(1.0F))))

struct benchmark {
    const char *name;
    uint32_t    total_cyccnt, single_cyccnt;
    float32_t   total_elapsed_us, single_elapsed_us;
    union {
        float32_t float_value;
        int32_t   int_value;
    } ret;
};

#define TEST_OP(res, iter_cnt_max, op_name, op_code)                 \
    do {                                                             \
        res.name = op_name;                                          \
        MEASURE_TIME(res.total_cyccnt, iter_cnt_max, op_code);       \
        res.single_cyccnt     = res.total_cyccnt / iter_cnt_max;     \
        res.total_elapsed_us  = CYCCNT2US(res.total_cyccnt);         \
        res.single_elapsed_us = res.total_elapsed_us / iter_cnt_max; \
    } while (0)

/* 整数运算 */
#define TEST_INT_ADD(res, iter_cnt_max)                                            \
    do {                                                                           \
        volatile int __a = 123456, __b = 789;                                      \
        TEST_OP(res, iter_cnt_max, "int_add", { res.ret.int_value = __a + __b; }); \
    } while (0)

#define TEST_INT_MUL(res, iter_cnt_max)                                            \
    do {                                                                           \
        volatile int __a = 123456, __b = 789;                                      \
        TEST_OP(res, iter_cnt_max, "int_mul", { res.ret.int_value = __a * __b; }); \
    } while (0)

#define TEST_INT_DIV(res, iter_cnt_max)                                            \
    do {                                                                           \
        volatile int __a = 123456, __b = 789;                                      \
        TEST_OP(res, iter_cnt_max, "int_div", { res.ret.int_value = __a / __b; }); \
    } while (0)

/* 浮点运算 */
#define TEST_FLOAT_ADD(res, iter_cnt_max)                                              \
    do {                                                                               \
        volatile float32_t __a = 123.456F, __b = 7.89F;                                \
        TEST_OP(res, iter_cnt_max, "float_add", { res.ret.float_value = __a + __b; }); \
    } while (0)

#define TEST_FLOAT_MUL(res, iter_cnt_max)                                              \
    do {                                                                               \
        volatile float32_t __a = 123.456F, __b = 7.89F;                                \
        TEST_OP(res, iter_cnt_max, "float_mul", { res.ret.float_value = __a * __b; }); \
    } while (0)

#define TEST_FLOAT_DIV(res, iter_cnt_max)                                              \
    do {                                                                               \
        volatile float32_t __a = 123.456F, __b = 7.89F;                                \
        TEST_OP(res, iter_cnt_max, "float_div", { res.ret.float_value = __a / __b; }); \
    } while (0)

/* 三角函数 */
#define TEST_SINF(res, iter_cnt_max)                                              \
    do {                                                                          \
        volatile float32_t __x = 0.785398F;                                       \
        TEST_OP(res, iter_cnt_max, "sinf", { res.ret.float_value = sinf(__x); }); \
    } while (0)

#define TEST_COSF(res, iter_cnt_max)                                              \
    do {                                                                          \
        volatile float32_t __x = 0.785398F;                                       \
        TEST_OP(res, iter_cnt_max, "cosf", { res.ret.float_value = cosf(__x); }); \
    } while (0)

#define TEST_TANF(res, iter_cnt_max)                                              \
    do {                                                                          \
        volatile float32_t __x = 0.785398F;                                       \
        TEST_OP(res, iter_cnt_max, "tanf", { res.ret.float_value = tanf(__x); }); \
    } while (0)

#ifdef ARM_MATH
#define TEST_ARM_SINF(res, iter_cnt_max)                                                     \
    do {                                                                                     \
        volatile float32_t __x = 0.785398F;                                                  \
        TEST_OP(res, iter_cnt_max, "arm_sinf", { res.ret.float_value = arm_sin_f32(__x); }); \
    } while (0)

#define TEST_ARM_COSF(res, iter_cnt_max)                                                     \
    do {                                                                                     \
        volatile float32_t __x = 0.785398F;                                                  \
        TEST_OP(res, iter_cnt_max, "arm_cosf", { res.ret.float_value = arm_cos_f32(__x); }); \
    } while (0)
#else
#define TEST_ARM_SINF(res, iter_cnt_max)
#define TEST_ARM_COSF(res, iter_cnt_max)
#endif

#define TEST_FAST_SINF(res, iter_cnt_max)                                                      \
    do {                                                                                       \
        volatile float32_t __x = 0.785398F;                                                    \
        TEST_OP(res, iter_cnt_max, "fast_sinf", { res.ret.float_value = fast_sinf_rt(__x); }); \
    } while (0)

#define TEST_FAST_COSF(res, iter_cnt_max)                                                      \
    do {                                                                                       \
        volatile float32_t __x = 0.785398F;                                                    \
        TEST_OP(res, iter_cnt_max, "fast_cosf", { res.ret.float_value = fast_cosf_rt(__x); }); \
    } while (0)

#define TEST_FAST_TANF(res, iter_cnt_max)                                                      \
    do {                                                                                       \
        volatile float32_t __x = 0.785398F;                                                    \
        TEST_OP(res, iter_cnt_max, "fast_tanf", { res.ret.float_value = fast_tanf_rt(__x); }); \
    } while (0)

#define TEST_ATAN2F_QUAD1(res, iter_cnt_max)                                                     \
    do {                                                                                         \
        volatile float32_t __y = 1.0F, __x = 1.0F;                                               \
        TEST_OP(res, iter_cnt_max, "atan2f_quad1", { res.ret.float_value = atan2f(__y, __x); }); \
    } while (0)

#define TEST_ATAN2F_QUAD2(res, iter_cnt_max)                                                     \
    do {                                                                                         \
        volatile float32_t __y = 1.0F, __x = -1.0F;                                              \
        TEST_OP(res, iter_cnt_max, "atan2f_quad2", { res.ret.float_value = atan2f(__y, __x); }); \
    } while (0)

#define TEST_ATAN2F_QUAD3(res, iter_cnt_max)                                                     \
    do {                                                                                         \
        volatile float32_t __y = -1.0F, __x = -1.0F;                                             \
        TEST_OP(res, iter_cnt_max, "atan2f_quad3", { res.ret.float_value = atan2f(__y, __x); }); \
    } while (0)

#define TEST_ATAN2F_QUAD4(res, iter_cnt_max)                                                     \
    do {                                                                                         \
        volatile float32_t __y = -1.0F, __x = 1.0F;                                              \
        TEST_OP(res, iter_cnt_max, "atan2f_quad4", { res.ret.float_value = atan2f(__y, __x); }); \
    } while (0)

/* 其他数学函数 */
#define TEST_SQRTF(res, iter_cnt_max)                                               \
    do {                                                                            \
        volatile float32_t __x = 2.0F;                                              \
        TEST_OP(res, iter_cnt_max, "sqrtf", { res.ret.float_value = sqrtf(__x); }); \
    } while (0)

#define TEST_LOGF(res, iter_cnt_max)                                              \
    do {                                                                          \
        volatile float32_t __x = 2.0F;                                            \
        TEST_OP(res, iter_cnt_max, "logf", { res.ret.float_value = logf(__x); }); \
    } while (0)

#define TEST_EXPF(res, iter_cnt_max)                                              \
    do {                                                                          \
        volatile float32_t __x = 2.0F;                                            \
        TEST_OP(res, iter_cnt_max, "expf", { res.ret.float_value = expf(__x); }); \
    } while (0)

#define TEST_FAST_EXPF(res, iter_cnt_max)                                                      \
    do {                                                                                       \
        volatile float32_t __x = 2.0F;                                                         \
        TEST_OP(res, iter_cnt_max, "fast_expf", { res.ret.float_value = fast_expf_rt(__x); }); \
    } while (0)

#define TEST_F16_TO_F32(res, iter_cnt_max)                 \
    do {                                                   \
        volatile uint16_t __x = fast_f32_to_f16(114.514F); \
        TEST_OP(res, iter_cnt_max, "fast_f16_to_f32", {    \
            res.ret.float_value = fast_f16_to_f32(__x);    \
        });                                                \
    } while (0)

#define TEST_F32_TO_F16(res, iter_cnt_max)              \
    do {                                                \
        volatile float32_t __x = 114.514F;              \
        TEST_OP(res, iter_cnt_max, "fast_f32_to_f16", { \
            res.ret.float_value = fast_f32_to_f16(__x); \
        });                                             \
    } while (0)

#define RUN_MATH_BENCHMARK(res, iter_cnt_max)        \
    do {                                             \
        volatile uint32_t __idx = 0;                 \
        TEST_INT_ADD(res[__idx], iter_cnt_max);      \
        __idx++;                                     \
        TEST_INT_MUL(res[__idx], iter_cnt_max);      \
        __idx++;                                     \
        TEST_INT_DIV(res[__idx], iter_cnt_max);      \
        __idx++;                                     \
        TEST_FLOAT_ADD(res[__idx], iter_cnt_max);    \
        __idx++;                                     \
        TEST_FLOAT_MUL(res[__idx], iter_cnt_max);    \
        __idx++;                                     \
        TEST_FLOAT_DIV(res[__idx], iter_cnt_max);    \
        __idx++;                                     \
        TEST_SINF(res[__idx], iter_cnt_max);         \
        __idx++;                                     \
        TEST_COSF(res[__idx], iter_cnt_max);         \
        __idx++;                                     \
        TEST_TANF(res[__idx], iter_cnt_max);         \
        __idx++;                                     \
        TEST_ARM_SINF(res[__idx], iter_cnt_max);     \
        __idx++;                                     \
        TEST_ARM_COSF(res[__idx], iter_cnt_max);     \
        __idx++;                                     \
        TEST_FAST_SINF(res[__idx], iter_cnt_max);    \
        __idx++;                                     \
        TEST_FAST_COSF(res[__idx], iter_cnt_max);    \
        __idx++;                                     \
        TEST_FAST_TANF(res[__idx], iter_cnt_max);    \
        __idx++;                                     \
        TEST_ATAN2F_QUAD1(res[__idx], iter_cnt_max); \
        __idx++;                                     \
        TEST_ATAN2F_QUAD2(res[__idx], iter_cnt_max); \
        __idx++;                                     \
        TEST_ATAN2F_QUAD3(res[__idx], iter_cnt_max); \
        __idx++;                                     \
        TEST_ATAN2F_QUAD4(res[__idx], iter_cnt_max); \
        __idx++;                                     \
        TEST_SQRTF(res[__idx], iter_cnt_max);        \
        __idx++;                                     \
        TEST_LOGF(res[__idx], iter_cnt_max);         \
        __idx++;                                     \
        TEST_EXPF(res[__idx], iter_cnt_max);         \
        __idx++;                                     \
        TEST_FAST_EXPF(res[__idx], iter_cnt_max);    \
        __idx++;                                     \
        TEST_F16_TO_F32(res[__idx], iter_cnt_max);   \
        __idx++;                                     \
        TEST_F32_TO_F16(res[__idx], iter_cnt_max);   \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif // !BENCHMARK_H
