#include "core/json_parser.hpp"
#include <fstream>
#include <sstream>

namespace
{
bool
readFile(const std::string &filename, std::string &outContent)
{
        std::ifstream ifs(filename);
        if (!ifs.is_open())
                return false;

        std::stringstream ss;
        ss << ifs.rdbuf();
        outContent = ss.str();
        return true;
}
} // namespace

void
JsonParser::parseRecursive(const cJSON *jsonNode, DataTree &node)
{
        const cJSON *nameItem = cJSON_GetObjectItem(jsonNode, "name");
        const cJSON *typeItem = cJSON_GetObjectItem(jsonNode, "type");

        if (!nameItem || !typeItem)
                return;

        node.name = nameItem->valuestring;

        node.type = Parser::strToDataType(typeItem->valuestring);
        if (node.type == DataType::UNKNOWN && std::string(typeItem->valuestring) == "array") {
                node.type = DataType::ARRAY;
        }

        if (node.type == DataType::ARRAY) {
                if (const cJSON *childrenJson = cJSON_GetObjectItem(jsonNode, "array")) {
                        for (const cJSON *child = childrenJson->child; child; child = child->next) {
                                DataTree childNode;
                                parseRecursive(child, childNode);
                                node.children.push_back(std::move(childNode));
                        }
                }
        }
}

bool
JsonParser::parse(const std::string &path)
{
        path_     = path;
        dataTree_ = DataTree{.name = "CFG", .type = DataType::ARRAY};

        std::string content;
        if (!readFile(path, content))
                return false;

        cJSON *root = cJSON_Parse(content.c_str());
        if (!root)
                return false;

        for (const cJSON *child = root->child; child; child = child->next) {
                DataTree node;
                parseRecursive(child, node);
                dataTree_.children.push_back(std::move(node));
        }

        cJSON_Delete(root);
        return true;
}
