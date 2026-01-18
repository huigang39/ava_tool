#ifndef TIMEOPS_H
#define TIMEOPS_H

#ifdef __linux__
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include <time.h>

#include "macrodef.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NANO_PER_SEC      (1000000000) // 10^9
#define MICRO_PER_SEC     (1000000)    // 10^6
#define MILLI_PER_SEC     (1000)       // 10^3

#define WIN_TO_UNIX_EPOCH (116444736000000000ULL)

#define NS2US(ns)         ((ns) / 1000.0F)
#define NS2MS(ns)         ((ns) / 1000000.0F)
#define NS2S(ns)          ((ns) / 1000000000.0F)
#define US2NS(us)         ((us) * 1000)
#define US2MS(us)         ((us) / 1000.0F)
#define US2S(us)          ((us) / 1000000.0F)
#define MS2NS(ms)         ((ms) * 1000000)
#define MS2US(ms)         ((ms) * 1000)
#define MS2S(ms)          ((ms) / 1000.0F)
#define S2NS(s)           ((s) * 1000000000)
#define S2US(s)           ((s) * 1000000)
#define S2MS(s)           ((s) * 1000)

#define HZ2S(hz)          (1.0F / (hz))
#define HZ2MS(hz)         (1.0F / (hz) * 1000)
#define HZ2US(hz)         (1.0F / (hz) * 1000000)

#define S2CNT(s, hz)      ((s) / HZ2S(hz))
#define MS2CNT(ms, hz)    ((ms) / HZ2MS(hz))

#define TIMED_EXEC(ret, period_us, code)                             \
        do {                                                         \
                const u64 start = get_mono_ts_us();                  \
                {code};                                              \
                const u64 elapsed = get_mono_ts_us() - start;        \
                ret               = (int)elapsed;                    \
                if (elapsed < (period_us)) {                         \
                        const u64 remaining = (period_us) - elapsed; \
                        delay_us(remaining);                         \
                }                                                    \
        } while (0)

HAPI u64
get_mono_ts_ns(void)
{
#ifdef __linux__
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        return ts.tv_sec * NANO_PER_SEC + ts.tv_nsec;
#elif defined(_WIN32)
        LARGE_INTEGER frequency, counter;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&counter);
        return counter.QuadPart * 1000000000 / frequency.QuadPart;
#endif
        return 0;
}

HAPI u64
get_mono_ts_us(void)
{
        return get_mono_ts_ns() / 1000;
}

HAPI u64
get_mono_ts_ms(void)
{
        return get_mono_ts_ns() / 1000000;
}

HAPI u64
get_mono_ts_s(void)
{
        return get_mono_ts_ns() / 1000000000;
}

HAPI u64
get_real_ts_ns(void)
{
#ifdef __linux__
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return ts.tv_sec * NANO_PER_SEC + ts.tv_nsec;
#elif defined(_WIN32)
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);

        ULARGE_INTEGER uli;
        uli.LowPart  = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;

        return (uli.QuadPart - WIN_TO_UNIX_EPOCH) * 100;
#endif
        return 0;
}

HAPI u64
get_real_ts_us(void)
{
        return get_real_ts_ns() / 1000;
}

HAPI u64
get_real_ts_ms(void)
{
        return get_real_ts_ns() / 1000000;
}

HAPI u64
get_real_ts_s(void)
{
        return get_real_ts_ns() / 1000000000;
}

typedef enum {
        SPIN,
        YIELD,
} delay_e;

HAPI void
spin(u32 us)
{
        u64 start = get_mono_ts_us();
        while ((get_mono_ts_us() - start) < us)
                asm volatile("nop" ::: "memory");
}

#ifdef __linux__
HAPI void
yield(const u32 ms)
{
        usleep(1000 * ms);
}
#elif defined(_WIN32)
#include <windows.h>
HAPI void
yield(const u32 ms)
{
        Sleep(ms);
}
#else
HAPI void
yield(const u32 ms)
{
        spin(MS2US(ms));
}
#endif

HAPI void
delay_us(const u64 us)
{
        spin(us);
}

HAPI void
delay_ms(const u64 ms, const delay_e e_delay)
{
        switch (e_delay) {
                case SPIN: {
                        spin(MS2US(ms));
                        break;
                }
                case YIELD: {
                        yield(ms);
                        break;
                }
                default:
                        break;
        }
}

HAPI void
delay_s(const u64 s, const delay_e e_delay)
{
        switch (e_delay) {
                case SPIN: {
                        spin(S2US(s));
                        break;
                }
                case YIELD: {
                        yield(S2MS(s));
                        break;
                }
                default:
                        break;
        }
}

#ifdef __cplusplus
}
#endif

#endif // !TIMEOPS_H
