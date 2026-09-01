#ifndef NUMERIC_FORMAT_HPP
#define NUMERIC_FORMAT_HPP

#include "ckit.h"

#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>

inline int
normalizeDisplayRadix(int radix)
{
        return radix == 2 || radix == 8 || radix == 16 ? radix : 10;
}

inline const char *
displayRadixName(int radix, bool chinese)
{
        switch (normalizeDisplayRadix(radix)) {
                case 16:
                        return chinese ? "十六进制" : "Hexadecimal";
                case 8:
                        return chinese ? "八进制" : "Octal";
                case 2:
                        return chinese ? "二进制" : "Binary";
                default:
                        return chinese ? "十进制" : "Decimal";
        }
}

inline std::string
formatIntegerValue(u64 raw, bool signedValue, u32 bitWidth, int radix)
{
        radix           = normalizeDisplayRadix(radix);
        bitWidth        = std::clamp<u32>(bitWidth, 1, 64);
        const u64 mask  = bitWidth == 64 ? ~u64{0} : ((u64{1} << bitWidth) - 1);
        raw            &= mask;

        if (radix == 10) {
                char buffer[32]{};
                if (signedValue) {
                        i64 val = static_cast<i64>(raw);
                        if (bitWidth < 64 && (raw & (u64{1} << (bitWidth - 1))))
                                val = static_cast<i64>(raw | ~mask);
                        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), val);
                        return result.ec == std::errc{} ? std::string(buffer, result.ptr) : std::string{"0"};
                }
                const auto result = std::to_chars(buffer, buffer + sizeof(buffer), raw);
                return result.ec == std::errc{} ? std::string(buffer, result.ptr) : std::string{"0"};
        }

        const char *digits = "0123456789ABCDEF";
        std::string body;
        const u32   digitBits = radix == 16 ? 4u : (radix == 8 ? 3u : 1u);
        const u32   minDigits = (bitWidth + digitBits - 1) / digitBits;
        do {
                body.push_back(digits[raw % static_cast<u64>(radix)]);
                raw /= static_cast<u64>(radix);
        } while (raw != 0);
        while (body.size() < minDigits)
                body.push_back('0');
        std::reverse(body.begin(), body.end());

        if (radix == 16)
                return "0x" + body;
        if (radix == 8)
                return "0o" + body;
        return "0b" + body;
}

inline bool
parseIntegerValue(std::string_view text, int radix, bool signedValue, u32 bitWidth, u64 &raw)
{
        radix = normalizeDisplayRadix(radix);
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
                text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
                text.remove_suffix(1);
        if (text.empty())
                return false;

        bool negative = false;
        if (text.front() == '+' || text.front() == '-') {
                negative = text.front() == '-';
                text.remove_prefix(1);
        }
        if (radix == 16 && text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
                text.remove_prefix(2);
        else if (radix == 8 && text.size() >= 2 && text[0] == '0' && (text[1] == 'o' || text[1] == 'O'))
                text.remove_prefix(2);
        else if (radix == 2 && text.size() >= 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B'))
                text.remove_prefix(2);
        if (text.empty() || (negative && !signedValue))
                return false;

        u64        magnitude = 0;
        const auto result    = std::from_chars(text.data(), text.data() + text.size(), magnitude, radix);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
                return false;

        bitWidth       = std::clamp<u32>(bitWidth, 1, 64);
        const u64 mask = bitWidth == 64 ? ~u64{0} : ((u64{1} << bitWidth) - 1);
        if (!signedValue || radix != 10) {
                if (magnitude > mask)
                        return false;
        } else {
                const u64 negativeLimit = u64{1} << (bitWidth - 1);
                const u64 positiveLimit = negativeLimit - 1;
                if ((negative && magnitude > negativeLimit) || (!negative && magnitude > positiveLimit))
                        return false;
        }

        raw = negative ? (~magnitude + 1) & mask : magnitude & mask;
        return true;
}

#endif
