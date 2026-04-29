#include <algorithm>
#include <cctype>

#include "ImGuiFileDialog.h"
#include "ImGuiNotify.hpp"
#include "imgui.h"

#include "editor.hpp"
#include "gui.hpp"
#include "monitor.hpp"

const char *
Editor::dataTypeToStr(const DataType type)
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

Editor::DataType
Editor::strToDataType(const std::string &str)
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
Editor::readFile(const std::string &filename, std::string &outContent)
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
Editor::parseCfg(const cJSON *jsonNode, DataTree &node)
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
Editor::loadCfg(const std::string &cfgPath)
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
        return true;
}

bool
Editor::storeCfg(const std::string &cfgPath) const
{
        cJSON *root = cJSON_CreateObject();
        cJSON *cfg  = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "CFG", cfg);

        std::function<void(const DataTree &, cJSON *)> serialize = [&](const DataTree &node, cJSON *parent) {
                cJSON *obj = cJSON_CreateObject();
                cJSON_AddStringToObject(obj, "name", node.name.c_str());
                cJSON_AddStringToObject(obj, "type", dataTypeToStr(node.type));

                if (node.type == DataType::ARRAY) {
                        cJSON *arr = cJSON_CreateArray();
                        for (auto &child : node.children)
                                serialize(child, arr);
                        cJSON_AddItemToObject(obj, "array", arr);
                }

                if (parent->type == cJSON_Array)
                        cJSON_AddItemToArray(parent, obj);
                else
                        cJSON_AddItemToObject(parent, node.name.c_str(), obj);
        };

        for (auto &n : dataTree_.children)
                serialize(n, cfg);

        char         *out = cJSON_Print(root);
        std::ofstream ofs(cfgPath);
        ofs << out;
        cJSON_free(out);
        cJSON_Delete(root);

        return static_cast<bool>(ofs);
}

bool
Editor::parseBin(std::ifstream &ifs, DataTree &node)
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
Editor::loadBin(const std::string &binPath)
{
        std::ifstream ifs(binPath, std::ios::binary);
        if (!ifs.is_open())
                return false;

        for (auto &child : dataTree_.children) {
                if (!parseBin(ifs, child))
                        return false;
        }
        return true;
}

bool
Editor::loadElf(const std::string &elfPath)
{
        if (!ElfParser::parse(elfPath, elfInfo_))
                return false;
        dwarf::parse(elfInfo_, dwarfInfo_);
        return true;
}

void
Editor::restoreFromPaths(const std::string &cfg, const std::string &bin, const std::string &elf)
{
        if (!cfg.empty() && loadCfg(cfg))
                cfgPath_ = cfg;
        if (!bin.empty() && loadBin(bin))
                binPath_ = bin;
        if (!elf.empty() && loadElf(elf))
                elfPath_ = elf;
}

void
Editor::handleDroppedFile(const std::string &path)
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

bool
Editor::storeBin(const std::string &binPath) const
{
        std::ofstream ofs(binPath, std::ios::binary);
        if (!ofs.is_open())
                return false;

        std::function<void(const DataTree &)> writeNode = [&](const DataTree &node) {
                switch (node.type) {
                        case DataType::F32: {
                                f32 val = std::get<f32>(node.val);
                                ofs.write(reinterpret_cast<char *>(&val), sizeof(val));
                                break;
                        }
                        case DataType::U32: {
                                u32 val = std::get<u32>(node.val);
                                ofs.write(reinterpret_cast<char *>(&val), sizeof(val));
                                break;
                        }
                        case DataType::I32: {
                                i32 val = std::get<i32>(node.val);
                                ofs.write(reinterpret_cast<char *>(&val), sizeof(val));
                                break;
                        }
                        case DataType::ARRAY: {
                                for (auto &child : node.children)
                                        writeNode(child);
                                break;
                        }
                        default:
                                break;
                }
        };

        for (auto &n : dataTree_.children)
                writeNode(n);

        return static_cast<bool>(ofs);
}

// Follow typedef / const / volatile / restrict to the underlying type.
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

// Pretty-print a type as it would appear in C (best-effort, for the Type column).
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

// Returns "F32" / "U32" / "I32" if this resolves to a scalar (base, enum, or pointer);
// nullptr for struct / union / array (not directly droppable as one channel).
static const char *
scalarPayloadType(const dwarf::Info &info, u64 typeOff)
{
        const dwarf::Type *t = resolveAlias(info, typeOff);
        if (!t)
                return nullptr;
        if (t->kind == dwarf::TypeKind::BASE) {
                switch (t->encoding) {
                        case 0x04: // DW_ATE_float
                                return "F32";
                        case 0x05: // DW_ATE_signed
                        case 0x06: // DW_ATE_signed_char
                                return "I32";
                        case 0x02: // DW_ATE_boolean
                        case 0x07: // DW_ATE_unsigned
                        case 0x08: // DW_ATE_unsigned_char
                        default:
                                return "U32";
                }
        }
        if (t->kind == dwarf::TypeKind::ENUM)
                return "I32";
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

void
Editor::drawVarRow(const std::string &displayName, const std::string &fullPath, const u64 addr, const u64 typeOff,
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
Editor::drawElfSymbols()
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
                                          | ImGuiTableFlags_ScrollY;

        if (dwarfInfo_.present && !dwarfInfo_.variables.empty()) {
                if (ImGui::BeginTable("ElfVarTable", 4, flags, ImVec2(0, 0))) {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableHeadersRow();

                        for (const auto &v : dwarfInfo_.variables) {
                                if (!filter.empty() && v.name.find(filter) == std::string::npos)
                                        continue;
                                drawVarRow(v.name, v.name, v.addr, v.type, 0);
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
                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Bind", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Section", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableHeadersRow();

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
                                ImGui::SetDragDropPayload("CHANNEL", s.name.c_str(), s.name.size() + 1);
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
Editor::drawDataTree(DataTree &node, const int indentLevel)
{
        // 本节点行
        ImGui::TableNextRow();

        // ---------------- Column 0: Name (tree node or simple name) ----------------
        ImGui::TableSetColumnIndex(0);

        // 生成稳定唯一 ID 后缀（用节点地址）
        const auto        addr     = reinterpret_cast<uintptr_t>(&node);
        const std::string idSuffix = std::to_string(addr);

        // 缩进显示层级（仅视觉效果）
        ImGui::Indent(indentLevel * 10.0f);

        // 如果是 ARRAY，用一个可展开的 tree 小箭头；否则正常显示/编辑 name
        const bool isArray = (node.type == DataType::ARRAY);
        bool       opened  = false;
        if (isArray) {
                // 注意：不要使用 SpanFullWidth，否则会跨列并导致错位
                opened = ImGui::TreeNodeEx((std::string("##tree_") + idSuffix).c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                // 在 tree 前显示节点名称（TreeNodeEx 本身不打印文本，故也显示 name）
                ImGui::SameLine();
                // 可编辑名字：按 Enter 确认（使用 unique id）
                char nameBuf[128];
                snprintf(nameBuf, sizeof(nameBuf), "%s", node.name.c_str());
                if (ImGui::InputText((std::string("##name_") + idSuffix).c_str(),
                                     nameBuf,
                                     sizeof(nameBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                        node.name = nameBuf;
                }
        } else {
                // 非 array：直接显示可编辑 name（按 Enter 确认）
                char nameBuf[128];
                snprintf(nameBuf, sizeof(nameBuf), "%s", node.name.c_str());
                if (ImGui::InputText((std::string("##name_") + idSuffix).c_str(),
                                     nameBuf,
                                     sizeof(nameBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                        node.name = nameBuf;
                }
        }

        ImGui::Unindent(static_cast<f32>(indentLevel) * 10.0f);

        // ---------------- Column 1: Type (Combo) ----------------
        ImGui::TableSetColumnIndex(1);
        // Combo 显示需要确保每次使用唯一 id，否则同名会冲突
        const char *types[]     = {"f32", "u32", "i32", "array"};
        int         currentType = static_cast<int>(node.type);
        if (ImGui::Combo((std::string("##type_") + idSuffix).c_str(), &currentType, types, IM_ARRAYSIZE(types))) {
                node.type = static_cast<DataType>(currentType);
                // 当类型变化时，可能需要初始化 node.val 或 children（根据你的需求处理）
        }

        // ---------------- Column 2: Value (editable / disabled) ----------------
        ImGui::TableSetColumnIndex(2);
        switch (node.type) {
                case DataType::F32: {
                        // 确保 variant 中已有 f32（如果没有，可先初始化为 0）
                        if (!std::holds_alternative<f32>(node.val))
                                node.val = 0.0f;
                        f32 val = std::get<f32>(node.val);
                        if (ImGui::InputFloat((std::string("##val_") + idSuffix).c_str(), &val, 0.0f, 0.0f, "%.6f"))
                                node.val = val;
                        break;
                }
                case DataType::U32: {
                        if (!std::holds_alternative<u32>(node.val))
                                node.val = static_cast<u32>(0);
                        const u32 val = std::get<u32>(node.val);
                        int       tmp = static_cast<int>(val);
                        if (ImGui::InputInt((std::string("##val_") + idSuffix).c_str(), &tmp))
                                node.val = static_cast<u32>(tmp);
                        break;
                }
                case DataType::I32: {
                        if (!std::holds_alternative<i32>(node.val))
                                node.val = static_cast<i32>(0);
                        i32 val = std::get<i32>(node.val);
                        if (ImGui::InputInt((std::string("##val_") + idSuffix).c_str(), &val))
                                node.val = val;
                        break;
                }
                case DataType::ARRAY: {
                        ImGui::TextDisabled("(array)");
                        break;
                }
                default:
                        ImGui::TextDisabled("(unknown)");
                        break;
        }

        // ---------------- 子节点（如果是 array 并且展开） ----------------
        if (isArray && opened) {
                // 子节点应该从新的一行开始绘制，每个子节点都会调用 TableNextRow()
                for (auto &child : node.children) {
                        // drawDataTree(child, indentLevel + 1) 内部会调用 TableNextRow()
                        drawDataTree(child, indentLevel + 1);
                }
                ImGui::TreePop();
        }
}

void
Editor::menu()
{
        if (ImGui::BeginPopupContextWindow("EditorMenu")) {
                if (ImGui::MenuItem("Load CFG")) {
                        state_ = EditorState::LoadCfg;

                        const IGFD::FileDialogConfig cfg = {
                            .fileName     = "CFG",
                            .filePathName = ".",
                        };

                        ImGuiFileDialog::Instance()->OpenDialog("ChooseCfgFile", "choose CFG file", ".json", cfg);
                }

                if (ImGui::MenuItem("Load BIN")) {
                        state_ = EditorState::LoadBin;

                        const IGFD::FileDialogConfig bin = {
                            .fileName     = "BIN",
                            .filePathName = ".",
                        };

                        ImGuiFileDialog::Instance()->OpenDialog("ChooseBinFile", "choose BIN file", ".bin", bin);
                }

                if (ImGui::MenuItem("Store CFG")) {
                        state_ = EditorState::StoreCfg;

                        const IGFD::FileDialogConfig cfg = {
                            .fileName     = "CFG",
                            .filePathName = ".",
                        };

                        ImGuiFileDialog::Instance()->OpenDialog("StoreCfgFile", "save CFG file", ".json", cfg);
                }

                if (ImGui::MenuItem("Store BIN")) {
                        state_ = EditorState::StoreBin;

                        const IGFD::FileDialogConfig bin = {
                            .fileName     = "BIN",
                            .filePathName = ".",
                        };

                        ImGuiFileDialog::Instance()->OpenDialog("StoreBinFile", "save BIN file", ".bin", bin);
                }

                if (ImGui::MenuItem("Load ELF/AXF")) {
                        state_ = EditorState::LoadElf;

                        const IGFD::FileDialogConfig elf = {
                            .fileName     = "ELF",
                            .filePathName = ".",
                        };

                        ImGuiFileDialog::Instance()->OpenDialog(
                            "ChooseElfFile", "choose ELF/AXF file", ".elf,.axf,.out,.*", elf);
                }

                ImGui::EndPopup();
        }

        switch (state_) {
                case EditorState::LoadCfg: {
                        if (ImGuiFileDialog::Instance()->Display("ChooseCfgFile")) {
                                if (ImGuiFileDialog::Instance()->IsOk()) {
                                        cfgPath_ = ImGuiFileDialog::Instance()->GetFilePathName();

                                        if (loadCfg(cfgPath_))
                                                ImGui::InsertNotification({ImGuiToastType::Success,
                                                                           toastDismissTime_,
                                                                           "load CFG success: %s",
                                                                           cfgPath_.c_str()});
                                        else
                                                ImGui::InsertNotification(
                                                    {ImGuiToastType::Error, toastDismissTime_, "load CFG failure"});
                                }
                                state_ = EditorState::None;
                                ImGuiFileDialog::Instance()->Close();
                        }
                        break;
                }
                case EditorState::LoadBin: {
                        if (ImGuiFileDialog::Instance()->Display("ChooseBinFile")) {
                                if (ImGuiFileDialog::Instance()->IsOk()) {
                                        binPath_ = ImGuiFileDialog::Instance()->GetFilePathName();

                                        if (loadBin(binPath_))
                                                ImGui::InsertNotification({ImGuiToastType::Success,
                                                                           toastDismissTime_,
                                                                           "load BIN success: %s",
                                                                           binPath_.c_str()});
                                        else
                                                ImGui::InsertNotification(
                                                    {ImGuiToastType::Error, toastDismissTime_, "load BIN failure"});
                                }
                                state_ = EditorState::None;
                                ImGuiFileDialog::Instance()->Close();
                        }
                        break;
                }
                case EditorState::StoreCfg: {
                        if (ImGuiFileDialog::Instance()->Display("StoreCfgFile")) {
                                if (ImGuiFileDialog::Instance()->IsOk()) {
                                        cfgPath_ = ImGuiFileDialog::Instance()->GetFilePathName();

                                        if (storeCfg(cfgPath_))
                                                ImGui::InsertNotification({ImGuiToastType::Success,
                                                                           toastDismissTime_,
                                                                           "store CFG success: %s",
                                                                           cfgPath_.c_str()});
                                        else
                                                ImGui::InsertNotification(
                                                    {ImGuiToastType::Error, toastDismissTime_, "store CFG failure"});
                                }
                                state_ = EditorState::None;
                                ImGuiFileDialog::Instance()->Close();
                        }
                        break;
                }
                case EditorState::StoreBin: {
                        if (ImGuiFileDialog::Instance()->Display("StoreBinFile")) {
                                if (ImGuiFileDialog::Instance()->IsOk()) {
                                        binPath_ = ImGuiFileDialog::Instance()->GetFilePathName();

                                        if (storeBin(binPath_))
                                                ImGui::InsertNotification({ImGuiToastType::Success,
                                                                           toastDismissTime_,
                                                                           "store BIN success: %s",
                                                                           binPath_.c_str()});
                                        else
                                                ImGui::InsertNotification(
                                                    {ImGuiToastType::Error, toastDismissTime_, "store BIN failure"});
                                }
                                state_ = EditorState::None;
                                ImGuiFileDialog::Instance()->Close();
                        }
                        break;
                }
                case EditorState::LoadElf: {
                        if (ImGuiFileDialog::Instance()->Display("ChooseElfFile")) {
                                if (ImGuiFileDialog::Instance()->IsOk()) {
                                        elfPath_ = ImGuiFileDialog::Instance()->GetFilePathName();

                                        if (loadElf(elfPath_))
                                                ImGui::InsertNotification({ImGuiToastType::Success,
                                                                           toastDismissTime_,
                                                                           "load ELF success: %zu symbols",
                                                                           elfInfo_.symbols.size()});
                                        else
                                                ImGui::InsertNotification(
                                                    {ImGuiToastType::Error, toastDismissTime_, "load ELF failure"});
                                }
                                state_ = EditorState::None;
                                ImGuiFileDialog::Instance()->Close();
                        }
                        break;
                }
                default:
                        break;
        }
}

void
Editor::draw()
{
        if (ImGui::BeginTabBar("EditorTabs", ImGuiTabBarFlags_None)) {
                if (ImGui::BeginTabItem("CFG")) {
                        if (ImGui::BeginTable("DataEditorTable",
                                              3,
                                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                                  | ImGuiTableFlags_Resizable)) {
                                ImGui::TableSetupColumn("Name");
                                ImGui::TableSetupColumn("Type");
                                ImGui::TableSetupColumn("Value");
                                ImGui::TableHeadersRow();

                                for (auto &child : dataTree_.children)
                                        drawDataTree(child);

                                ImGui::EndTable();
                        }
                        ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("ELF Symbols")) {
                        drawElfSymbols();
                        ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
        }
}

void
Editor::updateDisplay()
{
        if (ImGui::Begin(name_.c_str())) {
                if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
                        for (const auto &drops = Gui::getDroppedFiles(); const auto &p : drops)
                                handleDroppedFile(p);
                }

                menu();
                draw();
                ImGui::End();
        }
}
