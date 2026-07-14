/**
 * @file  jlink_port.cpp
 * @brief JLinkPort implementation — J-Link SDK wrapper.
 */
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <devguid.h>
#include <setupapi.h>
#endif

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

JLinkPort::~JLinkPort()
{
        vcomClose();
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
        (void)regIndex;
        return 0;
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

#ifdef _WIN32
static HANDLE
vcomHandleFromVoid(void *h)
{
        return reinterpret_cast<HANDLE>(h);
}
#endif

#ifdef _WIN32
static std::string
lowerAscii(std::string s)
{
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
}

static bool
extractComName(const std::string &text, std::string &out)
{
        size_t pos = text.find("COM");
        while (pos != std::string::npos) {
                size_t i = pos + 3;
                if (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
                        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
                                ++i;
                        out = text.substr(pos, i - pos);
                        return true;
                }
                pos = text.find("COM", pos + 3);
        }
        return false;
}

static std::string
setupDiStringProperty(HDEVINFO info, SP_DEVINFO_DATA &devInfo, DWORD prop)
{
        char  buf[512] = {};
        DWORD type     = 0;
        if (!SetupDiGetDeviceRegistryPropertyA(info, &devInfo, prop, &type, reinterpret_cast<PBYTE>(buf), sizeof(buf), nullptr))
                return {};
        return buf;
}
#endif

bool
JLinkPort::vcomRefreshPorts(bool preferJLink)
{
#ifdef _WIN32
        if (vcomOpen_.load(std::memory_order_acquire)) {
                lastErr_ = "VCOM: close port before refresh";
                return false;
        }

        HDEVINFO info = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
        if (info == INVALID_HANDLE_VALUE) {
                lastErr_ = "VCOM: SetupDiGetClassDevs failed";
                return false;
        }

        std::vector<std::string> ports;
        std::vector<std::string> labels;
        int                      jlinkIdx = -1;
        for (DWORD idx = 0;; ++idx) {
                SP_DEVINFO_DATA devInfo = {};
                devInfo.cbSize          = sizeof(devInfo);
                if (!SetupDiEnumDeviceInfo(info, idx, &devInfo))
                        break;

                std::string friendly = setupDiStringProperty(info, devInfo, SPDRP_FRIENDLYNAME);
                std::string desc     = setupDiStringProperty(info, devInfo, SPDRP_DEVICEDESC);
                std::string mfg      = setupDiStringProperty(info, devInfo, SPDRP_MFG);
                std::string com;
                if (!extractComName(friendly, com) && !extractComName(desc, com))
                        continue;

                std::string name    = !friendly.empty() ? friendly : (!desc.empty() ? desc : com);
                std::string hay     = lowerAscii(friendly + " " + desc + " " + mfg);
                bool        isJLink = hay.find("j-link") != std::string::npos || hay.find("jlink") != std::string::npos ||
                               hay.find("segger") != std::string::npos;

                ports.push_back(com);
                labels.push_back(com + " - " + name + (isJLink ? "  [J-Link]" : ""));
                if (isJLink && jlinkIdx < 0)
                        jlinkIdx = static_cast<int>(ports.size()) - 1;
        }
        SetupDiDestroyDeviceInfoList(info);

        if (ports.empty()) {
                vcomPortList_.clear();
                vcomPortLabels_.clear();
                vcomSelectedPort_ = -1;
                lastErr_          = "VCOM: no COM ports found";
                return false;
        }

        int selected = -1;
        if (preferJLink && jlinkIdx >= 0) {
                selected = jlinkIdx;
        } else {
                for (int i = 0; i < static_cast<int>(ports.size()); ++i) {
                        if (_stricmp(ports[i].c_str(), vcomPort_) == 0) {
                                selected = i;
                                break;
                        }
                }
                if (selected < 0)
                        selected = 0;
        }

        vcomPortList_     = std::move(ports);
        vcomPortLabels_   = std::move(labels);
        vcomSelectedPort_ = selected;
        snprintf(vcomPort_, sizeof(vcomPort_), "%s", vcomPortList_[selected].c_str());
        lastErr_.clear();
        LOG_I("JLink VCOM ports refreshed: count=%zu selected=%s", vcomPortList_.size(), vcomPort_);
        return true;
#else
        (void)preferJLink;
        lastErr_ = "VCOM: refresh only supported on Windows";
        return false;
#endif
}
bool
JLinkPort::vcomOpen()
{
#ifdef _WIN32
        std::lock_guard lk(vcomMtx_);
        if (vcomOpen_.load(std::memory_order_acquire))
                return true;

        std::string port = vcomPort_;
        if (port.empty()) {
                lastErr_ = "VCOM: empty COM port";
                return false;
        }
        std::string path = port;
        if (path.rfind("\\\\.\\", 0) != 0)
                path = "\\\\.\\" + path;

        HANDLE h =
            CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
                char msg[128];
                snprintf(msg, sizeof(msg), "VCOM: failed to open %s (err=%lu)", port.c_str(), GetLastError());
                lastErr_ = msg;
                return false;
        }

        DCB dcb       = {};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(h, &dcb)) {
                CloseHandle(h);
                lastErr_ = "VCOM: GetCommState failed";
                return false;
        }
        dcb.BaudRate    = static_cast<DWORD>(vcomBaud_.load(std::memory_order_relaxed));
        dcb.ByteSize    = 8;
        dcb.Parity      = NOPARITY;
        dcb.StopBits    = ONESTOPBIT;
        dcb.fBinary     = TRUE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        if (!SetCommState(h, &dcb)) {
                CloseHandle(h);
                lastErr_ = "VCOM: SetCommState failed";
                return false;
        }

        COMMTIMEOUTS timeouts                = {};
        timeouts.ReadIntervalTimeout         = 20;
        timeouts.ReadTotalTimeoutConstant    = 20;
        timeouts.ReadTotalTimeoutMultiplier  = 1;
        timeouts.WriteTotalTimeoutConstant   = 200;
        timeouts.WriteTotalTimeoutMultiplier = 2;
        SetCommTimeouts(h, &timeouts);
        PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

        vcomHandle_ = h;
        vcomReaderRunning_.store(true, std::memory_order_release);
        vcomOpen_.store(true, std::memory_order_release);
        lastErr_.clear();
        vcomThread_ = std::thread([this]() {
                char buf[256];
                while (vcomReaderRunning_.load(std::memory_order_acquire)) {
                        HANDLE rh = nullptr;
                        {
                                std::lock_guard lk(vcomMtx_);
                                rh = vcomHandleFromVoid(vcomHandle_);
                        }
                        if (!rh || rh == INVALID_HANDLE_VALUE)
                                break;

                        DWORD nr = 0;
                        if (ReadFile(rh, buf, sizeof(buf), &nr, nullptr) && nr > 0) {
                                std::lock_guard rx(vcomRxMtx_);
                                vcomRxLog_.append(buf, buf + nr);
                                static constexpr size_t kMaxLog = 64 * 1024;
                                if (vcomRxLog_.size() > kMaxLog)
                                        vcomRxLog_.erase(0, vcomRxLog_.size() - kMaxLog);
                        } else {
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        }
                }
        });
        return true;
#else
        lastErr_ = "VCOM: only supported on Windows";
        return false;
#endif
}

void
JLinkPort::vcomClose()
{
#ifdef _WIN32
        vcomReaderRunning_.store(false, std::memory_order_release);
        HANDLE h = nullptr;
        {
                std::lock_guard lk(vcomMtx_);
                h = vcomHandleFromVoid(vcomHandle_);
                if (h && h != INVALID_HANDLE_VALUE)
                        CancelIoEx(h, nullptr);
        }
        if (vcomThread_.joinable())
                vcomThread_.join();
        {
                std::lock_guard lk(vcomMtx_);
                h = vcomHandleFromVoid(vcomHandle_);
                if (h && h != INVALID_HANDLE_VALUE)
                        CloseHandle(h);
                vcomHandle_ = nullptr;
        }
#endif
        vcomOpen_.store(false, std::memory_order_release);
}

bool
JLinkPort::vcomSend(const char *text)
{
#ifdef _WIN32
        if (!text || !text[0])
                return true;
        std::lock_guard lk(vcomMtx_);
        HANDLE          h = vcomHandleFromVoid(vcomHandle_);
        if (!vcomOpen_.load(std::memory_order_acquire) || !h || h == INVALID_HANDLE_VALUE) {
                lastErr_ = "VCOM: not open";
                return false;
        }
        DWORD len = static_cast<DWORD>(strlen(text));
        DWORD nw  = 0;
        if (!WriteFile(h, text, len, &nw, nullptr) || nw != len) {
                lastErr_ = "VCOM: write failed";
                return false;
        }
        return true;
#else
        (void)text;
        lastErr_ = "VCOM: only supported on Windows";
        return false;
#endif
}

std::string
JLinkPort::vcomRxSnapshot() const
{
        std::lock_guard lk(vcomRxMtx_);
        return vcomRxLog_;
}

void
JLinkPort::vcomClearRx()
{
        std::lock_guard lk(vcomRxMtx_);
        vcomRxLog_.clear();
}

void
JLinkPort::drawVComPopup()
{
        if (!ImGui::BeginPopup("JLinkVComPopup"))
                return;

        ImGui::TextUnformatted(tr("J-Link Virtual COM", "J-Link 虚拟串口"));
        ImGui::Separator();

        if (vcomPortList_.empty() && !vcomOpen_.load(std::memory_order_acquire))
                vcomRefreshPorts(true);

        ImGui::SetNextItemWidth(260.0f);
        if (vcomPortLabels_.empty()) {
                ImGui::InputText("##jlink_vcom_port", vcomPort_, sizeof(vcomPort_));
        } else {
                const char *preview = (vcomSelectedPort_ >= 0 && vcomSelectedPort_ < static_cast<int>(vcomPortLabels_.size()))
                                          ? vcomPortLabels_[vcomSelectedPort_].c_str()
                                          : vcomPort_;
                if (ImGui::BeginCombo("##jlink_vcom_port_combo", preview)) {
                        for (int i = 0; i < static_cast<int>(vcomPortLabels_.size()); ++i) {
                                bool selected = i == vcomSelectedPort_;
                                if (ImGui::Selectable(vcomPortLabels_[i].c_str(), selected)) {
                                        vcomSelectedPort_ = i;
                                        snprintf(vcomPort_, sizeof(vcomPort_), "%s", vcomPortList_[i].c_str());
                                }
                                if (selected)
                                        ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                }
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("COM port", "串口号"));
        ImGui::SameLine();
        if (!vcomOpen_.load(std::memory_order_acquire)) {
                if (ui::SmallButton(tr("Refresh", "刷新"), ui::BtnStyle::Muted))
                        vcomRefreshPorts(false);
                ImGui::SameLine();
        }
        int baud = vcomBaud_.load(std::memory_order_relaxed);
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::InputInt("##jlink_vcom_baud", &baud, 0, 0)) {
                if (baud < 1200)
                        baud = 1200;
                vcomBaud_.store(baud, std::memory_order_relaxed);
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Baud rate", "波特率"));

        ImGui::SameLine();
        if (vcomOpen_.load(std::memory_order_acquire)) {
                if (ui::SmallButton(tr("CLOSE", "关闭"), ui::BtnStyle::Danger))
                        vcomClose();
        } else {
                if (ui::SmallButton(tr("OPEN", "打开"), ui::BtnStyle::Success))
                        vcomOpen();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Clear", "清空")))
                vcomClearRx();

        std::string       rx = vcomRxSnapshot();
        std::vector<char> rxBuf(rx.begin(), rx.end());
        rxBuf.push_back('\0');
        ImGui::InputTextMultiline(
            "##jlink_vcom_rx", rxBuf.data(), rxBuf.size(), ImVec2(520.0f, 220.0f), ImGuiInputTextFlags_ReadOnly);

        ImGui::SetNextItemWidth(430.0f);
        bool sendNow =
            ImGui::InputText("##jlink_vcom_tx", vcomTxBuf_, sizeof(vcomTxBuf_), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ui::SmallButton(tr("SEND", "发送"), ui::BtnStyle::Success))
                sendNow = true;
        if (sendNow) {
                vcomSend(vcomTxBuf_);
                vcomTxBuf_[0] = '\0';
        }

        ImGui::EndPopup();
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

        ImGui::SameLine();
        if (ui::SmallButton(vcomOpen_.load(std::memory_order_acquire) ? tr("VCOM ON", "串口 ON") : "VCOM",
                            vcomOpen_.load(std::memory_order_acquire) ? ui::BtnStyle::Success : ui::BtnStyle::Muted))
                ImGui::OpenPopup("JLinkVComPopup");
        drawVComPopup();

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
