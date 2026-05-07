#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <functional>

#include "ImGuiFileDialog.h"
#include "ImGuiNotify.hpp"
#include "imgui.h"

#include "parser.hpp"
#include "gui.hpp"
#include "monitor.hpp"

const char *
Parser::dataTypeToStr(const DataType type)
{
        switch (type) {
                case DataType::F32:
                        return "f32";
                case DataType::U32:
                        return "u32";
                case DataType::I32:
                        return "i32";
                case DataType::ARRAY:
                        return "array";
                default:
                        return "unknown";
        }
}

Parser::DataType
Parser::strToDataType(const std::string &str)
{
        auto type = DataType::UNKNOWN;
        if (str == "f32")
                type = DataType::F32;
        else if (str == "u32")
                type = DataType::U32;
        else if (str == "i32")
                type = DataType::I32;
        else if (str == "array")
                type = DataType::ARRAY;

        return type;
}

bool
Parser::readFile(const std::string &filename, std::string &outContent)
{
        std::ifstream ifs(filename);
        if (!ifs.is_open())
                return false;

        std::stringstream ss;
        ss << ifs.rdbuf();
        outContent = ss.str();
        return true;
}

void
Parser::parseCfg(const cJSON *jsonNode, DataTree &node)
{
        const cJSON *nameItem = cJSON_GetObjectItem(jsonNode, "name");
        const cJSON *typeItem = cJSON_GetObjectItem(jsonNode, "type");

        if (!nameItem || !typeItem)
                return;

        node.name = nameItem->valuestring;

        if (const std::string typeStr = typeItem->valuestring; typeStr == "f32")
                node.type = DataType::F32;
        else if (typeStr == "u32")
                node.type = DataType::U32;
        else if (typeStr == "i32")
                node.type = DataType::I32;
        else if (typeStr == "array") {
                node.type = DataType::ARRAY;
                if (const cJSON *childrenJson = cJSON_GetObjectItem(jsonNode, "array")) {
                        for (const cJSON *child = childrenJson->child; child; child = child->next) {
                                DataTree childNode;
                                parseCfg(child, childNode);
                                node.children.push_back(std::move(childNode));
                        }
                }
        }
}

bool
Parser::loadCfg(const std::string &cfgPath)
{
        std::string content;
        if (!readFile(cfgPath, content))
                return false;

        cJSON *root = cJSON_Parse(content.c_str());
        if (!root)
                return false;

        const cJSON *cfgJson = cJSON_GetObjectItem(root, "CFG");
        if (!cfgJson) {
                cJSON_Delete(root);
                return false;
        }

        dataTree_.children.clear();
        for (const cJSON *f = cfgJson->child; f; f = f->next) {
                DataTree node;
                parseCfg(f, node);
                dataTree_.children.push_back(std::move(node));
        }

        cJSON_Delete(root);
        cfgPath_ = cfgPath;
        return true;
}

bool
Parser::parseBin(std::ifstream &ifs, DataTree &node)
{
        switch (node.type) {
                case DataType::F32: {
                        f32 val = 0.0f;
                        ifs.read(reinterpret_cast<char *>(&val), sizeof(f32));
                        if (!ifs)
                                return false;

                        node.val = val;
                        break;
                }
                case DataType::U32: {
                        u32 val = 0;
                        ifs.read(reinterpret_cast<char *>(&val), sizeof(u32));
                        if (!ifs)
                                return false;

                        node.val = val;
                        break;
                }
                case DataType::I32: {
                        i32 val = 0;
                        ifs.read(reinterpret_cast<char *>(&val), sizeof(i32));
                        if (!ifs)
                                return false;

                        node.val = val;
                        break;
                }
                case DataType::ARRAY: {
                        for (auto &child : node.children) {
                                if (!parseBin(ifs, child))
                                        return false;
                        }
                        break;
                }
                default:
                        return false;
        }
        return true;
}

bool
Parser::loadBin(const std::string &binPath)
{
        std::ifstream ifs(binPath, std::ios::binary);
        if (!ifs.is_open())
                return false;

        for (auto &child : dataTree_.children) {
                if (!parseBin(ifs, child))
                        return false;
        }
        binPath_ = binPath; // Persist path
        return true;
}

bool
Parser::loadElf(const std::string &elfPath)
{
        if (!ElfParser::parse(elfPath, elfInfo_))
                return false;
        dwarf::parse(elfInfo_, dwarfInfo_);
        elfPath_ = elfPath; // Persist path
        return true;
}

void
Parser::handleDroppedFile(const std::string &path)
{
        const auto dot = path.find_last_of('.');
        std::string ext = (dot == std::string::npos) ? "" : path.substr(dot);
        std::ranges::transform(ext, ext.begin(), [](const unsigned char c) { return std::tolower(c); });

        if (ext == ".elf" || ext == ".axf" || ext == ".out") {
                elfPath_ = path;
                if (loadElf(elfPath_))
                        ImGui::InsertNotification({ImGuiToastType::Success,
                                                   toastDismissTime_,
                                                   "load ELF success: %zu symbols",
                                                   elfInfo_.symbols.size()});
                else
                        ImGui::InsertNotification({ImGuiToastType::Error, toastDismissTime_, "load ELF failure"});
        } else if (ext == ".json") {
                cfgPath_ = path;
                if (loadCfg(cfgPath_))
                        ImGui::InsertNotification(
                            {ImGuiToastType::Success, toastDismissTime_, "load CFG success: %s", cfgPath_.c_str()});
                else
                        ImGui::InsertNotification({ImGuiToastType::Error, toastDismissTime_, "load CFG failure"});
        } else if (ext == ".bin") {
                binPath_ = path;
                if (loadBin(binPath_))
                        ImGui::InsertNotification(
                            {ImGuiToastType::Success, toastDismissTime_, "load BIN success: %s", binPath_.c_str()});
                else
                        ImGui::InsertNotification({ImGuiToastType::Error, toastDismissTime_, "load BIN failure"});
        } else {
                ImGui::InsertNotification(
                    {ImGuiToastType::Warning, toastDismissTime_, "unsupported file: %s", path.c_str()});
        }
}

// Internal helper functions for DWARF parsing...
static const dwarf::Type *
resolveAlias(const dwarf::Info &info, u64 typeOff)
{
        for (int guard = 0; guard < 32; ++guard) {
                const auto it = info.types.find(typeOff);
                if (it == info.types.end())
                        return nullptr;
                const dwarf::Type &t = it->second;
                if (t.kind == dwarf::TypeKind::TYPEDEF || t.kind == dwarf::TypeKind::MODIFIER) {
                        if (t.inner == 0)
                                return &t;
                        typeOff = t.inner;
                        continue;
                }
                return &t;
        }
        return nullptr;
}

static std::string
prettyType(const dwarf::Info &info, u64 typeOff, int depth = 0)
{
        if (depth > 16 || typeOff == 0)
                return "void";
        const auto it = info.types.find(typeOff);
        if (it == info.types.end())
                return "?";
        const dwarf::Type &t = it->second;

        switch (t.kind) {
                case dwarf::TypeKind::BASE:
                case dwarf::TypeKind::TYPEDEF:
                        return t.name.empty() ? "?" : t.name;
                case dwarf::TypeKind::POINTER:
                        return prettyType(info, t.inner, depth + 1) + " *";
                case dwarf::TypeKind::MODIFIER:
                        return prettyType(info, t.inner, depth + 1);
                case dwarf::TypeKind::ARRAY: {
                        std::string s = prettyType(info, t.inner, depth + 1);
                        for (const u64 d : t.dims) {
                                s += "[";
                                if (d > 0)
                                        s += std::to_string(d);
                                s += "]";
                        }
                        return s;
                }
                case dwarf::TypeKind::STRUCT:
                        return "struct " + (t.name.empty() ? std::string("{...}") : t.name);
                case dwarf::TypeKind::UNION:
                        return "union " + (t.name.empty() ? std::string("{...}") : t.name);
                case dwarf::TypeKind::ENUM:
                        return "enum " + (t.name.empty() ? std::string("{...}") : t.name);
                case dwarf::TypeKind::SUBROUTINE:
                        return "func";
                default:
                        return "?";
        }
}

static const char *
scalarPayloadType(const dwarf::Info &info, u64 typeOff)
{
        const dwarf::Type *t = resolveAlias(info, typeOff);
        if (!t)
                return nullptr;
        if (t->kind == dwarf::TypeKind::BASE) {
                const u64 sz = t->size;
                switch (t->encoding) {
                        case 0x04: // DW_ATE_float
                                return (sz == 8) ? "F64" : "F32";
                        case 0x05: // DW_ATE_signed
                        case 0x06: // DW_ATE_signed_char
                                if (sz == 1)
                                        return "I8";
                                if (sz == 2)
                                        return "I16";
                                if (sz == 8)
                                        return "I64";
                                return "I32";
                        case 0x02: // DW_ATE_boolean
                        case 0x07: // DW_ATE_unsigned
                        case 0x08: // DW_ATE_unsigned_char
                        default:
                                if (sz == 1)
                                        return "U8";
                                if (sz == 2)
                                        return "U16";
                                if (sz == 8)
                                        return "U64";
                                return "U32";
                }
        }
        if (t->kind == dwarf::TypeKind::ENUM) {
                const u64 sz = t->size ? t->size : 4;
                if (sz == 1)
                        return "I8";
                if (sz == 2)
                        return "I16";
                if (sz == 8)
                        return "I64";
                return "I32";
        }
        if (t->kind == dwarf::TypeKind::POINTER)
                return "U32";
        return nullptr;
}

static u64
typeSize(const dwarf::Info &info, u64 typeOff)
{
        const dwarf::Type *t = resolveAlias(info, typeOff);
        if (!t)
                return 0;
        if (t->kind == dwarf::TypeKind::ARRAY) {
                if (t->size > 0)
                        return t->size;
                u64 elem = typeSize(info, t->inner);
                u64 total = elem ? elem : 1;
                for (const u64 d : t->dims)
                        total *= (d ? d : 1);
                return total;
        }
        return t->size;
}

struct VarMatch {
        std::string displayName;
        std::string fullPath;
        u64         addr;
        u64         typeOff;
};

static void
searchVariablesRecursive(const dwarf::Info &info, const std::string &filter, const std::string &currentPath,
                         const u64 addr, const u64 typeOff, const int depth, std::vector<VarMatch> &matches)
{
        if (depth > 8)
                return;

        const dwarf::Type *t = resolveAlias(info, typeOff);
        if (!t)
                return;

        if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
                for (const auto &m : t->members) {
                        const std::string childName = m.name.empty() ? "<anon>" : m.name;
                        const std::string childPath = currentPath + "." + childName;

                        auto it = std::search(childName.begin(), childName.end(), filter.begin(), filter.end(),
                                              [](char a, char b) {
                                                      return std::tolower(static_cast<unsigned char>(a))
                                                             == std::tolower(static_cast<unsigned char>(b));
                                              });

                        if (it != childName.end()) {
                                matches.push_back({childName, childPath, addr + m.offset, m.type});
                        }
                        searchVariablesRecursive(info, filter, childPath, addr + m.offset, m.type, depth + 1, matches);
                }
        }
}

void
Parser::drawVarRow(const std::string &displayName, const std::string &fullPath, const u64 addr, const u64 typeOff,
                   const int depth)
{
        if (depth > 16)
                return;

        ImGui::TableNextRow();
        ImGui::PushID(static_cast<int>(addr ^ (static_cast<u64>(depth) << 32) ^ typeOff));

        const dwarf::Type *t            = resolveAlias(dwarfInfo_, typeOff);
        const bool         isStruct     = t && (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION);
        const bool         isArray      = t && t->kind == dwarf::TypeKind::ARRAY;
        const bool         isEnum       = t && t->kind == dwarf::TypeKind::ENUM;
        const bool         expandable   = (isStruct && !t->members.empty()) || (isArray && !t->dims.empty()) || isEnum;
        const char        *scalarKind   = scalarPayloadType(dwarfInfo_, typeOff);

        ImGui::TableSetColumnIndex(0);
        bool open = false;
        if (expandable) {
                ImGuiTreeNodeFlags flags = (depth == 0) ? 0 : ImGuiTreeNodeFlags_None;
                open                     = ImGui::TreeNodeEx("##node", flags, "%s", displayName.c_str());
        } else {
                ImGui::TreeNodeEx(displayName.c_str(),
                                  ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen
                                      | ImGuiTreeNodeFlags_SpanAvailWidth);
        }
        if (scalarKind && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ChannelDropPayload p{};
                snprintf(p.name, sizeof(p.name), "%s", fullPath.c_str());
                p.addr = addr;
                snprintf(p.type, sizeof(p.type), "%s", scalarKind);
                snprintf(p.device, sizeof(p.device), "%s", "JLINK");
                p.numBytes = static_cast<u8>(typeBytes(scalarKind));
                if (isEnum && t && !t->enums.empty()) {
                        const usize cap = static_cast<usize>(ChannelDropPayload::kMaxEnums);
                        const usize cnt = (t->enums.size() < cap) ? t->enums.size() : cap;
                        p.numEnums      = static_cast<u8>(cnt);
                        for (usize i = 0; i < cnt; ++i) {
                                snprintf(p.enums[i].name, sizeof(p.enums[i].name), "%s",
                                         t->enums[i].name.c_str());
                                p.enums[i].value = t->enums[i].value;
                        }
                }
                ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                ImGui::Text("Dragging %s [%s @ 0x%08llX]", fullPath.c_str(), scalarKind,
                            static_cast<unsigned long long>(addr));
                ImGui::EndDragDropSource();
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::Text("0x%08llX", static_cast<unsigned long long>(addr));

        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%llu", static_cast<unsigned long long>(typeSize(dwarfInfo_, typeOff)));

        ImGui::TableSetColumnIndex(3);
        const std::string typeStr = prettyType(dwarfInfo_, typeOff);
        ImGui::TextUnformatted(typeStr.c_str());
        if (isEnum && t && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("enum %s", t->name.empty() ? "" : t->name.c_str());
                ImGui::Separator();
                for (const auto &e : t->enums)
                        ImGui::Text("%s = %lld", e.name.c_str(), static_cast<long long>(e.value));
                ImGui::EndTooltip();
        }

        if (open && expandable) {
                if (isStruct) {
                        for (const auto &m : t->members) {
                                const std::string childName = m.name.empty() ? "<anon>" : m.name;
                                const std::string childPath = fullPath + "." + childName;
                                drawVarRow(childName, childPath, addr + m.offset, m.type, depth + 1);
                        }
                } else if (isArray) {
                        const u64 elemSize  = typeSize(dwarfInfo_, t->inner);
                        const u64 dim       = t->dims.empty() ? 0 : t->dims.front();
                        const u64 cap       = static_cast<u64>(elfArrayMaxElems_);
                        const u64 displayed = (dim == 0) ? 0 : (dim < cap ? dim : cap);
                        for (u64 i = 0; i < displayed; ++i) {
                                char idx[24];
                                snprintf(idx, sizeof(idx), "[%llu]", static_cast<unsigned long long>(i));
                                const std::string childPath = fullPath + idx;
                                drawVarRow(idx, childPath, addr + i * elemSize, t->inner, depth + 1);
                        }
                        if (dim > displayed) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextDisabled("... %llu more (raise limit)",
                                                    static_cast<unsigned long long>(dim - displayed));
                        }
                } else if (isEnum) {
                        for (const auto &e : t->enums) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TreeNodeEx(e.name.c_str(),
                                                  ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
                                ImGui::TableSetColumnIndex(3);
                                ImGui::Text("= %lld", static_cast<long long>(e.value));
                        }
                }
                ImGui::TreePop();
        }

        ImGui::PopID();
}

void
Parser::drawElfSymbols()
{
        ImGui::Text("File: %s", elfPath_.empty() ? "(none)" : elfPath_.c_str());
        if (!elfPath_.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("| %s | %s | %zu sym | %s %zu vars",
                                    elfInfo_.is64 ? "ELF64" : "ELF32",
                                    ElfParser::machineStr(elfInfo_.machine),
                                    elfInfo_.symbols.size(),
                                    dwarfInfo_.present ? "DWARF" : "no-dwarf",
                                    dwarfInfo_.variables.size());
        }

        ImGui::SetNextItemWidth(240.0f);
        ImGui::InputTextWithHint("##elf_filter", "filter by name...", elfFilter_, sizeof(elfFilter_));
        ImGui::SameLine();
        ImGui::Checkbox("OBJECT only", &elfFilterObjectsOnly_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::InputInt("array cap", &elfArrayMaxElems_);
        if (elfArrayMaxElems_ < 1)
                elfArrayMaxElems_ = 1;

        const std::string filter = elfFilter_;

        constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable
                                           | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable;

        if (dwarfInfo_.present && !dwarfInfo_.variables.empty()) {
                if (ImGui::BeginTable("ElfVarTable", 4, flags, ImVec2(0, 0))) {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortAscending, 140.0f);
                        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort);
                        ImGui::TableHeadersRow();

                        if (ImGuiTableSortSpecs *sorts_specs = ImGui::TableGetSortSpecs()) {
                                if (sorts_specs->SpecsDirty && sorts_specs->SpecsCount > 0) {
                                        const ImGuiTableColumnSortSpecs *sort_spec = &sorts_specs->Specs[0];
                                        bool ascending = (sort_spec->SortDirection == ImGuiSortDirection_Ascending);
                                        if (sort_spec->ColumnIndex == 0) {
                                                std::sort(dwarfInfo_.variables.begin(), dwarfInfo_.variables.end(),
                                                          [ascending](const auto &a, const auto &b) {
                                                                  return ascending ? (a.name < b.name) : (a.name > b.name);
                                                          });
                                        } else if (sort_spec->ColumnIndex == 1) {
                                                std::sort(dwarfInfo_.variables.begin(), dwarfInfo_.variables.end(),
                                                          [ascending](const auto &a, const auto &b) {
                                                                  return ascending ? (a.addr < b.addr) : (a.addr > b.addr);
                                                          });
                                        } else if (sort_spec->ColumnIndex == 2) {
                                                std::sort(dwarfInfo_.variables.begin(), dwarfInfo_.variables.end(),
                                                          [&](const auto &a, const auto &b) {
                                                                  u64 szA = typeSize(dwarfInfo_, a.type);
                                                                  u64 szB = typeSize(dwarfInfo_, b.type);
                                                                  return ascending ? (szA < szB) : (szA > szB);
                                                          });
                                        }
                                        sorts_specs->SpecsDirty = false;
                                }
                        }

                        if (filter.empty()) {
                                for (const auto &v : dwarfInfo_.variables) {
                                        drawVarRow(v.name, v.name, v.addr, v.type, 0);
                                }
                        } else {
                                std::vector<VarMatch> matches;
                                for (const auto &v : dwarfInfo_.variables) {
                                        auto it = std::search(v.name.begin(), v.name.end(), filter.begin(),
                                                              filter.end(), [](char a, char b) {
                                                                      return std::tolower(static_cast<unsigned char>(a))
                                                                             == std::tolower(
                                                                                 static_cast<unsigned char>(b));
                                                              });
                                        if (it != v.name.end()) {
                                                matches.push_back({v.name, v.name, v.addr, v.type});
                                        }
                                        searchVariablesRecursive(dwarfInfo_, filter, v.name, v.addr, v.type, 0, matches);
                                }

                                for (const auto &m : matches) {
                                        drawVarRow(m.displayName, m.fullPath, m.addr, m.typeOff, 0);
                                }
                        }

                        ImGui::EndTable();
                }
                return;
        }

        if (elfInfo_.symbols.empty())
                return;

        if (ImGui::BeginTable("ElfSymbolTable", 6, flags, ImVec2(0, 0))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortAscending, 140.0f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 80.0f);
                ImGui::TableSetupColumn("Bind", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 80.0f);
                ImGui::TableSetupColumn("Section", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 120.0f);
                ImGui::TableHeadersRow();

                if (ImGuiTableSortSpecs *sorts_specs = ImGui::TableGetSortSpecs()) {
                        if (sorts_specs->SpecsDirty && sorts_specs->SpecsCount > 0) {
                                const ImGuiTableColumnSortSpecs *sort_spec = &sorts_specs->Specs[0];
                                bool ascending = (sort_spec->SortDirection == ImGuiSortDirection_Ascending);
                                if (sort_spec->ColumnIndex == 0) {
                                        std::sort(elfInfo_.symbols.begin(), elfInfo_.symbols.end(),
                                                  [ascending](const auto &a, const auto &b) {
                                                          return ascending ? (a.name < b.name) : (a.name > b.name);
                                                  });
                                } else if (sort_spec->ColumnIndex == 1) {
                                        std::sort(elfInfo_.symbols.begin(), elfInfo_.symbols.end(),
                                                  [ascending](const auto &a, const auto &b) {
                                                          return ascending ? (a.addr < b.addr) : (a.addr > b.addr);
                                                  });
                                } else if (sort_spec->ColumnIndex == 2) {
                                        std::sort(elfInfo_.symbols.begin(), elfInfo_.symbols.end(),
                                                  [ascending](const auto &a, const auto &b) {
                                                          return ascending ? (a.size < b.size) : (a.size > b.size);
                                                  });
                                }
                                sorts_specs->SpecsDirty = false;
                        }
                }

                int rowId = 0;
                for (const auto &s : elfInfo_.symbols) {
                        if (elfFilterObjectsOnly_ && s.type != 1)
                                continue;
                        if (!filter.empty() && s.name.find(filter) == std::string::npos)
                                continue;

                        ImGui::TableNextRow();
                        ImGui::PushID(rowId++);

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Selectable(s.name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                                ChannelDropPayload p{};
                                snprintf(p.name, sizeof(p.name), "%s", s.name.c_str());
                                p.addr = s.addr;
                                if (s.size == 8)
                                        snprintf(p.type, sizeof(p.type), "U64");
                                else if (s.size == 2)
                                        snprintf(p.type, sizeof(p.type), "U16");
                                else if (s.size == 1)
                                        snprintf(p.type, sizeof(p.type), "U8");
                                else
                                        snprintf(p.type, sizeof(p.type), "U32");
                                snprintf(p.device, sizeof(p.device), "JLINK");
                                p.numBytes = static_cast<u8>(typeBytes(p.type));

                                ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                                ImGui::Text("Dragging %s", s.name.c_str());
                                ImGui::EndDragDropSource();
                        }

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("0x%08llX", static_cast<unsigned long long>(s.addr));

                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%llu", static_cast<unsigned long long>(s.size));

                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(ElfParser::typeStr(s.type));

                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextUnformatted(ElfParser::bindStr(s.bind));

                        ImGui::TableSetColumnIndex(5);
                        ImGui::TextUnformatted(s.section.c_str());

                        ImGui::PopID();
                }

                ImGui::EndTable();
        }
}

void
Parser::drawDataTree(DataTree &node, const int indentLevel)
{
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);

        const auto        addr     = reinterpret_cast<uintptr_t>(&node);
        const std::string idSuffix = std::to_string(addr);

        ImGui::Indent(indentLevel * 10.0f);

        const bool isArray = (node.type == DataType::ARRAY);
        bool       opened  = false;
        if (isArray) {
                opened = ImGui::TreeNodeEx((std::string("##tree_") + idSuffix).c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                ImGui::SameLine();
                ImGui::TextUnformatted(node.name.c_str());
        } else {
                ImGui::TextUnformatted(node.name.c_str());
        }

        ImGui::Unindent(static_cast<f32>(indentLevel) * 10.0f);

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(dataTypeToStr(node.type));

        ImGui::TableSetColumnIndex(2);
        switch (node.type) {
                case DataType::F32: {
                        if (std::holds_alternative<f32>(node.val))
                                ImGui::Text("%f", std::get<f32>(node.val));
                        break;
                }
                case DataType::U32: {
                        if (std::holds_alternative<u32>(node.val))
                                ImGui::Text("%u", std::get<u32>(node.val));
                        break;
                }
                case DataType::I32: {
                        if (std::holds_alternative<i32>(node.val))
                                ImGui::Text("%d", std::get<i32>(node.val));
                        break;
                }
                default:
                        break;
        }

        if (isArray && opened) {
                for (auto &child : node.children)
                        drawDataTree(child, indentLevel + 1);
                ImGui::TreePop();
        }
}

void
Parser::menu()
{
        // Removed right-click context menu as requested.
}

void
Parser::draw()
{
        ImGui::BeginTabBar("ParserTabs");
        if (ImGui::BeginTabItem("ELF / DWARF")) {
                if (ImGui::Button("Load ELF"))
                        state_ = ParserState::LoadElf;
                ImGui::Separator();
                drawElfSymbols();
                ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("JSON / BIN")) {
                if (ImGui::Button("Load JSON"))
                        state_ = ParserState::LoadCfg;
                ImGui::SameLine();
                if (ImGui::Button("Load BIN"))
                        state_ = ParserState::LoadBin;

                ImGui::Separator();
                if (ImGui::BeginTable("ParserDataTable", 3,
                                     ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                        ImGui::TableSetupColumn("Name");
                        ImGui::TableSetupColumn("Type");
                        ImGui::TableSetupColumn("Value");
                        ImGui::TableHeadersRow();
                        for (auto &n : dataTree_.children)
                                drawDataTree(n);
                        ImGui::EndTable();
                }
                ImGui::EndTabItem();
        }
        ImGui::EndTabBar();

        if (state_ == ParserState::LoadCfg) {
                IGFD::FileDialogConfig config;
                config.path = ".";
                ImGuiFileDialog::Instance()->OpenDialog("ChooseCfgKey", "Choose JSON File", ".json", config);
                state_ = ParserState::None;
        } else if (state_ == ParserState::LoadBin) {
                IGFD::FileDialogConfig config;
                config.path = ".";
                ImGuiFileDialog::Instance()->OpenDialog("ChooseBinKey", "Choose BIN File", ".bin", config);
                state_ = ParserState::None;
        } else if (state_ == ParserState::LoadElf) {
                IGFD::FileDialogConfig config;
                config.path = ".";
                ImGuiFileDialog::Instance()->OpenDialog("ChooseElfKey", "Choose ELF File", ".elf,.axf,.out", config);
                state_ = ParserState::None;
        }

        if (ImGuiFileDialog::Instance()->Display("ChooseCfgKey")) {
                if (ImGuiFileDialog::Instance()->IsOk()) {
                        cfgPath_ = ImGuiFileDialog::Instance()->GetFilePathName();
                        loadCfg(cfgPath_);
                }
                ImGuiFileDialog::Instance()->Close();
        }
        if (ImGuiFileDialog::Instance()->Display("ChooseBinKey")) {
                if (ImGuiFileDialog::Instance()->IsOk()) {
                        binPath_ = ImGuiFileDialog::Instance()->GetFilePathName();
                        loadBin(binPath_);
                }
                ImGuiFileDialog::Instance()->Close();
        }
        if (ImGuiFileDialog::Instance()->Display("ChooseElfKey")) {
                if (ImGuiFileDialog::Instance()->IsOk()) {
                        elfPath_ = ImGuiFileDialog::Instance()->GetFilePathName();
                        loadElf(elfPath_);
                }
                ImGuiFileDialog::Instance()->Close();
        }

        for (auto &path : Gui::getDroppedFiles())
                handleDroppedFile(path);
        Gui::clearDroppedFiles();
}

void
Parser::updateDisplay()
{
        // ELF Auto-Reload Logic
        if (!elfPath_.empty()) {
                try {
                        if (std::filesystem::exists(elfPath_)) {
                                auto currentWriteTime = std::filesystem::last_write_time(elfPath_);
                                if (elfLastWriteTime_ != std::filesystem::file_time_type{} &&
                                    currentWriteTime > elfLastWriteTime_) {
                                        if (loadElf(elfPath_)) {
                                                ImGui::InsertNotification({ImGuiToastType::Info, 3000,
                                                                           "ELF Hot-Reloaded: %s", elfPath_.c_str()});
                                                elfReloaded_ = true;
                                        }
                                }
                                elfLastWriteTime_ = currentWriteTime;
                        }
                } catch (...) {
                }
        }

        ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(name_.c_str(), &open_)) {
                draw();
        }
        ImGui::End();
}
