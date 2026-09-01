#include "core/unit_conversion.hpp"
#include "gui/unit_conversion_json.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

using namespace unit_conversion;

static void
check(bool valid, const char *message)
{
        if (!valid)
                throw std::runtime_error(message);
}

static void
near(double actual, double expected, const char *message)
{
        check(std::isfinite(actual) && std::abs(actual - expected) <= std::max(1.0, std::abs(expected)) * 2e-13, message);
}

static Settings
config(Unit source, Unit target, double counts = 4096)
{
        return {true, QuantityOf(source), source, target, counts};
}

static double
converted(double value, Unit source, Unit target, double counts = 4096)
{
        const auto result = Convert(value, config(source, target, counts));
        check(static_cast<bool>(result), "Expected a valid conversion");
        return result.value;
}

static Settings
parse(const char *json)
{
        cJSON *root = cJSON_Parse(json);
        check(root != nullptr, "Test JSON must parse");
        const Settings settings = LoadSettings(root, "conversion");
        cJSON_Delete(root);
        return settings;
}

int
main()
{
        try {
                constexpr double pi = 3.141592653589793238462643383279502884;
                near(converted(pi, Unit::Radian, Unit::Degree), 180, "pi radians equals 180 degrees");
                near(converted(1, Unit::Revolution, Unit::Radian), 2 * pi, "One revolution equals 2 pi radians");
                near(converted(4096, Unit::Count, Unit::Degree), 360, "Encoder counts use configured counts/revolution");
                near(converted(-180, Unit::Degree, Unit::Count), -2048, "Signed position converts back to counts");
                near(converted(2 * pi, Unit::RadPerSecond, Unit::Rpm), 60, "2 pi rad/s equals 60 rpm");
                near(converted(360, Unit::DegPerSecond, Unit::Rpm), 60, "360 deg/s equals 60 rpm");
                near(converted(120, Unit::Rpm, Unit::RevPerSecond), 2, "120 rpm equals 2 rev/s");
                near(converted(8192, Unit::CountPerSecond, Unit::Rpm), 120, "Encoder count rate converts to rpm");
                near(converted(60, Unit::Rpm, Unit::CountPerSecond), 4096, "rpm converts back to count rate");
                near(converted(1, Unit::Inch, Unit::Millimeter), 25.4, "International inch equals 25.4 mm");
                near(converted(123, Unit::Centimeter, Unit::Meter), 1.23, "Centimeters convert to meters");
                near(converted(2000, Unit::Micrometer, Unit::Millimeter), 2, "Micrometers convert to millimeters");
                near(converted(1000, Unit::MillimeterPerSecond, Unit::MeterPerSecond), 1, "mm/s converts to m/s");
                near(converted(200, Unit::CentimeterPerSecond, Unit::MeterPerSecond), 2, "cm/s converts to m/s");
                near(converted(60, Unit::MeterPerMinute, Unit::MeterPerSecond), 1, "m/min converts to m/s");
                near(converted(60000, Unit::MillimeterPerMinute, Unit::MeterPerSecond), 1, "mm/min converts to m/s");
                for (int a = static_cast<int>(Unit::Radian); a <= static_cast<int>(Unit::MeterPerMinute); ++a) {
                        const auto source = static_cast<Unit>(a);
                        for (const unsigned char c : std::string(UnitLabel(source)))
                                check(c >= 32 && c < 127, "Unit labels must be printable ASCII");
                        for (int b = static_cast<int>(Unit::Radian); b <= static_cast<int>(Unit::MeterPerMinute); ++b) {
                                const auto target   = static_cast<Unit>(b);
                                const auto settings = config(source, target);
                                const auto result   = Convert(-12345.678, settings);
                                if (QuantityOf(source) == QuantityOf(target)) {
                                        check(static_cast<bool>(result), "Compatible unit pairs must convert");
                                        near(converted(result.value, target, source),
                                             -12345.678,
                                             "All compatible pairs round trip");
                                        if (source == target)
                                                check(result.value == -12345.678, "Identity conversion is exact");
                                } else {
                                        check(result.error == Error::IncompatibleUnits,
                                              "Cross-quantity conversions are rejected");
                                }
                        }
                }
                for (const auto quantity :
                     {Quantity::AngularPosition, Quantity::AngularSpeed, Quantity::LinearPosition, Quantity::LinearSpeed}) {
                        const auto defaults = DefaultSettings(quantity);
                        check(defaults.enabled && defaults.quantity == quantity && Convert(1, defaults),
                              "Each quantity has valid enabled defaults");
                }

                const double infinity = std::numeric_limits<double>::infinity();
                const double nan      = std::numeric_limits<double>::quiet_NaN();
                const double maximum  = std::numeric_limits<double>::max();
                const double minimum  = std::numeric_limits<double>::denorm_min();
                auto         settings = config(Unit::Count, Unit::Degree);
                for (double invalid : {0.0, -1.0, infinity, -infinity, nan}) {
                        settings.countsPerRevolution = invalid;
                        check(Convert(1, settings).error == Error::InvalidCountsPerRevolution,
                              "Count conversions require finite positive scaling");
                }
                settings = config(Unit::Radian, Unit::Degree, nan);
                check(static_cast<bool>(Convert(1, settings)), "Non-count conversion ignores an unused count scale");
                for (double invalid : {infinity, -infinity, nan})
                        check(Convert(invalid, settings).error == Error::NonFinite, "Nonfinite input is rejected");
                check(Convert(maximum, settings).error == Error::NonFinite, "Overflowing output is rejected");
                near(converted(maximum, Unit::Degree, Unit::Radian),
                     maximum * (pi / 180),
                     "Large finite conversion avoids temporary overflow");
                near(converted(maximum, Unit::Count, Unit::Degree, maximum),
                     360,
                     "Extreme count scaling avoids intermediate overflow");
                near(converted(minimum, Unit::Count, Unit::Radian, minimum),
                     2 * pi,
                     "Subnormal count scaling avoids intermediate underflow");
                check(converted(maximum, Unit::Radian, Unit::Radian) == maximum, "Maximum finite identity remains finite");
                check(std::signbit(converted(-0.0, Unit::Radian, Unit::Degree)), "Signed zero is preserved");
                settings.source = static_cast<Unit>(999);
                check(Convert(1, settings).error == Error::IncompatibleUnits, "Invalid unit enums are rejected safely");
                check(std::string(UnitLabel(settings.source)) == "?", "Invalid label access is safe");
                settings          = config(Unit::Radian, Unit::Degree);
                settings.quantity = static_cast<Quantity>(-1);
                check(Convert(1, settings).error == Error::IncompatibleUnits && !DefaultSettings(settings.quantity).enabled,
                      "Invalid quantities are rejected safely");

                cJSON *root         = cJSON_CreateObject();
                auto   angularSpeed = DefaultSettings(Quantity::AngularSpeed);
                SaveSettings(root, "conversion", angularSpeed);
                auto *saved = cJSON_GetObjectItemCaseSensitive(root, "conversion");
                check(std::string(cJSON_GetObjectItemCaseSensitive(saved, "quantity")->valuestring) == "angular_speed" &&
                          std::string(cJSON_GetObjectItemCaseSensitive(saved, "source")->valuestring) == "rad_per_second" &&
                          std::string(cJSON_GetObjectItemCaseSensitive(saved, "target")->valuestring) == "rpm",
                      "Persistence uses stable string IDs, not ordinal enum values");
                for (int i = static_cast<int>(Unit::Radian); i <= static_cast<int>(Unit::MeterPerMinute); ++i) {
                        const auto source = static_cast<Unit>(i);
                        settings          = config(source, DefaultSettings(QuantityOf(source)).target);
                        settings.enabled  = i % 2 != 0;
                        SaveSettings(root, "conversion", settings);
                        check(cJSON_GetArraySize(root) == 1, "Repeated saves replace the existing key without duplicates");
                        char      *text   = cJSON_PrintUnformatted(root);
                        const auto loaded = parse(text);
                        cJSON_free(text);
                        check(loaded.enabled == settings.enabled && loaded.quantity == settings.quantity &&
                                  loaded.source == settings.source && loaded.target == settings.target &&
                                  loaded.countsPerRevolution == settings.countsPerRevolution,
                              "All unit IDs and enabled/disabled settings round trip through serialized JSON");
                }
                settings = config(Unit::Count, Unit::Degree, 0);
                SaveSettings(root, "conversion", settings);
                check(!LoadSettings(root, "conversion").enabled, "Invalid saved count settings become disabled");
                cJSON_Delete(root);
                check(!LoadSettings(nullptr, "conversion").enabled, "Null parent is safely disabled");
                for (
                    const char *invalid :
                    {R"({})",
                     R"({"conversion":null})",
                     R"({"conversion":{"enabled":true,"quantity":"future_quantity","source":"radian","target":"degree"}})",
                     R"({"conversion":{"enabled":true,"quantity":"angular_position","source":"radian","target":"meter"}})",
                     R"({"conversion":{"enabled":true,"quantity":"angular_speed","source":"count_per_second","target":"rpm"}})",
                     R"({"conversion":{"enabled":true,"quantity":"angular_position","source":"count","target":"degree","countsPerRevolution":0}})",
                     R"({"conversion":{"enabled":true,"quantity":"angular_position","source":0,"target":"degree"}})",
                     R"({"conversion":{"enabled":"true","quantity":"angular_position","source":"radian","target":"degree"}})"})
                        check(!parse(invalid).enabled, "Malformed or incompatible persisted configuration must be disabled");
                const auto configuredCounts = parse(
                    R"({"conversion":{"enabled":true,"quantity":"angular_position","source":"count","target":"degree","countsPerRevolution":2048}})");
                check(configuredCounts.enabled, "Valid count configuration restores");
                near(Convert(1024, configuredCounts).value, 180, "Restored count configuration has correct scale");
                std::puts("PASS: unit identities, all-pair round trips, count scaling, nonfinite/overflow, enum safety and "
                          "JSON persistence");
        } catch (const std::exception &error) {
                std::fprintf(stderr, "FAIL: %s\n", error.what());
                return 1;
        }
}
