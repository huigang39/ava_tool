#pragma once
#include "../../sdk/ava_eth/include/ava/controller.h"
#include "core/dwarf_parser.hpp"
#include <array>
#include <functional>
#include <string>
#include <vector>

// The wire values are FocDriver's CiA402 values, not foc.lo.mode/state
namespace motor_jlink
{
constexpr uint32_t backend = 5;
enum Field {
        Mode,
        Control,
        Status,
        ActualMode,
        Special,
        TargetPosition,
        TargetVelocity,
        TargetCurrent,
        TargetTorque,
        TargetMode,
        TargetValid,
        PositionLatch,
        FeedVelocity,
        FeedCurrent,
        FeedTorque,
        Position,
        Velocity,
        Current,
        LoadTorque,
        ElectromagneticTorque,
        BusVoltage,
        WindingTemperature,
        InverterTemperature,
        ShmEnable,
        CiaEnable,
        AutoEnable,
        ActiveConfigValid,
        Direction,
        VelocityUnit,
        DriveHeartbeat,
        Count
};
struct Symbol {
        std::string source, name;
        uint32_t    address  = 0;
        uint8_t     size     = 0;
        bool        floating = false, signedValue = false;
};
struct Config {
        std::array<Symbol, Count> fields{};
};
const char         *label(Field field);
const char         *preset(Field field);
bool                optional(Field field);
std::vector<Symbol> symbols(const dwarf::Info &info, const std::string &source);
void                validate(const Config &config);

// Epoch is checked inside the J-Link transport lock, including on every write
struct Io {
        std::function<uint64_t()>                                       epoch;
        std::function<bool(uint64_t, uint32_t, uint32_t, void *)>       read;
        std::function<bool(uint64_t, uint32_t, uint32_t, const void *)> write;
};
Io portIo();
class Session
{
      public:
        explicit Session(Io io) : io_(std::move(io)) {}
        void           connect(const Config &config);
        void           disconnect();
        bool           connected() const { return connected_; }
        AvaEthFeedback feedback();
        AvaEthTarget   target();
        AvaEthTarget   setMode(AvaEthTarget target);
        void           apply(const AvaEthTarget &target);
        void           enable();
        void           stop();
        void           special(uint32_t command);

      private:
        Io       io_;
        Config   config_;
        uint64_t epoch_     = 0;
        bool     connected_ = false;
        double   read(Field field);
        void     write(Field field, double val);
        void     guard();
        double   velocityScale();
        void     waitFor(const std::function<bool()> &condition, const char *operation);
        void     writeTarget(const AvaEthTarget &target);
};
} // namespace motor_jlink
