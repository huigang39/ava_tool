#ifndef BITOPS_H
#define BITOPS_H

#include "macrodef.h"
#include "typedef.h"

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
HAPI u32
clz64(const u64 x)
{
        unsigned long index;
        if (_BitScanReverse64(&index, x))
                return 63 - index;

        return 64;
}

HAPI u32
ctz64(const u64 x)
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

HAPI u32
popcount64(const u64 x)
{
        return (u32)__popcnt64(x);
}

HAPI u64
bswap64(const u64 x)
{
        return _byteswap_uint64(x);
}

HAPI u64
rol64(const u64 x, const u32 n)
{
        return _rotl64(x, n);
}

HAPI u64
ror64(const u64 x, const u32 n)
{
        return _rotr64(x, n);
}

// BMI/BMI2 特殊操作
#ifdef __BMI__
HAPI u64
blsr64(const u64 x)
{
        return _blsr_u64(x);
} // 清除最低位 1
HAPI u64
blsi64(const u64 x)
{
        return _blsi_u64(x);
} // 提取最低位 1
#endif

/* -------------------------------------------------------------------------- */
/*                                    32bit                                   */
/* -------------------------------------------------------------------------- */
HAPI u32
clz32(const u32 x)
{
        unsigned long index;
        if (_BitScanReverse(&index, x))
                return 31 - index;

        return 32;
}

HAPI u32
ctz32(const u32 x)
{
#ifdef __BMI__
        return (u32)_tzcnt_u32(x);
#else
        unsigned long index;
        if (_BitScanForward(&index, x))
                return index;

        return 32;
#endif
}

HAPI u32
popcount32(const u32 x)
{
        return __popcnt(x);
}

HAPI u32
bswap32(const u32 x)
{
        return _byteswap_ulong(x);
}

HAPI u32
rol32(const u32 x, const u32 n)
{
        return _rotl(x, n);
}

HAPI u32
ror32(const u32 x, const u32 n)
{
        return _rotr(x, n);
}

#elif defined(__GNUC__) || defined(__clang__)

/* -------------------------------------------------------------------------- */
/*                                    64bit                                   */
/* -------------------------------------------------------------------------- */
#ifdef __BMI__ // BMI/BMI2 支持
#include <x86intrin.h>
HAPI u32
ctz64(const u64 x)
{
        return _tzcnt_u64(x);
}

HAPI u64
blsr64(const u64 x)
{
        return _blsr_u64(x);
}

HAPI u64
blsi64(const u64 x)
{
        return _blsi_u64(x);
}
#else
HAPI u32
ctz64(const u64 x)
{
        return x ? __builtin_ctzll(x) : 64;
}
#endif

HAPI u32
clz64(const u64 x)
{
        return x ? __builtin_clzll(x) : 64;
}

HAPI u32
popcount64(const u64 x)
{
        return __builtin_popcountll(x);
}

HAPI u64
bswap64(const u64 x)
{
        return __builtin_bswap64(x);
}

HAPI u64
rol64(const u64 x, const u32 n)
{
        return (x << n) | (x >> (64 - n));
}

HAPI u64
ror64(const u64 x, const u32 n)
{
        return (x >> n) | (x << (64 - n));
}

/* -------------------------------------------------------------------------- */
/*                                    32bit                                   */
/* -------------------------------------------------------------------------- */
#ifdef __BMI__
HAPI u32
ctz32(u32 x)
{
        return _tzcnt_u32(x);
}
#else
HAPI u32
ctz32(const u32 x)
{
        return x ? __builtin_ctz(x) : 32;
}
#endif

HAPI u32
clz32(const u32 x)
{
        return x ? __builtin_clz(x) : 32;
}

HAPI u32
popcount32(const u32 x)
{
        return __builtin_popcount(x);
}

HAPI u32
bswap32(const u32 x)
{
        return __builtin_bswap32(x);
}

HAPI u32
rol32(const u32 x, const u32 n)
{
        return (x << n) | (x >> (32 - n));
}

HAPI u32
ror32(const u32 x, const u32 n)
{
        return (x >> n) | (x << (32 - n));
}

#else // fallback 循环实现

/* -------------------------------------------------------------------------- */
/*                                    64bit                                   */
/* -------------------------------------------------------------------------- */
HAPI u32
clz64(u64 x)
{
        if (!x)
                return 64;
        u32 n = 0;
        for (int i = 63; i >= 0; i--)
                if ((x >> i) & 1)
                        break;
                else
                        n++;
        return n;
}

HAPI u32
ctz64(u64 x)
{
        if (!x)
                return 64;
        u32 n = 0;
        for (int i = 0; i < 64; i++)
                if ((x >> i) & 1)
                        break;
                else
                        n++;
        return n;
}

HAPI u32
popcount64(u64 x)
{
        u32 cnt = 0;
        while (x) {
                cnt  += x & 1;
                x   >>= 1;
        }
        return cnt;
}

HAPI u64
bswap64(u64 x)
{
        u64 y = 0;
        for (u32 i = 0; i < 8; i++)
                y |= ((x >> (i * 8)) & 0xFF) << ((7 - i) * 8);
        return y;
}

HAPI u64
rol64(u64 x, u32 n)
{
        return (x << n) | (x >> (64 - n));
}

HAPI u64
ror64(u64 x, u32 n)
{
        return (x >> n) | (x << (64 - n));
}

/* -------------------------------------------------------------------------- */
/*                                    32bit                                   */
/* -------------------------------------------------------------------------- */
HAPI u32
clz32(u32 x)
{
        if (!x)
                return 32;
        u32 n = 0;
        for (int i = 31; i >= 0; i--)
                if ((x >> i) & 1)
                        break;
                else
                        n++;
        return n;
}

HAPI u32
ctz32(u32 x)
{
        if (!x)
                return 32;
        u32 n = 0;
        for (int i = 0; i < 32; i++)
                if ((x >> i) & 1)
                        break;
                else
                        n++;
        return n;
}

HAPI u32
popcount32(u32 x)
{
        u32 cnt = 0;
        while (x) {
                cnt  += x & 1;
                x   >>= 1;
        }
        return cnt;
}

HAPI u32
bswap32(u32 x)
{
        u32 y = 0;
        for (u32 i = 0; i < 4; i++)
                y |= ((x >> (i * 8)) & 0xFF) << ((3 - i) * 8);
        return y;
}

HAPI u32
rol32(u32 x, u32 n)
{
        return (x << n) | (x >> (32 - n));
}

HAPI u32
ror32(u32 x, u32 n)
{
        return (x >> n) | (x << (32 - n));
}

#endif

/* -------------------------------------------------------------------------- */
/*                                  LSB / MSB                                 */
/* -------------------------------------------------------------------------- */
HAPI u32
lsb64(const u64 x)
{
        return ctz64(x);
}

HAPI u32
msb64(const u64 x)
{
        return 63 - clz64(x);
}

HAPI u32
lsb32(const u32 x)
{
        return ctz32(x);
}

HAPI u32
msb32(const u32 x)
{
        return 31 - clz32(x);
}

#ifdef __cplusplus
}
#endif

#endif // !BITOPS_H
