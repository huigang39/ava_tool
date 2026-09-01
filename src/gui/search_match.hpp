#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace ui::search
{
inline std::string
LowerCopy(std::string_view value)
{
        std::string result(value);
        std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
}

inline std::string
AddressText(unsigned long long address)
{
        char text[24];
        std::snprintf(text, sizeof(text), "0x%08llX", address);
        return text;
}

inline std::vector<unsigned long long>
AddressCandidates(std::string_view query)
{
        query.remove_prefix(std::min(query.size(), query.find_first_not_of(" \t\r\n")));
        while (!query.empty() && std::isspace(static_cast<unsigned char>(query.back())))
                query.remove_suffix(1);
        if (query.empty())
                return {};

        const bool prefixed = query.size() > 2 && query[0] == '0' && (query[1] == 'x' || query[1] == 'X');
        const auto digits   = prefixed ? query.substr(2) : query;
        if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
                return {};

        std::vector<unsigned long long> candidates;
        const auto                      add = [&](int base) {
                unsigned long long value = 0;
                const auto         result = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
                if (result.ec == std::errc{} && result.ptr == digits.data() + digits.size() &&
                    std::find(candidates.begin(), candidates.end(), value) == candidates.end())
                        candidates.push_back(value);
        };
        const bool hasHexLetter =
            std::any_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isalpha(c) != 0; });
        if (prefixed || hasHexLetter)
                add(16);
        else {
                add(10);
                add(16);
        }
        return candidates;
}

inline bool
MatchesAddress(unsigned long long address, std::string_view lowerQuery)
{
        if (lowerQuery.empty())
                return true;
        if (LowerCopy(AddressText(address)).find(lowerQuery) != std::string::npos)
                return true;
        const auto candidates = AddressCandidates(lowerQuery);
        return std::find(candidates.begin(), candidates.end(), address) != candidates.end();
}
} // namespace ui::search
