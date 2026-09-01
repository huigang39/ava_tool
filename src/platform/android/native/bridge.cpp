#include "cJSON.h"
#include "core/elf_parser.hpp"
#include <algorithm>
#include <cstdio>
#include <jni.h>
#include <memory>
#include <stdexcept>
#include <unordered_set>

FILE *
nativeFopen(const std::string &path, const char *mode)
{
        return std::fopen(path.c_str(), mode);
}
namespace
{
// The shared parser assumes complete section records and terminated string tables.
// Validate file-picker input before handing it to that parser.
void
validateElf(const std::string &filename)
{
        std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(filename.c_str(), "rb"), std::fclose);
        if (!file || std::fseek(file.get(), 0, SEEK_END))
                throw std::runtime_error("Cannot open ELF file");
        long length = std::ftell(file.get());
        if (length < 52 || length > 64 * 1024 * 1024)
                throw std::runtime_error("Invalid ELF file size");
        std::rewind(file.get());
        std::vector<u8> data(static_cast<size_t>(length));
        if (std::fread(data.data(), 1, data.size(), file.get()) != data.size())
                throw std::runtime_error("Incomplete ELF file");
        auto word = [&](size_t at, int bytes) {
                u64 value = 0;
                for (int i = 0; i < bytes; ++i)
                        value |= u64(data.at(at + i)) << (8 * i);
                return value;
        };
        if (word(0, 4) != 0x464c457f || data[4] != 1 || data[5] != 1 || word(18, 2) != 40)
                throw std::runtime_error("Select a little-endian 32-bit ARM ELF/AXF with symbols");
        const u64 start = word(32, 4), stride = word(46, 2), count = word(48, 2), strings = word(50, 2);
        if (!start || !count || stride < 40 || start + count * stride > data.size() || strings >= count)
                throw std::runtime_error("Invalid ELF section table");
        for (u64 i = 0; i < count; ++i) {
                const size_t sh   = static_cast<size_t>(start + i * stride);
                const u64    type = word(sh + 4, 4), offset = word(sh + 16, 4), size = word(sh + 20, 4);
                if (type != 8 && offset + size > data.size())
                        throw std::runtime_error("ELF section exceeds file");
                if ((type == 3 || i == strings) && size && data.at(static_cast<size_t>(offset + size - 1)) != 0)
                        throw std::runtime_error("Unterminated ELF string table");
                if (type == 2 || type == 11) {
                        const u64 linked = word(sh + 24, 4), entry = word(sh + 36, 4);
                        if (entry < 16 || size % entry || linked >= count ||
                            word(static_cast<size_t>(start + linked * stride + 4), 4) != 3)
                                throw std::runtime_error("Invalid ELF symbol table");
                }
        }
}
const dwarf::Type *
resolve(const dwarf::Info &info, u64 key)
{
        for (int depth = 0; depth < 32; ++depth) {
                auto it = info.types.find(key);
                if (it == info.types.end())
                        return nullptr;
                const auto &t = it->second;
                if (t.kind != dwarf::TypeKind::MODIFIER && t.kind != dwarf::TypeKind::TYPEDEF)
                        return &t;
                key = t.inner;
        }
        return nullptr;
}
std::string
scalar(const dwarf::Type &t)
{
        if (t.kind == dwarf::TypeKind::POINTER)
                return t.size == 4 ? "u32" : "";
        if (t.kind != dwarf::TypeKind::BASE && t.kind != dwarf::TypeKind::ENUM)
                return "";
        if (t.encoding == 4)
                return t.size == 4 ? "f32" : t.size == 8 ? "f64" : "";
        if (t.size != 1 && t.size != 2 && t.size != 4 && t.size != 8)
                return "";
        return std::string(t.encoding == 5 || t.encoding == 6 ? "i" : "u") + std::to_string(t.size * 8);
}
struct Exporter {
        const dwarf::Info              &info;
        cJSON                          *rows;
        std::unordered_set<std::string> names;
        int                             count = 0;
        void                            add(const std::string &name, u64 address, const std::string &type, u64 size)
        {
                if (count >= 20000 || address > UINT32_MAX || type.empty() || !names.insert(name).second)
                        return;
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "name", name.c_str());
                cJSON_AddNumberToObject(item, "address", static_cast<double>(address));
                cJSON_AddStringToObject(item, "type", type.c_str());
                cJSON_AddNumberToObject(item, "size", static_cast<double>(size));
                cJSON_AddItemToArray(rows, item);
                ++count;
        }
        void visit(const std::string &name, u64 address, u64 key, int depth = 0)
        {
                if (depth > 12 || count >= 20000 || address > UINT32_MAX)
                        return;
                const auto *t = resolve(info, key);
                if (!t)
                        return;
                auto type = scalar(*t);
                if (!type.empty()) {
                        add(name, address, type, t->size);
                        return;
                }
                if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
                        for (const auto &m : t->members) {
                                if (m.bitSize || m.offset > UINT32_MAX - address)
                                        continue; // bitfields need masked writes, excluded
                                visit(name + "." + m.name, address + m.offset, m.type, depth + 1);
                        }
                } else if (t->kind == dwarf::TypeKind::ARRAY && t->dims.size() == 1) {
                        const auto *element = resolve(info, t->inner);
                        if (!element || element->size == 0)
                                return;
                        for (u64 i = 0; i < std::min<u64>(t->dims[0], 256); ++i)
                                visit(name + "[" + std::to_string(i) + "]", address + i * element->size, t->inner, depth + 1);
                }
        }
};
} // namespace
extern "C" JNIEXPORT jstring JNICALL
Java_com_ava_tool_NativeCore_parseElf(JNIEnv *env, jclass, jstring path)
{
        const char *utf = env->GetStringUTFChars(path, nullptr);
        if (!utf)
                return nullptr;
        std::string filename(utf);
        env->ReleaseStringUTFChars(path, utf);
        try {
                validateElf(filename);
                ElfParser parser;
                if (!parser.parse(filename) || parser.getElfInfo().is64 || parser.getElfInfo().machine != 40) {
                        env->ThrowNew(env->FindClass("java/io/IOException"),
                                      "Select a little-endian 32-bit ARM ELF/AXF with symbols");
                        return nullptr;
                }
                std::unique_ptr<cJSON, decltype(&cJSON_Delete)> result(cJSON_CreateObject(), cJSON_Delete);
                auto                                           *rows = cJSON_AddArrayToObject(result.get(), "symbols");
                Exporter                                        out{parser.getDwarfInfo(), rows};
                for (const auto &v : parser.getDwarfInfo().variables)
                        out.visit(v.name, v.addr, v.type);
                for (const auto &s : parser.getElfInfo().symbols) {
                        if (s.type != 1 || !s.shndx || (s.size != 1 && s.size != 2 && s.size != 4 && s.size != 8))
                                continue;
                        out.add(s.name, s.addr, "u" + std::to_string(s.size * 8), s.size);
                }
                cJSON_AddBoolToObject(result.get(), "dwarf", parser.getDwarfInfo().present);
                cJSON_AddBoolToObject(result.get(), "truncated", out.count >= 20000);
                char *json = cJSON_PrintUnformatted(result.get());
                if (!json)
                        throw std::bad_alloc();
                jstring value = env->NewStringUTF(json);
                cJSON_free(json);
                return value;
        } catch (const std::exception &e) {
                env->ThrowNew(env->FindClass("java/io/IOException"), e.what());
                return nullptr;
        }
}
