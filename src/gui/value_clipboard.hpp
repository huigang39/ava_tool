#pragma once
#include <string>
#include <string_view>

namespace ui
{
// RFC 4180 CSV, including escaped quotes and multiline values.
inline void
appendClipboardRow(std::string &text, std::string_view name, std::string_view value, std::string_view address = {})
{
        auto cell = [&](std::string_view content) {
                const bool quote = content.find_first_of(",\r\n\"") != std::string_view::npos;
                if (quote)
                        text += '"';
                for (const char c : content) {
                        text += c;
                        if (quote && c == '"')
                                text += '"';
                }
                if (quote)
                        text += '"';
        };
        cell(name);
        text += ',';
        cell(value);
        text += ',';
        cell(address);
        text += "\r\n";
}
} // namespace ui
