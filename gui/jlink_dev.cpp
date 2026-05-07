#include <cstdio>
#include <cstring>

#include "imgui.h"

#include "jlink_dev.hpp"

JLinkDev &
JLinkDev::instance()
{
        static JLinkDev s;
        return s;
}

bool
JLinkDev::open()
{
        std::lock_guard lk(mtx_);
        if (isOpen_)
                return true;

        const char *err = JLINKARM_Open();
        if (err && err[0]) {
                lastErr_ = err;
                isOpen_  = false;
                return false;
        }
        isOpen_ = JLINKARM_IsOpen() != 0;
        if (!isOpen_)
                lastErr_ = "JLINKARM_Open returned but IsOpen is false";
        return isOpen_;
}

void
JLinkDev::close()
{
        std::lock_guard lk(mtx_);
        if (hssRunning_) {
                JLINK_HSS_Stop();
                hssRunning_ = false;
        }
        if (isOpen_) {
                JLINKARM_Close();
                isOpen_ = false;
        }
        isConnected_ = false;
}

bool
JLinkDev::connect()
{
        std::lock_guard lk(mtx_);
        lastErr_.clear(); // Clear previous error on new attempt

        if (!isOpen_) {
                lastErr_ = "not open";
                return false;
        }

        char cmd[160];
        snprintf(cmd, sizeof(cmd), "Device = %s", deviceName_.c_str());
        char ack[256] = {0};
        if (JLINKARM_ExecCommand(cmd, ack, sizeof(ack)) < 0) {
                lastErr_ = std::string("ExecCommand(Device): ") + ack;
                return false;
        }

        if (JLINKARM_TIF_Select(JLINKARM_TIF_SWD) < 0) {
                lastErr_ = "TIF_Select(SWD) failed";
                return false;
        }
        JLINKARM_SetSpeed(static_cast<u32>(speedKHz_));

        if (JLINKARM_Connect() < 0) {
                lastErr_     = "Connect failed (Check power/cable or replug J-Link)";
                isConnected_ = false;
                return false;
        }
        isConnected_ = true;
        return true;
}

bool
JLinkDev::readMem(const u32 addr, const u32 numBytes, void *dst)
{
        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_)
                return false;
        return JLINKARM_ReadMemEx(addr, numBytes, dst, 0) >= 0;
}

bool
JLinkDev::writeMem(const u32 addr, const u32 numBytes, const void *src)
{
        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_)
                return false;
        return JLINKARM_WriteMemEx(addr, numBytes, src, 0) >= 0;
}

bool
JLinkDev::hssStart(const std::vector<HssBlock> &blocks, const int periodUs)
{
        std::lock_guard lk(mtx_);
        if (!isOpen_ || !isConnected_) {
                lastErr_ = "HSS: not connected";
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

        int effectivePeriodUs = periodUs;
        if (effectivePeriodUs > 100000) {
                // HSS hardware typically doesn't support frequencies below 10Hz stably.
                // Clamping to 100ms period. For lower frequencies, use POLL mode.
                effectivePeriodUs = 100000;
        }

        int res = JLINK_HSS_Start(descs.data(), static_cast<i32>(descs.size()), effectivePeriodUs, 1);
        if (res < 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "HSS Error: HW rejected period %dus. Min 10Hz recommended.", effectivePeriodUs);
                lastErr_ = buf;
                print_info(false, "%s", buf);
                return false;
        }

        hssRunning_   = true;
        hssFrameSize_ = static_cast<int>(frameSize);
        hssPeriodUs_  = effectivePeriodUs;
        lastErr_.clear();
        return true;
}

void
JLinkDev::hssStop()
{
        std::lock_guard lk(mtx_);
        if (hssRunning_) {
                JLINK_HSS_Stop();
                hssRunning_   = false;
                hssFrameSize_ = 0;
                hssActualHz_.store(0.0f, std::memory_order_relaxed);
        }
}

int
JLinkDev::hssRead(void *buf, const u32 bufSize)
{
        std::lock_guard lk(mtx_);
        if (!hssRunning_)
                return 0;
        return JLINK_HSS_Read(buf, bufSize);
}

std::string
JLinkDev::lastError() const
{
        std::lock_guard lk(mtx_);
        return lastErr_;
}

void
JLinkDev::drawUI()
{
        char nameBuf[64];
        {
                std::lock_guard lk(mtx_);
                snprintf(nameBuf, sizeof(nameBuf), "%s", deviceName_.c_str());
        }

        char btnLabel[128];
        snprintf(btnLabel, sizeof(btnLabel), "%s##jlink_dev_btn", nameBuf[0] != '\0' ? nameBuf : "Select Device");
        if (ImGui::Button(btnLabel, ImVec2(180.0f, 0))) {
                JLINKARM_DEVICE_SELECT_INFO sinfo;
                sinfo.SizeOfStruct = sizeof(sinfo);
                sinfo.CoreIndex    = 0;
                int32_t devIdx     = JLINKARM_DEVICE_SelectDialog(nullptr, 0, &sinfo);
                if (devIdx >= 0) {
                        JLINKARM_DEVICE_INFO dinfo;
                        dinfo.SizeOfStruct = sizeof(dinfo);
                        if (JLINKARM_DEVICE_GetInfo(devIdx, &dinfo) >= 0) {
                                std::lock_guard lk(mtx_);
                                deviceName_ = dinfo.sName;
                        }
                }
        }
        ImGui::SameLine();
        ImGui::Text("device");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        bool speedCommitted = false;
        {
                std::lock_guard lk(mtx_);
                ImGui::InputInt("kHz##jlink", &speedKHz_, 0, 0);
                speedCommitted = ImGui::IsItemDeactivatedAfterEdit();
                if (speedKHz_ < 1)
                        speedKHz_ = 1;
        }
        if (speedCommitted && isOpen_) {
                std::lock_guard lk(mtx_);
                JLINKARM_SetSpeed(static_cast<u32>(speedKHz_));
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("J-Link SWD speed (kHz). Applies when you finish editing.");

        ImGui::SameLine();
        if (!isConnected_) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f)); // Red
                if (ImGui::SmallButton("CONNECT")) {
                        if (!isOpen_)
                                open();
                        if (connect()) {
                                // Handled by Gui::drawBar connection detection
                        }
                }
                ImGui::PopStyleColor();
        } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.8f, 0.2f, 1.0f)); // Green
                if (ImGui::SmallButton("DISCONNECT")) {
                        close();
                }
                ImGui::PopStyleColor();
        }

        if (!lastErr_.empty()) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::Text("Error: %s", lastErr_.c_str());
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "Try reducing the sampling frequency (Hz) if you see 'Low on memory' or 'Start failed'.");
                ImGui::PopStyleColor();
        }
}
