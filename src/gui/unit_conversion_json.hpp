#ifndef AVA_UNIT_CONVERSION_JSON_HPP
#define AVA_UNIT_CONVERSION_JSON_HPP

#include "cJSON.h"
#include "core/unit_conversion.hpp"
#include <cstring>
#include <initializer_list>

namespace unit_conversion
{
namespace detail
{
inline constexpr const char *
QuantityId(Quantity quantity)
{
        switch (quantity) {
                case Quantity::AngularPosition:
                        return "angular_position";
                case Quantity::AngularSpeed:
                        return "angular_speed";
                case Quantity::LinearPosition:
                        return "linear_position";
                case Quantity::LinearSpeed:
                        return "linear_speed";
        }
        return nullptr;
}

inline constexpr const char *
UnitId(Unit unit)
{
        switch (unit) {
                case Unit::Radian:
                        return "radian";
                case Unit::Degree:
                        return "degree";
                case Unit::Revolution:
                        return "revolution";
                case Unit::Count:
                        return "count";
                case Unit::RadPerSecond:
                        return "rad_per_second";
                case Unit::DegPerSecond:
                        return "deg_per_second";
                case Unit::Rpm:
                        return "rpm";
                case Unit::RevPerSecond:
                        return "rev_per_second";
                case Unit::CountPerSecond:
                        return "count_per_second";
                case Unit::Millimeter:
                        return "millimeter";
                case Unit::Centimeter:
                        return "centimeter";
                case Unit::Meter:
                        return "meter";
                case Unit::Micrometer:
                        return "micrometer";
                case Unit::Inch:
                        return "inch";
                case Unit::MillimeterPerSecond:
                        return "millimeter_per_second";
                case Unit::CentimeterPerSecond:
                        return "centimeter_per_second";
                case Unit::MeterPerSecond:
                        return "meter_per_second";
                case Unit::MillimeterPerMinute:
                        return "millimeter_per_minute";
                case Unit::MeterPerMinute:
                        return "meter_per_minute";
        }
        return nullptr;
}

inline bool
SameId(const cJSON *item, const char *id)
{
        return id && cJSON_IsString(item) && item->valuestring && std::strcmp(item->valuestring, id) == 0;
}
} // namespace detail

inline void
SaveSettings(cJSON *parent, const char *key, const Settings &settings)
{
        if (!cJSON_IsObject(parent) || !key || !*key)
                return;
        // Never serialize corrupt enum values or an invalid enabled config.
        const Settings safe   = Convert(0, settings) ? settings : Settings{};
        cJSON         *object = cJSON_CreateObject();
        if (!object)
                return;
        cJSON_AddBoolToObject(object, "enabled", safe.enabled);
        cJSON_AddStringToObject(object, "quantity", detail::QuantityId(safe.quantity));
        cJSON_AddStringToObject(object, "source", detail::UnitId(safe.source));
        cJSON_AddStringToObject(object, "target", detail::UnitId(safe.target));
        cJSON_AddNumberToObject(
            object,
            "countsPerRevolution",
            std::isfinite(safe.countsPerRevolution) && safe.countsPerRevolution > 0 ? safe.countsPerRevolution : 0);
        if (cJSON_GetObjectItemCaseSensitive(parent, key)) {
                if (!cJSON_ReplaceItemInObjectCaseSensitive(parent, key, object))
                        cJSON_Delete(object);
        } else if (!cJSON_AddItemToObject(parent, key, object)) {
                cJSON_Delete(object);
        }
}

inline Settings
LoadSettings(const cJSON *parent, const char *key)
{
        if (!cJSON_IsObject(parent) || !key || !*key)
                return {};
        const cJSON *object = cJSON_GetObjectItemCaseSensitive(parent, key);
        if (!cJSON_IsObject(object))
                return {};
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(object, "enabled");
        if (!cJSON_IsBool(enabled))
                return {};
        Settings settings;
        settings.enabled  = cJSON_IsTrue(enabled);
        settings.quantity = static_cast<Quantity>(-1);
        settings.source = settings.target = static_cast<Unit>(-1);
        const auto *quantity              = cJSON_GetObjectItemCaseSensitive(object, "quantity");
        const auto *source                = cJSON_GetObjectItemCaseSensitive(object, "source");
        const auto *target                = cJSON_GetObjectItemCaseSensitive(object, "target");
        for (auto candidate :
             {Quantity::AngularPosition, Quantity::AngularSpeed, Quantity::LinearPosition, Quantity::LinearSpeed})
                if (detail::SameId(quantity, detail::QuantityId(candidate)))
                        settings.quantity = candidate;
        for (int i = static_cast<int>(Unit::Radian); i <= static_cast<int>(Unit::MeterPerMinute); ++i) {
                const auto candidate = static_cast<Unit>(i);
                if (detail::SameId(source, detail::UnitId(candidate)))
                        settings.source = candidate;
                if (detail::SameId(target, detail::UnitId(candidate)))
                        settings.target = candidate;
        }
        const auto *counts = cJSON_GetObjectItemCaseSensitive(object, "countsPerRevolution");
        if (counts) {
                if (!cJSON_IsNumber(counts) || !std::isfinite(counts->valuedouble))
                        return {};
                settings.countsPerRevolution = counts->valuedouble;
        }
        return Convert(0, settings) ? settings : Settings{};
}
} // namespace unit_conversion
#endif
