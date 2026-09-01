#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
struct cJSON;
namespace flasharray
{
enum class Type { F16, F32, F64, I8, U8, I16, U16, I32, U32, I64, U64 };
inline constexpr const char *names[] = {
    "float16", "float32", "float64", "int8", "uint8", "int16", "uint16", "int32", "uint32", "int64", "uint64"};
size_t      width(Type type);
uint64_t    bits(const uint8_t *data, size_t size, bool bigEndian);
double      val(const uint8_t *data, Type type, bool bigEndian);
std::string format(const uint8_t *data, Type type, bool bigEndian);
struct Snapshot {
        std::string          name;
        uint32_t             address{};
        Type                 type{Type::F32};
        bool                 bigEndian{};
        double               step{1.0};
        std::vector<uint8_t> bytes;
        std::vector<double>  values;
};
std::shared_ptr<const Snapshot>
     decode(std::vector<uint8_t> bytes, uint32_t address, Type type, bool bigEndian, double step, std::string name);
void save(cJSON *node, const std::shared_ptr<const Snapshot> &snapshot);
std::shared_ptr<const Snapshot> load(const cJSON *node);
// Shared ownership keeps a drag snapshot valid even when its source is re-read
struct Drag {
        uint64_t                        id{};
        std::shared_ptr<const Snapshot> snapshot;
};
inline Drag &
drag()
{
        static Drag state;
        return state;
}
} // namespace flasharray
