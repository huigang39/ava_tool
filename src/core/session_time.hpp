/**
 * @file  session_time.hpp
 * @brief Global session clock — monotonic wall-time since application start.
 *
 * All telemetry tss are expressed relative to the session start so that
 * plots always begin near zero regardless of system uptime.
 */
#ifndef SESSION_TIME_HPP
#define SESSION_TIME_HPP

#include "ckit.h"
#include "timeops.h"
#include <atomic>
#include <chrono>

struct SessionClockOrigin {
        std::atomic<u64> monotonicUs{get_mono_ts_us()};
        std::atomic<i64> unixUs{
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count()};
};

inline SessionClockOrigin &
sessionClockOrigin()
{
        static SessionClockOrigin origin;
        return origin;
}

/**
 * @brief Return a reference to the session-start ts (microseconds, monotonic)
 */
inline std::atomic<u64> &
getSessionStartUs()
{
        return sessionClockOrigin().monotonicUs;
}

// Stable wall-clock anchor for the relative acquisition timestamps. Reading the
// current wall clock while drawing would make paused samples drift on screen.
inline double
sessionTimeOriginSeconds()
{
        return static_cast<double>(sessionClockOrigin().unixUs.load()) / 1000000.0;
}

/**
 * @brief Seconds elapsed since session start (double precision)
 */
inline f64
sessionTimeSec()
{
        u64 now   = get_mono_ts_us();
        u64 start = getSessionStartUs();
        if (now < start)
                return 0.0;
        return static_cast<f64>(now - start) / 1000000.0;
}

/**
 * @brief Reset the session clock to "now"
 */
inline void
resetSessionTime()
{
        auto &origin       = sessionClockOrigin();
        origin.monotonicUs = get_mono_ts_us();
        origin.unixUs =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

#endif // !SESSION_TIME_HPP
