#ifndef BITOPS_H
#define BITOPS_H

#include "macrodef.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BIT(n)           (1 << (n))
#define BIT_SET(v, n)    ((v) |= BIT(n))
#define BIT_CLEAR(v, n)  ((v) &= ~BIT(n))
#define BIT_TOGGLE(v, n) ((v) ^= BIT(n))
#define BIT_CHECK(v, n)  (((v) >> (n)) & 1)

#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(_BitScanForward, _BitScanReverse)
#pragma intrinsic(_BitScanForward64, _BitScanReverse64)
#pragma intrinsic(__popcnt, __popcnt64)
#pragma intrinsic(_rotl, _rotl64, _rotr, _rotr64)
#pragma intrinsic(_byteswap_ulong, _byteswap_uint64)
#ifdef __BMI__
#include <immintrin.h>
#endif

/* -------------------------------------------------------------------------- */
/*                                    64bit                                   */
/* -------------------------------------------------------------------------- */
HAPI uint32_t
clz64(const uint64_t x)
{
    unsigned long index;
    if (_BitScanReverse64(&index, x))
        return 63 - index;

    return 64;
}

HAPI uint32_t
ctz64(const uint64_t x)
{
#ifdef __BMI__
    return _tzcnt_u64(x); // BMI2 指令
#else
    unsigned long index;
    if (_BitScanForward64(&index, x))
        return index;

    return 64;
#endif
}

HAPI uint32_t
popcount64(const uint64_t x)
{
    return (uint32_t)__popcnt64(x);
}

HAPI uint64_t
bswap64(const uint64_t x)
{
    return _byteswap_uint64(x);
}

HAPI uint64_t
rol64(const uint64_t x, const uint32_t n)
{
    return _rotl64(x, n);
}

HAPI uint64_t
ror64(const uint64_t x, const uint32_t n)
{
    return _rotr64(x, n);
}

// BMI/BMI2 特殊操作
#ifdef __BMI__
HAPI uint64_t
blsr64(const uint64_t x)
{
    return _blsr_u64(x);
} // 清除最低位 1
HAPI uint64_t
blsi64(const uint64_t x)
{
    return _blsi_u64(x);
} // 提取最低位 1
#endif

/* -------------------------------------------------------------------------- */
/*                                    32bit                                   */
/* -------------------------------------------------------------------------- */
HAPI uint32_t
clz32(const uint32_t x)
{
    unsigned long index;
    if (_BitScanReverse(&index, x))
        return 31 - index;

    return 32;
}

HAPI uint32_t
ctz32(const uint32_t x)
{
#ifdef __BMI__
    return (uint32_t)_tzcnt_u32(x);
#else
    unsigned long index;
    if (_BitScanForward(&index, x))
        return index;

    return 32;
#endif
}

HAPI uint32_t
popcount32(const uint32_t x)
{
    return __popcnt(x);
}

HAPI uint32_t
bswap32(const uint32_t x)
{
    return _byteswap_ulong(x);
}

HAPI uint32_t
rol32(const uint32_t x, const uint32_t n)
{
    return _rotl(x, n);
}

HAPI uint32_t
ror32(const uint32_t x, const uint32_t n)
{
    return _rotr(x, n);
}

#elif defined(__GNUC__) || defined(__clang__)

/* -------------------------------------------------------------------------- */
/*                                    64bit                                   */
/* -------------------------------------------------------------------------- */
#ifdef __BMI__ // BMI/BMI2 支持
#include <x86intrin.h>
HAPI uint32_t
ctz64(const uint64_t x)
{
    return _tzcnt_u64(x);
}

HAPI uint64_t
blsr64(const uint64_t x)
{
    return _blsr_u64(x);
}

HAPI uint64_t
blsi64(const uint64_t x)
{
    return _blsi_u64(x);
}
#else
HAPI uint32_t
ctz64(const uint64_t x)
{
    return x ? __builtin_ctzll(x) : 64;
}
#endif

HAPI uint32_t
clz64(const uint64_t x)
{
    return x ? __builtin_clzll(x) : 64;
}

HAPI uint32_t
popcount64(const uint64_t x)
{
    return __builtin_popcountll(x);
}

HAPI uint64_t
bswap64(const uint64_t x)
{
    return __builtin_bswap64(x);
}

HAPI uint64_t
rol64(const uint64_t x, const uint32_t n)
{
    return (x << n) | (x >> (64 - n));
}

HAPI uint64_t
ror64(const uint64_t x, const uint32_t n)
{
    return (x >> n) | (x << (64 - n));
}

/* -------------------------------------------------------------------------- */
/*                                    32bit                                   */
/* -------------------------------------------------------------------------- */
#ifdef __BMI__
HAPI uint32_t
ctz32(uint32_t x)
{
    return _tzcnt_u32(x);
}
#else
HAPI uint32_t
ctz32(const uint32_t x)
{
    return x ? __builtin_ctz(x) : 32;
}
#endif

HAPI uint32_t
clz32(const uint32_t x)
{
    return x ? __builtin_clz(x) : 32;
}

HAPI uint32_t
popcount32(const uint32_t x)
{
    return __builtin_popcount(x);
}

HAPI uint32_t
bswap32(const uint32_t x)
{
    return __builtin_bswap32(x);
}

HAPI uint32_t
rol32(const uint32_t x, const uint32_t n)
{
    return (x << n) | (x >> (32 - n));
}

HAPI uint32_t
ror32(const uint32_t x, const uint32_t n)
{
    return (x >> n) | (x << (32 - n));
}

#else // 回退到循环实现

/* -------------------------------------------------------------------------- */
/*                                    64bit                                   */
/* -------------------------------------------------------------------------- */
HAPI uint32_t
clz64(uint64_t x)
{
    if (!x)
        return 64;
    uint32_t n = 0;
    for (int i = 63; i >= 0; i--)
        if ((x >> i) & 1)
            break;
        else
            n++;
    return n;
}

HAPI uint32_t
ctz64(uint64_t x)
{
    if (!x)
        return 64;
    uint32_t n = 0;
    for (int i = 0; i < 64; i++)
        if ((x >> i) & 1)
            break;
        else
            n++;
    return n;
}

HAPI uint32_t
popcount64(uint64_t x)
{
    uint32_t cnt = 0;
    while (x) {
        cnt  += x & 1;
        x   >>= 1;
    }
    return cnt;
}

HAPI uint64_t
bswap64(uint64_t x)
{
    uint64_t y = 0;
    for (uint32_t i = 0; i < 8; i++)
        y |= ((x >> (i * 8)) & 0xFF) << ((7 - i) * 8);
    return y;
}

HAPI uint64_t
rol64(uint64_t x, uint32_t n)
{
    return (x << n) | (x >> (64 - n));
}

HAPI uint64_t
ror64(uint64_t x, uint32_t n)
{
    return (x >> n) | (x << (64 - n));
}

/* -------------------------------------------------------------------------- */
/*                                    32bit                                   */
/* -------------------------------------------------------------------------- */
HAPI uint32_t
clz32(uint32_t x)
{
    if (!x)
        return 32;
    uint32_t n = 0;
    for (int i = 31; i >= 0; i--)
        if ((x >> i) & 1)
            break;
        else
            n++;
    return n;
}

HAPI uint32_t
ctz32(uint32_t x)
{
    if (!x)
        return 32;
    uint32_t n = 0;
    for (int i = 0; i < 32; i++)
        if ((x >> i) & 1)
            break;
        else
            n++;
    return n;
}

HAPI uint32_t
popcount32(uint32_t x)
{
    uint32_t cnt = 0;
    while (x) {
        cnt  += x & 1;
        x   >>= 1;
    }
    return cnt;
}

HAPI uint32_t
bswap32(uint32_t x)
{
    uint32_t y = 0;
    for (uint32_t i = 0; i < 4; i++)
        y |= ((x >> (i * 8)) & 0xFF) << ((3 - i) * 8);
    return y;
}

HAPI uint32_t
rol32(uint32_t x, uint32_t n)
{
    return (x << n) | (x >> (32 - n));
}

HAPI uint32_t
ror32(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32 - n));
}

#endif

/* -------------------------------------------------------------------------- */
/*                                  LSB / MSB                                 */
/* -------------------------------------------------------------------------- */
HAPI uint32_t
lsb64(const uint64_t x)
{
    return ctz64(x);
}

HAPI uint32_t
msb64(const uint64_t x)
{
    return 63 - clz64(x);
}

HAPI uint32_t
lsb32(const uint32_t x)
{
    return ctz32(x);
}

HAPI uint32_t
msb32(const uint32_t x)
{
    return 31 - clz32(x);
}

#ifdef __cplusplus
}
#endif

#endif // !BITOPS_H
