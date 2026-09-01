#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hss_stream
{
/**
 * @brief 限制启动尝试频率, 失败和配置变化都不能绕过重试间隔
 */
struct StartRetry {
        std::uint64_t lastMs{};
        bool          attempted{};

        /**
         * @brief 首次立即尝试, 后续尝试至少间隔 200 ms
         */
        bool allow(std::uint64_t nowMs)
        {
                if (attempted && nowMs - lastMs < 200)
                        return false;
                attempted = true;
                lastMs    = nowMs;
                return true;
        }
};

/**
 * @brief 检查时间戳向前推进, 允许 32 位计数器正常回绕
 */
inline bool
timestampAdvances(std::uint32_t prev, std::uint32_t next)
{
        const auto delta = next - prev;
        return delta != 0 && delta <= UINT32_C(0x7fffffff);
}

/**
 * @brief Preserve every incomplete frame, including reads containing no full frame
 */
inline std::size_t
retainTail(unsigned char *buffer, std::size_t bytes, std::size_t frameSize)
{
        const auto tail     = bytes % frameSize;
        const auto consumed = bytes - tail;
        if (tail && consumed)
                std::memmove(buffer, buffer + consumed, tail);
        return tail;
}
} // namespace hss_stream
