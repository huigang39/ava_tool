#include "core/spice_runner.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace ava_spice
{
namespace
{
std::string
utf(const std::filesystem::path &p)
{
        auto s = p.u8string();
        return std::string(s.begin(), s.end());
}
std::string
quoted(const std::string &s)
{
        if (s.find_first_of("\"\r\n") != std::string::npos)
                throw std::runtime_error("Invalid SPICE path / SPICE 路径无效");
        auto val = s;
        std::replace(val.begin(), val.end(), '\\', '/');
        return "\"" + val + "\"";
}
void
validate(const Config &c)
{
        auto range = [](double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; };
        if (c.topology < 0 || c.topology > 1 || c.device < 0 || c.device > 2 || !range(c.bus, 1, 1000) ||
            !range(c.gateOn, 2, 25) || !range(c.gateOff, -10, 2) || c.gateOn <= c.gateOff || !range(c.rg, .1, 100) ||
            !range(c.loopNh, .1, 1000) || !range(c.sourceNh, .1, 1000) || !range(c.gateNh, .1, 1000) ||
            !range(c.loopR, .001, 5) || !range(c.edgeNs, .1, 100) || !range(c.temperature, -55, 175))
                throw std::runtime_error("Invalid circuit parameters / 电路参数超出范围");
        if (c.topology == 0 && (!range(c.current, .1, 500) || !range(c.loadUh, 1, 10000) || !range(c.pulse1Us, .1, 50) ||
                                !range(c.gapUs, .1, 50) || !range(c.pulse2Us, .1, 50) || !range(c.stepNs, .05, 5) ||
                                (2 + c.pulse1Us + c.gapUs + c.pulse2Us) * 1000 / c.stepNs > 1000000))
                throw std::runtime_error("Invalid double-pulse parameters / 双脉冲参数超出范围或超过100万输出步");
        if (c.topology == 1 &&
            (!range(c.pwmHz, 100, 200000) || !range(c.pwmCycles, 1, 10000) || std::floor(c.pwmCycles) != c.pwmCycles ||
             threePhaseEnd(c) > .05 || !range(c.modulation, 0, .98) || !range(c.deadNs, 0, 10000) ||
             !range(c.windingR, .001, 100) || !range(c.windingUh, .1, 100000) || !range(c.rpm, -100000, 100000) ||
             !range(c.polePairs, 1, 100) || std::floor(c.polePairs) != c.polePairs || !range(c.emfPeak, 0, 1000) ||
             !range(c.phaseLeadDeg, -360, 360) || !range(c.stepNs, .05, 5000) || threePhaseEnd(c) * 1e9 / c.stepNs > 500000 ||
             (c.deadNs + c.edgeNs) * 1e-9 >= .005 / c.pwmHz || c.stepNs * 1e-9 > .02 / c.pwmHz))
                throw std::runtime_error(
                    "Invalid three-phase parameters: check PWM frequency/cycles, timestep and pulse width / "
                    "三相参数超出范围，请检查 PWM 频率、整数周期数、步长、最短脉宽及50万步上限");
        if (c.device) {
                if (!std::filesystem::is_regular_file(std::filesystem::u8path(c.library)))
                        throw std::runtime_error("Select the manufacturer SPICE library first / 请先选择厂商 SPICE 模型文件");
                if (c.model.empty() ||
                    c.model.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") !=
                        std::string::npos)
                        throw std::runtime_error("Invalid subcircuit name / 子电路名称无效");
        }
}
std::string
instantiate(const Config &c, const std::string &name, const std::map<std::string, std::string> &nodes)
{
        std::istringstream pins(c.pins);
        std::ostringstream s;
        s << name;
        std::string pin;
        int         n = 0;
        while (pins >> pin) {
                auto it = nodes.find(pin);
                if (it == nodes.end())
                        throw std::runtime_error("Invalid pin mapping / 引脚映射无效");
                s << ' ' << it->second;
                ++n;
        }
        if (n < 3 || n > 12)
                throw std::runtime_error("Pin mapping requires 3-12 pins / 引脚映射需要3到12个引脚");
        s << ' ' << c.model << '\n';
        return s.str();
}
} // namespace
std::vector<Model>
models(const std::string &path)
{
        std::ifstream      f(std::filesystem::u8path(path));
        std::vector<Model> result;
        std::string        line;
        while (std::getline(f, line)) {
                std::istringstream stream(line);
                std::string        word;
                stream >> word;
                std::transform(
                    word.begin(), word.end(), word.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (word != ".subckt")
                        continue;
                Model m;
                stream >> m.name;
                std::string pin;
                while (stream >> pin && pin.find('=') == std::string::npos && pin != "params:" && pin != "PARAMS:") {
                        if (!m.pins.empty())
                                m.pins += ' ';
                        m.pins += pin;
                }
                result.push_back(std::move(m));
        }
        return result;
}
std::string
defaultExecutable()
{
        std::filesystem::path app;
#ifdef _WIN32
        wchar_t path[32768]{};
        if (GetModuleFileNameW(nullptr, path, 32768))
                app = std::filesystem::path(path).parent_path();
#endif
        for (const auto &p : {app / "Spice64/bin/ngspice_con.exe",
                              std::filesystem::path("thirdparty/ngspice/Spice64/bin/ngspice_con.exe"),
                              std::filesystem::path("C:/Spice64/bin/ngspice_con.exe"),
                              std::filesystem::path("C:/Spice64/bin/ngspice.exe")})
                if (std::filesystem::is_regular_file(p))
                        return utf(std::filesystem::absolute(p));
        return {};
}
std::string
defaultLibrary()
{
        std::filesystem::path app;
#ifdef _WIN32
        wchar_t path[32768]{};
        if (GetModuleFileNameW(nullptr, path, 32768))
                app = std::filesystem::path(path).parent_path();
#endif
        for (const auto &p :
             {app / "SpiceModels/OptiMOS5_100V_Halfbridges_LTSpice.lib",
              std::filesystem::path("assets/spice/infineon_optimos_100v/OptiMOS5/OptiMOS5_100V_Halfbridges_LTSpice.lib")})
                if (std::filesystem::is_regular_file(p))
                        return utf(std::filesystem::absolute(p));
        return {};
}
std::vector<Result::Trace>
probeDefinitions(const Config &c)
{
        std::vector<Result::Trace> traces;
        auto                       add = [&](std::string key, std::string label, std::string unit, std::string expression) {
                traces.push_back({std::move(key), std::move(label), std::move(unit), std::move(expression), {}});
        };
        add("bus", "DC bus", "V", "v(bus)");
        add("neutral", "N / ground", "V", "v(neutral)");
        add("ibus", "DC current (+ into inverter)", "A", "-i(Vbus)");
        for (int phase = 0; phase < 3; ++phase) {
                std::string p(1, 'a' + phase), label(1, 'A' + phase);
                add(p, label + " / ground", "V", "v(" + p + ")");
                add("winding_" + p, "V" + label + "N (winding)", "V", "v(" + p + ")-v(neutral)");
                add("i_" + p, "I" + label + " (+ inverter to winding)", "A", "i(Vphase" + p + ")");
                add("emf_" + p, "EMF " + label, "V", "v(e" + p + ")-v(neutral)");
                add("s_" + p, label + " source / ground", "V", "v(s" + p + ")");
                add("d_" + p, label + " supply / ground", "V", "v(d" + p + ")");
                add("r_" + p, label + " R-L node / ground", "V", "v(r" + p + ")");
                add("e_" + p, label + " L-EMF node / ground", "V", "v(e" + p + ")");
                add("vr_" + p, label + " resistor voltage", "V", "v(w" + p + ")-v(r" + p + ")");
                add("vl_" + p, label + " inductance voltage", "V", "v(r" + p + ")-v(e" + p + ")");
                for (bool high : {true, false}) {
                        std::string side = high ? "h" : "l", q = label + (high ? "H" : "L");
                        std::string source = high ? p : "s" + p;
                        add("g_" + side + p, q + " gate / ground", "V", "v(g" + side + p + ")");
                        add("drive_" + side + p, q + " driver", "V", "v(drv" + side + p + ")" + (high ? "-v(" + p + ")" : ""));
                        add("vgs_" + side + p, q + " VGS", "V", "v(g" + side + p + ")-v(" + source + ")");
                        add("vds_" + side + p,
                            q + " VDS",
                            "V",
                            high ? "v(d" + p + ")-v(" + p + ")" : "v(" + p + ")-v(s" + p + ")");
                        add("id_" + side + p,
                            q + " ID (+ drain to source)",
                            "A",
                            high ? "i(Vbus" + p + ")" : "i(Vsource" + p + ")-i(Vgl" + p + ")");
                }
        }
        add("vab", "VAB", "V", "v(a)-v(b)");
        add("vbc", "VBC", "V", "v(b)-v(c)");
        add("vca", "VCA", "V", "v(c)-v(a)");
        add("ground", "Ground reference", "V", "0*v(bus)");
        return traces;
}
namespace
{
std::string
threePhaseNetlist(const Config &c, const std::string &dataPath)
{
        std::ostringstream s;
        s.imbue(std::locale::classic());
        s << std::setprecision(15);
        s << "AvA ngspice three phase inverter + star R L EMF windings\n";
        if (c.device)
                s << ".include " << quoted(utf(std::filesystem::absolute(std::filesystem::u8path(c.library)))) << '\n';
        else
                s << ".model AVA_DEMO VDMOS(Vto=4 Kp=40 Rd=.005 Rs=.005 Rg=1 Cgs=2n Cgdmax=.7n Cgdmin=30p Cjo=1n Is=1e-12 "
                     "Bv=120 Ibv=1m)\n";
        const double pi = 3.14159265358979323846, period = 1 / c.pwmHz, end = threePhaseEnd(c);
        const double edge = c.edgeNs * 1e-9, dead = c.deadNs * 1e-9, start = 1e-6;
        const double frequency = c.rpm * c.polePairs / 60;
        s << "Vbus bus 0 " << c.bus << "\nRneutral neutral 0 1e9\nVtemperature temp 0 " << c.temperature << '\n';
        for (int phase = 0; phase < 3; ++phase) {
                std::string p(1, 'a' + phase);
                // PWM generation only schedules ideal driver edges. Device dynamics
                // and winding currents are solved by ngspice, not by this generator
                double                                 angle = -2 * pi * phase / 3;
                std::vector<std::pair<double, double>> high{{0, c.gateOff}}, low{{0, c.gateOff}};
                auto                                   pulse = [&](auto &points, double on, double off) {
                        if (off - on <= 2 * edge)
                                return;
                        points.emplace_back(on, c.gateOff);
                        points.emplace_back(on + edge, c.gateOn);
                        points.emplace_back(off - edge, c.gateOn);
                        points.emplace_back(off, c.gateOff);
                };
                double lowOn = start + dead;
                for (int cycle = 0; cycle < static_cast<int>(c.pwmCycles); ++cycle) {
                        const double t = start + cycle * period;
                        double       duty =
                            .5 + .5 * c.modulation *
                                     std::sin(2 * pi * frequency * (t + period * .5) + angle + c.phaseLeadDeg * pi / 180);
                        double left = t + (1 - duty) * period * .5, right = t + (1 + duty) * period * .5;
                        pulse(high, left + dead, right);
                        pulse(low, lowOn, left);
                        lowOn = right + dead;
                }
                // Complementary low-side conduction continues across carrier
                // boundaries. Keep the final state through the transient end
                pulse(low, lowOn, end + period);
                auto driver = [&](const std::string &side, const std::string &ref, const auto &points) {
                        s << "Vdrv" << side << p << " drv" << side << p << ' ' << ref << " PWL(";
                        for (auto [t, v] : points)
                                s << t << ' ' << v << ' ';
                        s << ")\nRg" << side << p << " drv" << side << p << " gx" << side << p << ' ' << c.rg;
                        s << "\nLg" << side << p << " gx" << side << p << " gg" << side << p << ' ' << c.gateNh << "n";
                        s << "\nVg" << side << p << " gg" << side << p << " g" << side << p << " 0\n";
                };
                driver("h", p, high);
                driver("l", "0", low);
                s << "Rbus" << p << " bus rb" << p << ' ' << c.loopR << "\nLbus" << p << " rb" << p << " lb" << p << ' '
                  << c.loopNh << "n";
                s << "\nVbus" << p << " lb" << p << " d" << p << " 0";
                s << "\nVsource" << p << " s" << p << " sr" << p << " 0\nRs" << p << " sr" << p << " sx" << p << " .001";
                s << "\nLs" << p << " sx" << p << " 0 " << c.sourceNh << "n\n";
                if (c.device && c.package) {
                        s << instantiate(c,
                                         "Xbridge" + p,
                                         {{"BUS", "d" + p},
                                          {"HG", "gh" + p},
                                          {"SW", p},
                                          {"G", "gl" + p},
                                          {"S", "s" + p},
                                          {"K", "0"},
                                          {"TJ", "temp"},
                                          {"TC", "temp"}});
                } else if (c.device) {
                        s << instantiate(c,
                                         "Xhigh" + p,
                                         {{"D", "d" + p}, {"G", "gh" + p}, {"S", p}, {"K", p}, {"TJ", "temp"}, {"TC", "temp"}});
                        s << instantiate(
                            c,
                            "Xlow" + p,
                            {{"D", p}, {"G", "gl" + p}, {"S", "s" + p}, {"K", "sr" + p}, {"TJ", "temp"}, {"TC", "temp"}});
                } else {
                        s << "Mhigh" << p << " d" << p << " gh" << p << ' ' << p << " AVA_DEMO\nMlow" << p << ' ' << p << " gl"
                          << p << " s" << p << " AVA_DEMO\n";
                }
                s << "Vphase" << p << ' ' << p << " w" << p << " 0\nRw" << p << " w" << p << " r" << p << ' ' << c.windingR;
                s << "\nLw" << p << " r" << p << " e" << p << ' ' << c.windingUh << "u\n";
                // Smoothly introduce EMF from an unenergized operating point
                const double signedEmf = c.rpm == 0 ? 0 : std::copysign(c.emfPeak, c.rpm);
                s << "Bemf" << p << " e" << p << " neutral V=" << signedEmf << "*(1-exp(-time/1e-6))*sin(" << 2 * pi * frequency
                  << "*time+" << angle << ")\n";
        }
        s << ".temp " << c.temperature
          << "\n.options reltol=1e-4 abstol=1e-9 vntol=1e-6 method=gear maxord=2\n.control\nset wr_singlescale\nset "
             "wr_vecnames\nset "
             "numdgt=15\ntran "
          << c.stepNs << "n " << end << " 0 " << c.stepNs << "n\n";
        auto traces = probeDefinitions(c);
        for (const auto &trace : traces)
                s << "let probe_" << trace.key << " = " << trace.expression << '\n';
        s << "wrdata " << dataPath;
        for (const auto &trace : traces)
                s << " probe_" << trace.key;
        s << "\nquit\n.endc\n.end\n";
        return s.str();
}
} // namespace
std::string
netlist(const Config &c, const std::string &dataPath)
{
        validate(c);
        if (c.topology == 1)
                return threePhaseNetlist(c, dataPath);
        std::ostringstream s;
        s.imbue(std::locale::classic());
        s << std::setprecision(12);
        const double start = .5e-6, firstEnd = start + c.pulse1Us * 1e-6, secondStart = firstEnd + c.gapUs * 1e-6;
        const double secondEnd = secondStart + c.pulse2Us * 1e-6, edge = c.edgeNs * 1e-9, end = secondEnd + 1.5e-6;
        s << "AvA ngspice double pulse: MOSFET / GaN\n";
        if (c.device)
                s << ".include " << quoted(utf(std::filesystem::absolute(std::filesystem::u8path(c.library)))) << '\n';
        else
                s << ".model AVA_DEMO VDMOS(Vto=4 Kp=40 Rd=.005 Rs=.005 Rg=1 Cgs=2n Cgdmax=.7n Cgdmin=30p Cjo=1n Is=1e-12 "
                     "Bv=120 Ibv=1m)\n";
        s << "Vbus bus 0 " << c.bus << "\nBinitial bus preload V=exp(-time/1e-9)*(v(bus)-v(sw)-.001*(i(VloadSense)-"
          << c.current << "))"
          << "\nVloadSense preload lp 0\nLload lp sw " << c.loadUh << "u"
          << "\nVdriver drv 0 PWL(0 " << c.gateOff << ' ' << start << ' ' << c.gateOff << ' ' << start + edge << ' ' << c.gateOn
          << ' ' << firstEnd << ' ' << c.gateOn << ' ' << firstEnd + edge << ' ' << c.gateOff << ' ' << secondStart << ' '
          << c.gateOff << ' ' << secondStart + edge << ' ' << c.gateOn << ' ' << secondEnd << ' ' << c.gateOn << ' '
          << secondEnd + edge << ' ' << c.gateOff << ")\n"
          << "Rg drv gx " << c.rg << "\nLg gx gg " << c.gateNh << "n\nVgateSense gg g 0\n"
          << "Ls sx 0 " << c.sourceNh << "n\nRs sl sx .001\nVsourceSense s sl 0\n";
        if (c.package && c.device) {
                s << "VfreeSense bus hb 0\nRloop hb hl " << c.loopR << "\nLloop hl hd " << c.loopNh << "n IC=" << -c.current
                  << "\nVhighGate hg sw " << c.gateOff << '\n';
                s << instantiate(c,
                                 "Xpackage",
                                 {{"BUS", "hd"},
                                  {"HG", "hg"},
                                  {"SW", "sw"},
                                  {"G", "g"},
                                  {"S", "s"},
                                  {"K", "0"},
                                  {"TJ", "temp"},
                                  {"TC", "temp"}});
        } else {
                s << "VfreeSense bus hd 0\nRloop hs hl " << c.loopR << "\nLloop hl sw " << c.loopNh << "n IC=" << -c.current
                  << "\nVhighGate hg hs " << c.gateOff << '\n';
                s << "VdrainSense sw d 0\n";
                if (c.device) {
                        s << instantiate(
                            c, "Xdut", {{"D", "d"}, {"G", "g"}, {"S", "s"}, {"K", "sl"}, {"TJ", "temp"}, {"TC", "temp"}});
                        s << instantiate(
                            c, "Xfree", {{"D", "hd"}, {"G", "hg"}, {"S", "hs"}, {"K", "hs"}, {"TJ", "temp"}, {"TC", "temp"}});
                } else
                        s << "Mdut d g s AVA_DEMO\nMfree hd hg hs AVA_DEMO\n";
        }
        s << "Vtemperature temp 0 " << c.temperature << "\n.temp " << c.temperature
          << "\n.options reltol=1e-4 abstol=1e-9 vntol=1e-6 method=trap\n.control\nset wr_singlescale\nset wr_vecnames\nset "
             "numdgt=15\ntran "
          << c.stepNs << "n " << end << " 0 " << c.stepNs << "n\n"
          << "let vgsv = v(g)-v(s)\nlet vdsv = v(sw)-v(s)\nlet ids = "
          << (c.package && c.device ? "i(VsourceSense)-i(VgateSense)" : "i(VdrainSense)")
          << "\nlet ild = i(Lload)\nlet ifree = -i(VfreeSense)\nlet igate = i(VgateSense)\nlet pdevice = vdsv*ids\nwrdata "
          << dataPath << " v(drv) vgsv vdsv ids ild ifree igate pdevice\nquit\n.endc\n.end\n";
        return s.str();
}
Result
run(const Config &c, const std::shared_ptr<std::atomic<bool>> &cancel)
{
        Result result;
        try {
                auto exe = c.executable.empty() ? defaultExecutable() : c.executable;
                if (!std::filesystem::is_regular_file(std::filesystem::u8path(exe)))
                        throw std::runtime_error("ngspice executable not found / 未找到 ngspice 可执行文件");
                static std::atomic<unsigned> counter{0};
                auto                         dir = std::filesystem::temp_directory_path() /
                           ("ava-spice-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                            std::to_string(counter++));
                std::filesystem::create_directories(dir);
                result.directory = utf(dir);
                auto circuit = dir / "double_pulse.cir", data = dir / "waveform.txt", log = dir / "ngspice.log";
                std::ofstream(circuit) << netlist(c, "waveform.txt");
#ifdef _WIN32
                auto exePath = std::filesystem::absolute(std::filesystem::u8path(exe));
                auto cmdUtf =
                    quoted(utf(exePath)) + " -n -D ngbehavior=ltpsa -b -o " + quoted(utf(log)) + " " + quoted(utf(circuit));
                auto         command = std::filesystem::u8path(cmdUtf).native();
                auto         cwd     = dir;
                STARTUPINFOW startup{};
                startup.cb          = sizeof(startup);
                startup.dwFlags     = STARTF_USESHOWWINDOW;
                startup.wShowWindow = SW_HIDE;
                PROCESS_INFORMATION process{};
                if (!CreateProcessW(exePath.c_str(),
                                    command.data(),
                                    nullptr,
                                    nullptr,
                                    FALSE,
                                    CREATE_NO_WINDOW,
                                    nullptr,
                                    cwd.c_str(),
                                    &startup,
                                    &process))
                        throw std::runtime_error("Cannot launch ngspice / 无法启动 ngspice");
                CloseHandle(process.hThread);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(c.topology == 1 ? 180 : 60);
                bool       stopped  = false;
                while (WaitForSingleObject(process.hProcess, 100) == WAIT_TIMEOUT)
                        if ((cancel && cancel->load()) || std::chrono::steady_clock::now() > deadline) {
                                TerminateProcess(process.hProcess, 1);
                                stopped = true;
                                break;
                        }
                WaitForSingleObject(process.hProcess, 2000);
                DWORD code = 1;
                GetExitCodeProcess(process.hProcess, &code);
                CloseHandle(process.hProcess);
                std::ifstream logging(log);
                result.log.assign(std::istreambuf_iterator<char>(logging), {});
                if (stopped)
                        throw std::runtime_error("SPICE run cancelled or timed out / SPICE 已取消或运行超时");
                // ngspice can return success and export partial data after TRAN aborts
                if (result.log.find("tran simulation(s) aborted") != std::string::npos) {
                        auto begin = result.log.find("doAnalyses:");
                        auto end   = result.log.find('\n', begin);
                        throw std::runtime_error("ngspice transient aborted / ngspice 瞬态求解中止: " +
                                                 (begin == std::string::npos ? std::string("see solver log / 请查看日志")
                                                                             : result.log.substr(begin, end - begin)));
                }
                if (code || !std::filesystem::is_regular_file(data))
                        throw std::runtime_error("ngspice failed; see solver log / ngspice 求解失败，请查看日志");
#else
                throw std::runtime_error("This SPICE launcher currently supports Windows / 当前 SPICE 调用器支持 Windows");
#endif
                std::ifstream f(data);
                std::string   line;
                std::getline(f, line);
                if (c.topology == 1) {
                        result.traces = probeDefinitions(c);
                        while (std::getline(f, line)) {
                                std::istringstream stream(line);
                                stream.imbue(std::locale::classic());
                                double time;
                                if (!(stream >> time) || !std::isfinite(time) ||
                                    (!result.times.empty() && time <= result.times.back()))
                                        throw std::runtime_error("Invalid SPICE time / SPICE 时间数据无效");
                                result.times.push_back(time);
                                for (auto &trace : result.traces) {
                                        double val;
                                        if (!(stream >> val) || !std::isfinite(val))
                                                throw std::runtime_error("Invalid probe output / 探针数据无效");
                                        trace.values.push_back(val);
                                        if (trace.key.starts_with("vds_"))
                                                result.peakV = std::max(result.peakV, val);
                                        if (trace.key == "i_a" || trace.key == "i_b" || trace.key == "i_c")
                                                result.peakCurrent = std::max(result.peakCurrent, std::abs(val));
                                }
                                auto  &busCurrent = result.traces[2].values;
                                size_t n          = busCurrent.size();
                                if (n > 1)
                                        result.dcEnergy +=
                                            c.bus * .5 * (busCurrent[n - 1] + busCurrent[n - 2]) * (time - result.times[n - 2]);
                                if (result.times.size() > 500000)
                                        throw std::runtime_error("Probe result exceeds 500000 points / 探针数据超过50万点");
                        }
                        if (result.times.size() < 10 || result.times.back() < threePhaseEnd(c) * .999999)
                                throw std::runtime_error("Incomplete three-phase transient / 三相瞬态数据不完整");
                } else {
                        Sample previous;
                        double integral = 0;
                        while (std::getline(f, line)) {
                                std::istringstream stream(line);
                                stream.imbue(std::locale::classic());
                                Sample sample;
                                if (!(stream >> sample.time >> sample.drive >> sample.vgs >> sample.vds >> sample.id >>
                                      sample.iload >> sample.ifreewheel >> sample.ig >> sample.power))
                                        throw std::runtime_error("Malformed SPICE output / SPICE 输出格式无效");
                                for (double v : {sample.time,
                                                 sample.drive,
                                                 sample.vgs,
                                                 sample.vds,
                                                 sample.id,
                                                 sample.iload,
                                                 sample.ifreewheel,
                                                 sample.ig,
                                                 sample.power})
                                        if (!std::isfinite(v))
                                                throw std::runtime_error("Nonfinite SPICE output / SPICE 输出包含无效数值");
                                if (!result.samples.empty()) {
                                        double dt = sample.time - previous.time;
                                        if (dt <= 0)
                                                throw std::runtime_error("Nonmonotonic SPICE time / SPICE 时间戳无效");
                                        integral   += .5 * (sample.power + previous.power) * dt;
                                        double off = (.5 + c.pulse1Us) * 1e-6, on = (.5 + c.pulse1Us + c.gapUs) * 1e-6;
                                        if (previous.time >= off && sample.time <= off + .5e-6)
                                                result.eoff += .5 * (sample.power + previous.power) * dt;
                                        if (previous.time >= on && sample.time <= on + .5e-6)
                                                result.eon += .5 * (sample.power + previous.power) * dt;
                                }
                                sample.energy = integral;
                                previous      = sample;
                                result.peakV  = std::max(result.peakV, sample.vds);
                                result.samples.push_back(sample);
                                if (result.samples.size() > 1500000)
                                        throw std::runtime_error("SPICE result exceeds point limit / SPICE 输出超过150万点");
                        }
                        if (result.samples.size() < 10 ||
                            result.samples.back().time < (.5 + c.pulse1Us + c.gapUs + c.pulse2Us) * 1e-6)
                                throw std::runtime_error("Incomplete SPICE transient / SPICE 瞬态数据不完整");
                }
        } catch (const std::exception &e) {
                result.error = e.what();
                result.samples.clear();
                result.times.clear();
                result.traces.clear();
        }
        return result;
}
Runner::~Runner()
{
        cancel();
        if (future_.valid())
                future_.wait();
}
bool
Runner::busy() const
{
        return future_.valid();
}
void
Runner::start(const Config &c)
{
        if (busy())
                throw std::runtime_error("SPICE is already running / SPICE 正在运行");
        cancel_ = std::make_shared<std::atomic<bool>>(false);
        future_ = std::async(std::launch::async, [c, token = cancel_] { return run(c, token); });
}
void
Runner::cancel()
{
        if (cancel_)
                cancel_->store(true);
}
bool
Runner::poll(Result &result)
{
        if (!future_.valid() || future_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                return false;
        result = future_.get();
        cancel_.reset();
        return true;
}
} // namespace ava_spice
