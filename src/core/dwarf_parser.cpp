#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

#include "core/dwarf_parser.hpp"

namespace dwarf
{

namespace
{

// DWARF tags
constexpr u64 DW_TAG_array_type       = 0x01;
constexpr u64 DW_TAG_enumeration_type = 0x04;
constexpr u64 DW_TAG_member           = 0x0d;
constexpr u64 DW_TAG_pointer_type     = 0x0f;
constexpr u64 DW_TAG_compile_unit     = 0x11;
constexpr u64 DW_TAG_structure_type   = 0x13;
constexpr u64 DW_TAG_subroutine_type  = 0x15;
constexpr u64 DW_TAG_typedef          = 0x16;
constexpr u64 DW_TAG_union_type       = 0x17;
constexpr u64 DW_TAG_subrange_type    = 0x21;
constexpr u64 DW_TAG_base_type        = 0x24;
constexpr u64 DW_TAG_const_type       = 0x26;
constexpr u64 DW_TAG_enumerator       = 0x28;
constexpr u64 DW_TAG_subprogram       = 0x2e;
constexpr u64 DW_TAG_variable         = 0x34;
constexpr u64 DW_TAG_volatile_type    = 0x35;
constexpr u64 DW_TAG_restrict_type    = 0x37;
constexpr u64 DW_TAG_class_type       = 0x02;

// DWARF attributes
constexpr u64 DW_AT_location             = 0x02;
constexpr u64 DW_AT_name                 = 0x03;
constexpr u64 DW_AT_byte_size            = 0x0b;
constexpr u64 DW_AT_bit_offset           = 0x0c;
constexpr u64 DW_AT_bit_size             = 0x0d;
constexpr u64 DW_AT_low_pc               = 0x11;
constexpr u64 DW_AT_high_pc              = 0x12;
constexpr u64 DW_AT_language             = 0x13;
constexpr u64 DW_AT_str_offsets_base     = 0x72;
constexpr u64 DW_AT_addr_base            = 0x73;
constexpr u64 DW_AT_upper_bound          = 0x2f;
constexpr u64 DW_AT_count                = 0x37;
constexpr u64 DW_AT_data_member_location = 0x38;
constexpr u64 DW_AT_const_value          = 0x1c;
constexpr u64 DW_AT_encoding             = 0x3e;
constexpr u64 DW_AT_external             = 0x3f;
constexpr u64 DW_AT_type                 = 0x49;
constexpr u64 DW_AT_data_bit_offset      = 0x6b;
constexpr u64 DW_AT_linkage_name         = 0x6e;

// DWARF forms
constexpr u64 DW_FORM_addr           = 0x01;
constexpr u64 DW_FORM_block2         = 0x03;
constexpr u64 DW_FORM_block4         = 0x04;
constexpr u64 DW_FORM_data2          = 0x05;
constexpr u64 DW_FORM_data4          = 0x06;
constexpr u64 DW_FORM_data8          = 0x07;
constexpr u64 DW_FORM_string         = 0x08;
constexpr u64 DW_FORM_block          = 0x09;
constexpr u64 DW_FORM_block1         = 0x0a;
constexpr u64 DW_FORM_data1          = 0x0b;
constexpr u64 DW_FORM_flag           = 0x0c;
constexpr u64 DW_FORM_sdata          = 0x0d;
constexpr u64 DW_FORM_strp           = 0x0e;
constexpr u64 DW_FORM_udata          = 0x0f;
constexpr u64 DW_FORM_ref_addr       = 0x10;
constexpr u64 DW_FORM_ref1           = 0x11;
constexpr u64 DW_FORM_ref2           = 0x12;
constexpr u64 DW_FORM_ref4           = 0x13;
constexpr u64 DW_FORM_ref8           = 0x14;
constexpr u64 DW_FORM_ref_udata      = 0x15;
constexpr u64 DW_FORM_indirect       = 0x16;
constexpr u64 DW_FORM_sec_offset     = 0x17;
constexpr u64 DW_FORM_exprloc        = 0x18;
constexpr u64 DW_FORM_flag_present   = 0x19;
constexpr u64 DW_FORM_strx           = 0x1a;
constexpr u64 DW_FORM_addrx          = 0x1b;
constexpr u64 DW_FORM_ref_sup4       = 0x1c;
constexpr u64 DW_FORM_strp_sup       = 0x1d;
constexpr u64 DW_FORM_data16         = 0x1e;
constexpr u64 DW_FORM_line_strp      = 0x1f;
constexpr u64 DW_FORM_ref_sig8       = 0x20;
constexpr u64 DW_FORM_implicit_const = 0x21;
constexpr u64 DW_FORM_loclistx       = 0x22;
constexpr u64 DW_FORM_rnglistx       = 0x23;
constexpr u64 DW_FORM_ref_sup8       = 0x24;
constexpr u64 DW_FORM_strx1          = 0x25;
constexpr u64 DW_FORM_strx2          = 0x26;
constexpr u64 DW_FORM_strx3          = 0x27;
constexpr u64 DW_FORM_strx4          = 0x28;
constexpr u64 DW_FORM_addrx1         = 0x29;
constexpr u64 DW_FORM_addrx2         = 0x2a;
constexpr u64 DW_FORM_addrx3         = 0x2b;
constexpr u64 DW_FORM_addrx4         = 0x2c;

constexpr u8 DW_OP_addr  = 0x03;
constexpr u8 DW_OP_addrx = 0xa1;

struct AttrValue {
        enum Kind { INVALID, U64V, S64V, STR, BLOCK };
        Kind            kind{INVALID};
        u64             u{0};
        i64             s{0};
        std::string     str{};
        std::vector<u8> block{};
};

struct AbbrevAttr {
        u64 attr{0};
        u64 form{0};
        i64 implicitConst{0};
};

struct Abbrev {
        u64                     tag{0};
        bool                    hasChildren{false};
        std::vector<AbbrevAttr> attrs{};
};

using AbbrevTable = std::unordered_map<u64, Abbrev>;

struct CuCtx {
        const u8      *info{nullptr};
        u64            cuStart{0};
        u64            cuEnd{0};
        u64            dieStart{0};
        u8             version{0};
        u8             addrSize{4};
        u8             offsetSize{4};
        const ElfInfo *elf{nullptr};
        AbbrevTable    abbrev{};
        u64            strOffsetsBase{0};
        u64            addrBase{0};
};

inline u8
readU8(const u8 *p, u64 &off, u64 end)
{
        if (off + 1 > end)
                return 0;
        return p[off++];
}

inline u16
readU16(const u8 *p, u64 &off, u64 end)
{
        if (off + 2 > end)
                return 0;
        const u16 v  = static_cast<u16>(p[off]) | (static_cast<u16>(p[off + 1]) << 8);
        off         += 2;
        return v;
}

inline u32
readU24(const u8 *p, u64 &off, u64 end)
{
        if (off + 3 > end)
                return 0;
        const u32 v  = static_cast<u32>(p[off]) | (static_cast<u32>(p[off + 1]) << 8) | (static_cast<u32>(p[off + 2]) << 16);
        off         += 3;
        return v;
}

inline u32
readU32(const u8 *p, u64 &off, u64 end)
{
        if (off + 4 > end)
                return 0;
        const u32 v  = static_cast<u32>(p[off]) | (static_cast<u32>(p[off + 1]) << 8) | (static_cast<u32>(p[off + 2]) << 16) |
                       (static_cast<u32>(p[off + 3]) << 24);
        off         += 4;
        return v;
}

inline u64
readU64(const u8 *p, u64 &off, u64 end)
{
        if (off + 8 > end)
                return 0;
        u64 v = 0;
        for (int i = 0; i < 8; ++i)
                v |= static_cast<u64>(p[off + i]) << (i * 8);
        off += 8;
        return v;
}

inline u64
readULEB(const u8 *p, u64 &off, u64 end)
{
        u64 result = 0;
        int shift  = 0;
        while (off < end) {
                const u8 b  = p[off++];
                result     |= static_cast<u64>(b & 0x7F) << shift;
                if (!(b & 0x80))
                        break;
                shift += 7;
                if (shift >= 64)
                        break;
        }
        return result;
}

inline i64
readSLEB(const u8 *p, u64 &off, u64 end)
{
        i64 result = 0;
        int shift  = 0;
        u8  b      = 0;
        while (off < end) {
                b       = p[off++];
                result |= static_cast<i64>(b & 0x7F) << shift;
                shift  += 7;
                if (!(b & 0x80))
                        break;
                if (shift >= 64)
                        break;
        }
        if (shift < 64 && (b & 0x40))
                result |= -(static_cast<i64>(1) << shift);
        return result;
}

inline u64
readAddr(const u8 *p, u64 &off, u64 end, u8 addrSize)
{
        if (addrSize == 4)
                return readU32(p, off, end);
        if (addrSize == 8)
                return readU64(p, off, end);
        return 0;
}

inline u64
readOff(const u8 *p, u64 &off, u64 end, u8 offsetSize)
{
        if (offsetSize == 4)
                return readU32(p, off, end);
        if (offsetSize == 8)
                return readU64(p, off, end);
        return 0;
}

const char *
strAt(const std::vector<u8> &tbl, u64 o)
{
        if (o >= tbl.size())
                return "";
        return reinterpret_cast<const char *>(tbl.data()) + o;
}

AbbrevTable
parseAbbrev(const std::vector<u8> &abbrev, u64 startOff)
{
        AbbrevTable t;
        if (abbrev.empty() || startOff >= abbrev.size())
                return t;

        u64       off = startOff;
        const u64 end = abbrev.size();
        while (off < end) {
                const u64 code = readULEB(abbrev.data(), off, end);
                if (code == 0)
                        break;
                Abbrev a;
                a.tag         = readULEB(abbrev.data(), off, end);
                a.hasChildren = readU8(abbrev.data(), off, end) != 0;
                while (off < end) {
                        const u64 attr = readULEB(abbrev.data(), off, end);
                        const u64 form = readULEB(abbrev.data(), off, end);
                        if (attr == 0 && form == 0)
                                break;
                        AbbrevAttr aa;
                        aa.attr = attr;
                        aa.form = form;
                        if (form == DW_FORM_implicit_const)
                                aa.implicitConst = readSLEB(abbrev.data(), off, end);
                        a.attrs.push_back(aa);
                }
                t[code] = std::move(a);
        }
        return t;
}

AttrValue
readAttr(const CuCtx &ctx, u64 &off, u64 form, i64 implicitConst)
{
        AttrValue v;
        const u8 *p   = ctx.info;
        const u64 end = ctx.cuEnd;

        switch (form) {
                case DW_FORM_addr:
                        v.kind = AttrValue::U64V;
                        v.u    = readAddr(p, off, end, ctx.addrSize);
                        break;
                case DW_FORM_data1:
                case DW_FORM_flag:
                        v.kind = AttrValue::U64V;
                        v.u    = readU8(p, off, end);
                        break;
                case DW_FORM_ref1:
                        v.kind = AttrValue::U64V;
                        v.u    = readU8(p, off, end) + ctx.cuStart;
                        break;
                case DW_FORM_data2:
                        v.kind = AttrValue::U64V;
                        v.u    = readU16(p, off, end);
                        break;
                case DW_FORM_ref2:
                        v.kind = AttrValue::U64V;
                        v.u    = readU16(p, off, end) + ctx.cuStart;
                        break;
                case DW_FORM_data4:
                        v.kind = AttrValue::U64V;
                        v.u    = readU32(p, off, end);
                        break;
                case DW_FORM_ref4:
                        v.kind = AttrValue::U64V;
                        v.u    = readU32(p, off, end) + ctx.cuStart;
                        break;
                case DW_FORM_data8:
                        v.kind = AttrValue::U64V;
                        v.u    = readU64(p, off, end);
                        break;
                case DW_FORM_ref8:
                        v.kind = AttrValue::U64V;
                        v.u    = readU64(p, off, end) + ctx.cuStart;
                        break;
                case DW_FORM_ref_sig8:
                        v.kind = AttrValue::U64V;
                        v.u    = readU64(p, off, end);
                        break;
                case DW_FORM_data16:
                        v.kind = AttrValue::BLOCK;
                        if (off + 16 <= end) {
                                v.block.assign(p + off, p + off + 16);
                                off += 16;
                        }
                        break;
                case DW_FORM_sdata:
                        v.kind = AttrValue::S64V;
                        v.s    = readSLEB(p, off, end);
                        break;
                case DW_FORM_udata:
                        v.kind = AttrValue::U64V;
                        v.u    = readULEB(p, off, end);
                        break;
                case DW_FORM_ref_udata:
                        v.kind = AttrValue::U64V;
                        v.u    = readULEB(p, off, end) + ctx.cuStart;
                        break;
                case DW_FORM_implicit_const:
                        v.kind = AttrValue::S64V;
                        v.s    = implicitConst;
                        break;
                case DW_FORM_string: {
                        v.kind = AttrValue::STR;
                        while (off < end && p[off])
                                v.str.push_back(static_cast<char>(p[off++]));
                        if (off < end)
                                ++off;
                        break;
                }
                case DW_FORM_strp: {
                        const u64 s = readOff(p, off, end, ctx.offsetSize);
                        v.kind      = AttrValue::STR;
                        v.str       = strAt(ctx.elf->debug_str, s);
                        break;
                }
                case DW_FORM_line_strp: {
                        const u64 s = readOff(p, off, end, ctx.offsetSize);
                        v.kind      = AttrValue::STR;
                        v.str       = strAt(ctx.elf->debug_line_str, s);
                        break;
                }
                case DW_FORM_strp_sup:
                case DW_FORM_ref_sup4:
                        off += 4;
                        break;
                case DW_FORM_ref_sup8:
                        off += 8;
                        break;
                case DW_FORM_strx:
                case DW_FORM_strx1:
                case DW_FORM_strx2:
                case DW_FORM_strx3:
                case DW_FORM_strx4: {
                        u64 idx = 0;
                        if (form == DW_FORM_strx)
                                idx = readULEB(p, off, end);
                        else if (form == DW_FORM_strx1)
                                idx = readU8(p, off, end);
                        else if (form == DW_FORM_strx2)
                                idx = readU16(p, off, end);
                        else if (form == DW_FORM_strx3)
                                idx = readU24(p, off, end);
                        else
                                idx = readU32(p, off, end);

                        v.kind = AttrValue::STR;
                        if (!ctx.elf->debug_str_offsets.empty()) {
                                const u8 *so       = ctx.elf->debug_str_offsets.data();
                                const u64 soSize   = ctx.elf->debug_str_offsets.size();
                                const u64 entryOff = ctx.strOffsetsBase + idx * ctx.offsetSize;
                                u64       o2       = entryOff;
                                if (entryOff + ctx.offsetSize <= soSize) {
                                        const u64 strOff = readOff(so, o2, soSize, ctx.offsetSize);
                                        v.str            = strAt(ctx.elf->debug_str, strOff);
                                }
                        }
                        break;
                }
                case DW_FORM_addrx:
                case DW_FORM_addrx1:
                case DW_FORM_addrx2:
                case DW_FORM_addrx3:
                case DW_FORM_addrx4: {
                        u64 idx = 0;
                        if (form == DW_FORM_addrx)
                                idx = readULEB(p, off, end);
                        else if (form == DW_FORM_addrx1)
                                idx = readU8(p, off, end);
                        else if (form == DW_FORM_addrx2)
                                idx = readU16(p, off, end);
                        else if (form == DW_FORM_addrx3)
                                idx = readU24(p, off, end);
                        else
                                idx = readU32(p, off, end);

                        v.kind = AttrValue::U64V;
                        if (!ctx.elf->debug_addr.empty()) {
                                const u8 *ad       = ctx.elf->debug_addr.data();
                                const u64 adSize   = ctx.elf->debug_addr.size();
                                const u64 entryOff = ctx.addrBase + idx * ctx.addrSize;
                                u64       o2       = entryOff;
                                if (entryOff + ctx.addrSize <= adSize)
                                        v.u = readAddr(ad, o2, adSize, ctx.addrSize);
                        }
                        break;
                }
                case DW_FORM_ref_addr:
                case DW_FORM_sec_offset:
                        v.kind = AttrValue::U64V;
                        v.u    = readOff(p, off, end, ctx.offsetSize);
                        break;
                case DW_FORM_block1: {
                        const u64 len = readU8(p, off, end);
                        v.kind        = AttrValue::BLOCK;
                        if (off + len <= end) {
                                v.block.assign(p + off, p + off + len);
                                off += len;
                        }
                        break;
                }
                case DW_FORM_block2: {
                        const u64 len = readU16(p, off, end);
                        v.kind        = AttrValue::BLOCK;
                        if (off + len <= end) {
                                v.block.assign(p + off, p + off + len);
                                off += len;
                        }
                        break;
                }
                case DW_FORM_block4: {
                        const u64 len = readU32(p, off, end);
                        v.kind        = AttrValue::BLOCK;
                        if (off + len <= end) {
                                v.block.assign(p + off, p + off + len);
                                off += len;
                        }
                        break;
                }
                case DW_FORM_block:
                case DW_FORM_exprloc: {
                        const u64 len = readULEB(p, off, end);
                        v.kind        = AttrValue::BLOCK;
                        if (off + len <= end) {
                                v.block.assign(p + off, p + off + len);
                                off += len;
                        }
                        break;
                }
                case DW_FORM_flag_present:
                        v.kind = AttrValue::U64V;
                        v.u    = 1;
                        break;
                case DW_FORM_indirect: {
                        const u64 newForm = readULEB(p, off, end);
                        return readAttr(ctx, off, newForm, 0);
                }
                case DW_FORM_loclistx:
                case DW_FORM_rnglistx:
                        v.kind = AttrValue::U64V;
                        v.u    = readULEB(p, off, end);
                        break;
                default:
                        v.kind = AttrValue::INVALID;
                        break;
        }
        return v;
}

u64
parseLocationAddr(const CuCtx &ctx, const std::vector<u8> &blk)
{
        if (blk.empty())
                return 0;
        const u8 op = blk[0];
        if (op == DW_OP_addr) {
                if (blk.size() < 1u + ctx.addrSize)
                        return 0;
                u64 addr = 0;
                for (u8 i = 0; i < ctx.addrSize; ++i)
                        addr |= static_cast<u64>(blk[1 + i]) << (i * 8);
                return addr;
        }
        if (op == DW_OP_addrx) {
                u64       o   = 1;
                const u64 idx = readULEB(blk.data(), o, blk.size());
                if (ctx.elf->debug_addr.empty())
                        return 0;
                const u8 *ad       = ctx.elf->debug_addr.data();
                const u64 adSize   = ctx.elf->debug_addr.size();
                const u64 entryOff = ctx.addrBase + idx * ctx.addrSize;
                u64       o2       = entryOff;
                if (entryOff + ctx.addrSize > adSize)
                        return 0;
                return readAddr(ad, o2, adSize, ctx.addrSize);
        }
        return 0;
}

const AttrValue *
findAttr(const std::unordered_map<u64, AttrValue> &m, u64 a)
{
        const auto it = m.find(a);
        return (it == m.end()) ? nullptr : &it->second;
}

std::string
attrStr(const std::unordered_map<u64, AttrValue> &m, u64 a)
{
        if (const AttrValue *v = findAttr(m, a); v && v->kind == AttrValue::STR)
                return v->str;
        return {};
}

u64
attrU64(const std::unordered_map<u64, AttrValue> &m, u64 a, u64 dflt = 0)
{
        if (const AttrValue *v = findAttr(m, a)) {
                if (v->kind == AttrValue::U64V)
                        return v->u;
                if (v->kind == AttrValue::S64V)
                        return static_cast<u64>(v->s);
        }
        return dflt;
}

i64
attrS64(const std::unordered_map<u64, AttrValue> &m, u64 a, i64 dflt = 0)
{
        if (const AttrValue *v = findAttr(m, a)) {
                if (v->kind == AttrValue::S64V)
                        return v->s;
                if (v->kind == AttrValue::U64V)
                        return static_cast<i64>(v->u);
        }
        return dflt;
}

u64
attrMemberOffset(const std::unordered_map<u64, AttrValue> &m)
{
        const AttrValue *v = findAttr(m, DW_AT_data_member_location);
        if (!v)
                return 0;
        if (v->kind == AttrValue::U64V)
                return v->u;
        if (v->kind == AttrValue::S64V)
                return static_cast<u64>(v->s);
        if (v->kind == AttrValue::BLOCK && !v->block.empty()) {
                u64       o   = 0;
                const u8 *bp  = v->block.data();
                const u64 bsz = v->block.size();
                if (bp[0] == 0x23 /* DW_OP_plus_uconst */) {
                        o = 1;
                        return readULEB(bp, o, bsz);
                }
        }
        return 0;
}

void
walkChildren(CuCtx &ctx, u64 &off, Type *parent, u64 parentTag, int depth, Info &out)
{
        const u8 *p = ctx.info;
        while (off < ctx.cuEnd) {
                const u64 dieOff = off;
                const u64 code   = readULEB(p, off, ctx.cuEnd);
                if (code == 0)
                        return;

                const auto it = ctx.abbrev.find(code);
                if (it == ctx.abbrev.end())
                        return;
                const Abbrev &ab = it->second;

                std::unordered_map<u64, AttrValue> attrs;
                attrs.reserve(ab.attrs.size());
                for (const auto &aa : ab.attrs) {
                        AttrValue av   = readAttr(ctx, off, aa.form, aa.implicitConst);
                        attrs[aa.attr] = std::move(av);
                }

                if (parentTag == DW_TAG_compile_unit) {
                        if (const AttrValue *v = findAttr(attrs, DW_AT_str_offsets_base))
                                ctx.strOffsetsBase = v->u;
                        if (const AttrValue *v = findAttr(attrs, DW_AT_addr_base))
                                ctx.addrBase = v->u;
                }

                Type *myType = nullptr;

                switch (ab.tag) {
                        case DW_TAG_variable: {
                                if (depth == 1) {
                                        Variable var;
                                        var.name = attrStr(attrs, DW_AT_name);
                                        if (var.name.empty())
                                                var.name = attrStr(attrs, DW_AT_linkage_name);
                                        var.type     = attrU64(attrs, DW_AT_type, 0);
                                        var.external = attrU64(attrs, DW_AT_external, 0) != 0;

                                        if (const AttrValue *loc = findAttr(attrs, DW_AT_location);
                                            loc && loc->kind == AttrValue::BLOCK)
                                                var.addr = parseLocationAddr(ctx, loc->block);

                                        if (!var.name.empty() && var.addr != 0)
                                                out.variables.push_back(std::move(var));
                                }
                                break;
                        }
                        case DW_TAG_member: {
                                if (parent && (parent->kind == TypeKind::STRUCT || parent->kind == TypeKind::UNION)) {
                                        Type::Member m;
                                        m.name      = attrStr(attrs, DW_AT_name);
                                        m.type      = attrU64(attrs, DW_AT_type, 0);
                                        m.offset    = attrMemberOffset(attrs);
                                        m.bitOffset = static_cast<u32>(attrU64(attrs, DW_AT_data_bit_offset, 0));
                                        m.bitSize   = static_cast<u32>(attrU64(attrs, DW_AT_bit_size, 0));
                                        parent->members.push_back(std::move(m));
                                }
                                break;
                        }
                        case DW_TAG_subrange_type: {
                                if (parent && parent->kind == TypeKind::ARRAY) {
                                        const u64 cnt = attrU64(attrs, DW_AT_count, 0);
                                        if (cnt > 0) {
                                                parent->dims.push_back(cnt);
                                        } else if (const AttrValue *ub = findAttr(attrs, DW_AT_upper_bound)) {
                                                u64 dim = 0;
                                                if (ub->kind == AttrValue::U64V)
                                                        dim = ub->u + 1;
                                                else if (ub->kind == AttrValue::S64V)
                                                        dim = static_cast<u64>(ub->s + 1);
                                                parent->dims.push_back(dim);
                                        } else {
                                                parent->dims.push_back(0);
                                        }
                                }
                                break;
                        }
                        case DW_TAG_enumerator: {
                                if (parent && parent->kind == TypeKind::ENUM) {
                                        Type::Enumerator e;
                                        e.name  = attrStr(attrs, DW_AT_name);
                                        e.value = attrS64(attrs, DW_AT_const_value, 0);
                                        parent->enums.push_back(std::move(e));
                                }
                                break;
                        }
                        case DW_TAG_base_type: {
                                Type &t    = out.types[dieOff];
                                t.kind     = TypeKind::BASE;
                                t.name     = attrStr(attrs, DW_AT_name);
                                t.size     = attrU64(attrs, DW_AT_byte_size, 0);
                                t.encoding = static_cast<u32>(attrU64(attrs, DW_AT_encoding, 0));
                                myType     = &t;
                                break;
                        }
                        case DW_TAG_pointer_type: {
                                Type &t = out.types[dieOff];
                                t.kind  = TypeKind::POINTER;
                                t.size  = attrU64(attrs, DW_AT_byte_size, ctx.addrSize);
                                t.inner = attrU64(attrs, DW_AT_type, 0);
                                myType  = &t;
                                break;
                        }
                        case DW_TAG_array_type: {
                                Type &t = out.types[dieOff];
                                t.kind  = TypeKind::ARRAY;
                                t.inner = attrU64(attrs, DW_AT_type, 0);
                                t.size  = attrU64(attrs, DW_AT_byte_size, 0);
                                myType  = &t;
                                break;
                        }
                        case DW_TAG_structure_type:
                        case DW_TAG_class_type: {
                                Type &t = out.types[dieOff];
                                t.kind  = TypeKind::STRUCT;
                                t.name  = attrStr(attrs, DW_AT_name);
                                t.size  = attrU64(attrs, DW_AT_byte_size, 0);
                                myType  = &t;
                                break;
                        }
                        case DW_TAG_union_type: {
                                Type &t = out.types[dieOff];
                                t.kind  = TypeKind::UNION;
                                t.name  = attrStr(attrs, DW_AT_name);
                                t.size  = attrU64(attrs, DW_AT_byte_size, 0);
                                myType  = &t;
                                break;
                        }
                        case DW_TAG_enumeration_type: {
                                Type &t = out.types[dieOff];
                                t.kind  = TypeKind::ENUM;
                                t.name  = attrStr(attrs, DW_AT_name);
                                t.size  = attrU64(attrs, DW_AT_byte_size, 0);
                                t.inner = attrU64(attrs, DW_AT_type, 0);
                                myType  = &t;
                                break;
                        }
                        case DW_TAG_typedef: {
                                Type &t = out.types[dieOff];
                                t.kind  = TypeKind::TYPEDEF;
                                t.name  = attrStr(attrs, DW_AT_name);
                                t.inner = attrU64(attrs, DW_AT_type, 0);
                                myType  = &t;
                                break;
                        }
                        case DW_TAG_const_type:
                        case DW_TAG_volatile_type:
                        case DW_TAG_restrict_type: {
                                Type &t = out.types[dieOff];
                                t.kind  = TypeKind::MODIFIER;
                                t.inner = attrU64(attrs, DW_AT_type, 0);
                                myType  = &t;
                                break;
                        }
                        case DW_TAG_subroutine_type: {
                                Type &t = out.types[dieOff];
                                t.kind  = TypeKind::SUBROUTINE;
                                t.inner = attrU64(attrs, DW_AT_type, 0);
                                myType  = &t;
                                break;
                        }
                        default:
                                break;
                }

                if (ab.hasChildren)
                        walkChildren(ctx, off, myType, ab.tag, depth + 1, out);
        }
}

bool
parseCu(const ElfInfo &elf, u64 &cuOff, Info &out)
{
        const u8 *info  = elf.debug_info.data();
        const u64 total = elf.debug_info.size();
        if (cuOff + 4 > total)
                return false;

        const u64 cuStart = cuOff;
        u64       o       = cuOff;

        u64 lengthField = readU32(info, o, total);
        u8  offsetSize  = 4;
        if (lengthField == 0xFFFFFFFFu) {
                offsetSize  = 8;
                lengthField = readU64(info, o, total);
        }
        const u64 cuEnd = o + lengthField;
        if (cuEnd > total)
                return false;

        const u8 version = static_cast<u8>(readU16(info, o, total));

        u8  addrSize     = elf.addrSize;
        u64 abbrevOffset = 0;

        if (version >= 5) {
                const u8 unitType = readU8(info, o, total);
                (void)unitType;
                addrSize     = readU8(info, o, total);
                abbrevOffset = (offsetSize == 8) ? readU64(info, o, total) : readU32(info, o, total);
        } else {
                abbrevOffset = (offsetSize == 8) ? readU64(info, o, total) : readU32(info, o, total);
                addrSize     = readU8(info, o, total);
        }

        CuCtx ctx;
        ctx.info       = info;
        ctx.cuStart    = cuStart;
        ctx.cuEnd      = cuEnd;
        ctx.dieStart   = o;
        ctx.version    = version;
        ctx.addrSize   = addrSize ? addrSize : elf.addrSize;
        ctx.offsetSize = offsetSize;
        ctx.elf        = &elf;
        ctx.abbrev     = parseAbbrev(elf.debug_abbrev, abbrevOffset);

        u64 dieOff = o;
        walkChildren(ctx, dieOff, nullptr, 0, 0, out);

        cuOff = cuEnd;
        return true;
}

} // anonymous namespace

bool
parse(const ElfInfo &elf, Info &out)
{
        out = Info{};
        if (elf.debug_info.empty() || elf.debug_abbrev.empty())
                return false;

        u64       off   = 0;
        const u64 total = elf.debug_info.size();
        while (off < total) {
                if (!parseCu(elf, off, out))
                        break;
        }
        out.present = true;
        return true;
}

const char *
typeKindStr(TypeKind k)
{
        switch (k) {
                case TypeKind::BASE:
                        return "base";
                case TypeKind::POINTER:
                        return "pointer";
                case TypeKind::ARRAY:
                        return "array";
                case TypeKind::STRUCT:
                        return "struct";
                case TypeKind::UNION:
                        return "union";
                case TypeKind::ENUM:
                        return "enum";
                case TypeKind::TYPEDEF:
                        return "typedef";
                case TypeKind::MODIFIER:
                        return "modifier";
                case TypeKind::SUBROUTINE:
                        return "subroutine";
                default:
                        return "?";
        }
}

} // namespace dwarf
