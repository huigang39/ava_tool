#include "core/export_enum.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ─── platform includes ────────────────────────────────────────────────────────

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// windows.h must precede dbghelp.h — keep the blank line so clang-format
// treats them as separate include groups and does not re-sort alphabetically.
#include <windows.h>

#include <dbghelp.h>
// Link dbghelp.lib via Makefile — do NOT use #pragma comment here.

#elif defined(__APPLE__)
#include <cxxabi.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <sys/mman.h>
#include <sys/stat.h>

#else // Linux / generic POSIX
#include <cxxabi.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif

// ─── demangleSymbol ───────────────────────────────────────────────────────────

std::string
demangleSymbol(const std::string &mangled)
{
#if defined(_WIN32)
        // UnDecorateSymbolName handles MSVC-mangled (decorated) names.
        char  buf[2048] = {};
        DWORD r         = UnDecorateSymbolName(
            mangled.c_str(), buf, static_cast<DWORD>(sizeof(buf)), UNDNAME_COMPLETE | UNDNAME_32_BIT_DECODE);
        if (r > 0 && strcmp(buf, mangled.c_str()) != 0)
                return buf;
        return mangled;

#else
        // abi::__cxa_demangle handles Itanium ABI (GCC/Clang) mangled names.
        int   status = -1;
        char *dem    = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
        if (status == 0 && dem) {
                std::string r(dem);
                free(dem);
                return r;
        }
        return mangled;
#endif
}

// ─── Windows – PE export table ───────────────────────────────────────────────

#if defined(_WIN32)

std::vector<ExportedSymbol>
enumerateExports(void *handle, const std::string &path)
{
        std::vector<ExportedSymbol> result;
        // If no handle provided, try to get the already-loaded module by path.
        if (!handle && !path.empty())
                handle = static_cast<void *>(GetModuleHandleA(path.c_str()));
        if (!handle)
                return result;

        auto *hMod   = reinterpret_cast<HMODULE>(handle);
        auto *dosHdr = reinterpret_cast<PIMAGE_DOS_HEADER>(hMod);
        if (dosHdr->e_magic != IMAGE_DOS_SIGNATURE)
                return result;

        auto *ntHdr = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<BYTE *>(hMod) + dosHdr->e_lfanew);
        if (ntHdr->Signature != IMAGE_NT_SIGNATURE)
                return result;

        const auto &expEntry = ntHdr->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (expEntry.VirtualAddress == 0)
                return result;

        auto *expDir = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(reinterpret_cast<BYTE *>(hMod) + expEntry.VirtualAddress);

        auto *nameRVAs = reinterpret_cast<const DWORD *>(reinterpret_cast<BYTE *>(hMod) + expDir->AddressOfNames);
        auto *ordinals = reinterpret_cast<const WORD *>(reinterpret_cast<BYTE *>(hMod) + expDir->AddressOfNameOrdinals);
        auto *funcRVAs = reinterpret_cast<const DWORD *>(reinterpret_cast<BYTE *>(hMod) + expDir->AddressOfFunctions);

        for (DWORD i = 0; i < expDir->NumberOfNames; ++i) {
                const char *name    = reinterpret_cast<const char *>(reinterpret_cast<BYTE *>(hMod) + nameRVAs[i]);
                WORD        ord     = ordinals[i];
                DWORD       funcRVA = funcRVAs[ord];

                // Skip forwarded exports (their RVA falls inside the export directory).
                if (funcRVA >= expEntry.VirtualAddress && funcRVA < expEntry.VirtualAddress + expEntry.Size)
                        continue;

                ExportedSymbol sym;
                sym.mangled   = name;
                sym.demangled = demangleSymbol(name);
                result.push_back(std::move(sym));
        }
        return result;
}

// ─── Linux – ELF .dynsym ─────────────────────────────────────────────────────

#elif defined(__linux__)

std::vector<ExportedSymbol>
enumerateExports(void * /*handle*/, const std::string &path)
{
        std::vector<ExportedSymbol> result;

        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0)
                return result;

        struct stat st;
        if (fstat(fd, &st) < 0) {
                close(fd);
                return result;
        }
        const auto fileSize = static_cast<size_t>(st.st_size);

        void *mapped = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (mapped == MAP_FAILED)
                return result;

        auto *bytes = static_cast<const uint8_t *>(mapped);

        if (fileSize < sizeof(Elf64_Ehdr) || bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
                munmap(mapped, fileSize);
                return result;
        }

        auto *ehdr     = reinterpret_cast<const Elf64_Ehdr *>(bytes);
        auto *shdrs    = reinterpret_cast<const Elf64_Shdr *>(bytes + ehdr->e_shoff);
        auto *shstrtab = reinterpret_cast<const char *>(bytes + shdrs[ehdr->e_shstrndx].sh_offset);

        const Elf64_Sym *dynsym    = nullptr;
        size_t           dynsymCnt = 0;
        const char      *dynstr    = nullptr;

        for (uint16_t i = 0; i < ehdr->e_shnum; ++i) {
                const char *sname = shstrtab + shdrs[i].sh_name;
                if (strcmp(sname, ".dynsym") == 0) {
                        dynsym    = reinterpret_cast<const Elf64_Sym *>(bytes + shdrs[i].sh_offset);
                        dynsymCnt = shdrs[i].sh_size / sizeof(Elf64_Sym);
                } else if (strcmp(sname, ".dynstr") == 0) {
                        dynstr = reinterpret_cast<const char *>(bytes + shdrs[i].sh_offset);
                }
        }

        if (dynsym && dynstr) {
                for (size_t i = 0; i < dynsymCnt; ++i) {
                        unsigned char bind = ELF64_ST_BIND(dynsym[i].st_info);
                        unsigned char type = ELF64_ST_TYPE(dynsym[i].st_info);
                        if (type != STT_FUNC && type != STT_GNU_IFUNC)
                                continue;
                        if (bind != STB_GLOBAL && bind != STB_WEAK)
                                continue;
                        if (dynsym[i].st_name == 0)
                                continue;

                        const char *mang = dynstr + dynsym[i].st_name;

                        ExportedSymbol sym;
                        sym.mangled   = mang;
                        sym.demangled = demangleSymbol(mang);
                        result.push_back(std::move(sym));
                }
        }

        munmap(mapped, fileSize);
        return result;
}

// ─── macOS – Mach-O LC_SYMTAB ────────────────────────────────────────────────

#elif defined(__APPLE__)

std::vector<ExportedSymbol>
enumerateExports(void * /*handle*/, const std::string &path)
{
        std::vector<ExportedSymbol> result;

        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0)
                return result;

        struct stat st;
        if (fstat(fd, &st) < 0) {
                close(fd);
                return result;
        }
        const auto fileSize = static_cast<size_t>(st.st_size);

        void *mapped = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (mapped == MAP_FAILED)
                return result;

        auto *data  = static_cast<const uint8_t *>(mapped);
        auto *machO = data; // may be advanced to a fat-binary slice

        uint32_t magic = *reinterpret_cast<const uint32_t *>(data);
        if (magic == 0xBEBAFECAu || magic == 0xCAFEBABEu) {
                // Universal (fat) binary – find the matching architecture slice.
                auto    *fat   = reinterpret_cast<const fat_header *>(data);
                uint32_t na    = OSSwapBigToHostInt32(fat->nfat_arch);
                auto    *archs = reinterpret_cast<const fat_arch *>(data + sizeof(fat_header));
                for (uint32_t i = 0; i < na; ++i) {
                        auto ct = static_cast<cpu_type_t>(OSSwapBigToHostInt32(archs[i].cputype));
#if defined(__arm64__) || defined(__aarch64__)
                        if (ct == CPU_TYPE_ARM64)
#else
                        if (ct == CPU_TYPE_X86_64)
#endif
                        {
                                machO = data + OSSwapBigToHostInt32(archs[i].offset);
                                break;
                        }
                }
        }

        auto *hdr = reinterpret_cast<const mach_header_64 *>(machO);
        if (hdr->magic != MH_MAGIC_64) {
                munmap(mapped, fileSize);
                return result;
        }

        const uint8_t         *cmd    = reinterpret_cast<const uint8_t *>(hdr + 1);
        const struct nlist_64 *symtab = nullptr;
        const char            *strtab = nullptr;
        uint32_t               nsyms  = 0;

        for (uint32_t i = 0; i < hdr->ncmds; ++i) {
                auto *lc = reinterpret_cast<const load_command *>(cmd);
                if (lc->cmd == LC_SYMTAB) {
                        auto *st = reinterpret_cast<const symtab_command *>(cmd);
                        // symoff/stroff are relative to the start of the Mach-O slice.
                        symtab = reinterpret_cast<const struct nlist_64 *>(machO + st->symoff);
                        strtab = reinterpret_cast<const char *>(machO + st->stroff);
                        nsyms  = st->nsyms;
                }
                cmd += lc->cmdsize;
        }

        if (symtab && strtab) {
                for (uint32_t i = 0; i < nsyms; ++i) {
                        if (symtab[i].n_type & N_STAB)
                                continue; // debug symbol
                        if ((symtab[i].n_type & N_TYPE) != N_SECT)
                                continue; // not in a section
                        if (!(symtab[i].n_type & N_EXT))
                                continue; // not exported
                        if (symtab[i].n_strx == 0)
                                continue;

                        const char *mang = strtab + symtab[i].n_strx;
                        if (!mang || mang[0] == '\0')
                                continue;

                        // macOS C++ symbols are `__ZN...` in the binary.
                        // abi::__cxa_demangle expects `_ZN...` (one leading _ stripped).
                        const char *demInput = (mang[0] == '_') ? mang + 1 : mang;

                        ExportedSymbol sym;
                        sym.mangled   = mang;
                        sym.demangled = demangleSymbol(demInput);
                        result.push_back(std::move(sym));
                }
        }

        munmap(mapped, fileSize);
        return result;
}

#else
// ─── Unsupported platform ─────────────────────────────────────────────────────
std::vector<ExportedSymbol>
enumerateExports(void * /*handle*/, const std::string & /*path*/)
{
        return {};
}
#endif
