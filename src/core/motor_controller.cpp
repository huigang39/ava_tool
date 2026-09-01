#include "core/motor_controller.hpp"
#include "core/motor_faults.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
// Winsock must precede windows.h; IP Helper requires the Windows base types
// Keep this dependency order even when formatting/sorting includes on save
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
// clang-format on
#ifdef _MSC_VER
#pragma comment(lib, "iphlpapi.lib")
#endif
#else
#include <dlfcn.h>
#endif

namespace
{
using Clock = std::chrono::steady_clock;
class SdkFailure : public std::runtime_error
{
      public:
        bool permissionDenied;
        SdkFailure(const std::string &message, bool denied) : std::runtime_error(message), permissionDenied(denied) {}
};
class Library
{
      public:
        ~Library() { unload(); }
        void unload()
        {
                if (handle_) {
#ifdef _WIN32
                        FreeLibrary(static_cast<HMODULE>(handle_));
#else
                        dlclose(handle_);
#endif
                        handle_ = nullptr;
                }
        }
        void load(const std::string &path)
        {
                unload();
                const auto selectedPath = path.empty() ? MotorController::bundledLibrary() : path;
#ifdef _WIN32
                const auto wide =
                    std::filesystem::absolute(std::filesystem::path(std::u8string(selectedPath.begin(), selectedPath.end())))
                        .wstring();
                handle_ =
                    LoadLibraryExW(wide.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                if (!handle_)
                        throw std::runtime_error("Cannot load SDK DLL or dependencies; Windows error " +
                                                 std::to_string(GetLastError()));
#else
                handle_ = dlopen(selectedPath.c_str(), RTLD_NOW | RTLD_LOCAL);
                if (!handle_)
                        throw std::runtime_error(dlerror());
#endif
#define BIND(name) name = reinterpret_cast<decltype(name)>(symbol("ava_eth_" #name))
                try {
                        BIND(enumerate);
                        enumerate_npcap = nullptr;
                        try {
                                BIND(enumerate_npcap);
                        } catch (const std::runtime_error &) {
                        }
                        BIND(open);
                        set_pid        = nullptr;
                        get_pid_status = nullptr;
                        try {
                                BIND(set_pid);
                                BIND(get_pid_status);
                        } catch (const std::runtime_error &) {
                        }
                        BIND(close);
                        BIND(get_info);
                        BIND(set_targets);
                        switch_mode = nullptr;
                        try {
                                BIND(switch_mode);
                        } catch (const std::runtime_error &) {
                        }
                        set_interpolation = nullptr;
                        try {
                                BIND(set_interpolation);
                        } catch (const std::runtime_error &) {
                        }
                        BIND(enable);
                        BIND(disable);
                        BIND(reset_fault);
                        set_special_cmd = nullptr;
                        try {
                                BIND(set_special_cmd);
                        } catch (const std::runtime_error &) {
                        }
                        BIND(stop);
                        BIND(keepalive);
                        BIND(get_snapshot);
                        BIND(get_diagnostics);
                        BIND(result_string);
                        BIND(last_os_error);
                        BIND(last_error_message);
                } catch (...) {
                        unload();
                        throw;
                }
#undef BIND
        }
#define API(name) decltype(&ava_eth_##name) name = nullptr
        API(enumerate);
        API(enumerate_npcap);
        API(open);
        API(set_pid);
        API(get_pid_status);
        API(close);
        API(get_info);
        API(set_targets);
        API(switch_mode);
        API(set_interpolation);
        API(enable);
        API(disable);
        API(reset_fault);
        API(set_special_cmd);
        API(stop);
        API(keepalive);
        API(get_snapshot);
        API(get_diagnostics);
        API(result_string);
        API(last_os_error);
        API(last_error_message);
#undef API
        void check(int result, const char *operation = nullptr)
        {
                if (!result)
                        return;
                const auto        os       = last_os_error();
                const std::string detail   = last_error_message();
                std::string       message  = operation ? std::string(operation) + "：" : std::string();
                message                   += result == AVA_ETH_IO ? "设备通信或操作确认失败" : result_string(result);
                message                   += " [SDK " + std::to_string(result) + ", OS " + std::to_string(os) + "]";
                if (!detail.empty())
                        message += " " + detail;
                else if (result == AVA_ETH_IO)
                        message += " 运行库未提供详细原因；OS 0 表示没有附加的系统错误码。";
                auto lower = detail;
                std::transform(
                    lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                // Npcap reports pcap errors as text with OS=0; kernel drivers use AVA_ETH_ACCESS
                const bool denied =
                    result == AVA_ETH_ACCESS || os == 5 || os == 740 || lower.find("access is denied") != std::string::npos ||
                    lower.find("access denied") != std::string::npos || lower.find("permission denied") != std::string::npos ||
                    detail.find("拒绝访问") != std::string::npos;
                throw SdkFailure(message, denied);
        }

      private:
        void *symbol(const char *name)
        {
#ifdef _WIN32
                auto p = reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
                auto p = dlsym(handle_, name);
#endif
                if (!p)
                        throw std::runtime_error(std::string("SDK export missing: ") + name);
                return p;
        }
        void *handle_ = nullptr;
};
} // namespace

MotorController::MotorController() : worker_(&MotorController::run, this) {}

std::string
MotorController::bundledLibrary()
{
#ifdef _WIN32
        static std::mutex extractionMutex;
        std::lock_guard   lock(extractionMutex);
        struct Resource {
                const wchar_t *id;
                const char    *file;
                const char    *bytes;
                DWORD          size;
        };
        Resource resources[] = {{L"AVA_ETH_SDK", "ava_eth_sdk.dll", nullptr, 0},
                                {L"AVA_ETH_PTHREAD", "libwinpthread-1.dll", nullptr, 0}};
        uint64_t hash        = 14695981039346656037ull;
        for (auto &resource : resources) {
                const auto module = GetModuleHandleW(nullptr);
                const auto info   = FindResourceW(module, resource.id, MAKEINTRESOURCEW(10));
                const auto data   = info ? LoadResource(module, info) : nullptr;
                resource.bytes    = data ? static_cast<const char *>(LockResource(data)) : nullptr;
                resource.size     = info ? SizeofResource(module, info) : 0;
                if (!resource.bytes || !resource.size)
                        throw std::runtime_error(
                            "Bundled motor runtime is missing; rebuild/install the full application / 内置电机运行库缺失");
                for (DWORD i = 0; i < resource.size; ++i) {
                        hash ^= static_cast<unsigned char>(resource.bytes[i]);
                        hash *= 1099511628211ull;
                }
        }
        wchar_t    local[32768]{};
        const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
        if (!length || length >= 32768)
                throw std::runtime_error("Cannot locate the user runtime cache / 无法定位用户运行库缓存目录");
        std::ostringstream version;
        version << std::hex << hash;
        const auto directory = std::filesystem::path(local) / "ava_tool" / "runtime" / "ava_eth" / version.str();
        std::filesystem::create_directories(directory);
        for (const auto &resource : resources) {
                const auto target  = directory / resource.file;
                auto       matches = [&] {
                        std::error_code ec;
                        if (std::filesystem::file_size(target, ec) != resource.size || ec)
                                return false;
                        std::ifstream     input(target, std::ios::binary);
                        const std::string content((std::istreambuf_iterator<char>(input)), {});
                        return content.size() == resource.size && std::equal(content.begin(), content.end(), resource.bytes);
                };
                if (matches())
                        continue;
                static std::atomic<unsigned> sequence{0};
                auto                         temporary = target;
                temporary += "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(sequence++) + ".tmp";
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output.write(resource.bytes, resource.size);
                output.close();
                if (!output) {
                        std::filesystem::remove(temporary);
                        throw std::runtime_error("Cannot extract bundled motor runtime / 无法释放内置电机运行库");
                }
                if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                        std::filesystem::remove(temporary);
                        if (!matches())
                                throw std::runtime_error("Cannot update bundled motor runtime cache / 无法更新电机运行库缓存");
                }
        }
        const auto result = (directory / "ava_eth_sdk.dll").u8string();
        return std::string(result.begin(), result.end());
#else
        return "libava_eth_sdk.so";
#endif
}

MotorController::~MotorController()
{
        shutdown();
}

void
MotorController::shutdown()
{
        {
                std::lock_guard lock(mutex_);
                quitting_ = true;
                commands_.clear();
        }
        wake_.notify_one();
        if (worker_.joinable())
                worker_.join();
}

void
MotorController::pulse(uint32_t axis)
{
        std::lock_guard lock(mutex_);
        lastPulse_ = Clock::now();
        if (selectedAxis_ != axis)
                state_.history.clear();
        selectedAxis_ = axis;
}

MotorController::State
MotorController::state() const
{
        std::lock_guard lock(mutex_);
        auto            copy = state_;
        copy.busy            = processing_ || !commands_.empty();
        return copy;
}

void
MotorController::post(Command command)
{
        std::lock_guard lock(mutex_);
        if (quitting_)
                return;
        // Stop/disconnect invalidate queued motion requests before being processed
        if (command.action == Action::Stop || command.action == Action::Disconnect)
                commands_.clear();
        if (commands_.size() >= 32)
                return;
        commands_.push_back(std::move(command));
        wake_.notify_one();
}

std::vector<MotorController::Device>
MotorController::networkAdapters()
{
        std::vector<Device> adapters;
#ifdef _WIN32
        auto utf8 = [](const wchar_t *val) {
                if (!val || !*val)
                        return std::string{};
                const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, val, -1, nullptr, 0, nullptr, nullptr);
                if (count <= 0)
                        return std::string{};
                std::string text(count, '\0');
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, val, -1, text.data(), count, nullptr, nullptr);
                text.pop_back();
                return text;
        };
        ULONG                      size = 16384, result = ERROR_BUFFER_OVERFLOW;
        std::vector<unsigned char> buffer;
        for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
                buffer.resize(size);
                result = GetAdaptersAddresses(AF_UNSPEC,
                                              GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                              nullptr,
                                              reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()),
                                              &size);
        }
        if (result == NO_ERROR)
                for (auto *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()); adapter; adapter = adapter->Next)
                        if (adapter->AdapterName) {
                                auto name = utf8(adapter->FriendlyName), description = utf8(adapter->Description);
                                if (name.empty())
                                        name = description;
                                adapters.push_back({std::string("\\Device\\NPF_") + adapter->AdapterName, name, description});
                        }
#endif
        return adapters;
}

void
MotorController::run()
{
        Library                                sdk;
        motor_jlink::Session                   jlink(motor_jlink::portIo());
        AvaEthHandle                           handle      = nullptr;
        uint64_t                               pidSequence = 0;
        std::array<bool, AVA_ETH_MAX_AXES>     leases{};
        std::array<bool, AVA_ETH_MAX_AXES>     initialPositionPending{};
        auto                                   origin = Clock::now(), nextPoll = origin, nextHeartbeat = origin;
        bool                                   pollError = false;
        std::array<uint32_t, AVA_ETH_MAX_AXES> reportedFaults{};
        std::array<bool, AVA_ETH_MAX_AXES>     faultReported{};
        std::array<uint32_t, 8>                reportedPages{};
        auto                                   event = [&](const std::string &text, bool error = false, bool status = true) {
                std::lock_guard lock(mutex_);
                if (status) {
                        state_.message = text;
                        state_.error   = error;
                }
                char stamp[32];
                snprintf(stamp, sizeof(stamp), "[%8.2fs] ", std::chrono::duration<double>(Clock::now() - origin).count());
                state_.events.push_back(stamp + text);
                while (state_.events.size() > 100)
                        state_.events.pop_front();
        };
        auto stop = [&] {
                leases.fill(false);
                if (jlink.connected())
                        jlink.stop();
                if (handle)
                        sdk.check(sdk.stop(handle));
        };
        auto disconnect = [&] {
                bool confirmed = true;
                leases.fill(false);
                if (jlink.connected()) {
                        try {
                                jlink.disconnect();
                        } catch (const std::exception &e) {
                                confirmed = false;
                                event(e.what(), true);
                        }
                }
                if (handle) {
                        const int result = sdk.stop(handle);
                        if (result) {
                                confirmed = false;
                                try {
                                        sdk.check(result);
                                } catch (const std::exception &e) {
                                        event(e.what(), true);
                                }
                        }
                        sdk.close(handle);
                        handle = nullptr;
                }
                std::lock_guard lock(mutex_);
                state_.connected         = false;
                state_.pidPending        = false;
                pidSequence              = 0;
                state_.snapshot          = {};
                state_.diagnostics_valid = false;
                return confirmed;
        };
        for (;;) {
                Command  command;
                bool     hasCommand = false, quitting = false, stale = false;
                uint32_t selected = 0;
                {
                        std::unique_lock lock(mutex_);
                        wake_.wait_until(lock, nextPoll, [&] { return quitting_ || !commands_.empty(); });
                        quitting = quitting_;
                        stale    = Clock::now() - lastPulse_ > std::chrono::milliseconds(200);
                        selected = selectedAxis_;
                        if (!quitting && !commands_.empty()) {
                                command = std::move(commands_.front());
                                commands_.pop_front();
                                processing_ = hasCommand = true;
                        }
                }
                if (quitting)
                        break;
                if (stale && std::any_of(leases.begin(), leases.end(), [](bool val) { return val; })) {
                        try {
                                stop();
                                event("UI heartbeat expired; stopped / 界面心跳过期，已停止");
                        } catch (const std::exception &e) {
                                if (jlink.connected())
                                        disconnect();
                                event(e.what(), true);
                        }
                }
                if (hasCommand) {
                        if (command.action == Action::Connect || command.action == Action::Scan ||
                            command.action == Action::WritePid) {
                                std::lock_guard lock(mutex_);
                                state_.elevation_required = false;
                        }
                        try {
                                if (command.config.backend == motor_jlink::backend && command.action == Action::Connect) {
                                        disconnect();
                                        jlink.connect(command.config.jlink);
                                        const auto feedback = jlink.feedback();
                                        auto       target   = jlink.target();
                                        // A newly attached GUI starts with the measured position, never a saved draft
                                        target.position = feedback.position;
                                        target.velocity = target.current = target.torque = 0;
                                        {
                                                std::lock_guard lock(mutex_);
                                                state_.connected  = true;
                                                state_.backend    = motor_jlink::backend;
                                                state_.info       = {AVA_ETH_ABI,
                                                                     sizeof(AvaEthInfo),
                                                                     AVA_ETH_CAP_HARDWARE | AVA_ETH_CAP_SPECIAL_CMD,
                                                                     1,
                                                                     20000,
                                                                     0,
                                                                     1,
                                                                     0};
                                                state_.targets    = {};
                                                state_.targets[0] = target;
                                                state_.history.clear();
                                                state_.snapshot          = {AVA_ETH_ABI, sizeof(AvaEthSnapshot), 1, 0};
                                                state_.snapshot.axes[0]  = feedback;
                                                state_.diagnostics_valid = false;
                                                ++state_.target_sync_revisions[0];
                                        }
                                        pollError = false;
                                        initialPositionPending.fill(true);
                                        event(
                                            "J-Link 已连接，输出未使能；共享内存指令已关闭，断开后仍保持关闭，复位固件可恢复");
                                } else if (jlink.connected() && command.action != Action::Connect) {
                                        if (command.axis)
                                                throw std::runtime_error("J-Link 当前映射仅支持执行器 0");
                                        if (stale && command.action != Action::Stop && command.action != Action::Disconnect &&
                                            command.action != Action::Disable)
                                                throw std::runtime_error("J-Link 指令取消：界面心跳已过期");
                                        auto       target       = command.target;
                                        const bool seedPosition = initialPositionPending[0] &&
                                                                  (target.mode == 1 || target.mode == -1 || target.mode == 8 ||
                                                                   target.mode == -8 || target.mode == -128);
                                        switch (command.action) {
                                                case Action::Disconnect:
                                                        if (disconnect())
                                                                event("J-Link 电机控制已断开；共享内存指令保持关闭");
                                                        else
                                                                event("J-Link 控制连接已关闭，但失能未确认；请检查电机和探针",
                                                                      true);
                                                        break;
                                                case Action::Stop:
                                                case Action::Disable:
                                                        stop();
                                                        event("J-Link 失能已确认");
                                                        break;
                                                case Action::SetMode:
                                                        target                    = jlink.setMode(target);
                                                        initialPositionPending[0] = false;
                                                        {
                                                                std::lock_guard lock(mutex_);
                                                                state_.targets[0] = target;
                                                                ++state_.mode_revisions[0];
                                                                ++state_.target_sync_revisions[0];
                                                        }
                                                        event("J-Link 模式已确认，目标位置已同步当前位置");
                                                        break;
                                                case Action::Apply:
                                                case Action::Enable:
                                                        if (command.action == Action::Enable &&
                                                            (jlink.feedback().mode != target.mode || seedPosition)) {
                                                                target = jlink.setMode(target);
                                                                std::lock_guard lock(mutex_);
                                                                state_.targets[0] = target;
                                                                ++state_.target_sync_revisions[0];
                                                        }
                                                        jlink.apply(target);
                                                        initialPositionPending[0] = false;
                                                        if (command.action == Action::Enable) {
                                                                jlink.enable();
                                                                jlink.apply(target);
                                                        }
                                                        leases[0] = (jlink.feedback().flags & AVA_ETH_ENABLED) != 0;
                                                        {
                                                                std::lock_guard lock(mutex_);
                                                                state_.targets[0] = target;
                                                        }
                                                        event(command.action == Action::Enable ? "J-Link 使能已确认"
                                                                                               : "J-Link 目标指令已写入");
                                                        break;
                                                case Action::Special:
                                                        if (command.special_cmd == AVA_ETH_SPECIAL_ENABLE) {
                                                                if (jlink.feedback().mode != target.mode || seedPosition)
                                                                        target = jlink.setMode(target);
                                                                jlink.apply(target);
                                                                initialPositionPending[0] = false;
                                                                std::lock_guard lock(mutex_);
                                                                state_.targets[0] = target;
                                                                ++state_.target_sync_revisions[0];
                                                        }
                                                        jlink.special(command.special_cmd);
                                                        leases[0] = command.special_cmd == AVA_ETH_SPECIAL_ENABLE ||
                                                                    command.special_cmd == AVA_ETH_SPECIAL_CALI;
                                                        event("J-Link 特殊指令已写入，执行结果请查看反馈");
                                                        break;
                                                case Action::ResetFault:
                                                        jlink.special(AVA_ETH_SPECIAL_CLEAR_FAULT);
                                                        leases[0] = false;
                                                        event("J-Link 清错指令已写入");
                                                        break;
                                                default:
                                                        throw std::runtime_error("此操作不适用于 J-Link 后端");
                                        }
                                } else
                                        switch (command.action) {
                                                case Action::WritePid: {
                                                        if (!handle)
                                                                throw std::runtime_error("请先连接执行器，再实时配置 PID 参数");
                                                        if (!sdk.set_pid || !sdk.get_pid_status)
                                                                throw std::runtime_error(
                                                                    "运行库不支持实时 PID 配置，请使用新版内置运行库");
                                                        sdk.check(sdk.set_pid(handle,
                                                                              command.axis,
                                                                              command.pidMask,
                                                                              &command.pid,
                                                                              &pidSequence),
                                                                  "PD/PID 在线参数写入");
                                                        {
                                                                std::lock_guard lock(mutex_);
                                                                state_.pidPending  = true;
                                                                state_.pidSentMask = 0;
                                                                state_.pidConfig   = command.config;
                                                                state_.pidAxis     = command.axis;
                                                        }
                                                        event("正在在线更新 PID 参数，保持当前使能状态及运动指令");
                                                        break;
                                                }
                                                case Action::Scan: {
                                                        if (handle)
                                                                throw std::runtime_error(
                                                                    "Disconnect before scanning / 请先断开再扫描");
                                                        sdk.load(command.config.library);
                                                        AvaEthDeviceInfo devices[64]{};
                                                        uint32_t         count = 0;
                                                        if (command.config.backend == AVA_ETH_NPCAP) {
                                                                if (!sdk.enumerate_npcap)
                                                                        throw std::runtime_error(
                                                                            "Update SDK for Npcap / 请使用新版内置运行库");
                                                                sdk.check(sdk.enumerate_npcap(devices, 64, &count));
                                                        } else
                                                                sdk.check(sdk.enumerate(devices, 64, &count));
                                                        if (count > 64)
                                                                throw std::runtime_error("Too many control devices");
                                                        const auto          adapters = command.config.backend == AVA_ETH_NPCAP
                                                                                           ? networkAdapters()
                                                                                           : std::vector<Device>{};
                                                        std::vector<Device> namedDevices;
                                                        for (uint32_t i = 0; i < count; ++i) {
                                                                Device item{devices[i].path, devices[i].path, {}};
#ifdef _WIN32
                                                                for (const auto &adapter : adapters)
                                                                        if (_stricmp(adapter.path.c_str(), item.path.c_str()) ==
                                                                                0 &&
                                                                            !adapter.name.empty()) {
                                                                                item.name        = adapter.name;
                                                                                item.description = adapter.description;
                                                                                break;
                                                                        }
#endif
                                                                namedDevices.push_back(std::move(item));
                                                        }
                                                        {
                                                                std::lock_guard lock(mutex_);
                                                                state_.devices = std::move(namedDevices);
                                                        }
                                                        event("Scan complete / 扫描完成: " + std::to_string(count));
                                                        break;
                                                }
                                                case Action::Connect: {
                                                        disconnect();
                                                        sdk.load(command.config.library);
                                                        const auto       &c = command.config;
                                                        AvaEthOpenOptions options{sizeof(options),
                                                                                  c.backend,
                                                                                  c.axes,
                                                                                  c.period_us,
                                                                                  c.watchdog_ms,
                                                                                  c.relaxed ? AVA_ETH_RELAXED_RECEIVE : 0u,
                                                                                  c.device.empty() ? nullptr
                                                                                                   : c.device.c_str()};
                                                        sdk.check(sdk.open(&options, &handle));
                                                        AvaEthInfo info{};
                                                        sdk.check(sdk.get_info(handle, &info));
                                                        if (info.version != AVA_ETH_ABI || info.size != sizeof(info) ||
                                                            !info.axes || info.axes > 64)
                                                                throw std::runtime_error("Unsupported SDK ABI");
                                                        {
                                                                std::lock_guard lock(mutex_);
                                                                state_.connected   = true;
                                                                state_.pidSentMask = 0;
                                                                state_.backend     = c.backend;
                                                                state_.info        = info;
                                                                if (!sdk.set_special_cmd)
                                                                        state_.info.capabilities &= ~AVA_ETH_CAP_SPECIAL_CMD;
                                                                state_.targets = {};
                                                                state_.history.clear();
                                                                state_.snapshot    = {};
                                                                state_.diagnostics = {};
                                                                for (uint32_t i = 0; i < info.axes; ++i)
                                                                        state_.targets[i].axis = i;
                                                        }
                                                        pollError = false;
                                                        initialPositionPending.fill(true);
                                                        faultReported.fill(false);
                                                        reportedPages.fill(0);
                                                        event("Connected; outputs disabled / 已连接，输出未使能");
                                                        break;
                                                }
                                                case Action::Disconnect: {
                                                        const bool wasConnected = handle != nullptr;
                                                        if (disconnect() && wasConnected)
                                                                event("Disconnected / 已断开" + (command.reason.empty()
                                                                                                     ? std::string()
                                                                                                     : ": " + command.reason));
                                                        break;
                                                }
                                                case Action::Stop:
                                                        stop();
                                                        event("Stop confirmed / 停止通信已确认");
                                                        break;
                                                default: {
                                                        if (!handle)
                                                                throw std::runtime_error("Not connected / 未连接");
                                                        if (command.axis >= AVA_ETH_MAX_AXES)
                                                                throw std::runtime_error("Invalid axis");
                                                        if (command.action == Action::SetMode) {
                                                                if (stale || command.target.axis != command.axis)
                                                                        throw std::runtime_error(
                                                                            "模式切换已取消：界面心跳过期或执行器不匹配");
                                                                AvaEthSnapshot fresh{};
                                                                sdk.check(sdk.get_snapshot(handle, &fresh));
                                                                if (command.axis >= fresh.count)
                                                                        throw std::runtime_error("Invalid axis");
                                                                const auto &f = fresh.axes[command.axis];
                                                                if ((f.flags & 3u) != 3u || (f.flags & AVA_ETH_FAULT) ||
                                                                    f.special_cmd)
                                                                        throw std::runtime_error("模式切换已取消：反馈无效、存"
                                                                                                 "在故障或特殊指令仍生效");
                                                                const bool enabled = (f.flags & AVA_ETH_ENABLED) != 0;
                                                                auto       target  = command.target;
                                                                if (target.mode == 1 || target.mode == -1 || target.mode == 8 ||
                                                                    target.mode == -8 || target.mode == -128)
                                                                        target.position = f.position;
                                                                target.velocity = target.current = target.torque = 0;
                                                                AvaEthInterpolation cfg{AVA_ETH_ABI,
                                                                                        sizeof(cfg),
                                                                                        command.axis,
                                                                                        command.interpolate ? 1u : 0u,
                                                                                        command.max_velocity,
                                                                                        command.max_acceleration};
                                                                const int           switched =
                                                                    sdk.switch_mode ? sdk.switch_mode(handle, &target, &cfg)
                                                                                              : AVA_ETH_UNSUPPORTED;
                                                                if (switched == AVA_ETH_UNSUPPORTED) {
                                                                        // Older/kernel runtimes cannot switch atomically
                                                                        // Restore the previous enable state after their
                                                                        // required disable
                                                                        if (enabled)
                                                                                sdk.check(sdk.disable(handle, command.axis));
                                                                        if (sdk.set_interpolation)
                                                                                sdk.check(sdk.set_interpolation(
                                                                                    handle,
                                                                                    command.axis,
                                                                                    cfg.enabled,
                                                                                    cfg.max_velocity,
                                                                                    cfg.max_acceleration));
                                                                        else if (cfg.enabled)
                                                                                throw std::runtime_error(
                                                                                    "位置插补需要更新 SDK");
                                                                        sdk.check(sdk.set_targets(handle, &target, 1, nullptr));
                                                                        if (enabled)
                                                                                sdk.check(sdk.enable(handle, command.axis),
                                                                                          "切换模式后恢复使能失败");
                                                                } else
                                                                        sdk.check(switched, "切换模式失败");
                                                                leases[command.axis] = enabled;
                                                                {
                                                                        std::lock_guard lock(mutex_);
                                                                        state_.targets[command.axis] = target;
                                                                        ++state_.mode_revisions[command.axis];
                                                                        ++state_.target_sync_revisions[command.axis];
                                                                }
                                                                char detail[384];
                                                                snprintf(
                                                                    detail,
                                                                    sizeof(detail),
                                                                    "模式请求已处理：%d → %d；初始目标=%.9g "
                                                                    "rad，切换前位置=%.9g rad，"
                                                                    "切换前速度=%.6g rad/s；保持原有使能状态。实际模式见反馈。",
                                                                    f.mode,
                                                                    target.mode,
                                                                    target.position,
                                                                    f.position,
                                                                    f.velocity);
                                                                event(detail);
                                                                break;
                                                        }
                                                        const bool specialEnable =
                                                            command.action == Action::Special &&
                                                            command.special_cmd == AVA_ETH_SPECIAL_ENABLE;
                                                        if (command.action == Action::Special) {
                                                                if (stale)
                                                                        throw std::runtime_error(
                                                                            "UI heartbeat expired / 界面心跳已超时");
                                                                AvaEthInfo info{};
                                                                sdk.check(sdk.get_info(handle, &info));
                                                                if (!sdk.set_special_cmd ||
                                                                    !(info.capabilities & AVA_ETH_CAP_SPECIAL_CMD))
                                                                        throw std::runtime_error(
                                                                            "Special commands unavailable / "
                                                                            "当前后端或驱动不支持特殊指令");
                                                        }
                                                        if (command.action == Action::Enable ||
                                                            command.action == Action::Apply || specialEnable) {
                                                                if (stale)
                                                                        throw std::runtime_error("UI heartbeat expired");
                                                                if (command.target.axis != command.axis)
                                                                        throw std::runtime_error("Target axis mismatch");
                                                                if (command.action == Action::Enable || specialEnable) {
                                                                        AvaEthSnapshot fresh{};
                                                                        sdk.check(sdk.get_snapshot(handle, &fresh));
                                                                        if (command.axis >= fresh.count ||
                                                                            (fresh.axes[command.axis].flags &
                                                                             (AVA_ETH_ONLINE | AVA_ETH_FEEDBACK_VALID)) != 3u ||
                                                                            (fresh.axes[command.axis].flags &
                                                                             (AVA_ETH_ENABLED | AVA_ETH_FAULT)))
                                                                                throw std::runtime_error(
                                                                                    "Actuator is not ready for "
                                                                                    "enable / 执行器尚未就绪");
                                                                }
                                                                AvaEthSnapshot current{};
                                                                sdk.check(sdk.get_snapshot(handle, &current));
                                                                uint32_t capabilities;
                                                                bool     initializePosition = false;
                                                                {
                                                                        std::lock_guard lock(mutex_);
                                                                        capabilities   = state_.info.capabilities;
                                                                        const int mode = command.target.mode;
                                                                        initializePosition =
                                                                            (command.action == Action::Enable ||
                                                                             specialEnable) &&
                                                                            (mode == 1 || mode == -1 || mode == 8 ||
                                                                             mode == -8 || mode == -128) &&
                                                                            state_.targets[command.axis].mode != mode;
                                                                }
                                                                if (command.interpolate &&
                                                                    (!(capabilities & AVA_ETH_CAP_INTERPOLATION) ||
                                                                     !sdk.set_interpolation))
                                                                        throw std::runtime_error(
                                                                            "Update ava_eth SDK / kernel driver for position "
                                                                            "interpolation / 位置插补需要更新 SDK 和内核驱动");
                                                                if (command.axis >= current.count)
                                                                        throw std::runtime_error("Invalid axis");
                                                                if (!(current.axes[command.axis].flags & AVA_ETH_ENABLED) &&
                                                                    (capabilities & AVA_ETH_CAP_INTERPOLATION) &&
                                                                    sdk.set_interpolation)
                                                                        sdk.check(
                                                                            sdk.set_interpolation(handle,
                                                                                                  command.axis,
                                                                                                  command.interpolate ? 1u : 0u,
                                                                                                  command.max_velocity,
                                                                                                  command.max_acceleration));
                                                                if (initializePosition) {
                                                                        // The default/loaded mode can first reach the device
                                                                        // through Enable, without any SetMode command. Do not
                                                                        // replay its editor value
                                                                        AvaEthSnapshot fresh{};
                                                                        sdk.check(sdk.get_snapshot(handle, &fresh));
                                                                        if (command.axis >= fresh.count ||
                                                                            (fresh.axes[command.axis].flags & 3u) != 3u ||
                                                                            (fresh.axes[command.axis].flags &
                                                                             (AVA_ETH_FAULT | AVA_ETH_ENABLED)) ||
                                                                            !std::isfinite(fresh.axes[command.axis].position))
                                                                                throw std::runtime_error(
                                                                                    "位置模式初始化失败：无法获取有效的当前位"
                                                                                    "置");
                                                                        command.target.position =
                                                                            fresh.axes[command.axis].position;
                                                                        command.target.velocity   = command.target.current =
                                                                            command.target.torque = 0;
                                                                }
                                                                sdk.check(sdk.set_targets(handle, &command.target, 1, nullptr));
                                                                {
                                                                        std::lock_guard lock(mutex_);
                                                                        state_.targets[command.axis]         = command.target;
                                                                        initialPositionPending[command.axis] = false;
                                                                        if (initializePosition)
                                                                                ++state_.target_sync_revisions[command.axis];
                                                                }
                                                                if (initializePosition) {
                                                                        char detail[192];
                                                                        std::snprintf(
                                                                            detail,
                                                                            sizeof(detail),
                                                                            "位置模式首次使能：执行器 "
                                                                            "%u，mode=%d，目标已同步到最新反馈位置 %.9g rad",
                                                                            command.axis,
                                                                            command.target.mode,
                                                                            command.target.position);
                                                                        event(detail);
                                                                }
                                                                if (command.action == Action::Enable) {
                                                                        sdk.check(sdk.enable(handle, command.axis), "使能失败");
                                                                        leases[command.axis] = true;
                                                                }
                                                        } else if (command.action == Action::Disable) {
                                                                leases[command.axis] = false;
                                                                sdk.check(sdk.disable(handle, command.axis));
                                                        } else if (command.action == Action::ResetFault) {
                                                                leases[command.axis] = false;
                                                                sdk.check(sdk.reset_fault(handle, command.axis));
                                                        }
                                                        if (command.action == Action::Special) {
                                                                sdk.check(sdk.set_special_cmd(
                                                                    handle, command.axis, command.special_cmd));
                                                                leases[command.axis] =
                                                                    command.special_cmd == AVA_ETH_SPECIAL_ENABLE ||
                                                                    command.special_cmd == AVA_ETH_SPECIAL_CALI;
                                                                event("special_cmd=" + std::to_string(command.special_cmd) +
                                                                      " PDO 已确认；执行结果请查看反馈和故障详情，执行器 " +
                                                                      std::to_string(command.axis));
                                                        } else {
                                                                const char *operation =
                                                                    command.action == Action::Enable    ? "使能"
                                                                    : command.action == Action::Disable ? "失能"
                                                                    : command.action == Action::Apply   ? "目标指令"
                                                                                                        : "故障复位";
                                                                event(std::string(operation) + "已确认：执行器 " +
                                                                      std::to_string(command.axis));
                                                        }
                                                        break;
                                                }
                                        }
                        } catch (const std::exception &e) {
                                const std::string message = e.what();
                                const auto       *failure = dynamic_cast<const SdkFailure *>(&e);
                                if ((command.action == Action::Connect || command.action == Action::Scan ||
                                     command.action == Action::WritePid) &&
                                    failure && failure->permissionDenied) {
                                        std::lock_guard lock(mutex_);
                                        state_.elevation_required = true;
                                        ++state_.elevation_revision;
                                }
                                if (command.action == Action::Connect)
                                        disconnect();
                                // A rejected request never leaves an earlier active request renewed
                                if ((handle || jlink.connected()) && command.action != Action::WritePid) {
                                        try {
                                                stop();
                                        } catch (const std::exception &stopError) {
                                                if (jlink.connected())
                                                        disconnect();
                                                event(stopError.what(), true);
                                        }
                                }
                                event(message, true);
                                if (command.action == Action::Connect && command.config.backend == AVA_ETH_DEVICE_HARDWARE &&
                                    message.find("Windows error=2") != std::string::npos)
                                        event("Kernel DeviceIoControl error 2: re-scan the device and check driver version / "
                                              "adapter binding, or select Npcap / 内核设备调用错误 "
                                              "2：请重新扫描设备，核对驱动版本和网卡绑定，或切换 Npcap 后端",
                                              true);
                        }
                        {
                                std::lock_guard lock(mutex_);
                                processing_ = false;
                        }
                }
                const auto now = Clock::now();
                if (now >= nextPoll) {
                        nextPoll = now + std::chrono::milliseconds(20);
                        if (jlink.connected()) {
                                try {
                                        const auto feedback = jlink.feedback();
                                        if (leases[0] && (feedback.flags & AVA_ETH_FAULT)) {
                                                stop();
                                                event("J-Link 驱动故障，已请求失能", true);
                                        }
                                        {
                                                std::lock_guard lock(mutex_);
                                                state_.snapshot         = {AVA_ETH_ABI, sizeof(AvaEthSnapshot), 1, 0};
                                                state_.snapshot.axes[0] = feedback;
                                                if (selected == 0 && selectedAxis_ == 0) {
                                                        state_.history.push_back(
                                                            {std::chrono::duration<double>(now - origin).count(),
                                                             feedback,
                                                             state_.targets[0]});
                                                        while (state_.history.size() > 1500)
                                                                state_.history.pop_front();
                                                }
                                        }
                                } catch (const std::exception &e) {
                                        const std::string error = e.what();
                                        disconnect();
                                        event(error + "；控制连接已关闭，请检查探针和变量映射后重新连接。USB "
                                                      "中断时无法保证失能写入成功。",
                                              true);
                                }
                                continue;
                        }
                        if (!handle)
                                continue;
                        try {
                                if (pidSequence) {
                                        AvaEthPidStatus status{};
                                        sdk.check(sdk.get_pid_status(handle, &status));
                                        if (status.sequence == pidSequence && status.state != AVA_ETH_PID_PENDING) {
                                                {
                                                        std::lock_guard lock(mutex_);
                                                        state_.pidPending = false;
                                                        if (status.state == AVA_ETH_PID_SENT) {
                                                                state_.pid         = status.values;
                                                                state_.pidSentMask = status.mask;
                                                                ++state_.pidRevision;
                                                        }
                                                }
                                                pidSequence = 0;
                                                event(status.message, status.state != AVA_ETH_PID_SENT);
                                        }
                                }
                                AvaEthSnapshot feedback{};
                                sdk.check(sdk.get_snapshot(handle, &feedback));
                                if (feedback.version != AVA_ETH_ABI || feedback.size != sizeof(feedback) || feedback.count > 64)
                                        throw std::runtime_error("Invalid feedback ABI");
                                AvaEthDiagnostics diagnostics{};
                                const int         result = sdk.get_diagnostics(handle, &diagnostics);
                                if (result != AVA_ETH_UNSUPPORTED)
                                        sdk.check(result);
                                if (result == AVA_ETH_OK &&
                                    !std::equal(reportedPages.begin(), reportedPages.end(), diagnostics.error_pages)) {
                                        std::string detail  = "Firmware error pages changed / 固件故障与警告变化";
                                        const auto  entries = motor_faults::decode(diagnostics.error_pages);
                                        if (entries.empty())
                                                detail += "：当前错误页全部为 0（不代表驱动已经使能）";
                                        for (const auto &entry : entries)
                                                detail += "\n" + motor_faults::describe(entry);
                                        event(detail, false, false);
                                        std::copy(std::begin(diagnostics.error_pages),
                                                  std::end(diagnostics.error_pages),
                                                  reportedPages.begin());
                                }
                                for (uint32_t i = 0; i < feedback.count; ++i) {
                                        const auto flags = feedback.axes[i].flags;
                                        const auto code  = feedback.axes[i].error_code;
                                        if (flags & AVA_ETH_FAULT) {
                                                if (!faultReported[i] || reportedFaults[i] != code) {
                                                        const char *reason = "Slave fault / 从站故障";
                                                        switch (code) {
                                                                case AVA_ETH_OFFLINE:
                                                                        reason = "Communication offline / 通信已停止或设备离线";
                                                                        break;
                                                                case AVA_ETH_ERR_DRIVE_FAULT:
                                                                        reason = "Drive CiA402 fault / 驱动 CiA402 故障";
                                                                        break;
                                                                case AVA_ETH_ERR_RX_TIMEOUT:
                                                                        reason = "PDO receive timeout / PDO 接收超时";
                                                                        break;
                                                                case AVA_ETH_ERR_WKC:
                                                                        reason = "PDO WKC mismatch / PDO WKC 异常";
                                                                        break;
                                                                case AVA_ETH_ERR_AL:
                                                                        reason = "Slave AL state error / 从站 AL 状态异常";
                                                                        break;
                                                                case AVA_ETH_ERR_SEND:
                                                                        reason = "PDO send error / PDO 发送失败";
                                                                        break;
                                                                case AVA_ETH_ERR_DATA:
                                                                        reason = "Invalid PDO data / PDO 数据异常";
                                                                        break;
                                                        }
                                                        char details[768];
                                                        snprintf(details,
                                                                 sizeof(details),
                                                                 "%s: 执行器=%u error=0x%08X flags=0x%X; budget=%u us "
                                                                 "max_rtt=%.1f us timeouts=%llu WKC=%u/%u "
                                                                 "late=%llu unmatched=%llu missed_periods=%llu%s",
                                                                 reason,
                                                                 i,
                                                                 code,
                                                                 flags,
                                                                 diagnostics.receive_timeout_us,
                                                                 diagnostics.max_rtt_ns / 1000.0,
                                                                 (unsigned long long)diagnostics.timeouts,
                                                                 diagnostics.wkc,
                                                                 diagnostics.expected_wkc,
                                                                 (unsigned long long)diagnostics.late,
                                                                 (unsigned long long)diagnostics.unmatched,
                                                                 (unsigned long long)diagnostics.missed_periods,
                                                                 code == AVA_ETH_ERR_RX_TIMEOUT
                                                                     ? "；max_rtt "
                                                                       "仅统计成功回包，超时帧未计入。请检查网卡中断调整、节能"
                                                                       "设置及系统负载。"
                                                                     : "");
                                                        event(details, true);
                                                        faultReported[i]  = true;
                                                        reportedFaults[i] = code;
                                                }
                                        } else
                                                faultReported[i] = false;
                                        if (leases[i] &&
                                            ((flags & 3u) != 3u || (flags & (AVA_ETH_FAULT | AVA_ETH_COMMAND_EXPIRED)))) {
                                                stop();
                                                if (!(flags & AVA_ETH_FAULT))
                                                        event("Feedback lost or command expired; stopped / "
                                                              "反馈失效或命令过期，已停止",
                                                              true);
                                                break;
                                        }
                                }
                                if (!stale && now >= nextHeartbeat) {
                                        nextHeartbeat = now + std::chrono::milliseconds(50);
                                        for (uint32_t i = 0; i < feedback.count; ++i)
                                                if (leases[i])
                                                        sdk.check(sdk.keepalive(handle, i));
                                }
                                {
                                        std::lock_guard lock(mutex_);
                                        state_.snapshot = feedback;
                                        for (uint32_t i = 0; i < feedback.count; ++i) {
                                                const auto &f = feedback.axes[i];
                                                if (initialPositionPending[i] && !state_.targets[i].mode &&
                                                    (f.flags & 3u) == 3u && !(f.flags & AVA_ETH_FAULT) &&
                                                    std::isfinite(f.position)) {
                                                        state_.targets[i].position = f.position;
                                                        ++state_.target_sync_revisions[i];
                                                        initialPositionPending[i] = false;
                                                }
                                        }
                                        state_.diagnostics_valid = result == AVA_ETH_OK;
                                        state_.diagnostics       = diagnostics;
                                        if (selected < feedback.count && selected == selectedAxis_) {
                                                state_.history.push_back({std::chrono::duration<double>(now - origin).count(),
                                                                          feedback.axes[selected],
                                                                          state_.targets[selected]});
                                                while (state_.history.size() > 1500)
                                                        state_.history.pop_front();
                                        }
                                }
                                pollError = false;
                        } catch (const std::exception &e) {
                                if (!pollError) {
                                        try {
                                                stop();
                                        } catch (const std::exception &stopError) {
                                                event(stopError.what(), true);
                                        }
                                        event(e.what(), true);
                                }
                                pollError = true;
                                std::lock_guard lock(mutex_);
                                for (auto &axis : state_.snapshot.axes)
                                        axis.flags &= ~(AVA_ETH_ONLINE | AVA_ETH_FEEDBACK_VALID);
                                state_.diagnostics_valid = false;
                        }
                }
        }
        disconnect();
}
