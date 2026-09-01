#pragma once
#include "motor_fault_definitions.hpp"
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace motor_faults
{
inline const Page *
findPage(std::string_view id)
{
        for (const auto &page : pages)
                if (id == page.id)
                        return &page;
        return nullptr;
}
inline const Detail *
findDetail(const Page &page, uint32_t val)
{
        for (size_t i = 0; i < page.count; ++i)
                if (page.entries[i].val == val)
                        return &page.entries[i];
        return nullptr;
}
inline bool
parseWord(std::string_view text, uint32_t &out)
{
        const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        while (!text.empty() && space(text.front()))
                text.remove_prefix(1);
        while (!text.empty() && space(text.back()))
                text.remove_suffix(1);
        unsigned base = 10;
        if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
                base = 16;
                text.remove_prefix(2);
        }
        if (text.empty())
                return false;
        uint32_t result = 0;
        for (char c : text) {
                unsigned digit = c >= '0' && c <= '9'   ? c - '0'
                                 : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                 : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                        : 99;
                if (digit >= base || result > (UINT32_MAX - digit) / base)
                        return false;
                result = result * base + digit;
        }
        out = result;
        return true;
}
struct DecodedWord {
        std::vector<const Detail *> items;
        uint32_t                    unknownMask = 0, commCode = 0;
        bool                        unknownValue = false;
};
inline DecodedWord
decodeWord(const Page &page, uint32_t word)
{
        DecodedWord result;
        if (page.mode == Mode::Enum) {
                if (const auto *entry = findDetail(page, word))
                        result.items.push_back(entry);
                else
                        result.unknownValue = true;
                return result;
        }
        uint32_t known = 0;
        for (size_t i = 0; i < page.count; ++i) {
                const auto &entry  = page.entries[i];
                const auto  mask   = uint32_t(1) << entry.val;
                known             |= mask;
                if (word & mask)
                        result.items.push_back(&entry);
        }
        if (page.mode == Mode::Predrv)
                result.commCode = word & 0x7FFFFFFFu;
        else
                result.unknownMask = word & ~known;
        return result;
}
inline std::string
report(const Page &page, uint32_t word)
{
        char val[48];
        std::snprintf(val, sizeof(val), "0x%08X (%u)", word, word);
        std::string text   = std::string(page.title) + " [" + page.id + "] " + val + "\n目录: " + catalogId + "\n";
        text              += std::string(page.note) + "\n";
        const auto result  = decodeWord(page, word);
        for (const auto *item : result.items) {
                text += (page.mode == Mode::Enum ? "值 " : "bit ") + std::to_string(item->val) + ": " + item->zh + " [" +
                        item->symbol + "]\n";
                text += std::string("原因: ") + item->cause + "\n排查: " + item->action + "\n清除: " + item->clear + "\n";
                if (*item->sources)
                        text += std::string("内部来源: ") + item->sources + "\n";
        }
        if (result.unknownValue)
                text += "未定义的枚举值,保留原值.\n";
        if (result.unknownMask) {
                std::snprintf(val, sizeof(val), "0x%08X", result.unknownMask);
                text += std::string("未定义位: ") + val + ";核对固件版本及置位来源.\n";
        }
        if (page.mode == Mode::Predrv)
                text += "底层通信错误码: " + std::to_string(result.commCode) + "(按底层接口定义解析)\n";
        if (!word && page.mode != Mode::Enum)
                text += "本页无置位.\n";
        return text;
}
} // namespace motor_faults
