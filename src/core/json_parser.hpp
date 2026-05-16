#ifndef JSON_PARSER_HPP
#define JSON_PARSER_HPP

#include "cJSON.h"
#include "core/parser.hpp"

class JsonParser : public Parser
{
      private:
        std::string path_{};
        DataTree    dataTree_{};

        static void parseRecursive(const cJSON *jsonNode, DataTree &node);

      public:
        bool               parse(const std::string &path) override;
        const DataTree    &getDataTree() const override { return dataTree_; }
        const std::string &getPath() const override { return path_; }
};

#endif // !JSON_PARSER_HPP
