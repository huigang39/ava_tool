#pragma once
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace diagnostics
{
enum class Kind { Unsigned, Signed, Boolean, Floating };
struct Value {
        Kind                                         kind{Kind::Unsigned};
        uint64_t                                     bits{};
        unsigned                                     width{};
        double                                       real{};
        std::string                                  type;
        std::vector<std::pair<int64_t, std::string>> enums;
};
struct Field {
        std::string path, type, status;
        bool        supported{true};
};

inline int64_t
signValue(const Value &val)
{
        if (!val.width || val.width > 64)
                return std::bit_cast<int64_t>(val.bits);
        auto raw = val.bits;
        if (val.width < 64) {
                const auto mask  = (uint64_t(1) << val.width) - 1;
                raw             &= mask;
                if (raw & (uint64_t(1) << (val.width - 1)))
                        raw |= ~mask;
        }
        return std::bit_cast<int64_t>(raw);
}

inline bool
isZero(const Value &val)
{
        return val.kind == Kind::Floating ? val.real == 0.0 : val.bits == 0;
}

/**
 * @brief 整数直接格式化, 保留 64 位计数和位图的全部有效位
 */
inline std::string
formatValue(const Value &val)
{
        if (val.kind == Kind::Signed)
                return std::to_string(signValue(val));
        if (val.kind == Kind::Boolean && val.bits <= 1)
                return val.bits ? "true (1)" : "false (0)";
        if (val.kind != Kind::Floating)
                return std::to_string(val.bits);
        std::array<char, 96> buffer{};
        const int            precision =
            val.width == 32 ? std::numeric_limits<float>::max_digits10 : std::numeric_limits<double>::max_digits10;
        const auto result =
            std::to_chars(buffer.data(), buffer.data() + buffer.size(), val.real, std::chars_format::general, precision);
        return result.ec == std::errc{} ? std::string(buffer.data(), result.ptr) : "无法格式化";
}

inline std::string
formatBits(const Value &val)
{
        constexpr char digits[] = "0123456789ABCDEF";
        const unsigned count    = val.width && val.width <= 64 ? (val.width + 3) / 4 : 16;
        std::string    text(2 + count, '0');
        text[1] = 'x';
        for (unsigned i = 0; i < count; ++i)
                text[2 + count - i - 1] = digits[(val.bits >> (4 * i)) & 15];
        return text;
}
} // namespace diagnostics
