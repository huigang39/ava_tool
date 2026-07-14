#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <sstream>

#include "ImGuiNotify.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "platform/native_dlg.hpp"

#include "app_log.hpp"
#include "core/jlink_port.hpp"
#include "gui/audio_input.hpp"
#include "gui/gui.hpp"
#include "gui/i18n.hpp"
#include "gui/monitor.hpp"
#include "gui/tutorial_guide.hpp"
#include "gui/ui_theme.hpp"
#include "gui/variable.hpp"

// Port headers
#include "inc/jlinkport.h"
#include "inc/net.h"
#include "inc/shm.h"

// Helper to decode raw bytes to string based on DataType
static std::string
decodeValue(const u8 *raw, DataType type, u32 bitOffset = 0, u32 bitSize = 0)
{
        if (!raw)
                return "...";
        char buf[64];
        switch (type) {
                case DataType::U8: {
                        u8 v = *raw;
                        if (bitSize > 0)
                                v = (v >> bitOffset) & ((1 << bitSize) - 1);
                        snprintf(buf, sizeof(buf), "%u", v);
                        break;
                }
                case DataType::I8: {
                        u8 v = *raw;
                        if (bitSize > 0) {
                                v = (v >> bitOffset) & ((1 << bitSize) - 1);
                                if (v & (1 << (bitSize - 1)))
                                        v |= static_cast<u8>(~((1 << bitSize) - 1));
                                snprintf(buf, sizeof(buf), "%d", static_cast<i8>(v));
                        } else {
                                snprintf(buf, sizeof(buf), "%d", *(i8 *)raw);
                        }
                        break;
                }
                case DataType::U16: {
                        u16 v = *(u16 *)raw;
                        if (bitSize > 0)
                                v = (v >> bitOffset) & ((1 << bitSize) - 1);
                        snprintf(buf, sizeof(buf), "%u", v);
                        break;
                }
                case DataType::I16: {
                        u16 v = *(u16 *)raw;
                        if (bitSize > 0) {
                                v = (v >> bitOffset) & ((1 << bitSize) - 1);
                                if (v & (1 << (bitSize - 1)))
                                        v |= static_cast<u16>(~((1 << bitSize) - 1));
                                snprintf(buf, sizeof(buf), "%d", static_cast<i16>(v));
                        } else {
                                snprintf(buf, sizeof(buf), "%d", *(i16 *)raw);
                        }
                        break;
                }
                case DataType::U32: {
                        u32 v = *(u32 *)raw;
                        if (bitSize > 0)
                                v = (v >> bitOffset) & ((1ULL << bitSize) - 1);
                        snprintf(buf, sizeof(buf), "%u", v);
                        break;
                }
                case DataType::I32: {
                        u32 v = *(u32 *)raw;
                        if (bitSize > 0) {
                                v = (v >> bitOffset) & ((1ULL << bitSize) - 1);
                                if (v & (1ULL << (bitSize - 1)))
                                        v |= static_cast<u32>(~((1ULL << bitSize) - 1));
                                snprintf(buf, sizeof(buf), "%d", static_cast<i32>(v));
                        } else {
                                snprintf(buf, sizeof(buf), "%d", *(i32 *)raw);
                        }
                        break;
                }
                case DataType::F32:
                        snprintf(buf, sizeof(buf), "%.6f", *(f32 *)raw);
                        break;
                case DataType::U64: {
                        u64 v = *(u64 *)raw;
                        if (bitSize > 0)
                                v = (v >> bitOffset) & ((1ULL << bitSize) - 1);
                        snprintf(buf, sizeof(buf), "%llu", v);
                        break;
                }
                case DataType::I64: {
                        u64 v = *(u64 *)raw;
                        if (bitSize > 0) {
                                v = (v >> bitOffset) & ((1ULL << bitSize) - 1);
                                if (v & (1ULL << (bitSize - 1)))
                                        v |= ~((1ULL << bitSize) - 1);
                                snprintf(buf, sizeof(buf), "%lld", static_cast<i64>(v));
                        } else {
                                snprintf(buf, sizeof(buf), "%lld", *(i64 *)raw);
                        }
                        break;
                }
                case DataType::F64:
                        snprintf(buf, sizeof(buf), "%.8f", *(f64 *)raw);
                        break;
                default:
                        snprintf(buf, sizeof(buf), "Unknown");
                        break;
        }
        return buf;
}

static std::string
absoluteDisplayPath(const std::string &path)
{
        if (path.empty())
                return {};
        std::error_code       ec;
        std::filesystem::path p(path);
        if (!p.is_absolute())
                p = std::filesystem::absolute(p, ec);
        if (ec)
                return path;
        return p.lexically_normal().string();
}

static const char *
portName(PortType port)
{
        switch (port) {
                case PortType::JLINK:
                        return "JLINK";
                case PortType::UDP:
                        return "UDP";
                case PortType::SHM:
                        return "SHM";
                case PortType::MANUAL:
                        return "MANUAL";
                case PortType::LOCAL:
                        return "LOCAL";
                case PortType::AUDIO:
                        return "AUDIO";
                default:
                        return "JLINK";
        }
}

static void
drawAudioDeviceCombo(int &deviceIndex, char *deviceName, size_t deviceNameSize)
{
        auto &audio = AudioInput::instance();
        if (audio.devices().empty())
                audio.refreshDevices();
        if (deviceIndex < 0)
                deviceIndex = audio.defaultDeviceIndex();
        std::string cur = audio.deviceName(deviceIndex);
        if (deviceName && deviceNameSize > 0)
                snprintf(deviceName, deviceNameSize, "%s", cur.c_str());

        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::BeginCombo("##audioDevice", cur.c_str())) {
                for (const auto &dev : audio.devices()) {
                        const bool selected = dev.index == deviceIndex;
                        if (ImGui::Selectable(dev.name.c_str(), selected)) {
                                deviceIndex = dev.index;
                                if (deviceName && deviceNameSize > 0)
                                        snprintf(deviceName, deviceNameSize, "%s", dev.name.c_str());
                        }
                        if (selected)
                                ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Audio input device", "音频输入设备"));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Refresh##auddev", "刷新##auddev"))) {
                audio.refreshDevices();
                if (deviceIndex < 0)
                        deviceIndex = audio.defaultDeviceIndex();
                std::string refreshed = audio.deviceName(deviceIndex);
                if (deviceName && deviceNameSize > 0)
                        snprintf(deviceName, deviceNameSize, "%s", refreshed.c_str());
        }
}

bool
Variable::loadCfg(const std::string &cfgPath)
{
        JsonParser jp;
        if (!jp.parse(cfgPath))
                return false;
        dataTree_ = jp.getDataTree();
        cfgPath_  = cfgPath;
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

        elfPath_      = elfPath;
        isElfLoading_ = true;

        auto task           = std::make_shared<ElfLoadingTask>();
        currentLoadingTask_ = task;

        std::thread([this, elfPath, task]() {
                LOG_I("Async Load ELF start: %s", elfPath.c_str());
                u64 start = get_mono_ts_ms();

                ElfParser ep;
                if (!ep.parse(elfPath)) {
                        isElfLoading_ = false;
                        return;
                }
                if (task->aborted)
                        return;

                auto        info = ep.getElfInfo();
                dwarf::Info dwarf;
                dwarf::parse(info, dwarf);
                if (task->aborted)
                        return;

                // Build search pool in background WITHOUT lock
                std::vector<SearchEntry> localPool;
                if (dwarf.present) {
                        for (const auto &v : dwarf.variables) {
                                flattenDwarfType(localPool, dwarf, v.name, v.addr, v.type, 0);
                        }
                }

                {
                        std::lock_guard lk(mtxElf_);
                        if (task->aborted)
                                return;

                        elfInfo_    = std::move(info);
                        dwarfInfo_  = std::move(dwarf);
                        searchPool_ = std::move(localPool);

                        cfgPath_.clear();
                        binPath_.clear();

                        // Sync addresses
                        i32 syncCount = 0;
                        for (auto &v : vars_) {
                                if (v.port != PortType::JLINK)
                                        continue;
                                for (const auto &se : searchPool_) {
                                        if (se.path == v.name) {
                                                v.addr    = se.addr;
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
                        } catch (...) {
                        }
                }

                elfReloaded_  = true;
                isElfLoading_ = false;

                u64 end = get_mono_ts_ms();
                LOG_I("Async Load ELF finished: %llu ms", end - start);
        }).detach();

        return true;
}

void
Variable::handleDroppedFile(const std::string &path)
{
        const auto  dot = path.find_last_of('.');
        std::string ext = (dot == std::string::npos) ? "" : path.substr(dot);
        std::ranges::transform(ext, ext.begin(), [](const unsigned char c) { return std::tolower(c); });

        if (ext == ".elf" || ext == ".axf" || ext == ".out") {
                if (loadElf(path))
                        ImGui::InsertNotification({ImGuiToastType::Success, toastDismissTime_, "Loaded ELF: %s", path.c_str()});
                else
                        ImGui::InsertNotification({ImGuiToastType::Error, toastDismissTime_, "Failed to load ELF"});
        } else if (ext == ".json") {
                if (loadCfg(path))
                        ImGui::InsertNotification(
                            {ImGuiToastType::Success, toastDismissTime_, "Loaded JSON: %s", path.c_str()});
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

static void
fillEnumPayload(const dwarf::Info                    &info,
                u64                                   typeOff,
                ChannelDropPayload                   &p,
                const std::vector<VarEntry::EnumDef> *overrideDefs = nullptr)
{
        // User-defined overrides win over DWARF — they reflect the latest edits
        // in the Variable window.
        if (overrideDefs && !overrideDefs->empty()) {
                p.numEnums = (u8)std::min((int)overrideDefs->size(), ChannelDropPayload::kMaxEnums);
                for (int i = 0; i < p.numEnums; ++i) {
                        snprintf(p.enums[i].name, sizeof(p.enums[i].name), "%s", (*overrideDefs)[i].name.c_str());
                        p.enums[i].value = (*overrideDefs)[i].value;
                }
                return;
        }
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
        if (!t)
                return 0;
        if (t->kind == dwarf::TypeKind::ARRAY) {
                if (t->size > 0)
                        return t->size;
                u64 elem  = typeSize(info, t->inner);
                u64 total = elem ? elem : 1;
                for (const u64 d : t->dims)
                        total *= (d ? d : 1);
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

bool
Variable::refreshChannelEnums(MonitorChannel *ch)
{
        if (!ch)
                return false;
        const std::string &chPath = ch->getSymbolName();
        if (chPath.empty())
                return false;

        // 1. Locate the leaf typeOff via searchPool_ — established truth for
        //    every flattened path under this Variable.
        u64  leafTypeOff = 0;
        bool foundInPool = false;
        for (const auto &se : searchPool_) {
                if (se.path == chPath) {
                        leafTypeOff = se.typeOff;
                        foundInPool = true;
                        break;
                }
        }
        if (!foundInPool)
                return false; // Not this Variable's symbol — let caller try the next.

        // 2. Find user override (exact match first, then parent member match).
        const std::vector<VarEntry::EnumDef> *override_ = nullptr;
        for (const auto &v : vars_) {
                if (v.name == chPath) {
                        if (!v.enumDefs.empty())
                                override_ = &v.enumDefs;
                        break;
                }
                if (chPath.size() > v.name.size() + 1 && chPath.compare(0, v.name.size(), v.name) == 0 &&
                    (chPath[v.name.size()] == '.' || chPath[v.name.size()] == '[')) {
                        auto it = v.memberEnumDefs.find(chPath);
                        if (it != v.memberEnumDefs.end() && !it->second.empty()) {
                                override_ = &it->second;
                                break;
                        }
                        // No override for this leaf — fall through to DWARF below.
                }
        }

        // 3. Build new enum entries: override wins; otherwise DWARF at the leaf
        //    type. If DWARF says not-an-enum and there's no override, clear —
        //    the type may have changed in the reloaded ELF.
        std::vector<MonitorChannel::EnumEntry> ents;
        if (override_) {
                ents.reserve(override_->size());
                for (const auto &e : *override_)
                        ents.push_back({e.name, e.value});
        } else {
                std::lock_guard    lk(mtxElf_);
                const dwarf::Type *t = resolveAlias(dwarfInfo_, leafTypeOff);
                if (t && t->kind == dwarf::TypeKind::ENUM) {
                        ents.reserve(t->enums.size());
                        for (const auto &e : t->enums)
                                ents.push_back({e.name, e.value});
                }
        }
        ch->setEnums(std::move(ents));
        return true;
}

void
Variable::flattenDwarfType(std::vector<SearchEntry> &pool,
                           const dwarf::Info        &info,
                           const std::string        &parentPath,
                           u64                       parentAddr,
                           u64                       typeOff,
                           int                       depth,
                           u32                       bitOffset,
                           u32                       bitSize)
{
        if (depth > 8)
                return;
        const dwarf::Type *t = resolveAlias(info, typeOff);
        if (!t)
                return;

        if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
                if (!parentPath.empty()) {
                        SearchEntry e;
                        e.path        = parentPath;
                        e.addr        = parentAddr;
                        e.type        = DataType::U32;
                        e.defaultPort = PortType::JLINK;
                        e.typeOff     = typeOff;
                        pool.push_back(e);
                }
                for (const auto &m : t->members) {
                        std::string path = parentPath + "." + (m.name.empty() ? "<anon>" : m.name);
                        flattenDwarfType(pool, info, path, parentAddr + m.offset, m.type, depth + 1, m.bitOffset, m.bitSize);
                }
        } else if (t->kind == dwarf::TypeKind::ARRAY) {
                if (!parentPath.empty()) {
                        SearchEntry e;
                        e.path        = parentPath;
                        e.addr        = parentAddr;
                        e.type        = DataType::U32;
                        e.defaultPort = PortType::JLINK;
                        e.typeOff     = typeOff;
                        pool.push_back(e);
                }
                u64 elemSize  = typeSize(info, t->inner);
                u64 dim       = t->dims.empty() ? 0 : t->dims.front();
                u64 displayed = (dim == 0) ? 0 : (dim < 8 ? dim : 8);
                for (u64 i = 0; i < displayed; ++i) {
                        std::string path = parentPath + "[" + std::to_string(i) + "]";
                        flattenDwarfType(pool, info, path, parentAddr + i * elemSize, t->inner, depth + 1, 0, 0);
                }
        } else {
                const char *sType = scalarPayloadType(info, typeOff);
                if (sType) {
                        SearchEntry e;
                        e.path        = parentPath;
                        e.addr        = parentAddr;
                        e.type        = Parser::strToDataType(sType);
                        e.defaultPort = PortType::JLINK;
                        e.typeOff     = typeOff;
                        e.bitOffset   = bitOffset;
                        e.bitSize     = bitSize;
                        pool.push_back(e);
                } else if (!parentPath.empty()) {
                        SearchEntry e;
                        e.path        = parentPath;
                        e.addr        = parentAddr;
                        e.type        = DataType::U32;
                        e.defaultPort = PortType::JLINK;
                        e.typeOff     = typeOff;
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
                e.path        = parentPath;
                e.addr        = 0;
                e.type        = node.type;
                e.defaultPort = PortType::MANUAL;
                e.typeOff     = 0;
                pool.push_back(e);
        }
}

// Defined later in this file; forward-declared so drawSymbolLeaf can flatten a
// dragged struct/array into per-member channels (same as the watch list does).
static void flattenForStructPayload(const dwarf::Info                                                     &info,
                                    const std::string                                                     &prefix,
                                    u64                                                                    baseAddr,
                                    u64                                                                    typeOff,
                                    StructChannelPayload                                                  &sp,
                                    const std::unordered_map<std::string, std::vector<VarEntry::EnumDef>> *memberOverrides,
                                    int                                                                    maxDepth);

void
Variable::drawSymbolTree()
{
        std::lock_guard lk(mtxElf_);
        if (dwarfInfo_.present && !dwarfInfo_.variables.empty()) {
                if (ImGui::BeginTable("SymbolTreeTable",
                                      4,
                                      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable,
                                      ImVec2(0, 0))) {
                        ImGui::TableSetupColumn(tr("Name###col_name", "名称###col_name"),
                                                ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn(
                            tr("Address###col_addr", "地址###col_addr"), ImGuiTableColumnFlags_WidthFixed, 120.0f);
                        ImGui::TableSetupColumn(
                            tr("Size###col_size", "大小###col_size"), ImGuiTableColumnFlags_WidthFixed, 60.0f);
                        ImGui::TableSetupColumn(tr("Type###col_type", "类型###col_type"), ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableHeadersRow();

                        std::vector<const dwarf::Variable *> vars;
                        vars.reserve(dwarfInfo_.variables.size());
                        for (const auto &v : dwarfInfo_.variables)
                                vars.push_back(&v);

                        if (ImGuiTableSortSpecs *sorts_specs = ImGui::TableGetSortSpecs()) {
                                if (sorts_specs->SpecsCount > 0) {
                                        const auto *spec = &sorts_specs->Specs[0];
                                        std::sort(vars.begin(),
                                                  vars.end(),
                                                  [&](const dwarf::Variable *a, const dwarf::Variable *b) -> bool {
                                                          int cmp = 0;
                                                          switch (spec->ColumnIndex) {
                                                                  case 0: // Name
                                                                          cmp = a->name.compare(b->name);
                                                                          break;
                                                                  case 1: // Address
                                                                          cmp = (a->addr < b->addr)
                                                                                    ? -1
                                                                                    : (a->addr > b->addr ? 1 : 0);
                                                                          break;
                                                                  case 2: // Size
                                                                          cmp = ((int)typeSize(dwarfInfo_, a->type) -
                                                                                 (int)typeSize(dwarfInfo_, b->type));
                                                                          break;
                                                                  case 3: // Type
                                                                          cmp = prettyType(dwarfInfo_, a->type)
                                                                                    .compare(prettyType(dwarfInfo_, b->type));
                                                                          break;
                                                          }
                                                          if (cmp == 0)
                                                                  cmp = a->name.compare(b->name);
                                                          return spec->SortDirection == ImGuiSortDirection_Ascending
                                                                     ? (cmp < 0)
                                                                     : (cmp > 0);
                                                  });
                                }
                        }

                        for (const auto *v : vars) {
                                drawSymbolLeaf(v->name, v->name, v->addr, v->type, 0);
                        }
                        ImGui::EndTable();
                }
        } else if (!dataTree_.children.empty()) {
                if (ImGui::BeginTable("BinTreeTable",
                                      3,
                                      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable,
                                      ImVec2(0, 0))) {
                        ImGui::TableSetupColumn(tr("Name###col_name", "名称###col_name"),
                                                ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn(tr("Type###col_type", "类型###col_type"), ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn(tr("Value###col_value", "数值###col_value"),
                                                ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableHeadersRow();

                        std::vector<DataTree *> nodes;
                        nodes.reserve(dataTree_.children.size());
                        for (auto &n : dataTree_.children)
                                nodes.push_back(&n);

                        if (ImGuiTableSortSpecs *sorts_specs = ImGui::TableGetSortSpecs()) {
                                if (sorts_specs->SpecsCount > 0) {
                                        const auto *spec = &sorts_specs->Specs[0];
                                        std::sort(
                                            nodes.begin(), nodes.end(), [&](const DataTree *a, const DataTree *b) -> bool {
                                                    int cmp = 0;
                                                    switch (spec->ColumnIndex) {
                                                            case 0:
                                                                    cmp = a->name.compare(b->name);
                                                                    break;
                                                            case 1:
                                                                    cmp = std::string(Parser::dataTypeToStr(a->type))
                                                                              .compare(Parser::dataTypeToStr(b->type));
                                                                    break;
                                                    }
                                                    if (cmp == 0)
                                                            cmp = a->name.compare(b->name);
                                                    return spec->SortDirection == ImGuiSortDirection_Ascending ? (cmp < 0)
                                                                                                               : (cmp > 0);
                                            });
                                }
                        }

                        for (auto *n : nodes)
                                drawDataTreeLeaf(*n);
                        ImGui::EndTable();
                }
        } else {
                ImGui::TextDisabled("%s", tr("Drop an ELF or BIN file to browse symbols.", "拖入 ELF 或 BIN 文件以浏览符号。"));
        }
}

void
Variable::drawSymbolLeaf(
    const std::string &displayName, const std::string &fullPath, u64 addr, u64 typeOff, i32 depth, u32 bitOffset, u32 bitSize)
{
        // Note: mtxElf_ is already held by drawSymbolTree()
        if (depth > 16)
                return;
        const dwarf::Type *t          = resolveAlias(dwarfInfo_, typeOff);
        const bool         isStruct   = t && (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION);
        const bool         isArray    = t && t->kind == dwarf::TypeKind::ARRAY;
        const bool         isEnum     = t && t->kind == dwarf::TypeKind::ENUM;
        const bool         expandable = (isStruct && !t->members.empty()) || (isArray && !t->dims.empty()) || isEnum;
        const char        *scalarKind = scalarPayloadType(dwarfInfo_, typeOff);

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
                if (!scalarKind && (isStruct || isArray)) {
                        // Flatten the struct/array into per-member scalar channels so the
                        // monitor plots each leaf, matching the watch-list behaviour.
                        // Dropping a raw struct as a single CHANNEL produced one
                        // un-plottable channel (the abnormal display the user hit).
                        StructChannelPayload sp{};
                        snprintf(sp.device, sizeof(sp.device), "JLINK");
                        snprintf(sp.rootName, sizeof(sp.rootName), "%s", fullPath.c_str());
                        sp.rootAddr    = addr;
                        sp.rootTypeOff = typeOff;
                        sp.srcWatch    = this; // mirror back into this window's watch list on a monitor drop
                        flattenForStructPayload(dwarfInfo_, fullPath, addr, typeOff, sp, nullptr, 8);
                        ImGui::SetDragDropPayload("STRUCT_CHANNEL", &sp, sizeof(sp));
                } else {
                        ChannelDropPayload p{};
                        snprintf(p.name, sizeof(p.name), "%s", fullPath.c_str());
                        p.addr = addr;
                        if (scalarKind)
                                snprintf(p.type, sizeof(p.type), "%s", scalarKind);
                        snprintf(p.device, sizeof(p.device), "JLINK");
                        p.numBytes  = (u8)typeSize(dwarfInfo_, typeOff);
                        p.typeOff   = typeOff;
                        p.bitOffset = bitOffset;
                        p.bitSize   = bitSize;
                        p.srcWatch  = this; // mirror back into this window's watch list on a monitor drop
                        fillEnumPayload(dwarfInfo_, typeOff, p);
                        ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                }
                ImGui::Text(tr("Dragging %s", "拖拽 %s"), fullPath.c_str());
                ImGui::EndDragDropSource();
        }
        // Double click to add
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                if (scalarKind) {
                        VarEntry v;
                        v.name      = fullPath;
                        v.type      = Parser::strToDataType(scalarKind);
                        v.port      = PortType::JLINK;
                        v.addr      = addr;
                        v.writable  = true;
                        v.typeOff   = typeOff;
                        v.bitOffset = bitOffset;
                        v.bitSize   = bitSize;
                        vars_.push_back(v);
                        ImGui::InsertNotification({ImGuiToastType::Success, 2000, "Added %s to watch list", fullPath.c_str()});
                } else if (isStruct || isArray) {
                        addRecursive(fullPath, addr, typeOff, PortType::JLINK);
                }
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::Text("0x%08llX", (unsigned long long)addr);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%llu", (unsigned long long)typeSize(dwarfInfo_, typeOff));
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(prettyType(dwarfInfo_, typeOff).c_str());

        if (open) {
                if (isStruct) {
                        for (const auto &m : t->members) {
                                drawSymbolLeaf(m.name.empty() ? "<anon>" : m.name,
                                               fullPath + "." + (m.name.empty() ? "<anon>" : m.name),
                                               addr + m.offset,
                                               m.type,
                                               depth + 1,
                                               m.bitOffset,
                                               m.bitSize);
                        }
                } else if (isArray) {
                        u64 elemSize  = typeSize(dwarfInfo_, t->inner);
                        u64 dim       = t->dims.empty() ? 0 : t->dims.front();
                        u64 displayed = (dim == 0) ? 0 : (dim < elfArrayMaxElems_ ? dim : elfArrayMaxElems_);
                        for (u64 i = 0; i < displayed; ++i) {
                                std::string idx = "[" + std::to_string(i) + "]";
                                drawSymbolLeaf(idx, fullPath + idx, addr + i * elemSize, t->inner, depth + 1);
                        }
                } else if (isEnum) {
                        for (const auto &e : t->enums) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TreeNodeEx(e.name.c_str(),
                                                  ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                      ImGuiTreeNodeFlags_SpanFullWidth);
                                ImGui::TableSetColumnIndex(1);
                                ImGui::Text("%lld", (long long)e.value);
                        }
                }
                ImGui::TreePop();
        }
        ImGui::PopID();
}

void
Variable::drawDataTreeLeaf(DataTree &node, const int indentLevel)
{
        // Unique per-node ID: template names repeat across sibling structs/arrays,
        // and without this the TreeNodeEx IDs collide — ImGui then can't tell the
        // rows apart, which silently breaks drag-and-drop / hover for the dupes.
        // (drawSymbolLeaf and the watch table push IDs for the same reason.)
        ImGui::PushID(static_cast<const void *>(&node));

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);

        bool isArray = (node.type == DataType::ARRAY);
        bool open    = false;
        if (isArray) {
                open = ImGui::TreeNodeEx(node.name.c_str(), ImGuiTreeNodeFlags_None);
        } else {
                ImGui::TreeNodeEx(node.name.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                        VarEntry v;
                        v.name     = node.name;
                        v.type     = node.type;
                        v.port     = PortType::MANUAL;
                        v.addr     = 0;
                        v.writable = false;
                        vars_.push_back(v);
                }

                if (ImGui::BeginDragDropSource()) {
                        ChannelDropPayload p{};
                        snprintf(p.name, sizeof(p.name), "%s", node.name.c_str());
                        p.addr = 0;
                        snprintf(p.type, sizeof(p.type), "%s", Parser::dataTypeToStr(node.type));
                        snprintf(p.device, sizeof(p.device), "LOCAL");
                        p.numBytes = (u8)Parser::typeBytes(node.type);
                        p.typeOff  = 0;
                        ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                        ImGui::Text(tr("Dragging %s", "拖拽 %s"), node.name.c_str());
                        ImGui::EndDragDropSource();
                }
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(Parser::dataTypeToStr(node.type));
        ImGui::TableSetColumnIndex(2);
        if (std::holds_alternative<f32>(node.val))
                ImGui::Text("%f", std::get<f32>(node.val));
        else if (std::holds_alternative<u32>(node.val))
                ImGui::Text("%u", std::get<u32>(node.val));
        else if (std::holds_alternative<i32>(node.val))
                ImGui::Text("%d", std::get<i32>(node.val));

        if (open && isArray) {
                for (auto &child : node.children)
                        drawDataTreeLeaf(child, indentLevel + 1);
                ImGui::TreePop();
        }

        ImGui::PopID();
}

static void
flattenForStructPayload(const dwarf::Info                                                     &info,
                        const std::string                                                     &prefix,
                        u64                                                                    baseAddr,
                        u64                                                                    typeOff,
                        StructChannelPayload                                                  &sp,
                        const std::unordered_map<std::string, std::vector<VarEntry::EnumDef>> *memberOverrides = nullptr,
                        int                                                                    maxDepth        = 8)
{
        if (sp.count >= StructChannelPayload::kMaxEntries || maxDepth <= 0)
                return;
        const dwarf::Type *t = resolveAlias(info, typeOff);
        if (!t)
                return;

        // Fill a leaf entry's enum array: user override (if any) wins over DWARF.
        auto fillEntryEnums = [&info,
                               memberOverrides](StructChannelPayload::Entry &e, u64 leafTypeOff, const std::string &leafPath) {
                if (memberOverrides) {
                        auto it = memberOverrides->find(leafPath);
                        if (it != memberOverrides->end() && !it->second.empty()) {
                                e.numEnums = (u8)std::min((int)it->second.size(), StructChannelPayload::kMaxEnums);
                                for (int i = 0; i < e.numEnums; ++i) {
                                        snprintf(e.enums[i].name, sizeof(e.enums[i].name), "%s", it->second[i].name.c_str());
                                        e.enums[i].value = it->second[i].value;
                                }
                                return;
                        }
                }
                const dwarf::Type *lt = resolveAlias(info, leafTypeOff);
                if (!lt || lt->kind != dwarf::TypeKind::ENUM) {
                        e.numEnums = 0;
                        return;
                }
                e.numEnums = (u8)std::min((int)lt->enums.size(), StructChannelPayload::kMaxEnums);
                for (int i = 0; i < e.numEnums; ++i) {
                        snprintf(e.enums[i].name, sizeof(e.enums[i].name), "%s", lt->enums[i].name.c_str());
                        e.enums[i].value = lt->enums[i].value;
                }
        };

        if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
                for (const auto &m : t->members) {
                        if (sp.count >= StructChannelPayload::kMaxEntries)
                                break;
                        std::string mPath = prefix + "." + (m.name.empty() ? "<anon>" : m.name);
                        u64         mAddr = baseAddr + m.offset;
                        const char *sType = scalarPayloadType(info, m.type);
                        if (sType) {
                                auto &e = sp.entries[sp.count++];
                                snprintf(e.name, sizeof(e.name), "%s", mPath.c_str());
                                e.addr = mAddr;
                                snprintf(e.type, sizeof(e.type), "%s", sType);
                                e.bitOffset = m.bitOffset;
                                e.bitSize   = m.bitSize;
                                e.writable  = sp.writable;
                                fillEntryEnums(e, m.type, mPath);
                        } else {
                                flattenForStructPayload(info, mPath, mAddr, m.type, sp, memberOverrides, maxDepth - 1);
                        }
                }
        } else if (t->kind == dwarf::TypeKind::ARRAY) {
                u64         elemSize = typeSize(info, t->inner);
                u64         dim      = t->dims.empty() ? 0 : t->dims.front();
                const char *sType    = scalarPayloadType(info, t->inner);
                for (u64 i = 0; i < dim && sp.count < StructChannelPayload::kMaxEntries; ++i) {
                        std::string idxPath = prefix + "[" + std::to_string(i) + "]";
                        u64         eAddr   = baseAddr + i * elemSize;
                        if (sType) {
                                auto &e = sp.entries[sp.count++];
                                snprintf(e.name, sizeof(e.name), "%s", idxPath.c_str());
                                e.addr = eAddr;
                                snprintf(e.type, sizeof(e.type), "%s", sType);
                                e.writable = sp.writable;
                                fillEntryEnums(e, t->inner, idxPath);
                        } else {
                                flattenForStructPayload(info, idxPath, eAddr, t->inner, sp, memberOverrides, maxDepth - 1);
                        }
                }
        }
}

void
Variable::drawSymbolBrowser()
{
        TutorialGuide::instance().mark("variable_browser");
        if (isElfLoading_) {
                ImGui::Text("%s", tr("Loading symbols...", "正在加载符号..."));
                ImGui::SameLine();
                static float ang  = 0.0f;
                ang              += 0.1f;
                ImGui::Text("%c", "|/-\\"[(int)(ang) % 4]);
                return;
        }

        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##symbol_search", "Search symbols (e.g. motor.pos)...", searchBuf_, sizeof(searchBuf_))) {
                searchResults_.clear();
                std::string query = searchBuf_;
                if (!query.empty()) {
                        std::ranges::transform(query, query.begin(), [](unsigned char c) { return std::tolower(c); });
                        for (const auto &e : searchPool_) {
                                std::string lowPath = e.path;
                                std::ranges::transform(
                                    lowPath, lowPath.begin(), [](unsigned char c) { return std::tolower(c); });
                                if (lowPath.find(query) != std::string::npos) {
                                        // Filter: if we already have a parent of this path in searchResults_, don't add this
                                        // one. This ensures that if a struct matches, its members are only visible inside it.
                                        bool hasParent = false;
                                        for (const auto &res : searchResults_) {
                                                if (e.path.size() > res.path.size() && e.path.starts_with(res.path) &&
                                                    (e.path[res.path.size()] == '.' || e.path[res.path.size()] == '[')) {
                                                        hasParent = true;
                                                        break;
                                                }
                                        }
                                        if (!hasParent) {
                                                searchResults_.push_back(e);
                                        }
                                        if (searchResults_.size() > 500)
                                                break;
                                }
                        }
                }
        }

        if (searchBuf_[0] == '\0') {
                drawSymbolTree();
        } else {
                constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable;
                if (ImGui::BeginTable("SymbolSearchTable", 4, flags, ImVec2(0, 0))) {
                        ImGui::TableSetupColumn(tr("Name###col_name", "名称###col_name"),
                                                ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn(
                            tr("Address###col_addr", "地址###col_addr"), ImGuiTableColumnFlags_WidthFixed, 120.0f);
                        ImGui::TableSetupColumn(
                            tr("Size###col_size", "大小###col_size"), ImGuiTableColumnFlags_WidthFixed, 60.0f);
                        ImGui::TableSetupColumn(tr("Type###col_type", "类型###col_type"), ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableHeadersRow();

                        if (ImGuiTableSortSpecs *sorts_specs = ImGui::TableGetSortSpecs()) {
                                if (sorts_specs->SpecsCount > 0 && sorts_specs->SpecsDirty) {
                                        const auto *spec = &sorts_specs->Specs[0];
                                        std::sort(searchResults_.begin(),
                                                  searchResults_.end(),
                                                  [&](const SearchEntry &a, const SearchEntry &b) -> bool {
                                                          int cmp = 0;
                                                          switch (spec->ColumnIndex) {
                                                                  case 0:
                                                                          cmp = a.path.compare(b.path);
                                                                          break;
                                                                  case 1:
                                                                          cmp = (a.addr < b.addr) ? -1
                                                                                                  : (a.addr > b.addr ? 1 : 0);
                                                                          break;
                                                                  case 2:
                                                                          cmp = ((int)typeSize(dwarfInfo_, a.typeOff) -
                                                                                 (int)typeSize(dwarfInfo_, b.typeOff));
                                                                          break;
                                                                  case 3:
                                                                          cmp = prettyType(dwarfInfo_, a.typeOff)
                                                                                    .compare(prettyType(dwarfInfo_, b.typeOff));
                                                                          break;
                                                          }
                                                          if (cmp == 0)
                                                                  cmp = a.path.compare(b.path);
                                                          return spec->SortDirection == ImGuiSortDirection_Ascending
                                                                     ? (cmp < 0)
                                                                     : (cmp > 0);
                                                  });
                                        sorts_specs->SpecsDirty = false;
                                }
                        }

                        for (int i = 0; i < (int)searchResults_.size(); ++i) {
                                const auto &e = searchResults_[i];
                                ImGui::PushID(i);
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);

                                const dwarf::Type *t = resolveAlias(dwarfInfo_, e.typeOff);
                                const bool         isComplex =
                                    t && (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION ||
                                          t->kind == dwarf::TypeKind::ARRAY);

                                ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow;
                                if (!isComplex)
                                        nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

                                bool open = ImGui::TreeNodeEx(e.path.c_str(), nodeFlags);

                                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                                        addRecursive(e.path, e.addr, e.typeOff, e.defaultPort, e.bitOffset, e.bitSize);
                                }

                                if (ImGui::BeginDragDropSource()) {
                                        if (isComplex) {
                                                // Flatten struct/array into per-member channels so the
                                                // monitor plots each leaf instead of one bad channel.
                                                StructChannelPayload sp{};
                                                snprintf(sp.device,
                                                         sizeof(sp.device),
                                                         e.defaultPort == PortType::JLINK ? "JLINK" : "SHM");
                                                snprintf(sp.rootName, sizeof(sp.rootName), "%s", e.path.c_str());
                                                sp.rootAddr    = e.addr;
                                                sp.rootTypeOff = e.typeOff;
                                                flattenForStructPayload(dwarfInfo_, e.path, e.addr, e.typeOff, sp, nullptr, 8);
                                                ImGui::SetDragDropPayload("STRUCT_CHANNEL", &sp, sizeof(sp));
                                        } else {
                                                ChannelDropPayload p{};
                                                snprintf(p.name, sizeof(p.name), "%s", e.path.c_str());
                                                p.addr = e.addr;
                                                snprintf(p.type, sizeof(p.type), "%s", Parser::dataTypeToStr(e.type));
                                                snprintf(p.device,
                                                         sizeof(p.device),
                                                         e.defaultPort == PortType::JLINK ? "JLINK" : "SHM");
                                                p.numBytes  = (u8)Parser::typeBytes(e.type);
                                                p.typeOff   = e.typeOff;
                                                p.bitOffset = e.bitOffset;
                                                p.bitSize   = e.bitSize;
                                                fillEnumPayload(dwarfInfo_, e.typeOff, p);
                                                ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                                        }
                                        ImGui::Text(tr("Dragging %s", "拖拽 %s"), e.path.c_str());
                                        ImGui::EndDragDropSource();
                                }

                                ImGui::TableSetColumnIndex(1);
                                ImGui::Text("0x%08llX", (unsigned long long)e.addr);
                                ImGui::TableSetColumnIndex(2);
                                ImGui::Text("%llu", (unsigned long long)typeSize(dwarfInfo_, e.typeOff));
                                ImGui::TableSetColumnIndex(3);
                                ImGui::TextUnformatted(prettyType(dwarfInfo_, e.typeOff).c_str());

                                if (open && isComplex) {
                                        if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
                                                for (const auto &m : t->members)
                                                        drawSymbolLeaf(m.name.empty() ? "<anon>" : m.name,
                                                                       e.path + "." + m.name,
                                                                       e.addr + m.offset,
                                                                       m.type,
                                                                       1,
                                                                       m.bitOffset,
                                                                       m.bitSize);
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
        if (ImGui::Button(tr("Add Variable", "添加变量")))
                state_ = WindowState::AddVariable;
        ImGui::SameLine();
        if (ui::Button(tr("Clear All", "全部清空"), ui::BtnStyle::Warning)) {
                vars_.clear();
                isModified_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Load File...", "加载文件...")))
                state_ = WindowState::LoadElf;
        ImGui::SameLine();
        if (ImGui::Button(tr("Export .var", "导出 .var"))) {
                std::string defaultName = getTitle() + ".var";
                std::string p           = nativeDlgSave("Export Variables", {{"Variable Files", {"var"}}}, defaultName);
                if (!p.empty())
                        exportVarFile(p);
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Save the whole watch list to a .var file", "将整个监视列表保存为 .var 文件"));
        ImGui::SameLine();
        if (ImGui::Button(tr("Import .var", "导入 .var"))) {
                std::string p = nativeDlgOpen("Import Variables", {{"Variable Files", {"var", "json"}}});
                if (!p.empty())
                        importVarFile(p);
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "%s", tr("Merge variables from a .var file (skips duplicates)", "从 .var 文件合并变量（跳过重名）"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        if (ImGui::SliderInt("##RefreshMs", (int *)&updateIntervalMs_, 10, 2000)) {
                isModified_ = true;
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Refresh(ms)", "刷新间隔(毫秒)"));

        constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable;
        if (ImGui::BeginTable("VarMonitorTable", 6, flags, ImVec2(0, 0))) {
                ImGui::TableSetupColumn(tr("Name###col_name", "名称###col_name"),
                                        ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(tr("Value###col_value", "数值###col_value"),
                                        ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(tr("Type###col_type", "类型###col_type"), ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn(tr("Address###col_addr", "地址###col_addr"), ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn(tr("Port###col_port", "端口###col_port"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn(tr("R/W###col_rw", "读写###col_rw"), ImGuiTableColumnFlags_WidthFixed, 50.0f);
                ImGui::TableHeadersRow();

                if (ImGuiTableSortSpecs *sorts_specs = ImGui::TableGetSortSpecs()) {
                        if (sorts_specs->SpecsCount > 0 && sorts_specs->SpecsDirty) {
                                const auto *spec = &sorts_specs->Specs[0];
                                std::sort(vars_.begin(), vars_.end(), [&](const VarEntry &a, const VarEntry &b) -> bool {
                                        int cmp = 0;
                                        switch (spec->ColumnIndex) {
                                                case 0:
                                                        cmp = a.name.compare(b.name);
                                                        break;
                                                case 2:
                                                        cmp = prettyType(dwarfInfo_, a.typeOff)
                                                                  .compare(prettyType(dwarfInfo_, b.typeOff));
                                                        break;
                                                case 3:
                                                        cmp = (a.addr < b.addr) ? -1 : (a.addr > b.addr ? 1 : 0);
                                                        break;
                                                case 4:
                                                        cmp = (int)a.port - (int)b.port;
                                                        break;
                                                case 5:
                                                        cmp = (int)a.writable - (int)b.writable;
                                                        break;
                                        }
                                        if (cmp == 0)
                                                cmp = a.name.compare(b.name);
                                        return spec->SortDirection == ImGuiSortDirection_Ascending ? (cmp < 0) : (cmp > 0);
                                });
                                sorts_specs->SpecsDirty = false;
                        }
                }

                // Deferred reorder (drag grip): applied after the loop so we never
                // mutate vars_ mid-iteration.
                int varMoveSrc = -1, varMoveDst = -1;

                // Drop the grip drag state once the drag ends.
                if (!ImGui::IsDragDropActive()) {
                        rowDragSrc_      = -1;
                        fieldDragParent_ = -1;
                        fieldDragSrc_    = -1;
                }

                // Build the monitor drag payload (CHANNEL / STRUCT_CHANNEL) for a row. This is
                // carried by the "=" grip so the same handle both reorders the row (in-window)
                // and drops onto a monitor to add channels.
                auto setMonitorPayload = [&](const VarEntry &v, bool isComplex, bool hasManualStruct) {
                        if (isComplex) {
                                StructChannelPayload sp{};
                                sp.writable = v.writable;
                                snprintf(sp.device, sizeof(sp.device), "%s", portName(v.port));
                                if (v.port == PortType::SHM)
                                        snprintf(sp.shmName, sizeof(sp.shmName), "%s", v.shm.name);
                                if (v.port == PortType::AUDIO)
                                        snprintf(sp.shmName, sizeof(sp.shmName), "%s", v.audio.deviceName);
                                snprintf(sp.rootName, sizeof(sp.rootName), "%s", v.name.c_str());
                                sp.rootAddr    = v.addr;
                                sp.rootTypeOff = v.typeOff;
                                if (hasManualStruct) {
                                        for (const auto &sf : v.structFields) {
                                                if (sp.count >= StructChannelPayload::kMaxEntries)
                                                        break;
                                                auto &e = sp.entries[sp.count++];
                                                snprintf(e.name, sizeof(e.name), "%s.%s", v.name.c_str(), sf.name);
                                                e.addr = v.addr + sf.byteOffset;
                                                snprintf(e.type, sizeof(e.type), "%s", Parser::dataTypeToStr(sf.type));
                                                e.writable = v.writable;
                                                e.numEnums = 0;
                                                auto eit   = v.memberEnumDefs.find(e.name);
                                                if (eit != v.memberEnumDefs.end() && !eit->second.empty()) {
                                                        e.numEnums = (u8)std::min((int)eit->second.size(),
                                                                                  StructChannelPayload::kMaxEnums);
                                                        for (int k = 0; k < e.numEnums; ++k) {
                                                                snprintf(e.enums[k].name,
                                                                         sizeof(e.enums[k].name),
                                                                         "%s",
                                                                         eit->second[k].name.c_str());
                                                                e.enums[k].value = eit->second[k].value;
                                                        }
                                                }
                                        }
                                } else {
                                        std::lock_guard lk(mtxElf_);
                                        flattenForStructPayload(dwarfInfo_, v.name, v.addr, v.typeOff, sp, &v.memberEnumDefs);
                                }
                                ImGui::SetDragDropPayload("STRUCT_CHANNEL", &sp, sizeof(sp));
                        } else {
                                ChannelDropPayload p{};
                                p.writable = v.writable;
                                snprintf(p.name, sizeof(p.name), "%s", v.name.c_str());
                                p.addr = v.addr;
                                snprintf(p.type, sizeof(p.type), "%s", Parser::dataTypeToStr(v.type));
                                snprintf(p.device, sizeof(p.device), "%s", portName(v.port));
                                if (v.port == PortType::SHM)
                                        snprintf(p.shmName, sizeof(p.shmName), "%s", v.shm.name);
                                if (v.port == PortType::AUDIO)
                                        snprintf(p.shmName, sizeof(p.shmName), "%s", v.audio.deviceName);
                                p.numBytes  = (u8)Parser::typeBytes(v.type);
                                p.typeOff   = v.typeOff;
                                p.bitOffset = v.bitOffset;
                                p.bitSize   = v.bitSize;
                                {
                                        std::lock_guard lk(mtxElf_);
                                        fillEnumPayload(dwarfInfo_, v.typeOff, p, &v.enumDefs);
                                }
                                ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                        }
                };

                // A row reorder target: reorders when the active drag is an internal "=" grip
                // drag (rowDragSrc_ >= 0). Returns nothing; updates varMoveSrc/Dst.
                auto acceptRowReorder = [&](int dstRow) {
                        if (rowDragSrc_ < 0)
                                return;
                        const ImGuiPayload *pl = ImGui::GetDragDropPayload();
                        if (!pl || !(pl->IsDataType("CHANNEL") || pl->IsDataType("STRUCT_CHANNEL")))
                                return;
                        if (ImGui::AcceptDragDropPayload(pl->DataType)) {
                                varMoveSrc = rowDragSrc_;
                                varMoveDst = dstRow;
                        }
                };

                for (int i = 0; i < (int)vars_.size(); ++i) {
                        auto      &v          = vars_[i];
                        const bool isSelected = v.selected;
                        ImGui::PushID(i);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);

                        // Detect a struct/array type up front — the "=" grip needs it to build
                        // the monitor drag payload.
                        const dwarf::Type *t = nullptr;
                        {
                                std::lock_guard lk(mtxElf_);
                                t = resolveAlias(dwarfInfo_, v.typeOff);
                        }
                        const bool hasManualStruct = !v.structFields.empty();
                        const bool isComplex =
                            hasManualStruct || (t && (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION ||
                                                      t->kind == dwarf::TypeKind::ARRAY));

                        // "=" grip — selection + the single drag handle for this row. Dragging it
                        // reorders the row (drop on another row's grip) or adds the variable to a
                        // monitor (drop on a monitor scope).
                        if (ui::SmallButton("=##rowgrip", v.selected ? ui::BtnStyle::Primary : ui::BtnStyle::Neutral)) {
                                if (ImGui::GetIO().KeyCtrl) {
                                        v.selected = !v.selected;
                                } else if (ImGui::GetIO().KeyShift && lastSelectedIndex_ != -1) {
                                        int start = std::min(lastSelectedIndex_, i);
                                        int end   = std::max(lastSelectedIndex_, i);
                                        for (int j = start; j <= end; ++j)
                                                vars_[j].selected = true;
                                } else {
                                        for (auto &var : vars_)
                                                var.selected = false;
                                        v.selected = true;
                                }
                                lastSelectedIndex_ = i;
                        }
                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                                rowDragSrc_ = i;
                                setMonitorPayload(v, isComplex, hasManualStruct);
                                ImGui::Text(tr("Dragging %s", "拖拽 %s"), v.name.c_str());
                                ImGui::EndDragDropSource();
                        }
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", tr("Drag: reorder / add to monitor", "拖动：调整顺序 / 添加到监视器"));
                        if (ImGui::BeginDragDropTarget()) {
                                acceptRowReorder(i);
                                ImGui::EndDragDropTarget();
                        }
                        ImGui::OpenPopupOnItemClick("##var_row_ctx", ImGuiPopupFlags_MouseButtonRight);
                        ImGui::SameLine();

                        bool pendingDelete   = false;
                        bool pendingEnumEdit = false;
                        if (ImGui::BeginPopup("##var_row_ctx")) {
                                // Auto-select on right-click when nothing is selected.
                                if (!v.selected) {
                                        for (auto &var : vars_)
                                                var.selected = false;
                                        v.selected = true;
                                }
                                if (ImGui::MenuItem(tr("Delete Selected", "删除选中项"))) {
                                        pendingDelete = true;
                                }
                                if (ImGui::MenuItem(tr("Edit Properties...", "编辑属性..."))) {
                                        editPropIdx_ = i;
                                        snprintf(editPropBuf_.name, sizeof(editPropBuf_.name), "%s", v.name.c_str());
                                        snprintf(editPropBuf_.addrBuf,
                                                 sizeof(editPropBuf_.addrBuf),
                                                 "%llX",
                                                 (unsigned long long)v.addr);
                                        editPropBuf_.type         = v.type;
                                        editPropBuf_.port         = v.port;
                                        editPropBuf_.writable     = v.writable;
                                        editPropBuf_.structMode   = !v.structFields.empty();
                                        editPropBuf_.structFields = v.structFields;
                                        if (v.port == PortType::SHM) {
                                                snprintf(editPropBuf_.shmName, sizeof(editPropBuf_.shmName), "%s", v.shm.name);
                                        }
                                        if (v.port == PortType::AUDIO) {
                                                editPropBuf_.audioDeviceIndex = v.audio.deviceIndex;
                                                snprintf(editPropBuf_.audioDeviceName,
                                                         sizeof(editPropBuf_.audioDeviceName),
                                                         "%s",
                                                         v.audio.deviceName);
                                        }
                                }
                                const bool isEnumType = (t && t->kind == dwarf::TypeKind::ENUM) || !v.enumDefs.empty();
                                if (!isComplex && isEnumType &&
                                    ImGui::MenuItem(tr("Edit Enum Definition...", "编辑枚举定义..."))) {
                                        pendingEnumEdit = true;
                                }
                                if (isComplex && !v.hiddenMembers.empty()) {
                                        char restoreLabel[64];
                                        snprintf(restoreLabel,
                                                 sizeof(restoreLabel),
                                                 "Restore hidden (%d)",
                                                 (int)v.hiddenMembers.size());
                                        if (ImGui::MenuItem(restoreLabel)) {
                                                v.hiddenMembers.clear();
                                                isModified_ = true;
                                        }
                                }
                                ImGui::EndPopup();
                        }

                        // Popup is handled before TreeNodeEx so the ID stack matches
                        // OpenPopupOnItemClick (expanded structs push to the ID stack).
                        if (pendingDelete) {
                                ImGui::PopID();
                                for (int j = (int)vars_.size() - 1; j >= 0; --j)
                                        if (vars_[j].selected)
                                                vars_.erase(vars_.begin() + j);
                                isModified_        = true;
                                lastSelectedIndex_ = -1;
                                break;
                        }
                        if (pendingEnumEdit)
                                enumEditIdx_ = i;

                        ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow;
                        if (!isComplex)
                                nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

                        const bool wasOpenTop = isComplex && v.expandedMembers.count(v.name) > 0;
                        if (isComplex)
                                ImGui::SetNextItemOpen(wasOpenTop);
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
                        bool open = ImGui::TreeNodeEx(v.name.c_str(), nodeFlags & ~ImGuiTreeNodeFlags_SpanFullWidth);
                        ImGui::PopStyleColor(2);
                        if (isComplex && open != wasOpenTop) {
                                if (open)
                                        v.expandedMembers.insert(v.name);
                                else
                                        v.expandedMembers.erase(v.name);
                                isModified_ = true;
                        }

                        // Selection logic
                        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                                if (ImGui::GetIO().KeyCtrl) {
                                        v.selected = !v.selected;
                                } else if (ImGui::GetIO().KeyShift && lastSelectedIndex_ != -1) {
                                        int start = std::min(lastSelectedIndex_, i);
                                        int end   = std::max(lastSelectedIndex_, i);
                                        for (int j = start; j <= end; ++j)
                                                vars_[j].selected = true;
                                } else {
                                        for (auto &var : vars_)
                                                var.selected = false;
                                        v.selected = true;
                                }
                                lastSelectedIndex_ = i;
                        }

                        // (The monitor drag source now lives on the "=" grip — see above.)

                        // Value
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(v.valueStr.c_str());

                        // Type
                        ImGui::TableSetColumnIndex(2);
                        if (hasManualStruct) {
                                ImGui::TextUnformatted("STRUCT");
                        } else if (isComplex) {
                                ImGui::TextUnformatted(t->kind == dwarf::TypeKind::ARRAY ? "ARRAY" : "STRUCT");
                        } else if ((t && t->kind == dwarf::TypeKind::ENUM) || !v.enumDefs.empty()) {
                                ImGui::TextUnformatted("ENUM");
                        } else {
                                ImGui::TextUnformatted(Parser::dataTypeToStr(v.type));
                        }

                        // Address
                        ImGui::TableSetColumnIndex(3);
                        if (v.port == PortType::LOCAL) {
                                ImGui::TextDisabled("(local)");
                        } else if (v.port == PortType::SHM) {
                                ImGui::TextUnformatted(v.shm.name);
                        } else if (v.port == PortType::AUDIO) {
                                ImGui::TextUnformatted(v.audio.deviceName[0]
                                                           ? v.audio.deviceName
                                                           : AudioInput::instance().deviceName(v.audio.deviceIndex).c_str());
                        } else if (v.addrUnknown) {
                                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "UNKNOWN");
                        } else {
                                ImGui::Text("0x%08llX", (unsigned long long)v.addr);
                        }

                        // Port
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextUnformatted(portName(v.port));

                        // R/W
                        ImGui::TableSetColumnIndex(5);
                        if (v.writable)
                                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "RW");
                        else
                                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "RO");

                        if (open && isComplex) {
                                if (hasManualStruct) {
                                        // Render manually defined struct fields from LOCAL buffer
                                        int                          fieldMoveSrc = -1, fieldMoveDst = -1;
                                        std::unique_lock<std::mutex> lk(mtxLocal_);
                                        auto                         it = localBufs_.find(v.name);
                                        for (int fi = 0; fi < (int)v.structFields.size(); ++fi) {
                                                const auto &sf = v.structFields[fi];
                                                ImGui::PushID(fi);
                                                ImGui::TableNextRow();
                                                ImGui::TableSetColumnIndex(0);
                                                // "=" grip: reorder this field (drop on a sibling) or drag it to a
                                                // monitor (drop on a scope).
                                                ui::SmallButton("=##fgrip", ui::BtnStyle::Neutral);
                                                if (ImGui::BeginDragDropSource()) {
                                                        fieldDragParent_ = i;
                                                        fieldDragSrc_    = fi;
                                                        ChannelDropPayload p{};
                                                        p.writable = v.writable;
                                                        snprintf(p.name, sizeof(p.name), "%s.%s", v.name.c_str(), sf.name);
                                                        p.addr = v.addr + sf.byteOffset;
                                                        snprintf(p.type, sizeof(p.type), "%s", Parser::dataTypeToStr(sf.type));
                                                        snprintf(p.device, sizeof(p.device), "LOCAL");
                                                        p.numBytes = (u8)Parser::typeBytes(sf.type);
                                                        auto eit   = v.memberEnumDefs.find(p.name);
                                                        if (eit != v.memberEnumDefs.end() && !eit->second.empty()) {
                                                                p.numEnums = (u8)std::min((int)eit->second.size(),
                                                                                          ChannelDropPayload::kMaxEnums);
                                                                for (int k = 0; k < p.numEnums; ++k) {
                                                                        snprintf(p.enums[k].name,
                                                                                 sizeof(p.enums[k].name),
                                                                                 "%s",
                                                                                 eit->second[k].name.c_str());
                                                                        p.enums[k].value = eit->second[k].value;
                                                                }
                                                        }
                                                        ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                                                        ImGui::Text(tr("Dragging %s", "拖拽 %s"), p.name);
                                                        ImGui::EndDragDropSource();
                                                }
                                                if (ImGui::IsItemHovered())
                                                        ImGui::SetTooltip("%s",
                                                                          tr("Drag: reorder / add to monitor",
                                                                             "拖动：调整顺序 / 添加到监视器"));
                                                if (ImGui::BeginDragDropTarget()) {
                                                        if (fieldDragParent_ == i && fieldDragSrc_ >= 0) {
                                                                const ImGuiPayload *pl = ImGui::GetDragDropPayload();
                                                                if (pl && pl->IsDataType("CHANNEL") &&
                                                                    ImGui::AcceptDragDropPayload("CHANNEL")) {
                                                                        fieldMoveSrc = fieldDragSrc_;
                                                                        fieldMoveDst = fi;
                                                                }
                                                        }
                                                        ImGui::EndDragDropTarget();
                                                }
                                                ImGui::SameLine();
                                                // Suppress the hover/active highlight on struct sub-items.
                                                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
                                                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
                                                ImGui::TreeNodeEx(sf.name,
                                                                  ImGuiTreeNodeFlags_Leaf |
                                                                      ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                                      ImGuiTreeNodeFlags_SpanFullWidth);
                                                ImGui::PopStyleColor(2);
                                                ImGui::TableSetColumnIndex(1);
                                                if (it != localBufs_.end()) {
                                                        const auto &fbuf = it->second;
                                                        u32         fsz  = Parser::typeBytes(sf.type);
                                                        if (sf.byteOffset + fsz <= (u32)fbuf.size()) {
                                                                uint8_t tmp[8]{};
                                                                std::memcpy(tmp, fbuf.data() + sf.byteOffset, fsz);
                                                                ImGui::TextUnformatted(decodeValue(tmp, sf.type, 0, 0).c_str());
                                                        } else {
                                                                ImGui::TextDisabled("---");
                                                        }
                                                } else {
                                                        ImGui::TextDisabled("---");
                                                }
                                                ImGui::TableSetColumnIndex(2);
                                                ImGui::TextUnformatted(Parser::dataTypeToStr(sf.type));
                                                ImGui::TableSetColumnIndex(3);
                                                ImGui::Text("+0x%X", sf.byteOffset);
                                                ImGui::PopID();
                                        }
                                        lk.unlock();
                                        // Apply a deferred field reorder (struct sub-item grip). Reorders the
                                        // display order; byte offsets are unchanged.
                                        if (fieldMoveSrc >= 0 && fieldMoveDst >= 0 && fieldMoveSrc != fieldMoveDst &&
                                            fieldMoveSrc < (int)v.structFields.size() &&
                                            fieldMoveDst < (int)v.structFields.size()) {
                                                auto moved = v.structFields[fieldMoveSrc];
                                                v.structFields.erase(v.structFields.begin() + fieldMoveSrc);
                                                int dst = fieldMoveDst > fieldMoveSrc ? fieldMoveDst - 1 : fieldMoveDst;
                                                v.structFields.insert(v.structFields.begin() + dst, moved);
                                                isModified_ = true;
                                        }
                                } else {
                                        std::lock_guard lk(mtxElf_);
                                        drawVarVarTreeRow(v.name, v.addr, v.typeOff, 1, v.port, v.shm.name, i);
                                }
                                ImGui::TreePop();
                        }

                        ImGui::PopID();
                }

                // Apply a deferred row reorder (drag grip). Move the dragged entry so
                // it lands at the drop row's position.
                if (varMoveSrc >= 0 && varMoveDst >= 0 && varMoveSrc != varMoveDst && varMoveSrc < (int)vars_.size() &&
                    varMoveDst < (int)vars_.size()) {
                        VarEntry moved = std::move(vars_[varMoveSrc]);
                        vars_.erase(vars_.begin() + varMoveSrc);
                        int dst = varMoveDst > varMoveSrc ? varMoveDst - 1 : varMoveDst;
                        vars_.insert(vars_.begin() + dst, std::move(moved));
                        lastSelectedIndex_ = dst;
                        isModified_        = true;
                }

                ImGui::EndTable();
        }

        // Only accept "add to watch list" drops from outside (symbol browser). An internal
        // "=" grip drag (row or struct field) is a reorder/monitor drag — never re-add it here.
        if (rowDragSrc_ < 0 && fieldDragParent_ < 0 && ImGui::BeginDragDropTarget()) {
                // A struct/array dragged from the symbol browser arrives as a
                // STRUCT_CHANNEL (flattened for the monitor); here we re-add it as a
                // single expandable watch-list entry using the carried root metadata.
                if (const ImGuiPayload *sPayload = ImGui::AcceptDragDropPayload("STRUCT_CHANNEL")) {
                        if (sPayload->DataSize == sizeof(StructChannelPayload))
                                addStructToWatch(*static_cast<const StructChannelPayload *>(sPayload->Data));
                }

                const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("SYMBOL_CHANNEL");
                if (!payload)
                        payload = ImGui::AcceptDragDropPayload("CHANNEL");
                if (payload)
                        addScalarToWatch(*static_cast<const ChannelDropPayload *>(payload->Data));

                ImGui::EndDragDropTarget();
        }
}

bool
Variable::watchHasName(const std::string &name) const
{
        for (const auto &v : vars_)
                if (v.name == name)
                        return true;
        return false;
}

void
Variable::addScalarToWatch(const ChannelDropPayload &p)
{
        if (watchHasName(p.name)) // avoid duplicate watch entries
                return;
        VarEntry v;
        v.name      = p.name;
        v.addr      = p.addr;
        v.writable  = true;
        v.typeOff   = p.typeOff;
        v.bitOffset = p.bitOffset;
        v.bitSize   = p.bitSize;

        if (strcmp(p.device, "SHM") == 0) {
                v.port = PortType::SHM;
                snprintf(v.shm.name, sizeof(v.shm.name), "%s", p.name);
        } else if (strcmp(p.device, "AUDIO") == 0) {
                v.port              = PortType::AUDIO;
                v.writable          = false;
                v.audio.deviceIndex = static_cast<int>(p.addr);
                snprintf(v.audio.deviceName, sizeof(v.audio.deviceName), "%s", p.shmName);
        } else if (strcmp(p.device, "LOCAL") == 0 || strcmp(p.device, "MANUAL") == 0) {
                v.port     = PortType::MANUAL;
                v.writable = false; // Manual/Bin data is usually readonly in this context
        } else {
                v.port = PortType::JLINK;
        }

        if (strcmp(p.type, "STRUCT") == 0 || strcmp(p.type, "ARRAY") == 0)
                v.type = DataType::U32; // Placeholder; tree uses typeOff
        else
                v.type = Parser::strToDataType(p.type);

        vars_.push_back(v);
        isModified_ = true;
}

void
Variable::addStructToWatch(const StructChannelPayload &s)
{
        if (s.rootName[0] == '\0' || watchHasName(s.rootName))
                return;
        VarEntry v;
        v.name     = s.rootName;
        v.addr     = s.rootAddr;
        v.typeOff  = s.rootTypeOff;
        v.writable = true;
        v.type     = DataType::U32; // placeholder; tree uses typeOff
        if (strcmp(s.device, "SHM") == 0) {
                v.port = PortType::SHM;
                snprintf(v.shm.name, sizeof(v.shm.name), "%s", s.shmName[0] != '\0' ? s.shmName : s.rootName);
        } else if (strcmp(s.device, "AUDIO") == 0) {
                v.port              = PortType::AUDIO;
                v.writable          = false;
                v.audio.deviceIndex = static_cast<int>(s.rootAddr);
                snprintf(v.audio.deviceName, sizeof(v.audio.deviceName), "%s", s.shmName);
        } else if (strcmp(s.device, "LOCAL") == 0 || strcmp(s.device, "MANUAL") == 0) {
                v.port     = PortType::MANUAL;
                v.writable = false;
        } else {
                v.port = PortType::JLINK;
        }
        vars_.push_back(v);
        isModified_ = true;
}

void
Variable::mirrorFromMonitorDrop(const WatchMirrorRequest &req)
{
        if (req.isStruct)
                addStructToWatch(req.group);
        else
                addScalarToWatch(req.scalar);
}

void
Variable::drawAddVariableDialog()
{
        if (state_ != WindowState::AddVariable)
                return;
        ImGui::OpenPopup("###AddNewVariable");
        if (ImGui::BeginPopupModal(tr("Add New Variable###AddNewVariable", "添加新变量###AddNewVariable"),
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::InputText("##newVarName", newVar_.name, sizeof(newVar_.name));
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Name", "名称"));
                static const char    *ports[]   = {"JLINK", "SHM", "LOCAL", "AUDIO"};
                static const PortType portMap[] = {PortType::JLINK, PortType::SHM, PortType::LOCAL, PortType::AUDIO};
                static int            portIdx   = 0;
                if (ImGui::Combo("##newVarPort", &portIdx, ports, IM_ARRAYSIZE(ports))) {
                        newVar_.port = portMap[portIdx];
                        if (newVar_.port != PortType::LOCAL)
                                newVar_.structMode = false;
                        if (newVar_.port == PortType::AUDIO) {
                                newVar_.type             = DataType::F32;
                                newVar_.writable         = false;
                                newVar_.addr             = (u64)AudioInput::instance().defaultDeviceIndex();
                                newVar_.audioDeviceIndex = static_cast<int>(newVar_.addr);
                                std::string devName      = AudioInput::instance().deviceName(newVar_.audioDeviceIndex);
                                snprintf(newVar_.audioDeviceName, sizeof(newVar_.audioDeviceName), "%s", devName.c_str());
                        }
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Port", "端口"));

                if (newVar_.port == PortType::AUDIO) {
                        newVar_.type = DataType::F32;
                        ImGui::TextDisabled("%s", tr("F32 normalized PCM sample (-1..1)", "F32 归一化 PCM 采样值 (-1..1)"));
                } else if (newVar_.port == PortType::LOCAL) {
                        // LOCAL port: type dropdown includes "Struct" option
                        static const char *localTypes[] = {
                            "U8", "U16", "U32", "U64", "I8", "I16", "I32", "I64", "F32", "F64", "Struct"};
                        static const DataType localTypeVals[] = {DataType::U8,
                                                                 DataType::U16,
                                                                 DataType::U32,
                                                                 DataType::U64,
                                                                 DataType::I8,
                                                                 DataType::I16,
                                                                 DataType::I32,
                                                                 DataType::I64,
                                                                 DataType::F32,
                                                                 DataType::F64};
                        static int            localTypeIdx    = 2;
                        // Keep visual selection consistent with structMode
                        if (newVar_.structMode && localTypeIdx < 10)
                                localTypeIdx = 10;
                        else if (!newVar_.structMode && localTypeIdx == 10)
                                localTypeIdx = 2;
                        if (ImGui::Combo("##newVarType", &localTypeIdx, localTypes, IM_ARRAYSIZE(localTypes))) {
                                if (localTypeIdx < 10) {
                                        newVar_.type       = localTypeVals[localTypeIdx];
                                        newVar_.structMode = false;
                                        // Auto-size buffer to match type
                                        size_t tsz = Parser::typeBytes(newVar_.type);
                                        if (tsz > 0)
                                                newVar_.addr = (u64)tsz;
                                } else {
                                        newVar_.structMode = true;
                                }
                        }
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", tr("Type", "类型"));
                } else {
                        static const char *types[] = {"U8", "U16", "U32", "U64", "I8", "I16", "I32", "I64", "F32", "F64"};
                        static int         typeIdx = 2;
                        if (ImGui::Combo("##newVarType", &typeIdx, types, IM_ARRAYSIZE(types)))
                                newVar_.type = Parser::strToDataType(types[typeIdx]);
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", tr("Type", "类型"));
                }

                if (newVar_.port == PortType::JLINK) {
                        ImGui::InputText("##newVarAddress", newVar_.addrBuf, sizeof(newVar_.addrBuf));
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", tr("Address (Hex)", "地址（十六进制）"));
                        try {
                                newVar_.addr = std::stoull(newVar_.addrBuf, nullptr, 16);
                        } catch (...) {
                                newVar_.addr = 0;
                        }
                } else if (newVar_.port == PortType::SHM) {
                        ImGui::InputText("##newVarShmName", newVar_.shmName, sizeof(newVar_.shmName));
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", tr("SHM Name", "共享内存名称"));
                        ImGui::InputText("##newVarOffset", newVar_.addrBuf, sizeof(newVar_.addrBuf));
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", tr("Offset", "偏移"));
                        try {
                                newVar_.addr = std::stoull(newVar_.addrBuf, nullptr, 16);
                        } catch (...) {
                                newVar_.addr = 0;
                        }
                } else if (newVar_.port == PortType::AUDIO) {
                        drawAudioDeviceCombo(
                            newVar_.audioDeviceIndex, newVar_.audioDeviceName, sizeof(newVar_.audioDeviceName));
                        newVar_.addr = (u64)std::max(0, newVar_.audioDeviceIndex);
                } else if (newVar_.port == PortType::LOCAL) {
                        if (!newVar_.structMode) {
                                int localSz = (newVar_.addr > 0) ? (int)newVar_.addr : 8;
                                ImGui::SetNextItemWidth(120.0f);
                                if (ImGui::InputInt(tr("Size (bytes)##lsz", "大小(字节)##lsz"), &localSz)) {
                                        if (localSz < 1)
                                                localSz = 1;
                                        if (localSz > 4096)
                                                localSz = 4096;
                                        newVar_.addr = (u64)localSz;
                                }
                                ImGui::TextDisabled("%s",
                                                    tr("Use &varname in SDK sequence to pass as pointer arg.",
                                                       "在 SDK 序列参数中输入 &varname 可将其作为指针参数传入。"));
                        } else {
                                // ── Struct field editor ──
                                static const char *sfTypes[] = {
                                    "U8", "U16", "U32", "U64", "I8", "I16", "I32", "I64", "F32", "F64"};
                                static const DataType sfTypeVals[] = {DataType::U8,
                                                                      DataType::U16,
                                                                      DataType::U32,
                                                                      DataType::U64,
                                                                      DataType::I8,
                                                                      DataType::I16,
                                                                      DataType::I32,
                                                                      DataType::I64,
                                                                      DataType::F32,
                                                                      DataType::F64};
                                constexpr int         kSfTypeCount = 10;

                                constexpr ImGuiTableFlags tfl =
                                    ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
                                if (ImGui::BeginTable("##sftbl", 4, tfl, ImVec2(0, 120))) {
                                        ImGui::TableSetupColumn(tr("Name", "名称"), ImGuiTableColumnFlags_WidthStretch);
                                        ImGui::TableSetupColumn(tr("Type", "类型"), ImGuiTableColumnFlags_WidthFixed, 60.0f);
                                        ImGui::TableSetupColumn(tr("Offset", "偏移"), ImGuiTableColumnFlags_WidthFixed, 48.0f);
                                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 22.0f);
                                        ImGui::TableHeadersRow();

                                        int toDelete = -1;
                                        for (int fi = 0; fi < (int)newVar_.structFields.size(); ++fi) {
                                                auto &sf = newVar_.structFields[fi];
                                                ImGui::PushID(fi);
                                                ImGui::TableNextRow();

                                                ImGui::TableSetColumnIndex(0);
                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                ImGui::InputText("##fn", sf.name, sizeof(sf.name));

                                                ImGui::TableSetColumnIndex(1);
                                                int sfIdx = 2;
                                                for (int k = 0; k < kSfTypeCount; ++k)
                                                        if (sfTypeVals[k] == sf.type) {
                                                                sfIdx = k;
                                                                break;
                                                        }
                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                if (ImGui::Combo("##ft", &sfIdx, sfTypes, kSfTypeCount))
                                                        sf.type = sfTypeVals[sfIdx];

                                                ImGui::TableSetColumnIndex(2);
                                                int off = (int)sf.byteOffset;
                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                if (ImGui::InputInt("##fo", &off, 0, 0)) {
                                                        if (off < 0)
                                                                off = 0;
                                                        sf.byteOffset = (u32)off;
                                                }

                                                ImGui::TableSetColumnIndex(3);
                                                if (ImGui::SmallButton("X"))
                                                        toDelete = fi;

                                                ImGui::PopID();
                                        }
                                        if (toDelete >= 0)
                                                newVar_.structFields.erase(newVar_.structFields.begin() + toDelete);
                                        ImGui::EndTable();
                                }

                                if (ImGui::Button(tr("+ Field", "+ 添加字段"))) {
                                        VarEntry::StructField sf{};
                                        if (!newVar_.structFields.empty()) {
                                                const auto &last = newVar_.structFields.back();
                                                sf.byteOffset    = last.byteOffset + Parser::typeBytes(last.type);
                                        }
                                        snprintf(sf.name, sizeof(sf.name), "field%d", (int)newVar_.structFields.size());
                                        newVar_.structFields.push_back(sf);
                                }
                        }
                }

                if (newVar_.port == PortType::AUDIO) {
                        newVar_.writable = false;
                        ImGui::TextDisabled("%s", tr("Read-only", "只读"));
                } else {
                        ImGui::Checkbox(tr("Writable", "可写"), &newVar_.writable);
                }

                if (ImGui::Button("OK", ImVec2(120, 0))) {
                        VarEntry v;
                        v.name     = newVar_.name;
                        v.type     = newVar_.type;
                        v.port     = newVar_.port;
                        v.addr     = newVar_.addr;
                        v.writable = newVar_.writable;
                        if (v.port == PortType::SHM) {
                                snprintf(v.shm.name, sizeof(v.shm.name), "%s", newVar_.shmName);
                                v.shm.inited = false;
                        }
                        if (v.port == PortType::AUDIO) {
                                v.type              = DataType::F32;
                                v.addr              = (u64)std::max(0, newVar_.audioDeviceIndex);
                                v.audio.deviceIndex = newVar_.audioDeviceIndex;
                                snprintf(v.audio.deviceName, sizeof(v.audio.deviceName), "%s", newVar_.audioDeviceName);
                        }
                        if (v.port == PortType::LOCAL) {
                                size_t bsz;
                                if (newVar_.structMode && !newVar_.structFields.empty()) {
                                        v.structFields = newVar_.structFields;
                                        bsz            = 0;
                                        for (const auto &sf : v.structFields)
                                                bsz = std::max(bsz, (size_t)(sf.byteOffset + Parser::typeBytes(sf.type)));
                                        if (bsz == 0)
                                                bsz = 8;
                                } else {
                                        bsz = (v.addr > 0) ? (size_t)v.addr : 8;
                                }
                                std::lock_guard<std::mutex> lk(mtxLocal_);
                                localBufs_.emplace(v.name, std::vector<uint8_t>(bsz, 0));
                        }
                        vars_.push_back(v);
                        isModified_        = true;
                        propertiesChanged_ = true;
                        state_             = WindowState::None;
                        newVar_.structMode = false;
                        newVar_.structFields.clear();
                        ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Cancel", "取消"), ImVec2(120, 0))) {
                        state_             = WindowState::None;
                        newVar_.structMode = false;
                        newVar_.structFields.clear();
                        ImGui::CloseCurrentPopup();
                }
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
        ImGui::OpenPopup("###EditEnumDef");
        if (ImGui::BeginPopupModal(tr("Edit Enum Definition###EditEnumDef", "编辑枚举定义###EditEnumDef"),
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
                VarEntry &v = vars_[enumEditIdx_];

                // Pre-populate from DWARF on first open (when user hasn't set anything yet)
                if (v.enumDefs.empty()) {
                        std::lock_guard    lk(mtxElf_);
                        const dwarf::Type *et = resolveAlias(dwarfInfo_, v.typeOff);
                        if (et && et->kind == dwarf::TypeKind::ENUM) {
                                for (const auto &e : et->enums)
                                        v.enumDefs.push_back({e.name, e.value});
                        }
                }

                ImGui::Text(tr("Variable: %s", "变量: %s"), v.name.c_str());
                ImGui::Separator();

                constexpr ImGuiTableFlags tfl =
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
                if (ImGui::BeginTable("EnumDefTable", 3, tfl, ImVec2(400, 200))) {
                        ImGui::TableSetupColumn(tr("Label###col_label", "标签###col_label"),
                                                ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn(
                            tr("Value###col_enumval", "数值###col_enumval"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
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
                                if (ui::SmallButton("x", ui::BtnStyle::Danger))
                                        deleteIdx = j;
                                ImGui::PopID();
                        }
                        if (deleteIdx >= 0)
                                v.enumDefs.erase(v.enumDefs.begin() + deleteIdx);
                        ImGui::EndTable();
                }

                if (ImGui::Button(tr("+ Add", "+ 添加")))
                        v.enumDefs.push_back({"", 0});
                ImGui::SameLine();
                if (ImGui::Button(tr("Clear All", "全部清空")))
                        v.enumDefs.clear();
                ImGui::SameLine(0, 40);
                if (ImGui::Button(tr("Close", "关闭"), ImVec2(80, 0))) {
                        isModified_  = true;
                        enumEditIdx_ = -1;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }
}

void
Variable::drawSubEnumEditPopup()
{
        if (enumSubEditParentIdx_ < 0 || enumSubEditParentIdx_ >= (int)vars_.size()) {
                enumSubEditParentIdx_ = -1;
                return;
        }
        ImGui::OpenPopup("###EditMemberEnumDef");
        if (ImGui::BeginPopupModal(
                tr("Edit Member Enum Definition###EditMemberEnumDef", "编辑成员枚举定义###EditMemberEnumDef"),
                nullptr,
                ImGuiWindowFlags_AlwaysAutoResize)) {
                VarEntry &parent = vars_[enumSubEditParentIdx_];
                auto     &defs   = parent.memberEnumDefs[enumSubEditMemberPath_];

                if (defs.empty()) {
                        std::lock_guard    lk(mtxElf_);
                        const dwarf::Type *et = resolveAlias(dwarfInfo_, enumSubEditMemberTypeOff_);
                        if (et && et->kind == dwarf::TypeKind::ENUM)
                                for (const auto &e : et->enums)
                                        defs.push_back({e.name, e.value});
                }

                ImGui::Text(tr("Member: %s", "成员: %s"), enumSubEditMemberPath_.c_str());
                ImGui::Separator();

                constexpr ImGuiTableFlags tfl =
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
                if (ImGui::BeginTable("SubEnumDefTable", 3, tfl, ImVec2(400, 200))) {
                        ImGui::TableSetupColumn(tr("Label###col_label", "标签###col_label"),
                                                ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn(
                            tr("Value###col_enumval", "数值###col_enumval"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 24.0f);
                        ImGui::TableHeadersRow();

                        int deleteIdx = -1;
                        for (int j = 0; j < (int)defs.size(); ++j) {
                                ImGui::PushID(j);
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::SetNextItemWidth(-1);
                                char nameBuf[64];
                                snprintf(nameBuf, sizeof(nameBuf), "%s", defs[j].name.c_str());
                                if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
                                        defs[j].name = nameBuf;
                                ImGui::TableSetColumnIndex(1);
                                ImGui::SetNextItemWidth(-1);
                                int ival = (int)defs[j].value;
                                if (ImGui::InputInt("##val", &ival, 0, 0))
                                        defs[j].value = ival;
                                ImGui::TableSetColumnIndex(2);
                                if (ui::SmallButton("x", ui::BtnStyle::Danger))
                                        deleteIdx = j;
                                ImGui::PopID();
                        }
                        if (deleteIdx >= 0)
                                defs.erase(defs.begin() + deleteIdx);
                        ImGui::EndTable();
                }

                if (ImGui::Button(tr("+ Add", "+ 添加")))
                        defs.push_back({"", 0});
                ImGui::SameLine();
                if (ImGui::Button(tr("Clear All", "全部清空")))
                        defs.clear();
                ImGui::SameLine(0, 40);
                if (ImGui::Button(tr("Close", "关闭"), ImVec2(80, 0))) {
                        isModified_           = true;
                        enumSubEditParentIdx_ = -1;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }
}

void
Variable::drawEditPropertiesPopup()
{
        if (editPropIdx_ < 0 || editPropIdx_ >= (int)vars_.size()) {
                editPropIdx_ = -1;
                return;
        }
        ImGui::OpenPopup("###EditVarProperties");
        if (ImGui::BeginPopupModal(tr("Edit Variable Properties###EditVarProperties", "编辑变量属性###EditVarProperties"),
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {

                ImGui::Text("%s", tr("Name", "名称"));
                ImGui::SameLine(100);
                ImGui::SetNextItemWidth(260);
                ImGui::InputText("##editPropName", editPropBuf_.name, sizeof(editPropBuf_.name));

                ImGui::Text("%s", tr("Type", "类型"));
                ImGui::SameLine(100);
                ImGui::SetNextItemWidth(260);
                if (editPropBuf_.port == PortType::LOCAL) {
                        static const char *typesLocal[] = {
                            "U8", "U16", "U32", "U64", "I8", "I16", "I32", "I64", "F32", "F64", "STRUCT"};
                        static const DataType typeValsLocal[] = {DataType::U8,
                                                                 DataType::U16,
                                                                 DataType::U32,
                                                                 DataType::U64,
                                                                 DataType::I8,
                                                                 DataType::I16,
                                                                 DataType::I32,
                                                                 DataType::I64,
                                                                 DataType::F32,
                                                                 DataType::F64,
                                                                 DataType::UNKNOWN};
                        int                   localTypeIdx    = editPropBuf_.structMode ? 10 : 2;
                        if (!editPropBuf_.structMode) {
                                const char *curName = Parser::dataTypeToStr(editPropBuf_.type);
                                for (int k = 0; k < 10; ++k)
                                        if (strcmp(typesLocal[k], curName) == 0) {
                                                localTypeIdx = k;
                                                break;
                                        }
                        }
                        if (ImGui::Combo("##editPropType", &localTypeIdx, typesLocal, IM_ARRAYSIZE(typesLocal))) {
                                if (localTypeIdx == 10) {
                                        editPropBuf_.structMode = true;
                                } else {
                                        editPropBuf_.structMode = false;
                                        editPropBuf_.type       = typeValsLocal[localTypeIdx];
                                }
                        }
                } else {
                        static const char *types[] = {"U8", "U16", "U32", "U64", "I8", "I16", "I32", "I64", "F32", "F64"};
                        const char        *curName = Parser::dataTypeToStr(editPropBuf_.type);
                        int                typeIdx = 2;
                        for (int k = 0; k < IM_ARRAYSIZE(types); ++k)
                                if (strcmp(types[k], curName) == 0) {
                                        typeIdx = k;
                                        break;
                                }
                        if (ImGui::Combo("##editPropType", &typeIdx, types, IM_ARRAYSIZE(types)))
                                editPropBuf_.type = Parser::strToDataType(types[typeIdx]);
                }

                ImGui::Text("%s", tr("Address", "地址"));
                ImGui::SameLine(100);
                ImGui::SetNextItemWidth(260);
                ImGui::InputText("##editPropAddr", editPropBuf_.addrBuf, sizeof(editPropBuf_.addrBuf));

                ImGui::Text("%s", tr("Port", "端口"));
                ImGui::SameLine(100);
                ImGui::SetNextItemWidth(260);
                static const char    *ports[]   = {"JLINK", "SHM", "LOCAL", "MANUAL", "AUDIO"};
                static const PortType portMap[] = {
                    PortType::JLINK, PortType::SHM, PortType::LOCAL, PortType::MANUAL, PortType::AUDIO};
                int portIdx = 0;
                for (int i = 0; i < IM_ARRAYSIZE(portMap); ++i)
                        if (portMap[i] == editPropBuf_.port) {
                                portIdx = i;
                                break;
                        }
                if (ImGui::Combo("##editPropPort", &portIdx, ports, IM_ARRAYSIZE(ports)))
                        editPropBuf_.port = portMap[portIdx];

                if (editPropBuf_.port == PortType::AUDIO) {
                        editPropBuf_.type       = DataType::F32;
                        editPropBuf_.writable   = false;
                        editPropBuf_.structMode = false;
                        ImGui::TextDisabled("%s", tr("F32 normalized PCM sample (-1..1)", "F32 归一化 PCM 采样值 (-1..1)"));
                } else if (editPropBuf_.port == PortType::LOCAL) {
                        ImGui::TextDisabled("%s",
                                            tr("In-process buffer — use &varname in SDK sequence.",
                                               "进程内缓冲区，在 SDK 序列参数中用 &varname 引用。"));
                        if (editPropBuf_.structMode) {
                                static const char *sfTypes[] = {
                                    "U8", "U16", "U32", "U64", "I8", "I16", "I32", "I64", "F32", "F64"};
                                static const DataType     sfTypeVals[] = {DataType::U8,
                                                                          DataType::U16,
                                                                          DataType::U32,
                                                                          DataType::U64,
                                                                          DataType::I8,
                                                                          DataType::I16,
                                                                          DataType::I32,
                                                                          DataType::I64,
                                                                          DataType::F32,
                                                                          DataType::F64};
                                constexpr int             kSfTypeCount = 10;
                                constexpr ImGuiTableFlags tfl =
                                    ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
                                if (ImGui::BeginTable("##epSfTbl", 4, tfl, ImVec2(0, 120))) {
                                        ImGui::TableSetupColumn(tr("Name", "名称"), ImGuiTableColumnFlags_WidthStretch);
                                        ImGui::TableSetupColumn(tr("Type", "类型"), ImGuiTableColumnFlags_WidthFixed, 60.0f);
                                        ImGui::TableSetupColumn(tr("Offset", "偏移"), ImGuiTableColumnFlags_WidthFixed, 48.0f);
                                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 22.0f);
                                        ImGui::TableHeadersRow();
                                        int toDelete = -1;
                                        for (int fi = 0; fi < (int)editPropBuf_.structFields.size(); ++fi) {
                                                auto &sf = editPropBuf_.structFields[fi];
                                                ImGui::PushID(fi);
                                                ImGui::TableNextRow();
                                                ImGui::TableSetColumnIndex(0);
                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                ImGui::InputText("##fn", sf.name, sizeof(sf.name));
                                                ImGui::TableSetColumnIndex(1);
                                                int sfIdx = 2;
                                                for (int k = 0; k < kSfTypeCount; ++k)
                                                        if (sfTypeVals[k] == sf.type) {
                                                                sfIdx = k;
                                                                break;
                                                        }
                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                if (ImGui::Combo("##ft", &sfIdx, sfTypes, kSfTypeCount))
                                                        sf.type = sfTypeVals[sfIdx];
                                                ImGui::TableSetColumnIndex(2);
                                                int off = (int)sf.byteOffset;
                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                if (ImGui::InputInt("##fo", &off, 0, 0)) {
                                                        if (off < 0)
                                                                off = 0;
                                                        sf.byteOffset = (u32)off;
                                                }
                                                ImGui::TableSetColumnIndex(3);
                                                if (ImGui::SmallButton("X"))
                                                        toDelete = fi;
                                                ImGui::PopID();
                                        }
                                        if (toDelete >= 0)
                                                editPropBuf_.structFields.erase(editPropBuf_.structFields.begin() + toDelete);
                                        ImGui::EndTable();
                                }
                                if (ImGui::Button(tr("+ Field", "+ 添加字段"))) {
                                        VarEntry::StructField sf{};
                                        if (!editPropBuf_.structFields.empty()) {
                                                const auto &last = editPropBuf_.structFields.back();
                                                sf.byteOffset    = last.byteOffset + Parser::typeBytes(last.type);
                                        }
                                        snprintf(sf.name, sizeof(sf.name), "field%d", (int)editPropBuf_.structFields.size());
                                        editPropBuf_.structFields.push_back(sf);
                                }
                        }
                }
                if (editPropBuf_.port == PortType::SHM) {
                        ImGui::Text("%s", tr("SHM Name", "SHM 名称"));
                        ImGui::SameLine(100);
                        ImGui::SetNextItemWidth(260);
                        ImGui::InputText("##editPropShm", editPropBuf_.shmName, sizeof(editPropBuf_.shmName));
                } else if (editPropBuf_.port == PortType::AUDIO) {
                        ImGui::Text("%s", tr("Audio Device", "音频设备"));
                        ImGui::SameLine(100);
                        drawAudioDeviceCombo(
                            editPropBuf_.audioDeviceIndex, editPropBuf_.audioDeviceName, sizeof(editPropBuf_.audioDeviceName));
                }

                if (editPropBuf_.port != PortType::AUDIO)
                        ImGui::Checkbox(tr("Writable", "可写"), &editPropBuf_.writable);
                else
                        ImGui::TextDisabled("%s", tr("Read-only", "只读"));

                ImGui::Separator();
                if (ui::Button(tr("Apply", "应用"), ui::BtnStyle::Success, ImVec2(100, 0))) {
                        VarEntry &v = vars_[editPropIdx_];
                        v.name      = editPropBuf_.name;
                        v.type      = editPropBuf_.type;
                        v.port      = editPropBuf_.port;
                        v.writable  = editPropBuf_.writable;
                        try {
                                v.addr = std::stoull(editPropBuf_.addrBuf, nullptr, 16);
                        } catch (...) {
                                v.addr = 0;
                        }
                        if (v.port == PortType::SHM) {
                                snprintf(v.shm.name, sizeof(v.shm.name), "%s", editPropBuf_.shmName);
                        }
                        if (v.port == PortType::AUDIO) {
                                v.type              = DataType::F32;
                                v.writable          = false;
                                v.addr              = (u64)std::max(0, editPropBuf_.audioDeviceIndex);
                                v.audio.deviceIndex = editPropBuf_.audioDeviceIndex;
                                snprintf(v.audio.deviceName, sizeof(v.audio.deviceName), "%s", editPropBuf_.audioDeviceName);
                        }
                        if (v.port == PortType::LOCAL && editPropBuf_.structMode) {
                                v.isStruct     = true;
                                v.structFields = editPropBuf_.structFields;
                                size_t totalSz = 0;
                                for (const auto &sf : v.structFields)
                                        totalSz = std::max<size_t>(totalSz, sf.byteOffset + Parser::typeBytes(sf.type));
                                if (totalSz == 0)
                                        totalSz = 8;
                                std::lock_guard lk(mtxLocal_);
                                localBufs_[v.name].assign(totalSz, 0);
                        } else if (v.port == PortType::LOCAL && !editPropBuf_.structMode) {
                                v.isStruct = false;
                                v.structFields.clear();
                                std::lock_guard lk(mtxLocal_);
                                auto            it = localBufs_.find(v.name);
                                if (it != localBufs_.end())
                                        it->second.assign(Parser::typeBytes(v.type), 0);
                        }
                        isModified_        = true;
                        propertiesChanged_ = true;
                        editPropIdx_       = -1;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Cancel", "取消"), ImVec2(100, 0))) {
                        editPropIdx_ = -1;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }
}

void
Variable::exportVarFile(const std::string &path)
{
        cJSON *root = cJSON_CreateObject();
        cJSON *vArr = cJSON_CreateArray();
        for (const auto &v : vars_) {
                cJSON *vObj = cJSON_CreateObject();
                cJSON_AddStringToObject(vObj, "name", v.name.c_str());
                cJSON_AddNumberToObject(vObj, "type", (int)v.type);
                cJSON_AddStringToObject(vObj, "typeStr", Parser::dataTypeToStr(v.type));
                cJSON_AddNumberToObject(vObj, "port", (int)v.port);
                cJSON_AddStringToObject(vObj, "portStr", portName(v.port));
                char addrBuf[32];
                snprintf(addrBuf, sizeof(addrBuf), "0x%08llX", (unsigned long long)v.addr);
                cJSON_AddStringToObject(vObj, "addr", addrBuf);
                cJSON_AddBoolToObject(vObj, "writable", v.writable);
                cJSON_AddNumberToObject(vObj, "typeOff", (double)v.typeOff);
                cJSON_AddNumberToObject(vObj, "bitOffset", v.bitOffset);
                cJSON_AddNumberToObject(vObj, "bitSize", v.bitSize);
                if (v.port == PortType::SHM) {
                        cJSON_AddStringToObject(vObj, "shmName", v.shm.name);
                }
                if (v.port == PortType::AUDIO) {
                        cJSON_AddNumberToObject(vObj, "audioDeviceIndex", v.audio.deviceIndex);
                        cJSON_AddStringToObject(vObj, "audioDeviceName", v.audio.deviceName);
                }
                if (!v.enumDefs.empty()) {
                        cJSON *eArr = cJSON_CreateArray();
                        for (const auto &e : v.enumDefs) {
                                cJSON *eObj = cJSON_CreateObject();
                                cJSON_AddStringToObject(eObj, "name", e.name.c_str());
                                cJSON_AddNumberToObject(eObj, "value", (double)e.value);
                                cJSON_AddItemToArray(eArr, eObj);
                        }
                        cJSON_AddItemToObject(vObj, "enumDefs", eArr);
                }
                cJSON_AddItemToArray(vArr, vObj);
        }
        cJSON_AddItemToObject(root, "variables", vArr);

        // Remember the ELF/AXF this list was built from, stored RELATIVE to the .var
        // file so the pair stays portable when moved together (resolved on import).
        if (!elfPath_.empty()) {
                std::error_code             ec;
                const std::filesystem::path baseDir = std::filesystem::path(path).parent_path();
                const std::filesystem::path rel =
                    baseDir.empty() ? std::filesystem::path(elfPath_) : std::filesystem::relative(elfPath_, baseDir, ec);
                const std::string elfStored = (ec || rel.empty()) ? elfPath_ : rel.generic_string();
                cJSON_AddStringToObject(root, "elfPath", elfStored.c_str());
        }

        char *jsonStr = cJSON_Print(root);
        cJSON_Delete(root);
        if (jsonStr) {
                std::ofstream ofs(path, std::ios::trunc);
                if (ofs)
                        ofs << jsonStr;
                free(jsonStr);
                ImGui::InsertNotification(
                    {ImGuiToastType::Success, 3000, tr("Exported %d variables", "已导出 %d 个变量"), (int)vars_.size()});
        }
}

void
Variable::importVarFile(const std::string &path)
{
        std::ifstream ifs(path);
        if (!ifs) {
                ImGui::InsertNotification({ImGuiToastType::Error, 3000, tr("Failed to open file", "无法打开文件")});
                return;
        }
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        cJSON      *root = cJSON_Parse(content.c_str());
        if (!root) {
                ImGui::InsertNotification({ImGuiToastType::Error, 3000, tr("Invalid JSON", "JSON 格式错误")});
                return;
        }

        cJSON *vArr = cJSON_GetObjectItem(root, "variables");
        if (!cJSON_IsArray(vArr)) {
                cJSON_Delete(root);
                ImGui::InsertNotification(
                    {ImGuiToastType::Error, 3000, tr("No 'variables' array found", "未找到 'variables' 数组")});
                return;
        }

        int imported = 0;
        for (int i = 0; i < cJSON_GetArraySize(vArr); ++i) {
                cJSON *vObj  = cJSON_GetArrayItem(vArr, i);
                cJSON *nItem = cJSON_GetObjectItem(vObj, "name");
                if (!cJSON_IsString(nItem))
                        continue;

                VarEntry v;
                v.name = nItem->valuestring;

                // Check for duplicates
                if (watchHasName(v.name))
                        continue;

                if (cJSON_GetObjectItem(vObj, "type"))
                        v.type = (DataType)cJSON_GetObjectItem(vObj, "type")->valueint;
                if (cJSON_GetObjectItem(vObj, "port"))
                        v.port = (PortType)cJSON_GetObjectItem(vObj, "port")->valueint;

                // Address: prefer string "0x..." format, fall back to numeric
                cJSON *addrItem = cJSON_GetObjectItem(vObj, "addr");
                if (cJSON_IsString(addrItem)) {
                        try {
                                v.addr = std::stoull(addrItem->valuestring, nullptr, 16);
                        } catch (...) {
                                v.addr = 0;
                        }
                } else if (cJSON_IsNumber(addrItem)) {
                        v.addr = (u64)addrItem->valuedouble;
                }

                if (cJSON_GetObjectItem(vObj, "writable"))
                        v.writable = cJSON_IsTrue(cJSON_GetObjectItem(vObj, "writable"));
                if (cJSON_GetObjectItem(vObj, "typeOff"))
                        v.typeOff = (u64)cJSON_GetObjectItem(vObj, "typeOff")->valuedouble;
                if (cJSON_GetObjectItem(vObj, "bitOffset"))
                        v.bitOffset = (u32)cJSON_GetObjectItem(vObj, "bitOffset")->valueint;
                if (cJSON_GetObjectItem(vObj, "bitSize"))
                        v.bitSize = (u32)cJSON_GetObjectItem(vObj, "bitSize")->valueint;

                if (v.port == PortType::UDP) {
                        if (cJSON_IsString(cJSON_GetObjectItem(vObj, "udpIp")))
                                snprintf(v.udp.ip, sizeof(v.udp.ip), "%s", cJSON_GetObjectItem(vObj, "udpIp")->valuestring);
                        if (cJSON_GetObjectItem(vObj, "udpPort"))
                                v.udp.port = (u16)cJSON_GetObjectItem(vObj, "udpPort")->valueint;
                } else if (v.port == PortType::SHM) {
                        if (cJSON_IsString(cJSON_GetObjectItem(vObj, "shmName")))
                                snprintf(
                                    v.shm.name, sizeof(v.shm.name), "%s", cJSON_GetObjectItem(vObj, "shmName")->valuestring);
                } else if (v.port == PortType::AUDIO) {
                        v.type     = DataType::F32;
                        v.writable = false;
                        if (auto *adi = cJSON_GetObjectItem(vObj, "audioDeviceIndex"); cJSON_IsNumber(adi))
                                v.audio.deviceIndex = adi->valueint;
                        else
                                v.audio.deviceIndex = static_cast<int>(v.addr);
                        if (auto *adn = cJSON_GetObjectItem(vObj, "audioDeviceName"); cJSON_IsString(adn))
                                snprintf(v.audio.deviceName, sizeof(v.audio.deviceName), "%s", adn->valuestring);
                        else
                                snprintf(v.audio.deviceName,
                                         sizeof(v.audio.deviceName),
                                         "%s",
                                         AudioInput::instance().deviceName(v.audio.deviceIndex).c_str());
                        v.addr = (u64)std::max(0, v.audio.deviceIndex);
                }

                cJSON *eArr = cJSON_GetObjectItem(vObj, "enumDefs");
                if (cJSON_IsArray(eArr)) {
                        for (int j = 0; j < cJSON_GetArraySize(eArr); ++j) {
                                cJSON            *eObj = cJSON_GetArrayItem(eArr, j);
                                VarEntry::EnumDef d;
                                if (cJSON_IsString(cJSON_GetObjectItem(eObj, "name")))
                                        d.name = cJSON_GetObjectItem(eObj, "name")->valuestring;
                                if (cJSON_GetObjectItem(eObj, "value"))
                                        d.value = (i64)cJSON_GetObjectItem(eObj, "value")->valuedouble;
                                v.enumDefs.push_back(d);
                        }
                }

                vars_.push_back(v);
                imported++;
        }

        // Resolve the ELF/AXF path stored relative to this .var file. Loading it
        // re-syncs the imported variables' addresses/types against the symbol file.
        std::string elfToLoad;
        if (const cJSON *elfItem = cJSON_GetObjectItem(root, "elfPath");
            cJSON_IsString(elfItem) && elfItem->valuestring[0] != '\0') {
                const std::filesystem::path stored = elfItem->valuestring;
                std::error_code             ec;
                const std::filesystem::path resolved =
                    stored.is_absolute() ? stored : (std::filesystem::path(path).parent_path() / stored).lexically_normal();
                if (std::filesystem::exists(resolved, ec))
                        elfToLoad = resolved.string();
                else if (std::filesystem::exists(stored, ec)) // fall back to CWD-relative
                        elfToLoad = stored.string();
        }

        cJSON_Delete(root);
        isModified_        = true;
        propertiesChanged_ = true;
        ImGui::InsertNotification({ImGuiToastType::Success, 3000, tr("Imported %d variables", "已导入 %d 个变量"), imported});

        if (!elfToLoad.empty() && elfToLoad != elfPath_)
                loadElf(elfToLoad);
}

static void
populateShmMemberCache(const dwarf::Info                            &info,
                       const std::string                            &prefix,
                       u64                                           baseAddr,
                       u64                                           typeOff,
                       const u8                                     *blob,
                       usize                                         blobSize,
                       std::unordered_map<std::string, std::string> &cache)
{
        const dwarf::Type *t = resolveAlias(info, typeOff);
        if (!t)
                return;
        if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
                for (const auto &m : t->members) {
                        if (m.offset >= blobSize)
                                continue;
                        std::string memberPath = prefix + "." + (m.name.empty() ? "<anon>" : m.name);
                        u64         memberAddr = baseAddr + m.offset;
                        const char *sType      = scalarPayloadType(info, m.type);
                        if (sType) {
                                u32 sz = Parser::typeBytes(Parser::strToDataType(sType));
                                if (m.offset + sz <= blobSize)
                                        cache[memberPath] =
                                            decodeValue(blob + m.offset, Parser::strToDataType(sType), m.bitOffset, m.bitSize);
                        } else {
                                populateShmMemberCache(
                                    info, memberPath, memberAddr, m.type, blob + m.offset, blobSize - m.offset, cache);
                        }
                }
        } else if (t->kind == dwarf::TypeKind::ARRAY) {
                u64 elemSize = typeSize(info, t->inner);
                if (elemSize == 0)
                        return;
                u64         dim   = t->dims.empty() ? 0 : t->dims.front();
                const char *sType = scalarPayloadType(info, t->inner);
                for (u64 i = 0; i < dim; ++i) {
                        u64 offset = i * elemSize;
                        if (offset >= blobSize)
                                break;
                        std::string memberPath = prefix + "[" + std::to_string(i) + "]";
                        u64         memberAddr = baseAddr + offset;
                        if (sType) {
                                u32 sz = Parser::typeBytes(Parser::strToDataType(sType));
                                if (offset + sz <= blobSize)
                                        cache[memberPath] = decodeValue(blob + offset, Parser::strToDataType(sType), 0, 0);
                        } else {
                                populateShmMemberCache(
                                    info, memberPath, memberAddr, t->inner, blob + offset, blobSize - offset, cache);
                        }
                }
        }
}

static std::string
shmShadowKey(const VarEntry &v)
{
        return std::string(v.shm.name) + "\n" + v.name;
}

void
Variable::startPollThread()
{
        if (poll_)
                return; // already running
        poll_              = std::make_shared<PollState>();
        poll_->intervalMs  = updateIntervalMs_;
        auto        ps     = poll_; // worker holds its own ref — never touches `this`
        std::string nameCp = name_;
        pollThread_        = std::thread([ps, nameCp]() {
                LOG_I("Variable[%s] poll thread started", nameCp.c_str());
                try {
                        while (ps->running.load()) {
                                u32 interval = ps->intervalMs.load();
                                if (interval < 5)
                                        interval = 5;

                                if (!JLinkPort::instance().isConnected()) {
                                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                                        continue;
                                }

                                // Snapshot the read list published by the GUI thread.
                                std::vector<PollReq> reqs;
                                {
                                        std::lock_guard lk(ps->mtx);
                                        reqs = ps->reqs;
                                }

                                std::unordered_map<u64, PollVal> out;
                                out.reserve(reqs.size());
                                for (const auto &r : reqs) {
                                        if (!ps->running.load())
                                                break;
                                        PollVal pv{};
                                        pv.sz = r.sz;
                                        pv.ok = (r.sz > 0 && r.sz <= sizeof(pv.buf)) &&
                                                JLinkPort::instance().readMem(static_cast<u32>(r.addr), r.sz, pv.buf);
                                        out[r.addr] = pv;
                                }

                                {
                                        std::lock_guard lk(ps->mtx);
                                        ps->vals = std::move(out);
                                }

                                // Sleep in small slices so a stop request (e.g. the
                                // Variable being destroyed during a session import) is
                                // honored promptly instead of blocking the join for up
                                // to a full refresh interval.
                                for (u32 slept = 0; slept < interval && ps->running.load(); slept += 20)
                                        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(20u, interval - slept)));
                        }
                } catch (...) {
                        // Never let an exception escape — that would call
                        // std::terminate and take the whole app down.
                        LOG_E("Variable[%s] poll thread caught exception", nameCp.c_str());
                }
                LOG_I("Variable[%s] poll thread stopped", nameCp.c_str());
        });
}

void
Variable::stopPollThread()
{
        if (poll_)
                poll_->running.store(false);
        if (pollThread_.joinable())
                pollThread_.join();
        poll_.reset();
}

void
Variable::updateVariables()
{
        // Keep the worker's poll cadence in sync with the UI slider and make sure
        // the background reader is running (lazy start on first display).
        startPollThread();
        poll_->intervalMs.store(updateIntervalMs_);

        // Pause is display-only: the background poll thread keeps reading the target
        // (sampling continues), but we freeze the displayed values by not refreshing
        // valueStr while paused.
        if (g_monitorPaused.load())
                return;

        u64 now = get_mono_ts_ms();
        if (now - lastUpdateTs_ < updateIntervalMs_)
                return;
        lastUpdateTs_ = now;

        // Publish the current set of JLINK scalar reads for the worker thread, and
        // take a snapshot of the latest values it has produced.
        std::unordered_map<u64, PollVal> polledVals;
        {
                std::vector<PollReq> reqs;
                for (const auto &v : vars_) {
                        if (v.port != PortType::JLINK || v.is_editing)
                                continue;
                        const dwarf::Type *t = resolveAlias(dwarfInfo_, v.typeOff);
                        if (t && (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION ||
                                  t->kind == dwarf::TypeKind::ARRAY))
                                continue; // complex types aren't scalar-read here

                        reqs.push_back({v.addr, Parser::typeBytes(v.type)});
                }
                std::lock_guard lk(poll_->mtx);
                poll_->reqs = std::move(reqs);
                polledVals  = poll_->vals;
        }

        for (auto &v : vars_) {
                if (v.is_editing)
                        continue;

                const dwarf::Type *t         = resolveAlias(dwarfInfo_, v.typeOff);
                const bool         isComplex = t && (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION ||
                                             t->kind == dwarf::TypeKind::ARRAY);
                if (isComplex) {
                        v.valueStr = "...";
                        // For SHM structs: read the full blob and populate the member cache so the
                        // struct browser can display live values without requiring JLink.
                        if (v.port == PortType::SHM) {
                                if (!v.shm.inited) {
                                        shm_cfg_t cfg = {v.shm.name, SHM_READWRITE, 4096};
                                        if (shm_init(&v.shm.handle, cfg) == 0)
                                                v.shm.inited = true;
                                }
                                if (v.shm.inited) {
                                        usize sz = typeSize(dwarfInfo_, v.typeOff);
                                        if (sz > 0 && sz <= 4096) {
                                                auto &shadow = shmShadowBufs_[shmShadowKey(v)];
                                                if (shadow.size() != sz)
                                                        shadow.assign(sz, 0);
                                                std::vector<u8> tmp(sz);
                                                usize           nr = shm_read(&v.shm.handle, tmp.data(), sz);
                                                if (nr > 0) {
                                                        std::memcpy(shadow.data(), tmp.data(), std::min(nr, sz));
                                                        populateShmMemberCache(dwarfInfo_,
                                                                               v.name,
                                                                               v.addr,
                                                                               v.typeOff,
                                                                               shadow.data(),
                                                                               sz,
                                                                               memberValueCache_);
                                                } else if (!shadow.empty()) {
                                                        populateShmMemberCache(dwarfInfo_,
                                                                               v.name,
                                                                               v.addr,
                                                                               v.typeOff,
                                                                               shadow.data(),
                                                                               sz,
                                                                               memberValueCache_);
                                                }
                                        }
                                }
                        }
                        continue;
                }

                if (v.port == PortType::AUDIO) {
                        v.type     = DataType::F32;
                        v.writable = false;
                        if (v.audio.deviceName[0] == '\0') {
                                snprintf(v.audio.deviceName,
                                         sizeof(v.audio.deviceName),
                                         "%s",
                                         AudioInput::instance().deviceName(v.audio.deviceIndex).c_str());
                        }
                        AudioSample latest;
                        if (AudioInput::instance().latestSample(v.audio.deviceIndex, latest)) {
                                char buf[32];
                                snprintf(buf, sizeof(buf), "%.6f", latest.value);
                                v.valueStr = buf;
                        } else {
                                v.valueStr = "...";
                        }
                        continue;
                }

                u8  buf[8] = {0};
                int ret    = -2; // -2: No update, -1: Error, 0: Success
                u32 sz     = Parser::typeBytes(v.type);
                if (v.port == PortType::JLINK && JLinkPort::instance().isConnected()) {
                        // Value is read by the background poll thread; consume the
                        // cached bytes so the render thread never blocks on USB I/O.
                        auto it = polledVals.find(v.addr);
                        if (it != polledVals.end() && it->second.ok) {
                                std::memcpy(buf, it->second.buf, sizeof(buf));
                                ret = 0;
                        } else if (it != polledVals.end()) {
                                ret = -1;
                        }
                        // Not yet polled (it == end): leave ret = -2 (no update).
                } else if (v.port == PortType::SHM) {
                        if (!v.shm.inited) {
                                shm_cfg_t cfg = {v.shm.name, SHM_READWRITE, 4096};
                                if (shm_init(&v.shm.handle, cfg) == 0)
                                        v.shm.inited = true;
                                else
                                        ret = -1;
                        }
                        if (v.shm.inited) {
                                usize nr = shm_read(&v.shm.handle, buf, sz);
                                if (nr == sz) {
                                        ret = 0;
                                } else if (nr > 0) {
                                        // SHM is a byte stream, so a producer may have written a
                                        // narrower scalar than this row expects. Keep the bytes we
                                        // did receive instead of consuming them and showing "...".
                                        ret = 0;
                                } else {
                                        ret = -2;
                                }
                        }
                } else if (v.port == PortType::LOCAL) {
                        if (!v.structFields.empty()) {
                                // Manual struct: show field count summary
                                v.valueStr = "struct (" + std::to_string(v.structFields.size()) + " fields)";
                                continue;
                        }
                        std::lock_guard<std::mutex> lk(mtxLocal_);
                        auto                        it = localBufs_.find(v.name);
                        if (it != localBufs_.end() && !it->second.empty()) {
                                const auto &vd = it->second;
                                if (vd.size() > sizeof(double)) {
                                        // Struct-sized buffer: show hex dump, skip decodeValue.
                                        char   hs[80] = {};
                                        size_t showN  = std::min(vd.size(), (size_t)16);
                                        size_t pos    = 0;
                                        for (size_t b = 0; b < showN && pos + 3 < sizeof(hs); ++b)
                                                pos += (size_t)snprintf(hs + pos, sizeof(hs) - pos, "%02X ", vd[b]);
                                        if (vd.size() > 16 && pos > 0) {
                                                hs[pos - 1] = '\0';
                                                strncat(hs, "...", 4);
                                        }
                                        v.valueStr = hs;
                                        continue; // outer for(auto& v : vars_)
                                }
                                std::memcpy(buf, vd.data(), std::min<size_t>(sz, vd.size()));
                                ret = 0;
                        }
                }

                if (ret == 0) {
                        const dwarf::Type *et     = resolveAlias(dwarfInfo_, v.typeOff);
                        const bool         isEnum = (et && et->kind == dwarf::TypeKind::ENUM) || !v.enumDefs.empty();
                        if (isEnum) {
                                i64 ival = 0;
                                std::memcpy(&ival, buf, std::min((size_t)sz, sizeof(ival)));
                                v.valueStr = decodeValue(buf, v.type, v.bitOffset, v.bitSize);
                                // User-defined defs take priority over DWARF
                                bool found = false;
                                for (const auto &e : v.enumDefs)
                                        if (e.value == ival) {
                                                v.valueStr = e.name;
                                                found      = true;
                                                break;
                                        }
                                if (!found && et && et->kind == dwarf::TypeKind::ENUM)
                                        for (const auto &e : et->enums)
                                                if (e.value == ival) {
                                                        v.valueStr = e.name;
                                                        break;
                                                }
                        } else {
                                v.valueStr = decodeValue(buf, v.type, v.bitOffset, v.bitSize);
                        }
                } else if (ret == -1)
                        v.valueStr = "ERR";
                else if (v.valueStr.empty())
                        v.valueStr = "...";
        }
}

void
Variable::writeVariable(const VarEntry &v, const std::string &newVal)
{
        u8  buf[8];
        u32 sz = Parser::typeBytes(v.type);
        try {
                if (v.type == DataType::F32)
                        *(f32 *)buf = std::stof(newVal);
                else if (v.type == DataType::U32)
                        *(u32 *)buf = (u32)std::stoul(newVal);
                else if (v.type == DataType::I32)
                        *(i32 *)buf = (i32)std::stol(newVal);
                else if (v.type == DataType::U16)
                        *(u16 *)buf = (u16)std::stoul(newVal);
                else if (v.type == DataType::I16)
                        *(i16 *)buf = (i16)std::stol(newVal);
                else if (v.type == DataType::U8)
                        *(u8 *)buf = (u8)std::stoul(newVal);
                else if (v.type == DataType::I8)
                        *(i8 *)buf = (i8)std::stol(newVal);
                else if (v.type == DataType::F64)
                        *(f64 *)buf = std::stod(newVal);
                else if (v.type == DataType::U64)
                        *(u64 *)buf = std::stoull(newVal);
                else if (v.type == DataType::I64)
                        *(i64 *)buf = std::stoll(newVal);

                if (v.port == PortType::JLINK && JLinkPort::instance().isConnected()) {
                        jlink_port_write_mem((u32)v.addr, sz, buf);
                } else if (v.port == PortType::SHM && v.shm.inited) {
                        shm_write(const_cast<shm_t *>(&v.shm.handle), buf, sz);
                } else if (v.port == PortType::LOCAL) {
                        std::lock_guard<std::mutex> lk(mtxLocal_);
                        auto                       &vec = localBufs_[v.name];
                        if (vec.empty())
                                vec.resize(8, 0); // fallback
                        std::memcpy(vec.data(), buf, std::min<size_t>(sz, vec.size()));
                }
        } catch (...) {
                ImGui::InsertNotification({ImGuiToastType::Error, 2000, "Invalid input for %s", v.name.c_str()});
        }
}

void
Variable::draw()
{
        syncReadCache_.clear();

        // The Symbol Browser pane only appears once a symbol/data source has been
        // imported (ELF/AXF via drag-drop or "Load File", or a bin/json data tree),
        // or while an ELF is still loading. Until then the watch list fills the window.
        const bool showSymbolBrowser = isElfLoading_.load(std::memory_order_acquire) || !elfPath_.empty() ||
                                       !binPath_.empty() || !cfgPath_.empty() || !dataTree_.children.empty();

        if (showSymbolBrowser) {
                constexpr f32 kStripH = 22.0f; // header strip height
                constexpr f32 kSplitH = 6.0f;  // draggable splitter height
                f32           availY  = ImGui::GetContentRegionAvail().y;

                // Watch list takes all space except bottom strip (and symbol browser when expanded)
                f32 topH = symBrowserCollapsed_
                               ? availY - kStripH
                               : std::max(60.0f, std::min(watchListHeight_, availY - kStripH - kSplitH - 60.0f));

                if (ImGui::BeginChild("TopSection", ImVec2(0, topH), false)) {
                        drawVariableList();
                }
                ImGui::EndChild();

                if (!symBrowserCollapsed_) {
                        // Thin draggable splitter line
                        ImVec2 splitPos = ImGui::GetCursorScreenPos();
                        float  w        = ImGui::GetContentRegionAvail().x;
                        ImGui::InvisibleButton("##symSplit", ImVec2(w, kSplitH));
                        if (ImGui::IsItemActive())
                                watchListHeight_ = std::max(60.0f, watchListHeight_ + ImGui::GetIO().MouseDelta.y);
                        ImU32 lineCol = ImGui::IsItemHovered() || ImGui::IsItemActive()
                                            ? ImGui::GetColorU32(ImGuiCol_SeparatorActive)
                                            : ImGui::GetColorU32(ImGuiCol_Separator);
                        ImGui::GetWindowDrawList()->AddLine(ImVec2(splitPos.x, splitPos.y + kSplitH * 0.5f),
                                                            ImVec2(splitPos.x + w, splitPos.y + kSplitH * 0.5f),
                                                            lineCol,
                                                            1.0f);
                        if (ImGui::IsItemHovered())
                                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                }

                // Header strip: collapse/expand arrow + label
                ImGui::AlignTextToFramePadding();
                if (ImGui::ArrowButton("##symhdr", symBrowserCollapsed_ ? ImGuiDir_Right : ImGuiDir_Down))
                        symBrowserCollapsed_ = !symBrowserCollapsed_;
                ImGui::SameLine();
                const std::string symbolSourcePath =
                    absoluteDisplayPath(!elfPath_.empty() ? elfPath_ : (!binPath_.empty() ? binPath_ : cfgPath_));
                if (!symbolSourcePath.empty()) {
                        ImGui::TextDisabled("%s: %s", tr("Symbol Browser", "符号浏览器"), symbolSourcePath.c_str());
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", symbolSourcePath.c_str());
                } else {
                        ImGui::TextDisabled("%s", tr("Symbol Browser", "符号浏览器"));
                }

                if (!symBrowserCollapsed_) {
                        if (ImGui::BeginChild("BottomSection", ImVec2(0, 0), false)) {
                                drawSymbolBrowser();
                        }
                        ImGui::EndChild();
                }
        } else {
                // No symbol source yet — the watch list uses the full window.
                drawVariableList();
        }

        drawAddVariableDialog();
        drawEnumEditPopup();
        drawSubEnumEditPopup();
        drawEditPropertiesPopup();
        if (state_ == WindowState::LoadElf) {
                state_        = WindowState::None;
                std::string p = nativeDlgOpen("Choose Symbol File", {{"Symbol Files", {"elf", "axf", "out", "json", "bin"}}});
                if (!p.empty())
                        handleDroppedFile(p);
        }
        for (auto &path : Gui::getDroppedFiles())
                handleDroppedFile(path);
}

void
Variable::updateDisplay()
{
        if (!elfPath_.empty()) {
                try {
                        if (std::filesystem::exists(elfPath_)) {
                                auto currentWriteTime = std::filesystem::last_write_time(elfPath_);
                                if (elfLastWriteTime_ != std::filesystem::file_time_type{} &&
                                    currentWriteTime > elfLastWriteTime_) {
                                        if (loadElf(elfPath_)) {
                                                ImGui::InsertNotification(
                                                    {ImGuiToastType::Info, 3000, "ELF Hot-Reloaded: %s", elfPath_.c_str()});
                                                elfReloaded_ = true;
                                        }
                                }
                                elfLastWriteTime_ = currentWriteTime;
                        }
                } catch (...) {
                }
        }
        updateVariables();
        ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
        // "VisibleTitle###name_": the visible label tracks the user-editable title
        // while the trailing id keeps the ImGui window id (and dock layout) stable.
        const std::string winLabel = getTitle() + "###" + name_;
        if (ImGui::Begin(winLabel.c_str(), &open_, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                // Double-click the title bar (or dock tab) to rename this window.
                ImGuiWindow *win       = ImGui::GetCurrentWindow();
                ImRect       titleRect = win->DockIsActive ? win->DC.DockTabItemRect : win->TitleBarRect();
                if (ImGui::IsMouseHoveringRect(titleRect.Min, titleRect.Max, false) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        snprintf(renameBuf_, sizeof(renameBuf_), "%s", getTitle().c_str());
                        ImGui::OpenPopup("Rename Variable");
                }
                if (ImGui::BeginPopup("Rename Variable")) {
                        ImGui::TextDisabled("%s", tr("Rename", "重命名"));
                        ImGui::SetNextItemWidth(220);
                        if (ImGui::IsWindowAppearing())
                                ImGui::SetKeyboardFocusHere();
                        bool commit =
                            ImGui::InputText("##renameVar",
                                             renameBuf_,
                                             sizeof(renameBuf_),
                                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                        ImGui::SameLine();
                        if (ImGui::Button("OK"))
                                commit = true;
                        if (commit) {
                                if (renameBuf_[0] != '\0')
                                        setTitle(renameBuf_);
                                ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                }
                draw();
        }
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
                if (v.port == PortType::SHM) {
                        cJSON_AddStringToObject(vObj, "shmName", v.shm.name);
                }
                if (!v.structFields.empty()) {
                        cJSON *sfArr = cJSON_CreateArray();
                        for (const auto &sf : v.structFields) {
                                cJSON *fObj = cJSON_CreateObject();
                                cJSON_AddStringToObject(fObj, "name", sf.name);
                                cJSON_AddNumberToObject(fObj, "type", (int)sf.type);
                                cJSON_AddNumberToObject(fObj, "offset", (double)sf.byteOffset);
                                cJSON_AddItemToArray(sfArr, fObj);
                        }
                        cJSON_AddItemToObject(vObj, "structFields", sfArr);
                }
                if (!v.hiddenMembers.empty()) {
                        cJSON *hmArr = cJSON_CreateArray();
                        for (const auto &p : v.hiddenMembers)
                                cJSON_AddItemToArray(hmArr, cJSON_CreateString(p.c_str()));
                        cJSON_AddItemToObject(vObj, "hiddenMembers", hmArr);
                }
                if (!v.expandedMembers.empty()) {
                        cJSON *emArr = cJSON_CreateArray();
                        for (const auto &p : v.expandedMembers)
                                cJSON_AddItemToArray(emArr, cJSON_CreateString(p.c_str()));
                        cJSON_AddItemToObject(vObj, "expandedMembers", emArr);
                }
                if (!v.memberEnumDefs.empty()) {
                        cJSON *medObj = cJSON_CreateObject();
                        for (const auto &[mpath, mdefs] : v.memberEnumDefs) {
                                cJSON *defArr = cJSON_CreateArray();
                                for (const auto &d : mdefs) {
                                        cJSON *dObj = cJSON_CreateObject();
                                        cJSON_AddStringToObject(dObj, "name", d.name.c_str());
                                        cJSON_AddNumberToObject(dObj, "value", (double)d.value);
                                        cJSON_AddItemToArray(defArr, dObj);
                                }
                                cJSON_AddItemToObject(medObj, mpath.c_str(), defArr);
                        }
                        cJSON_AddItemToObject(vObj, "memberEnumDefs", medObj);
                }
                cJSON_AddItemToArray(vArr, vObj);
        }
        cJSON_AddItemToObject(root, "vars", vArr);
}

void
Variable::load(const void *node)
{
        const cJSON *root = static_cast<const cJSON *>(node);
        cJSON       *vArr = cJSON_GetObjectItem(root, "vars");
        if (cJSON_IsArray(vArr)) {
                vars_.clear();
                for (int i = 0; i < cJSON_GetArraySize(vArr); ++i) {
                        cJSON   *vObj = cJSON_GetArrayItem(vArr, i);
                        VarEntry v;
                        if (!cJSON_GetObjectItem(vObj, "name"))
                                continue;
                        v.name     = cJSON_GetObjectItem(vObj, "name")->valuestring;
                        v.type     = (DataType)cJSON_GetObjectItem(vObj, "type")->valueint;
                        v.port     = (PortType)cJSON_GetObjectItem(vObj, "port")->valueint;
                        v.addr     = (u64)cJSON_GetObjectItem(vObj, "addr")->valuedouble;
                        v.writable = cJSON_IsTrue(cJSON_GetObjectItem(vObj, "writable"));
                        if (cJSON_GetObjectItem(vObj, "typeOff"))
                                v.typeOff = (u64)cJSON_GetObjectItem(vObj, "typeOff")->valuedouble;
                        if (v.port == PortType::SHM) {
                                if (cJSON_GetObjectItem(vObj, "shmName"))
                                        snprintf(v.shm.name,
                                                 sizeof(v.shm.name),
                                                 "%s",
                                                 cJSON_GetObjectItem(vObj, "shmName")->valuestring);
                        }
                        if (v.port == PortType::AUDIO) {
                                v.type     = DataType::F32;
                                v.writable = false;
                                if (auto *adi = cJSON_GetObjectItem(vObj, "audioDeviceIndex"); cJSON_IsNumber(adi))
                                        v.audio.deviceIndex = adi->valueint;
                                else
                                        v.audio.deviceIndex = static_cast<int>(v.addr);
                                if (auto *adn = cJSON_GetObjectItem(vObj, "audioDeviceName"); cJSON_IsString(adn))
                                        snprintf(v.audio.deviceName, sizeof(v.audio.deviceName), "%s", adn->valuestring);
                                else
                                        snprintf(v.audio.deviceName,
                                                 sizeof(v.audio.deviceName),
                                                 "%s",
                                                 AudioInput::instance().deviceName(v.audio.deviceIndex).c_str());
                                v.addr = (u64)std::max(0, v.audio.deviceIndex);
                        }
                        if (cJSON *sfArr = cJSON_GetObjectItem(vObj, "structFields"); cJSON_IsArray(sfArr)) {
                                for (int si = 0; si < cJSON_GetArraySize(sfArr); ++si) {
                                        cJSON                *fObj = cJSON_GetArrayItem(sfArr, si);
                                        VarEntry::StructField sf;
                                        if (auto *n = cJSON_GetObjectItem(fObj, "name"); cJSON_IsString(n))
                                                strncpy(sf.name, n->valuestring, sizeof(sf.name) - 1);
                                        if (auto *ty = cJSON_GetObjectItem(fObj, "type"); cJSON_IsNumber(ty))
                                                sf.type = (DataType)(int)ty->valuedouble;
                                        if (auto *off = cJSON_GetObjectItem(fObj, "offset"); cJSON_IsNumber(off))
                                                sf.byteOffset = (u32)(int)off->valuedouble;
                                        v.structFields.push_back(sf);
                                }
                        }
                        if (v.port == PortType::LOCAL) {
                                size_t bsz = (v.addr > 0) ? (size_t)v.addr : 8;
                                if (!v.structFields.empty()) {
                                        bsz = 0;
                                        for (const auto &sf : v.structFields)
                                                bsz = std::max(bsz, (size_t)(sf.byteOffset + Parser::typeBytes(sf.type)));
                                        if (bsz == 0)
                                                bsz = 8;
                                }
                                std::lock_guard<std::mutex> lk(mtxLocal_);
                                localBufs_.emplace(v.name, std::vector<uint8_t>(bsz, 0));
                        }
                        cJSON *hmArr = cJSON_GetObjectItem(vObj, "hiddenMembers");
                        if (cJSON_IsArray(hmArr))
                                for (int k = 0; k < cJSON_GetArraySize(hmArr); ++k)
                                        if (cJSON_IsString(cJSON_GetArrayItem(hmArr, k)))
                                                v.hiddenMembers.insert(cJSON_GetArrayItem(hmArr, k)->valuestring);
                        cJSON *emArr = cJSON_GetObjectItem(vObj, "expandedMembers");
                        if (cJSON_IsArray(emArr))
                                for (int k = 0; k < cJSON_GetArraySize(emArr); ++k)
                                        if (cJSON_IsString(cJSON_GetArrayItem(emArr, k)))
                                                v.expandedMembers.insert(cJSON_GetArrayItem(emArr, k)->valuestring);
                        cJSON *medObj = cJSON_GetObjectItem(vObj, "memberEnumDefs");
                        if (cJSON_IsObject(medObj)) {
                                for (cJSON *child = medObj->child; child; child = child->next) {
                                        if (!cJSON_IsArray(child))
                                                continue;
                                        std::vector<VarEntry::EnumDef> mdefs;
                                        for (int k = 0; k < cJSON_GetArraySize(child); ++k) {
                                                cJSON            *dObj = cJSON_GetArrayItem(child, k);
                                                VarEntry::EnumDef d;
                                                if (cJSON_GetObjectItem(dObj, "name"))
                                                        d.name = cJSON_GetObjectItem(dObj, "name")->valuestring;
                                                if (cJSON_GetObjectItem(dObj, "value"))
                                                        d.value = (i64)cJSON_GetObjectItem(dObj, "value")->valuedouble;
                                                mdefs.push_back(d);
                                        }
                                        v.memberEnumDefs[child->string] = std::move(mdefs);
                                }
                        }
                        vars_.push_back(v);
                }
        }
}

void
Variable::addRecursive(const std::string &fullPath, u64 addr, u64 typeOff, PortType port, u32 bitOffset, u32 bitSize)
{
        const dwarf::Type *t = resolveAlias(dwarfInfo_, typeOff);
        if (!t)
                return;

        if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION || t->kind == dwarf::TypeKind::ARRAY) {
                VarEntry v;
                v.name      = fullPath;
                v.type      = DataType::U32;
                v.port      = port;
                v.addr      = addr;
                v.writable  = true;
                v.typeOff   = typeOff;
                v.bitOffset = bitOffset;
                v.bitSize   = bitSize;
                vars_.push_back(v);
                isModified_ = true;
        } else {
                const char *sType = scalarPayloadType(dwarfInfo_, typeOff);
                if (sType) {
                        VarEntry v;
                        v.name      = fullPath;
                        v.type      = Parser::strToDataType(sType);
                        v.port      = port;
                        v.addr      = addr;
                        v.writable  = true;
                        v.typeOff   = typeOff;
                        v.bitOffset = bitOffset;
                        v.bitSize   = bitSize;
                        vars_.push_back(v);
                        isModified_ = true;
                }
        }
}

void
Variable::drawVarVarTreeRow(const std::string &fullPath,
                            u64                addr,
                            u64                typeOff,
                            i32                depth,
                            PortType           port,
                            const std::string &shmRegionName,
                            i32                parentVarIdx)
{
        if (depth > 16)
                return;
        const dwarf::Type *t = resolveAlias(dwarfInfo_, typeOff);
        if (!t)
                return;

        const char *devLabel = (port == PortType::SHM)      ? "SHM"
                               : (port == PortType::LOCAL)  ? "LOCAL"
                               : (port == PortType::MANUAL) ? "MANUAL"
                               : (port == PortType::UDP)    ? "UDP"
                                                            : "JLINK";

        auto drawValueCell =
            [&](u64 memberAddr, const char *sType, u64 memberTypeOff, const std::string &mPath, u32 bitOffset, u32 bitSize) {
                    auto               it             = memberValueCache_.find(mPath);
                    const dwarf::Type *et             = resolveAlias(dwarfInfo_, memberTypeOff);
                    const bool         isEnum         = et && et->kind == dwarf::TypeKind::ENUM;
                    auto               decodeWithEnum = [&](const u8 *buf, u32 sz) -> std::string {
                            if (!isEnum)
                                    return decodeValue(buf, Parser::strToDataType(sType), bitOffset, bitSize);
                            // User-defined overrides for this member take priority
                            if (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()) {
                                    const auto &medMap = vars_[parentVarIdx].memberEnumDefs;
                                    auto        medIt  = medMap.find(mPath);
                                    if (medIt != medMap.end() && !medIt->second.empty()) {
                                            i64 ival = 0;
                                            std::memcpy(&ival, buf, std::min((size_t)sz, sizeof(ival)));
                                            for (const auto &e : medIt->second)
                                                    if (e.value == ival)
                                                            return e.name;
                                    }
                            }
                            i64 ival = 0;
                            std::memcpy(&ival, buf, std::min((size_t)sz, sizeof(ival)));
                            for (const auto &e : et->enums)
                                    if (e.value == ival)
                                            return e.name;
                            return decodeValue(buf, Parser::strToDataType(sType), bitOffset, bitSize);
                    };
                    if (port == PortType::JLINK) {
                            u8  buf[8]{};
                            u32 sz = Parser::typeBytes(Parser::strToDataType(sType));

                            auto itSync = syncReadCache_.find(memberAddr);
                            if (itSync != syncReadCache_.end()) {
                                    if (itSync->second.ok) {
                                            std::string val          = decodeWithEnum(itSync->second.buf, sz);
                                            memberValueCache_[mPath] = val;
                                            return val;
                                    }
                                    return std::string("ERR");
                            }

                            u64  now        = get_mono_ts_ms();
                            bool shouldRead = (now - lastUpdateTs_ < 20);
                            if (shouldRead && JLinkPort::instance().isConnected()) {
                                    bool    ok = JLinkPort::instance().readMem((u32)memberAddr, sz, buf);
                                    PollVal pv{};
                                    pv.sz = sz;
                                    pv.ok = ok;
                                    if (ok)
                                            std::memcpy(pv.buf, buf, sz);
                                    syncReadCache_[memberAddr] = pv;

                                    if (ok) {
                                            std::string val          = decodeWithEnum(buf, sz);
                                            memberValueCache_[mPath] = val;
                                            return val;
                                    }
                                    return std::string("ERR");
                            } else if (it != memberValueCache_.end()) {
                                    return it->second;
                            } else {
                                    return std::string("...");
                            }
                    } else {
                            // SHM/UDP: cache is populated by updateVariables
                            if (it != memberValueCache_.end())
                                    return it->second;
                            return std::string("...");
                    }
            };

        const std::set<std::string> *hidden =
            (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()) ? &vars_[parentVarIdx].hiddenMembers : nullptr;

        if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
                for (const auto &m : t->members) {
                        std::string memberPath = fullPath + "." + (m.name.empty() ? "<anon>" : m.name);
                        if (hidden && hidden->count(memberPath))
                                continue;
                        u64         memberAddr = addr + m.offset;
                        const char *sType      = scalarPayloadType(dwarfInfo_, m.type);
                        bool        isComplex  = !sType;

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        // "=" grip: drag this member to a monitor (DWARF members are memory-mapped,
                        // so they are not reorderable — the grip is the drag handle only).
                        {
                                char gid[192];
                                snprintf(gid, sizeof(gid), "=##mg_%s", memberPath.c_str());
                                ImGui::SmallButton(gid);
                                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                                        const std::unordered_map<std::string, std::vector<VarEntry::EnumDef>> *memOvr =
                                            (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size())
                                                ? &vars_[parentVarIdx].memberEnumDefs
                                                : nullptr;
                                        if (!sType) {
                                                StructChannelPayload sp{};
                                                snprintf(sp.device, sizeof(sp.device), "%s", devLabel);
                                                if (!shmRegionName.empty())
                                                        snprintf(sp.shmName, sizeof(sp.shmName), "%s", shmRegionName.c_str());
                                                flattenForStructPayload(dwarfInfo_, memberPath, memberAddr, m.type, sp, memOvr);
                                                ImGui::SetDragDropPayload("STRUCT_CHANNEL", &sp, sizeof(sp));
                                        } else {
                                                ChannelDropPayload p{};
                                                snprintf(p.name, sizeof(p.name), "%s", memberPath.c_str());
                                                p.addr = memberAddr;
                                                snprintf(p.type, sizeof(p.type), "%s", sType);
                                                snprintf(p.device, sizeof(p.device), "%s", devLabel);
                                                if (!shmRegionName.empty())
                                                        snprintf(p.shmName, sizeof(p.shmName), "%s", shmRegionName.c_str());
                                                p.numBytes  = (u8)typeSize(dwarfInfo_, m.type);
                                                p.typeOff   = m.type;
                                                p.bitOffset = m.bitOffset;
                                                p.bitSize   = m.bitSize;
                                                const std::vector<VarEntry::EnumDef> *leafOvr = nullptr;
                                                if (memOvr) {
                                                        auto it = memOvr->find(memberPath);
                                                        if (it != memOvr->end() && !it->second.empty())
                                                                leafOvr = &it->second;
                                                }
                                                fillEnumPayload(dwarfInfo_, m.type, p, leafOvr);
                                                ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                                        }
                                        ImGui::Text(tr("Dragging %s", "拖拽 %s"), memberPath.c_str());
                                        ImGui::EndDragDropSource();
                                }
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s", tr("Drag to add to monitor", "拖动以添加到监视器"));
                                ImGui::SameLine();
                        }
                        ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow;
                        if (!isComplex)
                                nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                        std::set<std::string> *expSetM  = (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size())
                                                              ? &vars_[parentVarIdx].expandedMembers
                                                              : nullptr;
                        const bool             wasOpenM = isComplex && expSetM && expSetM->count(memberPath) > 0;
                        if (isComplex && expSetM)
                                ImGui::SetNextItemOpen(wasOpenM);
                        // Suppress the hover/active highlight on struct sub-items.
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
                        bool open = ImGui::TreeNodeEx(m.name.empty() ? "<anon>" : m.name.c_str(),
                                                      nodeFlags & ~ImGuiTreeNodeFlags_SpanFullWidth);
                        ImGui::PopStyleColor(2);
                        if (isComplex && expSetM && open != wasOpenM) {
                                if (open)
                                        expSetM->insert(memberPath);
                                else
                                        expSetM->erase(memberPath);
                                isModified_ = true;
                        }

                        {
                                const dwarf::Type *mt           = resolveAlias(dwarfInfo_, m.type);
                                const bool         isMemberEnum = mt && mt->kind == dwarf::TypeKind::ENUM;
                                if (ImGui::BeginPopupContextItem()) {
                                        if (ImGui::MenuItem(tr("Delete", "删除"))) {
                                                if (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()) {
                                                        vars_[parentVarIdx].hiddenMembers.insert(memberPath);
                                                        isModified_ = true;
                                                }
                                        }
                                        if (isMemberEnum && ImGui::MenuItem(tr("Edit Enum Definition...", "编辑枚举定义..."))) {
                                                enumSubEditParentIdx_     = parentVarIdx;
                                                enumSubEditMemberPath_    = memberPath;
                                                enumSubEditMemberTypeOff_ = m.type;
                                        }
                                        ImGui::EndPopup();
                                }
                                // (Monitor drag source now lives on the "=" grip — see above.)
                                ImGui::TableSetColumnIndex(1);
                                if (sType) {
                                        std::string valStr =
                                            drawValueCell(memberAddr, sType, m.type, memberPath, m.bitOffset, m.bitSize);
                                        ImGui::TextUnformatted(valStr.c_str());
                                } else
                                        ImGui::TextUnformatted("...");
                                ImGui::TableSetColumnIndex(2);
                                if (isMemberEnum)
                                        ImGui::TextUnformatted("ENUM");
                                else
                                        ImGui::TextUnformatted(sType ? sType : "STRUCT");
                                ImGui::TableSetColumnIndex(3);
                                ImGui::Text("0x%08llX", (unsigned long long)memberAddr);
                                ImGui::TableSetColumnIndex(4);
                                ImGui::TextUnformatted(devLabel);

                                ImGui::TableSetColumnIndex(5);
                                bool parentWritable = true;
                                if (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()) {
                                        parentWritable = vars_[parentVarIdx].writable;
                                }
                                if (parentWritable)
                                        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "RW");
                                else
                                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "RO");
                        }

                        if (open && isComplex) {
                                drawVarVarTreeRow(memberPath, memberAddr, m.type, depth + 1, port, shmRegionName, parentVarIdx);
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
                        if (hidden && hidden->count(memberPath))
                                continue;
                        u64         memberAddr = addr + i * elemSize;
                        const char *sType      = scalarPayloadType(dwarfInfo_, t->inner);
                        bool        isComplex  = !sType;

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        // "=" grip: drag this array element to a monitor (memory-mapped, not reorderable).
                        {
                                char gid[192];
                                snprintf(gid, sizeof(gid), "=##ag_%s", memberPath.c_str());
                                ImGui::SmallButton(gid);
                                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                                        const std::unordered_map<std::string, std::vector<VarEntry::EnumDef>> *memOvr =
                                            (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size())
                                                ? &vars_[parentVarIdx].memberEnumDefs
                                                : nullptr;
                                        if (!sType) {
                                                StructChannelPayload sp{};
                                                snprintf(sp.device, sizeof(sp.device), "%s", devLabel);
                                                if (!shmRegionName.empty())
                                                        snprintf(sp.shmName, sizeof(sp.shmName), "%s", shmRegionName.c_str());
                                                flattenForStructPayload(
                                                    dwarfInfo_, memberPath, memberAddr, t->inner, sp, memOvr);
                                                ImGui::SetDragDropPayload("STRUCT_CHANNEL", &sp, sizeof(sp));
                                        } else {
                                                ChannelDropPayload p{};
                                                snprintf(p.name, sizeof(p.name), "%s", memberPath.c_str());
                                                p.addr = memberAddr;
                                                snprintf(p.type, sizeof(p.type), "%s", sType);
                                                snprintf(p.device, sizeof(p.device), "%s", devLabel);
                                                if (!shmRegionName.empty())
                                                        snprintf(p.shmName, sizeof(p.shmName), "%s", shmRegionName.c_str());
                                                p.numBytes = (u8)typeSize(dwarfInfo_, t->inner);
                                                p.typeOff  = t->inner;
                                                const std::vector<VarEntry::EnumDef> *leafOvr = nullptr;
                                                if (memOvr) {
                                                        auto it = memOvr->find(memberPath);
                                                        if (it != memOvr->end() && !it->second.empty())
                                                                leafOvr = &it->second;
                                                }
                                                fillEnumPayload(dwarfInfo_, t->inner, p, leafOvr);
                                                ImGui::SetDragDropPayload("CHANNEL", &p, sizeof(p));
                                        }
                                        ImGui::Text(tr("Dragging %s", "拖拽 %s"), memberPath.c_str());
                                        ImGui::EndDragDropSource();
                                }
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s", tr("Drag to add to monitor", "拖动以添加到监视器"));
                                ImGui::SameLine();
                        }
                        ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow;
                        if (!isComplex)
                                nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                        std::set<std::string> *expSetA  = (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size())
                                                              ? &vars_[parentVarIdx].expandedMembers
                                                              : nullptr;
                        const bool             wasOpenA = isComplex && expSetA && expSetA->count(memberPath) > 0;
                        if (isComplex && expSetA)
                                ImGui::SetNextItemOpen(wasOpenA);
                        // Suppress the hover/active highlight on struct/array sub-items.
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
                        bool open = ImGui::TreeNodeEx(idxStr.c_str(), nodeFlags & ~ImGuiTreeNodeFlags_SpanFullWidth);
                        ImGui::PopStyleColor(2);
                        if (isComplex && expSetA && open != wasOpenA) {
                                if (open)
                                        expSetA->insert(memberPath);
                                else
                                        expSetA->erase(memberPath);
                                isModified_ = true;
                        }

                        {
                                const dwarf::Type *et2        = resolveAlias(dwarfInfo_, t->inner);
                                const bool         isElemEnum = et2 && et2->kind == dwarf::TypeKind::ENUM;
                                if (ImGui::BeginPopupContextItem()) {
                                        if (ImGui::MenuItem(tr("Delete", "删除"))) {
                                                if (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()) {
                                                        vars_[parentVarIdx].hiddenMembers.insert(memberPath);
                                                        isModified_ = true;
                                                }
                                        }
                                        if (isElemEnum && ImGui::MenuItem(tr("Edit Enum Definition...", "编辑枚举定义..."))) {
                                                enumSubEditParentIdx_     = parentVarIdx;
                                                enumSubEditMemberPath_    = memberPath;
                                                enumSubEditMemberTypeOff_ = t->inner;
                                        }
                                        ImGui::EndPopup();
                                }
                                // (Monitor drag source now lives on the "=" grip — see above.)
                                ImGui::TableSetColumnIndex(1);
                                if (sType) {
                                        std::string valStr = drawValueCell(memberAddr, sType, t->inner, memberPath, 0, 0);
                                        ImGui::TextUnformatted(valStr.c_str());
                                } else
                                        ImGui::TextUnformatted("...");
                                ImGui::TableSetColumnIndex(2);
                                if (isElemEnum)
                                        ImGui::TextUnformatted("ENUM");
                                else
                                        ImGui::TextUnformatted(sType ? sType : "ARRAY");
                                ImGui::TableSetColumnIndex(3);
                                ImGui::Text("0x%08llX", (unsigned long long)memberAddr);
                                ImGui::TableSetColumnIndex(4);
                                ImGui::TextUnformatted(devLabel);

                                ImGui::TableSetColumnIndex(5);
                                bool parentWritable = true;
                                if (parentVarIdx >= 0 && parentVarIdx < (int)vars_.size()) {
                                        parentWritable = vars_[parentVarIdx].writable;
                                }
                                if (parentWritable)
                                        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "RW");
                                else
                                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "RO");
                        }

                        if (open && isComplex) {
                                drawVarVarTreeRow(
                                    memberPath, memberAddr, t->inner, depth + 1, port, shmRegionName, parentVarIdx);
                                ImGui::TreePop();
                        }
                }
        }
}

void *
Variable::getLocalBuf(const std::string &name)
{
        std::lock_guard<std::mutex> lk(mtxLocal_);
        auto                       &buf = localBufs_[name];
        if (buf.empty())
                buf.resize(8, 0); // fallback for entries not yet pre-allocated
        return buf.data();
}

void
Variable::notifyLocalWrite(const std::string & /*name*/)
{
        // updateVariables() reads localBufs_ on every tick automatically.
}

void
Variable::addLocalVar(const std::string &name, DataType type, size_t bufSize)
{
        if (watchHasName(name))
                return;
        VarEntry v;
        v.name     = name;
        v.type     = type;
        v.port     = PortType::LOCAL;
        v.addr     = 0;
        v.writable = true;
        {
                std::lock_guard<std::mutex> lk(mtxLocal_);
                localBufs_.emplace(name, std::vector<uint8_t>(bufSize > 0 ? bufSize : 8, 0));
        }
        vars_.push_back(v);
        isModified_        = true;
        propertiesChanged_ = true;
}

void
Variable::setLocalScalar(const std::string &name, double value)
{
        // Only LOCAL scalar entries are writable here; a user-configured JLINK/UDP/SHM
        // variable of the same name is intentionally left untouched.
        DataType type = DataType::UNKNOWN;
        for (const auto &v : vars_)
                if (v.name == name && v.port == PortType::LOCAL && v.structFields.empty()) {
                        type = v.type;
                        break;
                }
        if (type == DataType::UNKNOWN)
                return;

        std::lock_guard<std::mutex> lk(mtxLocal_);
        auto                        it = localBufs_.find(name);
        if (it == localBufs_.end())
                return;
        auto &buf = it->second;
        auto  put = [&](const void *src, size_t n) {
                if (buf.size() < n)
                        buf.resize(n, 0);
                std::memcpy(buf.data(), src, n);
        };
        switch (type) {
                case DataType::F32: {
                        float f = (float)value;
                        put(&f, 4);
                        break;
                }
                case DataType::F64: {
                        double d = value;
                        put(&d, 8);
                        break;
                }
                case DataType::U8: {
                        uint8_t v = (uint8_t)(int64_t)value;
                        put(&v, 1);
                        break;
                }
                case DataType::I8: {
                        int8_t v = (int8_t)(int64_t)value;
                        put(&v, 1);
                        break;
                }
                case DataType::U16: {
                        uint16_t v = (uint16_t)(int64_t)value;
                        put(&v, 2);
                        break;
                }
                case DataType::I16: {
                        int16_t v = (int16_t)(int64_t)value;
                        put(&v, 2);
                        break;
                }
                case DataType::U32: {
                        uint32_t v = (uint32_t)(int64_t)value;
                        put(&v, 4);
                        break;
                }
                case DataType::I32: {
                        int32_t v = (int32_t)(int64_t)value;
                        put(&v, 4);
                        break;
                }
                case DataType::U64: {
                        uint64_t v = (uint64_t)(int64_t)value;
                        put(&v, 8);
                        break;
                }
                case DataType::I64: {
                        int64_t v = (int64_t)value;
                        put(&v, 8);
                        break;
                }
                default:
                        return;
        }
        isModified_ = true;
}

void
Variable::addLocalStructVar(const std::string &name, const std::vector<VarEntry::StructField> &fields, size_t totalSize)
{
        if (watchHasName(name))
                return;
        // Compute buffer size from fields if totalSize was not supplied.
        size_t bsz = totalSize;
        if (bsz == 0) {
                for (const auto &sf : fields)
                        bsz = std::max(bsz, (size_t)(sf.byteOffset + Parser::typeBytes(sf.type)));
        }
        if (bsz == 0)
                bsz = 8;

        VarEntry v;
        v.name         = name;
        v.type         = DataType::U8; // per-byte access; display driven by structFields
        v.port         = PortType::LOCAL;
        v.addr         = 0;
        v.writable     = true;
        v.structFields = fields;
        {
                std::lock_guard<std::mutex> lk(mtxLocal_);
                localBufs_.emplace(name, std::vector<uint8_t>(bsz, 0));
        }
        vars_.push_back(v);
        isModified_        = true;
        propertiesChanged_ = true;
}

std::vector<Variable::PopupMember>
Variable::getPopupMembers(const std::string &varName, const std::string &pathPrefix, u64 typeOff, u64 baseAddr) const
{
        std::vector<PopupMember> result;
        const VarEntry          *ve = nullptr;
        for (const auto &v : vars_)
                if (v.name == varName) {
                        ve = &v;
                        break;
                }
        if (!ve)
                return result;
        if (typeOff == 0)
                typeOff = ve->typeOff;
        if (baseAddr == 0)
                baseAddr = ve->addr;

        const dwarf::Type *t = resolveAlias(dwarfInfo_, typeOff);
        if (!t || t->kind != dwarf::TypeKind::STRUCT)
                return result;

        for (const auto &m : t->members) {
                const std::string  mPath = pathPrefix + "." + m.name;
                const dwarf::Type *mt    = resolveAlias(dwarfInfo_, m.type);
                PopupMember        pm;
                pm.name     = m.name;
                pm.typeOff  = m.type;
                pm.addr     = baseAddr + m.offset;
                pm.isStruct = mt && mt->kind == dwarf::TypeKind::STRUCT;
                if (!pm.isStruct) {
                        auto it   = memberValueCache_.find(mPath);
                        pm.valStr = (it != memberValueCache_.end()) ? it->second : "...";
                }
                result.push_back(std::move(pm));
        }
        return result;
}

bool
Variable::getDwarfMemberAsFloat(const std::string &memberPath, float &out) const
{
        auto it = memberValueCache_.find(memberPath);
        if (it == memberValueCache_.end())
                return false;
        try {
                out = std::stof(it->second);
                return true;
        } catch (...) {
        }
        return false;
}

void
Variable::refreshDwarfMember(const std::string &memberPath)
{
        // memberPath e.g. "motorState.speed" or "motorState.ctrl.ref"
        auto dot = memberPath.find('.');
        if (dot == std::string::npos)
                return;
        const std::string varName = memberPath.substr(0, dot);
        std::string       subPath = memberPath.substr(dot + 1); // "speed" or "ctrl.ref"

        // Find parent VarEntry
        const VarEntry *ve = nullptr;
        for (const auto &v : vars_)
                if (v.name == varName) {
                        ve = &v;
                        break;
                }
        if (!ve || ve->port != PortType::JLINK)
                return;
        if (!JLinkPort::instance().isConnected())
                return;

        // Walk DWARF type tree along subPath to get final address + DataType
        u64 addr    = ve->addr;
        u64 typeOff = ve->typeOff;

        while (!subPath.empty()) {
                const dwarf::Type *t = resolveAlias(dwarfInfo_, typeOff);
                if (!t || t->kind != dwarf::TypeKind::STRUCT)
                        return;

                std::string elem;
                auto        nd = subPath.find('.');
                if (nd == std::string::npos) {
                        elem = subPath;
                        subPath.clear();
                } else {
                        elem    = subPath.substr(0, nd);
                        subPath = subPath.substr(nd + 1);
                }
                bool found = false;
                for (const auto &m : t->members) {
                        if (m.name == elem) {
                                addr    += m.offset;
                                typeOff  = m.type;
                                found    = true;
                                break;
                        }
                }
                if (!found)
                        return;
        }

        // Resolve leaf type → type string via existing scalarPayloadType helper
        const char *sType = scalarPayloadType(dwarfInfo_, typeOff);
        if (!sType)
                return; // nested struct / pointer — not a numeric leaf
        DataType dt = Parser::strToDataType(sType);

        u32 sz = Parser::typeBytes(dt);
        if (sz == 0 || sz > 8)
                return;

        // Rate-limit to ~100 ms per address
        u64   now    = get_mono_ts_ms();
        auto &lastTs = memberRefreshTs_[addr];
        if (now - lastTs < 100)
                return;
        lastTs = now;

        u8   buf[8]{};
        bool ok                       = JLinkPort::instance().readMem((u32)addr, sz, buf);
        memberValueCache_[memberPath] = ok ? decodeValue(buf, dt, 0, 0) : "ERR";
}

// Decode `fsz` bytes at `tmp` as the given scalar type into a float.
// Returns false for non-scalar/unsupported types.
static bool
decodeLocalScalar(const uint8_t *tmp, DataType type, float &out)
{
        switch (type) {
                case DataType::F32: {
                        float v;
                        std::memcpy(&v, tmp, 4);
                        out = v;
                        return true;
                }
                case DataType::F64: {
                        double v;
                        std::memcpy(&v, tmp, 8);
                        out = (float)v;
                        return true;
                }
                case DataType::U8:
                        out = (float)tmp[0];
                        return true;
                case DataType::I8:
                        out = (float)(int8_t)tmp[0];
                        return true;
                case DataType::U16: {
                        uint16_t v;
                        std::memcpy(&v, tmp, 2);
                        out = (float)v;
                        return true;
                }
                case DataType::I16: {
                        int16_t v;
                        std::memcpy(&v, tmp, 2);
                        out = (float)v;
                        return true;
                }
                case DataType::U32: {
                        uint32_t v;
                        std::memcpy(&v, tmp, 4);
                        out = (float)v;
                        return true;
                }
                case DataType::I32: {
                        int32_t v;
                        std::memcpy(&v, tmp, 4);
                        out = (float)v;
                        return true;
                }
                case DataType::U64: {
                        uint64_t v;
                        std::memcpy(&v, tmp, 8);
                        out = (float)v;
                        return true;
                }
                case DataType::I64: {
                        int64_t v;
                        std::memcpy(&v, tmp, 8);
                        out = (float)v;
                        return true;
                }
                default:
                        return false;
        }
}

// Snapshot all LOCAL variable values as (channelName, value) pairs. Channel
// names match what the watch-list drag produces: "<var>" for a scalar LOCAL
// variable and "<var>.<field>" for each field of a manual struct. The GUI feeds
// these into matching monitor scope channels each frame.
std::vector<std::pair<std::string, float>>
Variable::collectLocalChannelValues() const
{
        std::vector<std::pair<std::string, float>> outVals;
        std::lock_guard<std::mutex>                lk(mtxLocal_);
        for (const auto &v : vars_) {
                if (v.port != PortType::LOCAL && v.port != PortType::MANUAL)
                        continue;
                auto it = localBufs_.find(v.name);
                if (it == localBufs_.end() || it->second.empty())
                        continue;
                const auto &buf = it->second;

                if (!v.structFields.empty()) {
                        for (const auto &sf : v.structFields) {
                                u32 fsz = Parser::typeBytes(sf.type);
                                if (fsz == 0 || sf.byteOffset + fsz > (u32)buf.size())
                                        continue;
                                uint8_t tmp[8]{};
                                std::memcpy(tmp, buf.data() + sf.byteOffset, fsz);
                                float val;
                                if (decodeLocalScalar(tmp, sf.type, val))
                                        outVals.emplace_back(v.name + "." + sf.name, val);
                        }
                } else {
                        u32 sz = Parser::typeBytes(v.type);
                        if (sz == 0 || sz > buf.size())
                                continue;
                        uint8_t tmp[8]{};
                        std::memcpy(tmp, buf.data(), sz);
                        float val;
                        if (decodeLocalScalar(tmp, v.type, val))
                                outVals.emplace_back(v.name, val);
                }
        }
        return outVals;
}

bool
Variable::readLocalFieldAsFloat(const std::string &varName, const std::string &fieldName, float &out) const
{
        const VarEntry *ve = nullptr;
        for (const auto &v : vars_)
                if (v.name == varName) {
                        ve = &v;
                        break;
                }
        if (!ve || ve->structFields.empty())
                return false;

        const VarEntry::StructField *sf = nullptr;
        for (const auto &f : ve->structFields)
                if (fieldName == f.name) {
                        sf = &f;
                        break;
                }
        if (!sf)
                return false;

        u32 fsz = Parser::typeBytes(sf->type);
        if (fsz == 0)
                return false;

        std::lock_guard<std::mutex> lk(mtxLocal_);
        auto                        it = localBufs_.find(varName);
        if (it == localBufs_.end())
                return false;
        const auto &buf = it->second;
        if (sf->byteOffset + fsz > (u32)buf.size())
                return false;

        uint8_t tmp[8]{};
        std::memcpy(tmp, buf.data() + sf->byteOffset, fsz);
        return decodeLocalScalar(tmp, sf->type, out);
}

bool
Variable::readLocalScalar(const std::string &name, double &out) const
{
        DataType type = DataType::UNKNOWN;
        for (const auto &v : vars_)
                if (v.name == name && (v.port == PortType::LOCAL || v.port == PortType::MANUAL) && v.structFields.empty()) {
                        type = v.type;
                        break;
                }
        if (type == DataType::UNKNOWN)
                return false;

        std::lock_guard<std::mutex> lk(mtxLocal_);
        auto                        it = localBufs_.find(name);
        if (it == localBufs_.end() || it->second.empty())
                return false;
        u32 sz = Parser::typeBytes(type);
        if (sz == 0 || sz > it->second.size())
                return false;
        uint8_t tmp[8]{};
        std::memcpy(tmp, it->second.data(), sz);
        float f;
        if (!decodeLocalScalar(tmp, type, f))
                return false;
        out = f;
        return true;
}
