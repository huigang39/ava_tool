#include "core/jlink_port.hpp"
#include "gui/monitor_types.hpp"
#include "gui/variable.hpp"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <functional>
#include <limits>
#include <set>

namespace
{
const dwarf::Type *
diagnosticType(const dwarf::Info &info, u64 id)
{
        for (unsigned depth = 0; depth < 64; ++depth) {
                const auto found = info.types.find(id);
                if (found == info.types.end())
                        return nullptr;
                const auto &type = found->second;
                if (type.kind != dwarf::TypeKind::TYPEDEF && type.kind != dwarf::TypeKind::MODIFIER)
                        return &type;
                id = type.inner;
        }
        return nullptr;
}

u64
diagnosticSize(const dwarf::Info &info, u64 id, unsigned depth = 0)
{
        if (depth > 64)
                return 0;
        const auto *type = diagnosticType(info, id);
        if (!type)
                return 0;
        if (type->size)
                return type->size;
        if (type->kind == dwarf::TypeKind::ENUM)
                return type->inner ? diagnosticSize(info, type->inner, depth + 1) : 4;
        if (type->kind != dwarf::TypeKind::ARRAY)
                return 0;
        u64 size = diagnosticSize(info, type->inner, depth + 1);
        for (const auto dim : type->dims) {
                if (!dim || size > UINT64_MAX / dim)
                        return 0;
                size *= dim;
        }
        return size;
}

bool
diagnosticDescription(const dwarf::Info &info, u64 id, diagnostics::Value &val, unsigned depth = 0)
{
        if (depth > 64)
                return false;
        const auto *type = diagnosticType(info, id);
        if (!type)
                return false;
        if (type->kind == dwarf::TypeKind::ENUM) {
                if (type->inner) {
                        if (!diagnosticDescription(info, type->inner, val, depth + 1))
                                return false;
                } else {
                        const u64 size = diagnosticSize(info, id);
                        if (size != 1 && size != 2 && size != 4 && size != 8)
                                return false;
                        const bool isSigned =
                            type->encoding == 5 || type->encoding == 6 ||
                            (type->encoding == 0 && std::any_of(type->enums.begin(), type->enums.end(), [](const auto &item) {
                                     return item.val < 0;
                             }));
                        val.kind  = isSigned ? diagnostics::Kind::Signed : diagnostics::Kind::Unsigned;
                        val.width = unsigned(size * 8);
                }
                val.type = type->name.empty() ? "enum" : "enum " + type->name;
                for (const auto &item : type->enums)
                        val.enums.emplace_back(item.val, item.name);
                return true;
        }
        if (type->kind != dwarf::TypeKind::BASE || (type->size != 1 && type->size != 2 && type->size != 4 && type->size != 8))
                return false;
        val.width = unsigned(type->size * 8);
        switch (type->encoding) {
                case 2:
                        val.kind = diagnostics::Kind::Boolean;
                        break;
                case 4:
                        if (type->size == 1)
                                return false;
                        val.kind = diagnostics::Kind::Floating;
                        break;
                case 5:
                case 6:
                        val.kind = diagnostics::Kind::Signed;
                        break;
                case 7:
                case 8:
                        val.kind = diagnostics::Kind::Unsigned;
                        break;
                default:
                        return false;
        }
        val.type = type->name.empty() ? std::string(val.kind == diagnostics::Kind::Floating ? "f"
                                                    : val.kind == diagnostics::Kind::Signed ? "i"
                                                                                            : "u") +
                                            std::to_string(val.width)
                                      : type->name;
        return true;
}

struct DiagnosticNode {
        u64         address{}, type{}, offset{};
        u32         bitOffset{}, bitSize{};
        size_t      dimension{};
        PortType    port{PortType::JLINK};
        std::string owner;
        struct Pointer {
                u64 offset;
                u32 size;
        };
        std::vector<Pointer> pointers;
};

const dwarf::Type::Member *
diagnosticMember(const dwarf::Info &info, const dwarf::Type &type, const std::string &name, u64 &offset, unsigned depth = 0)
{
        if (depth > 64)
                return nullptr;
        for (const auto &member : type.members)
                if ((!member.name.empty() && member.name == name) || (member.name.empty() && name == "<anon>")) {
                        offset = member.offset;
                        return &member;
                }
        // Anonymous structs/unions promote their member names into the parent
        for (const auto &member : type.members)
                if (member.name.empty()) {
                        const auto *inner = diagnosticType(info, member.type);
                        if (!inner || (inner->kind != dwarf::TypeKind::STRUCT && inner->kind != dwarf::TypeKind::UNION))
                                continue;
                        u64 childOffset = 0;
                        if (const auto *found = diagnosticMember(info, *inner, name, childOffset, depth + 1)) {
                                if (childOffset > UINT64_MAX - member.offset)
                                        return nullptr;
                                offset = member.offset + childOffset;
                                return found;
                        }
                }
        return nullptr;
}

bool
diagnosticNode(const dwarf::Info &info, const std::vector<VarEntry> &vars, const std::string &name, DiagnosticNode &node)
{
        node = {};
        std::string root;
        auto        matches = [&](const std::string &candidate) {
                return name == candidate || (name.starts_with(candidate) && name.size() > candidate.size() &&
                                             (name[candidate.size()] == '.' || name[candidate.size()] == '['));
        };
        for (const auto &var : info.variables)
                if (matches(var.name) && var.name.size() > root.size()) {
                        root         = var.name;
                        node.address = var.addr;
                        node.type    = var.type;
                }
        for (const auto &var : vars)
                if (matches(var.name) && var.typeOff && var.name.size() >= root.size()) {
                        if (var.addrUnknown || var.flashArray)
                                return false;
                        root         = var.name;
                        node.address = var.addr;
                        node.type    = var.typeOff;
                        node.port    = var.port;
                }
        if (root.empty())
                return false;
        node.owner = root;
        size_t pos = root.size();
        for (unsigned depth = 0; pos < name.size() && depth < 128; ++depth) {
                auto *type = diagnosticType(info, node.type);
                if (!type)
                        return false;
                while (type->kind == dwarf::TypeKind::POINTER) {
                        if ((type->size != 4 && type->size != 8) || node.pointers.size() >= 16 || node.port != PortType::JLINK)
                                return false;
                        node.pointers.push_back({node.offset, u32(type->size)});
                        node.offset = 0;
                        node.type   = type->inner;
                        type        = diagnosticType(info, node.type);
                        if (!type)
                                return false;
                }
                if (name[pos] == '.') {
                        if (type->kind != dwarf::TypeKind::STRUCT && type->kind != dwarf::TypeKind::UNION)
                                return false;
                        const auto  end          = name.find_first_of(".[", pos + 1);
                        const auto  part         = name.substr(pos + 1, end == std::string::npos ? end : end - pos - 1);
                        u64         memberOffset = 0;
                        const auto *member       = diagnosticMember(info, *type, part, memberOffset);
                        if (!member || memberOffset > UINT64_MAX - node.offset)
                                return false;
                        node.offset    += memberOffset;
                        node.type       = member->type;
                        node.bitOffset  = member->bitOffset;
                        node.bitSize    = member->bitSize;
                        node.dimension  = 0;
                        pos             = end == std::string::npos ? name.size() : end;
                } else if (name[pos] == '[') {
                        if (type->kind != dwarf::TypeKind::ARRAY || node.dimension >= type->dims.size())
                                return false;
                        const auto end = name.find(']', pos);
                        if (end == std::string::npos)
                                return false;
                        u64        index  = 0;
                        const auto parsed = std::from_chars(name.data() + pos + 1, name.data() + end, index);
                        if (parsed.ec != std::errc{} || parsed.ptr != name.data() + end || index >= type->dims[node.dimension])
                                return false;
                        u64 stride = diagnosticSize(info, type->inner);
                        if (!stride)
                                return false;
                        for (size_t dim = node.dimension + 1; dim < type->dims.size(); ++dim) {
                                if (!type->dims[dim] || stride > UINT64_MAX / type->dims[dim])
                                        return false;
                                stride *= type->dims[dim];
                        }
                        if (index > (UINT64_MAX - node.offset) / stride)
                                return false;
                        node.offset += index * stride;
                        if (++node.dimension == type->dims.size()) {
                                node.type      = type->inner;
                                node.dimension = 0;
                        }
                        node.bitOffset = node.bitSize = 0;
                        pos                           = end + 1;
                } else
                        return false;
        }
        return pos == name.size();
}

void
diagnosticDecode(const std::array<u8, 8> &bytes, u32 storageSize, u32 offset, u32 bits, diagnostics::Value &val)
{
        std::memcpy(&val.bits, bytes.data(), storageSize);
        if (bits) {
                val.bits  >>= offset;
                val.width   = bits;
        }
        if (val.width < 64)
                val.bits &= (u64{1} << val.width) - 1;
        if (val.kind == diagnostics::Kind::Floating) {
                if (storageSize == 2)
                        val.real = fast_f16_to_f32(u16(val.bits));
                else if (storageSize == 4) {
                        float raw;
                        std::memcpy(&raw, bytes.data(), 4);
                        val.real = raw;
                } else
                        std::memcpy(&val.real, bytes.data(), 8);
        }
}

bool
diagnosticDataType(DataType type, diagnostics::Value &val)
{
        val.type  = Parser::dataTypeToStr(type);
        val.width = Parser::typeBytes(type) * 8;
        if (!val.width)
                return false;
        if (val.type[0] == 'f')
                val.kind = diagnostics::Kind::Floating;
        else if (val.type[0] == 'i')
                val.kind = diagnostics::Kind::Signed;
        else if (val.type[0] == 'u')
                val.kind = diagnostics::Kind::Unsigned;
        else
                return false;
        return true;
}
} // namespace

std::vector<diagnostics::Field>
Variable::diagnosticFields(const std::string &root) const
{
        std::vector<diagnostics::Field> fields;
        for (const auto &var : vars_)
                if (!var.typeOff)
                        for (const auto &field : var.structFields)
                                if (var.name + "." + field.name == root) {
                                        diagnostics::Value val;
                                        const bool         supported = diagnosticDataType(field.type, val);
                                        return {{root, val.type, supported ? "" : "不支持该类型", supported}};
                                }
        for (const auto &var : vars_)
                if (var.name == root && !var.typeOff) {
                        if (!var.structFields.empty()) {
                                for (const auto &field : var.structFields) {
                                        diagnostics::Value val;
                                        const bool         supported = diagnosticDataType(field.type, val);
                                        fields.push_back(
                                            {root + "." + field.name, val.type, supported ? "" : "不支持该类型", supported});
                                }
                        } else {
                                diagnostics::Value val;
                                const bool         supported =
                                    !var.isStruct && !var.isUnion && !var.isArray && diagnosticDataType(var.type, val);
                                fields.push_back({root, val.type, supported ? "" : "缺少结构体类型信息", supported});
                        }
                        return fields;
                }
        DiagnosticNode node;
        if (!diagnosticNode(dwarfInfo_, vars_, root, node))
                return {};
        std::set<u64>                                                   ancestors;
        constexpr size_t                                                maxFields = 16384;
        bool                                                            truncated = false;
        std::function<void(u64, const std::string &, size_t, unsigned)> walk;
        walk = [&](u64 id, const std::string &path, size_t dimension, unsigned depth) {
                if (fields.size() >= maxFields) {
                        truncated = true;
                        return;
                }
                const auto *type = diagnosticType(dwarfInfo_, id);
                if (!type || depth > 128) {
                        fields.push_back({path, "", "类型信息缺失或嵌套过深", false});
                        return;
                }
                if (type->kind == dwarf::TypeKind::ARRAY) {
                        if (dimension >= type->dims.size() || !type->dims[dimension]) {
                                fields.push_back({path, "array", "数组长度未知", false});
                                return;
                        }
                        for (u64 i = 0; i < type->dims[dimension]; ++i) {
                                if (fields.size() >= maxFields) {
                                        truncated = true;
                                        break;
                                }
                                const auto child = path + "[" + std::to_string(i) + "]";
                                if (dimension + 1 == type->dims.size())
                                        walk(type->inner, child, 0, depth + 1);
                                else
                                        walk(id, child, dimension + 1, depth + 1);
                        }
                        return;
                }
                if (type->kind == dwarf::TypeKind::STRUCT || type->kind == dwarf::TypeKind::UNION ||
                    type->kind == dwarf::TypeKind::POINTER) {
                        if (!ancestors.insert(id).second) {
                                fields.push_back({path, type->name, "递归指针类型:请显式指定所需成员路径", false});
                                return;
                        }
                        if (type->kind == dwarf::TypeKind::POINTER) {
                                if (type->size != 4 && type->size != 8)
                                        fields.push_back({path, type->name, "指针宽度不支持", false});
                                else {
                                        const auto *inner = diagnosticType(dwarfInfo_, type->inner);
                                        if (inner &&
                                            (inner->kind == dwarf::TypeKind::STRUCT || inner->kind == dwarf::TypeKind::UNION))
                                                walk(type->inner, path, 0, depth + 1);
                                        else
                                                fields.push_back({path, type->name, "仅支持指向结构体的成员路径", false});
                                }
                        } else if (type->members.empty())
                                fields.push_back({path, type->name, "结构体没有可读取成员", false});
                        else
                                for (const auto &member : type->members) {
                                        if (fields.size() >= maxFields) {
                                                truncated = true;
                                                break;
                                        }
                                        walk(member.type, member.name.empty() ? path : path + "." + member.name, 0, depth + 1);
                                }
                        ancestors.erase(id);
                        return;
                }
                diagnostics::Value val;
                const bool         supported = diagnosticDescription(dwarfInfo_, id, val);
                fields.push_back({path, val.type, supported ? "" : "不支持该类型", supported});
        };
        walk(node.type, root, node.dimension, 0);
        if (truncated)
                fields.push_back({root, "", "字段数量达到 16384 项上限,请缩小根路径", false});
        return fields;
}

bool
Variable::readDiagnosticValue(const std::string &name, diagnostics::Value &val, std::string &status)
{
        val = {};
        diagnostics::Value decoded;
        DiagnosticNode     node;
        bool               manual = false;
        for (const auto &var : vars_) {
                if (var.typeOff)
                        continue;
                DataType type = DataType::UNKNOWN;
                if (var.name == name && !var.isStruct && !var.isArray && !var.isUnion) {
                        type           = var.type;
                        node.bitOffset = var.bitOffset;
                        node.bitSize   = var.bitSize;
                } else
                        for (const auto &field : var.structFields)
                                if (var.name + "." + field.name == name) {
                                        type        = field.type;
                                        node.offset = field.byteOffset;
                                        break;
                                }
                if (type == DataType::UNKNOWN)
                        continue;
                if (var.addrUnknown || var.flashArray || !diagnosticDataType(type, decoded)) {
                        status = "变量地址失效或类型不支持";
                        return false;
                }
                node.address = var.addr;
                node.owner   = var.name;
                node.port    = var.port;
                manual       = true;
                break;
        }
        if (!manual &&
            (!diagnosticNode(dwarfInfo_, vars_, name, node) || !diagnosticDescription(dwarfInfo_, node.type, decoded))) {
                status = "变量不存在,地址失效或不是受支持的标量";
                return false;
        }
        const u32 size = node.bitSize && node.bitOffset < 64 && node.bitSize <= 64 ? (node.bitOffset + node.bitSize + 7) / 8
                                                                                   : decoded.width / 8;
        if (!size || size > 8 ||
            (node.bitSize && (decoded.kind == diagnostics::Kind::Floating || node.bitOffset >= size * 8 ||
                              node.bitSize > size * 8 - node.bitOffset || node.bitSize > decoded.width))) {
                status = "标量宽度或位域定义无效";
                return false;
        }
        std::array<u8, 8> bytes{};
        u64               diagnosticEpoch = 0;
        if (node.port == PortType::JLINK) {
                diagnosticEpoch = JLinkPort::instance().targetEpoch();
                auto read       = [&](u64 address, u32 count, std::array<u8, 8> &out) {
                        auto &port = JLinkPort::instance();
                        if (!count || count > 8 || !address || address > UINT32_MAX - (count - 1) || !port.isConnected() ||
                            port.targetEpoch() != diagnosticEpoch) {
                                status = "连接已断开或地址无效";
                                return false;
                        }
                        const auto epoch       = diagnosticEpoch;
                        const auto requestTime = get_mono_ts_ms();
                        auto      &request     = diagnosticPollReqs_[address];
                        request                = {std::max(request.size, count), requestTime};
                        startPollThread();
                        std::lock_guard lock(poll_->mtx);
                        const auto      found = poll_->vals.find(address);
                        if (found == poll_->vals.end() || found->second.epoch != epoch) {
                                status = "等待当前设备的首次采样";
                                return false;
                        }
                        const auto &sample = found->second;
                        // 在缓存锁内取时间, 避免把刚发布的样本误判为未来数据
                        const auto now = get_mono_ts_ms();
                        if (!port.isConnected() || port.targetEpoch() != epoch) {
                                status = "连接已断开或设备已切换";
                                return false;
                        }
                        if (!sample.ok) {
                                status = "读取失败";
                                return false;
                        }
                        if (sample.sz < count || now < sample.timestamp ||
                            now - sample.timestamp >
                                std::max<u64>(1000, (u64(updateIntervalMs_) + poll_->sweepMs.load()) * 3)) {
                                status = "等待刷新, 旧值已过期";
                                return false;
                        }
                        std::copy_n(sample.buf, count, out.data());
                        return true;
                };
                u64 address = node.address;
                for (const auto &pointer : node.pointers) {
                        if (pointer.offset > UINT64_MAX - address) {
                                status = "指针地址溢出";
                                return false;
                        }
                        std::array<u8, 8> pointerBytes{};
                        if (!read(address + pointer.offset, pointer.size, pointerBytes))
                                return false;
                        address = 0;
                        std::memcpy(&address, pointerBytes.data(), pointer.size);
                        if (!address || address > UINT32_MAX) {
                                status = "结构体指针为空或超出目标地址范围";
                                return false;
                        }
                }
                if (node.offset > UINT64_MAX - address) {
                        status = "成员地址溢出";
                        return false;
                }
                if (!read(address + node.offset, size, bytes))
                        return false;
        } else if (node.port == PortType::LOCAL) {
                std::lock_guard lock(mtxLocal_);
                const auto      found = localBufs_.find(node.owner);
                if (found == localBufs_.end() || node.offset > found->second.size() ||
                    size > found->second.size() - node.offset) {
                        status = "LOCAL 缓冲区不可用";
                        return false;
                }
                for (const auto &var : vars_)
                        if (var.name == node.owner && !var.readFunction.empty() &&
                            (!var.readFunctionError.empty() || var.valueStr.empty() || var.valueStr == "...")) {
                                status = "LOCAL 读取函数尚未返回有效数据或读取失败";
                                return false;
                        }
                std::copy_n(found->second.data() + node.offset, size, bytes.data());
        } else {
                status = "此读取接口仅支持 J-Link / LOCAL";
                return false;
        }
        diagnosticDecode(bytes, size, node.bitOffset, node.bitSize, decoded);
        if (node.port == PortType::JLINK &&
            (!JLinkPort::instance().isConnected() || JLinkPort::instance().targetEpoch() != diagnosticEpoch)) {
                status = "设备连接已改变,等待当前设备的采样";
                return false;
        }
        val    = std::move(decoded);
        status = "自动读取中";
        return true;
}

bool
Variable::resolveDiagnosticPath(const std::string &name, DiagnosticPath &path) const
{
        path             = {};
        const auto *root = static_cast<const dwarf::Variable *>(nullptr);
        for (const auto &candidate : dwarfInfo_.variables)
                if (name == candidate.name || (name.starts_with(candidate.name) && name.size() > candidate.name.size() &&
                                               (name[candidate.name.size()] == '.' || name[candidate.name.size()] == '[')))
                        if (!root || candidate.name.size() > root->name.size())
                                root = &candidate;
        if (!root)
                return false;
        u64 typeId   = root->type;
        path.address = root->addr;
        for (const auto &entry : vars_)
                if (entry.name == root->name) {
                        if (entry.addrUnknown || entry.port != PortType::JLINK)
                                return false;
                        path.address = entry.addr;
                        if (entry.typeOff)
                                typeId = entry.typeOff;
                        break;
                }
        auto resolve = [&](u64 id) -> const dwarf::Type * {
                for (int i = 0; i < 32; ++i) {
                        const auto it = dwarfInfo_.types.find(id);
                        if (it == dwarfInfo_.types.end())
                                return nullptr;
                        if (it->second.kind != dwarf::TypeKind::TYPEDEF && it->second.kind != dwarf::TypeKind::MODIFIER)
                                return &it->second;
                        id = it->second.inner;
                }
                return nullptr;
        };
        auto  *type   = resolve(typeId);
        size_t pos    = root->name.size();
        u64    offset = 0;
        for (int depth = 0; pos < name.size() && depth < 64; ++depth) {
                if (!type)
                        return false;
                if (type->kind == dwarf::TypeKind::POINTER) {
                        if (type->size != 4 || path.pointerOffsets.size() >= 8)
                                return false;
                        path.pointerOffsets.push_back(offset);
                        offset = 0;
                        type   = resolve(type->inner);
                        if (!type)
                                return false;
                }
                if (name[pos] == '.') {
                        if (type->kind != dwarf::TypeKind::STRUCT && type->kind != dwarf::TypeKind::UNION)
                                return false;
                        const auto end        = name.find_first_of(".[", pos + 1);
                        const auto memberName = name.substr(pos + 1, end == std::string::npos ? end : end - pos - 1);
                        const dwarf::Type::Member *member = nullptr;
                        for (const auto &m : type->members)
                                if (m.name == memberName) {
                                        member = &m;
                                        break;
                                }
                        if (!member)
                                return false;
                        offset         += member->offset;
                        path.bitOffset  = member->bitOffset;
                        path.bitSize    = member->bitSize;
                        type            = resolve(member->type);
                        pos             = end == std::string::npos ? name.size() : end;
                } else if (name[pos] == '[') {
                        if (type->kind != dwarf::TypeKind::ARRAY || type->dims.size() != 1)
                                return false;
                        const auto end = name.find(']', pos);
                        if (end == std::string::npos)
                                return false;
                        u64        index  = 0;
                        const auto parsed = std::from_chars(name.data() + pos + 1, name.data() + end, index);
                        if (parsed.ec != std::errc{} || parsed.ptr != name.data() + end || index >= type->dims[0])
                                return false;
                        type = resolve(type->inner);
                        if (!type || !type->size || index > UINT32_MAX / type->size)
                                return false;
                        offset         += index * type->size;
                        path.bitOffset = path.bitSize = 0;
                        pos                           = end + 1;
                } else
                        return false;
                if (offset > UINT32_MAX)
                        return false;
        }
        if (pos != name.size() || !type || (type->kind != dwarf::TypeKind::BASE && type->kind != dwarf::TypeKind::ENUM))
                return false;
        if (type->kind == dwarf::TypeKind::BASE && type->encoding != 2 && type->encoding != 5 && type->encoding != 6 &&
            type->encoding != 7 && type->encoding != 8)
                return false;
        if (type->size != 1 && type->size != 2 && type->size != 4)
                return false;
        path.size = u32(type->size);
        // Last offset follows the final dereference, or is relative to the root object
        path.pointerOffsets.push_back(offset);
        return path.address <= UINT32_MAX;
}

bool
Variable::hasDiagnosticWord(const std::string &name) const
{
        ChannelDropPayload scalar{};
        DiagnosticPath     path;
        return resolveDiagnosticVariable(name, scalar) || resolveDiagnosticPath(name, path);
}

bool
Variable::readDiagnosticMemory(u64 address, u32 size, u32 &word, std::string &status)
{
        if (!size || size > 4 || address > UINT32_MAX - (size - 1) || !JLinkPort::instance().isConnected()) {
                status = "连接已断开或地址无效";
                return false;
        }
        const auto epoch   = JLinkPort::instance().targetEpoch();
        auto      &request = diagnosticPollReqs_[address];
        request            = {std::max(request.size, size), get_mono_ts_ms()};
        startPollThread();
        std::lock_guard lock(poll_->mtx);
        const auto      it = poll_->vals.find(address);
        if (it == poll_->vals.end() || it->second.epoch != epoch) {
                status = "等待当前设备的首次采样";
                return false;
        }
        const auto &sample = it->second;
        const auto  now    = get_mono_ts_ms();
        if (!JLinkPort::instance().isConnected() || JLinkPort::instance().targetEpoch() != epoch) {
                status = "连接已断开或设备已切换";
                return false;
        }
        if (!sample.ok) {
                status = "读取失败";
                return false;
        }
        if (sample.sz < size || now < sample.timestamp ||
            now - sample.timestamp > std::max<u64>(1000, (u64(updateIntervalMs_) + poll_->sweepMs.load()) * 3)) {
                status = "等待刷新, 旧值已过期";
                return false;
        }
        status.clear();
        word = 0;
        std::memcpy(&word, sample.buf, size);
        return true;
}

bool
Variable::resolveDiagnosticVariable(const std::string &name, ChannelDropPayload &out) const
{
        out         = {};
        auto assign = [&](DataType type, PortType port, u64 addr, u32 offset, u32 bits) {
                const auto  bytes = Parser::typeBytes(type);
                const char *kind  = Parser::dataTypeToStr(type);
                if ((kind[0] != 'u' && kind[0] != 'i') || bytes == 0 || bytes > 4)
                        return false;
                if (port != PortType::JLINK && port != PortType::LOCAL)
                        return false;
                std::snprintf(out.name, sizeof(out.name), "%s", name.c_str());
                std::snprintf(out.type, sizeof(out.type), "%s", kind);
                std::snprintf(out.device, sizeof(out.device), "%s", port == PortType::JLINK ? "JLINK" : "LOCAL");
                out.addr      = addr;
                out.numBytes  = u8(bytes);
                out.bitOffset = offset;
                out.bitSize   = bits;
                out.srcWatch  = this;
                return true;
        };
        for (const auto &v : vars_) {
                if (v.name == name) {
                        if (v.isStruct || v.isArray || v.isUnion || v.addrUnknown || v.flashArray)
                                return false;
                        return assign(v.type, v.port, v.addr, v.bitOffset, v.bitSize);
                }
                for (const auto &field : v.structFields)
                        if (v.name + "." + field.name == name)
                                return assign(field.type, v.port, v.addr + field.byteOffset, 0, 0);
        }
        for (const auto &entry : searchPool_)
                if (entry.path == name) {
                        auto type = dwarfInfo_.types.find(entry.typeOff);
                        for (int depth = 0; type != dwarfInfo_.types.end() && depth < 32; ++depth) {
                                if (type->second.kind != dwarf::TypeKind::TYPEDEF &&
                                    type->second.kind != dwarf::TypeKind::MODIFIER)
                                        break;
                                type = dwarfInfo_.types.find(type->second.inner);
                        }
                        if (type != dwarfInfo_.types.end() && type->second.kind != dwarf::TypeKind::BASE &&
                            type->second.kind != dwarf::TypeKind::ENUM)
                                return false;
                        return assign(entry.type, entry.defaultPort, entry.addr, entry.bitOffset, entry.bitSize);
                }
        return false;
}

bool
Variable::readDiagnosticWord(const std::string &name, u32 &word, std::string &status)
{
        word = 0;
        diagnostics::Value val;
        if (!readDiagnosticValue(name, val, status))
                return false;
        if (val.kind == diagnostics::Kind::Floating || val.width > 32) {
                status = "故障页需要 8,16,32 位整数";
                return false;
        }
        word = static_cast<u32>(val.bits);
        return true;
}
