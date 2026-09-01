#include "cJSON.h"
#include "flash_array.hpp"
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace flasharray
{
void
save(cJSON *node, const std::shared_ptr<const Snapshot> &snapshot)
{
        if (!snapshot)
                return;
        const auto           &s        = *snapshot;
        static constexpr char digits[] = "0123456789ABCDEF";
        std::string           hex;
        hex.resize(s.bytes.size() * 2);
        for (size_t i = 0; i < s.bytes.size(); ++i) {
                hex[2 * i]     = digits[s.bytes[i] >> 4];
                hex[2 * i + 1] = digits[s.bytes[i] & 15];
        }
        auto *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", s.name.c_str());
        cJSON_AddNumberToObject(obj, "address", s.address);
        cJSON_AddNumberToObject(obj, "type", static_cast<int>(s.type));
        cJSON_AddBoolToObject(obj, "bigEndian", s.bigEndian);
        cJSON_AddNumberToObject(obj, "step", s.step);
        cJSON_AddStringToObject(obj, "bytesHex", hex.c_str());
        cJSON_AddItemToObject(node, "flashArray", obj);
}
std::shared_ptr<const Snapshot>
load(const cJSON *node)
{
        const auto *obj = cJSON_GetObjectItemCaseSensitive(node, "flashArray");
        if (!obj)
                return {};
        const auto *name    = cJSON_GetObjectItemCaseSensitive(obj, "name");
        const auto *address = cJSON_GetObjectItemCaseSensitive(obj, "address");
        const auto *type    = cJSON_GetObjectItemCaseSensitive(obj, "type");
        const auto *big     = cJSON_GetObjectItemCaseSensitive(obj, "bigEndian");
        const auto *step    = cJSON_GetObjectItemCaseSensitive(obj, "step");
        const auto *hex     = cJSON_GetObjectItemCaseSensitive(obj, "bytesHex");
        if (!cJSON_IsObject(obj) || !cJSON_IsString(name) || !cJSON_IsNumber(address) || address->valuedouble < 0 ||
            address->valuedouble > UINT32_MAX || std::floor(address->valuedouble) != address->valuedouble ||
            !cJSON_IsNumber(type) || type->valuedouble < 0 || type->valuedouble > 10 ||
            std::floor(type->valuedouble) != type->valuedouble || !cJSON_IsBool(big) || !cJSON_IsNumber(step) ||
            !cJSON_IsString(hex))
                throw std::invalid_argument("Invalid saved Flash array metadata.");
        const auto count = std::strlen(hex->valuestring);
        if (count == 0 || count % 2 || count > 32u * 1024u * 1024u)
                throw std::invalid_argument("Invalid saved Flash array length.");
        auto digit = [](char c) -> unsigned {
                if (c >= '0' && c <= '9')
                        return c - '0';
                if (c >= 'a' && c <= 'f')
                        return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                        return c - 'A' + 10;
                throw std::invalid_argument("Invalid saved Flash array byte.");
        };
        std::vector<uint8_t> bytes(count / 2);
        for (size_t i = 0; i < bytes.size(); ++i)
                bytes[i] = static_cast<uint8_t>((digit(hex->valuestring[2 * i]) << 4) | digit(hex->valuestring[2 * i + 1]));
        return decode(std::move(bytes),
                      static_cast<uint32_t>(address->valuedouble),
                      static_cast<Type>(type->valueint),
                      cJSON_IsTrue(big),
                      step->valuedouble,
                      name->valuestring);
}
} // namespace flasharray
