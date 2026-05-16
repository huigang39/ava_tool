#ifndef TIMEOPS_H
#define TIMEOPS_H

#include "platdef.h"

#ifdef OS_POSIX
#include <unistd.h>
#elif defined(OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <time.h>

#include "macrodef.h"
#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIN_TO_UNIX_EPOCH (116444736000000000ULL)

#ifndef NANO_PER_SEC
#define NANO_PER_SEC (1000000000ULL)
#endif

#define NS2US(ns)      ((ns) / 1000.0F)
#define NS2MS(ns)      ((ns) / 1000000.0F)
#define NS2S(ns)       ((ns) / 1000000000.0F)
#define US2NS(us)      ((us) * 1000)
#define US2MS(us)      ((us) / 1000.0F)
#define US2S(us)       ((us) / 1000000.0F)
#define MS2NS(ms)      ((ms) * 1000000)
#define MS2US(ms)      ((ms) * 1000)
#define MS2S(ms)       ((ms) / 1000.0F)
#define S2NS(s)        ((s) * 1000000000)
#define S2US(s)        ((s) * 1000000)
#define S2MS(s)        ((s) * 1000)

#define HZ2S(hz)       (1.0F / (hz))
#define HZ2MS(hz)      (1.0F / (hz) * 1000)
#define HZ2US(hz)      (1.0F / (hz) * 1000000)

#define S2CNT(s, hz)   ((s) * (hz))
#define MS2CNT(ms, hz) ((ms) * (hz) / 1000.0F)

#define TIMED_EXEC(ret, period_us, code)                               \
        do {                                                           \
                const u64 __start = get_mono_ts_us();                  \
                {code};                                                \
                const u64 __elapsed = get_mono_ts_us() - __start;      \
                (ret)               = __elapsed;                       \
                if (__elapsed < (period_us)) {                         \
                        const u64 remaining = (period_us) - __elapsed; \
                        delay_us(remaining);                           \
                }                                                      \
        } while (0)

HAPI u64
get_mono_ts_ns(void)
{
#ifdef OS_POSIX
        struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
        clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
        return ts.tv_sec * NANO_PER_SEC + ts.tv_nsec;
#elif defined(OS_WIN)
        LARGE_INTEGER frequency, counter;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&counter);
        u64 q = (u64)counter.QuadPart / (u64)frequency.QuadPart;
        u64 r = (u64)counter.QuadPart % (u64)frequency.QuadPart;
        return q * 1000000000ULL + (r * 1000000000ULL) / (u64)frequency.QuadPart;
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
#ifdef OS_POSIX
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return ts.tv_sec * NANO_PER_SEC + ts.tv_nsec;
#elif defined(OS_WIN)
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
        DELAY_SPIN,
        DELAY_YIELD,
} delay_e;

HAPI void
spin(u32 us)
{
        u64 start = get_mono_ts_us();
        while ((get_mono_ts_us() - start) < us)
                ;
}

#ifdef OS_POSIX
HAPI void
yield(const u32 ms)
{
        usleep((u32)(ms) * 1000);
}
#elif defined(OS_WIN)
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
        spin((u32)us);
}

HAPI void
delay_ms(const u64 ms, const delay_e e_delay)
{
        switch (e_delay) {
                case DELAY_SPIN: {
                        spin((u32)MS2US(ms));
                        break;
                }
                case DELAY_YIELD: {
                        yield((u32)ms);
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
                case DELAY_SPIN: {
                        spin((u32)S2US(s));
                        break;
                }
                case DELAY_YIELD: {
                        yield((u32)S2MS(s));
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
