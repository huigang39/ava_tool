#include "flash_array.hpp"
#include "../../ckit/include/fastmath.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
namespace flasharray
{
size_t
width(Type type)
{
        static constexpr size_t sizes[] = {2, 4, 8, 1, 1, 2, 2, 4, 4, 8, 8};
        const auto              i       = static_cast<unsigned>(type);
        if (i >= std::size(sizes))
                throw std::invalid_argument("Invalid array type");
        return sizes[i];
}
uint64_t
bits(const uint8_t *p, size_t n, bool big)
{
        uint64_t out = 0;
        for (size_t i = 0; i < n; ++i)
                out |= uint64_t(p[i]) << (8 * (big ? n - 1 - i : i));
        return out;
}
static int64_t
signedBits(uint64_t b, size_t n)
{
        if (n < 8 && (b & (uint64_t(1) << (n * 8 - 1))))
                b |= ~((uint64_t(1) << (n * 8)) - 1);
        return std::bit_cast<int64_t>(b);
}
double
val(const uint8_t *p, Type t, bool big)
{
        const auto n = width(t);
        const auto b = bits(p, n, big);
        switch (t) {
                case Type::F16:
                        return fast_f16_to_f32(static_cast<uint16_t>(b));
                case Type::F32:
                        return std::bit_cast<float>(uint32_t(b));
                case Type::F64:
                        return std::bit_cast<double>(b);
                case Type::I8:
                case Type::I16:
                case Type::I32:
                case Type::I64:
                        return double(signedBits(b, n));
                default:
                        return double(b);
        }
}
std::string
format(const uint8_t *p, Type t, bool big)
{
        const auto n = width(t);
        const auto b = bits(p, n, big);
        if (t == Type::I8 || t == Type::I16 || t == Type::I32 || t == Type::I64)
                return std::to_string(signedBits(b, n));
        if (t != Type::F16 && t != Type::F32 && t != Type::F64)
                return std::to_string(b);
        char text[64];
        std::snprintf(text, sizeof(text), "%.17g", val(p, t, big));
        return text;
}
std::shared_ptr<const Snapshot>
decode(std::vector<uint8_t> bytes, uint32_t address, Type type, bool big, double step, std::string name)
{
        const auto n = width(type);
        if (bytes.empty() || bytes.size() % n)
                throw std::invalid_argument("Byte length must be a nonzero multiple of element size.");
        if (!std::isfinite(step) || step <= 0 || !std::isfinite(step * double(bytes.size() / n)))
                throw std::invalid_argument("Sample interval must be finite and positive.");
        if (uint64_t(address) + bytes.size() > uint64_t(0x100000000ULL))
                throw std::invalid_argument("Address range exceeds 32 bits.");
        auto out       = std::make_shared<Snapshot>();
        out->name      = std::move(name);
        out->address   = address;
        out->type      = type;
        out->bigEndian = big;
        out->step      = step;
        out->bytes     = std::move(bytes);
        out->values.reserve(out->bytes.size() / n);
        for (size_t i = 0; i < out->bytes.size(); i += n)
                out->values.push_back(val(out->bytes.data() + i, type, big));
        return out;
}
} // namespace flasharray
