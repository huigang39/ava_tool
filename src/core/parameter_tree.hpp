#pragma once
#include "core/parameter_json.hpp"
#include <algorithm>
#include <functional>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace parameter_json
{
struct TreeNode {
        std::string           name, path;
        size_t                field = std::numeric_limits<size_t>::max();
        std::vector<TreeNode> children;
};
inline TreeNode
tree(const Document &doc)
{
        // A wide MCU array can have tens of thousands of siblings. Searching
        // each sibling vector on insertion makes rebuilding the tree quadratic
        // Stable numeric indices also avoid retaining pointers across growth
        struct ChildKey {
                size_t           parent;
                std::string_view name;
                bool             operator==(const ChildKey &) const = default;
        };
        struct ChildHash {
                size_t operator()(const ChildKey &key) const
                {
                        return std::hash<std::string_view>{}(key.name) ^
                               (key.parent + 0x9e3779b9 + (key.parent << 6) + (key.parent >> 2));
                }
        };
        struct Builder {
                TreeNode            node;
                std::vector<size_t> children;
        };
        std::vector<Builder>                            nodes(1);
        std::unordered_map<ChildKey, size_t, ChildHash> children;
        nodes.reserve(doc.fields.size() + 1);
        children.reserve(doc.fields.size());
        for (size_t index = 0; index < doc.fields.size(); ++index) {
                const auto &path = doc.fields[index].row.path;
                if (path.empty()) {
                        nodes[0].children.push_back(nodes.size());
                        nodes.push_back({{"value", "", index}, {}});
                        continue;
                }
                size_t node = 0;
                for (size_t i = 0; i < path.size();) {
                        if (path[i] == '.') {
                                ++i;
                                continue;
                        }
                        const size_t start = i;
                        if (path[i] == '[') {
                                const auto end = path.find(']', i);
                                i              = end == std::string::npos ? path.size() : end + 1;
                        } else {
                                while (i < path.size() && path[i] != '.' && path[i] != '[')
                                        ++i;
                        }
                        const std::string_view name(path.data() + start, i - start);
                        const auto [child, inserted] = children.try_emplace({node, name}, nodes.size());
                        if (inserted) {
                                nodes[node].children.push_back(child->second);
                                nodes.push_back({{std::string(name), path.substr(0, i)}, {}});
                        }
                        node = child->second;
                }
                nodes[node].node.field = index;
        }
        // Children always have larger indices, so assembling in reverse order
        // produces the same insertion order without a second recursive walk
        for (size_t index = nodes.size(); index-- > 0;) {
                auto &node = nodes[index].node;
                node.children.reserve(nodes[index].children.size());
                for (const auto child : nodes[index].children)
                        node.children.push_back(std::move(nodes[child].node));
                if (!node.children.empty() && node.children.front().name.starts_with("[")) {
                        std::stable_sort(node.children.begin(), node.children.end(), [](const auto &a, const auto &b) {
                                return a.name.size() != b.name.size() ? a.name.size() < b.name.size() : a.name < b.name;
                        });
                }
        }
        return std::move(nodes[0].node);
}

// FileEditor exposes its Document so callers can edit values directly. Compare
// paths and their order rather than relying on an invalidation flag that a
// caller could forget to set; values and display metadata do not affect trees
class TreeCache
{
        struct Data {
                TreeNode                 root;
                std::vector<std::string> paths;
        };
        std::shared_ptr<const Data> data_;

      public:
        const TreeNode &get(const Document &doc)
        {
                const bool same = data_ && data_->paths.size() == doc.fields.size() &&
                                  std::equal(data_->paths.begin(),
                                             data_->paths.end(),
                                             doc.fields.begin(),
                                             [](const auto &path, const auto &field) { return path == field.row.path; });
                if (!same) {
                        auto next  = std::make_shared<Data>();
                        next->root = tree(doc);
                        next->paths.reserve(doc.fields.size());
                        for (const auto &field : doc.fields)
                                next->paths.push_back(field.row.path);
                        data_ = std::move(next);
                }
                return data_->root;
        }
};
} // namespace parameter_json
