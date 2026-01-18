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

#define MEASURE_TIME(total_cyccnt, iter_cnt_max, code)             \
        do {                                                       \
                u32 cnt = (iter_cnt_max);                          \
                u32 start, end, single;                            \
                (total_cyccnt) = 0;                                \
                for (u32 i = 0; i < cnt; i++) {                    \
                        DWT->CYCCNT = 0;                           \
                        start       = DWT->CYCCNT;                 \
                        {code};                                    \
                        end = DWT->CYCCNT;                         \
                        if (end < start)                           \
                                single = 0xFFFFFFFF - start + end; \
                        else                                       \
                                single = end - start;              \
                        (total_cyccnt) += single;                  \
                }                                                  \
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
                cnt++;                                                       \
        } while (0)

/* 整数运算 */
#define TEST_INT_ADD(res, iter_cnt_max)                                      \
        do {                                                                 \
                volatile int a = 123456, b = 789;                            \
                TEST_OP(res, iter_cnt_max, "int_add", { res.ret = a + b; }); \
        } while (0)

#define TEST_INT_MUL(res, iter_cnt_max)                                      \
        do {                                                                 \
                volatile int a = 123456, b = 789;                            \
                TEST_OP(res, iter_cnt_max, "int_mul", { res.ret = a * b; }); \
        } while (0)

#define TEST_INT_DIV(res, iter_cnt_max)                                      \
        do {                                                                 \
                volatile int a = 123456, b = 789;                            \
                TEST_OP(res, iter_cnt_max, "int_div", { res.ret = a / b; }); \
        } while (0)

/* 浮点运算 */
#define TEST_FLOAT_ADD(res, iter_cnt_max)                                      \
        do {                                                                   \
                volatile f32 a = 123.456f, b = 7.89f;                          \
                TEST_OP(res, iter_cnt_max, "float_add", { res.ret = a + b; }); \
        } while (0)

#define TEST_FLOAT_MUL(res, iter_cnt_max)                                      \
        do {                                                                   \
                volatile f32 a = 123.456f, b = 7.89f;                          \
                TEST_OP(res, iter_cnt_max, "float_mul", { res.ret = a * b; }); \
        } while (0)

#define TEST_FLOAT_DIV(res, iter_cnt_max)                                      \
        do {                                                                   \
                volatile f32 a = 123.456f, b = 7.89f;                          \
                TEST_OP(res, iter_cnt_max, "float_div", { res.ret = a / b; }); \
        } while (0)

/* 三角函数 */
#define TEST_SINF(res, iter_cnt_max)                                        \
        do {                                                                \
                volatile f32 x = 0.785398f;                                 \
                TEST_OP(res, iter_cnt_max, "sinf", { res.ret = sinf(x); }); \
        } while (0)

#define TEST_COSF(res, iter_cnt_max)                                        \
        do {                                                                \
                volatile f32 x = 0.785398f;                                 \
                TEST_OP(res, iter_cnt_max, "cosf", { res.ret = cosf(x); }); \
        } while (0)

#define TEST_TANF(res, iter_cnt_max)                                        \
        do {                                                                \
                volatile f32 x = 0.785398f;                                 \
                TEST_OP(res, iter_cnt_max, "tanf", { res.ret = tanf(x); }); \
        } while (0)

#ifdef ARM_MATH
#define TEST_ARM_SINF(res, iter_cnt_max)                                               \
        do {                                                                           \
                volatile f32 x = 0.785398f;                                            \
                TEST_OP(res, iter_cnt_max, "arm_sinf", { res.ret = arm_sin_f32(x); }); \
        } while (0)

#define TEST_ARM_COSF(res, iter_cnt_max)                                               \
        do {                                                                           \
                volatile f32 x = 0.785398f;                                            \
                TEST_OP(res, iter_cnt_max, "arm_cosf", { res.ret = arm_cos_f32(x); }); \
        } while (0)
#else
#define TEST_ARM_SINF(res, iter_cnt_max)
#define TEST_ARM_COSF(res, iter_cnt_max)
#endif

#define TEST_FAST_SINF(res, iter_cnt_max)                                             \
        do {                                                                          \
                volatile f32 x = 0.785398f;                                           \
                TEST_OP(res, iter_cnt_max, "fast_sinf", { res.ret = fast_sinf(x); }); \
        } while (0)

#define TEST_FAST_COSF(res, iter_cnt_max)                                             \
        do {                                                                          \
                volatile f32 x = 0.785398f;                                           \
                TEST_OP(res, iter_cnt_max, "fast_cosf", { res.ret = fast_cosf(x); }); \
        } while (0)

#define TEST_FAST_TANF(res, iter_cnt_max)                                             \
        do {                                                                          \
                volatile f32 x = 0.785398f;                                           \
                TEST_OP(res, iter_cnt_max, "fast_tanf", { res.ret = fast_tanf(x); }); \
        } while (0)

#define TEST_ATAN2F_QUAD1(res, iter_cnt_max)                                             \
        do {                                                                             \
                volatile f32 y = 1.0f, x = 1.0f;                                         \
                TEST_OP(res, iter_cnt_max, "atan2f_quad1", { res.ret = atan2f(y, x); }); \
        } while (0)

#define TEST_ATAN2F_QUAD2(res, iter_cnt_max)                                             \
        do {                                                                             \
                volatile f32 y = 1.0f, x = -1.0f;                                        \
                TEST_OP(res, iter_cnt_max, "atan2f_quad2", { res.ret = atan2f(y, x); }); \
        } while (0)

#define TEST_ATAN2F_QUAD3(res, iter_cnt_max)                                             \
        do {                                                                             \
                volatile f32 y = -1.0f, x = -1.0f;                                       \
                TEST_OP(res, iter_cnt_max, "atan2f_quad3", { res.ret = atan2f(y, x); }); \
        } while (0)

#define TEST_ATAN2F_QUAD4(res, iter_cnt_max)                                             \
        do {                                                                             \
                volatile f32 y = -1.0f, x = 1.0f;                                        \
                TEST_OP(res, iter_cnt_max, "atan2f_quad4", { res.ret = atan2f(y, x); }); \
        } while (0)

/* 其他数学函数 */
#define TEST_SQRTF(res, iter_cnt_max)                                         \
        do {                                                                  \
                volatile f32 x = 2.0f;                                        \
                TEST_OP(res, iter_cnt_max, "sqrtf", { res.ret = sqrtf(x); }); \
        } while (0)

#define TEST_LOGF(res, iter_cnt_max)                                        \
        do {                                                                \
                volatile f32 x = 2.0f;                                      \
                TEST_OP(res, iter_cnt_max, "logf", { res.ret = logf(x); }); \
        } while (0)

#define TEST_EXPF(res, iter_cnt_max)                                        \
        do {                                                                \
                volatile f32 x = 2.0f;                                      \
                TEST_OP(res, iter_cnt_max, "expf", { res.ret = expf(x); }); \
        } while (0)

#define TEST_FAST_EXPF(res, iter_cnt_max)                                             \
        do {                                                                          \
                volatile f32 x = 2.0f;                                                \
                TEST_OP(res, iter_cnt_max, "fast_expf", { res.ret = fast_expf(x); }); \
        } while (0)

#define TEST_F16_TO_F32(res, iter_cnt_max)                                                        \
        do {                                                                                      \
                volatile f16 x = 114.514f;                                                        \
                TEST_OP(res, iter_cnt_max, "fast_f16_to_f32", { res.ret = fast_f16_to_f32(x); }); \
        } while (0)

#define TEST_F32_TO_F16(res, iter_cnt_max)                                                        \
        do {                                                                                      \
                volatile f32 x = 114.514f;                                                        \
                TEST_OP(res, iter_cnt_max, "fast_f32_to_f16", { res.ret = fast_f32_to_f16(x); }); \
        } while (0)

#define RUN_MATH_BENCHMARK(res, iter_cnt_max)              \
        do {                                               \
                volatile u32 cnt = 0;                      \
                TEST_INT_ADD(res[cnt], iter_cnt_max);      \
                TEST_INT_MUL(res[cnt], iter_cnt_max);      \
                TEST_INT_DIV(res[cnt], iter_cnt_max);      \
                TEST_FLOAT_ADD(res[cnt], iter_cnt_max);    \
                TEST_FLOAT_MUL(res[cnt], iter_cnt_max);    \
                TEST_FLOAT_DIV(res[cnt], iter_cnt_max);    \
                TEST_SINF(res[cnt], iter_cnt_max);         \
                TEST_COSF(res[cnt], iter_cnt_max);         \
                TEST_TANF(res[cnt], iter_cnt_max);         \
                TEST_ARM_SINF(res[cnt], iter_cnt_max);     \
                TEST_ARM_COSF(res[cnt], iter_cnt_max);     \
                TEST_FAST_SINF(res[cnt], iter_cnt_max);    \
                TEST_FAST_COSF(res[cnt], iter_cnt_max);    \
                TEST_FAST_TANF(res[cnt], iter_cnt_max);    \
                TEST_ATAN2F_QUAD1(res[cnt], iter_cnt_max); \
                TEST_ATAN2F_QUAD2(res[cnt], iter_cnt_max); \
                TEST_ATAN2F_QUAD3(res[cnt], iter_cnt_max); \
                TEST_ATAN2F_QUAD4(res[cnt], iter_cnt_max); \
                TEST_SQRTF(res[cnt], iter_cnt_max);        \
                TEST_LOGF(res[cnt], iter_cnt_max);         \
                TEST_EXPF(res[cnt], iter_cnt_max);         \
                TEST_FAST_EXPF(res[cnt], iter_cnt_max);    \
                TEST_F16_TO_F32(res[cnt], iter_cnt_max);   \
                TEST_F32_TO_F16(res[cnt], iter_cnt_max);   \
        } while (0)

#ifdef __cplusplus
}
#endif

#endif // !BENCHMARK_H
