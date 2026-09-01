#include "core/dso_controller.hpp"
#define DSOX2024A_STATIC
#include "../../sdk/dsox2024a/inc/dsox2024a.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace dso
{
namespace
{
constexpr size_t maxPoints = 4000000;
int
number(const std::string &command, double &val)
{
        char response[256]{};
        int  rc = dsox2024a_query(command.c_str(), response, sizeof(response));
        if (rc)
                return rc;
        char *end = nullptr;
        val       = std::strtod(response, &end);
        if (end == response || !std::isfinite(val))
                return DSOX2024A_ERR_PROTOCOL;
        return 0;
}
void
require(bool valid, const char *message)
{
        if (!valid)
                throw std::invalid_argument(message);
}
} // namespace
void
Controller::close()
{
        if (connected_)
                dsox2024a_close();
        connected_ = false;
}
int
Controller::readSettings(Config &c)
{
        for (int i = 0; i < 4; ++i) {
                if (cancel_)
                        return DSOX2024A_ERR_TIMEOUT;
                auto   prefix  = ":CHANnel" + std::to_string(i + 1) + ":";
                auto  &ch      = c.channels[i];
                double enabled = 0;
                int    rc      = number(prefix + "DISPlay?", enabled);
                if (rc)
                        return rc;
                ch.enabled = enabled != 0;
                if ((rc = number(prefix + "SCALe?", ch.scale)) || (rc = number(prefix + "OFFSet?", ch.offset)) ||
                    (rc = number(prefix + "PROBe?", ch.probe)))
                        return rc;
                char text[64]{};
                if ((rc = dsox2024a_query((prefix + "COUPling?").c_str(), text, sizeof(text))))
                        return rc;
                ch.coupling = text[0] == 'A' || text[0] == 'a';
        }
        int rc = number(":TIMebase:SCALe?", c.timeScale);
        return rc ? rc : number(":TIMebase:POSition?", c.timePosition);
}
Result
Controller::execute(Action action, const Config &c)
{
        Result r;
        r.settings = c;
        int  rc    = 0;
        bool check = false, refresh = false;
        char response[4096]{};
        try {
                require(action == Action::Connect || action == Action::Disconnect || connected_, "请先连接示波器");
                switch (action) {
                        case Action::Connect:
                                require(c.timeout >= 100 && c.timeout <= 10000, "超时应为 100–10000 ms");
                                require(c.usb || (!c.host.empty() && c.port > 0 && c.port <= 65535),
                                        "请填写有效的主机地址与端口");
                                close();
                                rc = c.usb ? dsox2024a_open_usb(c.resource.empty() ? nullptr : c.resource.c_str(), c.timeout)
                                           : dsox2024a_open(c.host.c_str(), (unsigned short)c.port, c.timeout);
                                if (!rc) {
                                        connected_ = true;
                                        rc         = dsox2024a_identify(response, sizeof(response));
                                        if (!rc)
                                                identity_ = response;
                                        refresh = true;
                                }
                                break;
                        case Action::Disconnect:
                                close();
                                r.message = "已断开连接";
                                break;
                        case Action::ReadSettings:
                                refresh = true;
                                break;
                        case Action::Channel: {
                                require(c.channel >= 0 && c.channel < 4, "无效的通道");
                                const auto &ch = c.channels[c.channel];
                                require(std::isfinite(ch.scale) && ch.scale > 0 && std::isfinite(ch.offset) &&
                                            std::isfinite(ch.probe) && ch.probe > 0 && ch.coupling >= 0 && ch.coupling <= 1,
                                        "量程和探头倍率必须为正数，偏置必须为有限值");
                                int n = c.channel + 1;
                                if (!(rc = dsox2024a_set_probe_ratio(n, ch.probe)) &&
                                    !(rc = dsox2024a_set_channel_scale(n, ch.scale)) &&
                                    !(rc = dsox2024a_set_channel_offset(n, ch.offset)) &&
                                    !(rc = dsox2024a_set_channel_coupling(n, ch.coupling ? "AC" : "DC")))
                                        rc = dsox2024a_set_channel_enabled(n, ch.enabled);
                                check = refresh = true;
                                break;
                        }
                        case Action::Timebase:
                                require(std::isfinite(c.timeScale) && c.timeScale > 0 && std::isfinite(c.timePosition),
                                        "时基必须为正数，时间偏移必须为有限值");
                                if (!(rc = dsox2024a_set_timebase_scale(c.timeScale)))
                                        rc = dsox2024a_set_timebase_position(c.timePosition);
                                check = refresh = true;
                                break;
                        case Action::Trigger:
                                require(c.triggerChannel >= 0 && c.triggerChannel < 4 && c.slope >= 0 && c.slope <= 2 &&
                                            std::isfinite(c.triggerLevel),
                                        "无效的触发设置");
                                rc    = dsox2024a_set_edge_trigger(c.triggerChannel + 1,
                                                                c.triggerLevel,
                                                                c.slope == 0   ? "POSitive"
                                                                   : c.slope == 1 ? "NEGative"
                                                                                  : "EITHer");
                                check = true;
                                break;
                        case Action::Run:
                                rc    = dsox2024a_run();
                                check = true;
                                break;
                        case Action::Stop:
                                rc    = dsox2024a_stop();
                                check = true;
                                break;
                        case Action::Single:
                                rc    = dsox2024a_single();
                                check = true;
                                break;
                        case Action::Digitize:
                                require(c.channel >= 0 && c.channel < 4, "无效的通道");
                                rc    = dsox2024a_digitize(c.channel + 1);
                                check = true;
                                break;
                        case Action::Autoscale:
                                rc    = dsox2024a_autoscale();
                                check = refresh = true;
                                break;
                        case Action::Reset:
                                rc    = dsox2024a_reset();
                                check = refresh = true;
                                break;
                        case Action::Capture: {
                                require(c.points >= 16 && c.points <= (int)maxPoints, "波形点数应为 16–4000000");
                                require(std::any_of(c.capture.begin(), c.capture.end(), [](bool v) { return v; }),
                                        "请至少选择一个读取通道");
                                for (int ch = 0; ch < 4 && !rc; ++ch) {
                                        if (!c.capture[ch])
                                                continue;
                                        if (cancel_) {
                                                rc = DSOX2024A_ERR_TIMEOUT;
                                                break;
                                        }
                                        Dsox2024aWaveformInfo info{};
                                        std::vector<double>   samples((size_t)c.points);
                                        rc = dsox2024a_read_waveform(ch + 1, c.points, samples.data(), samples.size(), &info);
                                        if (rc == DSOX2024A_ERR_BUFFER_TOO_SMALL && info.points <= maxPoints) {
                                                samples.resize(info.points);
                                                rc = dsox2024a_read_waveform(
                                                    ch + 1, c.points, samples.data(), samples.size(), &info);
                                        }
                                        if (rc)
                                                break;
                                        if (!info.points || info.points > samples.size() || !std::isfinite(info.x_increment) ||
                                            info.x_increment <= 0 || !std::isfinite(info.x_origin) ||
                                            !std::isfinite(info.x_reference)) {
                                                rc = DSOX2024A_ERR_PROTOCOL;
                                                break;
                                        }
                                        auto &wave = r.waves[ch];
                                        wave.values.reserve(info.points);
                                        wave.times.reserve(info.points);
                                        for (size_t i = 0; i < info.points; ++i) {
                                                wave.values.push_back((float)samples[i]);
                                                wave.times.push_back(info.x_origin +
                                                                     ((double)i - info.x_reference) * info.x_increment);
                                        }
                                }
                                r.captured = rc == 0;
                                break;
                        }
                        case Action::Measure:
                                for (auto &row : r.measurements)
                                        row.fill(std::numeric_limits<double>::quiet_NaN());
                                require(std::any_of(c.capture.begin(), c.capture.end(), [](bool v) { return v; }),
                                        "请至少选择一个测量通道");
                                for (int ch = 0; ch < 4 && !rc; ++ch) {
                                        if (!c.capture[ch])
                                                continue;
                                        if (cancel_) {
                                                rc = DSOX2024A_ERR_TIMEOUT;
                                                break;
                                        }
                                        auto &v = r.measurements[ch];
                                        if (!(rc = dsox2024a_measure_frequency(ch + 1, &v[0])) &&
                                            !(rc = dsox2024a_measure_vpp(ch + 1, &v[1])))
                                                rc = dsox2024a_measure_rms(ch + 1, &v[2]);
                                        for (auto &n : v)
                                                if (std::abs(n) >= 9e37)
                                                        n = std::numeric_limits<double>::quiet_NaN();
                                }
                                r.measured = rc == 0;
                                break;
                        case Action::Write:
                        case Action::Query:
                                require(!c.command.empty() && c.command.size() < 1024 &&
                                            c.command.find_first_of("\r\n") == std::string::npos,
                                        "请输入单行 SCPI 指令");
                                if (action == Action::Query) {
                                        require(c.command.find('?') != std::string::npos, "查询指令须包含 ?");
                                        std::string upper = c.command;
                                        for (char &ch : upper)
                                                ch = (char)std::toupper((unsigned char)ch);
                                        require(upper.find("DATA") == std::string::npos,
                                                "二进制数据请使用波形读取功能；此处仅支持文本查询");
                                        rc = dsox2024a_query(c.command.c_str(), response, sizeof(response));
                                        if (!rc)
                                                r.message = response;
                                } else {
                                        require(c.command.find('?') == std::string::npos, "带 ? 的指令请使用查询按钮");
                                        rc    = dsox2024a_write(c.command.c_str());
                                        check = true;
                                }
                                break;
                        case Action::Error:
                                rc        = dsox2024a_check_error(response, sizeof(response));
                                r.message = response;
                                break;
                }
                if (!rc && check)
                        rc = dsox2024a_check_error(response, sizeof(response));
                if (!rc && refresh) {
                        rc              = readSettings(r.settings);
                        r.settingsValid = rc == 0;
                }
                if (rc) {
                        r.error   = rc;
                        r.message = "操作失败 [" + std::to_string(rc) +
                                    "]: " + (dsox2024a_last_error_code() ? dsox2024a_last_error() : "响应格式或波形参数无效");
                        if (rc == DSOX2024A_ERR_TIMEOUT || rc == DSOX2024A_ERR_NETWORK || rc == DSOX2024A_ERR_PROTOCOL ||
                            rc == DSOX2024A_ERR_VISA) {
                                close();
                                r.message += "；连接已关闭，请重新连接";
                        }
                } else if (r.message.empty())
                        r.message = "操作完成";
        } catch (const std::exception &e) {
                r.error   = -1;
                r.message = e.what();
        }
        r.connected = connected_;
        r.identity  = identity_;
        return r;
}
} // namespace dso
