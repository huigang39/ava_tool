#pragma once
#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace ava_spice
{
struct Config {
        std::string executable, library, model = "ISG0616N10NM5HSC_L1", pins = "BUS SW S HG G";
        int         device   = 1; // 0: explicitly selected generic ngspice MOS demo; 1: vendor MOS; 2: vendor GaN
        bool        package  = true;
        int         topology = 0; // 0: double pulse; 1: three-phase inverter + star R-L-EMF windings
        double      pwmHz = 20000, pwmCycles = 2, modulation = .7, deadNs = 100;
        double      windingR = .2, windingUh = 100, rpm = 1000, polePairs = 7, emfPeak = 6, phaseLeadDeg = 0;
        double      bus = 48, current = 20, gateOn = 10, gateOff = 0, rg = 5;
        double      loopNh = 10, sourceNh = 1, gateNh = 5, loopR = .02, loadUh = 100;
        double      pulse1Us = 2, gapUs = 1, pulse2Us = 1, edgeNs = 3, stepNs = .5, temperature = 25;
};
/**
 * @brief A 1 us initialization interval precedes the requested complete PWM cycles
 */
inline double
threePhaseEnd(const Config &c)
{
        return 1e-6 + c.pwmCycles / c.pwmHz;
}
struct Sample {
        double time{}, drive{}, vgs{}, vds{}, id{}, iload{}, ifreewheel{}, ig{}, power{}, energy{};
};
struct Result {
        std::vector<Sample> samples;
        std::string         error, log, directory;
        double              eon = 0, eoff = 0, peakV = 0;
        struct Trace {
                std::string         key, label, unit, expression;
                std::vector<double> values;
        };
        std::vector<double> times;
        std::vector<Trace>  traces;
        double              peakCurrent = 0, dcEnergy = 0;
};
std::vector<Result::Trace> probeDefinitions(const Config &config);
struct Model {
        std::string name, pins;
};
std::vector<Model> models(const std::string &path);
std::string        defaultExecutable();
std::string        defaultLibrary();
std::string        netlist(const Config &c, const std::string &dataPath);
Result             run(const Config &c, const std::shared_ptr<std::atomic<bool>> &cancel = {});
class Runner
{
        std::future<Result>                future_;
        std::shared_ptr<std::atomic<bool>> cancel_;

      public:
        ~Runner();
        bool busy() const;
        void start(const Config &config);
        void cancel();
        bool poll(Result &result);
};
} // namespace ava_spice
