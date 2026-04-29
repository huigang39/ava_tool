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
                lastErr_     = "Connect failed";
                isConnected_ = false;
                return false;
        }
        isConnected_ = true;
        lastErr_.clear();
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
        u32                                   frameSize = 0;
        for (usize i = 0; i < blocks.size(); ++i) {
                descs[i].Addr     = blocks[i].addr;
                descs[i].NumBytes = blocks[i].numBytes;
                descs[i].Flags    = 0;
                descs[i].Dummy    = 0;
                frameSize += blocks[i].numBytes;
        }

        if (JLINK_HSS_Start(descs.data(), static_cast<i32>(descs.size()), periodUs, 0) < 0) {
                char buf[64];
                snprintf(buf, sizeof(buf), "HSS_Start failed (period=%dus, n=%zu)", periodUs, blocks.size());
                lastErr_ = buf;
                return false;
        }

        hssRunning_   = true;
        hssFrameSize_ = static_cast<int>(frameSize);
        hssPeriodUs_  = periodUs;
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

        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::InputText("device##jlink", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                std::lock_guard lk(mtx_);
                deviceName_ = nameBuf;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        bool speedCommitted = false;
        {
                std::lock_guard lk(mtx_);
                ImGui::InputInt("kHz##jlink", &speedKHz_);
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
        if (!isOpen_) {
                if (ImGui::SmallButton("Open"))
                        open();
        } else if (!isConnected_) {
                if (ImGui::SmallButton("Connect"))
                        connect();
                ImGui::SameLine();
                if (ImGui::SmallButton("Close"))
                        close();
        } else {
                if (ImGui::SmallButton("Disconnect"))
                        close();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("[%s%s%s]",
                            isOpen_ ? "open" : "closed",
                            isConnected_ ? "+conn" : "",
                            hssRunning_ ? "+HSS" : "");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        {
                std::lock_guard lk(mtx_);
                int hz = (hssPeriodUs_ > 0) ? (1000000 / hssPeriodUs_) : 1000;
                if (ImGui::DragInt("Hz##hsshz", &hz, 10.0f, 10, 100000, "%d Hz")) {
                        if (hz < 10)
                                hz = 10;
                        hssPeriodUs_ = 1000000 / hz;
                        if (hssPeriodUs_ < 10)
                                hssPeriodUs_ = 10;
                }
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("HSS sample frequency. Period = 1e6 / Hz us.\n10 Hz - 100000 Hz");

        if (!lastErr_.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", lastErr_.c_str());
        }
}
