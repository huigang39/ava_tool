#ifndef ELF_TYPES_HPP
#define ELF_TYPES_HPP

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

#endif // !ELF_TYPES_HPP
