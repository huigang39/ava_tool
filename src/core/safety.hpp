#pragma once
#include "core/diagnostic_value.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace safety
{
enum class Severity { Info, Ok, Warning, Error };
struct Entry {
        std::string val, mask, name, title, cause, action, clear;
        Severity    severity = Severity::Info;
};
struct Decoder {
        std::string              id, title, kind = "number", note, unit, type = "auto";
        unsigned                 width = 32;
        std::vector<Entry>       entries;
        std::vector<std::string> aliases;
};
struct Field {
        std::string path, title, decoder, note, unit;
};
struct Condition {
        std::string path, val, mask;
};
struct Group {
        std::string        id, title, root, note;
        bool               expand = true;
        Condition          when;
        std::vector<Field> fields;
};
struct Profile {
        std::string          name, description;
        std::vector<Decoder> decoders;
        std::vector<Group>   groups;
};
struct Finding {
        std::string name, title, cause, action, clear;
        Severity    severity = Severity::Info;
};
struct Result {
        std::string          val, raw, summary, note;
        std::vector<Finding> findings;
        bool                 unknown = false;
};

/**
 * @brief 读取并校验 JSON 定义, 无效输入抛出异常, 由调用方保留旧配置
 */
Profile parse(std::string_view json);
/**
 * @brief 从 UTF-8 路径读取配置, 不连接设备
 */
Profile load(const std::string &path);
/**
 * @brief 按名称或旧别名查找解析器, 未找到返回 nullptr
 */
const Decoder *findDecoder(const Profile &, std::string_view id);
/**
 * @brief 按相对路径查找字段说明
 */
const Field *findField(const Group &, std::string_view relativePath);
/**
 * @brief 调用公共 C 核心解析, 并附加 JSON 中的原因和处理说明
 */
Result describe(const Decoder *, const diagnostics::Value &);
/**
 * @brief 校验离线输入类型和范围, 失败时清空输出并给出错误说明
 */
bool parseValue(std::string_view text, const Decoder &, diagnostics::Value &, std::string &error);
/**
 * @brief 调用公共 C 核心检查分组条件
 */
bool matches(const Condition &, const diagnostics::Value &);
/**
 * @brief 生成包含原始值和处理说明的文本报告
 */
std::string report(std::string_view path, const Result &);
} // namespace safety
