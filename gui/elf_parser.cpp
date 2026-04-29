#include <cstring>
#include <fstream>
#include <vector>

#include "elf_parser.hpp"

namespace {

constexpr u8 ELF_MAGIC[4] = {0x7F, 'E', 'L', 'F'};

constexpr int EI_CLASS = 4;
constexpr int EI_DATA  = 5;

constexpr u8 ELFCLASS32  = 1;
constexpr u8 ELFCLASS64  = 2;
constexpr u8 ELFDATA2LSB = 1;
constexpr u8 ELFDATA2MSB = 2;

constexpr u32 SHT_SYMTAB = 2;
constexpr u32 SHT_STRTAB = 3;
constexpr u32 SHT_DYNSYM = 11;

#pragma pack(push, 1)
struct Elf32_Ehdr {
        u8  e_ident[16];
        u16 e_type;
        u16 e_machine;
        u32 e_version;
        u32 e_entry;
        u32 e_phoff;
        u32 e_shoff;
        u32 e_flags;
        u16 e_ehsize;
        u16 e_phentsize;
        u16 e_phnum;
        u16 e_shentsize;
        u16 e_shnum;
        u16 e_shstrndx;
};

struct Elf64_Ehdr {
        u8  e_ident[16];
        u16 e_type;
        u16 e_machine;
        u32 e_version;
        u64 e_entry;
        u64 e_phoff;
        u64 e_shoff;
        u32 e_flags;
        u16 e_ehsize;
        u16 e_phentsize;
        u16 e_phnum;
        u16 e_shentsize;
        u16 e_shnum;
        u16 e_shstrndx;
};

struct Elf32_Shdr {
        u32 sh_name;
        u32 sh_type;
        u32 sh_flags;
        u32 sh_addr;
        u32 sh_offset;
        u32 sh_size;
        u32 sh_link;
        u32 sh_info;
        u32 sh_addralign;
        u32 sh_entsize;
};

struct Elf64_Shdr {
        u32 sh_name;
        u32 sh_type;
        u64 sh_flags;
        u64 sh_addr;
        u64 sh_offset;
        u64 sh_size;
        u32 sh_link;
        u32 sh_info;
        u64 sh_addralign;
        u64 sh_entsize;
};

struct Elf32_Sym {
        u32 st_name;
        u32 st_value;
        u32 st_size;
        u8  st_info;
        u8  st_other;
        u16 st_shndx;
};

struct Elf64_Sym {
        u32 st_name;
        u8  st_info;
        u8  st_other;
        u16 st_shndx;
        u64 st_value;
        u64 st_size;
};
#pragma pack(pop)

const char *
strFromTbl(const std::vector<char> &tbl, const u32 off)
{
        if (off >= tbl.size())
                return "";
        return tbl.data() + off;
}

bool
loadFile(const std::string &path, std::vector<u8> &buf)
{
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs.is_open())
                return false;

        const std::streamsize sz = ifs.tellg();
        if (sz <= 0)
                return false;

        ifs.seekg(0, std::ios::beg);
        buf.resize(static_cast<usize>(sz));
        if (!ifs.read(reinterpret_cast<char *>(buf.data()), sz))
                return false;
        return true;
}

template <typename Ehdr, typename Shdr, typename Sym>
bool
parseElf(const std::vector<u8> &buf, ElfInfo &out)
{
        if (buf.size() < sizeof(Ehdr))
                return false;

        Ehdr eh{};
        std::memcpy(&eh, buf.data(), sizeof(Ehdr));

        out.machine = eh.e_machine;

        if (eh.e_shoff == 0 || eh.e_shnum == 0)
                return false;

        if (eh.e_shoff + static_cast<u64>(eh.e_shnum) * eh.e_shentsize > buf.size())
                return false;

        std::vector<Shdr> shdrs(eh.e_shnum);
        for (u16 i = 0; i < eh.e_shnum; ++i) {
                const usize off = eh.e_shoff + static_cast<u64>(i) * eh.e_shentsize;
                std::memcpy(&shdrs[i], buf.data() + off, sizeof(Shdr));
        }

        if (eh.e_shstrndx >= eh.e_shnum)
                return false;

        const Shdr &shstr = shdrs[eh.e_shstrndx];
        if (shstr.sh_offset + shstr.sh_size > buf.size())
                return false;

        std::vector<char> shStrTab(shstr.sh_size);
        std::memcpy(shStrTab.data(), buf.data() + shstr.sh_offset, shstr.sh_size);

        i32 symIdx    = -1;
        i32 strIdx    = -1;
        i32 dynSymIdx = -1;
        i32 dynStrIdx = -1;
        for (u16 i = 0; i < eh.e_shnum; ++i) {
                if (shdrs[i].sh_type == SHT_SYMTAB) {
                        symIdx = i;
                        strIdx = static_cast<i32>(shdrs[i].sh_link);
                } else if (shdrs[i].sh_type == SHT_DYNSYM) {
                        dynSymIdx = i;
                        dynStrIdx = static_cast<i32>(shdrs[i].sh_link);
                }
        }

        if (symIdx < 0) {
                symIdx = dynSymIdx;
                strIdx = dynStrIdx;
        }

        if (symIdx < 0 || strIdx < 0 || strIdx >= eh.e_shnum)
                return false;

        const Shdr &symSh = shdrs[symIdx];
        const Shdr &strSh = shdrs[strIdx];
        if (symSh.sh_offset + symSh.sh_size > buf.size())
                return false;
        if (strSh.sh_offset + strSh.sh_size > buf.size())
                return false;
        if (symSh.sh_entsize == 0 || symSh.sh_entsize < sizeof(Sym))
                return false;

        std::vector<char> strTab(strSh.sh_size);
        std::memcpy(strTab.data(), buf.data() + strSh.sh_offset, strSh.sh_size);

        auto copySectionByName = [&](const char *name, std::vector<u8> &dst) {
                for (u16 i = 0; i < eh.e_shnum; ++i) {
                        const char *sn = strFromTbl(shStrTab, shdrs[i].sh_name);
                        if (std::strcmp(sn, name) == 0) {
                                if (shdrs[i].sh_offset + shdrs[i].sh_size <= buf.size()) {
                                        dst.assign(buf.begin() + shdrs[i].sh_offset,
                                                   buf.begin() + shdrs[i].sh_offset + shdrs[i].sh_size);
                                }
                                return;
                        }
                }
        };
        copySectionByName(".debug_info", out.debug_info);
        copySectionByName(".debug_abbrev", out.debug_abbrev);
        copySectionByName(".debug_str", out.debug_str);
        copySectionByName(".debug_line_str", out.debug_line_str);
        copySectionByName(".debug_str_offsets", out.debug_str_offsets);
        copySectionByName(".debug_addr", out.debug_addr);

        const u64 nsym = symSh.sh_size / symSh.sh_entsize;
        out.symbols.reserve(nsym);

        for (u64 i = 0; i < nsym; ++i) {
                Sym         s{};
                const usize off = symSh.sh_offset + i * symSh.sh_entsize;
                std::memcpy(&s, buf.data() + off, sizeof(Sym));

                if (s.st_name == 0)
                        continue;

                ElfSymbol es{};
                es.name  = strFromTbl(strTab, s.st_name);
                es.addr  = static_cast<u64>(s.st_value);
                es.size  = static_cast<u64>(s.st_size);
                es.type  = s.st_info & 0x0F;
                es.bind  = s.st_info >> 4;
                es.shndx = s.st_shndx;

                if (s.st_shndx > 0 && s.st_shndx < eh.e_shnum) {
                        const Shdr &sec = shdrs[s.st_shndx];
                        es.section      = strFromTbl(shStrTab, sec.sh_name);
                }

                out.symbols.push_back(std::move(es));
        }

        return true;
}

} // namespace

bool
ElfParser::parse(const std::string &path, ElfInfo &out)
{
        out = ElfInfo{};

        std::vector<u8> buf;
        if (!loadFile(path, buf))
                return false;

        if (buf.size() < 16)
                return false;

        if (std::memcmp(buf.data(), ELF_MAGIC, 4) != 0)
                return false;

        const u8 cls  = buf[EI_CLASS];
        const u8 data = buf[EI_DATA];

        if (data != ELFDATA2LSB)
                return false;

        out.isLE = true;
        if (cls == ELFCLASS32) {
                out.is64    = false;
                out.addrSize = 4;
                return parseElf<Elf32_Ehdr, Elf32_Shdr, Elf32_Sym>(buf, out);
        }
        if (cls == ELFCLASS64) {
                out.is64    = true;
                out.addrSize = 8;
                return parseElf<Elf64_Ehdr, Elf64_Shdr, Elf64_Sym>(buf, out);
        }
        return false;
}

const char *
ElfParser::typeStr(const u8 type)
{
        switch (type) {
                case 0:
                        return "NOTYPE";
                case 1:
                        return "OBJECT";
                case 2:
                        return "FUNC";
                case 3:
                        return "SECTION";
                case 4:
                        return "FILE";
                case 5:
                        return "COMMON";
                case 6:
                        return "TLS";
                default:
                        return "?";
        }
}

const char *
ElfParser::bindStr(const u8 bind)
{
        switch (bind) {
                case 0:
                        return "LOCAL";
                case 1:
                        return "GLOBAL";
                case 2:
                        return "WEAK";
                default:
                        return "?";
        }
}

const char *
ElfParser::machineStr(const u16 machine)
{
        switch (machine) {
                case 0:
                        return "NONE";
                case 3:
                        return "x86";
                case 8:
                        return "MIPS";
                case 20:
                        return "PPC";
                case 40:
                        return "ARM";
                case 62:
                        return "x86_64";
                case 183:
                        return "AArch64";
                case 243:
                        return "RISC-V";
                default:
                        return "?";
        }
}
