#pragma once

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace parameter_json
{

/**
 * @brief 递归收集预设 JSON, 排除布局清单和安全定义
 */
inline std::vector<std::filesystem::path>
presetFiles(const std::filesystem::path &directory)
{
        std::vector<std::filesystem::path> files;
        std::error_code                    ec;
        for (std::filesystem::recursive_directory_iterator
                 it(directory, std::filesystem::directory_options::skip_permission_denied, ec),
             end;
             !ec && it != end;
             it.increment(ec)) {
                if (!it->is_regular_file(ec) || it->path().extension() != ".json")
                        continue;
                const auto name = it->path().filename();
                if (name != "param_list_registry.json" && name != "param_list_manifest.json" && name != "safety.json")
                        files.push_back(it->path());
        }
        std::sort(files.begin(), files.end());
        return files;
}

/**
 * @brief 按型号名查找默认参数, 根目录优先, 子目录中的重名配置报错
 */
inline std::filesystem::path
findPreset(const std::filesystem::path &directory, const std::string &name)
{
        const auto      filename = std::filesystem::u8path(name + ".json");
        const auto      direct   = directory / filename;
        std::error_code ec;
        if (std::filesystem::is_regular_file(direct, ec))
                return direct;
        std::filesystem::path found;
        for (const auto &file : presetFiles(directory)) {
                if (file.filename() != filename)
                        continue;
                if (!found.empty())
                        throw std::runtime_error("Duplicate parameter preset name: " + name);
                found = file;
        }
        return found;
}

} // namespace parameter_json
