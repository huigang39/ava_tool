#pragma once
#include <array>
#include <atomic>
#include <string>
#include <vector>

namespace dso
{
struct Channel {
        bool   enabled = false;
        double scale = 1, offset = 0, probe = 1;
        int    coupling = 0;
};
struct Config {
        bool                   usb = true;
        std::string            host, resource;
        int                    port = 5025, timeout = 2000;
        std::array<Channel, 4> channels{};
        double                 timeScale = 0.001, timePosition = 0, triggerLevel = 0;
        int                    channel = 0, triggerChannel = 0, slope = 0;
        std::array<bool, 4>    capture{true, false, false, false};
        int                    points = 10000;
        std::string            command;
};
enum class Action {
        Connect,
        Disconnect,
        ReadSettings,
        Channel,
        Timebase,
        Trigger,
        Run,
        Stop,
        Single,
        Digitize,
        Autoscale,
        Reset,
        Capture,
        Measure,
        Write,
        Query,
        Error
};
struct Wave {
        std::vector<float>  values;
        std::vector<double> times;
};
struct Result {
        bool                                 connected = false, settingsValid = false, captured = false, measured = false;
        int                                  error = 0;
        std::string                          message, identity;
        Config                               settings;
        std::array<Wave, 4>                  waves;
        std::array<std::array<double, 3>, 4> measurements{};
};
// All execute calls belong to one worker. Cancel is safe from the GUI thread
class Controller
{
      public:
        Result execute(Action action, const Config &config);
        void   cancel() { cancel_.store(true); }
        void   resume() { cancel_.store(false); }
        void   close();

      private:
        std::atomic<bool> cancel_{false};
        bool              connected_ = false;
        std::string       identity_;
        int               readSettings(Config &config);
};
} // namespace dso
