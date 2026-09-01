/**
 * @file  type_codec.hpp
 * @brief Encode / decode raw memory bytes to / from f32 based on type strings.
 *
 * Provides two core helpers used by the sampler thread and the variable window:
 *   - decodeAs()     : raw bytes  → f32
 *   - encodeFromF32() : f32       → raw bytes
 *   - typeBytes()     : type name → byte width
 *
 * Type strings are lowercase ("f32", "i16", …). Files written by older versions
 * carry uppercase spellings, so anything read from disk goes through
 * normalizeTypeName() before it reaches the comparisons below.
 */
#ifndef TYPE_CODEC_HPP
#define TYPE_CODEC_HPP

#include "ckit.h"
#include <cctype>
#include <cstring>
#include <string>

/**
 * @brief Lowercase a type token. Keeps legacy on-disk spellings ("U32") meaningful
 *        without rewriting every place a token enters the program
 */
inline std::string
normalizeTypeName(const std::string &t)
{
        std::string out = t;
        for (char &c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
}

/**
 * @brief Return the byte width for a type string ("f32", "i16", …). Defaults to 4
 */
inline u32
typeBytes(const std::string &t)
{
        if (t == "f64" || t == "i64" || t == "u64")
                return 8;
        if (t == "f16" || t == "i16" || t == "u16")
                return 2;
        if (t == "i8" || t == "u8")
                return 1;
        return 4; // f32 / i32 / u32 / empty / unknown
}

/**
 * @brief Decode raw bytes into an f32 value according to the type string
 */
inline f32
decodeAs(const u8 *raw, const std::string &type, u32 bitOffset = 0, u32 bitSize = 0)
{
        if (type == "u8") {
                u8 v;
                std::memcpy(&v, raw, 1);
                if (bitSize > 0)
                        v = (v >> bitOffset) & ((1 << bitSize) - 1);
                return static_cast<f32>(v);
        }
        if (type == "i8") {
                u8 v;
                std::memcpy(&v, raw, 1);
                if (bitSize > 0) {
                        v = (v >> bitOffset) & ((1 << bitSize) - 1);
                        if (v & (1 << (bitSize - 1)))
                                v |= static_cast<u8>(~((1 << bitSize) - 1));
                        return static_cast<f32>(static_cast<i8>(v));
                }
                i8 sv;
                std::memcpy(&sv, raw, 1);
                return static_cast<f32>(sv);
        }
        if (type == "u16") {
                u16 v;
                std::memcpy(&v, raw, 2);
                if (bitSize > 0)
                        v = (v >> bitOffset) & ((1 << bitSize) - 1);
                return static_cast<f32>(v);
        }
        if (type == "i16") {
                u16 v;
                std::memcpy(&v, raw, 2);
                if (bitSize > 0) {
                        v = (v >> bitOffset) & ((1 << bitSize) - 1);
                        if (v & (1 << (bitSize - 1)))
                                v |= static_cast<u16>(~((1 << bitSize) - 1));
                        return static_cast<f32>(static_cast<i16>(v));
                }
                i16 sv;
                std::memcpy(&sv, raw, 2);
                return static_cast<f32>(sv);
        }
        if (type == "f16") {
                u16 bits;
                std::memcpy(&bits, raw, 2);
                return fast_f16_to_f32(bits);
        }
        if (type == "i32") {
                u32 v;
                std::memcpy(&v, raw, 4);
                if (bitSize > 0) {
                        v = (v >> bitOffset) & ((1ULL << bitSize) - 1);
                        if (v & (1ULL << (bitSize - 1)))
                                v |= static_cast<u32>(~((1ULL << bitSize) - 1));
                        return static_cast<f32>(static_cast<i32>(v));
                }
                i32 sv;
                std::memcpy(&sv, raw, 4);
                return static_cast<f32>(sv);
        }
        if (type == "f32") {
                f32 f;
                std::memcpy(&f, raw, 4);
                return f;
        }
        if (type == "u64") {
                u64 v;
                std::memcpy(&v, raw, 8);
                if (bitSize > 0)
                        v = (v >> bitOffset) & ((1ULL << bitSize) - 1);
                return static_cast<f32>(v);
        }
        if (type == "i64") {
                u64 v;
                std::memcpy(&v, raw, 8);
                if (bitSize > 0) {
                        v = (v >> bitOffset) & ((1ULL << bitSize) - 1);
                        if (v & (1ULL << (bitSize - 1)))
                                v |= ~((1ULL << bitSize) - 1);
                        return static_cast<f32>(static_cast<i64>(v));
                }
                i64 sv;
                std::memcpy(&sv, raw, 8);
                return static_cast<f32>(sv);
        }
        if (type == "f64") {
                f64 d;
                std::memcpy(&d, raw, 8);
                return static_cast<f32>(d);
        }
        // Default / u32
        u32 v;
        std::memcpy(&v, raw, 4);
        if (bitSize > 0)
                v = (v >> bitOffset) & ((1ULL << bitSize) - 1);
        return static_cast<f32>(v);
}

/**
 * @brief Encode an f32 value back into raw bytes suitable for writing to target memory
 */
inline void
encodeFromF32(const f32 val, const std::string &type, u8 *out)
{
        if (type == "f16") {
                const u16 bits = fast_f32_to_f16(val);
                std::memcpy(out, &bits, 2);
                return;
        }
        if (type == "f32") {
                std::memcpy(out, &val, 4);
                return;
        }
        if (type == "f64") {
                const f64 d = static_cast<f64>(val);
                std::memcpy(out, &d, 8);
                return;
        }
        if (type == "i8") {
                const i8 v = static_cast<i8>(val);
                std::memcpy(out, &v, 1);
                return;
        }
        if (type == "i16") {
                const i16 v = static_cast<i16>(val);
                std::memcpy(out, &v, 2);
                return;
        }
        if (type == "i32") {
                const i32 v = static_cast<i32>(val);
                std::memcpy(out, &v, 4);
                return;
        }
        if (type == "i64") {
                const i64 v = static_cast<i64>(val);
                std::memcpy(out, &v, 8);
                return;
        }
        if (type == "u8") {
                const u8 v = static_cast<u8>(val);
                std::memcpy(out, &v, 1);
                return;
        }
        if (type == "u16") {
                const u16 v = static_cast<u16>(val);
                std::memcpy(out, &v, 2);
                return;
        }
        if (type == "u64") {
                const u64 v = static_cast<u64>(val);
                std::memcpy(out, &v, 8);
                return;
        }
        // Default / u32
        const u32 v = static_cast<u32>(val);
        std::memcpy(out, &v, 4);
}

#endif // !TYPE_CODEC_HPP
