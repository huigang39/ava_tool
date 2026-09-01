#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace variable_rewrite
{
inline constexpr size_t maxBytes = 1024 * 1024;
inline bool
range(size_t offset, size_t size, size_t total)
{
        return offset <= total && size <= total - offset;
}
inline uint64_t
mask(unsigned bytes, unsigned offset, unsigned width)
{
        if (!bytes || bytes > 8 || !width || offset >= bytes * 8 || width > bytes * 8 - offset)
                return 0;
        return (width == 64 ? UINT64_MAX : ((uint64_t{1} << width) - 1)) << offset;
}
inline bool
storeBits(uint8_t *dest, size_t bytes, unsigned offset, unsigned width, uint64_t val)
{
        const auto m = mask(static_cast<unsigned>(bytes), offset, width);
        if (!m || (width < 64 && val >= (uint64_t{1} << width)))
                return false;
        uint64_t storage = 0;
        std::memcpy(&storage, dest, bytes);
        storage = (storage & ~m) | ((val << offset) & m);
        std::memcpy(dest, &storage, bytes);
        return true;
}
inline bool
storeSignedBits(uint8_t *dest, size_t bytes, unsigned offset, unsigned width, int64_t val)
{
        if (!mask(static_cast<unsigned>(bytes), offset, width))
                return false;
        if (width < 64 && (val < -(int64_t{1} << (width - 1)) || val > (int64_t{1} << (width - 1)) - 1))
                return false;
        const uint64_t bits = static_cast<uint64_t>(val) & (width == 64 ? UINT64_MAX : (uint64_t{1} << width) - 1);
        return storeBits(dest, bytes, offset, width, bits);
}
/**
 * @brief Merge only explicitly edited fields. Reject concurrent edits of those fields
 */
inline bool
merge(const std::vector<uint8_t> &before,
      const std::vector<uint8_t> &edited,
      const std::vector<uint8_t> &current,
      const std::vector<uint8_t> &masks,
      std::vector<uint8_t>       &result)
{
        if (before.size() != edited.size() || before.size() != current.size() || before.size() != masks.size())
                return false;
        for (size_t i = 0; i < before.size(); ++i) {
                const auto m = masks[i];
                if ((current[i] & m) != (before[i] & m) && (current[i] & m) != (edited[i] & m))
                        return false;
        }
        result = current;
        for (size_t i = 0; i < before.size(); ++i)
                result[i] = (current[i] & ~masks[i]) | (edited[i] & masks[i]);
        return true;
}
template <class T>
bool
integer(const std::string &text, uint8_t *output)
{
        try {
                const auto first = text.find_first_not_of(" \t\r\n");
                const auto prev  = text.find_last_not_of(" \t\r\n");
                if (first == std::string::npos)
                        return false;
                const auto   s      = text.substr(first, prev - first + 1);
                size_t       used   = 0;
                const size_t prefix = (s[0] == '-' || s[0] == '+') ? 1 : 0;
                const int    base =
                    s.size() > prefix + 2 && s[prefix] == '0' && (s[prefix + 1] == 'x' || s[prefix + 1] == 'X') ? 16 : 10;
                T val;
                if constexpr (std::is_signed_v<T>) {
                        const auto parsed = std::stoll(s, &used, base);
                        if (parsed < std::numeric_limits<T>::min() || parsed > std::numeric_limits<T>::max())
                                return false;
                        val = static_cast<T>(parsed);
                } else {
                        if (s[0] == '-')
                                return false;
                        const auto parsed = std::stoull(s, &used, base);
                        if (parsed > std::numeric_limits<T>::max())
                                return false;
                        val = static_cast<T>(parsed);
                }
                if (used != s.size())
                        return false;
                std::memcpy(output, &val, sizeof(val));
                return true;
        } catch (...) {
                return false;
        }
}
} // namespace variable_rewrite
