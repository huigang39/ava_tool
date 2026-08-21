#include "../inc/elf_parser.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG_ARRAY               1U
#define TAG_CLASS               2U
#define TAG_ENUM                4U
#define TAG_POINTER             15U
#define TAG_MEMBER              13U
#define TAG_STRUCT              19U
#define TAG_TYPEDEF             22U
#define TAG_UNION               23U
#define TAG_CONST               38U
#define TAG_VARIABLE            52U
#define TAG_VOLATILE            53U
#define TAG_RESTRICT            55U
#define TAG_ATOMIC              71U
#define TAG_BASE                36U
#define AT_LOCATION             2U
#define AT_BYTE_SIZE            11U
#define AT_NAME                 3U
#define AT_DATA_MEMBER_LOCATION 56U
#define AT_TYPE                 73U
#define AT_ENCODING             62U
#define FORM_ADDR               1U
#define FORM_BLOCK2             3U
#define FORM_BLOCK4             4U
#define FORM_DATA2              5U
#define FORM_DATA4              6U
#define FORM_DATA8              7U
#define FORM_STRING             8U
#define FORM_BLOCK              9U
#define FORM_BLOCK1             10U
#define FORM_DATA1              11U
#define FORM_FLAG               12U
#define FORM_SDATA              13U
#define FORM_STRP               14U
#define FORM_UDATA              15U
#define FORM_REF_ADDR           16U
#define FORM_REF1               17U
#define FORM_REF2               18U
#define FORM_REF4               19U
#define FORM_REF8               20U
#define FORM_REF_UDATA          21U
#define FORM_INDIRECT           22U
#define FORM_SEC_OFFSET         23U
#define FORM_EXPRLOC            24U
#define FORM_FLAG_PRESENT       25U

struct attr {
    uint64_t name, form;
};
struct abbrev {
    uint64_t     code, tag;
    uint8_t      children;
    struct attr *attrs;
    size_t       count;
};
struct die {
    uint32_t    offset, parent, tag, type, address, member_offset, byte_size, encoding;
    const char *name;
    uint8_t     has_address, has_member;
};

struct section {
    const uint8_t *p;
    size_t         n;
};

struct elf_parser {
    uint8_t       *file;
    size_t         size;
    struct section info, abbrev, str;
    struct die    *dies;
    size_t         die_count, die_cap;
};
static uint16_t
read_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t
read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t
read_u64(const uint8_t *p)
{
    return (uint64_t)read_u32(p) | ((uint64_t)read_u32(p + 4) << 32);
}
static int
take(const uint8_t **p, const uint8_t *end, size_t n, const uint8_t **v)
{
    if ((size_t)(end - *p) < n)
        return 0;
    *v  = *p;
    *p += n;
    return 1;
}
static int
uleb(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
    uint64_t x = 0;
    unsigned s = 0;
    while (*p < end && s < 64) {
        uint8_t b  = *(*p)++;
        x         |= (uint64_t)(b & 127) << s;
        if (!(b & 128)) {
            *v = x;
            return 1;
        }
        s += 7;
    }
    return 0;
}
static int
sleb(const uint8_t **p, const uint8_t *end, int64_t *v)
{
    uint64_t x = 0;
    unsigned s = 0;
    uint8_t  b = 0;
    while (*p < end && s < 64) {
        b  = *(*p)++;
        x |= (uint64_t)(b & 127) << s;
        s += 7;
        if (!(b & 128))
            break;
    }
    if (b & 128)
        return 0;
    if (s < 64 && (b & 64))
        x |= (~0ULL) << s;
    *v = (int64_t)x;
    return 1;
}
static int
add_die(struct elf_parser *e, const struct die *d)
{
    if (e->die_count == e->die_cap) {
        size_t      c = e->die_cap ? e->die_cap * 2 : 4096;
        struct die *q = (struct die *)realloc(e->dies, c * sizeof(*q));
        if (!q)
            return 0;
        e->dies    = q;
        e->die_cap = c;
    }
    e->dies[e->die_count++] = *d;
    return 1;
}
static void
free_abbrevs(struct abbrev *a, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        free(a[i].attrs);
    free(a);
}
static int
load_abbrevs(const struct section *s, uint32_t off, struct abbrev **out, size_t *count)
{
    const uint8_t *p = s->p + off, *end = s->p + s->n;
    struct abbrev *a = NULL;
    size_t         n = 0, cap = 0;
    while (p < end) {
        uint64_t      code, tag, an, form;
        struct abbrev x;
        if (!uleb(&p, end, &code))
            goto bad;
        if (!code)
            break;
        if (!uleb(&p, end, &tag) || p >= end)
            goto bad;
        memset(&x, 0, sizeof(x));
        x.code     = code;
        x.tag      = tag;
        x.children = *p++;
        while (1) {
            if (!uleb(&p, end, &an) || !uleb(&p, end, &form))
                goto bad;
            if (!an && !form)
                break;
            x.attrs = (struct attr *)realloc(x.attrs, (x.count + 1) * sizeof(*x.attrs));
            if (!x.attrs)
                goto bad;
            x.attrs[x.count].name   = an;
            x.attrs[x.count++].form = form;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            a   = (struct abbrev *)realloc(a, cap * sizeof(*a));
            if (!a) {
                free(x.attrs);
                goto bad;
            }
        }
        a[n++] = x;
    }
    *out   = a;
    *count = n;
    return 1;
bad:
    free_abbrevs(a, n);
    return 0;
}
static const struct abbrev *
find_abbrev(const struct abbrev *a, size_t n, uint64_t code)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (a[i].code == code)
            return &a[i];
    return NULL;
}
static int
form_value(const struct elf_parser *e,
           const uint8_t          **p,
           const uint8_t           *end,
           uint64_t                 form,
           uint8_t                  addr_size,
           uint32_t                 cu,
           uint64_t                *v,
           const char             **str,
           const uint8_t          **block,
           size_t                  *blen)
{
    const uint8_t *q;
    uint64_t       n = 0;
    int64_t        sn;
    *v     = 0;
    *str   = NULL;
    *block = NULL;
    *blen  = 0;
    if (form == FORM_INDIRECT) {
        if (!uleb(p, end, &form))
            return 0;
        return form_value(e, p, end, form, addr_size, cu, v, str, block, blen);
    }
    switch (form) {
        case FORM_ADDR:
            if (!take(p, end, addr_size, &q))
                return 0;
            *v = addr_size == 4 ? read_u32(q) : read_u64(q);
            return 1;
        case FORM_DATA1:
        case FORM_FLAG:
        case FORM_REF1:
            if (!take(p, end, 1, &q))
                return 0;
            *v = *q;
            break;
        case FORM_DATA2:
        case FORM_REF2:
            if (!take(p, end, 2, &q))
                return 0;
            *v = read_u16(q);
            break;
        case FORM_DATA4:
        case FORM_REF4:
        case FORM_SEC_OFFSET:
        case FORM_STRP:
        case FORM_REF_ADDR:
            if (!take(p, end, 4, &q))
                return 0;
            *v = read_u32(q);
            break;
        case FORM_DATA8:
        case FORM_REF8:
            if (!take(p, end, 8, &q))
                return 0;
            *v = read_u64(q);
            break;
        case FORM_UDATA:
        case FORM_REF_UDATA:
            if (!uleb(p, end, v))
                return 0;
            break;
        case FORM_SDATA:
            if (!sleb(p, end, &sn))
                return 0;
            *v = (uint64_t)sn;
            break;
        case FORM_STRING:
            *str = (const char *)*p;
            n    = strnlen(*str, (size_t)(end - *p));
            if (*p + n >= end)
                return 0;
            *p += n + 1;
            return 1;
        case FORM_FLAG_PRESENT:
            *v = 1;
            return 1;
        case FORM_BLOCK1:
            if (!take(p, end, 1, &q))
                return 0;
            n = *q;
            goto blk;
        case FORM_BLOCK2:
            if (!take(p, end, 2, &q))
                return 0;
            n = read_u16(q);
            goto blk;
        case FORM_BLOCK4:
            if (!take(p, end, 4, &q))
                return 0;
            n = read_u32(q);
            goto blk;
        case FORM_BLOCK:
        case FORM_EXPRLOC:
            if (!uleb(p, end, &n))
                return 0;
        blk:
            if (!take(p, end, (size_t)n, &q))
                return 0;
            *block = q;
            *blen  = (size_t)n;
            return 1;
        default:
            return 0;
    }
    if (form == FORM_STRP) {
        if (*v >= e->str.n)
            return 0;
        *str = (const char *)(e->str.p + *v);
    }
    if (form >= FORM_REF1 && form <= FORM_REF_UDATA && form != FORM_REF_ADDR)
        *v += cu;
    return 1;
}
static int
parse_info(struct elf_parser *e)
{
    const uint8_t *p = e->info.p, *all_end = p + e->info.n;
    while (p < all_end) {
        const uint8_t *h, *end;
        uint32_t       cu, aboff, len;
        uint16_t       ver;
        uint8_t        as;
        struct abbrev *a  = NULL;
        size_t         ac = 0;
        uint32_t       stack[128];
        unsigned       depth = 0;
        if (!take(&p, all_end, 4, &h))
            return 0;
        len = read_u32(h);
        if (!len)
            continue;
        if (len == 0xffffffffU || len > (uint32_t)(all_end - p))
            return 0;
        end = p + len;
        cu  = (uint32_t)(p - e->info.p - 4);
        if (!take(&p, end, 2, &h))
            return 0;
        ver = read_u16(h);
        if (ver < 2 || ver > 4)
            return 0;
        if (!take(&p, end, 4, &h))
            return 0;
        aboff = read_u32(h);
        if (!take(&p, end, 1, &h))
            return 0;
        as = *h;
        if (!load_abbrevs(&e->abbrev, aboff, &a, &ac))
            return 0;
        while (p < end) {
            uint32_t             off = (uint32_t)(p - e->info.p);
            uint64_t             code;
            const struct abbrev *ab;
            struct die           d;
            size_t               i;
            if (!uleb(&p, end, &code)) {
                free_abbrevs(a, ac);
                return 0;
            }
            if (!code) {
                if (depth)
                    depth--;
                continue;
            }
            ab = find_abbrev(a, ac, code);
            if (!ab) {
                free_abbrevs(a, ac);
                return 0;
            }
            memset(&d, 0, sizeof(d));
            d.offset = off;
            d.parent = depth ? stack[depth - 1] : UINT32_MAX;
            d.tag    = (uint32_t)ab->tag;
            for (i = 0; i < ab->count; i++) {
                uint64_t       v;
                const char    *s;
                const uint8_t *b;
                size_t         bn;
                if (!form_value(e, &p, end, ab->attrs[i].form, as, cu, &v, &s, &b, &bn)) {
                    free_abbrevs(a, ac);
                    return 0;
                }
                if (ab->attrs[i].name == AT_NAME)
                    d.name = s;
                else if (ab->attrs[i].name == AT_TYPE)
                    d.type = (uint32_t)v;
                else if (ab->attrs[i].name == AT_BYTE_SIZE)
                    d.byte_size = (uint32_t)v;
                else if (ab->attrs[i].name == AT_ENCODING)
                    d.encoding = (uint32_t)v;
                else if (ab->attrs[i].name == AT_LOCATION && b && bn == (size_t)as + 1U &&
                         b[0] == 3) {
                    d.address     = as == 4 ? read_u32(b + 1) : (uint32_t)read_u64(b + 1);
                    d.has_address = 1;
                } else if (ab->attrs[i].name == AT_DATA_MEMBER_LOCATION) {
                    if (b && bn) {
                        const uint8_t *bp = b, *be = b + bn;
                        uint64_t       x;
                        if ((*bp == 0x23 || *bp == 0x10) && ++bp && uleb(&bp, be, &x)) {
                            d.member_offset = (uint32_t)x;
                            d.has_member    = 1;
                        }
                    } else {
                        d.member_offset = (uint32_t)v;
                        d.has_member    = 1;
                    }
                }
            }
            if (!add_die(e, &d)) {
                free_abbrevs(a, ac);
                return 0;
            }
            if (ab->children) {
                if (depth >= 128) {
                    free_abbrevs(a, ac);
                    return 0;
                }
                stack[depth++] = off;
            }
        }
        free_abbrevs(a, ac);
        p = end;
    }
    return 1;
}
static int
sections(struct elf_parser *e)
{
    const uint8_t *f = e->file, *shstr;
    uint32_t       shoff;
    uint16_t       shents, n, si, i;
    const uint8_t *sh;
    if (e->size < 52 || memcmp(f, "\177ELF\1\1\1", 7))
        return 0;
    shoff  = read_u32(f + 32);
    shents = read_u16(f + 46);
    n      = read_u16(f + 48);
    si     = read_u16(f + 50);
    if (shents < 40 || (uint64_t)shoff + (uint64_t)n * shents > e->size || si >= n)
        return 0;
    sh = f + shoff + si * shents;
    if ((uint64_t)read_u32(sh + 16) + read_u32(sh + 20) > e->size)
        return 0;
    shstr = f + read_u32(sh + 16);
    for (i = 0; i < n; i++) {
        uint32_t    no, off, sz;
        const char *name;
        sh  = f + shoff + i * shents;
        no  = read_u32(sh);
        off = read_u32(sh + 16);
        sz  = read_u32(sh + 20);
        if ((uint64_t)off + sz > e->size)
            continue;
        name = (const char *)(shstr + no);
        if (!strcmp(name, ".debug_info")) {
            e->info.p = f + off;
            e->info.n = sz;
        } else if (!strcmp(name, ".debug_abbrev")) {
            e->abbrev.p = f + off;
            e->abbrev.n = sz;
        } else if (!strcmp(name, ".debug_str")) {
            e->str.p = f + off;
            e->str.n = sz;
        }
    }
    return e->info.p && e->abbrev.p && e->str.p;
}
int
elf_parser_open(struct elf_parser **out, const char *path)
{
    FILE              *fp;
    long               n;
    struct elf_parser *e;
    if (!out || !path)
        return 0;
    *out = NULL;
    fp   = fopen(path, "rb");
    if (!fp)
        return 0;
    if (fseek(fp, 0, SEEK_END) || (n = ftell(fp)) <= 0 || fseek(fp, 0, SEEK_SET)) {
        fclose(fp);
        return 0;
    }
    e = (struct elf_parser *)calloc(1, sizeof(*e));
    if (!e) {
        fclose(fp);
        return 0;
    }
    e->file = (uint8_t *)malloc((size_t)n);
    e->size = (size_t)n;
    if (!e->file || fread(e->file, 1, e->size, fp) != e->size) {
        fclose(fp);
        elf_parser_close(e);
        return 0;
    }
    fclose(fp);
    if (!sections(e) || !parse_info(e)) {
        elf_parser_close(e);
        return 0;
    }
    *out = e;
    return 1;
}
void
elf_parser_close(struct elf_parser *e)
{
    if (e) {
        free(e->dies);
        free(e->file);
        free(e);
    }
}

static const struct die *
die_at(const struct elf_parser *parser, uint32_t offset)
{
    size_t lo = 0, hi = parser->die_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (parser->dies[mid].offset < offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < parser->die_count && parser->dies[lo].offset == offset ? &parser->dies[lo] : NULL;
}

static const struct die *
unwrap_type(const struct elf_parser *parser, const struct die *die)
{
    while (die && (die->tag == TAG_TYPEDEF || die->tag == TAG_CONST || die->tag == TAG_VOLATILE ||
                   die->tag == TAG_RESTRICT || die->tag == TAG_ATOMIC))
        die = die_at(parser, die->type);
    return die;
}

int
elf_parser_resolve_info(const struct elf_parser *parser,
                        const char              *expression,
                        uint32_t                *address,
                        uint32_t                *size)
{
    char              path[512];
    char             *next;
    char             *member;
    const struct die *die = NULL;
    size_t            i;

    if (!parser || !expression || !address || !size || strlen(expression) >= sizeof(path))
        return 0;

    memcpy(path, expression, strlen(expression) + 1);
    next = strchr(path, '.');
    if (next)
        *next++ = '\0';

    for (i = 0; i < parser->die_count; ++i) {
        const struct die *candidate = &parser->dies[i];
        if (candidate->tag == TAG_VARIABLE && candidate->name && candidate->has_address &&
            strcmp(candidate->name, path) == 0) {
            die = candidate;
            break;
        }
    }
    if (!die)
        return 0;

    *address = die->address;
    while (next) {
        member = next;
        next   = strchr(next, '.');
        if (next)
            *next++ = '\0';

        die = unwrap_type(parser, die_at(parser, die->type));
        if (!die || (die->tag != TAG_STRUCT && die->tag != TAG_UNION && die->tag != TAG_CLASS))
            return 0;

        for (i = 0; i < parser->die_count; ++i) {
            const struct die *candidate = &parser->dies[i];
            if (candidate->tag == TAG_MEMBER && candidate->parent == die->offset &&
                candidate->name && strcmp(candidate->name, member) == 0)
                break;
        }
        if (i == parser->die_count)
            return 0;
        die       = &parser->dies[i];
        *address += die->member_offset;
    }
    die = unwrap_type(parser, die_at(parser, die->type));
    if (!die || die->byte_size == 0U)
        return 0;
    *size = die->byte_size;
    return 1;
}

int
elf_parser_resolve(const struct elf_parser *parser, const char *expression, uint32_t *address)
{
    uint32_t size;
    return elf_parser_resolve_info(parser, expression, address, &size);
}
struct format_buffer {
    char  *data;
    size_t capacity;
    size_t length;
};

static void
format_append(struct format_buffer *out, const char *fmt, ...)
{
    va_list args;
    int     written;
    if (out->length >= out->capacity)
        return;
    va_start(args, fmt);
    written = vsnprintf(out->data + out->length, out->capacity - out->length, fmt, args);
    va_end(args);
    if (written < 0)
        return;
    if ((size_t)written >= out->capacity - out->length)
        out->length = out->capacity;
    else
        out->length += (size_t)written;
}

static const struct die *
resolve_expression_type(const struct elf_parser *parser, const char *expression)
{
    char              path[512], *next, *member;
    const struct die *die = NULL;
    size_t            i;
    if (!parser || !expression || strlen(expression) >= sizeof(path))
        return NULL;
    memcpy(path, expression, strlen(expression) + 1);
    next = strchr(path, '.');
    if (next)
        *next++ = '\0';
    for (i = 0; i < parser->die_count; ++i) {
        if (parser->dies[i].tag == TAG_VARIABLE && parser->dies[i].name &&
            strcmp(parser->dies[i].name, path) == 0) {
            die = &parser->dies[i];
            break;
        }
    }
    while (die && next) {
        member = next;
        next   = strchr(next, '.');
        if (next)
            *next++ = '\0';
        die = unwrap_type(parser, die_at(parser, die->type));
        if (!die || (die->tag != TAG_STRUCT && die->tag != TAG_UNION && die->tag != TAG_CLASS))
            return NULL;
        for (i = 0; i < parser->die_count; ++i)
            if (parser->dies[i].tag == TAG_MEMBER && parser->dies[i].parent == die->offset &&
                parser->dies[i].name && strcmp(parser->dies[i].name, member) == 0)
                break;
        if (i == parser->die_count)
            return NULL;
        die = &parser->dies[i];
    }
    return die ? unwrap_type(parser, die_at(parser, die->type)) : NULL;
}

static void
format_hex(struct format_buffer *out, const uint8_t *data, uint32_t size)
{
    uint32_t i;
    format_append(out, "0x");
    for (i = size; i > 0; --i)
        format_append(out, "%02X", data[i - 1]);
}

static void
format_typed_value(const struct elf_parser *parser,
                   const struct die        *type,
                   const uint8_t           *data,
                   uint32_t                 available,
                   struct format_buffer    *out,
                   unsigned                 depth)
{
    size_t i;
    type = unwrap_type(parser, type);
    if (!type || depth > 16U || type->byte_size > available) {
        format_append(out, "<invalid>");
        return;
    }
    if (type->tag == TAG_STRUCT || type->tag == TAG_CLASS || type->tag == TAG_UNION) {
        int first = 1;
        format_append(out, "{");
        for (i = 0; i < parser->die_count; ++i) {
            const struct die *member = &parser->dies[i];
            const struct die *member_type;
            uint32_t          offset;
            if (member->tag != TAG_MEMBER || member->parent != type->offset || !member->name)
                continue;
            member_type = unwrap_type(parser, die_at(parser, member->type));
            offset      = type->tag == TAG_UNION ? 0U : member->member_offset;
            if (!member_type || offset > available || member_type->byte_size > available - offset)
                continue;
            format_append(out,
                          "%s%s:%s=",
                          first ? "" : ", ",
                          member->name,
                          member_type->name ? member_type->name : "?");
            format_typed_value(
                parser, member_type, data + offset, available - offset, out, depth + 1U);
            first = 0;
        }
        format_append(out, "}");
        return;
    }
    if (type->tag == TAG_BASE) {
        if (type->encoding == 4U && type->byte_size == 4U) {
            float value;
            memcpy(&value, data, sizeof(value));
            format_append(out, "%g", value);
            return;
        }
        if (type->encoding == 4U && type->byte_size == 8U) {
            double value;
            memcpy(&value, data, sizeof(value));
            format_append(out, "%g", value);
            return;
        }
        if ((type->encoding == 5U || type->encoding == 6U) && type->byte_size <= 8U) {
            int64_t value = 0;
            memcpy(&value, data, type->byte_size);
            if (type->byte_size < 8U && (data[type->byte_size - 1U] & 0x80U))
                value |= -(INT64_C(1) << (type->byte_size * 8U));
            format_append(out, "%lld", (long long)value);
            return;
        }
        if ((type->encoding == 2U || type->encoding == 7U || type->encoding == 8U) &&
            type->byte_size <= 8U) {
            uint64_t value = 0;
            memcpy(&value, data, type->byte_size);
            format_append(out, "%llu", (unsigned long long)value);
            return;
        }
    }
    if (type->tag == TAG_POINTER || type->tag == TAG_ENUM || type->tag == TAG_ARRAY) {
        format_hex(out, data, type->byte_size);
        return;
    }
    format_hex(out, data, type->byte_size);
}

int
elf_parser_format(const struct elf_parser *parser,
                  const char              *expression,
                  const void              *data,
                  uint32_t                 size,
                  char                    *output,
                  size_t                   output_capacity)
{
    const struct die    *type;
    struct format_buffer out;
    if (!parser || !expression || !data || !output || output_capacity == 0U)
        return 0;
    type = resolve_expression_type(parser, expression);
    if (!type || type->byte_size > size)
        return 0;
    out.data     = output;
    out.capacity = output_capacity;
    out.length   = 0U;
    output[0]    = '\0';
    format_typed_value(parser, type, (const uint8_t *)data, size, &out, 0U);
    output[output_capacity - 1U] = '\0';
    return out.length < out.capacity;
}

static int
parse_end_ok(const char *end)
{
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        ++end;
    return *end == '\0';
}

int
elf_parser_encode(const struct elf_parser *parser,
                  const char              *expression,
                  const char              *text,
                  void                    *output,
                  uint32_t                 output_size)
{
    const struct die *type;
    char             *end;
    if (!parser || !expression || !text || !output)
        return 0;
    type = unwrap_type(parser, resolve_expression_type(parser, expression));
    if (!type || type->byte_size == 0U || type->byte_size > output_size)
        return 0;
    memset(output, 0, type->byte_size);
    if (type->tag == TAG_BASE && type->encoding == 4U) {
        if (type->byte_size == 4U) {
            float value = strtof(text, &end);
            if (end == text || !parse_end_ok(end))
                return 0;
            memcpy(output, &value, sizeof(value));
            return 1;
        }
        if (type->byte_size == 8U) {
            double value = strtod(text, &end);
            if (end == text || !parse_end_ok(end))
                return 0;
            memcpy(output, &value, sizeof(value));
            return 1;
        }
    }
    if ((type->tag == TAG_BASE && (type->encoding == 5U || type->encoding == 6U)) ||
        type->tag == TAG_ENUM) {
        int64_t value = strtoll(text, &end, 0);
        if (end == text || !parse_end_ok(end) || type->byte_size > sizeof(value))
            return 0;
        memcpy(output, &value, type->byte_size);
        return 1;
    }
    if ((type->tag == TAG_BASE &&
         (type->encoding == 2U || type->encoding == 7U || type->encoding == 8U)) ||
        type->tag == TAG_POINTER) {
        uint64_t value = strtoull(text, &end, 0);
        if (end == text || !parse_end_ok(end) || type->byte_size > sizeof(value))
            return 0;
        memcpy(output, &value, type->byte_size);
        return 1;
    }
    return 0;
}
