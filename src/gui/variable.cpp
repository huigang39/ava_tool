#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <functional>

#include "native_dlg.hpp"
#include "ImGuiNotify.hpp"
#include "imgui.h"

#include "gui/variable.hpp"
#include "app_log.hpp"
#include "gui/gui.hpp"
#include "gui/monitor.hpp"
#include "core/jlink_dev.hpp"

// Port headers
#include "inc/jlinkport.h"
#include "inc/net.h"
#include "inc/shm.h"

// Helper to decode raw bytes to string based on DataType
static std::string decodeValue(const u8* raw, DataType type) {
    if (!raw) return "...";
    char buf[64];
    switch (type) {
        case DataType::U8:  snprintf(buf, sizeof(buf), "%u", *raw); break;
        case DataType::I8:  snprintf(buf, sizeof(buf), "%d", *(i8*)raw); break;
        case DataType::U16: snprintf(buf, sizeof(buf), "%u", *(u16*)raw); break;
        case DataType::I16: snprintf(buf, sizeof(buf), "%d", *(i16*)raw); break;
        case DataType::U32: snprintf(buf, sizeof(buf), "%u", *(u32*)raw); break;
        case DataType::I32: snprintf(buf, sizeof(buf), "%d", *(i32*)raw); break;
        case DataType::F32: snprintf(buf, sizeof(buf), "%.4f", *(f32*)raw); break;
        case DataType::U64: snprintf(buf, sizeof(buf), "%llu", *(u64*)raw); break;
        case DataType::I64: snprintf(buf, sizeof(buf), "%lld", *(i64*)raw); break;
        case DataType::F64: snprintf(buf, sizeof(buf), "%.6f", *(f64*)raw); break;
        default: return "???";
    }
    return buf;
}

bool
Variable::loadCfg(const std::string &cfgPath)
{
        JsonParser jp;
        if (!jp.parse(cfgPath))
                return false;
        dataTree_ = jp.getDataTree();
        cfgPath_ = cfgPath;
        elfPath_.clear(); 
        binPath_.clear();
        rebuildSearchPool();
        return true;
}

bool
Variable::loadBin(const std::string &binPath)
{
        BinParser bp;
        bp.setTemplate(dataTree_); 
        if (!bp.parse(binPath))
                return false;
        dataTree_ = bp.getDataTree();
        binPath_  = binPath;
        elfPath_.clear();
        rebuildSearchPool();
        return true;
}

bool
Variable::loadElf(const std::string &elfPath)
{
        if (isElfLoading_)
                return false;

        elfPath_ = elfPath;
        isElfLoading_ = true;
        
        auto task = std::make_shared<ElfLoadingTask>();
        currentLoadingTask_ = task;

        std::thread([this, elfPath, task]() {
                LOG_I("Async Load ELF start: %s", elfPath.c_str());
                u64 start = get_mono_ts_ms();
                
                ElfParser ep;
                if (!ep.parse(elfPath)) {
                        isElfLoading_ = false;
                        return;
                }
                if (task->aborted) return;

                auto info = ep.getElfInfo();
                dwarf::Info dwarf;
                dwarf::parse(info, dwarf);
                if (task->aborted) return;

                // Build search pool in background WITHOUT lock
                std::vector<SearchEntry> localPool;
                if (dwarf.present) {
                    for (const auto &v : dwarf.variables) {
                        flattenDwarfType(localPool, dwarf, v.name, v.addr, v.type, 0);
                    }
                }

                {
                        std::lock_guard lk(mtxElf_);
                        if (task->aborted) return;
                        
                        elfInfo_    = std::move(info);
                        dwarfInfo_  = std::move(dwarf);
                        searchPool_ = std::move(localPool);
                        
                        cfgPath_.clear();
                        binPath_.clear();
                        
                        // Sync addresses
                        i32 syncCount = 0;
                        for (auto &v : vars_) {
                                if (v.port != PortType::JLINK) continue;
                                for (const auto &se : searchPool_) {
                                        if (se.path == v.name) {
                                                v.addr = se.addr;
                                                v.typeOff = se.typeOff;
                                                syncCount++;
                                                break;
                                        }
                                }
                        }
                        if (syncCount > 0) {
                                LOG_I("Synced %d addresses in %s", syncCount, name_.c_str());
                        }

                        try {
                                if (std::filesystem::exists(elfPath))
                                        elfLastWriteTime_ = std::filesystem::last_write_time(elfPath);
                        } catch (...) {}
                }

                elfReloaded_  = true;
                isElfLoading_ = false;
                
                u64 end = get_mono_ts_ms();
                LOG_I("Async Load ELF finished: %llu ms", 
                      end - start);
        }).detach();

        return true;
}

void
Variable::handleDroppedFile(const std::string &path)
{
        const auto dot = path.find_last_of('.');
        std::string ext = (dot == std::string::npos) ? "" : path.substr(dot);
        std::ranges::transform(ext, ext.begin(), [](const unsigned char c) { return std::tolower(c); });

        if (ext == ".elf" || ext == ".axf" || ext == ".out") {
                if (loadElf(path))
                        ImGui::InsertNotification({ImGuiToastType::Success, toastDismissTime_, "Loaded ELF: %s", path.c_str()});
                else
                        ImGui::InsertNotification({ImGuiToastType::Error, toastDismissTime_, "Failed to load ELF"});
        } else if (ext == ".json") {
                if (loadCfg(path))
                        ImGui::InsertNotification({ImGuiToastType::Success, toastDismissTime_, "Loaded JSON: %s", path.c_str()});
                else
                        ImGui::InsertNotification({ImGuiToastType::Error, toastDismissTime_, "Failed to load JSON"});
        } else if (ext == ".bin") {
                if (loadBin(path))
                        ImGui::InsertNotification({ImGuiToastType::Success, toastDismissTime_, "Loaded BIN: %s", path.c_str()});
                else
                        ImGui::InsertNotification({ImGuiToastType::Error, toastDismissTime_, "Failed to load BIN"});
        }
}

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
                                if (sz == 1) return "I8";
                                if (sz == 2) return "I16";
                                if (sz == 8) return "I64";
                                return "I32";
                        case 0x02: // DW_ATE_boolean
                        case 0x07: // DW_ATE_unsigned
                        case 0x08: // DW_ATE_unsigned_char
                        default:
                                if (sz == 1) return "U8";
                                if (sz == 2) return "U16";
                                if (sz == 8) return "U64";
                                return "U32";
                }
        }
        if (t->kind == dwarf::TypeKind::ENUM) {
                const u64 sz = t->size ? t->size : 4;
                if (sz == 1) return "I8";
                if (sz == 2) return "I16";
                if (sz == 8) return "I64";
                return "I32";
        }
        if (t->kind == dwarf::TypeKind::POINTER)
                return "U32";
        return nullptr;
}

static void
fillEnumPayload(const dwarf::Info &info, u64 typeOff, ChannelDropPayload &p)
{
        const dwarf::Type *t = resolveAlias(info, typeOff);
        if (!t || t->kind != dwarf::TypeKind::ENUM) {
                p.numEnums = 0;
                return;
        }
        p.numEnums = (u8)std::min((int)t->enums.size(), ChannelDropPayload::kMaxEnums);
        for (int i = 0; i < p.numEnums; ++i) {
                snprintf(p.enums[i].name, sizeof(p.enums[i].name), "%s", t->enums[i].name.c_str());
                p.enums[i].value = t->enums[i].value;
        }
}

static u64
typeSize(const dwarf::Info &info, u64 typeOff)
{
        const dwarf::Type *t = resolveAlias(info, typeOff);
        if (!t) return 0;
        if (t->kind == dwarf::TypeKind::ARRAY) {
                if (t->size > 0) return t->size;
                u64 elem = typeSize(info, t->inner);
                u64 total = elem ? elem : 1;
                for (const u64 d : t->dims) total *= (d ? d : 1);
                return total;
        }
        return t->size;
}

void
Variable::rebuildSearchPool()
{
    std::vector<SearchEntry> newPool;
    if (dwarfInfo_.present) {
        for (const auto &v : dwarfInfo_.variables) {
            flattenDwarfType(newPool, dwarfInfo_, v.name, v.addr, v.type, 0);
        }
    }
    if (!dataTree_.children.empty()) {
        for (const auto &n : dataTree_.children) {
            flattenDataTree(newPool, n.name, n);
        }
    }
    searchPool_ = std::move(newPool);
}

void
Variable::flattenDwarfType(std::vector<SearchEntry> &pool, const dwarf::Info &info, const std::string &parentPath, u64 parentAddr, u64 typeOff, int depth)
{
    if (depth > 8) return; 
    const dwarf::Type *t = resolveAlias(info, typeOff);
    if (!t) return;

    if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
        if (!parentPath.empty()) {
            SearchEntry e;
            e.path = parentPath; e.addr = parentAddr; e.type = DataType::U32; e.defaultPort = PortType::JLINK; e.typeOff = typeOff;
            pool.push_back(e);
        }
        for (const auto &m : t->members) {
            std::string path = parentPath + "." + (m.name.empty() ? "<anon>" : m.name);
            flattenDwarfType(pool, info, path, parentAddr + m.offset, m.type, depth + 1);
        }
    } else if (t->kind == dwarf::TypeKind::ARRAY) {
        if (!parentPath.empty()) {
            SearchEntry e;
            e.path = parentPath; e.addr = parentAddr; e.type = DataType::U32; e.defaultPort = PortType::JLINK; e.typeOff = typeOff;
            pool.push_back(e);
        }
        u64 elemSize = typeSize(info, t->inner);
        u64 dim = t->dims.empty() ? 0 : t->dims.front();
        u64 displayed = (dim == 0) ? 0 : (dim < 8 ? dim : 8);
        for (u64 i = 0; i < displayed; ++i) {
            std::string path = parentPath + "[" + std::to_string(i) + "]";
            flattenDwarfType(pool, info, path, parentAddr + i * elemSize, t->inner, depth + 1);
        }
    } else {
        const char* sType = scalarPayloadType(info, typeOff);
        if (sType) {
            SearchEntry e;
            e.path = parentPath; e.addr = parentAddr; e.type = Parser::strToDataType(sType); e.defaultPort = PortType::JLINK; e.typeOff = typeOff;
            pool.push_back(e);
        } else if (!parentPath.empty()) {
            SearchEntry e;
            e.path = parentPath; e.addr = parentAddr; e.type = DataType::U32; e.defaultPort = PortType::JLINK; e.typeOff = typeOff;
            pool.push_back(e);
        }
    }
}

void
Variable::flattenDataTree(std::vector<SearchEntry> &pool, const std::string &parentPath, const DataTree &node)
{
    if (node.type == DataType::ARRAY) {
        for (size_t i = 0; i < node.children.size() && i < 32; ++i) {
            flattenDataTree(pool, parentPath + "[" + std::to_string(i) + "]", node.children[i]);
        }
    } else {
        SearchEntry e;
        e.path = parentPath; e.addr = 0; e.type = node.type; e.defaultPort = PortType::MANUAL; e.typeOff = 0;
        pool.push_back(e);
    }
}

void
Variable::drawSymbolTree()
{
    std::lock_guard lk(mtxElf_);
    if (dwarfInfo_.present && !dwarfInfo_.variables.empty()) {
        if (ImGui::BeginTable("SymbolTreeTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (const auto &v : dwarfInfo_.variables) {
                drawSymbolLeaf(v.name, v.name, v.addr, v.type, 0);
            }
            ImGui::EndTable();
        }
    } else if (!dataTree_.children.empty()) {
        if (ImGui::BeginTable("BinTreeTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Value");
            ImGui::TableHeadersRow();
            for (auto &n : dataTree_.children) drawDataTreeLeaf(n);
            ImGui::EndTable();
        }
    } else {
        ImGui::TextDisabled("Drop an ELF or BIN file to browse symbols.");
    }
}

void
Variable::drawSymbolLeaf(const std::string &displayName, const std::string &fullPath, u64 addr, u64 typeOff, i32 depth)
{
    std::lock_guard lk(mtxElf_);
    if (depth > 16) return;
    const dwarf::Type *t = resolveAlias(dwarfInfo_, typeOff);
    const bool isStruct = t && (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION);
    const bool isArray = t && t->kind == dwarf::TypeKind::ARRAY;
    const bool isEnum = t && t->kind == dwarf::TypeKind::ENUM;
    const bool expandable = (isStruct && !t->members.empty()) || (isArray && !t->dims.empty()) || isEnum;
    const char *scalarKind = scalarPayloadType(dwarfInfo_, typeOff);

    ImGui::PushID((int)addr ^ (depth << 16) ^ (int)typeOff);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    
    bool open = false;
    if (expandable) {
        open = ImGui::TreeNodeEx(displayName.c_str(), ImGuiTreeNodeFlags_OpenOnArrow);
    } else {
        ImGui::TreeNodeEx(displayName.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
    }

    // Drag support
    if (ImGui::BeginDragDropSource()) {
        ChannelDropPayload p{};
        snprintf(p.name, sizeof(p.name), "%s", fullPath.c_str());
        p.addr = addr;
        if (scalarKind) {
            snprintf(p.type, sizeof(p.type), "%s", scalarKind);
        } else if (isStruct) {
            snprintf(p.type, sizeof(p.type), "STRUCT");
        } else if (isArray) {
            snprintf(p.type, sizeof(p.type), "ARRAY");
        }
        snprintf(p.device, sizeof(p.device), "JLINK");
        p.numBytes = (u8)typeSize(dwarfInfo_, typeOff);
        p.typeOff = typeOff;
        fillEnumPayload(dwarfInfo_, typeOff, p);
        ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
        ImGui::Text("Dragging %s", fullPath.c_str());
        ImGui::EndDragDropSource();
    }
    // Double click to add
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        if (scalarKind) {
            VarEntry v;
            v.name = fullPath; v.type = Parser::strToDataType(scalarKind); v.port = PortType::JLINK; v.addr = addr; v.writable = true;
            v.typeOff = typeOff;
            vars_.push_back(v);
            ImGui::InsertNotification({ImGuiToastType::Success, 2000, "Added %s to watch list", fullPath.c_str()});
        } else if (isStruct || isArray) {
            addRecursive(fullPath, addr, typeOff, PortType::JLINK);
        }
    }

    ImGui::TableSetColumnIndex(1); ImGui::Text("0x%08llX", (unsigned long long)addr);
    ImGui::TableSetColumnIndex(2); ImGui::Text("%llu", (unsigned long long)typeSize(dwarfInfo_, typeOff));
    ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(prettyType(dwarfInfo_, typeOff).c_str());

    if (open) {
        if (isStruct) {
            for (const auto &m : t->members) {
                drawSymbolLeaf(m.name.empty() ? "<anon>" : m.name, fullPath + "." + (m.name.empty() ? "<anon>" : m.name), addr + m.offset, m.type, depth + 1);
            }
        } else if (isArray) {
            u64 elemSize = typeSize(dwarfInfo_, t->inner);
            u64 dim = t->dims.empty() ? 0 : t->dims.front();
            u64 displayed = (dim == 0) ? 0 : (dim < elfArrayMaxElems_ ? dim : elfArrayMaxElems_);
            for (u64 i = 0; i < displayed; ++i) {
                std::string idx = "[" + std::to_string(i) + "]";
                drawSymbolLeaf(idx, fullPath + idx, addr + i * elemSize, t->inner, depth + 1);
            }
        } else if (isEnum) {
            for (const auto &e : t->enums) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TreeNodeEx(e.name.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth);
                ImGui::TableSetColumnIndex(1); ImGui::Text("%lld", (long long)e.value);
            }
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void
Variable::drawDataTreeLeaf(DataTree &node, const int indentLevel)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    
    bool isArray = (node.type == DataType::ARRAY);
    bool open = false;
    if (isArray) {
        open = ImGui::TreeNodeEx(node.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
    } else {
        ImGui::TreeNodeEx(node.name.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            VarEntry v;
            v.name = node.name; v.type = node.type; v.port = PortType::MANUAL; v.addr = 0; v.writable = false;
            vars_.push_back(v);
        }

        if (ImGui::BeginDragDropSource()) {
            ChannelDropPayload p{};
            snprintf(p.name, sizeof(p.name), "%s", node.name.c_str());
            p.addr = 0;
            snprintf(p.type, sizeof(p.type), "%s", Parser::dataTypeToStr(node.type));
            snprintf(p.device, sizeof(p.device), "LOCAL");
            p.numBytes = (u8)Parser::typeBytes(node.type);
            p.typeOff = 0;
            ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
            ImGui::Text("Dragging %s", node.name.c_str());
            ImGui::EndDragDropSource();
        }
    }

    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(Parser::dataTypeToStr(node.type));
    ImGui::TableSetColumnIndex(2);
    if (std::holds_alternative<f32>(node.val)) ImGui::Text("%f", std::get<f32>(node.val));
    else if (std::holds_alternative<u32>(node.val)) ImGui::Text("%u", std::get<u32>(node.val));
    else if (std::holds_alternative<i32>(node.val)) ImGui::Text("%d", std::get<i32>(node.val));

    if (open && isArray) {
        for (auto &child : node.children) drawDataTreeLeaf(child, indentLevel + 1);
        ImGui::TreePop();
    }
}

void
Variable::drawSymbolBrowser()
{
    ImGui::SeparatorText("Symbol Browser");
    if (isElfLoading_) {
        ImGui::Text("Loading symbols...");
        ImGui::SameLine();
        static float ang = 0.0f; ang += 0.1f;
        ImGui::Text("%c", "|/-\\"[(int)(ang) % 4]);
        return;
    }

    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##symbol_search", "Search symbols (e.g. motor.pos)...", searchBuf_, sizeof(searchBuf_))) {
        searchResults_.clear();
        std::string query = searchBuf_;
        if (!query.empty()) {
            std::ranges::transform(query, query.begin(), [](unsigned char c){ return std::tolower(c); });
            for (const auto &e : searchPool_) {
                std::string lowPath = e.path;
                std::ranges::transform(lowPath, lowPath.begin(), [](unsigned char c){ return std::tolower(c); });
                if (lowPath.find(query) != std::string::npos) {
                    // Filter: if we already have a parent of this path in searchResults_, don't add this one.
                    // This ensures that if a struct matches, its members are only visible inside it.
                    bool hasParent = false;
                    for (const auto &res : searchResults_) {
                        if (e.path.size() > res.path.size() && e.path.starts_with(res.path) && (e.path[res.path.size()] == '.' || e.path[res.path.size()] == '[')) {
                            hasParent = true;
                            break;
                        }
                    }
                    if (!hasParent) {
                        searchResults_.push_back(e);
                    }
                    if (searchResults_.size() > 500) break; 
                }
            }
        }
    }

    if (searchBuf_[0] == '\0') {
        drawSymbolTree();
    } else {
        constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("SymbolSearchTable", 4, flags, ImVec2(0, 0))) {
            ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Size",    ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Type",    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (int i = 0; i < (int)searchResults_.size(); ++i) {
                const auto &e = searchResults_[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                const dwarf::Type *t = resolveAlias(dwarfInfo_, e.typeOff);
                const bool isComplex = t && (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION || t->kind == dwarf::TypeKind::ARRAY);

                ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow;
                if (!isComplex) nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

                bool open = ImGui::TreeNodeEx(e.path.c_str(), nodeFlags);

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    addRecursive(e.path, e.addr, e.typeOff, e.defaultPort);
                }

                if (ImGui::BeginDragDropSource()) {
                    ChannelDropPayload p{};
                    snprintf(p.name, sizeof(p.name), "%s", e.path.c_str());
                    p.addr = e.addr;
                    if (isComplex) {
                        snprintf(p.type, sizeof(p.type), "STRUCT");
                    } else {
                        snprintf(p.type, sizeof(p.type), "%s", Parser::dataTypeToStr(e.type));
                    }
                    snprintf(p.device, sizeof(p.device), e.defaultPort == PortType::JLINK ? "JLINK" : "SHM");
                    p.numBytes = (u8)Parser::typeBytes(e.type);
                    p.typeOff = e.typeOff;
                    fillEnumPayload(dwarfInfo_, e.typeOff, p);
                    ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                    ImGui::Text("Dragging %s", e.path.c_str());
                    ImGui::EndDragDropSource();
                }

                ImGui::TableSetColumnIndex(1); ImGui::Text("0x%08llX", (unsigned long long)e.addr);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%llu", (unsigned long long)typeSize(dwarfInfo_, e.typeOff));
                ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(prettyType(dwarfInfo_, e.typeOff).c_str());

                if (open && isComplex) {
                    if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
                        for (const auto &m : t->members)
                            drawSymbolLeaf(m.name.empty() ? "<anon>" : m.name,
                                           e.path + "." + (m.name.empty() ? "<anon>" : m.name),
                                           e.addr + m.offset, m.type, 1);
                    } else if (t->kind == dwarf::TypeKind::ARRAY) {
                        u64 elemSize  = typeSize(dwarfInfo_, t->inner);
                        u64 dim       = t->dims.empty() ? 0 : t->dims.front();
                        u64 displayed = dim < (u64)elfArrayMaxElems_ ? dim : (u64)elfArrayMaxElems_;
                        for (u64 j = 0; j < displayed; ++j) {
                            std::string idx = "[" + std::to_string(j) + "]";
                            drawSymbolLeaf(idx, e.path + idx, e.addr + j * elemSize, t->inner, 1);
                        }
                    }
                    ImGui::TreePop();
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
}

void
Variable::drawVariableList()
{
    if (ImGui::Button("Add Variable")) state_ = WindowState::AddVariable;
    ImGui::SameLine();
    if (ImGui::Button("Clear All")) { vars_.clear(); isModified_ = true; }
    ImGui::SameLine();
    if (ImGui::Button("Load File...")) state_ = WindowState::LoadElf; 
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    if (ImGui::SliderInt("Refresh(ms)", (int*)&updateIntervalMs_, 10, 2000)) { isModified_ = true; }

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("VarMonitorTable", 5, flags, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Value",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type",    ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Port",    ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)vars_.size(); ++i) {
            auto &v = vars_[i];
            const bool isSelected = v.selected;
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            
            const dwarf::Type *t = nullptr;
            {
                std::lock_guard lk(mtxElf_);
                t = resolveAlias(dwarfInfo_, v.typeOff);
            }
            const bool isComplex = t && (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION || t->kind == dwarf::TypeKind::ARRAY);
            
            ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow;
            if (isSelected) nodeFlags |= ImGuiTreeNodeFlags_Selected;
            if (!isComplex) nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

            bool open = ImGui::TreeNodeEx(v.name.c_str(), nodeFlags & ~ImGuiTreeNodeFlags_SpanFullWidth);

            // Selection logic
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                if (ImGui::GetIO().KeyCtrl) {
                    v.selected = !v.selected;
                } else if (ImGui::GetIO().KeyShift && lastSelectedIndex_ != -1) {
                    int start = std::min(lastSelectedIndex_, i);
                    int end = std::max(lastSelectedIndex_, i);
                    for (int j = start; j <= end; ++j) vars_[j].selected = true;
                } else {
                    for (auto &var : vars_) var.selected = false;
                    v.selected = true;
                }
                lastSelectedIndex_ = i;
            }

            bool pendingDelete   = false;
            bool pendingEnumEdit = false;
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Delete")) {
                    pendingDelete = true;
                }
                const bool isEnumType = (t && t->kind == dwarf::TypeKind::ENUM) || !v.enumDefs.empty();
                if (!isComplex && isEnumType && ImGui::MenuItem("Edit Enum Definition...")) {
                    pendingEnumEdit = true;
                }
                ImGui::EndPopup();
            }

            if (pendingDelete) {
                vars_.erase(vars_.begin() + i);
                isModified_ = true;
                lastSelectedIndex_ = -1;
                if (open && isComplex) ImGui::TreePop();
                ImGui::PopID();
                break;
            }
            if (pendingEnumEdit)
                enumEditIdx_ = i;

            if (ImGui::BeginDragDropSource()) {
                ChannelDropPayload p{};
                snprintf(p.name, sizeof(p.name), "%s", v.name.c_str());
                p.addr = v.addr;
                if (isComplex) {
                    snprintf(p.type, sizeof(p.type), t->kind == dwarf::TypeKind::ARRAY ? "ARRAY" : "STRUCT");
                } else {
                    snprintf(p.type, sizeof(p.type), "%s", Parser::dataTypeToStr(v.type));
                }
                snprintf(p.device, sizeof(p.device), v.port == PortType::JLINK ? "JLINK" : (v.port == PortType::SHM ? "SHM" : "LOCAL"));
                if (v.port == PortType::SHM)
                    snprintf(p.shmName, sizeof(p.shmName), "%s", v.shm.name);
                p.numBytes = (u8)Parser::typeBytes(v.type);
                p.typeOff = v.typeOff;
                fillEnumPayload(dwarfInfo_, v.typeOff, p);
                ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                ImGui::Text("Dragging %s", v.name.c_str());
                ImGui::EndDragDropSource();
            }

            // Value
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(v.valueStr.c_str());

            // Type
            ImGui::TableSetColumnIndex(2);
            if (isComplex) {
                ImGui::TextUnformatted(t->kind == dwarf::TypeKind::ARRAY ? "ARRAY" : "STRUCT");
            } else if ((t && t->kind == dwarf::TypeKind::ENUM) || !v.enumDefs.empty()) {
                ImGui::TextUnformatted("ENUM");
            } else {
                ImGui::TextUnformatted(Parser::dataTypeToStr(v.type));
            }

            // Address
            ImGui::TableSetColumnIndex(3);
            if (v.port == PortType::UDP) {
                ImGui::Text("%s:%d", v.udp.ip, v.udp.port);
            } else if (v.port == PortType::SHM) {
                ImGui::TextUnformatted(v.shm.name);
            } else {
                ImGui::Text("0x%08llX", (unsigned long long)v.addr);
            }

            // Port
            ImGui::TableSetColumnIndex(4);
            const char* portNames[] = {"JLINK", "UDP", "SHM", "MANUAL"};
            ImGui::TextUnformatted(portNames[(int)v.port]);

            if (open && isComplex) {
                {
                    std::lock_guard lk(mtxElf_);
                    drawVarVarTreeRow(v.name, v.addr, v.typeOff, 1, v.port, v.shm.name);
                }
                ImGui::TreePop();
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("SYMBOL_CHANNEL");
        if (!payload) payload = ImGui::AcceptDragDropPayload("CHANNEL");

        if (payload) {
            auto *p = static_cast<ChannelDropPayload *>(payload->Data);
            VarEntry v;
            v.name = p->name;
            v.addr = p->addr;
            v.writable = true;
            v.typeOff = p->typeOff;
            
            if (strcmp(p->device, "SHM") == 0) {
                v.port = PortType::SHM;
                snprintf(v.shm.name, sizeof(v.shm.name), "%s", p->name);
            } else if (strcmp(p->device, "LOCAL") == 0 || strcmp(p->device, "MANUAL") == 0) {
                v.port = PortType::MANUAL;
                v.writable = false; // Manual/Bin data is usually readonly in this context
            } else {
                v.port = PortType::JLINK;
            }

            if (strcmp(p->type, "STRUCT") == 0 || strcmp(p->type, "ARRAY") == 0) {
                v.type = DataType::U32; // Placeholder
            } else {
                v.type = Parser::strToDataType(p->type);
            }
            vars_.push_back(v);
        }
        ImGui::EndDragDropTarget();
    }
}

void
Variable::drawAddVariableDialog()
{
    if (state_ != WindowState::AddVariable) return;
    ImGui::OpenPopup("Add New Variable");
    if (ImGui::BeginPopupModal("Add New Variable", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Name", newVar_.name, sizeof(newVar_.name));
        static const char* types[] = {"U8", "U16", "U32", "U64", "I8", "I16", "I32", "I64", "F32", "F64"};
        static int typeIdx = 2;
        if (ImGui::Combo("Type", &typeIdx, types, IM_ARRAYSIZE(types))) newVar_.type = Parser::strToDataType(types[typeIdx]);
        static const char* ports[] = {"JLINK", "UDP", "SHM"};
        static int portIdx = 0;
        if (ImGui::Combo("Port", &portIdx, ports, IM_ARRAYSIZE(ports))) newVar_.port = (PortType)portIdx;

        if (newVar_.port == PortType::JLINK) {
            ImGui::InputText("Address (Hex)", newVar_.addrBuf, sizeof(newVar_.addrBuf));
            try { newVar_.addr = std::stoull(newVar_.addrBuf, nullptr, 16); } catch(...) { newVar_.addr = 0; }
        } else if (newVar_.port == PortType::UDP) {
            ImGui::InputText("Target IP", newVar_.udpIp, sizeof(newVar_.udpIp));
            ImGui::InputInt("Target Port", &newVar_.udpPort);
            ImGui::InputText("Offset/Addr", newVar_.addrBuf, sizeof(newVar_.addrBuf));
            try { newVar_.addr = std::stoull(newVar_.addrBuf, nullptr, 16); } catch(...) { newVar_.addr = 0; }
        } else if (newVar_.port == PortType::SHM) {
            ImGui::InputText("SHM Name", newVar_.shmName, sizeof(newVar_.shmName));
            ImGui::InputText("Offset", newVar_.addrBuf, sizeof(newVar_.addrBuf));
            try { newVar_.addr = std::stoull(newVar_.addrBuf, nullptr, 16); } catch(...) { newVar_.addr = 0; }
        }

        ImGui::Checkbox("Writable", &newVar_.writable);
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            VarEntry v;
            v.name = newVar_.name; v.type = newVar_.type; v.port = newVar_.port; v.addr = newVar_.addr; v.writable = newVar_.writable;
            if (v.port == PortType::UDP) { snprintf(v.udp.ip, sizeof(v.udp.ip), "%s", newVar_.udpIp); v.udp.port = (u16)newVar_.udpPort; }
            if (v.port == PortType::SHM) { snprintf(v.shm.name, sizeof(v.shm.name), "%s", newVar_.shmName); v.shm.inited = false; }
            vars_.push_back(v);
            isModified_ = true;
            state_ = WindowState::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { state_ = WindowState::None; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

void
Variable::drawEnumEditPopup()
{
    if (enumEditIdx_ < 0 || enumEditIdx_ >= (int)vars_.size()) {
        enumEditIdx_ = -1;
        return;
    }
    ImGui::OpenPopup("Edit Enum Definition");
    if (ImGui::BeginPopupModal("Edit Enum Definition", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        VarEntry &v = vars_[enumEditIdx_];

        // Pre-populate from DWARF on first open (when user hasn't set anything yet)
        if (v.enumDefs.empty()) {
            std::lock_guard lk(mtxElf_);
            const dwarf::Type *et = resolveAlias(dwarfInfo_, v.typeOff);
            if (et && et->kind == dwarf::TypeKind::ENUM) {
                for (const auto &e : et->enums)
                    v.enumDefs.push_back({e.name, e.value});
            }
        }

        ImGui::Text("Variable: %s", v.name.c_str());
        ImGui::Separator();

        constexpr ImGuiTableFlags tfl = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("EnumDefTable", 3, tfl, ImVec2(400, 200))) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 24.0f);
            ImGui::TableHeadersRow();

            int deleteIdx = -1;
            for (int j = 0; j < (int)v.enumDefs.size(); ++j) {
                ImGui::PushID(j);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::SetNextItemWidth(-1);
                char nameBuf[64];
                snprintf(nameBuf, sizeof(nameBuf), "%s", v.enumDefs[j].name.c_str());
                if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
                    v.enumDefs[j].name = nameBuf;
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-1);
                int ival = (int)v.enumDefs[j].value;
                if (ImGui::InputInt("##val", &ival, 0, 0))
                    v.enumDefs[j].value = ival;
                ImGui::TableSetColumnIndex(2);
                if (ImGui::SmallButton("x")) deleteIdx = j;
                ImGui::PopID();
            }
            if (deleteIdx >= 0)
                v.enumDefs.erase(v.enumDefs.begin() + deleteIdx);
            ImGui::EndTable();
        }

        if (ImGui::Button("+ Add")) v.enumDefs.push_back({"", 0});
        ImGui::SameLine();
        if (ImGui::Button("Clear All")) v.enumDefs.clear();
        ImGui::SameLine(0, 40);
        if (ImGui::Button("Close", ImVec2(80, 0))) {
            isModified_ = true;
            enumEditIdx_ = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

static void
populateShmMemberCache(const dwarf::Info &info, u64 baseAddr, u64 typeOff,
                       const u8 *blob, usize blobSize,
                       std::unordered_map<u64, std::string> &cache)
{
    const dwarf::Type *t = resolveAlias(info, typeOff);
    if (!t) return;
    if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
        for (const auto &m : t->members) {
            if (m.offset >= blobSize) continue;
            u64 memberAddr = baseAddr + m.offset;
            const char *sType = scalarPayloadType(info, m.type);
            if (sType) {
                u32 sz = Parser::typeBytes(Parser::strToDataType(sType));
                if (m.offset + sz <= blobSize)
                    cache[memberAddr] = decodeValue(blob + m.offset, Parser::strToDataType(sType));
            } else {
                populateShmMemberCache(info, memberAddr, m.type,
                                       blob + m.offset, blobSize - m.offset, cache);
            }
        }
    } else if (t->kind == dwarf::TypeKind::ARRAY) {
        u64 elemSize = typeSize(info, t->inner);
        if (elemSize == 0) return;
        u64 dim = t->dims.empty() ? 0 : t->dims.front();
        const char *sType = scalarPayloadType(info, t->inner);
        for (u64 i = 0; i < dim; ++i) {
            u64 offset = i * elemSize;
            if (offset >= blobSize) break;
            u64 memberAddr = baseAddr + offset;
            if (sType) {
                u32 sz = Parser::typeBytes(Parser::strToDataType(sType));
                if (offset + sz <= blobSize)
                    cache[memberAddr] = decodeValue(blob + offset, Parser::strToDataType(sType));
            } else {
                populateShmMemberCache(info, memberAddr, t->inner,
                                       blob + offset, blobSize - offset, cache);
            }
        }
    }
}

void
Variable::updateVariables()
{
    u64 now = get_mono_ts_ms();
    if (now - lastUpdateTs_ < updateIntervalMs_) return;
    lastUpdateTs_ = now;

    for (auto &v : vars_) {
        if (v.is_editing) continue;

        const dwarf::Type *t = resolveAlias(dwarfInfo_, v.typeOff);
        const bool isComplex = t && (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION || t->kind == dwarf::TypeKind::ARRAY);
        if (isComplex) {
            v.valueStr = "...";
            // For SHM structs: read the full blob and populate the member cache so the
            // struct browser can display live values without requiring JLink.
            if (v.port == PortType::SHM) {
                if (!v.shm.inited) {
                    shm_cfg_t cfg = {v.shm.name, SHM_READWRITE, 4096};
                    if (shm_init(&v.shm.handle, cfg) == 0) v.shm.inited = true;
                }
                if (v.shm.inited) {
                    usize sz = typeSize(dwarfInfo_, v.typeOff);
                    if (sz > 0 && sz <= 4096) {
                        std::vector<u8> blob(sz);
                        if (shm_read(&v.shm.handle, blob.data(), sz) == sz)
                            populateShmMemberCache(dwarfInfo_, v.addr, v.typeOff,
                                                   blob.data(), sz, memberValueCache_);
                    }
                }
            }
            continue;
        }

        u8 buf[8];
        int ret = -2; // -2: No update, -1: Error, 0: Success
        u32 sz = Parser::typeBytes(v.type);
        if (v.port == PortType::JLINK && JLinkDev::instance().isConnected()) {
            if (JLinkDev::instance().readMem((u32)v.addr, sz, buf)) ret = 0;
            else ret = -1;
        } else if (v.port == PortType::SHM) {
            if (!v.shm.inited) {
                shm_cfg_t cfg = { v.shm.name, SHM_READWRITE, 4096 };
                if (shm_init(&v.shm.handle, cfg) == 0) v.shm.inited = true;
                else ret = -1;
            }
            if (v.shm.inited) {
                if (shm_read(&v.shm.handle, buf, sz) == sz) {
                    ret = 0;
                } else {
                    ret = -2;
                }
            }
        }
        
        if (ret == 0) {
            const dwarf::Type *et = resolveAlias(dwarfInfo_, v.typeOff);
            const bool isEnum = (et && et->kind == dwarf::TypeKind::ENUM) || !v.enumDefs.empty();
            if (isEnum) {
                i64 ival = 0;
                std::memcpy(&ival, buf, std::min((size_t)sz, sizeof(ival)));
                v.valueStr = decodeValue(buf, v.type);
                // User-defined defs take priority over DWARF
                bool found = false;
                for (const auto &e : v.enumDefs)
                    if (e.value == ival) { v.valueStr = e.name; found = true; break; }
                if (!found && et && et->kind == dwarf::TypeKind::ENUM)
                    for (const auto &e : et->enums)
                        if (e.value == ival) { v.valueStr = e.name; break; }
            } else {
                v.valueStr = decodeValue(buf, v.type);
            }
        } else if (ret == -1) v.valueStr = "ERR";
        else if (v.valueStr.empty()) v.valueStr = "...";
    }
}

void
Variable::writeVariable(const VarEntry &v, const std::string &newVal)
{
    u8 buf[8];
    u32 sz = Parser::typeBytes(v.type);
    try {
        if (v.type == DataType::F32) *(f32*)buf = std::stof(newVal);
        else if (v.type == DataType::U32) *(u32*)buf = (u32)std::stoul(newVal);
        else if (v.type == DataType::I32) *(i32*)buf = (i32)std::stol(newVal);
        else if (v.type == DataType::U16) *(u16*)buf = (u16)std::stoul(newVal);
        else if (v.type == DataType::I16) *(i16*)buf = (i16)std::stol(newVal);
        else if (v.type == DataType::U8)  *(u8*)buf = (u8)std::stoul(newVal);
        else if (v.type == DataType::I8)  *(i8*)buf = (i8)std::stol(newVal);
        else if (v.type == DataType::F64) *(f64*)buf = std::stod(newVal);
        else if (v.type == DataType::U64) *(u64*)buf = std::stoull(newVal);
        else if (v.type == DataType::I64) *(i64*)buf = std::stoll(newVal);
        
        if (v.port == PortType::JLINK && JLinkDev::instance().isConnected()) {
            jlink_port_write_mem((u32)v.addr, sz, buf);
        } else if (v.port == PortType::SHM && v.shm.inited) {
            shm_write(const_cast<shm_t*>(&v.shm.handle), buf, sz);
        }
    } catch (...) {
        ImGui::InsertNotification({ImGuiToastType::Error, 2000, "Invalid input for %s", v.name.c_str()});
    }
}

void
Variable::draw()
{
    f32 availY = ImGui::GetContentRegionAvail().y;
    f32 splitterSize = 8.0f;
    f32 topHeight = watchListHeight_;
    f32 bottomHeight = availY - topHeight - splitterSize;
    if (bottomHeight < 100.0f) {
        bottomHeight = 100.0f;
        topHeight = availY - bottomHeight - splitterSize;
    }

    if (ImGui::BeginChild("TopSection", ImVec2(0, topHeight), false)) {
        drawVariableList();
    }
    ImGui::EndChild();

    ImGui::Button("##Splitter", ImVec2(-1, splitterSize));
    if (ImGui::IsItemActive()) {
        watchListHeight_ += ImGui::GetIO().MouseDelta.y;
    }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    if (ImGui::BeginChild("BottomSection", ImVec2(0, 0), false)) {
        drawSymbolBrowser();
    }
    ImGui::EndChild();

    drawAddVariableDialog();
    drawEnumEditPopup();
    if (state_ == WindowState::LoadElf) {
        state_ = WindowState::None;
        std::string p = nativeDlgOpen("Choose Symbol File",
                                      {{"Symbol Files", {"elf", "axf", "out", "json", "bin"}}});
        if (!p.empty()) handleDroppedFile(p);
    }
    for (auto &path : Gui::getDroppedFiles()) handleDroppedFile(path);
}

void
Variable::updateDisplay()
{
    if (!elfPath_.empty()) {
        try {
            if (std::filesystem::exists(elfPath_)) {
                auto currentWriteTime = std::filesystem::last_write_time(elfPath_);
                if (elfLastWriteTime_ != std::filesystem::file_time_type{} && currentWriteTime > elfLastWriteTime_) {
                    if (loadElf(elfPath_)) { ImGui::InsertNotification({ImGuiToastType::Info, 3000, "ELF Hot-Reloaded: %s", elfPath_.c_str()}); elfReloaded_ = true; }
                }
                elfLastWriteTime_ = currentWriteTime;
            }
        } catch (...) {}
    }
    updateVariables();
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(name_.c_str(), &open_)) draw();
    ImGui::End();
}

void
Variable::save(void *node) const
{
    cJSON *root = static_cast<cJSON *>(node);
    cJSON *vArr = cJSON_CreateArray();
    for (const auto &v : vars_) {
        cJSON *vObj = cJSON_CreateObject();
        cJSON_AddStringToObject(vObj, "name", v.name.c_str());
        cJSON_AddNumberToObject(vObj, "type", (int)v.type);
        cJSON_AddNumberToObject(vObj, "port", (int)v.port);
        cJSON_AddNumberToObject(vObj, "addr", (double)v.addr);
        cJSON_AddBoolToObject(vObj, "writable", v.writable);
        cJSON_AddNumberToObject(vObj, "typeOff", (double)v.typeOff);
        if (v.port == PortType::UDP) {
            cJSON_AddStringToObject(vObj, "ip", v.udp.ip);
            cJSON_AddNumberToObject(vObj, "uport", v.udp.port);
        } else if (v.port == PortType::SHM) {
            cJSON_AddStringToObject(vObj, "shmName", v.shm.name);
        }
        cJSON_AddItemToArray(vArr, vObj);
    }
    cJSON_AddItemToObject(root, "vars", vArr);
}

void
Variable::load(const void *node)
{
    const cJSON *root = static_cast<const cJSON *>(node);
    cJSON *vArr = cJSON_GetObjectItem(root, "vars");
    if (cJSON_IsArray(vArr)) {
        vars_.clear();
        for (int i = 0; i < cJSON_GetArraySize(vArr); ++i) {
            cJSON *vObj = cJSON_GetArrayItem(vArr, i);
            VarEntry v;
            if (!cJSON_GetObjectItem(vObj, "name")) continue;
            v.name = cJSON_GetObjectItem(vObj, "name")->valuestring;
            v.type = (DataType)cJSON_GetObjectItem(vObj, "type")->valueint;
            v.port = (PortType)cJSON_GetObjectItem(vObj, "port")->valueint;
            v.addr = (u64)cJSON_GetObjectItem(vObj, "addr")->valuedouble;
            v.writable = cJSON_IsTrue(cJSON_GetObjectItem(vObj, "writable"));
            if (cJSON_GetObjectItem(vObj, "typeOff")) v.typeOff = (u64)cJSON_GetObjectItem(vObj, "typeOff")->valuedouble;
            if (v.port == PortType::UDP) {
                if (cJSON_GetObjectItem(vObj, "ip")) snprintf(v.udp.ip, sizeof(v.udp.ip), "%s", cJSON_GetObjectItem(vObj, "ip")->valuestring);
                if (cJSON_GetObjectItem(vObj, "uport")) v.udp.port = (u16)cJSON_GetObjectItem(vObj, "uport")->valueint;
            } else if (v.port == PortType::SHM) {
                if (cJSON_GetObjectItem(vObj, "shmName")) snprintf(v.shm.name, sizeof(v.shm.name), "%s", cJSON_GetObjectItem(vObj, "shmName")->valuestring);
            }
            vars_.push_back(v);
        }
    }
}

void
Variable::addRecursive(const std::string &fullPath, u64 addr, u64 typeOff, PortType port)
{
    const dwarf::Type *t = resolveAlias(dwarfInfo_, typeOff);
    if (!t) return;

    if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION || t->kind == dwarf::TypeKind::ARRAY) {
        VarEntry v;
        v.name = fullPath; v.type = DataType::U32; v.port = port; v.addr = addr; v.writable = true;
        v.typeOff = typeOff;
        vars_.push_back(v);
        isModified_ = true;
    } else {
        const char* sType = scalarPayloadType(dwarfInfo_, typeOff);
        if (sType) {
            VarEntry v;
            v.name = fullPath; v.type = Parser::strToDataType(sType); v.port = port; v.addr = addr; v.writable = true;
            v.typeOff = typeOff;
            vars_.push_back(v);
            isModified_ = true;
        }
    }
}

void
Variable::drawVarVarTreeRow(const std::string &fullPath, u64 addr, u64 typeOff, i32 depth, PortType port, const std::string &shmRegionName)
{
    if (depth > 16) return;
    const dwarf::Type *t = resolveAlias(dwarfInfo_, typeOff);
    if (!t) return;

    const char *devLabel = (port == PortType::SHM) ? "SHM" : (port == PortType::UDP) ? "UDP" : "JLINK";

    auto drawValueCell = [&](u64 memberAddr, const char *sType, u64 memberTypeOff) {
        auto it = memberValueCache_.find(memberAddr);
        const dwarf::Type *et = resolveAlias(dwarfInfo_, memberTypeOff);
        const bool isEnum = et && et->kind == dwarf::TypeKind::ENUM;
        auto decodeWithEnum = [&](const u8 *buf, u32 sz) -> std::string {
            if (!isEnum) return decodeValue(buf, Parser::strToDataType(sType));
            i64 ival = 0;
            std::memcpy(&ival, buf, std::min((size_t)sz, sizeof(ival)));
            for (const auto &e : et->enums)
                if (e.value == ival) return e.name;
            return decodeValue(buf, Parser::strToDataType(sType));
        };
        if (port == PortType::JLINK) {
            u8  buf[8]{};
            u32 sz         = Parser::typeBytes(Parser::strToDataType(sType));
            u64 now        = get_mono_ts_ms();
            bool shouldRead = (now - lastUpdateTs_ < 20);
            if (shouldRead && JLinkDev::instance().isConnected() &&
                JLinkDev::instance().readMem((u32)memberAddr, sz, buf)) {
                std::string val         = decodeWithEnum(buf, sz);
                memberValueCache_[memberAddr] = val;
                ImGui::TextUnformatted(val.c_str());
            } else if (it != memberValueCache_.end()) {
                ImGui::TextUnformatted(it->second.c_str());
            } else {
                ImGui::TextUnformatted("...");
            }
        } else {
            // SHM/UDP: cache is populated by updateVariables — just display
            if (it != memberValueCache_.end()) ImGui::TextUnformatted(it->second.c_str());
            else ImGui::TextUnformatted("...");
        }
    };

    if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
        for (const auto &m : t->members) {
            std::string memberPath = fullPath + "." + (m.name.empty() ? "<anon>" : m.name);
            u64         memberAddr = addr + m.offset;
            const char *sType      = scalarPayloadType(dwarfInfo_, m.type);
            bool        isComplex  = !sType;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow;
            if (!isComplex) nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            bool open = ImGui::TreeNodeEx(m.name.empty() ? "<anon>" : m.name.c_str(),
                                          nodeFlags & ~ImGuiTreeNodeFlags_SpanFullWidth);

            if (ImGui::BeginDragDropSource()) {
                ChannelDropPayload p{};
                snprintf(p.name, sizeof(p.name), "%s", memberPath.c_str());
                p.addr = memberAddr;
                snprintf(p.type, sizeof(p.type), "%s", sType ? sType : "STRUCT");
                snprintf(p.device, sizeof(p.device), "%s", devLabel);
                if (!shmRegionName.empty())
                    snprintf(p.shmName, sizeof(p.shmName), "%s", shmRegionName.c_str());
                p.numBytes = (u8)typeSize(dwarfInfo_, m.type);
                p.typeOff  = m.type;
                fillEnumPayload(dwarfInfo_, m.type, p);
                ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                ImGui::Text("Dragging %s", memberPath.c_str());
                ImGui::EndDragDropSource();
            }

            {
                const dwarf::Type *mt = resolveAlias(dwarfInfo_, m.type);
                const bool isMemberEnum = mt && mt->kind == dwarf::TypeKind::ENUM;
                ImGui::TableSetColumnIndex(1);
                if (sType) drawValueCell(memberAddr, sType, m.type);
                else ImGui::TextUnformatted("...");
                ImGui::TableSetColumnIndex(2);
                if (isMemberEnum) ImGui::TextUnformatted(mt->name.empty() ? "enum" : mt->name.c_str());
                else ImGui::TextUnformatted(sType ? sType : "STRUCT");
                ImGui::TableSetColumnIndex(3); ImGui::Text("0x%08llX", (unsigned long long)memberAddr);
                ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(devLabel);
            }

            if (open && isComplex) {
                drawVarVarTreeRow(memberPath, memberAddr, m.type, depth + 1, port, shmRegionName);
                ImGui::TreePop();
            }
        }
    } else if (t->kind == dwarf::TypeKind::ARRAY) {
        u64 elemSize  = typeSize(dwarfInfo_, t->inner);
        u64 dim       = t->dims.empty() ? 0 : t->dims.front();
        u64 displayed = (dim == 0) ? 0 : (dim < (u64)elfArrayMaxElems_ ? dim : (u64)elfArrayMaxElems_);
        for (u64 i = 0; i < displayed; ++i) {
            std::string idxStr     = "[" + std::to_string(i) + "]";
            std::string memberPath = fullPath + idxStr;
            u64         memberAddr = addr + i * elemSize;
            const char *sType      = scalarPayloadType(dwarfInfo_, t->inner);
            bool        isComplex  = !sType;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow;
            if (!isComplex) nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            bool open = ImGui::TreeNodeEx(idxStr.c_str(), nodeFlags & ~ImGuiTreeNodeFlags_SpanFullWidth);

            if (ImGui::BeginDragDropSource()) {
                ChannelDropPayload p{};
                snprintf(p.name, sizeof(p.name), "%s", memberPath.c_str());
                p.addr = memberAddr;
                snprintf(p.type, sizeof(p.type), "%s", sType ? sType : "ARRAY");
                snprintf(p.device, sizeof(p.device), "%s", devLabel);
                if (!shmRegionName.empty())
                    snprintf(p.shmName, sizeof(p.shmName), "%s", shmRegionName.c_str());
                p.numBytes = (u8)typeSize(dwarfInfo_, t->inner);
                p.typeOff  = t->inner;
                fillEnumPayload(dwarfInfo_, t->inner, p);
                ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                ImGui::Text("Dragging %s", memberPath.c_str());
                ImGui::EndDragDropSource();
            }

            {
                const dwarf::Type *et2 = resolveAlias(dwarfInfo_, t->inner);
                const bool isElemEnum = et2 && et2->kind == dwarf::TypeKind::ENUM;
                ImGui::TableSetColumnIndex(1);
                if (sType) drawValueCell(memberAddr, sType, t->inner);
                else ImGui::TextUnformatted("...");
                ImGui::TableSetColumnIndex(2);
                if (isElemEnum) ImGui::TextUnformatted(et2->name.empty() ? "enum" : et2->name.c_str());
                else ImGui::TextUnformatted(sType ? sType : "ARRAY");
                ImGui::TableSetColumnIndex(3); ImGui::Text("0x%08llX", (unsigned long long)memberAddr);
                ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(devLabel);
            }

            if (open && isComplex) {
                drawVarVarTreeRow(memberPath, memberAddr, t->inner, depth + 1, port, shmRegionName);
                ImGui::TreePop();
            }
        }
    }
}
