#ifndef ELF_PARSER_HPP
#define ELF_PARSER_HPP

#include <string>
#include <vector>

#include "module.h"

struct ElfSymbol {
        std::string name;
        std::string section;
        u64         addr;
        u64         size;
        u8          type;
        u8          bind;
        u16         shndx;
};

struct ElfInfo {
        bool                   is64{false};
        bool                   isLE{true};
        u16                    machine{0};
        u8                     addrSize{4};
        std::vector<ElfSymbol> symbols{};

        std::vector<u8> debug_info{};
        std::vector<u8> debug_abbrev{};
        std::vector<u8> debug_str{};
        std::vector<u8> debug_line_str{};
        std::vector<u8> debug_str_offsets{};
        std::vector<u8> debug_addr{};
};

class ElfParser {
      public:
        static bool        parse(const std::string &path, ElfInfo &out);
        static const char *typeStr(u8 type);
        static const char *bindStr(u8 bind);
        static const char *machineStr(u16 machine);
};

#endif // !ELF_PARSER_HPP
