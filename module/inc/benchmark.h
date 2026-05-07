#ifndef BENCHMARK_H
#define BENCHMARK_H

#ifdef ARM_MATH
#include "arm_math.h"
#endif

#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DWT_INIT()                                              \
        do {                                                    \
                CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; \
                DWT->CYCCNT       = 0;                          \
                DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;     \
        } while (0)

#define MEASURE_TIME(total_cyccnt, iter_cnt_max, code)                   \
        do {                                                             \
                u32 __start, __end, __single;                            \
                (total_cyccnt) = 0;                                      \
                for (u32 i = 0; i < (iter_cnt_max); i++) {               \
                        DWT->CYCCNT = 0;                                 \
                        __start     = DWT->CYCCNT;                       \
                        {code};                                          \
                        __end = DWT->CYCCNT;                             \
                        if (__end < __start)                             \
                                __single = 0xFFFFFFFF - __start + __end; \
                        else                                             \
                                __single = __end - __start;              \
                        (total_cyccnt) += __single;                      \
                }                                                        \
        } while (0)

#define CYCCNT2US(cyccnt) ((cyccnt) * (1.0F / (HAL_RCC_GetSysClockFreq() / M(1.0F))))

typedef struct {
        const char *name;
        u32         total_cyccnt, single_cyccnt;
        f32         total_elapsed_us, single_elapsed_us;
        f32         ret;
} benchmark_t;

#define TEST_OP(res, iter_cnt_max, op_name, op_code)                         \
        do {                                                                 \
                res.name = op_name;                                          \
                MEASURE_TIME(res.total_cyccnt, iter_cnt_max, op_code);       \
                res.single_cyccnt     = res.total_cyccnt / iter_cnt_max;     \
                res.total_elapsed_us  = CYCCNT2US(res.total_cyccnt);         \
                res.single_elapsed_us = res.total_elapsed_us / iter_cnt_max; \
        } while (0)

/* 整数运算 */
#define TEST_INT_ADD(res, iter_cnt_max)                                          \
        do {                                                                     \
                volatile int __a = 123456, __b = 789;                            \
                TEST_OP(res, iter_cnt_max, "int_add", { res.ret = __a + __b; }); \
        } while (0)

#define TEST_INT_MUL(res, iter_cnt_max)                                          \
        do {                                                                     \
                volatile int __a = 123456, __b = 789;                            \
                TEST_OP(res, iter_cnt_max, "int_mul", { res.ret = __a * __b; }); \
        } while (0)

#define TEST_INT_DIV(res, iter_cnt_max)                                          \
        do {                                                                     \
                volatile int __a = 123456, __b = 789;                            \
                TEST_OP(res, iter_cnt_max, "int_div", { res.ret = __a / __b; }); \
        } while (0)

/* 浮点运算 */
#define TEST_FLOAT_ADD(res, iter_cnt_max)                                          \
        do {                                                                       \
                volatile f32 __a = 123.456f, __b = 7.89f;                          \
                TEST_OP(res, iter_cnt_max, "float_add", { res.ret = __a + __b; }); \
        } while (0)

#define TEST_FLOAT_MUL(res, iter_cnt_max)                                          \
        do {                                                                       \
                volatile f32 __a = 123.456f, __b = 7.89f;                          \
                TEST_OP(res, iter_cnt_max, "float_mul", { res.ret = __a * __b; }); \
        } while (0)

#define TEST_FLOAT_DIV(res, iter_cnt_max)                                          \
        do {                                                                       \
                volatile f32 __a = 123.456f, __b = 7.89f;                          \
                TEST_OP(res, iter_cnt_max, "float_div", { res.ret = __a / __b; }); \
        } while (0)

/* 三角函数 */
#define TEST_SINF(res, iter_cnt_max)                                          \
        do {                                                                  \
                volatile f32 __x = 0.785398f;                                 \
                TEST_OP(res, iter_cnt_max, "sinf", { res.ret = sinf(__x); }); \
        } while (0)

#define TEST_COSF(res, iter_cnt_max)                                          \
        do {                                                                  \
                volatile f32 __x = 0.785398f;                                 \
                TEST_OP(res, iter_cnt_max, "cosf", { res.ret = cosf(__x); }); \
        } while (0)

#define TEST_TANF(res, iter_cnt_max)                                          \
        do {                                                                  \
                volatile f32 __x = 0.785398f;                                 \
                TEST_OP(res, iter_cnt_max, "tanf", { res.ret = tanf(__x); }); \
        } while (0)

#ifdef ARM_MATH
#define TEST_ARM_SINF(res, iter_cnt_max)                                                 \
        do {                                                                             \
                volatile f32 __x = 0.785398f;                                            \
                TEST_OP(res, iter_cnt_max, "arm_sinf", { res.ret = arm_sin_f32(__x); }); \
        } while (0)

#define TEST_ARM_COSF(res, iter_cnt_max)                                                 \
        do {                                                                             \
                volatile f32 __x = 0.785398f;                                            \
                TEST_OP(res, iter_cnt_max, "arm_cosf", { res.ret = arm_cos_f32(__x); }); \
        } while (0)
#else
#define TEST_ARM_SINF(res, iter_cnt_max)
#define TEST_ARM_COSF(res, iter_cnt_max)
#endif

#define TEST_FAST_SINF(res, iter_cnt_max)                                               \
        do {                                                                            \
                volatile f32 __x = 0.785398f;                                           \
                TEST_OP(res, iter_cnt_max, "fast_sinf", { res.ret = fast_sinf(__x); }); \
        } while (0)

#define TEST_FAST_COSF(res, iter_cnt_max)                                               \
        do {                                                                            \
                volatile f32 __x = 0.785398f;                                           \
                TEST_OP(res, iter_cnt_max, "fast_cosf", { res.ret = fast_cosf(__x); }); \
        } while (0)

#define TEST_FAST_TANF(res, iter_cnt_max)                                               \
        do {                                                                            \
                volatile f32 __x = 0.785398f;                                           \
                TEST_OP(res, iter_cnt_max, "fast_tanf", { res.ret = fast_tanf(__x); }); \
        } while (0)

#define TEST_ATAN2F_QUAD1(res, iter_cnt_max)                                                 \
        do {                                                                                 \
                volatile f32 __y = 1.0f, __x = 1.0f;                                         \
                TEST_OP(res, iter_cnt_max, "atan2f_quad1", { res.ret = atan2f(__y, __x); }); \
        } while (0)

#define TEST_ATAN2F_QUAD2(res, iter_cnt_max)                                                 \
        do {                                                                                 \
                volatile f32 __y = 1.0f, __x = -1.0f;                                        \
                TEST_OP(res, iter_cnt_max, "atan2f_quad2", { res.ret = atan2f(__y, __x); }); \
        } while (0)

#define TEST_ATAN2F_QUAD3(res, iter_cnt_max)                                                 \
        do {                                                                                 \
                volatile f32 __y = -1.0f, __x = -1.0f;                                       \
                TEST_OP(res, iter_cnt_max, "atan2f_quad3", { res.ret = atan2f(__y, __x); }); \
        } while (0)

#define TEST_ATAN2F_QUAD4(res, iter_cnt_max)                                                 \
        do {                                                                                 \
                volatile f32 __y = -1.0f, __x = 1.0f;                                        \
                TEST_OP(res, iter_cnt_max, "atan2f_quad4", { res.ret = atan2f(__y, __x); }); \
        } while (0)

/* 其他数学函数 */
#define TEST_SQRTF(res, iter_cnt_max)                                           \
        do {                                                                    \
                volatile f32 __x = 2.0f;                                        \
                TEST_OP(res, iter_cnt_max, "sqrtf", { res.ret = sqrtf(__x); }); \
        } while (0)

#define TEST_LOGF(res, iter_cnt_max)                                          \
        do {                                                                  \
                volatile f32 __x = 2.0f;                                      \
                TEST_OP(res, iter_cnt_max, "logf", { res.ret = logf(__x); }); \
        } while (0)

#define TEST_EXPF(res, iter_cnt_max)                                          \
        do {                                                                  \
                volatile f32 __x = 2.0f;                                      \
                TEST_OP(res, iter_cnt_max, "expf", { res.ret = expf(__x); }); \
        } while (0)

#define TEST_FAST_EXPF(res, iter_cnt_max)                                               \
        do {                                                                            \
                volatile f32 __x = 2.0f;                                                \
                TEST_OP(res, iter_cnt_max, "fast_expf", { res.ret = fast_expf(__x); }); \
        } while (0)

#define TEST_F16_TO_F32(res, iter_cnt_max)                                                          \
        do {                                                                                        \
                volatile f16 __x = 114.514f;                                                        \
                TEST_OP(res, iter_cnt_max, "fast_f16_to_f32", { res.ret = fast_f16_to_f32(__x); }); \
        } while (0)

#define TEST_F32_TO_F16(res, iter_cnt_max)                                                          \
        do {                                                                                        \
                volatile f32 __x = 114.514f;                                                        \
                TEST_OP(res, iter_cnt_max, "fast_f32_to_f16", { res.ret = fast_f32_to_f16(__x); }); \
        } while (0)

#define RUN_MATH_BENCHMARK(res, iter_cnt_max)                \
        do {                                                 \
                volatile u32 __idx = 0;                      \
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
