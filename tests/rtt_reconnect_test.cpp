#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
// Replace just the J-Link implementation object so its private DLL dispatch
// can use an in-process SDK fixture. No probe or SEGGER DLL is accessed
#include "../src/core/jlink_port.cpp"
#include <stdexcept>

namespace {
std::atomic<bool> sdkOpen{false};
std::atomic<int> resets{0}, runs{0}, resetFailure{0}, runFailure{0};
std::atomic<bool> halted{false}, stuckHalt{false};
uint32_t heartbeatValue = 0;
std::vector<std::string> attachCommands;
std::string rejectedAttachCommand;
std::string sdkDevice;
bool disconnectScriptSelected = false;
int connectCalls{};
std::vector<std::string> sdkLogPaths;
int sdkOpenCalls{}, diagnosticReads{}, timingReads{};
bool restartOnClose = true;
std::atomic<bool> debugPowerDownOnClose{true}, skipDebugDeInit{false}, dwtClockEnabled{true};
std::atomic<int> starts{0}, connectFailure{0}, startFailure{0}, receivedPacket{0}, readsWhileClosed{0};
firmware_match::Bytes runtimeIdentity{}, flashIdentity{};
constexpr uint32_t sampleAddress = 0x24001000U;
constexpr uint32_t identityAddress = 0x08001000U;
constexpr uint32_t resetAddress = 0x08000401U;
uint32_t sampleValue = 0x12345678U;
int sampleReads{}, sampleWrites{}, hssStarts{}, hssStops{}, hssReads{};
bool identityReadable = true;

/**
 * @brief 构造设备发布的身份, 所有访问均由进程内 SDK 返回
 */
void putIdentityWord(firmware_match::Bytes &bytes, size_t offset, uint32_t val) {
        for (size_t index = 0; index < 4; ++index)
                bytes[offset + index] = uint8_t(val >> (index * 8));
}

int fixtureRead(uint32_t address, uint32_t size, void *output, uint32_t) {
        if (address == 0x20004C20u && size == 4) {
                if (!halted.load()) ++heartbeatValue;
                std::memcpy(output, &heartbeatValue, size);
                return int(size);
        }
        if (address == 0x20004E6Cu && size == 16) {
                ++timingReads;
                const uint32_t snapshot[] = {1, 1, 2097152, 162774};
                std::memcpy(output, snapshot, size);
                return int(size);
        }
        if (size == 4 && (address == 0xE000EDFCu || address == 0xE0001000u || address == 0xE0001004u)) {
                ++diagnosticReads;
                const uint32_t value = address == 0xE000EDFCu ? (1u << 24u) : address == 0xE0001000u ? 1u : 12345u;
                std::memcpy(output, &value, size);
                return 4;
        }
        if (address == sampleAddress && size == sizeof(sampleValue)) {
                ++sampleReads;
                std::memcpy(output, &sampleValue, size);
                return int(size);
        }
        if (!identityReadable) return -1;
        if (size == firmware_match::infoSize &&
            (address == firmware_match::mailboxAddress || address == identityAddress)) {
                const auto &identity = address == firmware_match::mailboxAddress ? runtimeIdentity : flashIdentity;
                std::memcpy(output, identity.data(), size);
                return int(size);
        }
        if (size == 4 && (address == 0xE000ED08U || address == 4)) {
                const uint32_t val = address == 4 ? resetAddress : 0;
                std::memcpy(output, &val, size);
                return int(size);
        }
        return -1;
}
}

struct RttReconnectTestAccess {
        static bool start(JLinkPort &port) { return port.rttStart(); }
        static void stop(JLinkPort &port) { port.rttStop(); }
        static bool active(JLinkPort &port) { return port.rttActive_.load(); }
        static bool wanted(JLinkPort &port) { return port.rttResumeRequested_.load(); }
        static bool send(JLinkPort &port) { return port.rttSend("stale", 5); }
        static usize pending(JLinkPort &port) { return port.rttTxPendingBytes(); }
        static std::string rx(JLinkPort &port) { return port.rttRxSnapshot(); }
        static void loseConnection(JLinkPort &port) { port.isConnected_.store(false); }
        static void refreshIdentity(JLinkPort &port) {
                std::lock_guard lock(port.mtx_);
                port.firmwareCheckedAt_ = {};
        }
        static void status(JLinkPort &port, firmware_match::State state) {
                std::lock_guard lock(port.firmwareStatusMtx_);
                port.firmwareStatus_ = {state, "fixture identity detail", "device-build", "expected-build"};
        }
};

static void check(bool ok, const char *message) {
        if (!ok) throw std::runtime_error(message);
}
template <class F> static void waitFor(F predicate, const char *message) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!predicate() && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        check(predicate(), message);
}

/**
 * @brief 验证强制使用只绕过本次身份不匹配, 不绕过连接和会话边界
 */
static void checkFirmwareOverride(JLinkPort &port) {
        firmware_match::Image source;
        source.address = identityAddress;
        source.path = "override-fixture.axf";
        putIdentityWord(source.info, 0, firmware_match::magic);
        putIdentityWord(source.info, 4, 1);
        putIdentityWord(source.info, 8, firmware_match::infoSize);
        putIdentityWord(source.info, 12, identityAddress);
        putIdentityWord(source.info, 56, resetAddress);
        source.info[16] = 1;
        flashIdentity = source.info;
        flashIdentity[16] = 2;
        runtimeIdentity = flashIdentity;
        putIdentityWord(runtimeIdentity, 48, 1);
        putIdentityWord(runtimeIdentity, 52, ~1U);
        g_jlinkApi.ReadMemEx = fixtureRead;
        g_jlinkApi.WriteMemEx = +[](uint32_t address, uint32_t size, const void *data, uint32_t) -> int {
                if (address != sampleAddress || size != sizeof(sampleValue)) return -1;
                ++sampleWrites;
                std::memcpy(&sampleValue, data, size);
                return int(size);
        };
        g_jlinkApi.HSS_Start = +[](JLINK_HSS_MEM_BLOCK_DESC *, int32_t, int32_t, int32_t) -> int {
                ++hssStarts;
                return 0;
        };
        g_jlinkApi.HSS_Stop = +[]() -> int { ++hssStops; return 0; };
        g_jlinkApi.HSS_Read = +[](void *output, uint32_t size) -> int {
                ++hssReads;
                if (size < 8) return -1;
                std::memset(output, 0, 8);
                std::memcpy(static_cast<uint8_t *>(output) + 4, &sampleValue, sizeof(sampleValue));
                return 8;
        };

        constexpr uintptr_t owner = 0xA11U;
        const std::vector<HssBlock> blocks{{sampleAddress, sizeof(sampleValue)}};
        uint32_t result{}, desired = 0x87654321U;
        std::array<uint8_t, 8> frame{};
        port.setFirmwareSource(owner, source);
        auto epoch = port.targetEpoch();
        check(!port.readMem(sampleAddress, sizeof(result), &result) &&
              !port.writeMem(sampleAddress, sizeof(desired), &desired) &&
              !port.readMemAtEpoch(epoch, sampleAddress, sizeof(result), &result) &&
              !port.writeMemAtEpoch(epoch, sampleAddress, sizeof(desired), &desired) &&
              !port.writeMemBitfield(sampleAddress, sizeof(desired), &desired, 0, 4, epoch) &&
              !port.hssStart(blocks, 1000, epoch), "mismatch blocks every memory and HSS access path");
        check(sampleReads == 0 && sampleWrites == 0 && hssStarts == 0 &&
              port.firmwareStatus().state == firmware_match::State::Mismatch && !port.firmwareStatus().forced,
              "blocked mismatch never dispatches memory or sampling SDK calls");

        check(port.setFirmwareMismatchOverride(true), "explicit override accepts a verified mismatch");
        check(port.firmwareStatus().forced && port.firmwareStatus().state == firmware_match::State::Mismatch,
              "override preserves the reported mismatch");
        epoch = port.targetEpoch();
        check(port.readMem(sampleAddress, sizeof(result), &result) && result == sampleValue &&
              port.writeMem(sampleAddress, sizeof(desired), &desired) && sampleValue == desired &&
              port.readMemAtEpoch(epoch, sampleAddress, sizeof(result), &result) && result == desired &&
              port.writeMemAtEpoch(epoch, sampleAddress, sizeof(desired), &desired),
              "override restores direct and epoch-bound memory access");
        const uint32_t nibble = 5;
        check(port.writeMemBitfield(sampleAddress, sizeof(nibble), &nibble, 4, 4, epoch) &&
              sampleValue == ((desired & ~0xF0U) | 0x50U), "override restores atomic bitfield updates");
        check(port.hssStart(blocks, 1000, epoch) && port.hssRead(frame.data(), uint32_t(frame.size())) == 8 &&
              hssStarts == 1 && hssReads == 1, "override restores HSS start and frame reads");

        const int readsBeforeDisable = sampleReads, writesBeforeDisable = sampleWrites;
        check(port.setFirmwareMismatchOverride(false) && !port.firmwareStatus().forced && !port.isHssRunning() &&
              hssStops == 1, "disabling override immediately stops active HSS");
        check(!port.readMem(sampleAddress, sizeof(result), &result) &&
              !port.writeMem(sampleAddress, sizeof(desired), &desired) &&
              port.hssRead(frame.data(), uint32_t(frame.size())) == 0 &&
              sampleReads == readsBeforeDisable && sampleWrites == writesBeforeDisable && hssReads == 1,
              "disabled override rejects reads, writes and stale HSS frames");

        check(port.setFirmwareMismatchOverride(true), "override can be explicitly enabled again");
        const auto oldEpoch = port.targetEpoch();
        check(port.hssStart(blocks, 1000, oldEpoch), "HSS starts before changing symbol source");
        source.path = "replacement-fixture.elf";
        port.setFirmwareSource(owner, source);
        check(!port.isHssRunning() && !port.firmwareStatus().forced &&
              !port.readMem(sampleAddress, sizeof(result), &result), "loading another AXF or ELF clears override");
        check(port.setFirmwareMismatchOverride(true), "replacement source requires a fresh override");
        const int readsBeforeStale = sampleReads, writesBeforeStale = sampleWrites, startsBeforeStale = hssStarts;
        check(!port.readMemAtEpoch(oldEpoch, sampleAddress, sizeof(result), &result) &&
              !port.writeMemAtEpoch(oldEpoch, sampleAddress, sizeof(desired), &desired) &&
              !port.writeMemBitfield(sampleAddress, sizeof(desired), &desired, 0, 4, oldEpoch) &&
              !port.hssStart(blocks, 1000, oldEpoch) && sampleReads == readsBeforeStale &&
              sampleWrites == writesBeforeStale && hssStarts == startsBeforeStale,
              "override never authorizes operations queued against an old target epoch");

        port.close();
        check(!port.setFirmwareMismatchOverride(true), "disconnected targets cannot be forced");
        check(port.open() && port.connect() && !port.readMem(sampleAddress, sizeof(result), &result) &&
              !port.firmwareStatus().forced, "reconnection does not retain override");
        check(port.setFirmwareMismatchOverride(true), "reconnected mismatch can be approved again");
        check(port.hssStart(blocks, 1000, port.targetEpoch()), "HSS starts before device identity changes");
        runtimeIdentity[16] = flashIdentity[16] = 3;
        const int writesBeforeIdentityChange = sampleWrites;
        check(!port.writeMem(sampleAddress, sizeof(desired), &desired) && !port.firmwareStatus().forced &&
              !port.isHssRunning() && sampleWrites == writesBeforeIdentityChange,
              "a different device build cancels override before a write and stops HSS");

        check(port.setFirmwareMismatchOverride(true), "new device build requires explicit override");
        const auto beforeMatchingBuild = port.targetEpoch();
        runtimeIdentity[16] = flashIdentity[16] = source.info[16];
        check(!port.writeMemAtEpoch(beforeMatchingBuild, sampleAddress, sizeof(desired), &desired) &&
              sampleWrites == writesBeforeIdentityChange && !port.firmwareStatus().forced &&
              port.firmwareStatus().state == firmware_match::State::Matched,
              "a build becoming matched still rejects a write queued before override expiration");
        check(port.writeMemAtEpoch(port.targetEpoch(), sampleAddress, sizeof(desired), &desired),
              "the newly matched build permits requests with the new epoch");
        runtimeIdentity[16] = flashIdentity[16] = 3;
        check(!port.writeMem(sampleAddress, sizeof(desired), &desired) &&
              port.setFirmwareMismatchOverride(true), "a later mismatch again requires explicit override");
        check(port.hssStart(blocks, 1000, port.targetEpoch()), "HSS starts before a matching build appears");
        runtimeIdentity[16] = flashIdentity[16] = source.info[16];
        RttReconnectTestAccess::refreshIdentity(port);
        const int readsBeforeMatchingBuild = hssReads;
        check(port.hssRead(frame.data(), uint32_t(frame.size())) == 0 && !port.isHssRunning() &&
              hssReads == readsBeforeMatchingBuild && !port.firmwareStatus().forced,
              "HSS does not read a frame after identity verification stops its old session");
        runtimeIdentity[16] = flashIdentity[16] = 3;
        check(!port.writeMem(sampleAddress, sizeof(desired), &desired) && port.setFirmwareMismatchOverride(true),
              "override can be enabled for the next verified mismatch");
        identityReadable = false;
        check(!port.writeMem(sampleAddress, sizeof(desired), &desired) &&
              port.firmwareStatus().state == firmware_match::State::Unavailable &&
              !port.setFirmwareMismatchOverride(true) && !port.firmwareStatus().forced,
              "unavailable identity cannot be bypassed or reuse the prior override");
        identityReadable = true;
        port.setFirmwareSource(owner, source, true);
        check(!port.setFirmwareMismatchOverride(true) && !port.readMem(sampleAddress, sizeof(result), &result) &&
              port.firmwareStatus().state == firmware_match::State::Loading,
              "loading symbols cannot be bypassed");
        port.removeFirmwareSource(owner);
}

static void checkPeerRouting(JLinkPort &port) {
        const auto key = ava_peer::randomKey();
        port.startDeviceSharing("SDK fixture", 0, key, true);
        const auto listenPort = ava_peer::server().port();
        ava_peer::Client observer;
        observer.connect("127.0.0.1", uint16_t(listenPort), key);
        uint32_t result{}, desired = 0x31415926;
        check(observer.read(sampleAddress, 4, &result) && result == sampleValue, "export reads use the local SDK fixture");
        observer.claim(true);
        check(!port.writeMem(sampleAddress, 4, &desired), "remote lease blocks local SDK writes");
        check(observer.write(sampleAddress, 4, &desired), "lease owner writes through the local firmware gate");
        observer.claim(false);
        const auto oldLocal = port.targetEpoch();
        port.connectRemoteDevice("127.0.0.1", listenPort, key);
        check(port.usingRemote() && port.isConnected(), "desktop can control its exported fixture without recursive routing");
        check(!port.readMemAtEpoch(oldLocal, sampleAddress, 4, &result), "old local requests cannot reach the new remote source");
        const auto remoteEpoch = port.targetEpoch();
        check(port.readMemAtEpoch(remoteEpoch, sampleAddress, 4, &result) && result == desired, "routed remote read reaches the physical backend once");
        check(!port.writeMemAtEpoch(remoteEpoch, sampleAddress, 4, &desired), "remote port cannot write before taking control");
        ava_peer::client().claim(true);
        check(port.writeMemAtEpoch(remoteEpoch, sampleAddress, 4, &desired), "remote port writes with control");
        check(port.hssStart({{sampleAddress, 4}}, 1000, remoteEpoch), "existing sampler starts source-side peer polling");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::array<uint8_t, 256> frames{};
        check(port.hssFrameSize() == 8 && port.hssRead(frames.data(), uint32_t(frames.size())) >= 16, "peer frames use the existing timestamped sampler layout");
        port.hssStop();
        port.selectLocalDevice();
        check(!port.usingRemote() && port.isConnected(), "local device survives the peer connection");
        check(!port.writeMemAtEpoch(remoteEpoch, sampleAddress, 4, &desired), "old remote requests cannot fall back to local writes");
        check(observer.read(sampleAddress, 4, &result), "switching the controller back to local preserves other observers");
        observer.close();ava_peer::server().stop();
        puts("PASS: desktop peer export/control, local lease exclusion, generation routing, timestamped sampler and independent local lifetime");
}

int main(int, char **) {
        module_init();
        auto &port = JLinkPort::instance();
        port.setDeviceName("STM32H745II");
        try {
                // A real module handle allows resolvedPath(), while matching
                // loadedPath avoids LoadLibrary entirely in open()
                g_jlinkApi.dll = GetModuleHandleW(nullptr);
                check(_putenv_s("AVA_JLINK_LOG_DIR", "jlink-diagnostics") == 0, "enable test diagnostics");
                check(_putenv_s("AVA_JLINK_TIMING_ADDRESS", "0x20004E6C") == 0, "configure timing RAM snapshot");
                check(_putenv_s("AVA_JLINK_HEARTBEAT_ADDRESS", "0x20004C20") == 0, "configure live heartbeat");
                g_jlinkApi.SetLogFile = +[](const char *path) { sdkLogPaths.emplace_back(path); };
                g_jlinkApi.ReadMemEx = fixtureRead;
                g_jlinkApi.Open = +[]() -> const char * {
                        ++sdkOpenCalls;
                        check(sdkLogPaths.size() == size_t(sdkOpenCalls), "SDK log is configured before every Open");
                        attachCommands.clear(); debugPowerDownOnClose.store(true); skipDebugDeInit.store(false);
                        restartOnClose = true; sdkOpen.store(true); return nullptr;
                };
                g_jlinkApi.Close = +[]() {
                        // Debug de-initialization and debug power-down are separate close policies.
                        // H7 device callback clears DBGMCU even when generic de-init is skipped.
                        const bool deviceCleanup = sdkDevice.starts_with("STM32H7") && !disconnectScriptSelected;
                        if (sdkOpen.exchange(false)) {
                                // Model the observed SDK behavior: restart disabled leaves CPU halted.
                                halted.store(!restartOnClose);
                                if (deviceCleanup || !skipDebugDeInit.load() || debugPowerDownOnClose.load())
                                        dwtClockEnabled.store(false);
                        }
                };
                g_jlinkApi.IsOpen = +[]() -> char { return sdkOpen.load(); };
                g_jlinkApi.Connect = +[]() -> int {
                        ++connectCalls;
                        check(attachCommands == std::vector<std::string>{"SetRestartOnClose=1", "SetSkipDebugDeInit=1", "SetDbgPowerDownOnClose=0", "ForceAttachTarget=1", "InhibitConnectRetries=1", "SetAllowStopMode=0"},
                              "every SDK session configures live attach before connecting");
                        check(disconnectScriptSelected == sdkDevice.starts_with("STM32H7"),
                              "H7 disconnect override is applied before connect and cleared for other targets");
                        return connectFailure.load() ? -1 : 0;
                };
                g_jlinkApi.ExecCommand = +[](const char *command, char *, int32_t) -> int {
                        const std::string value(command);
                        if (value.starts_with("Device = ")) {
                                attachCommands.clear(); sdkDevice = value.substr(9);
                        }
                        if (value.starts_with("ScriptFile = ")) {
                                if (rejectedAttachCommand == "ScriptFile") return -1;
                                const std::string argument = value.substr(13);
                                check(argument.size() >= 2 && argument.front() == '"' && argument.back() == '"',
                                      "script path is quoted for spaces");
                                const std::string path = argument.substr(1, argument.size() - 2);
                                check(path.empty() || std::filesystem::is_regular_file(path), "packaged script exists outside working directory");
                                disconnectScriptSelected = !path.empty();
                        }
                        if (value == "SetRestartOnClose=1" || value == "SetSkipDebugDeInit=1" || value == "SetDbgPowerDownOnClose=0" || value == "ForceAttachTarget=1" || value == "InhibitConnectRetries=1" || value == "SetAllowStopMode=0") {
                                attachCommands.push_back(value);
                                if (value == rejectedAttachCommand) return -1;
                                if (value == "SetRestartOnClose=1") { const bool previous = restartOnClose; restartOnClose = true; return previous ? 1 : 0; }
                                if (value == "SetSkipDebugDeInit=1") skipDebugDeInit.store(true);
                                // Real V8.10k returns the old setting, so +1 is successful.
                                if (value == "SetDbgPowerDownOnClose=0") return debugPowerDownOnClose.exchange(false) ? 1 : 0;
                        }
                        return 0;
                };
                g_jlinkApi.TIF_Select = +[](int32_t) -> int { return 0; };
                g_jlinkApi.SetSpeed = +[](uint32_t) {};
                g_jlinkApi.IsHalted = +[]() -> char { return halted.load(); };
                g_jlinkApi.SetResetType = +[](int32_t) -> int { return 0; };
                g_jlinkApi.Reset = +[]() -> int { resets.fetch_add(1); return resetFailure.load() ? -1 : 0; };
                g_jlinkApi.Go = +[]() -> int { runs.fetch_add(1); if (runFailure.load()) return -1; if (!stuckHalt.load()) halted.store(false); return 0; };
                g_jlinkApi.RTTERMINAL_Control = +[](uint32_t command, void *) -> int {
                        if (command == 0) {
                                starts.fetch_add(1);
                                return startFailure.load() ? -1 : 0;
                        }
                        return command == 3 ? 1 : 0;
                };
                g_jlinkApi.RTTERMINAL_Read = +[](uint32_t, char *buffer, uint32_t capacity) -> int {
                        if (!sdkOpen.load()) readsWhileClosed.fetch_add(1);
                        if (capacity >= 3 && receivedPacket.exchange(0)) {
                                std::memcpy(buffer, "ok\n", 3);
                                return 3;
                        }
                        return 0;
                };
                g_jlinkApi.RTTERMINAL_Write = +[](uint32_t, const void *, uint32_t) -> int { return 0; };
                auto reconnect = [&] {
                        port.close();
                        check(dwtClockEnabled.load(), "disconnect preserves the firmware DWT timebase");
                        check(port.open() && port.connect(), "fixture reconnect succeeds");
                        check(dwtClockEnabled.load(), "reconnect preserves the firmware DWT timebase");
                };
                for (const char *command : {"SetRestartOnClose=1", "SetSkipDebugDeInit=1", "SetDbgPowerDownOnClose=0", "ForceAttachTarget=1", "InhibitConnectRetries=1", "SetAllowStopMode=0", "ScriptFile"}) {
                        rejectedAttachCommand = command;
                        const int beforeConnect = connectCalls;
                        check(port.open() && !port.connect() && !port.isOpen() && !port.isConnected(),
                              "unsupported live-attach option closes the session");
                        check(connectCalls == beforeConnect && resets == 0 && runs == 0 && starts == 0,
                              "rejected option never falls back to connect, reset, run or RTT");
                }
                rejectedAttachCommand.clear();
                dwtClockEnabled.store(true); // Simulate firmware startup after rejected SDK options.
                check(port.open() && port.connect() && resets == 0, "ordinary running-target connection does not reset");
                port.close();
                check(dwtClockEnabled.load(), "closing a live attach does not disable DWT");
                port.setDeviceName("STM32F407VG");
                check(port.open() && port.connect() && !disconnectScriptSelected, "switching target clears H7 script selection");
                port.close();
                port.setDeviceName("STM32H745II");
                halted.store(true);
                check(port.open() && port.connect() && !halted && resets == 0 && runs == 1,
                      "monitoring resumes a halted target without resetting it");
                const auto beforeHeartbeat = heartbeatValue;
                captureJlinkTimingState("running-heartbeat-1");
                captureJlinkTimingState("running-heartbeat-2");
                check(heartbeatValue > beforeHeartbeat + 1, "running target heartbeat advances");
                port.close();
                check(!halted.load(), "disconnect leaves the application running");
                check(port.open() && port.connect(true) && resets == 1 && runs == 2,
                      "connect and reset restarts a running target exactly once");
                port.close(); halted.store(true);
                check(port.open() && port.connect(true) && resets == 2 && runs == 3,
                      "halted target is not reset twice");
                port.close(); connectFailure.store(1);
                check(port.open() && !port.connect(true) && resets == 2, "failed connect never resets target");
                check(dwtClockEnabled.load(), "failed connect cleanup does not disable DWT");
                connectFailure.store(0); resetFailure.store(1);
                check(port.open() && !port.connect(true) && !port.isConnected() && runs == 3,
                      "reset failure closes connection without running or starting acquisition");
                resetFailure.store(0); runFailure.store(1);
                check(port.open() && !port.connect(true) && !port.isConnected(), "run failure is not reported as connected");
                runFailure.store(0);
                halted.store(true); runFailure.store(1);
                const int beforeResumeReset = resets.load();
                check(port.open() && !port.connect() && !port.isConnected() && resets == beforeResumeReset,
                      "failed resume never reports a live monitoring connection or silently resets");
                runFailure.store(0); halted.store(true); stuckHalt.store(true);
                check(port.open() && !port.connect() && !port.isConnected(),
                      "Go success with CPU still halted is rejected");
                stuckHalt.store(false); halted.store(false);
                const int beforeAsync = resets.load();
                port.connectAsync(true);
                waitFor([&] { return !port.isBusy(); }, "asynchronous connect and reset completes");
                check(port.isConnected() && resets == beforeAsync + 1 && starts == 0,
                      "asynchronous request preserves reset choice and leaves unsolicited RTT stopped");
                port.close();
                check(!RttReconnectTestAccess::start(port) && !RttReconnectTestAccess::wanted(port),
                      "starting while disconnected does not arm RTT");
                check(port.open() && port.connect() && starts.load() == 0, "ordinary connect leaves RTT stopped");
                check(RttReconnectTestAccess::start(port), "explicit RTT start succeeds");
                receivedPacket.store(1);
                waitFor([&] { return RttReconnectTestAccess::rx(port) == "ok\n"; }, "reader receives fixture data");
                check(RttReconnectTestAccess::send(port) && RttReconnectTestAccess::pending(port) == 5,
                      "backpressure leaves an unsent command queued");
                reconnect();
                check(starts.load() == 2 && RttReconnectTestAccess::active(port) &&
                      RttReconnectTestAccess::pending(port) == 0 && RttReconnectTestAccess::rx(port) == "ok\n",
                      "reconnect resumes RTT, retains the log and clears stale transmit data");
                receivedPacket.store(1);
                waitFor([&] { return RttReconnectTestAccess::rx(port) == "ok\nok\n"; }, "resumed reader receives new data");
                check(port.connect() && starts.load() == 2, "already active RTT is not started twice");
                RttReconnectTestAccess::stop(port);
                reconnect();
                check(starts.load() == 2 && !RttReconnectTestAccess::active(port), "explicit stop disables auto-resume");
                startFailure.store(1);
                check(!RttReconnectTestAccess::start(port) && !RttReconnectTestAccess::wanted(port),
                      "failed initial RTT start does not enable auto-resume");
                reconnect();
                check(starts.load() == 3, "connect does not retry RTT that was never enabled");
                startFailure.store(0);
                check(RttReconnectTestAccess::start(port), "RTT can be enabled again");
                port.close();
                check(RttReconnectTestAccess::wanted(port), "disconnect preserves enabled intent");
                RttReconnectTestAccess::stop(port);
                check(port.open() && port.connect() && starts.load() == 4,
                      "stop while disconnected cancels a pending resume");
                check(RttReconnectTestAccess::start(port), "RTT starts before transport loss");
                RttReconnectTestAccess::loseConnection(port);
                waitFor([&] { return !RttReconnectTestAccess::active(port); }, "reader exits on transport loss");
                reconnect();
                check(starts.load() == 6 && RttReconnectTestAccess::active(port), "transport loss also preserves resume intent");
                port.close(); connectFailure.store(1);
                check(port.open() && !port.connect() && starts.load() == 6 && RttReconnectTestAccess::wanted(port),
                      "failed J-Link connection does not start RTT or lose enabled intent");
                connectFailure.store(0);
                check(port.open() && port.connect() && starts.load() == 7, "subsequent successful connect resumes RTT");
                port.close(); startFailure.store(1);
                check(port.open() && port.connect() && !RttReconnectTestAccess::active(port) &&
                      RttReconnectTestAccess::wanted(port), "RTT resume failure does not fail the J-Link connection");
                startFailure.store(0); reconnect();
                check(starts.load() == 9 && RttReconnectTestAccess::active(port), "later reconnect can retry a failed resume");
                check(readsWhileClosed.load() == 0, "RTT reader never accesses a closed SDK session");
                RttReconnectTestAccess::stop(port);
                checkFirmwareOverride(port);
                checkPeerRouting(port);

                // Real ImGui hover against the connection control: identity
                // status belongs to its tooltip, rather than the toolbar
                ImGui::CreateContext();
                auto &io = ImGui::GetIO();
                io.IniFilename = nullptr; io.DisplaySize = ImVec2(1280, 240); io.DeltaTime = 1.0f / 60;
                io.ConfigInputTrickleEventQueue = false;
                unsigned char *pixels; int width, height;
                io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
                g_lang = Lang::EN;
                RttReconnectTestAccess::status(port, firmware_match::State::Matched);
                auto frame = [&](ImVec2 mouse = ImVec2(-100, -100)) {
                        io.AddMousePosEvent(mouse.x, mouse.y);
                        ImGui::NewFrame();
                        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(1200, 200));
                        ImGui::Begin("J-Link toolbar regression"); port.drawUI(); ImGui::End(); ImGui::Render();
                };
                frame(); frame();
                auto *host = ImGui::FindWindowByName("J-Link toolbar regression");
                ImGui::SetFocusID(host->GetID("Disconnect"), host);
                host->NavRectRel[ImGuiNavLayer_Main] = ImRect(-100, -100, -99, -99);
                frame();
                const auto button = ImGui::WindowRectRelToAbs(host, host->NavRectRel[ImGuiNavLayer_Main]);
                check(button.Min.x >= 0, "connection control is located");
                frame(button.GetCenter()); frame(button.GetCenter());
                const auto green = ImGui::GetColorU32(ImVec4(.35f, .85f, .45f, 1));
                auto hasColor = [](ImGuiWindow *window, ImU32 color) {
                        return std::any_of(window->DrawList->VtxBuffer.begin(), window->DrawList->VtxBuffer.end(),
                                           [&](const ImDrawVert &vertex) { return vertex.col == color; });
                };
                bool matchedTooltip = false;
                for (auto *window : ImGui::GetCurrentContext()->Windows)
                        matchedTooltip |= window->Active && (window->Flags & ImGuiWindowFlags_Tooltip) && hasColor(window, green);
                check(matchedTooltip && !hasColor(host, green), "matched identity status appears only in the connection tooltip");
                RttReconnectTestAccess::status(port, firmware_match::State::Mismatch);
                frame(button.GetCenter()); frame(button.GetCenter());
                const auto red = ImGui::GetColorU32(ImVec4(1, .35f, .30f, 1));
                bool mismatchTooltip = false;
                for (auto *window : ImGui::GetCurrentContext()->Windows)
                        mismatchTooltip |= window->Active && (window->Flags & ImGuiWindowFlags_Tooltip) && hasColor(window, red);
                check(mismatchTooltip && !hasColor(host, red), "mismatch status also moves to the connection tooltip");
                ImGui::DestroyContext();
                RttReconnectTestAccess::stop(port); port.close();
                check(diagnosticReads > 0, "diagnostic connection snapshots read clock registers");
                std::ifstream snapshotFile("jlink-diagnostics/clock-state.log");
                const std::string snapshots{std::istreambuf_iterator<char>(snapshotFile), std::istreambuf_iterator<char>()};
                check(snapshots.find("view=SDK_may_be_cached_or_virtualized") != std::string::npos &&
                      snapshots.find(" delta=") == std::string::npos,
                      "SDK snapshots disclose caching and do not infer counter progress");
                check(std::unordered_set<std::string>(sdkLogPaths.begin(), sdkLogPaths.end()).size() == sdkLogPaths.size(),
                      "reconnect uses unique SDK log files and preserves previous disconnect evidence");
                std::ifstream timingFile("jlink-diagnostics/timing-state.log");
                const std::string timingLog{std::istreambuf_iterator<char>(timingFile), std::istreambuf_iterator<char>()};
                check(timingReads > 0 && timingLog.find("after-sdk-connect") != std::string::npos &&
                      timingLog.find("before-sdk-close") != std::string::npos &&
                      timingLog.find("before-hss-stop") != std::string::npos &&
                      timingLog.find("after-hss-stop") != std::string::npos &&
                      timingLog.find("cnt=1 code=1 mask=2097152 tick=162774") != std::string::npos,
                      "timing snapshots surround disconnect operations and decode application RAM");
                const int previousTimingReads = timingReads;
                check(_putenv_s("AVA_JLINK_TIMING_ADDRESS", "0x40000000") == 0, "configure invalid timing address");
                captureJlinkTimingState("invalid-address");
                check(timingReads == previousTimingReads, "timing diagnostics reject peripheral addresses");
                check(_putenv_s("AVA_JLINK_TIMING_ADDRESS", "0x20004E6C") == 0, "restore timing address");
                check(_putenv_s("AVA_JLINK_LOG_DIR", "") == 0, "disable test diagnostics");
                captureJlinkTimingState("disabled");
                check(timingReads == previousTimingReads, "disabled diagnostics add no timing reads");
                check(_putenv_s("AVA_JLINK_TIMING_ADDRESS", "") == 0, "clear timing address");
                check(_putenv_s("AVA_JLINK_HEARTBEAT_ADDRESS", "") == 0, "clear heartbeat address");
                const int previousReads = diagnosticReads;
                captureJlinkClockState("disabled");
                configureJlinkDiagnosticLog();
                check(diagnosticReads == previousReads && sdkLogPaths.size() == size_t(sdkOpenCalls),
                      "disabled diagnostics perform no SDK calls");
                puts("PASS: opt-in SDK session logs and read-only DWT snapshots");
                puts("PASS: RTT reconnect, forced firmware mismatch memory/HSS access, override lifecycle, stale epochs and identity tooltip");
                return 0;
        } catch (const std::exception &error) {
                RttReconnectTestAccess::stop(port); port.close();
                fprintf(stderr, "FAIL: %s\n", error.what());
                return 1;
        }
}
