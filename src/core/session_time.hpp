/**
 * @file  session_time.hpp
 * @brief Global session clock — monotonic wall-time since application start.
 *
 * All telemetry timestamps are expressed relative to the session start so that
 * plots always begin near zero regardless of system uptime.
 */
#ifndef SESSION_TIME_HPP
#define SESSION_TIME_HPP

#include "module.h"
#include "timeops.h"

// Return a reference to the session-start timestamp (microseconds, monotonic).
inline u64 &
getSessionStartUs()
{
        static u64 start = get_mono_ts_us();
        return start;
}

// Seconds elapsed since session start (double precision).
inline f64
sessionTimeSec()
{
        u64 now   = get_mono_ts_us();
        u64 start = getSessionStartUs();
        if (now < start)
                return 0.0;
        return static_cast<f64>(now - start) / 1000000.0;
}

// Reset the session clock to "now".
inline void
resetSessionTime()
{
        getSessionStartUs() = get_mono_ts_us();
}

#endif // !SESSION_TIME_HPP
