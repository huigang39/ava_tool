/**
 * @file  jlink_port.cpp
 * @brief JLinkPort implementation — J-Link SDK wrapper.
 */
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "imgui.h"

#include "app_log.hpp"
#include "core/jlink_port.hpp"
#include "gui/i18n.hpp"
#include "gui/tutorial_guide.hpp"
#include "gui/ui_theme.hpp"

JLinkPort &
JLinkPort::instance()
{
        static JLinkPort s;
        return s;
}

bool
JLinkPort::open()
{
        std::lock_guard lk(mtx_);
        if (isOpen_)
                return true;

        const char *err = JLINKARM_Open();
        if (err && err[0]) {
                lastErr_ = err;
                isOpen_  = false;
                LOG_E("JLinkPort::open() FAILED: %s", err);
                return false;
        }
        isOpen_ = JLINKARM_IsOpen() != 0;
        if (!isOpen_) {
                lastErr_ = "JLINKARM_Open returned but IsOpen is false";
                LOG_E("JLinkPort::open() FAILED: IsOpen is false");
        } else {
                LOG_I("JLinkPort::open() SUCCEEDED");
        }
        return isOpen_;
}

void
JLinkPort::close()
{
        std::lock_guard lk(mtx_);

        // Flip state flags BEFORE any J-Link API calls so a concurrent isConnected()
        // / isOpen() check from the sampler sees the new state immediately.
        const bool wasOpen      = isOpen_;
        const bool wasConnected = isConnected_;
        isConnected_            = false;
        isOpen_                 = false;

        if (hssRunning_) {
                LOG_I("Stopping HSS...");
                JLINK_HSS_Stop();
                hssRunning_   = false;
                hssFrameSize_ = 0;
                hssActualHz_.store(0.0f, std::memory_order_relaxed);
        }
        if (wasOpen) {
                LOG_I("Closing JLink connection...");
                // Best-effort C_DEBUGEN detach. Skipped when the link looks dead —
                // touching MCU memory on a stalled USB connection can hang inside
                // the SDK and leave it in a state where re-Open() permanently
                // returns a stale handle (requires app restart to recover).
                if (wasConnected && JLINKARM_IsOpen()) {
                        if (JLINKARM_IsHalted()) {
                                LOG_I("JLinkPort::close(): Target was halted, resuming before close...");
                                JLINKARM_Go();
                        }
                        u32 dhcsr = 0xA05F0000;
                        JLINKARM_WriteMemEx(0xE000EDF0, 4, &dhcsr, 0);
                        LOG_I("JLinkPort::close(): Cleared C_DEBUGEN in DHCSR.");
                }
                JLINKARM_Close();
                // Give the SDK time to release the USB handle. Without this, an
                // immediately-following Open() sometimes returns a stale handle
                // that can never Connect() until the process is restarted.
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                LOG_I("JLink closed.");
        }
}

bool
JLinkPort::connect()
{
        std::lock_guard lk(mtx_);
        lastErr_.clear(); // Clear previous error on new attempt

        LOG_I("JLinkPort::connect() attempt");
        if (!isOpen_) {
                lastErr_ = "not open";
                LOG_E("JLinkPort::connect() FAILED: J-Link not open");
                return false;
        }

        // On any failure below: tear down the SDK state so the next CONNECT click
        // does a clean Open()+Connect() cycle. Without this, a partial connect
        // leaves the SDK holding a stale USB handle that never recovers — even
        // after the user unplugs and replugs the J-Link — until the app restarts.
        auto resetSdk = [&]() {
                JLINKARM_Close();
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
                lastErr_ = std::string("ExecCommand(Device): ") + ack;
                LOG_E("JLinkPort::connect() FAILED: ExecCommand Device failed: %s", ack);
                resetSdk();
                return false;
        }
        LOG_I("JLinkPort::connect(): Device set to %s", dev.c_str());

        if (JLINKARM_TIF_Select(JLINKARM_TIF_SWD) < 0) {
                lastErr_ = "TIF_Select(SWD) failed";
                LOG_E("JLinkPort::connect() FAILED: TIF_Select(SWD) failed");
                resetSdk();
                return false;
        }
        JLINKARM_SetSpeed(static_cast<u32>(spdKHz));
        LOG_I("JLinkPort::connect(): TIF SWD selected, speed %d KHz", spdKHz);

        if (JLINKARM_Connect() < 0) {
                lastErr_ = "Connect failed (Check power/cable or replug J-Link)";
                LOG_E("JLinkPort::connect() FAILED: JLINKARM_Connect() < 0; resetting SDK");
                resetSdk();
                return false;
        }

        // Ensure the MCU keeps running and doesn't get stuck in halted state upon connection.
        // After a power cycle, Go() alone may not recover — do a full reset.
        if (JLINKARM_IsHalted()) {
                LOG_I("JLinkPort::connect(): Target is halted — performing reset + go to recover...");
                JLINKARM_SetResetType(0);
                JLINKARM_Reset();
                JLINKARM_Go();
                // Give the MCU time to boot after reset
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (JLINKARM_IsHalted()) {
                        LOG_W("JLinkPort::connect(): Target still halted after reset — proceeding anyway");
                }
        }

        readFailCount_.store(0, std::memory_order_relaxed);
        isConnected_ = true;
        LOG_I("JLinkPort::connect() SUCCEEDED");
        return true;
}

bool
JLinkPort::resetTarget()
{
        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_)
                return false;
        LOG_I("JLinkPort::resetTarget() requested");

        // Use normal reset (not halting)
        JLINKARM_SetResetType(0);
        JLINKARM_Reset();
        JLINKARM_Go();

        return true;
}

void
JLinkPort::connectAsync()
{
        bool expected = false;
        if (!busy_.compare_exchange_strong(expected, true))
                return; // already busy
        std::thread([this]() {
                // Always do a full close+open+connect cycle.
                // This handles the case where the MCU lost power and the
                // old SDK state is stale ("cpu is halt" on reconnect).
                if (isConnected_ || isOpen_)
                        close();
                if (!isOpen_)
                        open();
                if (isOpen_)
                        connect();
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

bool
JLinkPort::readMem(const u32 addr, const u32 numBytes, void *dst)
{
        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_)
                return false;
        bool ok = JLINKARM_ReadMemEx(addr, numBytes, dst, 0) >= 0;
        if (!ok) {
                int fc = readFailCount_.fetch_add(1, std::memory_order_relaxed) + 1;
                if (fc >= kMaxReadFails) {
                        LOG_E("JLinkPort::readMem() %d consecutive failures — auto-disconnecting", fc);
                        isConnected_ = false;
                        isOpen_      = false;
                }
        } else {
                readFailCount_.store(0, std::memory_order_relaxed);
        }
        return ok;
}

bool
JLinkPort::writeMem(const u32 addr, const u32 numBytes, const void *src)
{
        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_)
                return false;
        return JLINKARM_WriteMemEx(addr, numBytes, src, 0) >= 0;
}

u32
JLinkPort::readReg(u32 regIndex)
{
        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_)
                return 0;
        return JLINKARM_ReadReg(regIndex);
}

bool
JLinkPort::isHalted()
{
        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_)
                return false;
        return JLINKARM_IsHalted() != 0;
}

bool
JLinkPort::halt()
{
        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_)
                return false;
        return JLINKARM_Halt() >= 0;
}

bool
JLinkPort::resume()
{
        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_)
                return false;
        return JLINKARM_Go() >= 0;
}

bool
JLinkPort::hssStart(const std::vector<HssBlock> &blocks, const i32 periodUs)
{
        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_) {
                lastErr_ = "HSS: not connected";
                LOG_E("JLinkPort::hssStart() FAILED: not connected");
                return false;
        }
        if (blocks.empty()) {
                lastErr_ = "HSS: no blocks";
                return false;
        }
        if (hssRunning_) {
                JLINK_HSS_Stop();
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
                char buf[160];
                snprintf(buf,
                         sizeof(buf),
                         "HSS Error: HW rejected period %dus (~%dHz). Try reducing the sample rate (MaxHz).",
                         effectivePeriodUs,
                         1000000 / effectivePeriodUs);
                lastErr_ = buf;
                LOG_E("JLinkPort::hssStart() FAILED: %s", buf);
                return false;
        }
        LOG_I("JLinkPort::hssStart() SUCCEEDED");

        hssRunning_   = true;
        hssFrameSize_ = static_cast<i32>(frameSize);
        hssPeriodUs_  = effectivePeriodUs;
        lastErr_.clear();
        return true;
}

void
JLinkPort::hssStop()
{
        std::lock_guard lk(mtx_);
        if (hssRunning_) {
                LOG_I("JLinkPort::hssStop()");
                JLINK_HSS_Stop();
                hssRunning_   = false;
                hssFrameSize_ = 0;
                hssActualHz_.store(0.0f, std::memory_order_relaxed);
        }
}

i32
JLinkPort::hssRead(void *buf, const u32 bufSize)
{
        std::lock_guard lk(mtx_);
        if (!hssRunning_)
                return 0;
        return JLINK_HSS_Read(buf, bufSize);
}

std::string
JLinkPort::lastError() const
{
        std::lock_guard lk(mtx_);
        return lastErr_;
}

void
JLinkPort::drawUI()
{
        // Read config via the lightweight cfgMtx_ getters — never the J-Link I/O
        // FairMutex — so the render thread doesn't stall behind slow J-Link reads.
        char        nameBuf[64];
        std::string dev = deviceName();
        snprintf(nameBuf, sizeof(nameBuf), "%s", dev.c_str());

        char btnLabel[128];
        snprintf(
            btnLabel, sizeof(btnLabel), "%s##jlink_port_btn", nameBuf[0] != '\0' ? nameBuf : tr("Select Device", "选择设备"));
        if (ImGui::Button(btnLabel, ImVec2(180.0f, 0))) {
                JLINKARM_DEVICE_SELECT_INFO sinfo;
                sinfo.SizeOfStruct = sizeof(sinfo);
                sinfo.CoreIndex    = 0;
                int32_t devIdx     = JLINKARM_DEVICE_SelectDialog(nullptr, 0, &sinfo);
                if (devIdx >= 0) {
                        JLINKARM_DEVICE_INFO dinfo;
                        dinfo.SizeOfStruct = sizeof(dinfo);
                        if (JLINKARM_DEVICE_GetInfo(devIdx, &dinfo) >= 0)
                                setDeviceName(dinfo.sName);
                }
        }
        TutorialGuide::instance().mark("device_btn");
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Device", "设备"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        // Logarithmic slider: SWD speeds span a wide range (kHz .. tens of MHz).
        int spd = speedKHz_.load(std::memory_order_relaxed);
        ImGui::SliderInt("##jlinkSpeed", &spd, 5, 50000, "%d kHz", ImGuiSliderFlags_Logarithmic);
        TutorialGuide::instance().mark("speed_slider");
        const bool speedCommitted = ImGui::IsItemDeactivatedAfterEdit();
        if (spd < 1)
                spd = 1;
        speedKHz_.store(spd, std::memory_order_relaxed);
        if (speedCommitted && isOpen_) {
                // One-off J-Link call on slider release (rare) — OK under the I/O lock.
                std::lock_guard lk(mtx_);
                JLINKARM_SetSpeed(static_cast<u32>(spd));
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                                  tr("J-Link SWD speed (kHz). Drag to set; applies when you release. Ctrl+click to type.",
                                     "J-Link SWD 速度 (kHz)。拖动设置，松开后生效。Ctrl+点击可输入。"));

        ImGui::SameLine();
        const bool busy = busy_.load(std::memory_order_acquire);
        if (busy) {
                // An async connect/disconnect/reset is running — show a spinner-ish
                // label instead of a button so the GUI never blocks on USB I/O.
                ui::Button(isConnected_ ? tr("Working...", "处理中...") : tr("Connecting...", "连接中..."),
                           ui::BtnStyle::Muted);
        } else if (!isConnected_) {
                // Connect = constructive action → green.
                if (ui::SmallButton(tr("CONNECT", "连接"), ui::BtnStyle::Success))
                        connectAsync();
                TutorialGuide::instance().mark("connect_btn");
        } else {
                // Disconnect = teardown → red; Reset = caution → amber.
                if (ui::SmallButton(tr("DISCONNECT", "断开"), ui::BtnStyle::Danger))
                        disconnectAsync();
                TutorialGuide::instance().mark("connect_btn");

                ImGui::SameLine();
                if (ui::SmallButton(tr("RESET MCU", "复位 MCU"), ui::BtnStyle::Warning))
                        resetAsync();
        }

        if (!lastErr_.empty()) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::Text(tr("Error: %s", "错误: %s"), lastErr_.c_str());
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "%s",
                            tr("Try reducing the sampling frequency (Hz) if you see 'Low on memory' or 'Start failed'.",
                               "若出现 'Low on memory' 或 'Start failed'，请尝试降低采样频率 (Hz)。"));
                ImGui::PopStyleColor();
        }
}
