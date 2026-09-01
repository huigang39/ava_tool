#pragma once
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace parameter_csv
{
struct Row {
        std::string path, type, val, unit, min, max, description;
        size_t      line{};
};
inline std::string
quote(const std::string &s)
{
        std::string out = "\"";
        for (char c : s) {
                if (c == '"')
                        out += '"';
                out += c;
        }
        return out + '"';
}
inline std::string
format(const std::vector<Row> &rows)
{
        std::string out = "\xEF\xBB\xBFpath,type,default,unit,min,max,description\r\n";
        for (const auto &r : rows) {
                out += quote(r.path) + ',' + quote(r.type) + ',' + quote(r.val) + ',' + quote(r.unit) + ',' + quote(r.min) +
                       ',' + quote(r.max) + ',' + quote(r.description) + "\r\n";
        }
        return out;
}
inline std::vector<Row>
parse(const std::string &input)
{
        if (input.size() > 16 * 1024 * 1024)
                throw std::runtime_error("CSV exceeds 16 MiB");
        size_t                                i = input.starts_with("\xEF\xBB\xBF") ? 3 : 0, line = 1;
        std::vector<std::vector<std::string>> records;
        std::vector<size_t>                   lines;
        while (i < input.size()) {
                const auto               start = line;
                std::vector<std::string> cells;
                while (true) {
                        std::string cell;
                        if (i < input.size() && input[i] == '"') {
                                ++i;
                                bool closed = false;
                                while (i < input.size()) {
                                        char c = input[i++];
                                        if (c == '"') {
                                                if (i < input.size() && input[i] == '"') {
                                                        ++i;
                                                        cell += '"';
                                                } else {
                                                        closed = true;
                                                        break;
                                                }
                                        } else {
                                                cell += c;
                                                if (c == '\n')
                                                        ++line;
                                        }
                                }
                                if (!closed)
                                        throw std::runtime_error("CSV line " + std::to_string(start) + ": unclosed quote");
                        } else {
                                while (i < input.size() && input[i] != ',' && input[i] != '\r' && input[i] != '\n') {
                                        if (input[i] == '"')
                                                throw std::runtime_error("CSV line " + std::to_string(line) +
                                                                         ": unexpected quote");
                                        cell += input[i++];
                                }
                        }
                        cells.push_back(cell);
                        if (cells.size() > 7)
                                throw std::runtime_error("CSV line " + std::to_string(start) + ": too many columns");
                        if (i == input.size())
                                break;
                        if (input[i] == ',') {
                                ++i;
                                continue;
                        }
                        if (input[i] == '\r' || input[i] == '\n') {
                                if (input[i++] == '\r' && i < input.size() && input[i] == '\n')
                                        ++i;
                                ++line;
                                break;
                        }
                        throw std::runtime_error("CSV line " + std::to_string(start) + ": invalid delimiter");
                }
                if (cells.size() == 1 && cells[0].empty())
                        continue;
                records.push_back(std::move(cells));
                lines.push_back(start);
                if (records.size() > 65537)
                        throw std::runtime_error("Too many CSV rows");
        }
        const std::vector<std::string> header{"path", "type", "default", "unit", "min", "max", "description"};
        if (records.empty() || records[0] != header)
                throw std::runtime_error("Expected CSV header: path,type,default,unit,min,max,description");
        std::vector<Row>                rows;
        std::unordered_set<std::string> seen;
        for (size_t n = 1; n < records.size(); ++n) {
                const auto &r = records[n];
                if (r.size() != 7 || !seen.insert(r[0]).second)
                        throw std::runtime_error("CSV line " + std::to_string(lines[n]) +
                                                 ": invalid columns or duplicate path");
                rows.push_back({r[0], r[1], r[2], r[3], r[4], r[5], r[6], lines[n]});
        }
        if (rows.empty())
                throw std::runtime_error("CSV has no parameters");
        return rows;
}
inline long double
number(const std::string &text)
{
        size_t     end = 0;
        const auto v   = std::stold(text, &end);
        if (end != text.size() || !std::isfinite(v))
                throw std::runtime_error("invalid finite number");
        return v;
}
inline void
bounds(const Row &r, const std::string &val)
{
        if (r.min.empty() && r.max.empty())
                return;
        const auto v = number(val);
        if ((!r.min.empty() && v < number(r.min)) || (!r.max.empty() && v > number(r.max)) ||
            (!r.min.empty() && !r.max.empty() && number(r.min) > number(r.max)))
                throw std::runtime_error("value outside min/max");
}
} // namespace parameter_csv
