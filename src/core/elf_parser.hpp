#ifndef ELF_PARSER_HPP
#define ELF_PARSER_HPP

#include "core/dwarf_parser.hpp"
#include "core/elf_types.hpp"
#include "core/parser.hpp"

class ElfParser : public Parser
{
      private:
        std::string path_{};
        DataTree    dataTree_{};
        ElfInfo     elfInfo_{};
        dwarf::Info dwarfInfo_{};

      public:
        bool               parse(const std::string &path) override;
        const DataTree    &getDataTree() const override { return dataTree_; }
        const std::string &getPath() const override { return path_; }
        void               setTemplate(const DataTree &tree) override { dataTree_ = tree; }

        const ElfInfo     &getElfInfo() const { return elfInfo_; }
        const dwarf::Info &getDwarfInfo() const { return dwarfInfo_; }

        static const char *typeStr(u8 type);
        static const char *bindStr(u8 bind);
        static const char *machineStr(u16 machine);
};

#endif // !ELF_PARSER_HPP
