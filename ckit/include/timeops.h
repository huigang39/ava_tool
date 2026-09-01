#ifndef TIMEOPS_H
#define TIMEOPS_H

#include "platdef.h"

#if OS(POSIX)
#include <unistd.h>
#elif OS(WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <time.h>

#include "macrodef.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIN_TO_UNIX_EPOCH (116444736000000000ULL)

#ifndef NANO_PER_SEC
#define NANO_PER_SEC (1000000000ULL)
#endif

#define NS2US(ns) ((ns) / 1000.0F)
#define NS2MS(ns) ((ns) / 1000000.0F)
#define NS2S(ns) ((ns) / 1000000000.0F)
#define US2NS(us) ((us) * 1000)
#define US2MS(us) ((us) / 1000.0F)
#define US2S(us) ((us) / 1000000.0F)
#define MS2NS(ms) ((ms) * 1000000)
#define MS2US(ms) ((ms) * 1000)
#define MS2S(ms) ((ms) / 1000.0F)
#define S2NS(s) ((s) * 1000000000)
#define S2US(s) ((s) * 1000000)
#define S2MS(s) ((s) * 1000)

#define HZ2S(hz) (1.0F / (hz))
#define HZ2MS(hz) (1.0F / (hz) * 1000)
#define HZ2US(hz) (1.0F / (hz) * 1000000)

#define S2CNT(s, hz) ((s) * (hz))
#define MS2CNT(ms, hz) ((ms) * (hz) / 1000.0F)

#define TIMED_EXEC(ret, period_us, code)                        \
    do {                                                        \
        const uint64_t __start = get_mono_ts_us();              \
        {code};                                                 \
        const uint64_t __elapsed = get_mono_ts_us() - __start;  \
        (ret) = __elapsed;                                      \
        if (__elapsed < (period_us)) {                          \
            const uint64_t remaining = (period_us) - __elapsed; \
            delay_us(remaining);                                \
        }                                                       \
    } while (0)

/**
 * @brief  获取单调时间戳, 单位: ns
 * @return 对应时钟的时间戳, 单位由函数后缀指定
 */
HAPI uint64_t get_mono_ts_ns(void)
{
#if OS(POSIX)
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return ts.tv_sec * NANO_PER_SEC + ts.tv_nsec;
#elif OS(WIN)
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    uint64_t q = (uint64_t)counter.QuadPart / (uint64_t)freq.QuadPart;
    uint64_t r = (uint64_t)counter.QuadPart % (uint64_t)freq.QuadPart;
    return q * 1000000000ULL + (r * 1000000000ULL) / (uint64_t)freq.QuadPart;
#endif
    return 0;
}

/**
 * @brief  获取单调时间戳, 单位: us
 * @return 对应时钟的时间戳, 单位由函数后缀指定
 */
HAPI uint64_t get_mono_ts_us(void)
{
    return get_mono_ts_ns() / 1000;
}

/**
 * @brief  获取单调时间戳, 单位: ms
 * @return 对应时钟的时间戳, 单位由函数后缀指定
 */
HAPI uint64_t get_mono_ts_ms(void)
{
    return get_mono_ts_ns() / 1000000;
}

/**
 * @brief  获取单调时间戳, 单位: s
 * @return 对应时钟的时间戳, 单位由函数后缀指定
 */
HAPI uint64_t get_mono_ts_s(void)
{
    return get_mono_ts_ns() / 1000000000;
}

/**
 * @brief  获取实时时钟时间戳, 单位: ns
 * @return 对应时钟的时间戳, 单位由函数后缀指定
 */
HAPI uint64_t get_real_ts_ns(void)
{
#if OS(POSIX)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * NANO_PER_SEC + ts.tv_nsec;
#elif OS(WIN)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;

    return (uli.QuadPart - WIN_TO_UNIX_EPOCH) * 100;
#endif
    return 0;
}

/**
 * @brief  获取实时时钟时间戳, 单位: us
 * @return 对应时钟的时间戳, 单位由函数后缀指定
 */
HAPI uint64_t get_real_ts_us(void)
{
    return get_real_ts_ns() / 1000;
}

/**
 * @brief  获取实时时钟时间戳, 单位: ms
 * @return 对应时钟的时间戳, 单位由函数后缀指定
 */
HAPI uint64_t get_real_ts_ms(void)
{
    return get_real_ts_ns() / 1000000;
}

/**
 * @brief  获取实时时钟时间戳, 单位: s
 * @return 对应时钟的时间戳, 单位由函数后缀指定
 */
HAPI uint64_t get_real_ts_s(void)
{
    return get_real_ts_ns() / 1000000000;
}

enum delay {
    DELAY_SPIN,  // 忙等待延时
    DELAY_YIELD, // 让出处理器的延时方式
};

/**
 * @brief        忙等待指定时间
 * @param[in] us 等待时长, 单位: us
 */
HAPI void spin(uint32_t us)
{
    uint64_t start = get_mono_ts_us();
    while ((get_mono_ts_us() - start) < us)
        ;
}

#if OS(POSIX)
/**
 * @brief        延时并在支持的平台上让出处理器
 * @param[in] ms 等待时长, 单位: ms
 */
HAPI void yield(const uint32_t ms)
{
    usleep((uint32_t)(ms) * 1000);
}
#elif OS(WIN)
/**
 * @brief        延时并在支持的平台上让出处理器
 * @param[in] ms 等待时长, 单位: ms
 */
HAPI void yield(const uint32_t ms)
{
    Sleep(ms);
}
#else
/**
 * @brief        延时并在支持的平台上让出处理器
 * @param[in] ms 等待时长, 单位: ms
 */
HAPI void yield(const uint32_t ms)
{
    spin(MS2US(ms));
}
#endif

/**
 * @brief        按 us 忙等待
 * @param[in] us 等待时长, 单位: us
 */
HAPI void delay_us(const uint64_t us)
{
    spin((uint32_t)us);
}

/**
 * @brief           按指定方式延时, 单位: ms
 * @param[in] ms    等待时长, 单位: ms
 * @param[in] delay 延时方式
 */
HAPI void delay_ms(const uint64_t ms, const enum delay delay)
{
    switch (delay) {
    case DELAY_SPIN: {
        spin((uint32_t)MS2US(ms));
        break;
    }
    case DELAY_YIELD: {
        yield((uint32_t)ms);
        break;
    }
    default: {
        break;
    }
    }
}

/**
 * @brief           按指定方式延时, 单位: s
 * @param[in] s     等待时长, 单位: s
 * @param[in] delay 延时方式
 */
HAPI void delay_s(const uint64_t s, const enum delay delay)
{
    switch (delay) {
    case DELAY_SPIN: {
        spin((uint32_t)S2US(s));
        break;
    }
    case DELAY_YIELD: {
        yield((uint32_t)S2MS(s));
        break;
    }
    default: {
        break;
    }
    }
}

#ifdef __cplusplus
}
#endif

#endif
