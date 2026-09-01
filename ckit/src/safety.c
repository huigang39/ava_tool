#include "safety.h"
#include <string.h>

/**
 * @brief 生成指定有效位宽的掩码, 调用前须确保 width 在 1..64
 */
static uint64_t width_mask(unsigned width)
{
    return width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
}

/**
 * @brief 判断 ASCII 空白字符, 不依赖当前语言环境
 */
static int space_char(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' ||
           c == '\f';
}

int safety_parse_integer(const char *text,
                         unsigned width,
                         int allow_negative,
                         uint64_t *out)
{
    uint64_t val = 0, limit;
    unsigned base = 10;
    int negative = 0, digits = 0;
    if (out) {
        *out = 0;
    }
    if (!text || !out || width < 1 || width > 64) {
        return SAFETY_INVALID;
    }
    while (space_char(*text)) {
        ++text;
    }
    if (*text == '-') {
        if (!allow_negative) {
            return SAFETY_INVALID;
        }
        negative = 1;
        ++text;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
    }
    limit = negative ? UINT64_C(1) << (width - 1) : width_mask(width);
    while (*text && !space_char(*text)) {
        unsigned digit;
        if (*text >= '0' && *text <= '9') {
            digit = (unsigned)(*text - '0');
        } else if (*text >= 'a' && *text <= 'f') {
            digit = (unsigned)(*text - 'a' + 10);
        } else if (*text >= 'A' && *text <= 'F') {
            digit = (unsigned)(*text - 'A' + 10);
        } else {
            return SAFETY_INVALID;
        }
        if (digit >= base || digit > limit || val > (limit - digit) / base) {
            return SAFETY_INVALID;
        }
        val = val * base + digit;
        digits = 1;
        ++text;
    }
    while (space_char(*text)) {
        ++text;
    }
    if (!digits || *text) {
        return SAFETY_INVALID;
    }
    *out = negative ? (UINT64_C(0) - val) & width_mask(width) : val;
    return SAFETY_OK;
}

int safety_validate(const struct safety_decoder *decoder)
{
    size_t i, j;
    uint64_t mask;
    if (!decoder || decoder->width < 1 || decoder->width > 64 ||
        decoder->type < SAFETY_AUTO || decoder->type > SAFETY_FLOATING ||
        decoder->kind < SAFETY_NUMBER || decoder->kind > SAFETY_ENUM ||
        (decoder->count && !decoder->entries)) {
        return SAFETY_INVALID;
    }
    if (decoder->kind == SAFETY_NUMBER && decoder->count) {
        return SAFETY_INVALID;
    }
    if (decoder->type == SAFETY_FLOATING &&
        (decoder->kind != SAFETY_NUMBER ||
         (decoder->width != 16 && decoder->width != 32 &&
          decoder->width != 64))) {
        return SAFETY_INVALID;
    }
    if (decoder->type == SAFETY_BOOLEAN && decoder->width != 1) {
        return SAFETY_INVALID;
    }
    mask = width_mask(decoder->width);
    for (i = 0; i < decoder->count; ++i) {
        const uint64_t val = decoder->entries[i].val;
        if ((val & ~mask) || (decoder->kind == SAFETY_BITS && !val)) {
            return SAFETY_INVALID;
        }
        for (j = 0; j < i; ++j) {
            if (decoder->entries[j].val == val) {
                return SAFETY_INVALID;
            }
        }
    }
    return SAFETY_OK;
}

/**
 * @brief 检查采样类型,位宽及原始位范围
 */
static int value_valid(const struct safety_value *val)
{
    if (!val || val->width < 1 || val->width > 64 ||
        val->type < SAFETY_UNSIGNED || val->type > SAFETY_FLOATING) {
        return 0;
    }
    if (val->type == SAFETY_FLOATING && val->width != 16 && val->width != 32 &&
        val->width != 64) {
        return 0;
    }
    if (val->bits & ~width_mask(val->width)) {
        return 0;
    }
    if (val->type == SAFETY_BOOLEAN && val->bits > 1) {
        return 0;
    }
    return 1;
}

int safety_decode(const struct safety_decoder *decoder,
                  const struct safety_value *val,
                  size_t *indices,
                  size_t capacity,
                  struct safety_result *result)
{
    uint64_t bits, known = 0;
    size_t i;
    if (result) {
        memset(result, 0, sizeof(*result));
    }
    if (!result || (capacity && !indices) ||
        safety_validate(decoder) != SAFETY_OK || !value_valid(val)) {
        return SAFETY_INVALID;
    }
    if (decoder->kind == SAFETY_NUMBER) {
        if (decoder->type != SAFETY_AUTO &&
            (decoder->type != val->type || (decoder->type != SAFETY_BOOLEAN &&
                                            decoder->width != val->width))) {
            return SAFETY_MISMATCH;
        }
        return SAFETY_OK;
    }
    if (val->type == SAFETY_FLOATING ||
        (val->type != SAFETY_BOOLEAN && val->width > decoder->width)) {
        return SAFETY_MISMATCH;
    }
    bits = val->bits;
    // 位图保留原始有效位; 仅枚举将负整数扩展到定义位宽
    if (decoder->kind == SAFETY_ENUM && val->type == SAFETY_SIGNED &&
        (bits & (UINT64_C(1) << (val->width - 1)))) {
        bits |= ~width_mask(val->width);
    }
    bits &= width_mask(decoder->width);
    for (i = 0; i < decoder->count; ++i) {
        const uint64_t expected = decoder->entries[i].val;
        const int hit = decoder->kind == SAFETY_BITS
                            ? (bits & expected) == expected
                            : bits == expected;
        if (!hit) {
            continue;
        }
        if (result->matched < capacity) {
            indices[result->matched] = i;
        }
        ++result->matched;
        known |= expected;
    }
    if (decoder->kind == SAFETY_BITS) {
        result->unknown_mask = bits & ~known;
    } else {
        result->unknown_value = result->matched == 0;
    }
    return result->matched > capacity ? SAFETY_CAPACITY : SAFETY_OK;
}

int safety_matches(const struct safety_value *val,
                   uint64_t expected,
                   uint64_t mask)
{
    if (!value_valid(val) || val->type == SAFETY_FLOATING || !mask ||
        (mask & ~width_mask(val->width)) || (expected & ~mask)) {
        return SAFETY_INVALID;
    }
    return (val->bits & mask) == expected;
}
