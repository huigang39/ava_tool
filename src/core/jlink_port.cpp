#include "gui/choice_control.hpp"
#include "gui/dock_close.hpp"
/**
 * @file  jlink_port.cpp
 * @brief JLinkPort implementation — J-Link SDK wrapper.
 */
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
#include <unordered_set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#pragma comment(lib, "version.lib")
#endif

#include "cJSON.h"
#include "core/jlink_download.hpp"
#include "imgui.h"
#include "imgui_internal.h"

#include "app_log.hpp"
#include "core/flash_erase.hpp"
#include "core/jlink_port.hpp"
#include "gui/firmware_status.hpp"
#include "gui/i18n.hpp"
#include "gui/log_view.hpp"
#include "gui/platform_style.hpp"
#include "gui/tutorial_guide.hpp"
#include "gui/ui_theme.hpp"
#include "platform/native_dlg.hpp"

// SEGGER RTT terminal exports are consumed only by ava_tool's J-Link transport
// They were removed from ckit/include/jlink.h, so keep the DLL ABI private here
extern "C" {
int JLINK_RTTERMINAL_Control(std::uint32_t command, void *parameter);
int JLINK_RTTERMINAL_Read(std::uint32_t bufferIndex, char *buffer, std::uint32_t bufferSize);
int JLINK_RTTERMINAL_Write(std::uint32_t bufferIndex, const void *buffer, std::uint32_t bufferSize);
}

static constexpr std::uint32_t JLINKARM_RTTERMINAL_CMD_START      = 0u;
static constexpr std::uint32_t JLINKARM_RTTERMINAL_CMD_STOP       = 1u;
static constexpr std::uint32_t JLINKARM_RTTERMINAL_CMD_GETNUMBUF  = 3u;
static constexpr std::uint32_t JLINKARM_RTTERMINAL_DIRECTION_UP   = 0u;
static constexpr std::uint32_t JLINKARM_RTTERMINAL_DIRECTION_DOWN = 1u;

#ifdef _WIN32
namespace
{
struct JLinkRuntimeApi {
        HMODULE                                   dll{};
        std::string                               loadedPath;
        decltype(&::JLINKARM_Open)                Open{};
        decltype(&::JLINKARM_Close)               Close{};
        decltype(&::JLINKARM_IsOpen)              IsOpen{};
        decltype(&::JLINKARM_Connect)             Connect{};
        decltype(&::JLINKARM_ReadMemEx)           ReadMemEx{};
        decltype(&::JLINKARM_WriteMemEx)          WriteMemEx{};
        decltype(&::JLINKARM_ExecCommand)         ExecCommand{};
        decltype(&::JLINKARM_TIF_Select)          TIF_Select{};
        decltype(&::JLINKARM_SetSpeed)            SetSpeed{};
        decltype(&::JLINKARM_Reset)               Reset{};
        decltype(&::JLINKARM_Go)                  Go{};
        decltype(&::JLINKARM_Halt)                Halt{};
        decltype(&::JLINKARM_IsHalted)            IsHalted{};
        decltype(&::JLINKARM_SetResetType)        SetResetType{};
        decltype(&::JLINK_DownloadFile)           DownloadFile{};
        decltype(&::JLINKARM_BeginDownload)       BeginDownload{};
        decltype(&::JLINKARM_EndDownload)         EndDownload{};
        decltype(&::JLINKARM_WriteVerifyMem)      WriteVerifyMem{};
        decltype(&::JLINK_EraseChip)              EraseChip{};
        decltype(&::JLINKARM_DEVICE_SelectDialog) DEVICE_SelectDialog{};
        decltype(&::JLINKARM_DEVICE_GetInfo)      DEVICE_GetInfo{};
        decltype(&::JLINK_HSS_Start)              HSS_Start{};
        decltype(&::JLINK_HSS_Stop)               HSS_Stop{};
        decltype(&::JLINK_HSS_Read)               HSS_Read{};
        decltype(&::JLINK_RTTERMINAL_Control)     RTTERMINAL_Control{};
        decltype(&::JLINK_RTTERMINAL_Read)        RTTERMINAL_Read{};
        decltype(&::JLINK_RTTERMINAL_Write)       RTTERMINAL_Write{};
        decltype(&::JLINKARM_SWO_Control)         SWO_Control{};
        decltype(&::JLINKARM_SWO_DisableTarget)   SWO_DisableTarget{};
        decltype(&::JLINKARM_SWO_EnableTarget)    SWO_EnableTarget{};
        decltype(&::JLINKARM_SWO_Read)            SWO_Read{};
        int(__cdecl *GetDLLVersion)(){};
        void(__cdecl *SetLogFile)(const char *){}; // Optional SDK diagnostics.

        std::string resolvedPath() const
        {
                if (!dll)
                        return {};
                std::vector<char> path(32768);
                const DWORD       length = GetModuleFileNameA(dll, path.data(), static_cast<DWORD>(path.size()));
                if (length == 0 || length >= path.size())
                        return loadedPath;
                return std::string(path.data(), length);
        }

        bool load(const std::string &path, std::string &error)
        {
                if (dll && loadedPath == path)
                        return true;
                if (dll) {
                        FreeLibrary(dll);
                        *this = {};
                }
#if defined(_M_ARM64)
                dll = path.empty() ? LoadLibraryW(L"JLink_arm64.dll") : LoadLibraryA(path.c_str());
#else
                dll = path.empty() ? LoadLibraryW(L"JLink_x64.dll") : LoadLibraryA(path.c_str());
#endif
                if (!dll) {
                        error = "无法加载 J-Link DLL（Windows 错误码：" + std::to_string(GetLastError()) + "）";
                        return false;
                }
                auto symbol = [&](auto &target, const char *name) {
                        target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(GetProcAddress(dll, name));
                        if (!target && error.empty())
                                error = std::string("J-Link DLL 缺少导出函数：") + name;
                };
                symbol(Open, "JLINKARM_Open");
                symbol(Close, "JLINKARM_Close");
                symbol(IsOpen, "JLINKARM_IsOpen");
                symbol(Connect, "JLINKARM_Connect");
                symbol(ReadMemEx, "JLINKARM_ReadMemEx");
                symbol(WriteMemEx, "JLINKARM_WriteMemEx");
                symbol(ExecCommand, "JLINKARM_ExecCommand");
                symbol(TIF_Select, "JLINKARM_TIF_Select");
                symbol(SetSpeed, "JLINKARM_SetSpeed");
                symbol(Reset, "JLINKARM_Reset");
                symbol(Go, "JLINKARM_Go");
                symbol(Halt, "JLINKARM_Halt");
                symbol(IsHalted, "JLINKARM_IsHalted");
                symbol(SetResetType, "JLINKARM_SetResetType");
                symbol(DownloadFile, "JLINK_DownloadFile");
                symbol(BeginDownload, "JLINKARM_BeginDownload");
                symbol(EndDownload, "JLINKARM_EndDownload");
                symbol(WriteVerifyMem, "JLINKARM_WriteVerifyMem");
                symbol(EraseChip, "JLINK_EraseChip");
                symbol(DEVICE_SelectDialog, "JLINKARM_DEVICE_SelectDialog");
                symbol(DEVICE_GetInfo, "JLINKARM_DEVICE_GetInfo");
                symbol(HSS_Start, "JLINK_HSS_Start");
                symbol(HSS_Stop, "JLINK_HSS_Stop");
                symbol(HSS_Read, "JLINK_HSS_Read");
                symbol(RTTERMINAL_Control, "JLINK_RTTERMINAL_Control");
                symbol(RTTERMINAL_Read, "JLINK_RTTERMINAL_Read");
                symbol(RTTERMINAL_Write, "JLINK_RTTERMINAL_Write");
                symbol(SWO_Control, "JLINKARM_SWO_Control");
                symbol(SWO_DisableTarget, "JLINKARM_SWO_DisableTarget");
                symbol(SWO_EnableTarget, "JLINKARM_SWO_EnableTarget");
                symbol(SWO_Read, "JLINKARM_SWO_Read");
                GetDLLVersion = reinterpret_cast<decltype(GetDLLVersion)>(GetProcAddress(dll, "JLINKARM_GetDLLVersion"));
                SetLogFile    = reinterpret_cast<decltype(SetLogFile)>(GetProcAddress(dll, "JLINKARM_SetLogFile"));
                if (!error.empty()) {
                        FreeLibrary(dll);
                        *this = {};
                        return false;
                }
                loadedPath = path;
                return true;
        }
};

JLinkRuntimeApi g_jlinkApi;
} // namespace

#define JLINKARM_Open                g_jlinkApi.Open
#define JLINKARM_Close               g_jlinkApi.Close
#define JLINKARM_IsOpen              g_jlinkApi.IsOpen
#define JLINKARM_Connect             g_jlinkApi.Connect
#define JLINKARM_ReadMemEx           g_jlinkApi.ReadMemEx
#define JLINKARM_WriteMemEx          g_jlinkApi.WriteMemEx
#define JLINKARM_ExecCommand         g_jlinkApi.ExecCommand
#define JLINKARM_TIF_Select          g_jlinkApi.TIF_Select
#define JLINKARM_SetSpeed            g_jlinkApi.SetSpeed
#define JLINKARM_Reset               g_jlinkApi.Reset
#define JLINKARM_Go                  g_jlinkApi.Go
#define JLINKARM_Halt                g_jlinkApi.Halt
#define JLINKARM_IsHalted            g_jlinkApi.IsHalted
#define JLINKARM_SetResetType        g_jlinkApi.SetResetType
#define JLINK_DownloadFile           g_jlinkApi.DownloadFile
#define JLINKARM_BeginDownload       g_jlinkApi.BeginDownload
#define JLINKARM_EndDownload         g_jlinkApi.EndDownload
#define JLINKARM_WriteVerifyMem      g_jlinkApi.WriteVerifyMem
#define JLINK_EraseChip              g_jlinkApi.EraseChip
#define JLINKARM_DEVICE_SelectDialog g_jlinkApi.DEVICE_SelectDialog
#define JLINKARM_DEVICE_GetInfo      g_jlinkApi.DEVICE_GetInfo
#define JLINK_HSS_Start              g_jlinkApi.HSS_Start
#define JLINK_HSS_Stop               g_jlinkApi.HSS_Stop
#define JLINK_HSS_Read               g_jlinkApi.HSS_Read
#define JLINK_RTTERMINAL_Control     g_jlinkApi.RTTERMINAL_Control
#define JLINK_RTTERMINAL_Read        g_jlinkApi.RTTERMINAL_Read
#define JLINK_RTTERMINAL_Write       g_jlinkApi.RTTERMINAL_Write
#define JLINKARM_SWO_Control         g_jlinkApi.SWO_Control
#define JLINKARM_SWO_DisableTarget   g_jlinkApi.SWO_DisableTarget
#define JLINKARM_SWO_EnableTarget    g_jlinkApi.SWO_EnableTarget
#define JLINKARM_SWO_Read            g_jlinkApi.SWO_Read
#endif

namespace
{
// STM32H7's device-specific OnDisconnectTarget runs before generic debug
// de-init and still resets DBGMCU with SetSkipDebugDeInit=1 (DLL V8.10k).
// Override only that callback, as in SEGGER's STM32H7 example:
// https://kb.segger.com/images/e/e7/Example_SkipDebugDeInit.JLinkScript
bool
configureJlinkDisconnectScript(const std::string &device, std::string &error)
{
        // SDK script selection can outlive Close/Open. Clear only our own override
        // when switching devices; otherwise leave any default script alone.
        static bool selected   = false; // Called with JLinkPort::mtx_ held.
        std::string normalized = device;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
        });
        const bool isH7 = normalized.starts_with("STM32H7");
        if (!isH7 && !selected)
                return true;
        try {
                std::string command = "ScriptFile = \"\"";
                if (isH7) {
                        std::filesystem::path directory = std::filesystem::current_path();
#ifdef _WIN32
                        std::vector<char> executable(32768);
                        const DWORD       length =
                            GetModuleFileNameA(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
                        if (length == 0 || length >= executable.size()) {
                                error = "无法定位 STM32H7 连接脚本目录";
                                return false;
                        }
                        directory = std::filesystem::path(std::string(executable.data(), length)).parent_path();
#endif
                        const auto path = directory / "config" / "STM32H7_LiveAttach.JLinkScript";
                        if (!std::filesystem::is_regular_file(path)) {
                                error = "缺少 STM32H7 连接脚本：" + path.string();
                                return false;
                        }
                        command = "ScriptFile = \"" + path.string() + "\"";
                }
                char response[512]{};
                if (JLINKARM_ExecCommand(command.c_str(), response, sizeof(response)) < 0) {
                        error = std::string("设置 J-Link 断开脚本失败：") + response;
                        return false;
                }
                selected = isH7;
                return true;
        } catch (const std::exception &exception) {
                error = std::string("准备 J-Link 断开脚本失败：") + exception.what();
                return false;
        }
}

// Opt-in diagnostics only. No target writes and no device access before Connect.
// Separate SDK files preserve Close() from the previous session across reconnect.
void
configureJlinkDiagnosticLog()
{
#ifdef _WIN32
        const char *directory = std::getenv("AVA_JLINK_LOG_DIR");
        if (!directory || !*directory || !g_jlinkApi.SetLogFile)
                return;
        try {
                std::error_code error;
                const auto      folder = std::filesystem::absolute(directory, error);
                if (error)
                        return;
                std::filesystem::create_directories(folder, error);
                if (error)
                        return;
                static unsigned sequence = 0;
                const auto      stamp =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                        .count();
                const auto path = folder / ("JLink-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(stamp) +
                                            "-" + std::to_string(++sequence) + ".log");
                g_jlinkApi.SetLogFile(path.string().c_str());
                LOG_I("J-Link SDK diagnostic log: %s", path.string().c_str());
        } catch (const std::exception &error) {
                LOG_W("J-Link diagnostic log unavailable: %s", error.what());
        }
#endif
}

// Caller holds the SDK I/O lock and has a connected, usable session.
// Optional raw RAM snapshot for the firmware currently being diagnosed.
// Unlike debug-register views, these are ordinary application RAM reads.
// The capture launcher supplies the verified address; normal operation adds no reads.
void
captureJlinkTimingState(const char *phase)
{
#ifdef _WIN32
        const char *directory  = std::getenv("AVA_JLINK_LOG_DIR");
        const char *configured = std::getenv("AVA_JLINK_TIMING_ADDRESS");
        if (!directory || !*directory || !configured || !*configured || !g_jlinkApi.ReadMemEx)
                return;
        char *end          = nullptr;
        errno              = 0;
        const auto address = std::strtoull(configured, &end, 0);
        // This diagnostic layout is cnt/code/mask/tick in STM32H745 DTCM RAM.
        if (errno || !end || *end || (address & 3u) || address < 0x20000000u || address > 0x2001FFF0u)
                return;
        try {
                u32        values[4]{};
                const int  result = JLINKARM_ReadMemEx(static_cast<u32>(address), sizeof(values), values, 0);
                const auto stamp =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                        .count();
                std::ofstream out(std::filesystem::path(directory) / "timing-state.log", std::ios::app);
                out << stamp << " pid=" << GetCurrentProcessId() << " " << phase << " address=0x" << std::hex << address
                    << std::dec << " read_bytes=" << result;
                if (result == sizeof(values))
                        out << " cnt=" << values[0] << " code=" << values[1] << " mask=" << values[2] << " tick=" << values[3];
                if (g_jlinkApi.IsHalted)
                        out << " halted=" << int(JLINKARM_IsHalted() != 0);
                const char *heartbeat = std::getenv("AVA_JLINK_HEARTBEAT_ADDRESS");
                if (heartbeat && *heartbeat) {
                        char *heartbeatEnd          = nullptr;
                        errno                       = 0;
                        const auto heartbeatAddress = std::strtoull(heartbeat, &heartbeatEnd, 0);
                        if (!errno && heartbeatEnd && !*heartbeatEnd && !(heartbeatAddress & 3u) &&
                            heartbeatAddress >= 0x20000000u && heartbeatAddress <= 0x2001FFFCu) {
                                u32       counter{};
                                const int heartbeatResult =
                                    JLINKARM_ReadMemEx(static_cast<u32>(heartbeatAddress), sizeof(counter), &counter, 0);
                                out << " heartbeat_read_bytes=" << heartbeatResult;
                                if (heartbeatResult == sizeof(counter))
                                        out << " heartbeat=" << counter;
                        }
                }
                out << "\n";
        } catch (const std::exception &error) {
                LOG_W("J-Link timing snapshot unavailable: %s", error.what());
        }
#else
        (void)phase;
#endif
}

void
stopJlinkHssWithDiagnostics()
{
        captureJlinkTimingState("before-hss-stop");
        JLINK_HSS_Stop();
        captureJlinkTimingState("after-hss-stop");
}

void
captureJlinkClockState(const char *phase)
{
#ifdef _WIN32
        const char *directory = std::getenv("AVA_JLINK_LOG_DIR");
        if (!directory || !*directory || !g_jlinkApi.ReadMemEx)
                return;
        try {
                // These are the SDK debug-register view and may be cached/virtualized.
                // They cannot establish whether the hardware counter is advancing.
                // Never read after Close: that could implicitly attach again.
                constexpr u32 addresses[] = {0xE000EDFCu, 0xE0001000u, 0xE0001004u};
                u32           values[3]{};
                int           results[3]{};
                for (unsigned i = 0; i < 3; ++i)
                        results[i] = JLINKARM_ReadMemEx(addresses[i], sizeof(u32), &values[i], 0);
                const auto stamp =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                        .count();
                std::ofstream out(std::filesystem::path(directory) / "clock-state.log", std::ios::app);
                out << stamp << " pid=" << GetCurrentProcessId() << " " << phase << std::hex << " DEMCR=0x" << values[0]
                    << " DWT_CTRL=0x" << values[1] << " CYCCNT=0x" << values[2] << std::dec << " read_bytes=" << results[0]
                    << "," << results[1] << "," << results[2] << " view=SDK_may_be_cached_or_virtualized\n";
        } catch (const std::exception &error) {
                LOG_W("J-Link clock snapshot unavailable: %s", error.what());
        }
#else
        (void)phase;
#endif
}

int
writeVerifyMemGuarded(const u32 address, const u32 numBytes, const void *data, bool &dllException)
{
        dllException = false;
#ifdef _WIN32
        __try {
                return JLINKARM_WriteVerifyMem(address, numBytes, data);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
                dllException = true;
                return -1;
        }
#else
        return JLINKARM_WriteVerifyMem(address, numBytes, data);
#endif
}
} // namespace

JLinkPort &
JLinkPort::instance()
{
        static JLinkPort s;
        return s;
}

JLinkPort::~JLinkPort()
{
        // Finish UI connection/sharing work before destroying its captured port.
        if (remoteJob_.valid())
                remoteJob_.wait();
        ava_peer::server().stop();
        ava_peer::client().close();
        dllScanCancel_.store(true, std::memory_order_release);
        if (dllScanThread_.joinable())
                dllScanThread_.join();
        swoStop();
        rttStop();
}

std::string
JLinkPort::selectedDllPath() const
{
        std::lock_guard lk(dllMtx_);
        return selectedDllPath_;
}

bool
JLinkPort::open()
{
        auto route = lockRoute();
        if (usingRemote())
                return ava_peer::client().connected();
        std::lock_guard lk(mtx_);
        if (isOpen_)
                return true;

#ifdef _WIN32
        std::string loadError;
        if (!g_jlinkApi.load(selectedDllPath(), loadError)) {
                prevErr_ = std::move(loadError);
                LOG_E("打开 J-Link 失败：%s", prevErr_.c_str());
                return false;
        }
        {
                std::lock_guard dllLock(dllMtx_);
                loadedDllPath_ = g_jlinkApi.resolvedPath();
        }
#endif

        configureJlinkDiagnosticLog();
        const char *err = JLINKARM_Open();
        if (err && err[0]) {
                prevErr_ = std::string("打开 J-Link 失败（SDK 原始信息）：") + err;
                isOpen_  = false;
                // Open() may have created a partial SDK session before failing
                // (notably while a probe firmware update is re-enumerating USB)
                JLINKARM_Close();
                sdkSessionDirty_.store(false, std::memory_order_release);
                LOG_E("%s", prevErr_.c_str());
                return false;
        }
        isOpen_ = JLINKARM_IsOpen() != 0;
        if (isOpen_) {
                const char *directory = std::getenv("AVA_JLINK_LOG_DIR");
                if (directory && *directory) {
                        char response[256]{};
                        (void)JLINKARM_ExecCommand("SetLogVerbose=1", response, sizeof(response));
                }
        }
        if (!isOpen_) {
                prevErr_ = "J-Link 打开操作已返回，但探针仍未打开";
                // Treat this as a partial open as well. Leaving it alive makes
                // every later Open() observe the same stale SDK state
                JLINKARM_Close();
                sdkSessionDirty_.store(false, std::memory_order_release);
                LOG_E("打开 J-Link 失败：探针仍未打开");
        } else {
                sdkSessionDirty_.store(false, std::memory_order_release);
                LOG_I("JLinkPort::open() SUCCEEDED");
        }
        return isOpen_;
}

void
JLinkPort::close()
{
        auto route = lockRoute();
        if (usingRemote()) {
                ava_peer::client().close();
                remoteSampling_ = false;
                targetEpoch_.fetch_add(1);
                return;
        }
        // RTT uses the same J-Link DLL session. Keep its lifecycle lock until the
        // probe is closed so a GUI START cannot race between RTT STOP and Close()
        std::unique_lock rttLifecycle(rttLifecycleMtx_);
        rttStopImpl();
        std::unique_lock swoLifecycle(swoLifecycleMtx_);
        swoStopImpl();

        std::lock_guard lk(mtx_);

        // Flip state flags BEFORE any J-Link API calls so a concurrent isConnected()
        // / isOpen() check from the sampler sees the new state immediately
        const bool wasOpen      = isOpen_;
        const bool wasConnected = isConnected_;
        const bool sessionDirty = sdkSessionDirty_.exchange(false, std::memory_order_acq_rel);
        isConnected_            = false;
        isOpen_                 = false;
        rttStatus_.store(RttStatus::Disconnected, std::memory_order_release);
        firmwareGate_.resetConnection();
        firmwareCheckedAt_ = {};
        {
                std::lock_guard statusLock(firmwareStatusMtx_);
                firmwareStatus_ = {};
        }

        if (hssRunning_) {
                LOG_I("Stopping HSS...");
                // Do not send more commands over a known-dead USB session
                if (!sessionDirty && wasOpen)
                        stopJlinkHssWithDiagnostics();
                hssRunning_   = false;
                hssFrameSize_ = 0;
                hssActualHz_.store(0.0f, std::memory_order_relaxed);
        }
        if (wasOpen || sessionDirty) {
                if (wasConnected && !sessionDirty) {
                        captureJlinkClockState("before-close");
                        captureJlinkTimingState("before-sdk-close");
                }
                LOG_I("Closing JLink connection...");
                // JLINKARM_Close owns the target detach sequence. Manually
                // clearing C_DEBUGEN here leaves some dual-core Cortex-M7
                // targets in a state where the next SWO_EnableTarget cannot
                // resume the core after its temporary halt
                JLINKARM_Close();
                // Give the SDK time to release the USB handle. Without this, an
                // immediately-following Open() sometimes returns a stale handle
                // that can never Connect() until the process is restarted
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                LOG_I("JLink closed.");
        }
        readFailCount_.store(0, std::memory_order_relaxed);
}

bool
JLinkPort::connect(bool resetOnConnect)
{
        auto route = lockRoute();
        if (usingRemote())
                return resetOnConnect ? ava_peer::client().reset() : ava_peer::client().connected();
        ava_peer::WriteGuard writeGuard;
        if (!writeGuard)
                return false;
        std::unique_lock lk(mtx_);
        prevErr_.clear(); // Clear prev error on new attempt

        LOG_I("JLinkPort::connect() attempt");
        if (!isOpen_) {
                prevErr_ = "J-Link 尚未打开";
                LOG_E("连接 J-Link 失败：探针尚未打开");
                return false;
        }

        // On any failure below: tear down the SDK state so the next CONNECT click
        // does a clean Open()+Connect() cycle. Without this, a partial connect
        // leaves the SDK holding a stale USB handle that never recovers — even
        // after the user unplugs and replugs the J-Link — until the app restarts
        auto resetSdk = [&]() {
                JLINKARM_Close();
                sdkSessionDirty_.store(false, std::memory_order_release);
                isOpen_      = false;
                isConnected_ = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
        };

        std::string dev;
        {
                std::lock_guard cfg(cfgMtx_);
                dev = deviceName_;
        }
        const int spdKHz = speedKHz_.load(std::memory_order_relaxed);

        char cmd[160];
        snprintf(cmd, sizeof(cmd), "Device = %s", dev.c_str());
        char ack[256] = {0};
        if (JLINKARM_ExecCommand(cmd, ack, sizeof(ack)) < 0) {
                prevErr_ = std::string("设置 J-Link 目标芯片失败（SDK 原始信息）：") + ack;
                LOG_E("连接 J-Link 失败：设置目标芯片失败（SDK 原始信息）：%s", ack);
                resetSdk();
                return false;
        }
        LOG_I("JLinkPort::connect(): Device set to %s", dev.c_str());

        if (JLINKARM_TIF_Select(JLINKARM_TIF_SWD) < 0) {
                prevErr_ = "选择 SWD 接口失败";
                LOG_E("连接 J-Link 失败：选择 SWD 接口失败");
                resetSdk();
                return false;
        }
        JLINKARM_SetSpeed(static_cast<u32>(spdKHz));
        LOG_I("JLinkPort::connect(): TIF SWD selected, speed %d KHz", spdKHz);

        // Attach to the live control loop without target/RAM initialization,
        // reset retries, or stop-mode memory/RTT access. Reapply each session.
        // https://kb.segger.com/J-Link_Command_Strings
        const char *attachCommands[] = {
            // DLL V8.10k leaves this target halted when restart-on-close is disabled.
            // A monitoring disconnect must leave the application running.
            "SetRestartOnClose=1",
            // Firmware uses DWT->CYCCNT. Retaining debug power alone does not
            // suppress SDK debug-register cleanup (observed with DLL V8.10k).
            // Set both close policies before attaching to the running target.
            "SetSkipDebugDeInit=1",
            "SetDbgPowerDownOnClose=0",
            "ForceAttachTarget=1",
            "InhibitConnectRetries=1",
            "SetAllowStopMode=0",
        };
        for (const char *attachCommand : attachCommands) {
                char response[256] = {0};
                // SetDbgPowerDownOnClose returns its previous value, including +1.
                if (JLINKARM_ExecCommand(attachCommand, response, sizeof(response)) < 0) {
                        prevErr_ = std::string("设置 J-Link 后台连接选项失败：") + attachCommand + "；" + response;
                        LOG_E("%s", prevErr_.c_str());
                        resetSdk();
                        return false;
                }
        }

        if (!configureJlinkDisconnectScript(dev, prevErr_)) {
                LOG_E("%s", prevErr_.c_str());
                resetSdk();
                return false;
        }

        if (JLINKARM_Connect() < 0) {
                prevErr_ = "J-Link 连接失败，请检查目标供电和连接线，或重新插拔 J-Link";
                LOG_E("J-Link 连接失败（JLINKARM_Connect 返回负值），正在重置 SDK 会话");
                resetSdk();
                return false;
        }

        captureJlinkTimingState("after-sdk-connect");
        captureJlinkClockState("after-connect-before-reset-or-sampling");
        captureJlinkTimingState("after-clock-snapshot");

        // Reset only on explicit request. Monitoring may resume a halted CPU,
        // including one left stopped by an older ava_tool disconnect policy.
        if (resetOnConnect) {
                LOG_I("JLinkPort::connect(): Resetting target before starting acquisition...");
                JLINKARM_SetResetType(0);
                const int resetCode = JLINKARM_Reset();
                if (resetCode < 0) {
                        prevErr_ = "J-Link 连接后复位失败，返回值=" + std::to_string(resetCode);
                        LOG_E("%s", prevErr_.c_str());
                        resetSdk();
                        return false;
                }
                const int runCode = JLINKARM_Go();
                if (runCode < 0) {
                        prevErr_ = "J-Link 复位后运行失败，返回值=" + std::to_string(runCode);
                        LOG_E("%s", prevErr_.c_str());
                        resetSdk();
                        return false;
                }
                // Give the MCU time to boot after reset
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } else if (JLINKARM_IsHalted()) {
                LOG_I("J-Link 目标处于暂停状态，继续运行以恢复实时监测（不复位）");
                const int runCode = JLINKARM_Go();
                if (runCode < 0) {
                        prevErr_ = "J-Link 继续运行失败，返回值=" + std::to_string(runCode);
                        LOG_E("%s", prevErr_.c_str());
                        resetSdk();
                        return false;
                }
        }
        if (JLINKARM_IsHalted()) {
                prevErr_ = "目标 CPU 仍处于暂停状态，无法开始实时监测";
                LOG_E("%s", prevErr_.c_str());
                resetSdk();
                return false;
        }
        captureJlinkTimingState("ready-running");

        readFailCount_.store(0, std::memory_order_relaxed);
        sdkSessionDirty_.store(false, std::memory_order_release);
        isConnected_ = true;
        {
                std::lock_guard cfg(cfgMtx_);
                connectedDeviceName_ = dev;
        }
        targetEpoch_.fetch_add(1u, std::memory_order_acq_rel);
        deviceEpoch_.fetch_add(1u, std::memory_order_acq_rel);
        LOG_I("JLinkPort::connect() SUCCEEDED");
        // RTT takes lifecycle -> I/O locks, just like close(). Never start it
        // with the I/O mutex still held. The lifecycle lock also serializes
        // this resume request against an explicit STOP from another thread
        lk.unlock();
        rttStart(true);
        return true;
}

bool
JLinkPort::resetTarget(u64 epoch)
{
        auto route = lockRoute();
        if (usingRemote()) {
                bool ok = ava_peer::client().reset();
                if (ok)
                        targetEpoch_.fetch_add(1);
                return ok;
        }
        ava_peer::WriteGuard writeGuard;
        if (!writeGuard)
                return false;
        std::lock_guard lk(mtx_);
        if (epoch && epoch != targetEpoch_)
                return false;
        if (!isOpen_ || !isConnected_)
                return false;
        LOG_I("JLinkPort::resetTarget(u64 epoch) requested");

        // Use normal reset (not halting)
        JLINKARM_SetResetType(0);
        JLINKARM_Reset();
        JLINKARM_Go();
        // Let C runtime initialization clear/initialize volatile DTCM control
        // blocks before clients observe the new epoch and restore configuration
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        targetEpoch_.fetch_add(1u, std::memory_order_acq_rel);
        deviceEpoch_.fetch_add(1u, std::memory_order_acq_rel);

        return true;
}

namespace
{
struct ElfFlashSegment {
        u32             address{};
        std::vector<u8> data;
};

u16
readLe16(const std::vector<u8> &data, size_t offset)
{
        return static_cast<u16>(data[offset]) | (static_cast<u16>(data[offset + 1]) << 8u);
}

u32
readLe32(const std::vector<u8> &data, size_t offset)
{
        return static_cast<u32>(data[offset]) | (static_cast<u32>(data[offset + 1]) << 8u) |
               (static_cast<u32>(data[offset + 2]) << 16u) | (static_cast<u32>(data[offset + 3]) << 24u);
}

u64
readLe64(const std::vector<u8> &data, size_t offset)
{
        return static_cast<u64>(readLe32(data, offset)) | (static_cast<u64>(readLe32(data, offset + 4)) << 32u);
}

bool
loadElfFlashSegments(const std::string &path, std::vector<ElfFlashSegment> &segments, std::string &error)
{
        std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
        if (!input) {
                error = "无法打开 ELF/AXF 文件";
                return false;
        }
        input.seekg(0, std::ios::end);
        const std::streamoff length = input.tellg();
        input.seekg(0, std::ios::beg);
        if (length < 52 || static_cast<u64>(length) > std::numeric_limits<size_t>::max()) {
                error = "ELF/AXF 文件大小无效";
                return false;
        }
        std::vector<u8> file(static_cast<size_t>(length));
        input.read(reinterpret_cast<char *>(file.data()), length);
        if (!input || file[0] != 0x7Fu || file[1] != 'E' || file[2] != 'L' || file[3] != 'F') {
                error = "所选 AXF/ELF 文件缺少有效的 ELF 文件头";
                return false;
        }
        const u8 elfClass = file[4];
        if ((elfClass != 1 && elfClass != 2) || file[5] != 1) {
                error = "仅支持小端 ELF32/ELF64 固件";
                return false;
        }

        const size_t minHeader = elfClass == 1 ? 52u : 64u;
        if (file.size() < minHeader) {
                error = "ELF 文件头不完整";
                return false;
        }
        const u64    phOffset       = elfClass == 1 ? readLe32(file, 28) : readLe64(file, 32);
        const u16    phSize         = readLe16(file, elfClass == 1 ? 42 : 54);
        const u16    phCount        = readLe16(file, elfClass == 1 ? 44 : 56);
        const size_t requiredPhSize = elfClass == 1 ? 32u : 56u;
        if (phSize < requiredPhSize || phOffset > file.size() ||
            static_cast<u64>(phCount) > (file.size() - static_cast<size_t>(phOffset)) / phSize) {
                error = "ELF 程序头表无效";
                return false;
        }

        for (u16 i = 0; i < phCount; ++i) {
                const size_t ph = static_cast<size_t>(phOffset) + static_cast<size_t>(i) * phSize;
                if (readLe32(file, ph) != 1u) // PT_LOAD
                        continue;
                const u64 fileOffset = elfClass == 1 ? readLe32(file, ph + 4) : readLe64(file, ph + 8);
                const u64 vaddr      = elfClass == 1 ? readLe32(file, ph + 8) : readLe64(file, ph + 16);
                const u64 paddr      = elfClass == 1 ? readLe32(file, ph + 12) : readLe64(file, ph + 24);
                const u64 fileSize   = elfClass == 1 ? readLe32(file, ph + 16) : readLe64(file, ph + 32);
                if (fileSize == 0)
                        continue;
                const u64 address = paddr != 0 ? paddr : vaddr;
                if (address > std::numeric_limits<u32>::max() || fileSize > std::numeric_limits<u32>::max() ||
                    fileOffset > file.size() || fileSize > file.size() - static_cast<size_t>(fileOffset)) {
                        error = "ELF 包含不支持或不完整的加载段";
                        return false;
                }
                ElfFlashSegment segment;
                segment.address = static_cast<u32>(address);
                segment.data.assign(file.begin() + static_cast<size_t>(fileOffset),
                                    file.begin() + static_cast<size_t>(fileOffset + fileSize));
                segments.push_back(std::move(segment));
        }
        if (segments.empty()) {
                error = "ELF/AXF 中没有可加载的段";
                return false;
        }
        return true;
}
} // namespace

bool
JLinkPort::programFile(const std::string                &path,
                       const u32                         binAddress,
                       const bool                        eraseChip,
                       const bool                        resetAfter,
                       std::string                      &error,
                       const std::function<void(float)> &onProgress)
{
        auto route = lockRoute();
        if (usingRemote()) {
                error = "远端未提供烧录能力";
                return false;
        }
        ava_peer::WriteGuard writeGuard;
        if (!writeGuard) {
                error = "设备正由远端控制";
                return false;
        }
        error.clear();
        std::string extension = std::filesystem::u8path(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
        });
        const bool isElf = extension == ".elf" || extension == ".axf";
        if (!isElf && extension != ".hex" && extension != ".bin") {
                error = "支持的固件格式为 AXF、ELF、HEX 和 BIN";
                return false;
        }

        std::vector<ElfFlashSegment> segments;
        if (isElf && !loadElfFlashSegments(path, segments, error))
                return false;

        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_) {
                error = "J-Link 未连接";
                return false;
        }
        auto fail = [&](const std::string &message) {
                error    = message;
                prevErr_ = message;
                LOG_E("J-Link 烧录失败：%s", message.c_str());
                return false;
        };

        if (onProgress)
                onProgress(0.02f);
        // Even a failed/partial download can change the firmware. Invalidate
        // parameter snapshots before the first erase/write, including no-reset downloads
        targetEpoch_.fetch_add(1u, std::memory_order_acq_rel);
        deviceEpoch_.fetch_add(1u, std::memory_order_acq_rel);
        if (eraseChip) {
                JLINKARM_SetResetType(0);
                const int resetCode = JLINKARM_Reset();
                const int haltCode  = JLINKARM_Halt();
                if (resetCode < 0 || haltCode < 0 || !JLINKARM_IsHalted())
                        return fail("擦除准备失败：复位返回值=" + std::to_string(resetCode) +
                                    "，暂停返回值=" + std::to_string(haltCode) + "；目标未进入暂停状态");
                const int eraseCode = JLINK_EraseChip();
                if (eraseCode < 0)
                        return fail("整片擦除失败（J-Link 错误码：" + std::to_string(eraseCode) +
                                    "）。请检查完整 MCU 型号及内核、Flash 保护、复位连接和 SWD 速度。"
                                    "可通过独立擦除面板收集 SEGGER 日志。");
        }
        if (onProgress)
                onProgress(eraseChip ? 0.18f : 0.08f);

        if (isElf) {
                JLINKARM_BeginDownload(0);
                u64 total = 0;
                for (const auto &segment : segments)
                        total += segment.data.size();
                u64 written = 0;
                for (const auto &segment : segments) {
                        size_t offset = 0;
                        while (offset < segment.data.size()) {
                                const u32 chunk = static_cast<u32>(std::min<size_t>(64u * 1024u, segment.data.size() - offset));
                                bool      dllException = false;
                                if (writeVerifyMemGuarded(segment.address + static_cast<u32>(offset),
                                                          chunk,
                                                          segment.data.data() + offset,
                                                          dllException) < 0) {
                                        // After an SEH exception the DLL's download state is unknown;
                                        // calling back into it may raise a second exception
                                        if (!dllException)
                                                JLINKARM_EndDownload();
                                        return fail(dllException ? "J-Link DLL 在烧录时发生异常" : "ELF/AXF 段烧录或校验失败");
                                }
                                offset  += chunk;
                                written += chunk;
                                if (onProgress)
                                        onProgress(0.18f + 0.72f * static_cast<float>(written) / static_cast<float>(total));
                        }
                }
                if (JLINKARM_EndDownload() < 0)
                        return fail("结束 ELF/AXF 烧录失败");
        } else if (JLINK_DownloadFile(path.c_str(), extension == ".bin" ? binAddress : 0u) < 0) {
                return fail("J-Link 无法烧录或校验所选文件");
        }

        if (onProgress)
                onProgress(0.94f);
        if (resetAfter) {
                JLINKARM_SetResetType(0);
                JLINKARM_Reset();
                JLINKARM_Go();
                targetEpoch_.fetch_add(1u, std::memory_order_acq_rel);
                deviceEpoch_.fetch_add(1u, std::memory_order_acq_rel);
        }
        readFailCount_.store(0, std::memory_order_relaxed);
        if (onProgress)
                onProgress(1.0f);
        LOG_I("J-Link flash succeeded: %s", path.c_str());
        return true;
}

bool
JLinkPort::eraseFlash(const std::string &expectedDevice,
                      u64                expectedEpoch,
                      bool               entireChip,
                      uint32_t           sectorMask,
                      int                eraseSpeed,
                      std::string       &error,
                      std::string       &logPath)
{
        auto route = lockRoute();
        if (usingRemote()) {
                error = "远端未提供擦除能力";
                return false;
        }
        ava_peer::WriteGuard writeGuard;
        if (!writeGuard) {
                error = "设备正由远端控制";
                return false;
        }
        bool expected = false;
        if (!busy_.compare_exchange_strong(expected, true)) {
                error = "J-Link 正忙";
                return false;
        }
        struct BusyGuard {
                std::atomic<bool> &flag;
                ~BusyGuard() { flag = false; }
        } busyGuard{busy_};
        try {
#ifdef _WIN32
                const auto script = flasherase::script(expectedDevice, entireChip, sectorMask);
                if (expectedDevice.empty() ||
                    expectedDevice.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.()") !=
                        std::string::npos)
                        throw std::runtime_error("请先在 J-Link 中选择完整 MCU 型号");
                if (eraseSpeed < 5 || eraseSpeed > 50000)
                        throw std::runtime_error("SWD 速度无效");
                std::filesystem::path commander;
                uint32_t              serial = 0;
                {
                        std::lock_guard lk(mtx_);
                        std::lock_guard cfg(cfgMtx_);
                        if (!isConnected_ || expectedEpoch != targetEpoch_ || expectedDevice != connectedDeviceName_ ||
                            deviceName_ != expectedDevice)
                                throw std::runtime_error("目标已变化，请重新连接并选择扇区");
                        auto getSerial =
                            reinterpret_cast<uint32_t(__cdecl *)()>(GetProcAddress(g_jlinkApi.dll, "JLINKARM_GetSN"));
                        if (!getSerial || !(serial = getSerial()))
                                throw std::runtime_error("无法识别当前 J-Link 探针的序列号");
                        commander = std::filesystem::u8path(g_jlinkApi.resolvedPath()).parent_path() / "JLink.exe";
                }
                if (!std::filesystem::is_regular_file(commander)) {
                        std::vector<std::filesystem::path> candidates;
                        for (const char *root : {"C:/Program Files/SEGGER", "C:/Program Files (x86)/SEGGER"}) {
                                std::error_code ec;
                                for (const auto &entry : std::filesystem::directory_iterator(root, ec))
                                        if (entry.is_directory() &&
                                            std::filesystem::is_regular_file(entry.path() / "JLink.exe"))
                                                candidates.push_back(entry.path() / "JLink.exe");
                        }
                        std::sort(candidates.rbegin(), candidates.rend());
                        if (candidates.empty())
                                throw std::runtime_error("请安装 SEGGER J-Link 软件包（JLink.exe）以擦除 Flash");
                        commander = candidates.front();
                }
                const auto dir =
                    std::filesystem::temp_directory_path() /
                    ("ava-flash-erase-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
                std::filesystem::create_directories(dir);
                const auto commands = dir / "erase.jlink";
                const auto output   = dir / "commander.log";
                logPath             = output.string();
                std::ofstream commandFile(commands, std::ios::binary);
                commandFile << script;
                commandFile.close();
                if (!commandFile)
                        throw std::runtime_error("无法创建擦除命令文件");
                // Stop background users before handing this exact probe to Commander
                std::unique_lock rttLifecycle(rttLifecycleMtx_);
                rttStopImpl();
                std::unique_lock swoLifecycle(swoLifecycleMtx_);
                swoStopImpl();
                std::lock_guard lk(mtx_);
                {
                        std::lock_guard cfg(cfgMtx_);
                        if (!isConnected_ || expectedEpoch != targetEpoch_ || expectedDevice != deviceName_)
                                throw std::runtime_error("擦除前目标已变化");
                }
                if (hssRunning_) {
                        stopJlinkHssWithDiagnostics();
                        hssRunning_   = false;
                        hssFrameSize_ = 0;
                        hssActualHz_  = 0;
                }
                char response[256]{};
                if (JLINKARM_ExecCommand("SetRestartOnClose = 0", response, sizeof(response)) < 0)
                        throw std::runtime_error("无法设置目标在断开后保持暂停");
                JLINKARM_Halt();
                isConnected_     = false;
                isOpen_          = false;
                sdkSessionDirty_ = false;
                targetEpoch_.fetch_add(1u, std::memory_order_acq_rel);
                deviceEpoch_.fetch_add(1u, std::memory_order_acq_rel);
                JLINKARM_Close();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                const auto quote   = [](const std::wstring &val) { return L"\"" + val + L"\""; };
                const auto device  = std::filesystem::u8path(expectedDevice).wstring();
                auto       command = quote(commander.wstring()) + L" -NoGui 1 -ExitOnError 1 -AutoConnect 1 -USB " +
                               std::to_wstring(serial) + L" -Device " + quote(device) + L" -If SWD -Speed " +
                               std::to_wstring(eraseSpeed) + L" -CommandFile " + quote(commands.wstring());
                SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
                HANDLE              outputHandle = CreateFileW(
                    output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (outputHandle == INVALID_HANDLE_VALUE)
                        throw std::runtime_error("无法创建擦除日志");
                HANDLE inputHandle =
                    CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
                STARTUPINFOW startup{};
                startup.cb          = sizeof(startup);
                startup.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
                startup.wShowWindow = SW_HIDE;
                startup.hStdOutput  = outputHandle;
                startup.hStdError   = outputHandle;
                startup.hStdInput   = inputHandle;
                PROCESS_INFORMATION process{};
                const bool          started     = CreateProcessW(commander.c_str(),
                                                    command.data(),
                                                    nullptr,
                                                    nullptr,
                                                    TRUE,
                                                    CREATE_NO_WINDOW,
                                                    nullptr,
                                                    commander.parent_path().c_str(),
                                                    &startup,
                                                    &process) != FALSE;
                const DWORD         launchError = started ? 0 : GetLastError();
                CloseHandle(outputHandle);
                if (inputHandle != INVALID_HANDLE_VALUE)
                        CloseHandle(inputHandle);
                if (!started)
                        throw std::runtime_error("无法启动 SEGGER Commander（Windows 错误码：" + std::to_string(launchError) +
                                                 "）");
                CloseHandle(process.hThread);
                const auto wait     = WaitForSingleObject(process.hProcess, 300000);
                DWORD      exitCode = 1;
                if (wait == WAIT_OBJECT_0)
                        GetExitCodeProcess(process.hProcess, &exitCode);
                else {
                        TerminateProcess(process.hProcess, 1);
                        WaitForSingleObject(process.hProcess, 2000);
                }
                CloseHandle(process.hProcess);
                std::ifstream     log(output, std::ios::binary);
                const std::string text((std::istreambuf_iterator<char>(log)), {});
                std::string       lower = text;
                std::transform(
                    lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                size_t completed = 0, at = 0;
                while ((at = lower.find("erasing done", at)) != std::string::npos) {
                        ++completed;
                        at += 12;
                }
                size_t required = entireChip ? 1 : 0;
                if (!entireChip)
                        for (unsigned i = 0; i < 16; ++i)
                                required += (sectorMask >> i) & 1u;
                if (wait != WAIT_OBJECT_0 || exitCode || completed < required) {
                        error              = wait != WAIT_OBJECT_0 ? "擦除超时，目标状态未确认"
                                                                   : "擦除失败（Commander 错误码：" + std::to_string(exitCode) + "）";
                        const auto failure = lower.find("failed");
                        if (failure != std::string::npos) {
                                const auto begin  = text.rfind('\n', failure);
                                error            += "\nCommander 原始信息：" +
                                         text.substr(begin == std::string::npos ? 0 : begin + 1,
                                                     text.find('\n', failure) - (begin == std::string::npos ? 0 : begin + 1));
                        }
                        error    += "\n日志：" + logPath;
                        prevErr_  = error;
                        LOG_E("%s", error.c_str());
                        return false;
                }
                prevErr_.clear();
                LOG_I("Flash erase completed: device=%s probe=%u speed=%d log=%s",
                      expectedDevice.c_str(),
                      serial,
                      eraseSpeed,
                      logPath.c_str());
                return true;
#else
                throw std::runtime_error("Flash 擦除功能需要 Windows 系统");
#endif
        } catch (const std::exception &e) {
                error = std::string("Flash 擦除失败：") + e.what();
                return false;
        }
}

void
JLinkPort::connectAsync(bool resetOnConnect)
{
        bool expected = false;
        if (!busy_.compare_exchange_strong(expected, true))
                return; // already busy
        std::thread([this, resetOnConnect]() {
                // Always do a full close+open+connect cycle
                // In particular, do not trust isOpen_: transport failures clear
                // the UI state before the SDK's stale USB handle can be closed
                close();

                // A firmware update makes the probe disappear and re-enumerate
                // Give that process one short retry without requiring another
                // click (or an application restart)
                constexpr int kConnectAttempts = 2;
                for (int attempt = 0; attempt < kConnectAttempts; ++attempt) {
                        if (attempt > 0)
                                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        if (open() && connect(resetOnConnect))
                                break;
                        if (attempt + 1 < kConnectAttempts) {
                                LOG_W("J-Link 重连失败，将重置 SDK 会话后重试");
                                close();
                        }
                }
                busy_.store(false, std::memory_order_release);
        }).detach();
}

void
JLinkPort::disconnectAsync()
{
        bool expected = false;
        if (!busy_.compare_exchange_strong(expected, true))
                return;
        std::thread([this]() {
                close();
                busy_.store(false, std::memory_order_release);
        }).detach();
}

void
JLinkPort::resetAsync()
{
        bool expected = false;
        if (!busy_.compare_exchange_strong(expected, true))
                return;
        std::thread([this]() {
                resetTarget();
                busy_.store(false, std::memory_order_release);
        }).detach();
}

void
JLinkPort::setFirmwareSource(uintptr_t owner, std::optional<firmware_match::Image> image, bool loading)
{
        if (usingRemote()) {
                remoteSampling_ = false;
                symbolEpoch_.fetch_add(1);
                targetEpoch_.fetch_add(1);
                return;
        }
        std::lock_guard lock(mtx_);
        firmwareGate_.setSource(owner, std::move(image), loading);
        symbolEpoch_.fetch_add(1u, std::memory_order_acq_rel);
        targetEpoch_.fetch_add(1u, std::memory_order_acq_rel);
        {
                std::lock_guard statusLock(firmwareStatusMtx_);
                firmwareStatus_ = loading ? firmware_match::Status{firmware_match::State::Loading, "AXF is loading"}
                                          : firmware_match::Status{};
        }
        firmwareCheckedAt_ = {};
        if (hssRunning_) {
                stopJlinkHssWithDiagnostics();
                hssRunning_   = false;
                hssFrameSize_ = 0;
        }
        hssActualHz_.store(0.0f, std::memory_order_relaxed);
}

void
JLinkPort::removeFirmwareSource(uintptr_t owner)
{
        if (usingRemote()) {
                remoteSampling_ = false;
                symbolEpoch_.fetch_add(1);
                targetEpoch_.fetch_add(1);
                return;
        }
        std::lock_guard lock(mtx_);
        firmwareGate_.removeSource(owner);
        symbolEpoch_.fetch_add(1u, std::memory_order_acq_rel);
        targetEpoch_.fetch_add(1u, std::memory_order_acq_rel);
        {
                std::lock_guard statusLock(firmwareStatusMtx_);
                firmwareStatus_ = {};
        }
        firmwareCheckedAt_ = {};
        if (hssRunning_) {
                stopJlinkHssWithDiagnostics();
                hssRunning_   = false;
                hssFrameSize_ = 0;
        }
        hssActualHz_.store(0.0f, std::memory_order_relaxed);
}

firmware_match::Status
JLinkPort::firmwareStatus() const
{
        std::lock_guard lock(firmwareStatusMtx_);
        return isConnected_ ? firmwareStatus_ : firmware_match::Status{};
}

bool
JLinkPort::setFirmwareMismatchOverride(bool enabled)
{
        std::lock_guard lock(mtx_);
        if (enabled) {
                if (!isOpen_ || !isConnected_ || busy_)
                        return false;
                firmwareAllowsAccessLocked(true);
        }
        const bool wasForced = firmwareGate_.status().forced;
        if (!firmwareGate_.setMismatchOverride(enabled))
                return false;
        const auto status = firmwareGate_.status();
        {
                std::lock_guard statusLock(firmwareStatusMtx_);
                firmwareStatus_ = status;
        }
        if (wasForced == status.forced)
                return true;
        // 切换放行状态后, 旧请求和采样数据不得继续使用
        targetEpoch_.fetch_add(1u, std::memory_order_acq_rel);
        if (hssRunning_) {
                stopJlinkHssWithDiagnostics();
                hssRunning_   = false;
                hssFrameSize_ = 0;
        }
        hssActualHz_.store(0.0f, std::memory_order_relaxed);
        if (status.forced)
                LOG_W("已强制使用 AXF/ELF: device=%s file=%s", status.deviceId.c_str(), status.detail.c_str());
        else
                LOG_I("已取消强制使用 AXF/ELF");
        return true;
}

bool
JLinkPort::firmwareAllowsAccessLocked(bool refresh)
{
        const auto now       = std::chrono::steady_clock::now();
        const auto epoch     = deviceEpoch_.load(std::memory_order_acquire);
        const bool wasForced = firmwareGate_.status().forced;
        if (firmwareCheckedEpoch_ != epoch) {
                firmwareGate_.resetConnection();
                firmwareCheckedAt_    = {};
                firmwareCheckedEpoch_ = epoch;
        }
        if (!refresh && firmwareCheckedAt_ != std::chrono::steady_clock::time_point{} &&
            now - firmwareCheckedAt_ < std::chrono::milliseconds(100))
                return firmwareGate_.status().allowsAccess();
        const auto previous        = firmwareGate_.status();
        const auto status          = firmwareGate_.check([](uint32_t address, uint32_t size, uint8_t *bytes) {
                return JLINKARM_ReadMemEx(address, size, bytes, 0) == int(size);
        });
        const bool overrideExpired = wasForced && !status.forced;
        if (overrideExpired)
                targetEpoch_.fetch_add(1u, std::memory_order_acq_rel);
        {
                std::lock_guard statusLock(firmwareStatusMtx_);
                firmwareStatus_ = status;
        }
        firmwareCheckedAt_ = now;
        if (status.state != previous.state || status.detail != previous.detail || status.forced != previous.forced) {
                if (!status.allowsAccess())
                        LOG_W("AXF/ELF 访问已暂停: %s", status.detail.c_str());
                else if (status.forced)
                        LOG_W("AXF/ELF 仍在强制使用: %s", status.detail.c_str());
                else if (status.state == firmware_match::State::Unverified)
                        LOG_W("AXF/ELF 身份尚未验证: %s", status.detail.c_str());
                else
                        LOG_I("AXF build verified: %s", status.deviceId.c_str());
        }
        if ((!status.allowsAccess() || overrideExpired) && hssRunning_) {
                stopJlinkHssWithDiagnostics();
                hssRunning_   = false;
                hssFrameSize_ = 0;
                hssActualHz_.store(0.0f, std::memory_order_relaxed);
        }
        return status.allowsAccess();
}

bool
JLinkPort::readMem(const u32 addr, const u32 numBytes, void *dst)
{
        auto route = lockRoute();
        if (usingRemote())
                return ava_peer::client().read(addr, numBytes, dst);
        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_)
                return false;
        if (!firmwareAllowsAccessLocked(false))
                return false;
        bool ok = JLINKARM_ReadMemEx(addr, numBytes, dst, 0) == int(numBytes);
        if (!ok && readFailCount_.load(std::memory_order_relaxed) == 0)
                LOG_E("J-Link 内存读取首次失败：地址=0x%08X，字节数=%u", addr, numBytes);
        recordTransportResultLocked(ok, "readMem");
        return ok;
}

bool
JLinkPort::writeMem(const u32 addr, const u32 numBytes, const void *src)
{
        auto route = lockRoute();
        if (usingRemote())
                return ava_peer::client().write(addr, numBytes, src);
        ava_peer::WriteGuard writeGuard;
        if (!writeGuard)
                return false;
        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_)
                return false;
        if (!firmwareAllowsAccessLocked(true))
                return false;
        const bool ok = JLINKARM_WriteMemEx(addr, numBytes, src, 0) == int(numBytes);
        recordTransportResultLocked(ok, "writeMem");
        return ok;
}

bool
JLinkPort::readMemAtEpoch(const u64 epoch, const u32 addr, const u32 numBytes, void *dst)
{
        auto route = lockRoute();
        if (usingRemote())
                return epoch && epoch == targetEpoch() && ava_peer::client().read(addr, numBytes, dst);
        std::lock_guard lk(mtx_);
        if (!epoch || !isOpen_ || !isConnected_ || busy_ || targetEpoch_ != epoch)
                return false;
        if (!firmwareAllowsAccessLocked(false) || targetEpoch_ != epoch)
                return false;
        const bool ok = JLINKARM_ReadMemEx(addr, numBytes, dst, 0) == int(numBytes);
        recordTransportResultLocked(ok, "motor read");
        return ok;
}

bool
JLinkPort::writeMemAtEpoch(const u64 epoch, const u32 addr, const u32 numBytes, const void *src)
{
        auto route = lockRoute();
        if (usingRemote())
                return epoch && epoch == targetEpoch() && ava_peer::client().write(addr, numBytes, src);
        ava_peer::WriteGuard writeGuard;
        if (!writeGuard)
                return false;
        std::lock_guard lk(mtx_);
        if (!epoch || !isOpen_ || !isConnected_ || busy_ || targetEpoch_ != epoch)
                return false;
        if (!firmwareAllowsAccessLocked(true) || targetEpoch_ != epoch)
                return false;
        const bool ok = JLINKARM_WriteMemEx(addr, numBytes, src, 0) == int(numBytes);
        recordTransportResultLocked(ok, "motor write");
        return ok;
}

bool
JLinkPort::writeMemBitfield(
    const u32 addr, const u32 numBytes, const void *encodedValue, const u32 bitOffset, const u32 bitSize, const u64 epoch)
{
        auto route = lockRoute();
        if (usingRemote())
                return false; // Remote v1 exposes full-width writes only.
        ava_peer::WriteGuard writeGuard;
        if (!writeGuard)
                return false;
        if (!encodedValue || numBytes == 0 || numBytes > sizeof(u64) || bitSize == 0 || bitSize > 64 ||
            bitOffset >= numBytes * 8 || bitSize > numBytes * 8 - bitOffset)
                return false;

        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_)
                return false;

        if (epoch && targetEpoch_.load(std::memory_order_acquire) != epoch)
                return false;
        if (!firmwareAllowsAccessLocked(true) || (epoch && targetEpoch_.load(std::memory_order_acquire) != epoch))
                return false;
        u64  current = 0;
        u64  desired = 0;
        bool ok      = JLINKARM_ReadMemEx(addr, numBytes, &current, 0) == int(numBytes);
        if (!ok) {
                recordTransportResultLocked(false, "writeMemBitfield/read");
                return false;
        }
        std::memcpy(&desired, encodedValue, numBytes);

        const u64 valueMask = bitSize == 64 ? ~u64{0} : ((u64{1} << bitSize) - 1);
        const u64 fieldMask = valueMask << bitOffset;
        current             = (current & ~fieldMask) | ((desired & valueMask) << bitOffset);

        ok = JLINKARM_WriteMemEx(addr, numBytes, &current, 0) == int(numBytes);
        recordTransportResultLocked(ok, "writeMemBitfield/write");
        return ok;
}

void
JLinkPort::recordTransportResultLocked(const bool success, const char *operation)
{
        if (success) {
                readFailCount_.store(0, std::memory_order_relaxed);
                return;
        }

        const int failures = readFailCount_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failures < kMaxReadFails)
                return;

        LOG_E("J-Link %s 操作连续失败 %d 次，正在使 SDK 会话失效", operation, failures);
        prevErr_ = "J-Link 通信已中断，重新连接将重置 USB 会话";
        // We already hold mtx_, so close() cannot be called here. Remember that
        // the DLL still needs JLINKARM_Close() after publishing disconnected UI
        // state; connectAsync() will perform that reset before its next Open()
        sdkSessionDirty_.store(true, std::memory_order_release);
        targetEpoch_.fetch_add(1u, std::memory_order_acq_rel);
        deviceEpoch_.fetch_add(1u, std::memory_order_acq_rel);
        isConnected_.store(false, std::memory_order_release);
        isOpen_.store(false, std::memory_order_release);
        hssRunning_   = false;
        hssFrameSize_ = 0;
        hssActualHz_.store(0.0f, std::memory_order_relaxed);
        rttStatus_.store(RttStatus::Disconnected, std::memory_order_release);
}

u32
JLinkPort::readReg(u32 regIndex)
{
        (void)regIndex;
        return 0;
}

bool
JLinkPort::isHalted()
{
        auto route = lockRoute();
        if (usingRemote())
                return false;
        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_)
                return false;
        return JLINKARM_IsHalted() != 0;
}

bool
JLinkPort::halt()
{
        auto route = lockRoute();
        if (usingRemote())
                return false;
        ava_peer::WriteGuard writeGuard;
        if (!writeGuard)
                return false;
        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_)
                return false;
        return JLINKARM_Halt() >= 0;
}

bool
JLinkPort::resume()
{
        auto route = lockRoute();
        if (usingRemote())
                return false;
        ava_peer::WriteGuard writeGuard;
        if (!writeGuard)
                return false;
        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_)
                return false;
        return JLINKARM_Go() >= 0;
}

bool
JLinkPort::hssStart(const std::vector<HssBlock> &blocks, const i32 periodUs, const u64 epoch)
{
        auto route = lockRoute();
        if (usingRemote()) {
                if (epoch && epoch != targetEpoch())
                        return false;
                try {
                        std::vector<ava_peer::Block> remoteBlocks;
                        for (const auto &block : blocks)
                                remoteBlocks.push_back({block.addr, block.numBytes});
                        remoteFrameSize_ = ava_peer::client().startSamples(remoteBlocks, periodUs);
                        remoteSampling_  = true;
                        return true;
                } catch (const std::exception &e) {
                        std::lock_guard lock(mtx_);
                        prevErr_ = e.what();
                        return false;
                }
        }
        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_) {
                prevErr_ = "HSS：J-Link 未连接";
                LOG_E("HSS 启动失败：J-Link 未连接");
                return false;
        }
        if (epoch && targetEpoch_.load(std::memory_order_acquire) != epoch)
                return false;
        if (!firmwareAllowsAccessLocked(true) || (epoch && targetEpoch_.load(std::memory_order_acquire) != epoch))
                return false;
        if (blocks.empty()) {
                prevErr_ = "HSS：未配置采样数据块";
                return false;
        }
        if (hssRunning_) {
                stopJlinkHssWithDiagnostics();
                hssRunning_ = false;
        }

        std::vector<JLINK_HSS_MEM_BLOCK_DESC> descs(blocks.size());
        u32                                   frameSize = static_cast<u32>(kHssHeaderBytes);
        for (usize i = 0; i < blocks.size(); ++i) {
                descs[i].Addr      = blocks[i].addr;
                descs[i].NumBytes  = blocks[i].numBytes;
                descs[i].Flags     = 0;
                descs[i].Dummy     = 0;
                frameSize         += blocks[i].numBytes;
        }

        i32 effectivePeriodUs = periodUs;
        if (effectivePeriodUs > 100000)
                effectivePeriodUs = 100000; // clamp to 10Hz minimum
        if (effectivePeriodUs < 20)
                effectivePeriodUs = 20; // clamp to 50kHz maximum (matches UI cap; HW rejects if too fast)

        LOG_I("JLinkPort::hssStart(): starting with %zu blocks, period %d us", descs.size(), effectivePeriodUs);
        i32 res = JLINK_HSS_Start(descs.data(), static_cast<i32>(descs.size()), effectivePeriodUs, 1);
        if (res < 0) {
                char buf[256];
                snprintf(buf,
                         sizeof(buf),
                         "HSS 错误：硬件不支持 %d 微秒的采样周期（约 %d Hz），请尝试降低最大采样频率（MaxHz）。",
                         effectivePeriodUs,
                         1000000 / effectivePeriodUs);
                prevErr_ = buf;
                LOG_E("HSS 启动失败：%s", buf);
                return false;
        }
        LOG_I("JLinkPort::hssStart() SUCCEEDED");

        hssRunning_   = true;
        hssFrameSize_ = static_cast<i32>(frameSize);
        hssPeriodUs_  = effectivePeriodUs;
        prevErr_.clear();
        return true;
}

void
JLinkPort::hssStop()
{
        auto route = lockRoute();
        if (usingRemote()) {
                ava_peer::client().stopSamples();
                remoteSampling_  = false;
                remoteFrameSize_ = 0;
                return;
        }
        std::lock_guard lk(mtx_);
        if (hssRunning_) {
                LOG_I("JLinkPort::hssStop()");
                stopJlinkHssWithDiagnostics();
                hssRunning_   = false;
                hssFrameSize_ = 0;
                hssActualHz_.store(0.0f, std::memory_order_relaxed);
        }
}

i32
JLinkPort::hssRead(void *buf, const u32 bufSize)
{
        auto route = lockRoute();
        if (usingRemote()) {
                int result = ava_peer::client().samples(buf, bufSize);
                if (result < 0)
                        remoteSampling_ = false;
                return result;
        }
        std::lock_guard lk(mtx_);
        if (!hssRunning_)
                return 0;
        if (!firmwareAllowsAccessLocked(false) || !hssRunning_)
                return 0;
        const i32 result = JLINK_HSS_Read(buf, bufSize);
        // Zero means no frame is currently available and is not an error
        recordTransportResultLocked(result >= 0, "hssRead");
        if (result >= 0 && std::getenv("AVA_JLINK_LOG_DIR")) {
                static auto nextSnapshot = std::chrono::steady_clock::time_point{};
                const auto  now          = std::chrono::steady_clock::now();
                if (now >= nextSnapshot) {
                        nextSnapshot = now + std::chrono::milliseconds(500);
                        captureJlinkTimingState("sampling");
                }
        }
        return result;
}

namespace
{
constexpr size_t kRttMaxLogBytes          = 256u * 1024u;
constexpr size_t kRttMaxPendingBytes      = 256u * 1024u;
constexpr size_t kRttIoChunkBytes         = 1024u;
constexpr int    kRttControlBlockNotFound = -2;

std::string
rttApiError(const char *operation, int result)
{
        char message[256];
        snprintf(message, sizeof(message), "RTT：%s 操作失败（错误码：%d）", operation, result);
        return message;
}

bool
parseRttControlBlockAddress(const char *text, u32 &address, bool &hasAddress, std::string &error)
{
        hasAddress = false;
        address    = 0;
        error.clear();

        if (!text)
                return true;
        while (*text != '\0' && std::isspace(static_cast<unsigned char>(*text)))
                ++text;
        if (*text == '\0')
                return true;

        errno                  = 0;
        char              *end = nullptr;
        unsigned long long val = std::strtoull(text, &end, 16);
        if (end == text || errno == ERANGE || val == 0 || val > std::numeric_limits<u32>::max()) {
                error = "RTT：控制块地址无效";
                return false;
        }
        while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end)))
                ++end;
        if (*end != '\0') {
                error = "RTT：控制块地址必须为十六进制";
                return false;
        }

        address    = static_cast<u32>(val);
        hasAddress = true;
        return true;
}
} // namespace

bool
JLinkPort::rttStart(bool resumeOnly)
{
        if (usingRemote() || ava_peer::server().running())
                return false;
        std::lock_guard lifecycle(rttLifecycleMtx_);
        if (resumeOnly && !rttResumeRequested_.load(std::memory_order_acquire))
                return false;
        if (rttActive_.load(std::memory_order_acquire))
                return true;

        rttReaderRunning_.store(false, std::memory_order_release);
        rttWakeCv_.notify_all();
        if (rttThread_.joinable())
                rttThread_.join();

        u32         controlBlock = 0;
        bool        hasAddress   = false;
        std::string parseError;
        if (!parseRttControlBlockAddress(rttControlBlock_, controlBlock, hasAddress, parseError)) {
                setRttError(parseError);
                rttStatus_.store(RttStatus::Stopped, std::memory_order_release);
                return false;
        }

        int startResult = -1;
        int upCount     = -1;
        int downCount   = -1;
        {
                std::lock_guard io(mtx_);
                if (!isOpen_.load(std::memory_order_acquire) || !isConnected_.load(std::memory_order_acquire)) {
                        setRttError("RTT：请先通过 J-Link 连接目标设备");
                        rttStatus_.store(RttStatus::Disconnected, std::memory_order_release);
                        return false;
                }

                struct RttStartConfig {
                        u32 ConfigBlockAddress;
                        u32 Reserved[3];
                } startConfig{controlBlock, {0, 0, 0}};
                static_assert(sizeof(RttStartConfig) == 16, "J-Link RTT START ABI requires 16 bytes");
                void *startParam = hasAddress ? static_cast<void *>(&startConfig) : nullptr;
                startResult      = JLINK_RTTERMINAL_Control(JLINKARM_RTTERMINAL_CMD_START, startParam);
                if (startResult >= 0) {
                        u32 dir   = JLINKARM_RTTERMINAL_DIRECTION_UP;
                        upCount   = JLINK_RTTERMINAL_Control(JLINKARM_RTTERMINAL_CMD_GETNUMBUF, &dir);
                        dir       = JLINKARM_RTTERMINAL_DIRECTION_DOWN;
                        downCount = JLINK_RTTERMINAL_Control(JLINKARM_RTTERMINAL_CMD_GETNUMBUF, &dir);
                }
        }

        if (startResult < 0) {
                setRttError("RTT：启动终端失败");
                rttStatus_.store(RttStatus::Stopped, std::memory_order_release);
                return false;
        }

        {
                std::lock_guard data(rttDataMtx_);
                rttTxPending_.clear();
                rttError_.clear();
        }
        rttUpBufferCount_.store(upCount, std::memory_order_release);
        rttDownBufferCount_.store(downCount, std::memory_order_release);
        if (upCount == kRttControlBlockNotFound || downCount == kRttControlBlockNotFound) {
                rttStatus_.store(RttStatus::Searching, std::memory_order_release);
        } else if (upCount < 0 || downCount < 0) {
                const int error = upCount < 0 ? upCount : downCount;
                setRttError(rttApiError("GETNUMBUF", error));
                rttStatus_.store(RttStatus::Error, std::memory_order_release);
        } else if (rttUpChannel_ >= upCount) {
                rttStatus_.store(RttStatus::UpChannelUnavailable, std::memory_order_release);
        } else {
                rttStatus_.store(RttStatus::Running, std::memory_order_release);
        }
        rttActive_.store(true, std::memory_order_release);
        rttReaderRunning_.store(true, std::memory_order_release);
        try {
                rttThread_ = std::thread(&JLinkPort::rttReaderLoop, this);
        } catch (const std::exception &e) {
                rttReaderRunning_.store(false, std::memory_order_release);
                rttActive_.store(false, std::memory_order_release);
                {
                        std::lock_guard io(mtx_);
                        if (isOpen_.load(std::memory_order_acquire))
                                JLINK_RTTERMINAL_Control(JLINKARM_RTTERMINAL_CMD_STOP, nullptr);
                }
                setRttError(std::string("RTT：创建读取线程失败（系统原始信息）：") + e.what());
                rttStatus_.store(RttStatus::Stopped, std::memory_order_release);
                return false;
        }

        LOG_I("JLink RTT started: up=%d/%d down=%d/%d%s",
              rttUpChannel_,
              upCount,
              rttDownChannel_,
              downCount,
              hasAddress ? " (fixed control block)" : "");
        rttResumeRequested_.store(true, std::memory_order_release);
        return true;
}

void
JLinkPort::rttStop()
{
        std::lock_guard lifecycle(rttLifecycleMtx_);
        rttResumeRequested_.store(false, std::memory_order_release);
        rttStopImpl();
}

void
JLinkPort::rttStopImpl()
{
        rttReaderRunning_.store(false, std::memory_order_release);
        rttWakeCv_.notify_all();
        if (rttThread_.joinable())
                rttThread_.join();

        const bool wasActive = rttActive_.exchange(false, std::memory_order_acq_rel);
        if (wasActive) {
                std::lock_guard io(mtx_);
                if (isOpen_.load(std::memory_order_acquire))
                        JLINK_RTTERMINAL_Control(JLINKARM_RTTERMINAL_CMD_STOP, nullptr);
        }

        {
                std::lock_guard data(rttDataMtx_);
                rttTxPending_.clear();
        }
        rttUpBufferCount_.store(-1, std::memory_order_release);
        rttDownBufferCount_.store(-1, std::memory_order_release);
        rttStatus_.store(isConnected_.load(std::memory_order_acquire) ? RttStatus::Stopped : RttStatus::Disconnected,
                         std::memory_order_release);
}

void
JLinkPort::rttReaderLoop()
{
        char rxBuffer[kRttIoChunkBytes];
        auto nextProbe = std::chrono::steady_clock::now();

        while (rttReaderRunning_.load(std::memory_order_acquire)) {
                if (!isOpen_.load(std::memory_order_acquire) || !isConnected_.load(std::memory_order_acquire)) {
                        setRttError("RTT：J-Link 目标设备已断开");
                        rttStatus_.store(RttStatus::Disconnected, std::memory_order_release);
                        rttActive_.store(false, std::memory_order_release);
                        break;
                }

                auto now = std::chrono::steady_clock::now();
                if (now >= nextProbe) {
                        int upCount   = -1;
                        int downCount = -1;
                        {
                                std::lock_guard io(mtx_);
                                if (isOpen_.load(std::memory_order_acquire) && isConnected_.load(std::memory_order_acquire)) {
                                        u32 dir   = JLINKARM_RTTERMINAL_DIRECTION_UP;
                                        upCount   = JLINK_RTTERMINAL_Control(JLINKARM_RTTERMINAL_CMD_GETNUMBUF, &dir);
                                        dir       = JLINKARM_RTTERMINAL_DIRECTION_DOWN;
                                        downCount = JLINK_RTTERMINAL_Control(JLINKARM_RTTERMINAL_CMD_GETNUMBUF, &dir);
                                }
                        }
                        rttUpBufferCount_.store(upCount, std::memory_order_release);
                        rttDownBufferCount_.store(downCount, std::memory_order_release);
                        const RttStatus prevStatus = rttStatus_.load(std::memory_order_acquire);
                        if (upCount == kRttControlBlockNotFound || downCount == kRttControlBlockNotFound) {
                                rttStatus_.store(RttStatus::Searching, std::memory_order_release);
                        } else if (upCount < 0 || downCount < 0) {
                                const int error = upCount < 0 ? upCount : downCount;
                                setRttError(rttApiError("GETNUMBUF", error));
                                rttStatus_.store(RttStatus::Error, std::memory_order_release);
                        } else if (rttUpChannel_ >= upCount) {
                                rttStatus_.store(RttStatus::UpChannelUnavailable, std::memory_order_release);
                        } else {
                                if (prevStatus == RttStatus::Error)
                                        setRttError({});
                                rttStatus_.store(RttStatus::Running, std::memory_order_release);
                        }
                        nextProbe = now + std::chrono::milliseconds(250);
                }

                bool      didIo   = false;
                const int upCount = rttUpBufferCount_.load(std::memory_order_acquire);
                if (rttUpChannel_ >= 0 && rttUpChannel_ < upCount) {
                        int readBytes = -1;
                        {
                                std::lock_guard io(mtx_);
                                if (isOpen_.load(std::memory_order_acquire) && isConnected_.load(std::memory_order_acquire)) {
                                        readBytes = JLINK_RTTERMINAL_Read(
                                            static_cast<u32>(rttUpChannel_), rxBuffer, static_cast<u32>(sizeof(rxBuffer)));
                                }
                        }
                        if (readBytes > 0) {
                                size_t count = std::min(static_cast<size_t>(readBytes), sizeof(rxBuffer));
                                {
                                        std::lock_guard data(rttDataMtx_);
                                        rttRxLog_.append(rxBuffer, count);
                                        if (rttRxLog_.size() > kRttMaxLogBytes)
                                                rttRxLog_.erase(0, rttRxLog_.size() - kRttMaxLogBytes);
                                }
                                rttRxBytes_.fetch_add(count, std::memory_order_relaxed);
                                didIo = true;
                        } else if (readBytes < 0) {
                                rttUpBufferCount_.store(-1, std::memory_order_release);
                                if (readBytes == kRttControlBlockNotFound) {
                                        rttStatus_.store(RttStatus::Searching, std::memory_order_release);
                                } else {
                                        setRttError(rttApiError("READ", readBytes));
                                        rttStatus_.store(RttStatus::Error, std::memory_order_release);
                                }
                                nextProbe = std::chrono::steady_clock::now();
                        }
                }

                std::string txChunk;
                {
                        std::lock_guard data(rttDataMtx_);
                        if (!rttTxPending_.empty())
                                txChunk.assign(rttTxPending_.data(), std::min(rttTxPending_.size(), kRttIoChunkBytes));
                }
                const int downCount = rttDownBufferCount_.load(std::memory_order_acquire);
                if (!txChunk.empty() && rttDownChannel_ >= 0 && rttDownChannel_ < downCount) {
                        int written = -1;
                        {
                                std::lock_guard io(mtx_);
                                if (isOpen_.load(std::memory_order_acquire) && isConnected_.load(std::memory_order_acquire)) {
                                        written = JLINK_RTTERMINAL_Write(static_cast<u32>(rttDownChannel_),
                                                                         txChunk.data(),
                                                                         static_cast<u32>(txChunk.size()));
                                }
                        }
                        if (written > 0) {
                                size_t consumed = std::min(static_cast<size_t>(written), txChunk.size());
                                {
                                        std::lock_guard data(rttDataMtx_);
                                        rttTxPending_.erase(0, std::min(consumed, rttTxPending_.size()));
                                }
                                rttTxBytes_.fetch_add(consumed, std::memory_order_relaxed);
                                didIo = true;
                        } else if (written < 0) {
                                rttDownBufferCount_.store(-1, std::memory_order_release);
                                if (written != kRttControlBlockNotFound) {
                                        setRttError(rttApiError("WRITE", written));
                                        rttStatus_.store(RttStatus::Error, std::memory_order_release);
                                }
                                nextProbe = std::chrono::steady_clock::now();
                        }
                }

                std::unique_lock wake(rttWakeMtx_);
                rttWakeCv_.wait_for(wake, didIo ? std::chrono::milliseconds(1) : std::chrono::milliseconds(10), [this] {
                        return !rttReaderRunning_.load(std::memory_order_acquire);
                });
        }

        rttReaderRunning_.store(false, std::memory_order_release);
}

bool
JLinkPort::rttSend(const char *data, usize size)
{
        if (!data || size == 0)
                return true;

        // Serialize queueing with STOP/close so a sender cannot append after
        // rttStopImpl() has cleared the pending queue
        std::lock_guard lifecycle(rttLifecycleMtx_);
        if (!rttActive_.load(std::memory_order_acquire)) {
                setRttError("RTT：终端尚未运行");
                return false;
        }

        const int downCount = rttDownBufferCount_.load(std::memory_order_acquire);
        if (downCount >= 0 && (rttDownChannel_ < 0 || rttDownChannel_ >= downCount)) {
                setRttError("RTT：所选下行通道不可用");
                return false;
        }

        bool queued = false;
        {
                std::lock_guard lock(rttDataMtx_);
                size_t          used = std::min(rttTxPending_.size(), kRttMaxPendingBytes);
                if (size <= kRttMaxPendingBytes - used) {
                        rttTxPending_.append(data, size);
                        queued = true;
                }
        }
        if (!queued) {
                setRttError("RTT：发送队列已满");
                return false;
        }

        rttWakeCv_.notify_all();
        return true;
}

std::string
JLinkPort::rttRxSnapshot() const
{
        std::lock_guard lock(rttDataMtx_);
        return rttRxLog_;
}

std::string
JLinkPort::rttErrorSnapshot() const
{
        std::lock_guard lock(rttDataMtx_);
        return rttError_;
}

usize
JLinkPort::rttTxPendingBytes() const
{
        std::lock_guard lock(rttDataMtx_);
        return rttTxPending_.size();
}

void
JLinkPort::rttClearRx()
{
        std::lock_guard lock(rttDataMtx_);
        rttRxLog_.clear();
        rttRxBytes_.store(0, std::memory_order_relaxed);
}

void
JLinkPort::setRttError(const std::string &error)
{
        std::lock_guard lock(rttDataMtx_);
        rttError_ = error;
}

void
JLinkPort::drawRttWindow()
{
        if (!rttWindowOpen_)
                return;

        ImGui::SetNextWindowSize(ImVec2(640.0f, 440.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 320.0f),
                                            ImVec2(std::numeric_limits<float>::max(), std::numeric_limits<float>::max()));
        if (rttWindowFocusRequested_)
                ImGui::SetNextWindowFocus();
        rttWindowFocusRequested_ = false;

        if (!dock_close::begin(tr("J-Link RTT Terminal###JLinkRttWindow", "J-Link RTT 终端###JLinkRttWindow"),
                               &rttWindowOpen_)) {
                ImGui::End();
                return;
        }

        bool      active = rttActive_.load(std::memory_order_acquire);
        RttStatus status = rttStatus_.load(std::memory_order_acquire);

        ImGui::BeginDisabled(active || busy_.load(std::memory_order_acquire));
        ImGui::TextUnformatted(tr("Control Block", "控制块"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::InputTextWithHint("##jlink_rtt_cb", "Auto / 0x20000000", rttControlBlock_, sizeof(rttControlBlock_)))
                configModified_.store(true, std::memory_order_release);
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                                  tr("Optional RTT control block address (hex). Leave empty for automatic search.",
                                     "可选 RTT 控制块地址（十六进制），留空自动搜索。"));
        ImGui::SameLine();
        ImGui::TextUnformatted("Up");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        if (ImGui::InputInt("##jlink_rtt_up", &rttUpChannel_, 0, 0)) {
                rttUpChannel_ = std::max(rttUpChannel_, 0);
                configModified_.store(true, std::memory_order_release);
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("RTT up-buffer index", "RTT 上行缓冲区编号"));
        ImGui::SameLine();
        ImGui::TextUnformatted("Down");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        if (ImGui::InputInt("##jlink_rtt_down", &rttDownChannel_, 0, 0)) {
                rttDownChannel_ = std::max(rttDownChannel_, 0);
                configModified_.store(true, std::memory_order_release);
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("RTT down-buffer index", "RTT 下行缓冲区编号"));
        ImGui::EndDisabled();

        const char *statusText = tr("Stopped", "已停止");
        switch (status) {
                case RttStatus::Searching:
                        statusText = tr("Searching for RTT control block", "正在搜索 RTT 控制块");
                        break;
                case RttStatus::Running:
                        statusText = tr("Running", "运行中");
                        break;
                case RttStatus::UpChannelUnavailable:
                        statusText = tr("Selected up ch is unavailable", "所选上行通道不可用");
                        break;
                case RttStatus::Disconnected:
                        statusText = tr("J-Link disconnected", "J-Link 未连接");
                        break;
                case RttStatus::Error:
                        statusText = tr("RTT I/O error", "RTT 输入输出错误");
                        break;
                case RttStatus::Stopped:
                        break;
        }
        const int  downBufferCount = rttDownBufferCount_.load(std::memory_order_relaxed);
        const bool downUnavailable =
            active && downBufferCount >= 0 && (rttDownChannel_ < 0 || rttDownChannel_ >= downBufferCount);
        if (status == RttStatus::Error || status == RttStatus::UpChannelUnavailable)
                ui::ErrorText("%s", statusText);
        else
                ImGui::TextUnformatted(statusText);
        ImGui::SameLine();
        ImGui::Text("| Up: %d | Down: %d | RX: %llu | TX: %llu | Queue: %zu",
                    rttUpBufferCount_.load(std::memory_order_relaxed),
                    rttDownBufferCount_.load(std::memory_order_relaxed),
                    static_cast<unsigned long long>(rttRxBytes_.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(rttTxBytes_.load(std::memory_order_relaxed)),
                    rttTxPendingBytes());
        if (downUnavailable) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.2f, 1.0f));
                ImGui::TextUnformatted(
                    tr("Selected down ch is unavailable; sending is disabled.", "所选下行通道不可用，发送已禁用。"));
                ImGui::PopStyleColor();
        }

        const bool resumeRequested = rttResumeRequested_.load(std::memory_order_acquire);
        if (active || resumeRequested) {
                ImGui::BeginDisabled(busy_.load(std::memory_order_acquire));
                if (ui::SmallButton(tr("STOP RTT", "停止 RTT"), ui::BtnStyle::Danger))
                        rttStop();
                ImGui::EndDisabled();
        } else {
                ImGui::BeginDisabled(!isConnected_.load(std::memory_order_acquire) || busy_.load(std::memory_order_acquire));
                if (ui::SmallButton(tr("START RTT", "启动 RTT"), ui::BtnStyle::Success))
                        rttStart();
                ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Clear", "清空")))
                rttClearRx();
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Copy all", "复制全部")))
                ImGui::SetClipboardText(rttRxSnapshot().c_str());
        if (!active && resumeRequested)
                ImGui::TextDisabled("%s", tr("RTT will resume when J-Link reconnects.", "RTT 将在 J-Link 重连后自动恢复。"));

        std::string error = rttErrorSnapshot();
        if (!error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::ErrorTextColor());
                ImGui::TextWrapped("%s", error.c_str());
                ImGui::PopStyleColor();
        }

        std::string rx       = rttRxSnapshot();
        const float rxHeight = std::max(120.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing());
        ui::LogView("##jlink_rtt_rx", rx, ImVec2(-1.0f, rxHeight), &rttAutoScroll_);

        ImGui::BeginDisabled(!active || downUnavailable || !isConnected_.load(std::memory_order_acquire));
        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 185.0f));
        bool sendNow = ImGui::InputText("##jlink_rtt_tx", rttTxBuf_, sizeof(rttTxBuf_), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ui::SmallButton(tr("SEND", "发送"), ui::BtnStyle::Success))
                sendNow = true;
        ImGui::SameLine();
        if (ImGui::Checkbox(tr("Append LF", "追加换行"), &rttAppendNewline_))
                configModified_.store(true, std::memory_order_release);
        if (sendNow && rttTxBuf_[0] != '\0') {
                std::string message(rttTxBuf_);
                if (rttAppendNewline_)
                        message.push_back('\n');
                if (rttSend(message.data(), message.size()))
                        rttTxBuf_[0] = '\0';
        }
        ImGui::EndDisabled();

        ImGui::End();
}

namespace
{
constexpr usize kSwoMaxTextBytes    = 512u * 1024u;
constexpr usize kSwoReadBufferBytes = 1024u * 1024u;
constexpr usize kSwoMaxEvents       = 4096u;

constexpr u32 kDemcrAddress        = 0xE000EDFCu;
constexpr u32 kItmTcrAddress       = 0xE0000E80u;
constexpr u32 kItmLarAddress       = 0xE0000FB0u;
constexpr u32 kDwtCtrlAddress      = 0xE0001000u;
constexpr u32 kDwtCyccntAddress    = 0xE0001004u;
constexpr u32 kDwtLarAddress       = 0xE0001FB0u;
constexpr u32 kTpiuAcprAddress     = 0xE0040010u;
constexpr u32 kTpiuSpprAddress     = 0xE00400F0u;
constexpr u32 kTpiuFfcrAddress     = 0xE0040304u;
constexpr u32 kH7DbgMcuCrAddress   = 0x5C001004u;
constexpr u32 kH7SwoCodrAddress    = 0x5C003010u;
constexpr u32 kH7SwoSpprAddress    = 0x5C0030F0u;
constexpr u32 kH7SwoLarAddress     = 0x5C003FB0u;
constexpr u32 kH7SwtfCtrlAddress   = 0x5C004000u;
constexpr u32 kH7SwtfLarAddress    = 0x5C004FB0u;
constexpr u32 kH7TraceClockMask    = 0x00700000u; // TRACECLKEN, D1DBGCKEN, D3DBGCKEN
constexpr u32 kDwtComp0Address     = 0xE0001020u;
constexpr u32 kDwtMask0Address     = 0xE0001024u;
constexpr u32 kDwtFunction0Address = 0xE0001028u;
constexpr u32 kCoreSightUnlockKey  = 0xC5ACCE55u;

u32
traceWatchMask(const int size)
{
        if (size >= 4)
                return 2;
        if (size >= 2)
                return 1;
        return 0;
}

std::vector<int>
swoSpeedLevels(const int cpuMHz)
{
        if (cpuMHz <= 0)
                return {1000};

        const u64 cpuKHz = static_cast<u64>(cpuMHz) * 1000u;
        // Generate a small set of useful bands. Each selected frequency is an
        // exact CPU-clock divisor and therefore maps to an integer TPIU divider
        constexpr int    targetKHz[] = {50000, 40000, 25000, 20000, 16000, 12000, 10000, 8000, 4000, 2000, 1000, 500, 250, 125};
        std::vector<int> levels;
        levels.reserve(IM_ARRAYSIZE(targetKHz));
        for (const int target : targetKHz) {
                u64 divider = std::max<u64>(1u, (cpuKHz + static_cast<u64>(target) - 1u) / static_cast<u64>(target));
                for (; divider <= 8192u; ++divider) {
                        if (cpuKHz % divider != 0)
                                continue;
                        const u64 speedKHz = cpuKHz / divider;
                        if (speedKHz <= 50000u && speedKHz <= static_cast<u64>(std::numeric_limits<int>::max())) {
                                const int speed = static_cast<int>(speedKHz);
                                if (levels.empty() || levels.back() != speed)
                                        levels.push_back(speed);
                        }
                        break;
                }
        }
        if (levels.empty())
                levels.push_back(1);
        else
                std::reverse(levels.begin(), levels.end());
        return levels;
}
} // namespace

void
JLinkPort::openTraceWindow()
{
        if (!swoWindowOpen_)
                configModified_.store(true, std::memory_order_release);
        swoWindowOpen_           = true;
        swoWindowFocusRequested_ = true;
}

void
JLinkPort::configureDwtWriteTrace(const u32 address, const u32 size, const std::string &name)
{
        const u32 supportedSize = size >= 4 ? 4u : (size >= 2 ? 2u : 1u);
        snprintf(swoWatchAddress_, sizeof(swoWatchAddress_), "0x%08X", address);
        snprintf(swoWatchName_, sizeof(swoWatchName_), "%s", name.c_str());
        swoWatchSize_            = static_cast<int>(supportedSize);
        swoWatchWrite_           = true;
        swoWindowOpen_           = true;
        swoWindowFocusRequested_ = true;
        if (swoActive_.load(std::memory_order_acquire))
                setSwoError("DWT：请停止并重新启动跟踪，以应用新的变量监视配置");
}

void
JLinkPort::setTraceAddressResolver(std::function<std::string(u32)> resolver)
{
        traceAddressResolver_ = std::move(resolver);
}

std::string
JLinkPort::resolveTraceAddress(const u32 address) const
{
        if (traceAddressResolver_) {
                std::string result = traceAddressResolver_(address);
                if (!result.empty())
                        return result;
        }
        char text[24];
        snprintf(text, sizeof(text), "0x%08X", address);
        return text;
}

bool
JLinkPort::swoConfigureHardwareLocked()
{
        swoTargetStateSaved_     = false;
        swoWatchStateSaved_      = false;
        const std::string device = deviceName();
        swoUsesStm32H7SystemSwo_ = device.starts_with("STM32H745") || device.starts_with("STM32H747") ||
                                   device.starts_with("STM32H755") || device.starts_with("STM32H757");

        auto readU32  = [](const u32 address, u32 &val) { return JLINKARM_ReadMemEx(address, sizeof(val), &val, 0) >= 0; };
        auto writeU32 = [](const u32 address, const u32 val) {
                return JLINKARM_WriteMemEx(address, sizeof(val), &val, 0) >= 0;
        };

        const bool coreStateRead = readU32(kDemcrAddress, swoSavedDemcr_) && readU32(kDwtCtrlAddress, swoSavedDwtCtrl_) &&
                                   readU32(kDwtCyccntAddress, swoSavedDwtCyccnt_) && readU32(kItmTcrAddress, swoSavedItmTcr_);
        const bool outputStateRead =
            swoUsesStm32H7SystemSwo_
                ? (readU32(kH7DbgMcuCrAddress, swoSavedH7DbgMcuCr_) && readU32(kH7SwoCodrAddress, swoSavedH7SwoCodr_) &&
                   readU32(kH7SwoSpprAddress, swoSavedH7SwoSppr_) && readU32(kH7SwtfCtrlAddress, swoSavedH7SwtfCtrl_))
                : (readU32(kTpiuAcprAddress, swoSavedTpiuAcpr_) && readU32(kTpiuSpprAddress, swoSavedTpiuSppr_) &&
                   readU32(kTpiuFfcrAddress, swoSavedTpiuFfcr_));
        if (!coreStateRead || !outputStateRead) {
                setSwoError("DWT：目标设备未提供所需的 CoreSight 寄存器");
                return false;
        }
        swoTargetStateSaved_ = true;

        // Cortex-M7 implements lock access registers for ITM and DWT. A debug
        // memory write can report success while the protected register write is
        // silently ignored, so unlock both blocks before changing their state
        if (!writeU32(kItmLarAddress, kCoreSightUnlockKey) || !writeU32(kDwtLarAddress, kCoreSightUnlockKey)) {
                setSwoError("DWT：解锁 Cortex-M7 的 ITM/DWT 模块失败");
                return false;
        }

        if (!writeU32(kDemcrAddress, swoSavedDemcr_ | (1u << 24u))) {
                setSwoError("DWT：启用 CoreSight 跟踪失败");
                return false;
        }

        const u64 cpuHz   = static_cast<u64>(swoCpuMHz_) * 1000000u;
        const u64 swoHz   = static_cast<u64>(swoSpeedKHz_) * 1000u;
        const u64 divider = cpuHz / swoHz;
        // Dual-core STM32H745/747/755/757 route M7 ITM through ST's system
        // SWO trace funnel, not through the Cortex-M TPIU window at E0040000
        bool outputConfigured = false;
        if (swoUsesStm32H7SystemSwo_) {
                outputConfigured =
                    writeU32(kItmTcrAddress, 0u) && writeU32(kH7DbgMcuCrAddress, swoSavedH7DbgMcuCr_ | kH7TraceClockMask) &&
                    writeU32(kH7SwoLarAddress, kCoreSightUnlockKey) && writeU32(kH7SwtfLarAddress, kCoreSightUnlockKey) &&
                    writeU32(kH7SwoSpprAddress, 2u) && writeU32(kH7SwoCodrAddress, static_cast<u32>(divider - 1u)) &&
                    writeU32(kH7SwtfCtrlAddress, swoSavedH7SwtfCtrl_ | 1u); // M7 ITM input
        } else {
                // SPPR=2 selects asynchronous NRZ (UART) SWO
                outputConfigured = writeU32(kItmTcrAddress, 0u) && writeU32(kTpiuSpprAddress, 2u) &&
                                   writeU32(kTpiuAcprAddress, static_cast<u32>(divider - 1u)) &&
                                   writeU32(kTpiuFfcrAddress, swoSavedTpiuFfcr_ | (1u << 8u));
        }
        if (!outputConfigured) {
                setSwoError("SWO：配置目标 TPIU 的 UART 输出失败");
                return false;
        }

        // ITMENA routes ITM packets and DWTENA routes hardware packets from DWT
        // through the same TPIU/SWO stream
        if (!writeU32(kItmTcrAddress, swoSavedItmTcr_ | (1u << 0u) | (1u << 3u))) {
                setSwoError("DWT：启用 ITM 硬件数据包通路失败");
                return false;
        }

        u32 dwtCtrl = swoSavedDwtCtrl_;
        if (swoExceptionTrace_)
                dwtCtrl |= 1u << 16u; // EXCTRCENA
        if (swoPcSampling_) {
                if ((dwtCtrl & (1u << 25u)) != 0) {
                        setSwoError("DWT：当前内核没有周期计数器，无法使用 PC 采样");
                } else {
                        dwtCtrl              |= (1u << 0u) | (1u << 12u); // CYCCNTENA + PCSAMPLENA
                        dwtCtrl              &= ~((0xFu << 1u) | (0xFu << 5u) | (1u << 9u));
                        const u32 postPreset  = swoPcSampleRate_ >= 2 ? 15u : 3u;
                        dwtCtrl              |= (postPreset << 1u) | (postPreset << 5u);
                        if (swoPcSampleRate_ >= 1)
                                dwtCtrl |= 1u << 9u; // use CYCCNT bit 10 instead of bit 6
                }
        }
        if (!writeU32(kDwtCtrlAddress, dwtCtrl)) {
                setSwoError("DWT：配置异常或 PC 采样失败");
                return false;
        }

        u32       itmTcrReadback     = 0;
        u32       dwtCtrlReadback    = 0;
        u32       tpiuAcprReadback   = 0;
        u32       tpiuSpprReadback   = 0;
        u32       h7SwoCodrReadback  = 0;
        u32       h7SwoSpprReadback  = 0;
        u32       h7SwtfCtrlReadback = 0;
        const u32 requiredDwtBits =
            (swoExceptionTrace_ ? (1u << 16u) : 0u) | (swoPcSampling_ ? ((1u << 0u) | (1u << 12u)) : 0u);
        const bool outputVerified = swoUsesStm32H7SystemSwo_
                                        ? (readU32(kH7SwoSpprAddress, h7SwoSpprReadback) && h7SwoSpprReadback == 2u &&
                                           readU32(kH7SwoCodrAddress, h7SwoCodrReadback) && h7SwoCodrReadback == divider - 1u &&
                                           readU32(kH7SwtfCtrlAddress, h7SwtfCtrlReadback) && (h7SwtfCtrlReadback & 1u) != 0u)
                                        : (readU32(kTpiuSpprAddress, tpiuSpprReadback) && tpiuSpprReadback == 2u &&
                                           readU32(kTpiuAcprAddress, tpiuAcprReadback) && tpiuAcprReadback == divider - 1u);
        if (!readU32(kItmTcrAddress, itmTcrReadback) ||
            (itmTcrReadback & ((1u << 0u) | (1u << 3u))) != ((1u << 0u) | (1u << 3u)) ||
            !readU32(kDwtCtrlAddress, dwtCtrlReadback) || (dwtCtrlReadback & requiredDwtBits) != requiredDwtBits ||
            !outputVerified) {
                setSwoError("SWO：Cortex-M7 拒绝了 ITM/DWT/TPIU 配置");
                return false;
        }

        if (swoWatchWrite_) {
                char     *end          = nullptr;
                const u64 watchAddress = std::strtoull(swoWatchAddress_, &end, 0);
                while (end && *end != '\0' && std::isspace(static_cast<unsigned char>(*end)))
                        ++end;
                if (!end || end == swoWatchAddress_ || *end != '\0' || watchAddress > std::numeric_limits<u32>::max()) {
                        setSwoError("DWT：变量地址无效");
                        return false;
                }
                swoWatchSize_ = swoWatchSize_ >= 4 ? 4 : (swoWatchSize_ >= 2 ? 2 : 1);
                if ((watchAddress & static_cast<u64>(swoWatchSize_ - 1)) != 0) {
                        setSwoError("DWT：监视地址必须按数据大小对齐");
                        return false;
                }
                const u32 comparatorCount = (swoSavedDwtCtrl_ >> 28u) & 0xFu;
                if (comparatorCount == 0) {
                        setSwoError("DWT：当前内核没有硬件观察点比较器");
                        return false;
                }
                if (!readU32(kDwtComp0Address, swoSavedDwtComp0_) || !readU32(kDwtMask0Address, swoSavedDwtMask0_) ||
                    !readU32(kDwtFunction0Address, swoSavedDwtFunction0_)) {
                        setSwoError("DWT：保存比较器 0 的配置失败");
                        return false;
                }
                swoWatchStateSaved_ = true;
                // FUNCTION=0xA: data-address write match with PC and data-value
                // trace packets on Armv7-M DWT. Disable before changing COMP/MASK
                if (!writeU32(kDwtFunction0Address, 0) || !writeU32(kDwtComp0Address, static_cast<u32>(watchAddress)) ||
                    !writeU32(kDwtMask0Address, traceWatchMask(swoWatchSize_)) || !writeU32(kDwtFunction0Address, 0xAu)) {
                        setSwoError("DWT：配置写访问跟踪失败");
                        return false;
                }
        }
        return true;
}

void
JLinkPort::swoRestoreHardwareLocked()
{
        auto writeU32 = [](const u32 address, const u32 val) {
                return JLINKARM_WriteMemEx(address, sizeof(val), &val, 0) >= 0;
        };
        if (swoWatchStateSaved_) {
                writeU32(kDwtFunction0Address, 0);
                writeU32(kDwtComp0Address, swoSavedDwtComp0_);
                writeU32(kDwtMask0Address, swoSavedDwtMask0_);
                writeU32(kDwtFunction0Address, swoSavedDwtFunction0_);
        }
        if (swoTargetStateSaved_) {
                writeU32(kDwtCtrlAddress, swoSavedDwtCtrl_);
                writeU32(kDwtCyccntAddress, swoSavedDwtCyccnt_);
                writeU32(kItmTcrAddress, 0u);
                if (swoUsesStm32H7SystemSwo_) {
                        writeU32(kH7SwtfCtrlAddress, swoSavedH7SwtfCtrl_);
                        writeU32(kH7SwoCodrAddress, swoSavedH7SwoCodr_);
                        writeU32(kH7SwoSpprAddress, swoSavedH7SwoSppr_);
                        // Keep D1DBGCKEN/D3DBGCKEN enabled for the lifetime of
                        // the open J-Link session. Clearing them here gates the
                        // dual-core debug infrastructure and makes the next
                        // CoreSight access fail until the probe reconnects
                } else {
                        writeU32(kTpiuAcprAddress, swoSavedTpiuAcpr_);
                        writeU32(kTpiuSpprAddress, swoSavedTpiuSppr_);
                        writeU32(kTpiuFfcrAddress, swoSavedTpiuFfcr_);
                }
                writeU32(kItmTcrAddress, swoSavedItmTcr_);
                writeU32(kDemcrAddress, swoSavedDemcr_);
        }
        swoWatchStateSaved_  = false;
        swoTargetStateSaved_ = false;
}

bool
JLinkPort::swoStart()
{
        if (usingRemote() || ava_peer::server().running())
                return false;
        std::lock_guard lifecycle(swoLifecycleMtx_);
        if (swoActive_.load(std::memory_order_acquire))
                return true;

        swoReaderRunning_.store(false, std::memory_order_release);
        swoWakeCv_.notify_all();
        if (swoThread_.joinable())
                swoThread_.join();
        setSwoError({});

        swoItmPort_  = std::clamp(swoItmPort_, 0, 31);
        swoCpuMHz_   = std::max(swoCpuMHz_, 0);
        swoSpeedKHz_ = std::max(swoSpeedKHz_, 0);
        // The SDK documents CPUSpeed=0 as automatic measurement, but some
        // probe/STM32H7 combinations never return from SWO_EnableTarget
        // Reject it before entering that synchronous SDK call and holding the
        // shared J-Link I/O lock
        if (swoCpuMHz_ == 0) {
                setSwoError("SWO：请填写 CPU 频率（MHz），自动测量时钟可能导致 STM32H7 卡住");
                swoStatus_.store(SwoStatus::Error, std::memory_order_release);
                return false;
        }
        if (swoSpeedKHz_ == 0) {
                setSwoError("SWO：请填写 SWO 频率（kHz），以配置目标 TPIU 分频器");
                swoStatus_.store(SwoStatus::Error, std::memory_order_release);
                return false;
        }
        const u32 portMask = 1u << static_cast<u32>(swoItmPort_);
        const u32 cpuHz =
            static_cast<u32>(std::min<u64>(static_cast<u64>(swoCpuMHz_) * 1000000u, std::numeric_limits<u32>::max()));
        const u32 swoHz =
            static_cast<u32>(std::min<u64>(static_cast<u64>(swoSpeedKHz_) * 1000u, std::numeric_limits<u32>::max()));
        const u64 divider = static_cast<u64>(cpuHz) / swoHz;
        if (static_cast<u64>(cpuHz) % swoHz != 0 || divider == 0 || divider > 8192u) {
                setSwoError("SWO：CPU 频率换算为 kHz 后必须是 SWO 频率的整数倍（TPIU 分频范围为 1～8192）");
                swoStatus_.store(SwoStatus::Error, std::memory_order_release);
                return false;
        }

        int result = -1;
        {
                std::lock_guard io(mtx_);
                if (!isOpen_.load(std::memory_order_acquire) || !isConnected_.load(std::memory_order_acquire)) {
                        setSwoError("SWO：请先通过 J-Link 连接目标设备");
                        swoStatus_.store(SwoStatus::Disconnected, std::memory_order_release);
                        return false;
                }
                // A prev target disconnect can leave the DLL's host-side
                // SWO receiver marked as started even after a new J-Link
                // connection has been established. STOP is host-side only and
                // is safe when no receiver is active
                JLINKARM_SWO_Control(JLINKARM_SWO_CMD_STOP, nullptr);
                result = JLINKARM_SWO_EnableTarget(cpuHz, swoHz, JLINKARM_SWO_IF_UART, portMask);
                if (result >= 0 && !swoConfigureHardwareLocked()) {
                        swoRestoreHardwareLocked();
                        JLINKARM_SWO_Control(JLINKARM_SWO_CMD_STOP, nullptr);
                        JLINKARM_SWO_DisableTarget(portMask);
                        result = -1;
                } else if (result < 0) {
                        // EnableTarget may have started the host receiver before
                        // failing to resume/configure the CPU. Do not carry that
                        // partial state into reconnect or application shutdown
                        JLINKARM_SWO_Control(JLINKARM_SWO_CMD_STOP, nullptr);
                }
        }
        if (result < 0) {
                if (swoErrorSnapshot().empty()) {
                        char message[160];
                        snprintf(message,
                                 sizeof(message),
                                 "SWO：启用目标设备失败（错误码：%d），请检查 CPU 时钟和 SWO 接线。",
                                 result);
                        setSwoError(message);
                }
                swoStatus_.store(SwoStatus::Error, std::memory_order_release);
                return false;
        }

        {
                std::lock_guard data(swoDataMtx_);
                swoDecodePending_.clear();
                swoEvents_.clear();
                swoTimelineView_.clear();
                swoPcSamples_.clear();
                swoPcSampleTotal_        = 0;
                swoLastWatchPc_          = 0;
                swoLastTimelineUs_       = 0.0;
                swoTimelineFollow_       = true;
                swoTimelineScrollFrames_ = 3;
        }
        swoStartTime_  = std::chrono::steady_clock::now();
        swoActivePort_ = swoItmPort_;
        swoStatus_.store(SwoStatus::Running, std::memory_order_release);
        swoActive_.store(true, std::memory_order_release);
        swoReaderRunning_.store(true, std::memory_order_release);
        try {
                swoThread_ = std::thread(&JLinkPort::swoReaderLoop, this);
        } catch (const std::exception &e) {
                swoReaderRunning_.store(false, std::memory_order_release);
                swoActive_.store(false, std::memory_order_release);
                {
                        std::lock_guard io(mtx_);
                        if (isOpen_.load(std::memory_order_acquire)) {
                                JLINKARM_SWO_Control(JLINKARM_SWO_CMD_STOP, nullptr);
                                JLINKARM_SWO_DisableTarget(portMask);
                        }
                }
                setSwoError(std::string("SWO：创建读取线程失败（系统原始信息）：") + e.what());
                swoStatus_.store(SwoStatus::Error, std::memory_order_release);
                return false;
        }

        LOG_I("J-Link SWO started: CPU=%dMHz SWO=%dkHz ITM port=%d", swoCpuMHz_, swoSpeedKHz_, swoActivePort_);
        return true;
}

void
JLinkPort::swoStop()
{
        std::lock_guard lifecycle(swoLifecycleMtx_);
        swoStopImpl();
}

void
JLinkPort::swoStopImpl()
{
        swoReaderRunning_.store(false, std::memory_order_release);
        swoWakeCv_.notify_all();
        if (swoThread_.joinable())
                swoThread_.join();

        const bool wasActive = swoActive_.exchange(false, std::memory_order_acq_rel);
        if (wasActive) {
                const u32       portMask = 1u << static_cast<u32>(std::clamp(swoActivePort_, 0, 31));
                std::lock_guard io(mtx_);
                if (isOpen_.load(std::memory_order_acquire)) {
                        // Stop the host capture before touching target CoreSight
                        // state. This also prevents JLINKARM_Close from waiting
                        // on a receiver left over from the prev connection
                        JLINKARM_SWO_Control(JLINKARM_SWO_CMD_STOP, nullptr);
                        if (isConnected_.load(std::memory_order_acquire))
                                swoRestoreHardwareLocked();
                        JLINKARM_SWO_DisableTarget(portMask);
                }
        }

        {
                std::lock_guard data(swoDataMtx_);
                swoDecodePending_.clear();
        }
        swoStatus_.store(isConnected_.load(std::memory_order_acquire) ? SwoStatus::Stopped : SwoStatus::Disconnected,
                         std::memory_order_release);
}

void
JLinkPort::swoConsumeRaw(const u8 *data, const usize size)
{
        if (!data || size == 0)
                return;

        std::lock_guard lock(swoDataMtx_);
        swoDecodePending_.insert(swoDecodePending_.end(), data, data + size);

        // USB delivers SWO in batches. Reusing the host receive ts for
        // every packet makes fast interrupt events appear simultaneous. Anchor
        // the batch to host time, then distribute packets by their UART wire
        // position (8 data + start/stop = 10 bits per byte)
        const double batchEndUs =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - swoStartTime_).count();
        const double byteTimeUs = 10000.0 / static_cast<double>(std::max(swoSpeedKHz_, 1));
        const double batchStartUs =
            std::max(swoLastTimelineUs_, batchEndUs - byteTimeUs * static_cast<double>(swoDecodePending_.size()));

        const auto packetTimeUs = [&](const usize packetEnd) {
                return batchStartUs + byteTimeUs * static_cast<double>(packetEnd);
        };

        usize pos = 0;
        while (pos < swoDecodePending_.size()) {
                const usize packetStart = pos;
                const u8    header      = swoDecodePending_[pos++];

                // ITM source packet: bits[1:0] encode a 1/2/4-byte payload,
                // bit 2 selects hardware (DWT) versus software stimulus
                if ((header & 0x03u) != 0) {
                        const usize payloadSize = usize{1} << static_cast<unsigned>((header & 0x03u) - 1u);
                        if (swoDecodePending_.size() - pos < payloadSize) {
                                pos = packetStart;
                                break;
                        }
                        const bool isHardwarePacket = (header & 0x04u) != 0;
                        const int  port             = static_cast<int>(header >> 3u);
                        if (isHardwarePacket) {
                                u64 val = 0;
                                for (usize i = 0; i < payloadSize; ++i)
                                        val |= static_cast<u64>(swoDecodePending_[pos + i]) << (i * 8u);

                                const double eventTimeUs = packetTimeUs(pos + payloadSize);
                                if (port == 1 && payloadSize >= 2) {
                                        TraceEvent event;
                                        event.kind            = TraceEventKind::Exception;
                                        event.timeUs          = eventTimeUs;
                                        event.exceptionNumber = static_cast<u16>(val & 0x1FFu);
                                        event.action          = static_cast<u8>((val >> 12u) & 0x3u);
                                        swoEvents_.push_back(event);
                                } else if (port == 2) {
                                        // A four-byte packet contains a sampled PC. A short
                                        // packet denotes sleep/no executable PC
                                        const u32 pc = payloadSize == 4 ? static_cast<u32>(val) : 0xFFFFFFFFu;
                                        ++swoPcSamples_[pc];
                                        ++swoPcSampleTotal_;
                                        TraceEvent event;
                                        event.kind   = TraceEventKind::PcSample;
                                        event.timeUs = eventTimeUs;
                                        event.pc     = pc;
                                        swoEvents_.push_back(event);
                                } else if (port == 8 && payloadSize == 4) {
                                        // Comparator 0 data-trace PC packet
                                        swoLastWatchPc_ = static_cast<u32>(val);
                                } else if (port == 9) {
                                        TraceEvent event;
                                        event.kind      = TraceEventKind::DataWrite;
                                        event.timeUs    = eventTimeUs;
                                        event.pc        = swoLastWatchPc_;
                                        event.val       = val;
                                        event.valueSize = static_cast<u8>(payloadSize);
                                        swoEvents_.push_back(event);
                                }
                                while (swoEvents_.size() > kSwoMaxEvents)
                                        swoEvents_.pop_front();
                                swoLastTimelineUs_ = std::max(swoLastTimelineUs_, eventTimeUs);
                        } else if (port == swoActivePort_) {
                                for (usize i = 0; i < payloadSize; ++i) {
                                        const u8 ch = swoDecodePending_[pos + i];
                                        // Keep UTF-8 bytes and normal terminal controls. Replace
                                        // embedded NUL/other controls so ImGui cannot truncate text
                                        if (ch == '\n' || ch == '\r' || ch == '\t' || ch >= 0x20u)
                                                swoTextLog_.push_back(static_cast<char>(ch));
                                        else
                                                swoTextLog_.push_back('.');
                                }
                        }
                        pos += payloadSize;
                        continue;
                }

                if (header == 0x70u) {
                        swoOverflowPackets_.fetch_add(1, std::memory_order_relaxed);
                        continue;
                }
                if (header == 0x00u || header == 0x80u)
                        continue; // synchronization padding

                // Timestamp/extension packets use continuation bytes. We do not
                // display them in the text view, but must consume the full packet
                // to keep subsequent ITM stimulus packets aligned
                bool more = (header & 0x80u) != 0;
                while (more) {
                        if (pos >= swoDecodePending_.size()) {
                                pos = packetStart;
                                break;
                        }
                        more = (swoDecodePending_[pos++] & 0x80u) != 0;
                }
                if (pos == packetStart)
                        break;
        }

        if (pos > 0)
                swoDecodePending_.erase(swoDecodePending_.begin(), swoDecodePending_.begin() + pos);
        // Corrupt/noisy SWO input must not grow the partial-packet buffer forever
        if (swoDecodePending_.size() > 64u)
                swoDecodePending_.clear();
        if (swoTextLog_.size() > kSwoMaxTextBytes)
                swoTextLog_.erase(0, swoTextLog_.size() - kSwoMaxTextBytes);
}

void
JLinkPort::swoReaderLoop()
{
        std::vector<u8> buffer(kSwoReadBufferBytes);
        int             consecutiveErrors = 0;

        while (swoReaderRunning_.load(std::memory_order_acquire)) {
                if (!isOpen_.load(std::memory_order_acquire) || !isConnected_.load(std::memory_order_acquire)) {
                        setSwoError("SWO：J-Link 目标设备已断开");
                        swoStatus_.store(SwoStatus::Disconnected, std::memory_order_release);
                        swoActive_.store(false, std::memory_order_release);
                        break;
                }

                u32 numBytes = static_cast<u32>(buffer.size());
                int result   = -1;
                {
                        std::lock_guard io(mtx_);
                        if (isOpen_.load(std::memory_order_acquire) && isConnected_.load(std::memory_order_acquire)) {
                                result = JLINKARM_SWO_Read(buffer.data(), 0, &numBytes);
                                // SWO_Read uses an offset into the host buffer; it
                                // does not consume those bytes. Flush exactly the
                                // copied range so the next iteration reads new
                                // data instead of replaying the same packet
                                if (result >= 0 && numBytes > 0) {
                                        u32       flushBytes  = numBytes;
                                        const int flushResult = JLINKARM_SWO_Control(JLINKARM_SWO_CMD_FLUSH, &flushBytes);
                                        if (flushResult < 0)
                                                result = flushResult;
                                }
                        }
                }

                if (result < 0) {
                        if (++consecutiveErrors >= 3) {
                                char message[256];
                                snprintf(message, sizeof(message), "SWO：读取失败（错误码：%d）", result);
                                setSwoError(message);
                                swoStatus_.store(SwoStatus::Error, std::memory_order_release);
                        }
                        numBytes = 0;
                } else {
                        consecutiveErrors = 0;
                        if (numBytes > buffer.size())
                                numBytes = static_cast<u32>(buffer.size());
                        if (numBytes > 0) {
                                swoRxBytes_.fetch_add(numBytes, std::memory_order_relaxed);
                                swoConsumeRaw(buffer.data(), numBytes);
                        }
                }

                std::unique_lock wake(swoWakeMtx_);
                swoWakeCv_.wait_for(wake, numBytes > 0 ? std::chrono::milliseconds(1) : std::chrono::milliseconds(10), [this] {
                        return !swoReaderRunning_.load(std::memory_order_acquire);
                });
        }

        swoReaderRunning_.store(false, std::memory_order_release);
}

std::string
JLinkPort::swoTextSnapshot() const
{
        std::lock_guard lock(swoDataMtx_);
        return swoTextLog_;
}

std::string
JLinkPort::swoErrorSnapshot() const
{
        std::lock_guard lock(swoDataMtx_);
        return swoError_;
}

void
JLinkPort::swoClear()
{
        std::lock_guard lock(swoDataMtx_);
        swoTextLog_.clear();
        swoDecodePending_.clear();
        swoEvents_.clear();
        swoPcSamples_.clear();
        swoPcSampleTotal_ = 0;
        swoLastWatchPc_   = 0;
        swoRxBytes_.store(0, std::memory_order_relaxed);
        swoOverflowPackets_.store(0, std::memory_order_relaxed);
}

void
JLinkPort::setSwoError(const std::string &error)
{
        std::lock_guard lock(swoDataMtx_);
        swoError_ = error;
}

void
JLinkPort::drawSwoTraceWindow()
{
        if (!swoWindowOpen_)
                return;

        ImGui::SetNextWindowSize(ImVec2(680.0f, 460.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 320.0f),
                                            ImVec2(std::numeric_limits<float>::max(), std::numeric_limits<float>::max()));
        if (swoWindowFocusRequested_)
                ImGui::SetNextWindowFocus();
        swoWindowFocusRequested_ = false;

        if (!dock_close::begin(tr("SWO / ITM Trace###SwoItmTraceWindow", "SWO / ITM 跟踪###SwoItmTraceWindow"),
                               &swoWindowOpen_)) {
                ImGui::End();
                return;
        }

        const bool active = swoActive_.load(std::memory_order_acquire);
        ImGui::BeginDisabled(active);
        ImGui::SetNextItemWidth(115.0f);
        if (ImGui::InputInt(tr("CPU MHz", "CPU MHz"), &swoCpuMHz_, 0, 0))
                configModified_.store(true, std::memory_order_release);
        swoCpuMHz_ = std::max(swoCpuMHz_, 0);
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                                  tr("Enter the actual core clock; automatic measurement is disabled because it can hang",
                                     "请输入实际内核时钟；自动测量可能卡死，因此已禁用"));
        ImGui::SameLine();
        const std::vector<int> swoLevels = swoSpeedLevels(swoCpuMHz_);
        int                    swoLevel  = 0;
        for (int i = 1; i < static_cast<int>(swoLevels.size()); ++i) {
                if (std::abs(swoLevels[static_cast<usize>(i)] - swoSpeedKHz_) <
                    std::abs(swoLevels[static_cast<usize>(swoLevel)] - swoSpeedKHz_))
                        swoLevel = i;
        }
        // CPU clock edits immediately snap SWO to the nearest legal level
        swoSpeedKHz_ = swoLevels[static_cast<usize>(swoLevel)];
        char swoLevelText[32];
        snprintf(swoLevelText, sizeof(swoLevelText), "%d kHz", swoSpeedKHz_);
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderInt(tr("SWO kHz", "SWO kHz"),
                             &swoLevel,
                             0,
                             static_cast<int>(swoLevels.size()) - 1,
                             swoLevelText,
                             ImGuiSliderFlags_AlwaysClamp)) {
                swoSpeedKHz_ = swoLevels[static_cast<usize>(swoLevel)];
                configModified_.store(true, std::memory_order_release);
        }
        if (ImGui::IsItemHovered()) {
                std::string levelTip = tr("Exact divider levels: ", "可整除档位：");
                for (usize i = 0; i < swoLevels.size(); ++i) {
                        if (i != 0)
                                levelTip += " / ";
                        levelTip += std::to_string(swoLevels[i]);
                }
                levelTip += " kHz";
                ImGui::SetTooltip("%s", levelTip.c_str());
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::SliderInt(tr("ITM Port", "ITM 端口"), &swoItmPort_, 0, 31, "%d", ImGuiSliderFlags_AlwaysClamp))
                configModified_.store(true, std::memory_order_release);
        if (ImGui::Checkbox(tr("Exception trace", "异常/中断跟踪"), &swoExceptionTrace_))
                configModified_.store(true, std::memory_order_release);
        ImGui::SameLine();
        if (ImGui::Checkbox(tr("PC sampling", "PC 采样"), &swoPcSampling_))
                configModified_.store(true, std::memory_order_release);
        if (swoPcSampling_) {
                ImGui::SameLine();
                const char *rates[] = {tr("Fast", "快速"), tr("Normal", "普通"), tr("Slow", "低速")};
                ImGui::SetNextItemWidth(90.0f);
                if (ui::Choice("##swo_pc_rate", &swoPcSampleRate_, rates, IM_ARRAYSIZE(rates)))
                        configModified_.store(true, std::memory_order_release);
        }
        if (ImGui::Checkbox(tr("Trace variable writes", "跟踪变量写入"), &swoWatchWrite_))
                configModified_.store(true, std::memory_order_release);
        if (swoWatchWrite_) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(130.0f);
                if (ImGui::InputText("##swo_watch_address", swoWatchAddress_, sizeof(swoWatchAddress_)))
                        configModified_.store(true, std::memory_order_release);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(130.0f);
                if (ImGui::InputTextWithHint(
                        "##swo_watch_name", tr("Variable name", "变量名称"), swoWatchName_, sizeof(swoWatchName_)))
                        configModified_.store(true, std::memory_order_release);
                ImGui::SameLine();
                const char *sizes[] = {"1 byte", "2 bytes", "4 bytes"};
                int         sizeIdx = swoWatchSize_ >= 4 ? 2 : (swoWatchSize_ >= 2 ? 1 : 0);
                ImGui::SetNextItemWidth(85.0f);
                if (ui::Choice("##swo_watch_size", &sizeIdx, sizes, IM_ARRAYSIZE(sizes))) {
                        swoWatchSize_ = 1 << sizeIdx;
                        configModified_.store(true, std::memory_order_release);
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s",
                                          tr("Temporarily uses DWT comparator 0 and restores it when Trace stops.",
                                             "临时占用 DWT 比较器 0，停止跟踪时恢复原设置。"));
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s",
                            tr("SWO pin required. Exception, PC and DWT write trace need no firmware instrumentation.",
                               "需要连接 SWO 引脚；异常、PC 和 DWT 写入跟踪不需要修改固件。"));

        const SwoStatus status     = swoStatus_.load(std::memory_order_acquire);
        const char     *statusText = tr("Stopped", "已停止");
        switch (status) {
                case SwoStatus::Running:
                        statusText = tr("Running", "运行中");
                        break;
                case SwoStatus::Disconnected:
                        statusText = tr("J-Link disconnected", "J-Link 未连接");
                        break;
                case SwoStatus::Error:
                        statusText = tr("Trace error", "跟踪错误");
                        break;
                case SwoStatus::Stopped:
                        break;
        }
        if (status == SwoStatus::Error)
                ui::ErrorText("%s", statusText);
        else
                ImGui::TextUnformatted(statusText);
        ImGui::SameLine();
        ImGui::Text("| %s: %llu | %s: %llu",
                    tr("Raw bytes", "原始字节"),
                    static_cast<unsigned long long>(swoRxBytes_.load(std::memory_order_relaxed)),
                    tr("Overflow", "溢出"),
                    static_cast<unsigned long long>(swoOverflowPackets_.load(std::memory_order_relaxed)));

        if (active) {
                if (ui::SmallButton(tr("STOP TRACE", "停止跟踪"), ui::BtnStyle::Danger))
                        swoStop();
        } else {
                ImGui::BeginDisabled(!isConnected_.load(std::memory_order_acquire) || busy_.load(std::memory_order_acquire));
                if (ui::SmallButton(tr("START TRACE", "开始跟踪"), ui::BtnStyle::Success))
                        swoStart();
                ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Clear", "清空")))
                swoClear();

        const std::string error = swoErrorSnapshot();
        if (!error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::ErrorTextColor());
                ImGui::TextWrapped("%s", error.c_str());
                ImGui::PopStyleColor();
        }

        std::vector<TraceEvent>          events;
        std::vector<std::pair<u32, u64>> profile;
        u64                              profileTotal = 0;
        {
                std::lock_guard data(swoDataMtx_);
                events.assign(swoEvents_.begin(), swoEvents_.end());
                profile.assign(swoPcSamples_.begin(), swoPcSamples_.end());
                profileTotal = swoPcSampleTotal_;
        }
        std::sort(profile.begin(), profile.end(), [](const auto &a, const auto &b) { return a.second > b.second; });

        if (ImGui::BeginTabBar("##swo_trace_tabs")) {
                if (ImGui::BeginTabItem(tr("Timeline", "时间线"))) {
                        if (swoTimelineFollow_)
                                swoTimelineView_ = events;
                        const std::vector<TraceEvent> &timelineEvents = swoTimelineView_;
                        if (ImGui::BeginTable("##swo_timeline",
                                              4,
                                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                                  ImGuiTableFlags_Resizable,
                                              ImVec2(-1.0f, -1.0f))) {
                                ImGui::TableSetupColumn(tr("Time (us)", "时间 (us)"), ImGuiTableColumnFlags_WidthFixed, 115.0f);
                                ImGui::TableSetupColumn(tr("Type", "类型"), ImGuiTableColumnFlags_WidthFixed, 135.0f);
                                ImGui::TableSetupColumn(tr("Event", "事件"), ImGuiTableColumnFlags_WidthStretch);
                                ImGui::TableSetupColumn(tr("Value", "数值"), ImGuiTableColumnFlags_WidthFixed, 120.0f);
                                ImGui::TableHeadersRow();

                                const float timelineScrollY    = ImGui::GetScrollY();
                                const float timelineScrollMaxY = ImGui::GetScrollMaxY();
                                const bool  timelineAtBottom =
                                    timelineScrollMaxY <= 1.0f || timelineScrollY >= timelineScrollMaxY - 1.0f;
                                const bool timelineHovered  = ImGui::IsWindowHovered();
                                const bool timelineFocused  = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
                                const bool keyboardScrollUp = timelineFocused && (ImGui::IsKeyPressed(ImGuiKey_PageUp) ||
                                                                                  ImGui::IsKeyPressed(ImGuiKey_Home) ||
                                                                                  ImGui::IsKeyPressed(ImGuiKey_UpArrow));

                                // The scroll position is the only follow control. Moving away from
                                // the bottom freezes this snapshot; returning to the bottom resumes
                                // display updates and catches up with newly captured events
                                if (swoTimelineScrollFrames_ <= 0 &&
                                    ((timelineHovered && ImGui::GetIO().MouseWheel > 0.0f) || keyboardScrollUp ||
                                     (swoTimelineFollow_ && !timelineAtBottom))) {
                                        swoTimelineFollow_ = false;
                                } else if (!swoTimelineFollow_ && timelineAtBottom) {
                                        swoTimelineFollow_ = true;
                                        swoTimelineView_   = events;
                                }

                                ImGuiListClipper clipper;
                                clipper.Begin(static_cast<int>(timelineEvents.size()));
                                if (swoTimelineFollow_ && !timelineEvents.empty())
                                        clipper.IncludeItemByIndex(static_cast<int>(timelineEvents.size()) - 1);
                                while (clipper.Step()) {
                                        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                                                const TraceEvent &event = timelineEvents[static_cast<usize>(row)];
                                                ImGui::TableNextRow();
                                                ImGui::TableSetColumnIndex(0);
                                                ImGui::Text("%.1f", event.timeUs);
                                                ImGui::TableSetColumnIndex(1);
                                                if (event.kind == TraceEventKind::PcSample)
                                                        ImGui::TextUnformatted("PC");
                                                else if (event.kind == TraceEventKind::Exception)
                                                        ImGui::TextUnformatted(tr("Interrupt / exception", "中断/异常事件"));
                                                else
                                                        ImGui::TextUnformatted(tr("Data write", "变量写入"));
                                                ImGui::TableSetColumnIndex(2);
                                                if (event.kind == TraceEventKind::PcSample) {
                                                        ImGui::TextUnformatted(event.pc == 0xFFFFFFFFu
                                                                                   ? tr("CPU sleeping", "CPU 休眠")
                                                                                   : resolveTraceAddress(event.pc).c_str());
                                                } else if (event.kind == TraceEventKind::Exception) {
                                                        std::string exceptionName;
                                                        switch (event.exceptionNumber) {
                                                                case 0:
                                                                        exceptionName = tr("Thread mode", "线程模式");
                                                                        break;
                                                                case 2:
                                                                        exceptionName = "NMI";
                                                                        break;
                                                                case 3:
                                                                        exceptionName = "HardFault";
                                                                        break;
                                                                case 4:
                                                                        exceptionName = "MemManage";
                                                                        break;
                                                                case 5:
                                                                        exceptionName = "BusFault";
                                                                        break;
                                                                case 6:
                                                                        exceptionName = "UsageFault";
                                                                        break;
                                                                case 11:
                                                                        exceptionName = "SVCall";
                                                                        break;
                                                                case 12:
                                                                        exceptionName = "DebugMonitor";
                                                                        break;
                                                                case 14:
                                                                        exceptionName = "PendSV";
                                                                        break;
                                                                case 15:
                                                                        exceptionName = "SysTick";
                                                                        break;
                                                                default:
                                                                        if (event.exceptionNumber >= 16)
                                                                                exceptionName =
                                                                                    "IRQ" +
                                                                                    std::to_string(event.exceptionNumber - 16);
                                                                        else
                                                                                exceptionName =
                                                                                    "Exception " +
                                                                                    std::to_string(event.exceptionNumber);
                                                                        break;
                                                        }
                                                        const char *action = event.action == 1   ? tr("enter", "进入")
                                                                             : event.action == 2 ? tr("exit", "退出")
                                                                             : event.action == 3 ? tr("return", "返回")
                                                                                                 : tr("unknown", "未知");
                                                        ImGui::Text("%s — %s", exceptionName.c_str(), action);
                                                } else {
                                                        ImGui::Text("%s @ %s",
                                                                    swoWatchName_[0] ? swoWatchName_
                                                                                     : tr("watched address", "监视地址"),
                                                                    event.pc ? resolveTraceAddress(event.pc).c_str()
                                                                             : tr("PC unavailable", "PC 不可用"));
                                                }
                                                ImGui::TableSetColumnIndex(3);
                                                if (event.kind == TraceEventKind::DataWrite)
                                                        ImGui::Text("0x%0*llX",
                                                                    static_cast<int>(event.valueSize) * 2,
                                                                    static_cast<unsigned long long>(event.val));
                                                else if (event.kind == TraceEventKind::Exception)
                                                        ImGui::Text("%u", event.exceptionNumber);
                                                else
                                                        ImGui::Text("0x%08X", event.pc);
                                                if (swoTimelineFollow_ && row + 1 == static_cast<int>(timelineEvents.size()))
                                                        ImGui::SetScrollHereY(1.0f);
                                        }
                                }
                                if (swoTimelineScrollFrames_ > 0 && timelineScrollMaxY > 1.0f && timelineAtBottom)
                                        --swoTimelineScrollFrames_;
                                ImGui::EndTable();
                        }
                        ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(tr("CPU Profile", "CPU 占用"))) {
                        ImGui::Text(tr("Samples: %llu", "采样数: %llu"), static_cast<unsigned long long>(profileTotal));
                        if (ImGui::BeginTable("##swo_profile",
                                              4,
                                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                                  ImGuiTableFlags_Resizable,
                                              ImVec2(-1.0f, -1.0f))) {
                                ImGui::TableSetupColumn(tr("Function / Address", "函数 / 地址"));
                                ImGui::TableSetupColumn(tr("Address", "地址"), ImGuiTableColumnFlags_WidthFixed, 100.0f);
                                ImGui::TableSetupColumn(tr("Samples", "采样数"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
                                ImGui::TableSetupColumn(tr("CPU %", "CPU %"), ImGuiTableColumnFlags_WidthFixed, 75.0f);
                                ImGui::TableHeadersRow();
                                ImGuiListClipper clipper;
                                clipper.Begin(static_cast<int>(profile.size()));
                                while (clipper.Step()) {
                                        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                                                const auto &[pc, count] = profile[static_cast<usize>(row)];
                                                ImGui::TableNextRow();
                                                ImGui::TableSetColumnIndex(0);
                                                ImGui::TextUnformatted(pc == 0xFFFFFFFFu ? tr("CPU sleeping", "CPU 休眠")
                                                                                         : resolveTraceAddress(pc).c_str());
                                                ImGui::TableSetColumnIndex(1);
                                                if (pc == 0xFFFFFFFFu)
                                                        ImGui::TextUnformatted("-");
                                                else
                                                        ImGui::Text("0x%08X", pc);
                                                ImGui::TableSetColumnIndex(2);
                                                ImGui::Text("%llu", static_cast<unsigned long long>(count));
                                                ImGui::TableSetColumnIndex(3);
                                                ImGui::Text("%.2f",
                                                            profileTotal ? 100.0 * static_cast<double>(count) /
                                                                               static_cast<double>(profileTotal)
                                                                         : 0.0);
                                        }
                                }
                                ImGui::EndTable();
                        }
                        ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(tr("ITM Console", "ITM 控制台"))) {
                        std::string textLog = swoTextSnapshot();
                        ui::CopyLogButton(textLog);
                        ui::LogView("##swo_itm_text", textLog, ImVec2(-1.0f, -1.0f), &swoAutoScroll_);
                        ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
        }
        ImGui::End();
}

std::string
JLinkPort::prevError() const
{
        std::lock_guard lk(mtx_);
        return prevErr_;
}

void
JLinkPort::saveSession(void *node) const
{
        cJSON *jlink = static_cast<cJSON *>(node);
        if (!cJSON_IsObject(jlink))
                return;

        cJSON_AddStringToObject(jlink, "device", deviceName().c_str());
        cJSON_AddNumberToObject(jlink, "speedKHz", speed());
        cJSON_AddNumberToObject(jlink, "hssPeriodUs", hssPeriodUs_);
        cJSON_AddStringToObject(jlink, "dllPath", selectedDllPath().c_str());

        cJSON *trace = cJSON_CreateObject();
        cJSON_AddNumberToObject(trace, "cpuMHz", swoCpuMHz_);
        cJSON_AddNumberToObject(trace, "swoKHz", swoSpeedKHz_);
        cJSON_AddNumberToObject(trace, "itmPort", swoItmPort_);
        cJSON_AddBoolToObject(trace, "exceptionTrace", swoExceptionTrace_);
        cJSON_AddBoolToObject(trace, "pcSampling", swoPcSampling_);
        cJSON_AddNumberToObject(trace, "pcSampleRate", swoPcSampleRate_);
        cJSON_AddBoolToObject(trace, "watchWrite", swoWatchWrite_);
        cJSON_AddStringToObject(trace, "watchAddress", swoWatchAddress_);
        cJSON_AddStringToObject(trace, "watchName", swoWatchName_);
        cJSON_AddNumberToObject(trace, "watchSize", swoWatchSize_);
        cJSON_AddBoolToObject(trace, "windowOpen", swoWindowOpen_);
        cJSON_AddItemToObject(jlink, "trace", trace);

        cJSON *rtt = cJSON_CreateObject();
        cJSON_AddStringToObject(rtt, "controlBlock", rttControlBlock_);
        cJSON_AddNumberToObject(rtt, "upChannel", rttUpChannel_);
        cJSON_AddNumberToObject(rtt, "downChannel", rttDownChannel_);
        cJSON_AddBoolToObject(rtt, "appendNewline", rttAppendNewline_);
        cJSON_AddBoolToObject(rtt, "windowOpen", rttWindowOpen_);
        cJSON_AddItemToObject(jlink, "rtt", rtt);
}

void
JLinkPort::loadSession(const void *node)
{
        const cJSON *jlink = static_cast<const cJSON *>(node);
        if (!cJSON_IsObject(jlink))
                return;

        if (const cJSON *v = cJSON_GetObjectItem(jlink, "device"); cJSON_IsString(v))
                setDeviceName(v->valuestring);
        if (const cJSON *v = cJSON_GetObjectItem(jlink, "speedKHz"); cJSON_IsNumber(v))
                setSpeed(std::clamp(v->valueint, 1, 50000));
        if (const cJSON *v = cJSON_GetObjectItem(jlink, "hssPeriodUs"); cJSON_IsNumber(v))
                hssPeriodUs_ = std::max(v->valueint, 1);
        if (const cJSON *v = cJSON_GetObjectItem(jlink, "dllPath"); cJSON_IsString(v)) {
                std::lock_guard lk(dllMtx_);
                selectedDllPath_ = v->valuestring;
        }

        if (const cJSON *trace = cJSON_GetObjectItem(jlink, "trace"); cJSON_IsObject(trace)) {
                if (const cJSON *v = cJSON_GetObjectItem(trace, "cpuMHz"); cJSON_IsNumber(v))
                        swoCpuMHz_ = std::max(v->valueint, 1);
                if (const cJSON *v = cJSON_GetObjectItem(trace, "swoKHz"); cJSON_IsNumber(v))
                        swoSpeedKHz_ = std::max(v->valueint, 1);
                if (const cJSON *v = cJSON_GetObjectItem(trace, "itmPort"); cJSON_IsNumber(v))
                        swoItmPort_ = std::clamp(v->valueint, 0, 31);
                if (const cJSON *v = cJSON_GetObjectItem(trace, "exceptionTrace"); cJSON_IsBool(v))
                        swoExceptionTrace_ = cJSON_IsTrue(v);
                if (const cJSON *v = cJSON_GetObjectItem(trace, "pcSampling"); cJSON_IsBool(v))
                        swoPcSampling_ = cJSON_IsTrue(v);
                if (const cJSON *v = cJSON_GetObjectItem(trace, "pcSampleRate"); cJSON_IsNumber(v))
                        swoPcSampleRate_ = std::clamp(v->valueint, 0, 2);
                if (const cJSON *v = cJSON_GetObjectItem(trace, "watchWrite"); cJSON_IsBool(v))
                        swoWatchWrite_ = cJSON_IsTrue(v);
                if (const cJSON *v = cJSON_GetObjectItem(trace, "watchAddress"); cJSON_IsString(v))
                        snprintf(swoWatchAddress_, sizeof(swoWatchAddress_), "%s", v->valuestring);
                if (const cJSON *v = cJSON_GetObjectItem(trace, "watchName"); cJSON_IsString(v))
                        snprintf(swoWatchName_, sizeof(swoWatchName_), "%s", v->valuestring);
                if (const cJSON *v = cJSON_GetObjectItem(trace, "watchSize"); cJSON_IsNumber(v))
                        swoWatchSize_ = v->valueint >= 4 ? 4 : (v->valueint >= 2 ? 2 : 1);
                if (const cJSON *v = cJSON_GetObjectItem(trace, "windowOpen"); cJSON_IsBool(v))
                        swoWindowOpen_ = cJSON_IsTrue(v);
        }

        if (const cJSON *rtt = cJSON_GetObjectItem(jlink, "rtt"); cJSON_IsObject(rtt)) {
                if (const cJSON *v = cJSON_GetObjectItem(rtt, "controlBlock"); cJSON_IsString(v))
                        snprintf(rttControlBlock_, sizeof(rttControlBlock_), "%s", v->valuestring);
                if (const cJSON *v = cJSON_GetObjectItem(rtt, "upChannel"); cJSON_IsNumber(v))
                        rttUpChannel_ = std::max(v->valueint, 0);
                if (const cJSON *v = cJSON_GetObjectItem(rtt, "downChannel"); cJSON_IsNumber(v))
                        rttDownChannel_ = std::max(v->valueint, 0);
                if (const cJSON *v = cJSON_GetObjectItem(rtt, "appendNewline"); cJSON_IsBool(v))
                        rttAppendNewline_ = cJSON_IsTrue(v);
                if (const cJSON *v = cJSON_GetObjectItem(rtt, "windowOpen"); cJSON_IsBool(v))
                        rttWindowOpen_ = cJSON_IsTrue(v);
        }
        configModified_.store(false, std::memory_order_release);
}

void
JLinkPort::drawUI(bool stacked)
{
        if (ui::Button(tr("Remote devices…", "远程设备…"), ui::BtnStyle::Neutral))
                remoteWindowOpen_ = true;
        if (usingRemote()) {
                ImGui::TextUnformatted(tr("Source: remote device", "来源：远程设备"));
                ImGui::TextWrapped("%s",
                                   tr("Timestamped polling on the device · up to 50 Hz. Configure in Remote devices.",
                                      "设备端带时间戳轮询 · 最高 50 Hz。在远程设备中管理连接。"));
                return;
        }
        if (ava_peer::remoteOwnsWrites()) {
                ImGui::TextWrapped("%s",
                                   tr("Remote controller owns writes. Stop sharing in Remote devices to regain control.",
                                      "远端持有写入控制权。可在远程设备中停止共享，收回控制权。"));
                return;
        }
        const bool rttActive = rttActive_.load(std::memory_order_acquire);
        auto       rttButton = [&] {
                if (ui::Button(stacked ? tr("RTT terminal", "RTT 终端") : (rttActive ? "RTT ON" : "RTT"),
                               rttActive ? ui::BtnStyle::Success : ui::BtnStyle::Neutral)) {
                        rttWindowOpen_           = true;
                        rttWindowFocusRequested_ = true;
                }
        };
        if (stacked) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(tr("Connection", "连接"));
                const float rttWidth =
                    ImGui::CalcTextSize(tr("RTT terminal", "RTT 终端")).x + ImGui::GetStyle().FramePadding.x * 2;
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - rttWidth);
                rttButton();
                ImGui::Spacing();
        }
        const float fieldX     = ImGui::GetCursorPosX() + ImGui::GetFontSize() * 5.5f;
        auto        fieldLabel = [&](const char *label) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::SameLine(fieldX);
        };
        // Read config via the lightweight cfgMtx_ getters — never the J-Link I/O
        // FairMutex — so the render thread doesn't stall behind slow J-Link reads
        char        nameBuf[64];
        std::string dev = deviceName();
        snprintf(nameBuf, sizeof(nameBuf), "%s", dev.c_str());

        char btnLabel[128];
        snprintf(
            btnLabel, sizeof(btnLabel), "%s##jlink_port_btn", nameBuf[0] != '\0' ? nameBuf : tr("Select Device", "选择设备"));
        if (stacked)
                fieldLabel(tr("Device", "目标芯片"));
        if (ImGui::Button(btnLabel, ImVec2(stacked ? ImGui::GetContentRegionAvail().x : 180.0f, 0))) {
#ifdef _WIN32
                std::string loadError;
                if (!g_jlinkApi.load(selectedDllPath(), loadError)) {
                        std::lock_guard lk(mtx_);
                        prevErr_ = std::move(loadError);
                } else {
                        std::lock_guard dllLock(dllMtx_);
                        loadedDllPath_ = g_jlinkApi.resolvedPath();
                }
                if (loadError.empty())
#endif
                {
                        JLINKARM_DEVICE_SELECT_INFO sinfo;
                        sinfo.SizeOfStruct = sizeof(sinfo);
                        sinfo.CoreIndex    = 0;
                        int devIdx         = JLINKARM_DEVICE_SelectDialog(nullptr, 0, &sinfo);
                        if (devIdx >= 0) {
                                JLINKARM_DEVICE_INFO dinfo;
                                dinfo.SizeOfStruct = sizeof(dinfo);
                                if (JLINKARM_DEVICE_GetInfo(devIdx, &dinfo) >= 0)
                                        if (deviceName() != dinfo.sName) {
                                                setDeviceName(dinfo.sName);
                                                configModified_.store(true, std::memory_order_release);
                                        }
                        }
                }
        }
        TutorialGuide::instance().mark("device_btn");
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Device", "设备"));
        if (!stacked)
                ImGui::SameLine();
        else
                fieldLabel(tr("SWD speed", "SWD 速度"));
        ImGui::SetNextItemWidth(stacked ? -FLT_MIN : 180.0f);
        // Logarithmic slider: SWD speeds span a wide range (kHz .. tens of MHz)
        int spd = speedKHz_.load(std::memory_order_relaxed);
        if (ImGui::SliderInt("##jlinkSpeed", &spd, 5, 50000, "%d kHz", ImGuiSliderFlags_Logarithmic))
                configModified_.store(true, std::memory_order_release);
        TutorialGuide::instance().mark("vel_slider");
        const bool speedCommitted = ImGui::IsItemDeactivatedAfterEdit();
        if (spd < 1)
                spd = 1;
        speedKHz_.store(spd, std::memory_order_relaxed);
        if (speedCommitted && isOpen_) {
                // One-off J-Link call on slider release (rare) — OK under the I/O lock
                std::lock_guard lk(mtx_);
                JLINKARM_SetSpeed(static_cast<u32>(spd));
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "%s",
                    ui::ShortcutLabel(tr("J-Link SWD speed (kHz). Drag to set; applies when you release. Ctrl+click to type.",
                                         "J-Link SWD 速度 (kHz)。拖动设置，松开后生效。Ctrl+点击可输入。"),
                                      tr("J-Link SWD speed (kHz). Drag to set; applies when you release. Cmd+click to type.",
                                         "J-Link SWD 速度 (kHz)。拖动设置，松开后生效。Cmd+点击可输入。")));

        if (stacked) {
                const double rate = sampleBytesPerSecond();
                ImGui::Text(tr("Sampling: %.1f %s", "采样吞吐: %.1f %s"),
                            rate >= 1000000 ? rate / 1000000
                            : rate >= 1000  ? rate / 1000
                                            : rate,
                            rate >= 1000000 ? "MB/s"
                            : rate >= 1000  ? "KB/s"
                                            : "B/s");
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s",
                                          tr("Measured HSS stream bytes plus successful POLL reads per second.",
                                             "按实际接收的 HSS 流和成功的 POLL 读取字节数计时。"));
        }

        if (!stacked)
                ImGui::SameLine();
        const bool busy      = busy_.load(std::memory_order_acquire);
        const bool connected = isConnected_.load(std::memory_order_acquire);
        if (stacked)
                ImGui::Spacing();
        const float actionWidth  = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        auto        actionButton = [&](const char *label, ui::BtnStyle tone) {
                return stacked ? ui::Button(label, tone, ImVec2(actionWidth, 0)) : ui::SmallButton(label, tone);
        };
        if (busy) {
                ImGui::BeginDisabled();
                actionButton(connected ? tr("Working...", "处理中...") : tr("Connecting...", "连接中..."), ui::BtnStyle::Muted);
                ImGui::EndDisabled();
        } else if (!connected) {
                if (actionButton(tr("Connect", "连接"), ui::BtnStyle::Success))
                        connectAsync();
                TutorialGuide::instance().mark("connect_btn");
        } else {
                if (actionButton(tr("Disconnect", "断开"), ui::BtnStyle::Danger))
                        disconnectAsync();
                TutorialGuide::instance().mark("connect_btn");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(busy        ? tr("J-Link operation in progress", "正在处理 J-Link 操作")
                                       : connected ? tr("Disconnect J-Link", "断开 J-Link 连接")
                                                   : tr("Connect J-Link", "连接 J-Link"));
                if (connected) {
                        const auto buildStatus = firmwareStatus();
                        const auto buildColor  = buildStatus.state == firmware_match::State::Matched
                                                     ? ImVec4(0.35f, 0.85f, 0.45f, 1.0f)
                                                 : buildStatus.allowsAccess() ? ImVec4(0.95f, 0.72f, 0.25f, 1.0f)
                                                                              : ImVec4(1.0f, 0.35f, 0.30f, 1.0f);
                        ImGui::Separator();
                        ImGui::TextColored(buildColor, "%s", firmwareStatusLabel(buildStatus));
                        if (!buildStatus.detail.empty())
                                ImGui::TextWrapped("%s", firmwareStatusDetail(buildStatus));
                        if (!buildStatus.deviceId.empty())
                                ImGui::Text("Device: %s", buildStatus.deviceId.c_str());
                        if (!buildStatus.expectedId.empty())
                                ImGui::Text("AXF: %s", buildStatus.expectedId.c_str());
                }
                ImGui::EndTooltip();
        }
        if ((!busy || stacked) && !connected) {
                ImGui::SameLine();
                ImGui::BeginDisabled(busy);
                if (actionButton(tr("Connect and reset", "连接并复位"), ui::BtnStyle::Warning))
                        connectAsync(true);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s",
                                          tr("Connect, reset the MCU, then run and start acquisition.",
                                             "连接成功后复位 MCU，重新运行程序，再开始采集。"));
        }
        if ((!busy || stacked) && connected) {
                ImGui::SameLine();
                ImGui::BeginDisabled(busy);
                if (actionButton(tr("Reset MCU", "复位 MCU"), ui::BtnStyle::Warning))
                        resetAsync();
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Reset the connected target", "复位当前连接的目标"));
        }

        if (!stacked) {
                ImGui::SameLine();
                rttButton();
        }
        if (!prevErr_.empty()) {
                if (!stacked)
                        ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ui::ErrorTextColor());
                ImGui::TextWrapped(tr("Error: %s", "错误: %s"), prevErr_.c_str());
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "%s",
                            tr("Try reducing the sampling frequency (Hz) if you see 'Low on memory' or 'Start failed'.",
                               "若出现 'Low on memory' 或 'Start failed'，请尝试降低采样频率 (Hz)。"));
                ImGui::PopStyleColor();
        }
}

#ifdef _WIN32
static std::string
jlinkDllFileVersion(const std::string &path)
{
        DWORD       ignored = 0;
        const DWORD size    = GetFileVersionInfoSizeA(path.c_str(), &ignored);
        if (size == 0)
                return "Unknown";
        std::vector<unsigned char> data(size);
        if (!GetFileVersionInfoA(path.c_str(), 0, size, data.data()))
                return "Unknown";

        struct LanguageAndCodePage {
                WORD language;
                WORD codePage;
        };
        LanguageAndCodePage *translations    = nullptr;
        UINT                 translationSize = 0;
        if (VerQueryValueA(
                data.data(), "\\VarFileInfo\\Translation", reinterpret_cast<void **>(&translations), &translationSize) &&
            translations) {
                const UINT count = translationSize / sizeof(LanguageAndCodePage);
                for (UINT i = 0; i < count; ++i) {
                        char query[96];
                        snprintf(query,
                                 sizeof(query),
                                 "\\StringFileInfo\\%04x%04x\\FileVersion",
                                 translations[i].language,
                                 translations[i].codePage);
                        char *text       = nullptr;
                        UINT  textLength = 0;
                        if (VerQueryValueA(data.data(), query, reinterpret_cast<void **>(&text), &textLength) && text &&
                            textLength > 1)
                                return text;
                }
        }
        VS_FIXEDFILEINFO *info     = nullptr;
        UINT              infoSize = 0;
        if (!VerQueryValueA(data.data(), "\\", reinterpret_cast<void **>(&info), &infoSize) || !info)
                return "Unknown";
        char version[64];
        snprintf(version,
                 sizeof(version),
                 "%u.%u.%u.%u",
                 HIWORD(info->dwFileVersionMS),
                 LOWORD(info->dwFileVersionMS),
                 HIWORD(info->dwFileVersionLS),
                 LOWORD(info->dwFileVersionLS));
        return version;
}

static const char *
jlinkDllFileName()
{
#if defined(_M_ARM64)
        return "JLink_arm64.dll";
#else
        return "JLink_x64.dll";
#endif
}

static std::string
jlinkBundledDllPath()
{
        std::vector<char> executable(32768);
        const DWORD       length = GetModuleFileNameA(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (length == 0 || length >= executable.size())
                return jlinkDllFileName();
        return (std::filesystem::path(std::string(executable.data(), length)).parent_path() / jlinkDllFileName()).string();
}

static std::string
jlinkResolvedDefaultDllPath()
{
        const std::string bundled = jlinkBundledDllPath();
        std::error_code   ec;
        if (std::filesystem::is_regular_file(bundled, ec))
                return bundled;

        std::vector<char> resolved(32768);
        const DWORD       length =
            SearchPathA(nullptr, jlinkDllFileName(), nullptr, static_cast<DWORD>(resolved.size()), resolved.data(), nullptr);
        if (length > 0 && length < resolved.size())
                return std::string(resolved.data(), length);
        return bundled;
}
#endif

void
JLinkPort::startDllScan()
{
#ifndef _WIN32
        return;
#else
        if (dllScanning_.exchange(true, std::memory_order_acq_rel))
                return;
        dllScanCompleted_.store(false, std::memory_order_release);
        dllScanCancel_.store(false, std::memory_order_release);
        if (dllScanThread_.joinable())
                dllScanThread_.join();
        {
                std::lock_guard lk(dllMtx_);
                dllCandidates_.clear();
                dllScanLocation_ = "Preparing drive scan...";
        }
        dllScanThread_ = std::thread([this]() {
                std::vector<DllCandidate>       found;
                std::unordered_set<std::string> seen;
#if defined(_M_ARM64)
                const std::string wanted = "jlink_arm64.dll";
#else
                const std::string wanted = "jlink_x64.dll";
#endif
                auto addCandidate = [&](const std::filesystem::path &path) {
                        std::string full = path.lexically_normal().string();
                        std::string key  = full;
                        std::ranges::transform(
                            key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (!seen.insert(key).second)
                                return;
                        found.push_back({full, jlinkDllFileVersion(full)});
                        std::sort(found.begin(), found.end(), [](const DllCandidate &a, const DllCandidate &b) {
                                if (a.version != b.version)
                                        return a.version > b.version;
                                return a.path < b.path;
                        });
                        std::lock_guard lk(dllMtx_);
                        dllCandidates_ = found;
                };

                const DWORD drives = GetLogicalDrives();
                for (int drive = 0; drive < 26 && !dllScanCancel_.load(std::memory_order_acquire); ++drive) {
                        if ((drives & (1u << drive)) == 0)
                                continue;
                        char rootText[] = "A:\\";
                        rootText[0]     = static_cast<char>('A' + drive);
                        const UINT kind = GetDriveTypeA(rootText);
                        if (kind != DRIVE_FIXED && kind != DRIVE_REMOVABLE)
                                continue;
                        std::error_code                               ec;
                        std::filesystem::recursive_directory_iterator it(
                            rootText, std::filesystem::directory_options::skip_permission_denied, ec);
                        std::filesystem::recursive_directory_iterator end;
                        size_t                                        visited = 0;
                        while (it != end && !dllScanCancel_.load(std::memory_order_acquire)) {
                                if (!ec && it->is_regular_file(ec)) {
                                        std::string fileName = it->path().filename().string();
                                        std::ranges::transform(fileName, fileName.begin(), [](unsigned char c) {
                                                return static_cast<char>(std::tolower(c));
                                        });
                                        if (fileName == wanted)
                                                addCandidate(it->path());
                                }
                                if ((++visited % 1000) == 0) {
                                        std::lock_guard lk(dllMtx_);
                                        dllScanLocation_ = it->path().parent_path().string();
                                }
                                it.increment(ec);
                                if (ec)
                                        ec.clear();
                        }
                }
                {
                        std::lock_guard lk(dllMtx_);
                        dllScanLocation_ = "Scan complete";
                }
                dllScanCompleted_.store(true, std::memory_order_release);
                dllScanning_.store(false, std::memory_order_release);
        });
#endif
}

void
JLinkPort::drawDllSettingsMenu()
{
        const bool  connected = isOpen() || isConnected() || isBusy();
        std::string selected  = selectedDllPath();
#ifdef _WIN32
        const std::string         bundledPath     = jlinkBundledDllPath();
        const std::string         bundledVersion  = jlinkDllFileVersion(bundledPath);
        const std::string         selectedPath    = selected.empty() ? jlinkResolvedDefaultDllPath() : selected;
        const std::string         selectedVersion = jlinkDllFileVersion(selectedPath);
        std::string               loadedPath;
        std::vector<DllCandidate> candidates;
        std::string               location;
        {
                std::lock_guard lk(dllMtx_);
                loadedPath = loadedDllPath_;
                candidates = dllCandidates_;
                location   = dllScanLocation_;
        }

        ImGui::TextDisabled("%s", tr("Current:", "当前："));
        ImGui::Text("%s", selectedVersion.c_str());
        ImGui::TextWrapped("%s", selectedPath.c_str());
        ImGui::TextDisabled(tr("Bundled: %s", "内置：%s"), bundledVersion.c_str());
        if (!loadedPath.empty())
                ImGui::TextDisabled(tr("Loaded: %s", "已加载：%s"), jlinkDllFileVersion(loadedPath).c_str());
#else
        std::vector<DllCandidate> candidates;
        std::string               location;
        {
                std::lock_guard lk(dllMtx_);
                candidates = dllCandidates_;
                location   = dllScanLocation_;
        }
        ImGui::TextDisabled("%s", tr("Current:", "当前："));
        ImGui::TextWrapped("%s", selected.empty() ? tr("Bundled / system default", "内置 / 系统默认") : selected.c_str());
#endif
        ImGui::Separator();
        ImGui::BeginDisabled(connected);
        if (ImGui::MenuItem(tr("Change...", "更改..."))) {
                const std::string path = nativeDlgOpen(tr("Select J-Link DLL", "选择 J-Link DLL"), {{"J-Link DLL", {"dll"}}});
                if (!path.empty()) {
                        std::lock_guard lk(dllMtx_);
                        selectedDllPath_ = path;
                        configModified_.store(true, std::memory_order_release);
                }
        }
        if (ImGui::MenuItem(
                tr("Reset to bundled / system default", "重置为内置 / 系统默认"), nullptr, false, !selected.empty())) {
                std::lock_guard lk(dllMtx_);
                selectedDllPath_.clear();
                configModified_.store(true, std::memory_order_release);
        }
        if (ImGui::BeginMenu(tr("Detected versions", "检测到的版本"), !candidates.empty())) {
                for (const auto &candidate : candidates) {
                        std::string label = candidate.version + "##" + candidate.path;
                        if (ImGui::MenuItem(label.c_str(), nullptr, selected == candidate.path)) {
                                std::lock_guard lk(dllMtx_);
                                selectedDllPath_ = candidate.path;
                                configModified_.store(true, std::memory_order_release);
                        }
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", candidate.path.c_str());
                }
                ImGui::EndMenu();
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        if (dllScanning_.load(std::memory_order_acquire)) {
                ImGui::TextDisabled("%s", tr("Scanning drives...", "正在扫描磁盘..."));
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", location.c_str());
                if (ImGui::MenuItem(tr("Stop Scan", "停止扫描")))
                        dllScanCancel_.store(true, std::memory_order_release);
        } else if (ImGui::MenuItem(tr("Scan All Drives", "扫描所有磁盘"))) {
                startDllScan();
        }
        if (connected) {
                ImGui::Separator();
                ImGui::TextDisabled("%s", tr("Disconnect J-Link before changing.", "断开 J-Link 后才可更改。"));
        }
}

void
JLinkPort::drawWindows()
{
        drawRemoteDevices();
        drawRttWindow();
        drawSwoTraceWindow();
}

bool
JLinkPort::readFlash(u32                               addr,
                     u32                               numBytes,
                     std::vector<uint8_t>             &out,
                     std::string                      &error,
                     const std::atomic<bool>          &cancel,
                     const std::function<void(float)> &progress)
{
        auto route = lockRoute();
        if (usingRemote()) {
                error = "远端未提供 Flash 导出能力";
                return false;
        }
        if (!numBytes || numBytes > 16u * 1024u * 1024u || uint64_t(addr) + numBytes > 0x100000000ULL) {
                error = "Flash 地址范围无效（最大允许 16 MiB）。";
                return false;
        }
        bool expected = false;
        if (!busy_.compare_exchange_strong(expected, true)) {
                error = "J-Link 正忙。";
                return false;
        }
        struct BusyGuard {
                std::atomic<bool> &state;
                ~BusyGuard() { state.store(false); }
        } busyGuard{busy_};
        std::vector<uint8_t> bytes(numBytes);
        std::lock_guard      lock(mtx_);
        if (!isOpen_ || !isConnected_) {
                error = "J-Link 未连接。";
                return false;
        }
        for (u32 offset = 0; offset < numBytes;) {
                if (cancel.load()) {
                        error = "已取消 Flash 读取。";
                        return false;
                }
                const u32 count    = std::min<u32>(4096, numBytes - offset);
                const int received = JLINKARM_ReadMemEx(addr + offset, count, bytes.data() + offset, 0);
                if (received != static_cast<int>(count)) {
                        char message[256];
                        snprintf(message,
                                 sizeof(message),
                                 "读取 Flash 失败，地址 0x%08X：预期读取 %u 字节，实际返回 %d。",
                                 addr + offset,
                                 count,
                                 received);
                        error = message;
                        recordTransportResultLocked(false, "readFlash");
                        return false;
                }
                offset += count;
                if (progress)
                        progress(float(offset) / float(numBytes));
        }
        recordTransportResultLocked(true, "readFlash");
        out = std::move(bytes);
        return true;
}
