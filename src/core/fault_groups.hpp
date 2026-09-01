#pragma once
#include "core/fault_decoder.hpp"
#include <vector>

namespace motor_faults
{
struct GroupField {
        std::string              page, title;
        std::vector<std::string> suffixes;
};
inline std::vector<GroupField>
groupFields(bool shared)
{
        std::vector<GroupField> fields;
        if (shared) {
                for (unsigned i = 0; i < 8; ++i) {
                        const auto n = std::to_string(i + 1), index = "[" + std::to_string(i) + "]";
                        fields.push_back({"ext" + n,
                                          "Error" + n,
                                          {".err_arr" + index,
                                           index,
                                           ".ext" + n + ".val",
                                           ".u_ext" + n + ".val",
                                           ".ext" + n,
                                           ".Error" + n}});
                }
                return fields;
        }
        for (const auto *kind : {"fault", "warn"}) {
                const unsigned count   = std::string_view(kind) == "fault" ? 4 : 3;
                const char    *names[] = {"mech", "hw", "sw", "cali"};
                for (unsigned i = 0; i < count; ++i) {
                        const auto id = std::string(kind) + "." + names[i];
                        fields.push_back({id,
                                          findPage(id)->title,
                                          {"." + std::string(kind) + ".page." + names[i] + ".val",
                                           "." + std::string(kind) + ".val[" + std::to_string(i) + "]",
                                           ".u_" + std::string(kind) + ".page.u_" + names[i] + ".val",
                                           ".u_" + std::string(kind) + ".val[" + std::to_string(i) + "]"}});
                }
        }
        constexpr std::string_view prefix = "g_mc.lo.safety";
        for (const auto &page : pages)
                if (page.mode == Mode::Enum && std::string_view(page.field).starts_with(prefix))
                        fields.push_back({page.id, page.title, {std::string(page.field).substr(prefix.size())}});
        fields.push_back({"predrv.err", "预驱通信 / nFAULT", {".predrv.comm.err_code"}});
        fields.push_back({"", "故障输出动作", {".fault_action"}});
        fields.push_back({"", "最近控制错误码", {".prev_err"}});
        fields.push_back({"", "后台保存结果", {".store_save_result"}});
        fields.push_back({"", "掉电 PWM 锁止", {".pwm_power_off_latched"}});
        return fields;
}
} // namespace motor_faults
