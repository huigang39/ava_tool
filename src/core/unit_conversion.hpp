#ifndef AVA_UNIT_CONVERSION_HPP
#define AVA_UNIT_CONVERSION_HPP

#include <cmath>

namespace unit_conversion
{
enum class Quantity { AngularPosition, AngularSpeed, LinearPosition, LinearSpeed };
enum class Unit {
        Radian,
        Degree,
        Revolution,
        Count,
        RadPerSecond,
        DegPerSecond,
        Rpm,
        RevPerSecond,
        CountPerSecond,
        Millimeter,
        Centimeter,
        Meter,
        Micrometer,
        Inch,
        MillimeterPerSecond,
        CentimeterPerSecond,
        MeterPerSecond,
        MillimeterPerMinute,
        MeterPerMinute
};
struct Settings {
        bool     enabled  = false;
        Quantity quantity = Quantity::AngularPosition;
        Unit     source = Unit::Radian, target = Unit::Degree;
        double   countsPerRevolution = 0;
};
enum class Error { None, IncompatibleUnits, InvalidCountsPerRevolution, NonFinite };
struct Result {
        double   value = 0;
        Error    error = Error::None;
        explicit operator bool() const { return error == Error::None; }
};

inline constexpr Quantity
QuantityOf(Unit unit)
{
        switch (unit) {
                case Unit::Radian:
                case Unit::Degree:
                case Unit::Revolution:
                case Unit::Count:
                        return Quantity::AngularPosition;
                case Unit::RadPerSecond:
                case Unit::DegPerSecond:
                case Unit::Rpm:
                case Unit::RevPerSecond:
                case Unit::CountPerSecond:
                        return Quantity::AngularSpeed;
                case Unit::Millimeter:
                case Unit::Centimeter:
                case Unit::Meter:
                case Unit::Micrometer:
                case Unit::Inch:
                        return Quantity::LinearPosition;
                case Unit::MillimeterPerSecond:
                case Unit::CentimeterPerSecond:
                case Unit::MeterPerSecond:
                case Unit::MillimeterPerMinute:
                case Unit::MeterPerMinute:
                        return Quantity::LinearSpeed;
        }
        return static_cast<Quantity>(-1);
}

inline constexpr bool
UsesCounts(Unit unit)
{
        return unit == Unit::Count || unit == Unit::CountPerSecond;
}

inline constexpr const char *
UnitLabel(Unit unit)
{
        switch (unit) {
                case Unit::Radian:
                        return "rad";
                case Unit::Degree:
                        return "deg";
                case Unit::Revolution:
                        return "rev";
                case Unit::Count:
                        return "count";
                case Unit::RadPerSecond:
                        return "rad/s";
                case Unit::DegPerSecond:
                        return "deg/s";
                case Unit::Rpm:
                        return "rpm";
                case Unit::RevPerSecond:
                        return "rev/s";
                case Unit::CountPerSecond:
                        return "count/s";
                case Unit::Millimeter:
                        return "mm";
                case Unit::Centimeter:
                        return "cm";
                case Unit::Meter:
                        return "m";
                case Unit::Micrometer:
                        return "um";
                case Unit::Inch:
                        return "in";
                case Unit::MillimeterPerSecond:
                        return "mm/s";
                case Unit::CentimeterPerSecond:
                        return "cm/s";
                case Unit::MeterPerSecond:
                        return "m/s";
                case Unit::MillimeterPerMinute:
                        return "mm/min";
                case Unit::MeterPerMinute:
                        return "m/min";
        }
        return "?";
}

inline constexpr Settings
DefaultSettings(Quantity quantity)
{
        switch (quantity) {
                case Quantity::AngularPosition:
                        return {true, quantity, Unit::Radian, Unit::Degree, 0};
                case Quantity::AngularSpeed:
                        return {true, quantity, Unit::RadPerSecond, Unit::Rpm, 0};
                case Quantity::LinearPosition:
                        return {true, quantity, Unit::Millimeter, Unit::Meter, 0};
                case Quantity::LinearSpeed:
                        return {true, quantity, Unit::MillimeterPerSecond, Unit::MeterPerSecond, 0};
        }
        return {};
}

namespace detail
{
inline constexpr bool
ValidQuantity(Quantity quantity)
{
        return quantity == Quantity::AngularPosition || quantity == Quantity::AngularSpeed ||
               quantity == Quantity::LinearPosition || quantity == Quantity::LinearSpeed;
}

// Canonical units are radians, radians/second, meters and meters/second.
// A Count factor is per revolution; its configurable divisor is applied by
// Convert without forming a potentially overflowing intermediate scale.
inline constexpr double
CanonicalFactor(Unit unit)
{
        constexpr double pi = 3.141592653589793238462643383279502884;
        switch (unit) {
                case Unit::Radian:
                case Unit::RadPerSecond:
                case Unit::Meter:
                case Unit::MeterPerSecond:
                        return 1;
                case Unit::Degree:
                case Unit::DegPerSecond:
                        return pi / 180;
                case Unit::Revolution:
                case Unit::RevPerSecond:
                case Unit::Count:
                case Unit::CountPerSecond:
                        return 2 * pi;
                case Unit::Rpm:
                        return 2 * pi / 60;
                case Unit::Millimeter:
                case Unit::MillimeterPerSecond:
                        return 0.001;
                case Unit::Centimeter:
                case Unit::CentimeterPerSecond:
                        return 0.01;
                case Unit::Micrometer:
                        return 0.000001;
                case Unit::Inch:
                        return 0.0254;
                case Unit::MillimeterPerMinute:
                        return 0.001 / 60;
                case Unit::MeterPerMinute:
                        return 1.0 / 60;
        }
        return 0;
}
} // namespace detail

// enabled is a presentation opt-in, not a numeric operation. Callers decide
// whether to show a converted tooltip; raw measurements remain unchanged.
inline Result
Convert(double value, const Settings &settings)
{
        if (!detail::ValidQuantity(settings.quantity) || QuantityOf(settings.source) != settings.quantity ||
            QuantityOf(settings.target) != settings.quantity)
                return {0, Error::IncompatibleUnits};
        const bool sourceCounts = UsesCounts(settings.source), targetCounts = UsesCounts(settings.target);
        if ((sourceCounts || targetCounts) &&
            (!std::isfinite(settings.countsPerRevolution) || settings.countsPerRevolution <= 0))
                return {0, Error::InvalidCountsPerRevolution};
        if (!std::isfinite(value))
                return {0, Error::NonFinite};
        if (settings.source == settings.target || value == 0)
                return {value, Error::None};

        // Carry exponents separately so even valid extreme count scales avoid
        // overflow/underflow before the final result (also on MSVC, whose long
        // double has the same range as double).
        int          exponent = 0, sourceExponent = 0, targetExponent = 0;
        double       mantissa        = std::frexp(value, &exponent);
        const double sourceMantissa  = std::frexp(detail::CanonicalFactor(settings.source), &sourceExponent);
        const double targetMantissa  = std::frexp(detail::CanonicalFactor(settings.target), &targetExponent);
        mantissa                    *= sourceMantissa / targetMantissa;
        exponent                    += sourceExponent - targetExponent;
        if (sourceCounts != targetCounts) {
                int          countExponent = 0;
                const double countMantissa = std::frexp(settings.countsPerRevolution, &countExponent);
                if (sourceCounts) {
                        mantissa /= countMantissa;
                        exponent -= countExponent;
                } else {
                        mantissa *= countMantissa;
                        exponent += countExponent;
                }
        }
        const double converted = std::scalbn(mantissa, exponent);
        return std::isfinite(converted) ? Result{converted, Error::None} : Result{0, Error::NonFinite};
}
} // namespace unit_conversion
#endif
