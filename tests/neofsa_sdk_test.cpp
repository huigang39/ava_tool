#include "../sdk/neofsa/neofsa_c.h"
#include "core/sdk_loader.hpp"
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <windows.h>

int
main(int argc, char **argv)
{
        assert(argc == 2);
        std::ifstream file("sdk/neofsa/neofsa_c.h");
        std::string   source((std::istreambuf_iterator<char>(file)), {});
        auto          parsed = parseHeaderFull(source);
        for (auto [name, size] : {std::pair{"neofsa_param", sizeof(neofsa_param)},
                                  {"neofsa_adapter_param", sizeof(neofsa_adapter_param)},
                                  {"neofsa_pd_param", sizeof(neofsa_pd_param)},
                                  {"neofsa_rt", sizeof(neofsa_rt)}}) {
                const auto *table = findStruct(parsed, name);
                assert(table && table->isPOD && table->totalSize == size);
        }
        const auto *rtLayout    = findStruct(parsed, "neofsa_rt");
        bool        targetFound = false, feedbackFound = false;
        for (const auto &f : rtLayout->fields) {
                if (f.name == "target.torque") {
                        assert(f.offset == offsetof(neofsa_rt, target) + offsetof(neofsa_target, torque));
                        targetFound = true;
                }
                if (f.name == "feedback.timestamp_ns") {
                        assert(f.offset == offsetof(neofsa_rt, feedback) + offsetof(neofsa_feedback, timestamp_ns));
                        feedbackFound = true;
                }
        }
        assert(targetFound && feedbackFound);
        SdkLoader loader;
        assert(loader.load(argv[1]));
        HMODULE hold = LoadLibraryA(argv[1]);
        assert(hold);
#define API(n)                                                                                \
        auto n = reinterpret_cast<decltype(&neofsa_##n)>(GetProcAddress(hold, "neofsa_" #n)); \
        assert(n)
        API(write_param);
        API(read_param);
        API(write_rt);
        API(read_rt);
        API(last_error);
        API(shutdown);
        API(read_pd_param);
        API(write_pd_param);
#undef API
        assert(write_param(nullptr) == NEOFSA_ERR_ARG);
        assert(std::string(last_error()).find("为空") != std::string::npos);
        neofsa_param p{};
        p.backend       = NEOFSA_BACKEND_SIM;
        p.master_action = NEOFSA_MASTER_START;
        assert(write_param(&p) == 0 && p.master_running && p.axes == 1 && p.watchdog_ms == 100);
        neofsa_rt rt{};
        rt.target.mode  = NEOFSA_MODE_CST;
        rt.write_target = 1;
        assert(write_rt(&rt) == 0 && !rt.write_target);
        rt.action = NEOFSA_ACTION_ENABLE;
        assert(write_rt(&rt) == 0 && !rt.action);
        Sleep(10);
        assert(read_rt(&rt) == 0 && (rt.feedback.flags & 7) == 7);
        rt.target.torque = .125f;
        rt.write_target  = 1;
        assert(write_rt(&rt) == 0);
        Sleep(10);
        assert(read_rt(&rt) == 0);
        assert(rt.feedback.applied_sequence > 0 && std::abs(rt.feedback.torque - .125f) < .001f);
        // 普通写目标不隐式在线切换模式，失败保留写门控
        rt.target.mode  = NEOFSA_MODE_CSV;
        rt.write_target = 1;
        assert(write_rt(&rt) != 0 && rt.write_target);
        rt.target.mode  = NEOFSA_MODE_CST;
        rt.write_target = 0;
        // 显式续约有效；读反馈本身不续约
        Sleep(60);
        rt.action = NEOFSA_ACTION_KEEPALIVE;
        assert(write_rt(&rt) == 0);
        Sleep(60);
        assert(read_rt(&rt) == 0 && (rt.feedback.flags & 4));
        auto seq         = rt.feedback.accepted_sequence;
        rt.target.torque = std::numeric_limits<float>::quiet_NaN();
        rt.write_target  = 1;
        assert(write_rt(&rt) == NEOFSA_ERR_ARG && rt.write_target);
        assert(read_rt(&rt) == 0 && rt.feedback.accepted_sequence == seq);
        // 即使目标非法，失能仍优先执行
        rt.action = NEOFSA_ACTION_DISABLE;
        assert(write_rt(&rt) == 0 && !rt.write_target);
        assert(read_rt(&rt) == 0 && !(rt.feedback.flags & 4));
        rt.target.torque = 0;
        rt.write_target  = 1;
        assert(write_rt(&rt) == 0);
        rt.action = NEOFSA_ACTION_ENABLE;
        assert(write_rt(&rt) == 0);
        Sleep(140);
        assert(read_rt(&rt) == 0 && (rt.feedback.flags & 16) && !(rt.feedback.flags & 4));
        rt.axis = 99;
        assert(read_rt(&rt) != 0 && rt.feedback.flags == 0);
        rt.axis = 0;
        // 保留额外 DLL 引用，验证 loader 的卸载钩子确实关闭了会话
        loader.unload();
        assert(read_rt(&rt) == NEOFSA_ERR_NO_MASTER);
        assert(read_param(&p) == 0 && !p.master_running);
        p.master_action = NEOFSA_MASTER_START;
        assert(write_param(&p) == 0);
        neofsa_pd_param gains{};
        gains.kp = .5f;
        gains.kd = .05f;
        assert(write_pd_param(nullptr) == NEOFSA_ERR_ARG);
        gains.kp = -1;
        assert(write_pd_param(&gains) == NEOFSA_ERR_ARG);
        gains.kp = .5f;
        // 模拟器不会假装成功写入 PD 增益
        assert(write_pd_param(&gains) == -102 && read_pd_param(&gains) == -102);
        rt                 = {};
        rt.target.mode     = NEOFSA_MODE_PD;
        rt.target.position = .1f;
        rt.write_target    = 1;
        assert(write_rt(&rt) == 0);
        rt.action = NEOFSA_ACTION_ENABLE;
        assert(write_rt(&rt) == 0);
        Sleep(10);
        assert(read_rt(&rt) == 0 && rt.feedback.mode == -128 && (rt.feedback.flags & 7) == 7);
        assert(write_pd_param(&gains) == NEOFSA_ERR_ARG); // 运行中不改增益
        rt.action = NEOFSA_ACTION_DISABLE;
        assert(write_rt(&rt) == 0);
        assert(shutdown() == 0 && shutdown() == 0);
        assert(FreeLibrary(hold));
        std::cout << "neofsa SDK layout, torque, validation, watchdog and unload passed\n";
}
