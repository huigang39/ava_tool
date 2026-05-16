#ifndef BIN_PARSER_HPP
#define BIN_PARSER_HPP

#include "core/parser.hpp"
#include <fstream>

class BinParser : public Parser
{
      private:
        std::string path_{};
        DataTree    dataTree_{};

        static bool parseRecursive(std::ifstream &ifs, DataTree &node);

      public:
        bool               parse(const std::string &path) override;
        const DataTree    &getDataTree() const override { return dataTree_; }
        const std::string &getPath() const override { return path_; }
        void               setTemplate(const DataTree &tree) override;
};

#endif // !BIN_PARSER_HPP
