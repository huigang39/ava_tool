#ifndef DWARF_PARSER_HPP
#define DWARF_PARSER_HPP

#include <string>
#include <unordered_map>
#include <vector>

#include "core/elf_types.hpp"
#include "module.h"

namespace dwarf
{

enum class TypeKind {
        UNKNOWN,
        BASE,
        POINTER,
        ARRAY,
        STRUCT,
        UNION,
        ENUM,
        TYPEDEF,
        MODIFIER,
        SUBROUTINE,
};

struct Type {
        TypeKind         kind{TypeKind::UNKNOWN};
        std::string      name{};
        u64              size{0};
        u32              encoding{0};
        u64              inner{0};
        std::vector<u64> dims{};

        struct Member {
                std::string name;
                u64         offset{0};
                u64         type{0};
                u32         bitOffset{0};
                u32         bitSize{0};
        };
        std::vector<Member> members{};

        struct Enumerator {
                std::string name;
                i64         value{0};
        };
        std::vector<Enumerator> enums{};
};

struct Variable {
        std::string name{};
        u64         addr{0};
        u64         type{0};
        bool        external{false};
};

struct Info {
        std::unordered_map<u64, Type> types{};
        std::vector<Variable>         variables{};
        bool                          present{false};
};

bool parse(const ElfInfo &elf, Info &out);

const char *typeKindStr(TypeKind k);

} // namespace dwarf

#endif // !DWARF_PARSER_HPP
