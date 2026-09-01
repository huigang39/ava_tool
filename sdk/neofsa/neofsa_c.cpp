#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "neofsa_c.h"
#include <ava/sdk.h>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

namespace
{
std::mutex               mutex;
thread_local std::string error;
HMODULE                  module{};
AvaEthHandle             handle{};
std::filesystem::path    loadedPath;
neofsa_param             active{};
#define API(n) \
        decltype(&ava_eth_##n) n {}
API(open);
API(close);
API(get_info);
API(enumerate);
API(enumerate_npcap);
API(set_targets);
API(get_feedback);
API(enable);
API(disable);
API(reset_fault);
API(keepalive);
API(last_error_message);
API(set_pid);
API(get_pid_status);
#undef API

int
fail(int code, const std::string &message)
{
        error = message;
        return code;
}
int
check(int rc)
{
        if (!rc)
                return 0;
        static const char *messages[]  = {"成功",
                                          "参数无效",
                                          "运行库或后端不支持该操作",
                                          "当前控制状态不允许操作",
                                          "执行器故障",
                                          "设备离线",
                                          "命令看门狗已过期",
                                          "访问被拒绝",
                                          "通信失败",
                                          "内存不足",
                                          "设备或请求忙"};
        error                          = rc >= 0 && rc < 11 ? messages[rc] : "ava_eth 未知错误";
        error                         += " [ava_eth=" + std::to_string(rc) + "]";
        const char *detail             = last_error_message ? last_error_message() : nullptr;
        if (detail && *detail)
                error += std::string("；运行库原文：") + detail;
        return -100 - rc;
}
template <size_t N>
std::string
text(const char (&s)[N])
{
        size_t n = 0;
        while (n < N && s[n])
                ++n;
        return std::string(s, n);
}
std::filesystem::path
utf8(const std::string &s)
{
        return std::filesystem::path(std::u8string(s.begin(), s.end()));
}
std::filesystem::path
dllDir()
{
        HMODULE self{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&dllDir),
                                &self))
                return {};
        wchar_t buffer[32768];
        DWORD   n = GetModuleFileNameW(self, buffer, 32768);
        return n && n < 32768 ? std::filesystem::path(buffer).parent_path() : std::filesystem::path{};
}
int
load(const std::string &configured)
{
        auto path = configured.empty() ? dllDir() / L"ava_eth_sdk.dll" : std::filesystem::absolute(utf8(configured));
        if (configured.empty() && !std::filesystem::exists(path)) {
                auto root = dllDir();
                for (int i = 0; i < 7; ++i, root = root.parent_path()) {
                        auto candidate = root / L"sdk/ava_eth/bin/win/ava_eth_sdk.dll";
                        if (std::filesystem::exists(candidate)) {
                                path = candidate;
                                break;
                        }
                }
        }
        path = std::filesystem::absolute(path).lexically_normal();
        if (module) {
                if (configured.empty() || path == loadedPath)
                        return 0;
                return fail(NEOFSA_ERR_RUNTIME, "更换运行库前请先调用 neofsa_shutdown");
        }
        module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module)
                return fail(NEOFSA_ERR_RUNTIME,
                            "无法加载 ava_eth_sdk.dll 或其依赖，Windows 错误 " + std::to_string(GetLastError()));
#define BIND(n)                                                                   \
        n = reinterpret_cast<decltype(n)>(GetProcAddress(module, "ava_eth_" #n)); \
        if (!n) {                                                                 \
                FreeLibrary(module);                                              \
                module = nullptr;                                                 \
                return fail(NEOFSA_ERR_RUNTIME, "运行库缺少接口 ava_eth_" #n);    \
        }
        BIND(open);
        BIND(close);
        BIND(get_info);
        BIND(enumerate);
        BIND(enumerate_npcap);
        BIND(set_targets);
        BIND(get_feedback);
        BIND(enable);
        BIND(disable);
        BIND(reset_fault);
        BIND(keepalive);
        BIND(last_error_message);
#undef BIND
        // PD 功能按需检查；旧运行库仍可使用基础接口
        set_pid        = reinterpret_cast<decltype(set_pid)>(GetProcAddress(module, "ava_eth_set_pid"));
        get_pid_status = reinterpret_cast<decltype(get_pid_status)>(GetProcAddress(module, "ava_eth_get_pid_status"));
        loadedPath     = path;
        return 0;
}
void
disconnect()
{
        if (handle) {
                close(handle);
                handle = nullptr;
        }
        active = {};
}
int
info(neofsa_param &p)
{
        p.master_running = handle ? 1 : 0;
        p.capabilities = p.online = 0;
        if (!handle)
                return 0;
        AvaEthInfo i{};
        int        rc = check(get_info(handle, &i));
        if (rc)
                return rc;
        if (i.version != AVA_ETH_ABI || i.size != sizeof(i))
                return fail(NEOFSA_ERR_ABI, "ava_eth ABI 不兼容，请同步 SDK 头文件和运行库");
        p.capabilities = i.capabilities;
        p.online       = i.online;
        p.backend      = active.backend;
        p.axes         = i.axes;
        p.cycle_us     = i.period_us;
        p.watchdog_ms  = i.watchdog_ms;
        std::snprintf(p.adapter, sizeof(p.adapter), "%s", active.adapter);
        std::snprintf(p.runtime_dll, sizeof(p.runtime_dll), "%s", active.runtime_dll);
        return 0;
}
template <class F>
int
guarded(F f) noexcept
{
        try {
                std::lock_guard<std::mutex> lock(mutex);
                error.clear();
                return f();
        } catch (const std::exception &e) {
                return fail(NEOFSA_ERR_RUNTIME, std::string("neofsa 内部错误：") + e.what());
        } catch (...) {
                return fail(NEOFSA_ERR_RUNTIME, "neofsa 内部未知错误");
        }
}
} // namespace
extern "C" const char *
neofsa_last_error()
{
        return error.c_str();
}
extern "C" int
neofsa_shutdown()
{
        return guarded([] {
                disconnect();
                if (module)
                        FreeLibrary(module);
                module = nullptr;
                loadedPath.clear();
                return 0;
        });
}
extern "C" int
neofsa_read_param(neofsa_param *p)
{
        return guarded([&] {
                if (!p)
                        return fail(-1, "参数指针为空");
                return info(*p);
        });
}
extern "C" int
neofsa_write_param(neofsa_param *p)
{
        return guarded([&] {
                if (!p)
                        return fail(-1, "参数指针为空");
                auto action      = p->master_action;
                p->master_action = NEOFSA_MASTER_NONE;
                if (action == NEOFSA_MASTER_STOP) {
                        disconnect();
                        return info(*p);
                }
                if (action == NEOFSA_MASTER_NONE)
                        return info(*p);
                if (action != NEOFSA_MASTER_START)
                        return fail(-1, "无效的主站操作");
                if (handle)
                        return fail(-1, "主站已连接，请先停止再修改连接配置");
                p->master_running = p->capabilities = p->online = 0;
                if (!p->backend)
                        p->backend = NEOFSA_BACKEND_NPCAP;
                if (!p->axes)
                        p->axes = 1;
                if (!p->cycle_us)
                        p->cycle_us = 1000;
                if (!p->watchdog_ms)
                        p->watchdog_ms = 100;
                int rc = load(text(p->runtime_dll));
                if (rc)
                        return rc;
                const auto        adapter = text(p->adapter);
                AvaEthOpenOptions options{sizeof(options),
                                          static_cast<uint32_t>(p->backend),
                                          p->axes,
                                          p->cycle_us,
                                          p->watchdog_ms,
                                          0,
                                          adapter.c_str()};
                rc = check(open(&options, &handle));
                if (rc)
                        return rc;
                active                                             = *p;
                active.adapter[sizeof(active.adapter) - 1]         = 0;
                active.runtime_dll[sizeof(active.runtime_dll) - 1] = 0;
                rc                                                 = info(*p);
                if (rc) {
                        disconnect();
                        p->master_running = 0;
                }
                return rc;
        });
}
extern "C" int
neofsa_read_adapter_param(neofsa_adapter_param *p)
{
        return guarded([&] {
                if (!p)
                        return fail(-1, "参数指针为空");
                p->count   = 0;
                p->name[0] = 0;
                if (p->backend == NEOFSA_BACKEND_SIM)
                        return 0;
                if (p->backend != 0 && p->backend != NEOFSA_BACKEND_NPCAP && p->backend != NEOFSA_BACKEND_DEVICE_SIM &&
                    p->backend != NEOFSA_BACKEND_DEVICE_HARDWARE)
                        return fail(-1, "无效的主站后端");
                int rc = load(text(p->runtime_dll));
                if (rc)
                        return rc;
                auto     enumerateFn = (!p->backend || p->backend == NEOFSA_BACKEND_NPCAP) ? enumerate_npcap : enumerate;
                uint32_t count       = 0;
                rc                   = check(enumerateFn(nullptr, 0, &count));
                if (rc)
                        return rc;
                std::vector<AvaEthDeviceInfo> devices(count);
                rc = check(enumerateFn(devices.data(), count, &p->count));
                if (rc)
                        return rc;
                if (p->index >= p->count || p->index >= devices.size())
                        return p->count ? fail(-1, "网卡索引越界或枚举结果变化，请重新枚举") : 0;
                std::snprintf(p->name, sizeof(p->name), "%s", devices[p->index].path);
                return 0;
        });
}
extern "C" int
neofsa_read_rt(neofsa_rt *r)
{
        return guarded([&] {
                if (!r)
                        return fail(-1, "实时参数指针为空");
                r->feedback = {};
                if (!handle)
                        return fail(-2, "主站未连接");
                AvaEthFeedback f{};
                int            rc = check(get_feedback(handle, r->axis, &f));
                if (rc)
                        return rc;
                auto &v                  = r->feedback;
                v.flags                  = f.flags;
                v.error_code             = f.error_code;
                v.mode                   = f.mode;
                v.special_cmd            = f.special_cmd;
                v.position               = f.position;
                v.velocity               = f.velocity;
                v.current                = f.current;
                v.torque                 = f.torque;
                v.electromagnetic_torque = (f.flags & AVA_ETH_ELECTROMAGNETIC_TORQUE_VALID) ? f.electromagnetic_torque : 0;
                v.vbus                   = f.vbus;
                v.motor_temperature      = f.motor_temperature;
                v.drive_temperature      = f.drive_temperature;
                v.timestamp_ns           = f.timestamp_ns;
                v.accepted_sequence      = f.accepted_sequence;
                v.applied_sequence       = f.applied_sequence;
                return 0;
        });
}
extern "C" int
neofsa_write_rt(neofsa_rt *r)
{
        return guarded([&] {
                if (!r)
                        return fail(-1, "实时参数指针为空");
                auto action = r->action;
                r->action   = NEOFSA_ACTION_NONE;
                if (!handle)
                        return fail(-2, "主站未连接");
                if (action < NEOFSA_ACTION_NONE || action > NEOFSA_ACTION_KEEPALIVE)
                        return fail(-1, "无效的执行器操作");
                // 失能优先，丢弃同一请求中的待发送目标
                if (action == NEOFSA_ACTION_DISABLE) {
                        r->write_target = 0;
                        return check(disable(handle, r->axis));
                }
                if (r->write_target) {
                        const auto &t = r->target;
                        if (!std::isfinite(t.position) || !std::isfinite(t.velocity) || !std::isfinite(t.current) ||
                            !std::isfinite(t.torque))
                                return fail(-1, "目标值必须是有限数值");
                        AvaEthTarget target{r->axis, static_cast<int32_t>(t.mode), t.position, t.velocity, t.current, t.torque};
                        int          rc = check(set_targets(handle, &target, 1, nullptr));
                        if (rc)
                                return rc;
                        r->write_target = 0;
                }
                switch (action) {
                        case NEOFSA_ACTION_ENABLE:
                                return check(enable(handle, r->axis));
                        case NEOFSA_ACTION_RESET_FAULT:
                                return check(reset_fault(handle, r->axis));
                        case NEOFSA_ACTION_KEEPALIVE:
                                return check(keepalive(handle, r->axis));
                        default:
                                return 0;
                }
        });
}

// SDK 面板卸载前的可选生命周期钩子，避免在 DllMain 的加载器锁下等待周期线程
extern "C" void
ava_tool_sdk_shutdown()
{
        neofsa_shutdown();
}

extern "C" int
neofsa_write_pd_param(neofsa_pd_param *p)
{
        return guarded([&] {
                if (!p)
                        return fail(-1, "PD 参数指针为空");
                if (!handle)
                        return fail(-2, "主站未连接");
                if (!set_pid || !get_pid_status)
                        return fail(NEOFSA_ERR_RUNTIME, "运行库缺少 PD 参数接口，请更新 ava_eth");
                if (p->axis || !std::isfinite(p->kp) || !std::isfinite(p->kd) || p->kp < 0 || p->kd < 0 || p->kp > 65504 ||
                    p->kd > 65504)
                        return fail(-1, "PD 参数要求 axis=0，Kp/Kd 为 0..65504 的有限数值");
                AvaEthFeedback f{};
                int            rc = check(get_feedback(handle, p->axis, &f));
                if (rc)
                        return rc;
                if (!(f.flags & AVA_ETH_FEEDBACK_VALID) || (f.flags & AVA_ETH_ENABLED))
                        return fail(-1, "请在反馈有效且执行器失能时设置 PD 增益");
                AvaEthPidParameters values{};
                values.values[0]  = p->kp;
                values.values[1]  = p->kd;
                uint64_t sequence = 0;
                rc                = check(set_pid(handle, p->axis, 3, &values, &sequence));
                if (rc)
                        return rc;
                p->sequence = sequence;
                p->state    = AVA_ETH_PID_PENDING;
                p->result   = 0;
                p->sent_kp = p->sent_kd = 0;
                p->message[0]           = 0;
                return 0;
        });
}
extern "C" int
neofsa_read_pd_param(neofsa_pd_param *p)
{
        return guarded([&] {
                if (!p)
                        return fail(-1, "PD 参数指针为空");
                if (!handle)
                        return fail(-2, "主站未连接");
                if (p->axis)
                        return fail(-1, "PD 参数仅支持 axis=0");
                if (!get_pid_status)
                        return fail(NEOFSA_ERR_RUNTIME, "运行库缺少 PD 参数状态接口，请更新 ava_eth");
                AvaEthPidStatus status{};
                int             rc = check(get_pid_status(handle, &status));
                if (rc)
                        return rc;
                p->state    = status.state;
                p->result   = status.result;
                p->sequence = status.sequence;
                p->sent_kp  = status.values.values[0];
                p->sent_kd  = status.values.values[1];
                std::snprintf(p->message, sizeof(p->message), "%s", status.message);
                return 0;
        });
}
