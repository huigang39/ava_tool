/**
 * @file  type_codec.hpp
 * @brief Encode / decode raw memory bytes to / from f32 based on type strings.
 *
 * Provides two core helpers used by the sampler thread and the variable window:
 *   - decodeAs()     : raw bytes  → f32
 *   - encodeFromF32() : f32       → raw bytes
 *   - typeBytes()     : type name → byte width
 */
#ifndef TYPE_CODEC_HPP
#define TYPE_CODEC_HPP

#include "module.h"
#include <cstring>
#include <string>

// Return the byte width for a type string ("F32", "I16", …). Defaults to 4.
inline u32
typeBytes(const std::string &t)
{
        if (t == "F64" || t == "I64" || t == "U64")
                return 8;
        if (t == "I16" || t == "U16")
                return 2;
        if (t == "I8" || t == "U8")
                return 1;
        return 4; // F32 / I32 / U32 / empty / unknown
}

// Decode raw bytes into an f32 value according to the type string.
inline f32
decodeAs(const u8 *raw, const std::string &type, u32 bitOffset = 0, u32 bitSize = 0)
{
        if (type == "U8") {
                u8 v;
                std::memcpy(&v, raw, 1);
                if (bitSize > 0)
                        v = (v >> bitOffset) & ((1 << bitSize) - 1);
                return static_cast<f32>(v);
        }
        if (type == "I8") {
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
        if (type == "U16") {
                u16 v;
                std::memcpy(&v, raw, 2);
                if (bitSize > 0)
                        v = (v >> bitOffset) & ((1 << bitSize) - 1);
                return static_cast<f32>(v);
        }
        if (type == "I16") {
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
        if (type == "I32") {
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
        if (type == "F32") {
                f32 f;
                std::memcpy(&f, raw, 4);
                return f;
        }
        if (type == "U64") {
                u64 v;
                std::memcpy(&v, raw, 8);
                if (bitSize > 0)
                        v = (v >> bitOffset) & ((1ULL << bitSize) - 1);
                return static_cast<f32>(v);
        }
        if (type == "I64") {
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
        if (type == "F64") {
                f64 d;
                std::memcpy(&d, raw, 8);
                return static_cast<f32>(d);
        }
        // Default / U32
        u32 v;
        std::memcpy(&v, raw, 4);
        if (bitSize > 0)
                v = (v >> bitOffset) & ((1ULL << bitSize) - 1);
        return static_cast<f32>(v);
}

// Encode an f32 value back into raw bytes suitable for writing to target memory.
inline void
encodeFromF32(const f32 val, const std::string &type, u8 *out)
{
        if (type == "F32") {
                std::memcpy(out, &val, 4);
                return;
        }
        if (type == "F64") {
                const f64 d = static_cast<f64>(val);
                std::memcpy(out, &d, 8);
                return;
        }
        if (type == "I8") {
                const i8 v = static_cast<i8>(val);
                std::memcpy(out, &v, 1);
                return;
        }
        if (type == "I16") {
                const i16 v = static_cast<i16>(val);
                std::memcpy(out, &v, 2);
                return;
        }
        if (type == "I32") {
                const i32 v = static_cast<i32>(val);
                std::memcpy(out, &v, 4);
                return;
        }
        if (type == "I64") {
                const i64 v = static_cast<i64>(val);
                std::memcpy(out, &v, 8);
                return;
        }
        if (type == "U8") {
                const u8 v = static_cast<u8>(val);
                std::memcpy(out, &v, 1);
                return;
        }
        if (type == "U16") {
                const u16 v = static_cast<u16>(val);
                std::memcpy(out, &v, 2);
                return;
        }
        if (type == "U64") {
                const u64 v = static_cast<u64>(val);
                std::memcpy(out, &v, 8);
                return;
        }
        // Default / U32
        const u32 v = static_cast<u32>(val);
        std::memcpy(out, &v, 4);
}

#endif // !TYPE_CODEC_HPP
