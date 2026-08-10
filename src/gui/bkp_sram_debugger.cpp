#include "gui/bkp_sram_debugger.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

#include "cJSON.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "implot.h"
#include "implot_internal.h"

#include "core/jlink_port.hpp"
#include "core/session_time.hpp"
#include "gui/i18n.hpp"
#include "gui/monitor.hpp"
#include "gui/monitor_scope.hpp"
#include "gui/monitor_types.hpp"
#include "gui/ui_theme.hpp"
#include "platform/native_dlg.hpp"

namespace
{
struct BkpChannelReorderPayload {
        const void   *owner{nullptr};
        std::uint32_t index{0};
};

std::string
hexAddress(const std::uint32_t value)
{
        char text[16];
        std::snprintf(text, sizeof(text), "0x%08X", value);
        return text;
}
} // namespace

BkpSramDebugger::BkpSramDebugger()
{
        channels_.push_back({"Channel 1", 0u, ValueType::F32});
}

BkpSramDebugger::~BkpSramDebugger()
{
        stopRequested_.store(true, std::memory_order_release);
        if (worker_.joinable())
                worker_.join();
}

void
BkpSramDebugger::setSymbolResolver(std::function<bool(const std::string &, std::uint32_t &)> resolver)
{
        std::lock_guard lock(configMtx_);
        symbolResolver_ = std::move(resolver);
}

bool
BkpSramDebugger::resolveProtocolLayout(ProtocolLayout &layout) const
{
        std::function<bool(const std::string &, std::uint32_t &)> resolver;
        {
                std::lock_guard lock(configMtx_);
                resolver = symbolResolver_;
        }
        if (!resolver || !resolver("g_debug_trace_ctrl", layout.control) || !resolver("g_debug_trace_data", layout.data))
                return false;

        // ABI fallback for symbol-only release ELFs. If DWARF member symbols are
        // available they override every fallback below, including padding and
        // array strides.
        layout.seq          = layout.control + 0x0cu;
        layout.ack          = layout.control + 0x10u;
        layout.enableMask   = layout.control + 0x14u;
        layout.sampleDiv    = layout.control + 0x18u;
        layout.channelBase  = layout.control + 0x1cu;
        layout.status       = layout.control + 0x9cu;
        layout.errorChannel = layout.control + 0xa0u;
        layout.stateBase    = layout.control + 0xa4u;

        auto resolve = [&](const char *name, std::uint32_t &address) {
                std::uint32_t found = 0;
                if (resolver(name, found))
                        address = found;
        };
        resolve("g_debug_trace_ctrl.seq", layout.seq);
        resolve("g_debug_trace_ctrl.ack", layout.ack);
        resolve("g_debug_trace_ctrl.channel_enable.value", layout.enableMask);
        resolve("g_debug_trace_ctrl.sample_div", layout.sampleDiv);
        resolve("g_debug_trace_ctrl.status", layout.status);
        resolve("g_debug_trace_ctrl.error_channel", layout.errorChannel);

        std::uint32_t channel0Address = layout.channelBase;
        std::uint32_t channel0Type    = layout.channelBase + 4u;
        std::uint32_t channel1Address = layout.channelBase + 8u;
        resolve("g_debug_trace_ctrl.channel[0].address", channel0Address);
        resolve("g_debug_trace_ctrl.channel[0].type", channel0Type);
        resolve("g_debug_trace_ctrl.channel[1].address", channel1Address);
        layout.channelBase       = channel0Address;
        layout.channelStride     = channel1Address > channel0Address ? channel1Address - channel0Address : 8u;
        layout.channelTypeOffset = channel0Type - channel0Address;

        std::uint32_t state0Active = layout.stateBase;
        std::uint32_t state1Active = layout.stateBase + 32u;
        resolve("g_debug_trace_ctrl.state[0].active", state0Active);
        resolve("g_debug_trace_ctrl.state[1].active", state1Active);
        layout.stateBase   = state0Active;
        layout.stateStride = state1Active > state0Active ? state1Active - state0Active : 32u;
        auto stateOffset   = [&](const char *name, std::uint32_t &offset) {
                std::uint32_t address = 0;
                if (resolver(name, address) && address >= state0Active)
                        offset = address - state0Active;
        };
        stateOffset("g_debug_trace_ctrl.state[0].address", layout.stateAddressOffset);
        stateOffset("g_debug_trace_ctrl.state[0].type", layout.stateTypeOffset);
        stateOffset("g_debug_trace_ctrl.state[0].buffer_address", layout.stateBufferOffset);
        stateOffset("g_debug_trace_ctrl.state[0].capacity", layout.stateCapacityOffset);
        stateOffset("g_debug_trace_ctrl.state[0].generation", layout.stateGenerationOffset);
        stateOffset("g_debug_trace_ctrl.state[0].write_seq", layout.stateWriteSeqOffset);
        stateOffset("g_debug_trace_ctrl.state[0].overwrites", layout.stateOverwritesOffset);
        return layout.valid();
}

void
BkpSramDebugger::open()
{
        if (!windowOpen_)
                modified_.store(true, std::memory_order_release);
        windowOpen_     = true;
        focusRequested_ = true;
}

void
BkpSramDebugger::resetSession()
{
        stopRequested_.store(true, std::memory_order_release);
        restartRequested_.store(false, std::memory_order_release);
        if (worker_.joinable())
                worker_.join();
        state_.store(State::Stopped, std::memory_order_release);
        channels_.assign(1u, {"Channel 1", 0u, ValueType::F32});
        sampleDiv_ = 1u;
        pollIntervalMs_.store(5, std::memory_order_relaxed);
        historySeconds_.store(0.0f, std::memory_order_relaxed);
        maxDisplayPoints_ = 5000u;
        actualSampleHz_.store(0.0f, std::memory_order_relaxed);
        actualReadHz_.store(0.0f, std::memory_order_relaxed);
        viewMode_            = MonitorViewMode::FOLLOW;
        plotViewMin_         = 0.0;
        plotViewMax_         = 1.0;
        plotFollowSpan_      = 1000.0;
        plotPaneHeight_      = 320.0f;
        plotPaneRatio_       = 0.65f;
        configExpanded_      = true;
        plotCacheUpdateTime_ = 0.0;
        windowOpen_          = false;
        clearData();
        setStatus({});
}

const char *
BkpSramDebugger::typeName(const ValueType type)
{
        static constexpr const char *names[] = {"U8", "I8", "U16", "I16", "U32", "I32", "F32"};
        const auto                   idx     = static_cast<std::uint32_t>(type);
        return idx < std::size(names) ? names[idx] : "?";
}

double
BkpSramDebugger::decodeValue(const std::uint32_t raw, const ValueType type)
{
        switch (type) {
                case ValueType::U8:
                        return static_cast<std::uint8_t>(raw);
                case ValueType::I8:
                        return static_cast<std::int8_t>(raw);
                case ValueType::U16:
                        return static_cast<std::uint16_t>(raw);
                case ValueType::I16:
                        return static_cast<std::int16_t>(raw);
                case ValueType::U32:
                        return raw;
                case ValueType::I32:
                        return static_cast<std::int32_t>(raw);
                case ValueType::F32: {
                        float value = 0.0f;
                        std::memcpy(&value, &raw, sizeof(value));
                        return value;
                }
        }
        return 0.0;
}

bool
BkpSramDebugger::parseValueType(const char *text, ValueType &type)
{
        if (!text)
                return false;
        std::string value(text);
        std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
                return static_cast<char>(std::toupper(c));
        });
        static constexpr std::array<const char *, 7> names{"U8", "I8", "U16", "I16", "U32", "I32", "F32"};
        for (std::size_t i = 0; i < names.size(); ++i) {
                if (value == names[i]) {
                        type = static_cast<ValueType>(i);
                        return true;
                }
        }
        if (value == "FLOAT" || value == "FLOAT32") {
                type = ValueType::F32;
                return true;
        }
        return false;
}

bool
BkpSramDebugger::assignDroppedChannel(
    const std::size_t index, const char *name, const std::uint64_t address, const char *type, const char *device)
{
        if (index >= channels_.size())
                return false;
        if (device && device[0] != '\0' && std::strcmp(device, "JLINK") != 0) {
                setStatus(tr("Only J-Link variables can be sampled by BKP SRAM.", "BKP SRAM 只能采集 J-Link 变量。"));
                return false;
        }
        if (address > 0xffffffffull) {
                setStatus(tr("The dropped variable address exceeds 32 bits.", "拖入变量的地址超过 32 位。"));
                return false;
        }
        ValueType mappedType{};
        if (!parseValueType(type, mappedType)) {
                setStatus(tr("Unsupported variable type. BKP SRAM supports U8/I8/U16/I16/U32/I32/F32.",
                             "不支持该变量类型；BKP SRAM 支持 U8/I8/U16/I16/U32/I32/F32。"));
                return false;
        }
        auto &channel   = channels_[index];
        channel.name    = name && name[0] != '\0' ? name : ("Channel " + std::to_string(index + 1u));
        channel.address = static_cast<std::uint32_t>(address);
        channel.type    = mappedType;
        std::snprintf(channel.addressText, sizeof(channel.addressText), "0x%08X", channel.address);
        modified_.store(true, std::memory_order_release);
        setStatus({});
        return true;
}

bool
BkpSramDebugger::appendDroppedChannel(const char *name, const std::uint64_t address, const char *type, const char *device)
{
        // Treat an untouched row created by “Add channel” as an insertion slot.
        // This keeps the initial placeholder from becoming an invalid address-0
        // channel when the first real variable is dropped.
        const auto placeholder = std::find_if(channels_.begin(), channels_.end(), [](const ChannelConfig &channel) {
                return channel.address == 0u && channel.name.rfind("Channel ", 0u) == 0u;
        });
        if (placeholder != channels_.end())
                return assignDroppedChannel(
                    static_cast<std::size_t>(placeholder - channels_.begin()), name, address, type, device);
        if (channels_.size() >= kMaxChannels) {
                setStatus(tr("The maximum of 16 BKP SRAM channels has been reached.", "BKP SRAM 通道已达到 16 个上限。"));
                return false;
        }
        std::uint32_t slot = 0u;
        while (
            slot < kMaxChannels &&
            std::any_of(channels_.begin(), channels_.end(), [&](const ChannelConfig &channel) { return channel.slot == slot; }))
                ++slot;
        ChannelConfig channel;
        channel.name = "Channel " + std::to_string(slot + 1u);
        channel.slot = slot;
        channels_.push_back(std::move(channel));
        if (!assignDroppedChannel(channels_.size() - 1u, name, address, type, device)) {
                channels_.pop_back();
                return false;
        }
        return true;
}

void
BkpSramDebugger::acceptNewChannelDrop()
{
        ImGuiWindow *window = ImGui::GetCurrentWindow();
        const ImRect targetRect(window->InnerRect.Min, window->InnerRect.Max);
        if (!ImGui::BeginDragDropTargetCustom(targetRect, ImGui::GetID("##bkp_add_drop_target")))
                return;

        const ImGuiPayload *scalar = ImGui::AcceptDragDropPayload("SYMBOL_CHANNEL");
        if (!scalar)
                scalar = ImGui::AcceptDragDropPayload("CHANNEL");
        if (scalar && scalar->DataSize == sizeof(ChannelDropPayload)) {
                const auto &source = *static_cast<const ChannelDropPayload *>(scalar->Data);
                appendDroppedChannel(source.name, source.addr, source.type, source.device);
        }
        if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("STRUCT_CHANNEL")) {
                if (payload->DataSize == sizeof(StructChannelPayload)) {
                        const auto &source = *static_cast<const StructChannelPayload *>(payload->Data);
                        for (int i = 0; i < source.count && channels_.size() < kMaxChannels; ++i) {
                                const auto &entry = source.entries[i];
                                appendDroppedChannel(entry.name, entry.addr, entry.type, source.device);
                        }
                }
        }
        if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("DND_CHANNEL_MOVE")) {
                if (payload->DataSize == sizeof(ChannelMovePayload)) {
                        const auto &source = *static_cast<const ChannelMovePayload *>(payload->Data);
                        if (source.srcScope && !source.isGroup) {
                                const auto found = source.srcScope->getChannels().find(source.chName);
                                if (found != source.srcScope->getChannels().end() && found->second) {
                                        auto &channel = *found->second;
                                        appendDroppedChannel(channel.getLabel().c_str(),
                                                             channel.getAddr(),
                                                             channel.getType().c_str(),
                                                             channel.getDevice().c_str());
                                }
                        } else {
                                setStatus(tr("Drop a scalar monitor channel, not a group.",
                                             "请拖入监视器中的标量通道，而不是变量组。"));
                        }
                }
        }
        ImGui::EndDragDropTarget();
}

void
BkpSramDebugger::setStatus(const std::string &message)
{
        std::lock_guard lock(statusMtx_);
        statusText_ = message;
}

void
BkpSramDebugger::setError(const std::string &message)
{
        setStatus(message);
        state_.store(State::Error, std::memory_order_release);
}

std::string
BkpSramDebugger::statusSnapshot() const
{
        std::lock_guard lock(statusMtx_);
        return statusText_;
}

bool
BkpSramDebugger::submitConfig(const ProtocolLayout             &layout,
                              const std::vector<ChannelConfig> &config,
                              const std::uint32_t               sampleDiv,
                              const bool                        enable)
{
        auto &jlink = JLinkPort::instance();
        if (!jlink.isConnected()) {
                setError(tr("J-Link is not connected.", "J-Link 未连接。"));
                return false;
        }

        std::uint32_t enableMask = 0u;
        for (const auto &channel : config) {
                if (channel.slot >= kMaxChannels)
                        continue;
                const std::uint32_t channelAddress = layout.channelBase + channel.slot * layout.channelStride;
                const std::uint32_t type           = static_cast<std::uint32_t>(channel.type);
                if (!jlink.writeMem(channelAddress + layout.channelAddressOffset, sizeof(channel.address), &channel.address) ||
                    !jlink.writeMem(channelAddress + layout.channelTypeOffset, sizeof(type), &type)) {
                        setError(tr("Failed to write debug channel configuration.", "写入调试通道配置失败。"));
                        return false;
                }
                enableMask |= 1u << channel.slot;
        }
        if (!enable)
                enableMask = 0u;
        const std::uint32_t divider = std::max(sampleDiv, 1u);
        // The enable word is always a zero-extended 16-bit mask. seq is written
        // separately and last, making it the firmware-visible commit point.
        if (!jlink.writeMem(layout.enableMask, sizeof(enableMask), &enableMask) ||
            !jlink.writeMem(layout.sampleDiv, sizeof(divider), &divider)) {
                setError(tr("Failed to write debug trace enable/divider.", "写入调试跟踪使能或分频失败。"));
                return false;
        }

        std::uint32_t sequence = 0;
        if (!jlink.readMem(layout.seq, sizeof(sequence), &sequence)) {
                setError(tr("Failed to read configuration sequence.", "读取配置序号失败。"));
                return false;
        }
        ++sequence;
        if (sequence == 0u)
                sequence = 1u;
        if (!jlink.writeMem(layout.seq, sizeof(sequence), &sequence)) {
                setError(tr("Failed to submit configuration sequence.", "提交配置序号失败。"));
                return false;
        }

        for (int attempt = 0; attempt < 200; ++attempt) {
                std::uint32_t ack = 0;
                if (!jlink.readMem(layout.ack, sizeof(ack), &ack)) {
                        setError(tr("Failed while waiting for configuration ACK.", "等待配置 ACK 时读取失败。"));
                        return false;
                }
                if (ack == sequence) {
                        std::uint32_t status       = 0;
                        std::uint32_t errorChannel = 0xffffffffu;
                        if (!jlink.readMem(layout.status, sizeof(status), &status) ||
                            !jlink.readMem(layout.errorChannel, sizeof(errorChannel), &errorChannel)) {
                                setError(tr("Failed to read firmware configuration result.", "读取固件配置结果失败。"));
                                return false;
                        }
                        if (status != 1u) {
                                std::ostringstream message;
                                message << tr("Firmware rejected configuration: status=", "固件拒绝配置：status=") << status
                                        << tr(", error channel=", "，错误通道=") << errorChannel;
                                setError(message.str());
                                return false;
                        }
                        return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        setError(tr("Timed out waiting for configuration ACK.", "等待配置 ACK 超时。"));
        return false;
}

bool
BkpSramDebugger::readChannel(const ProtocolLayout &layout, const std::size_t index, const ValueType configuredType)
{
        if (index >= kMaxChannels)
                return false;
        auto               &jlink        = JLinkPort::instance();
        const std::uint32_t stateAddress = layout.stateBase + static_cast<std::uint32_t>(index) * layout.stateStride;
        if (layout.stateStride < 32u || layout.stateStride > 256u)
                return false;
        std::vector<std::uint8_t> state(layout.stateStride);
        if (!jlink.readMem(stateAddress, layout.stateStride, state.data()))
                return false;
        auto field = [&](const std::uint32_t offset) {
                std::uint32_t value = 0;
                if (offset + sizeof(value) <= state.size())
                        std::memcpy(&value, state.data() + offset, sizeof(value));
                return value;
        };

        const bool          active        = field(layout.stateActiveOffset) != 0u;
        const std::uint32_t actualAddress = field(layout.stateAddressOffset);
        const std::uint32_t actualTypeRaw = field(layout.stateTypeOffset);
        const std::uint32_t sampleBase    = field(layout.stateBufferOffset);
        const std::uint32_t capacity      = field(layout.stateCapacityOffset);
        const std::uint32_t generation    = field(layout.stateGenerationOffset);
        const std::uint32_t writeSeq      = field(layout.stateWriteSeqOffset);
        const std::uint32_t overwrites    = field(layout.stateOverwritesOffset);
        const ValueType     actualType    = actualTypeRaw <= static_cast<std::uint32_t>(ValueType::F32)
                                                ? static_cast<ValueType>(actualTypeRaw)
                                                : configuredType;

        if (!active) {
                std::lock_guard lock(dataMtx_);
                runtime_[index].active     = false;
                runtime_[index].generation = generation;
                return true;
        }
        const std::uint64_t poolEnd = static_cast<std::uint64_t>(layout.data) + kDataPoolSize;
        const std::uint64_t bufferEnd =
            static_cast<std::uint64_t>(sampleBase) + static_cast<std::uint64_t>(capacity) * kSampleStride;
        if (capacity == 0u || capacity > kDataPoolSize / kSampleStride || sampleBase < layout.data || bufferEnd > poolEnd) {
                setError(tr("Firmware returned an invalid debug-trace buffer layout.", "固件返回的调试跟踪缓冲区布局无效。"));
                return false;
        }

        std::uint32_t cursor = 0;
        {
                std::lock_guard lock(dataMtx_);
                auto           &runtime = runtime_[index];
                runtime.active          = active;
                runtime.writeSeq        = writeSeq;
                runtime.address         = actualAddress;
                runtime.capacity        = capacity;
                runtime.overwrites      = overwrites;
                // write_seq and the per-channel cursor are uint32_t sequence numbers. Keep all
                // distance calculations unsigned so they remain valid when write_seq wraps from
                // UINT32_MAX to zero. Once the firmware reports an overwrite, the ring is full
                // even when the wrapped write_seq is numerically smaller than capacity.
                const bool          ringFull = overwrites != 0u || writeSeq >= capacity;
                const std::uint32_t oldest   = ringFull ? writeSeq - capacity : 0u;
                if (!runtime.generationKnown || runtime.generation != generation) {
                        runtime.generationKnown = true;
                        runtime.generation      = generation;
                        runtime.cursor          = oldest;
                        runtime.displayType     = actualType;
                        runtime.points.clear();
                        runtime.display.clear();
                }
                // A modular distance greater than the capacity means the reader fell behind.
                // A normal numeric comparison is incorrect on the UINT32_MAX -> 0 boundary.
                if (writeSeq - runtime.cursor > capacity)
                        runtime.cursor = writeSeq - capacity;
                cursor = runtime.cursor;
        }

        const std::uint32_t       sampleBytes = capacity * kSampleStride;
        std::vector<std::uint8_t> before(sampleBytes);
        std::vector<std::uint8_t> after(sampleBytes);
        if (!jlink.readMem(sampleBase, sampleBytes, before.data()))
                return false;
        if (!jlink.readMem(sampleBase, sampleBytes, after.data()))
                return false;

        std::uint32_t remaining = writeSeq - cursor;
        if (remaining > capacity) {
                cursor    = writeSeq - capacity;
                remaining = capacity;
        }
        const bool         attemptedRead = remaining != 0u;
        std::vector<Point> collected;
        std::uint64_t      dropped  = 0;
        std::uint32_t      expected = cursor;
        std::uint32_t      observed = cursor;
        while (remaining != 0u && !stopRequested_.load(std::memory_order_acquire)) {
                --remaining;
                const std::size_t offset    = static_cast<std::size_t>(cursor % capacity) * kSampleStride;
                std::uint32_t     seqBefore = 0;
                std::uint32_t     seqAfter  = 0;
                std::uint32_t     tick      = 0;
                std::uint32_t     raw       = 0;
                std::memcpy(&seqBefore, before.data() + offset, sizeof(seqBefore));
                std::memcpy(&tick, before.data() + offset + 4u, sizeof(tick));
                std::memcpy(&raw, before.data() + offset + 8u, sizeof(raw));
                std::memcpy(&seqAfter, after.data() + offset, sizeof(seqAfter));
                expected = cursor;
                observed = seqAfter;
                if (seqBefore != cursor || seqAfter != cursor) {
                        ++dropped;
                        ++cursor;
                        continue;
                }

                collected.push_back(
                    {cursor, tick, raw, static_cast<double>(tick), decodeValue(raw, actualType), sessionTimeSec()});
                ++cursor;
        }

        std::uint32_t generationAfter = 0;
        if (!jlink.readMem(stateAddress + layout.stateGenerationOffset, sizeof(generationAfter), &generationAfter))
                return false;
        if (generationAfter != generation) {
                std::lock_guard lock(dataMtx_);
                runtime_[index].generationKnown = false;
                return true; // discard the complete batch and reacquire layout
        }

        {
                std::lock_guard lock(dataMtx_);
                auto           &runtime = runtime_[index];
                runtime.points.insert(runtime.points.end(), collected.begin(), collected.end());
                for (const auto &point : collected)
                        runtime.display.push(static_cast<float>(point.value), point.plotTick);
                const float historySeconds = historySeconds_.load(std::memory_order_relaxed);
                if (historySeconds > 0.0f && !runtime.points.empty()) {
                        const double cutoff = runtime.points.back().hostTimeSec - historySeconds;
                        const auto   keep   = std::lower_bound(
                            runtime.points.begin(), runtime.points.end(), cutoff, [](const Point &point, const double time) {
                                    return point.hostTimeSec < time;
                            });
                        if (keep != runtime.points.begin()) {
                                const std::size_t drop = static_cast<std::size_t>(keep - runtime.points.begin());
                                runtime.points.erase(runtime.points.begin(), keep);
                                runtime.display.dropFront(drop);
                        }
                }
                runtime.cursor            = cursor;
                runtime.writeSeq          = writeSeq;
                runtime.expectedSeq       = expected;
                runtime.observedSeq       = observed;
                runtime.droppedSamples   += dropped;
                runtime.validatedSamples += collected.size();
        }
        if (collected.empty() && attemptedRead)
                setStatus(tr("Target ring buffer is being overwritten before J-Link can read it; increase the sample divider.",
                             "目标环形区在 J-Link 读完前已被覆盖，请增大采样分频。"));
        return true;
}

void
BkpSramDebugger::workerLoop(std::vector<ChannelConfig> config, const std::uint32_t sampleDiv, const ProtocolLayout layout)
{
        std::uint64_t configuredEpoch    = 0u;
        int           readFailures       = 0;
        bool          rateKnown          = false;
        std::uint32_t lastRateWriteSeq   = 0u;
        std::uint32_t lastRateGeneration = 0u;
        std::uint64_t lastRateReadCount  = 0u;
        auto          lastRateTime       = std::chrono::steady_clock::now();
        while (!stopRequested_.load(std::memory_order_acquire)) {
                auto &jlink = JLinkPort::instance();
                if (!jlink.isConnected()) {
                        configuredEpoch = 0u;
                        rateKnown       = false;
                        actualSampleHz_.store(0.0f, std::memory_order_relaxed);
                        actualReadHz_.store(0.0f, std::memory_order_relaxed);
                        state_.store(State::Starting, std::memory_order_release);
                        setStatus(tr("Waiting for J-Link; configuration will be restored after reconnect.",
                                     "正在等待 J-Link；重新连接后将自动恢复配置。"));
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                }

                const std::uint64_t epoch = jlink.targetEpoch();
                if (configuredEpoch != epoch) {
                        {
                                std::lock_guard lock(dataMtx_);
                                for (auto &runtime : runtime_)
                                        runtime.generationKnown = false;
                        }
                        state_.store(State::Starting, std::memory_order_release);
                        setStatus(tr("Submitting configuration after target connection/reset...",
                                     "目标连接或复位后正在重新提交配置……"));
                        if (!submitConfig(layout, config, sampleDiv, true)) {
                                if (!jlink.isConnected()) {
                                        configuredEpoch = 0u;
                                        continue;
                                }
                                return;
                        }
                        configuredEpoch = epoch;
                        readFailures    = 0;
                        rateKnown       = false;
                        actualSampleHz_.store(0.0f, std::memory_order_relaxed);
                        actualReadHz_.store(0.0f, std::memory_order_relaxed);
                        state_.store(State::Running, std::memory_order_release);
                        setStatus({});
                }

                bool ok = true;
                for (const auto &channel : config) {
                        if (!readChannel(layout, channel.slot, channel.type)) {
                                ok = false;
                                break;
                        }
                }
                if (!ok) {
                        if (state_.load(std::memory_order_acquire) == State::Error)
                                return;
                        if (!jlink.isConnected()) {
                                configuredEpoch = 0u;
                                continue;
                        }
                        if (++readFailures >= 10) {
                                setError(tr("Debug trace repeatedly failed while J-Link remained connected.",
                                            "J-Link 保持连接时调试跟踪连续读取失败。"));
                                return;
                        }
                        setStatus(tr("Debug trace read failed; retrying...", "调试跟踪读取失败，正在重试……"));
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        continue;
                }
                readFailures = 0;
                if (!config.empty()) {
                        std::uint32_t currentWriteSeq   = 0u;
                        std::uint32_t currentGeneration = 0u;
                        std::uint64_t currentReadCount  = 0u;
                        {
                                std::lock_guard lock(dataMtx_);
                                currentWriteSeq   = runtime_[config.front().slot].writeSeq;
                                currentGeneration = runtime_[config.front().slot].generation;
                                currentReadCount  = runtime_[config.front().slot].validatedSamples;
                        }
                        const auto rateNow = std::chrono::steady_clock::now();
                        if (!rateKnown || currentGeneration != lastRateGeneration) {
                                rateKnown          = true;
                                lastRateWriteSeq   = currentWriteSeq;
                                lastRateGeneration = currentGeneration;
                                lastRateReadCount  = currentReadCount;
                                lastRateTime       = rateNow;
                        } else {
                                const double elapsed = std::chrono::duration<double>(rateNow - lastRateTime).count();
                                if (elapsed >= 0.5) {
                                        // Unsigned subtraction also handles the normal uint32 wrap.
                                        const std::uint32_t produced = currentWriteSeq - lastRateWriteSeq;
                                        const std::uint64_t received = currentReadCount - lastRateReadCount;
                                        const float measured     = static_cast<float>(static_cast<double>(produced) / elapsed);
                                        const float measuredRead = static_cast<float>(static_cast<double>(received) / elapsed);
                                        const float previous     = actualSampleHz_.load(std::memory_order_relaxed);
                                        const float previousRead = actualReadHz_.load(std::memory_order_relaxed);
                                        actualSampleHz_.store(previous > 0.0f ? previous * 0.7f + measured * 0.3f : measured,
                                                              std::memory_order_relaxed);
                                        actualReadHz_.store(previousRead > 0.0f ? previousRead * 0.7f + measuredRead * 0.3f
                                                                                : measuredRead,
                                                            std::memory_order_relaxed);
                                        lastRateWriteSeq  = currentWriteSeq;
                                        lastRateReadCount = currentReadCount;
                                        lastRateTime      = rateNow;
                                }
                        }
                }
                bool allActive = true;
                {
                        std::lock_guard lock(dataMtx_);
                        for (const auto &channel : config) {
                                if (!runtime_[channel.slot].active) {
                                        allActive = false;
                                        break;
                                }
                        }
                }
                if (!allActive) {
                        // An external reset can clear DTCM without changing the
                        // probe connection state. Treat inactive configured
                        // channels as a lost target configuration and restore it.
                        configuredEpoch = 0u;
                        rateKnown       = false;
                        actualSampleHz_.store(0.0f, std::memory_order_relaxed);
                        actualReadHz_.store(0.0f, std::memory_order_relaxed);
                        setStatus(
                            tr("Target configuration was cleared; restoring it...", "检测到目标配置已清零，正在自动恢复……"));
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        continue;
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(std::max(pollIntervalMs_.load(std::memory_order_relaxed), 1)));
        }

        state_.store(State::Stopping, std::memory_order_release);
        // A disconnected target is already effectively stopped. Never turn a
        // user-requested stop into an error merely because the probe is absent.
        const bool disabled = !JLinkPort::instance().isConnected() || submitConfig(layout, config, sampleDiv, false);
        if (disabled) {
                std::lock_guard lock(dataMtx_);
                for (const auto &channel : config)
                        runtime_[channel.slot].active = false;
        }
        state_.store(disabled ? State::Stopped : State::Error, std::memory_order_release);
        actualSampleHz_.store(0.0f, std::memory_order_relaxed);
        actualReadHz_.store(0.0f, std::memory_order_relaxed);
        if (disabled)
                setStatus({});
}

void
BkpSramDebugger::start()
{
        if (state_.load(std::memory_order_acquire) == State::Running ||
            state_.load(std::memory_order_acquire) == State::Starting)
                return;
        if (worker_.joinable())
                worker_.join();

        std::vector<ChannelConfig> config;
        std::uint32_t              sampleDiv = 1;
        {
                std::lock_guard lock(configMtx_);
                config    = channels_;
                sampleDiv = sampleDiv_;
        }
        if (config.empty() || config.size() > kMaxChannels) {
                setError(tr("Configure between 1 and 16 channels.", "请配置 1 到 16 个通道。"));
                return;
        }
        std::uint32_t usedSlots = 0u;
        for (const auto &channel : config) {
                if (channel.slot >= kMaxChannels || (usedSlots & (1u << channel.slot)) != 0u) {
                        setError(
                            tr("Channel numbers must be unique and between 0 and 15.", "通道编号必须唯一且位于 0 到 15。"));
                        return;
                }
                usedSlots |= 1u << channel.slot;
        }
        ProtocolLayout layout;
        if (!resolveProtocolLayout(layout)) {
                setError(tr("Load an ELF containing g_debug_trace_ctrl and g_debug_trace_data first.",
                            "请先加载包含 g_debug_trace_ctrl 和 g_debug_trace_data 的 ELF。"));
                return;
        }
        stopRequested_.store(false, std::memory_order_release);
        state_.store(State::Starting, std::memory_order_release);
        setStatus(tr("Submitting configuration...", "正在提交配置……"));
        worker_ = std::thread(&BkpSramDebugger::workerLoop, this, std::move(config), sampleDiv, layout);
}

void
BkpSramDebugger::stop()
{
        const State state = state_.load(std::memory_order_acquire);
        if (state == State::Running || state == State::Starting) {
                stopRequested_.store(true, std::memory_order_release);
                state_.store(State::Stopping, std::memory_order_release);
                setStatus(tr("Stopping sampling...", "正在停止采样……"));
        }
}

void
BkpSramDebugger::clearData()
{
        std::lock_guard lock(dataMtx_);
        for (auto &runtime : runtime_)
                runtime.points.clear();
        for (auto &runtime : runtime_)
                runtime.display.clear();
        for (auto &points : plotCache_)
                points.clear();
}

void
BkpSramDebugger::exportCsv()
{
        const std::string path =
            nativeDlgSave(tr("Export BKP SRAM Data", "导出 BKP SRAM 数据"), {{"CSV", {"csv"}}}, "bkp_sram_capture.csv");
        if (path.empty())
                return;

        std::vector<ChannelConfig>                   config;
        std::array<std::vector<Point>, kMaxChannels> points;
        {
                std::lock_guard lock(configMtx_);
                config = channels_;
        }
        {
                std::lock_guard lock(dataMtx_);
                for (std::size_t i = 0; i < config.size(); ++i)
                        points[i] = runtime_[config[i].slot].points;
        }
        std::ofstream file(std::filesystem::path(reinterpret_cast<const char8_t *>(path.c_str())), std::ios::trunc);
        if (!file) {
                setStatus(tr("Could not create CSV file.", "无法创建 CSV 文件。"));
                return;
        }
        file << "channel,name,type,address,seq,tick,raw_hex,value\n";
        file << std::setprecision(9);
        for (std::size_t i = 0; i < config.size(); ++i) {
                for (const auto &point : points[i]) {
                        file << config[i].slot << ",\"" << config[i].name << "\"," << typeName(config[i].type) << ','
                             << hexAddress(config[i].address) << ',' << point.seq << ',' << point.tick << ",0x" << std::hex
                             << std::setw(8) << std::setfill('0') << point.raw << std::dec << std::setfill(' ') << ','
                             << point.value << '\n';
                }
        }
        setStatus(file.good() ? tr("CSV export completed.", "CSV 导出完成。")
                              : tr("CSV export failed while writing.", "写入 CSV 时失败。"));
}

void
BkpSramDebugger::draw()
{
        if (!windowOpen_)
                return;
        if (restartRequested_.load(std::memory_order_acquire) && (state_.load(std::memory_order_acquire) == State::Stopped ||
                                                                  state_.load(std::memory_order_acquire) == State::Error)) {
                restartRequested_.store(false, std::memory_order_release);
                start();
        }
        if (focusRequested_) {
                ImGui::SetNextWindowFocus();
                focusRequested_ = false;
        }
        const bool wasOpen = windowOpen_;
        if (!ImGui::Begin(tr("BKP SRAM Capture###BkpSramCapture", "BKP SRAM 实时采集###BkpSramCapture"), &windowOpen_)) {
                ImGui::End();
                if (wasOpen != windowOpen_)
                        modified_.store(true, std::memory_order_release);
                return;
        }
        if (wasOpen != windowOpen_)
                modified_.store(true, std::memory_order_release);

        const State state   = state_.load(std::memory_order_acquire);
        const bool  busy    = state == State::Starting || state == State::Stopping;
        const bool  running = state == State::Running;
        const bool  active  = running || state == State::Starting;
        ImGui::TextDisabled("g_debug_trace_data: BKP SRAM (4096 B)");
        ImGui::SameLine();
        ImGui::TextDisabled("| %s", tr("16 channels max / dynamically shared ring pool", "最多 16 通道 / 动态均分环形数据池"));

        ImGui::SetNextItemWidth(150.0f);
        std::uint32_t           div    = sampleDiv_;
        constexpr std::uint32_t minDiv = 1u;
        constexpr std::uint32_t maxDiv = 32u;
        if (ImGui::SliderScalar(tr("Sample divider", "采样分频"),
                                ImGuiDataType_U32,
                                &div,
                                &minDiv,
                                &maxDiv,
                                "%u",
                                ImGuiSliderFlags_Logarithmic)) {
                sampleDiv_ = std::max(div, 1u);
                modified_.store(true, std::memory_order_release);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0f);
        int pollInterval = pollIntervalMs_.load(std::memory_order_relaxed);
        if (ImGui::SliderInt(tr("Read interval", "读取周期"), &pollInterval, 1, 100, "%d ms")) {
                pollIntervalMs_.store(pollInterval, std::memory_order_relaxed);
                modified_.store(true, std::memory_order_release);
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        float historySeconds = historySeconds_.load(std::memory_order_relaxed);
        if (ImGui::SliderFloat("##bkp_history", &historySeconds, 0.0f, 3600.0f, "%.1f s", ImGuiSliderFlags_Logarithmic)) {
                historySeconds_.store(historySeconds, std::memory_order_relaxed);
                modified_.store(true, std::memory_order_release);
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("History(s), 0 keeps all data", "历史时长(秒)，0 表示保留全部数据"));

        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        int maxPoints = static_cast<int>(maxDisplayPoints_);
        if (ImGui::SliderInt("##bkp_max_points", &maxPoints, 100, 100000, "%d pts", ImGuiSliderFlags_Logarithmic)) {
                maxDisplayPoints_ = static_cast<std::uint32_t>(maxPoints);
                modified_.store(true, std::memory_order_release);
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Maximum displayed points", "最大显示点数"));

        ImGui::SameLine();
        const float actualHz = actualSampleHz_.load(std::memory_order_relaxed);
        const float readHz   = actualReadHz_.load(std::memory_order_relaxed);
        if (actualHz > 0.05f || readHz > 0.05f) {
                const float  ratio     = actualHz > 0.05f ? readHz / actualHz : 1.0f;
                const ImVec4 rateColor = ratio >= 0.9f   ? ImVec4(0.35f, 0.9f, 0.45f, 1.0f)
                                         : ratio >= 0.5f ? ImVec4(1.0f, 0.75f, 0.25f, 1.0f)
                                                         : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                ImGui::TextColored(
                    rateColor, tr("Write %.1f Hz | Read %.1f Hz", "写入 %.1f Hz | 读取 %.1f Hz"), actualHz, readHz);
        } else {
                ImGui::TextDisabled("%s", tr("Write -- Hz | Read -- Hz", "写入 -- Hz | 读取 -- Hz"));
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                                  tr("Write: samples produced by the MCU from write_seq.\n"
                                     "Read: stable samples accepted by the host after both seq checks.\n"
                                     "Rates are per channel, using the first configured channel.",
                                     "写入：根据 write_seq 计算的 MCU 样本生成率。\n"
                                     "读取：通过前后两次 seq 校验后上位机成功接收的样本率。\n"
                                     "两者均按第一个已配置通道计算。"));
        if (!active && !busy) {
                if (ui::Button(tr("Start", "启动"), ui::BtnStyle::Success))
                        start();
        } else if (active) {
                if (ui::Button(tr("Stop", "停止"), ui::BtnStyle::Danger))
                        stop();
                if (running) {
                        ImGui::SameLine();
                        if (ui::Button(tr("Apply configuration", "应用配置"), ui::BtnStyle::Warning)) {
                                restartRequested_.store(true, std::memory_order_release);
                                stop();
                        }
                }
        } else {
                ui::Button(tr("Working...", "处理中……"), ui::BtnStyle::Muted);
        }
        ImGui::SameLine();
        if (ui::Button(tr("Clear Data", "清空数据"), ui::BtnStyle::Warning))
                clearData();
        ImGui::SameLine();
        if (ui::Button(tr("Export CSV", "导出 CSV"), ui::BtnStyle::Success))
                exportCsv();

        const char *viewModeNames[] = {tr("FULL", "全览"), tr("FOLLOW", "跟随"), tr("MANUAL", "手动")};
        const float modeWidth       = 90.0f;
        const float modeX           = ImGui::GetWindowContentRegionMax().x - modeWidth;
        if (ImGui::GetCursorPosX() < modeX)
                ImGui::SameLine(modeX);
        else {
                ImGui::NewLine();
                ImGui::SetCursorPosX(modeX);
        }
        int mode = static_cast<int>(viewMode_);
        ImGui::SetNextItemWidth(modeWidth);
        if (ImGui::Combo("##bkp_view_mode", &mode, viewModeNames, 3)) {
                viewMode_ = static_cast<MonitorViewMode>(mode);
                if (viewMode_ == MonitorViewMode::FOLLOW)
                        plotFollowSpan_ = std::max(1.0, plotViewMax_ - plotViewMin_);
                modified_.store(true, std::memory_order_release);
        }

        const std::string status = statusSnapshot();
        if (!status.empty()) {
                const ImVec4 color = state == State::Error ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f) : ImVec4(0.45f, 0.85f, 0.55f, 1.0f);
                ImGui::TextColored(color, "%s", status.c_str());
        }

        // Keep every acquired point in runtime_. The GUI cache is only a
        // decimated view of that complete history, just like Scope display data.
        const double now              = ImGui::GetTime();
        const bool   paused           = g_monitorPaused.load(std::memory_order_acquire);
        const bool   plotCacheUpdated = !paused && now - plotCacheUpdateTime_ >= (1.0 / 30.0);
        if (plotCacheUpdated) {
                plotCacheUpdateTime_ = now;
                std::lock_guard lock(dataMtx_);
                for (auto &points : plotCache_)
                        points.clear();
                for (std::size_t i = 0; i < channels_.size(); ++i) {
                        const std::size_t slot    = channels_[i].slot;
                        const auto       &runtime = runtime_[slot];
                        const auto       &source  = runtime.points;
                        const auto       &display = runtime.display;
                        auto             &dest    = plotCache_[slot];
                        if (source.empty())
                                continue;
                        double xmin = source.front().plotTick;
                        double xmax = source.back().plotTick;
                        if (viewMode_ == MonitorViewMode::FOLLOW) {
                                xmin = xmax - std::max(1.0, plotFollowSpan_);
                        } else if (viewMode_ == MonitorViewMode::MANUAL) {
                                xmin = plotViewMin_;
                                xmax = plotViewMax_;
                        }
                        const std::size_t maxPlotPoints = std::max<std::size_t>(maxDisplayPoints_, 2u);
                        const int         level         = display.pickLevel(xmin, xmax, maxPlotPoints);
                        if (level < 0) {
                                const std::size_t first = display.rawLowerBound(xmin);
                                const std::size_t last  = display.rawUpperBound(xmax);
                                dest.reserve(last > first ? last - first : 0u);
                                for (std::size_t p = first; p < last; ++p) {
                                        Point point{};
                                        point.plotTick = display.rawTs(p);
                                        point.value    = display.rawVal(p);
                                        dest.push_back(point);
                                }
                        } else {
                                const std::size_t first = display.lodLowerBound(level, xmin);
                                const std::size_t last  = display.lodUpperBound(level, xmax);
                                dest.reserve(last > first ? (last - first) * 2u : 0u);
                                for (std::size_t p = first; p < last; ++p) {
                                        const LodSample &sample = display.lodAt(level, p);
                                        Point            lo{};
                                        lo.plotTick = sample.t;
                                        lo.value    = sample.vmin;
                                        Point hi    = lo;
                                        hi.value    = sample.vmax;
                                        dest.push_back(lo);
                                        dest.push_back(hi);
                                }
                        }
                        // LOD levels contain only completed buckets. Keep the
                        // live edge explicit so FOLLOW never waits for the next
                        // 4^N bucket to close, and FULL always retains both ends.
                        if (viewMode_ == MonitorViewMode::FULL &&
                            (dest.empty() || dest.front().plotTick > source.front().plotTick))
                                dest.insert(dest.begin(), source.front());
                        if (viewMode_ != MonitorViewMode::MANUAL &&
                            (dest.empty() || dest.back().plotTick < source.back().plotTick))
                                dest.push_back(source.back());
                }
        }

        const ImVec2    paneAvail       = ImGui::GetContentRegionAvail();
        constexpr float splitterHeight  = 7.0f;
        constexpr float minPlotHeight   = 100.0f;
        constexpr float minConfigHeight = 90.0f;
        const bool      layoutHasRoom   = paneAvail.y >= minPlotHeight + splitterHeight + minConfigHeight;
        const float     maxPlotHeight   = std::max(minPlotHeight, paneAvail.y - splitterHeight - minConfigHeight);
        // Do not overwrite the persisted height during the first dock-layout
        // frame, when ImGui may temporarily report a tiny content region.
        if (layoutHasRoom) {
                const float minRatio = minPlotHeight / paneAvail.y;
                const float maxRatio = maxPlotHeight / paneAvail.y;
                plotPaneRatio_       = std::clamp(plotPaneRatio_, minRatio, maxRatio);
                plotPaneHeight_      = std::clamp(paneAvail.y * plotPaneRatio_, minPlotHeight, maxPlotHeight);
        }
        const float plotDrawHeight = layoutHasRoom ? plotPaneHeight_ : std::max(1.0f, paneAvail.y - splitterHeight - 1.0f);

        ImGui::BeginChild("##bkp_plot_pane", ImVec2(0.0f, plotDrawHeight), true, ImGuiWindowFlags_NoScrollbar);
        std::vector<std::pair<std::size_t, ImPlotItem *>> legendSync;
        if (ImPlot::BeginPlot("##bkp_plot", ImVec2(-1.0f, -1.0f))) {
                ImPlot::SetupAxes(nullptr,
                                  nullptr,
                                  ImPlotAxisFlags_None,
                                  viewMode_ == MonitorViewMode::FULL ? ImPlotAxisFlags_AutoFit : ImPlotAxisFlags_None);
                double latestTick   = 0.0;
                double earliestTick = std::numeric_limits<double>::infinity();
                for (const auto &channel : channels_) {
                        const auto &points = plotCache_[channel.slot];
                        if (!points.empty()) {
                                earliestTick = std::min(earliestTick, points.front().plotTick);
                                latestTick   = std::max(latestTick, points.back().plotTick);
                        }
                }
                if (viewMode_ == MonitorViewMode::FULL && std::isfinite(earliestTick) && latestTick >= earliestTick) {
                        ImPlot::SetupAxisLimits(
                            ImAxis_X1, earliestTick, std::max(earliestTick + 1.0, latestTick), ImGuiCond_Always);
                } else if (viewMode_ == MonitorViewMode::FOLLOW && latestTick > 0.0 &&
                           !ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::GetIO().MouseWheel == 0.0f) {
                        ImPlot::SetupAxisLimits(
                            ImAxis_X1, latestTick - std::max(1.0, plotFollowSpan_), latestTick, ImGuiCond_Always);
                }
                for (std::size_t i = 0; i < channels_.size(); ++i) {
                        auto        &channel = channels_[i];
                        auto        &points  = plotCache_[channel.slot];
                        const ImVec4 autoColor =
                            ImPlot::GetColormapColor(static_cast<int>(channel.slot % ImPlot::GetColormapSize()));
                        const ImVec4 color =
                            channel.useAutoColor
                                ? autoColor
                                : ImVec4(channel.color[0], channel.color[1], channel.color[2], channel.color[3]);
                        ImPlot::SetNextLineStyle(color, channel.lineWeight);
                        if (channel.showMarkers)
                                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f, color, IMPLOT_AUTO, color);
                        const std::string label    = channel.name + "###bkp" + std::to_string(channel.slot);
                        ImPlotItem       *plotItem = nullptr;
                        if (auto *context = ImPlot::GetCurrentContext(); context && context->CurrentPlot) {
                                plotItem = context->CurrentPlot->Items.GetItem(
                                    ImPlot::GetCurrentPlot()->Items.GetItemID(label.c_str()));
                                if (plotItem) {
                                        plotItem->Show = channel.show;
                                        legendSync.emplace_back(i, plotItem);
                                }
                        }
                        ImPlot::HideNextItem(!channel.show, ImPlotCond_Once);
                        if (points.empty()) {
                                ImPlot::PlotLine(label.c_str(),
                                                 static_cast<const double *>(nullptr),
                                                 static_cast<const double *>(nullptr),
                                                 0);
                        } else {
                                ImPlot::PlotLine(label.c_str(),
                                                 &points.front().plotTick,
                                                 &points.front().value,
                                                 static_cast<int>(points.size()),
                                                 0,
                                                 0,
                                                 static_cast<int>(sizeof(Point)));
                        }

                        if (ImPlot::BeginLegendPopup(label.c_str())) {
                                char alias[128];
                                std::snprintf(alias, sizeof(alias), "%s", channel.name.c_str());
                                ImGui::SetNextItemWidth(190.0f);
                                if (ImGui::InputText(tr("Name", "名称"), alias, sizeof(alias))) {
                                        channel.name = alias;
                                        modified_.store(true, std::memory_order_release);
                                }
                                float editedColor[4]{channel.color[0], channel.color[1], channel.color[2], channel.color[3]};
                                if (ImGui::ColorEdit4(tr("Color", "颜色"), editedColor, ImGuiColorEditFlags_NoInputs)) {
                                        std::memcpy(channel.color, editedColor, sizeof(channel.color));
                                        channel.useAutoColor = false;
                                        modified_.store(true, std::memory_order_release);
                                }
                                ImGui::SameLine();
                                if (ImGui::Checkbox(tr("Auto", "自动"), &channel.useAutoColor)) {
                                        if (!channel.useAutoColor)
                                                std::memcpy(channel.color, &autoColor.x, sizeof(channel.color));
                                        modified_.store(true, std::memory_order_release);
                                }
                                ImGui::SetNextItemWidth(130.0f);
                                if (ImGui::SliderFloat(tr("Line width", "线宽"), &channel.lineWeight, 0.5f, 5.0f, "%.1f"))
                                        modified_.store(true, std::memory_order_release);
                                if (ImGui::Checkbox(tr("Markers", "标记点"), &channel.showMarkers))
                                        modified_.store(true, std::memory_order_release);
                                if (ImGui::Checkbox(tr("Visible", "显示"), &channel.show)) {
                                        if (plotItem)
                                                plotItem->Show = channel.show;
                                        modified_.store(true, std::memory_order_release);
                                }
                                ImPlot::EndLegendPopup();
                        }
                }
                const bool hovered =
                    ImPlot::IsPlotHovered() || ImPlot::IsAxisHovered(ImAxis_X1) || ImPlot::IsAxisHovered(ImAxis_Y1);
                const bool doubleClick = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                if (doubleClick) {
                        if (viewMode_ == MonitorViewMode::MANUAL)
                                viewMode_ = MonitorViewMode::FOLLOW;
                        else if (viewMode_ == MonitorViewMode::FOLLOW)
                                viewMode_ = MonitorViewMode::FULL;
                        modified_.store(true, std::memory_order_release);
                } else if (hovered && ImGui::GetIO().MouseWheel != 0.0f && viewMode_ == MonitorViewMode::FULL) {
                        viewMode_ = MonitorViewMode::FOLLOW;
                        modified_.store(true, std::memory_order_release);
                } else if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) &&
                           viewMode_ != MonitorViewMode::MANUAL) {
                        viewMode_ = MonitorViewMode::MANUAL;
                        modified_.store(true, std::memory_order_release);
                }
                const ImPlotRect limits = ImPlot::GetPlotLimits();
                plotViewMin_            = limits.X.Min;
                plotViewMax_            = limits.X.Max;
                if (viewMode_ == MonitorViewMode::FOLLOW)
                        plotFollowSpan_ = std::max(1.0, plotViewMax_ - plotViewMin_);
                ImPlot::EndPlot();
                for (const auto &[index, item] : legendSync) {
                        if (index < channels_.size() && channels_[index].show != item->Show) {
                                channels_[index].show = item->Show;
                                modified_.store(true, std::memory_order_release);
                        }
                }
        }
        ImGui::EndChild();

        ImGui::InvisibleButton("##bkp_splitter", ImVec2(std::max(1.0f, paneAvail.x), splitterHeight));
        const bool splitterHovered = ImGui::IsItemHovered();
        const bool splitterActive  = ImGui::IsItemActive();
        if (splitterHovered || splitterActive)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (splitterActive && layoutHasRoom) {
                plotPaneHeight_ = std::clamp(plotPaneHeight_ + ImGui::GetIO().MouseDelta.y, minPlotHeight, maxPlotHeight);
                plotPaneRatio_  = plotPaneHeight_ / paneAvail.y;
                modified_.store(true, std::memory_order_release);
        }
        if (splitterHovered && layoutHasRoom && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                plotPaneRatio_  = 0.65f;
                plotPaneHeight_ = std::clamp(paneAvail.y * plotPaneRatio_, minPlotHeight, maxPlotHeight);
                modified_.store(true, std::memory_order_release);
        }
        const ImVec2 splitterMin   = ImGui::GetItemRectMin();
        const ImVec2 splitterMax   = ImGui::GetItemRectMax();
        const ImU32  splitterColor = ImGui::GetColorU32(splitterActive    ? ImGuiCol_SeparatorActive
                                                       : splitterHovered ? ImGuiCol_SeparatorHovered
                                                                         : ImGuiCol_Separator);
        const float  splitterY     = (splitterMin.y + splitterMax.y) * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(splitterMin.x, splitterY), ImVec2(splitterMax.x, splitterY), splitterColor, splitterActive ? 2.0f : 1.0f);

        ImGui::BeginChild("##bkp_config_pane", ImVec2(0.0f, 0.0f), true);

        ImGui::SetNextItemOpen(configExpanded_, ImGuiCond_Always);
        const bool configExpanded = ImGui::CollapsingHeader(tr("Channel configuration", "通道配置"));
        if (configExpanded != configExpanded_) {
                configExpanded_ = configExpanded;
                modified_.store(true, std::memory_order_release);
        }
        if (configExpanded_) {
                int reorderSource = -1;
                int reorderTarget = -1;
                int deleteIndex   = -1;
                if (ImGui::BeginTable(
                        "##bkp_cfg", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                        ImGui::TableSetupColumn("=", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 24.0f);
                        ImGui::TableSetupColumn("CH", ImGuiTableColumnFlags_WidthFixed, 58.0f);
                        ImGui::TableSetupColumn(tr("Name", "名称"));
                        ImGui::TableSetupColumn(tr("Variable address", "变量地址"));
                        ImGui::TableSetupColumn(tr("Type", "类型"));
                        ImGui::TableSetupColumn(tr("Latest", "最新值"));
                        ImGui::TableSetupColumn(tr("Runtime", "采集状态"));
                        ImGui::TableHeadersRow();
                        for (std::size_t i = 0; i < channels_.size(); ++i) {
                                auto &channel = channels_[i];
                                ImGui::PushID(static_cast<int>(i));
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::Selectable(
                                    "=", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
                                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                                        const BkpChannelReorderPayload payload{this, static_cast<std::uint32_t>(i)};
                                        ImGui::SetDragDropPayload("BKP_CHANNEL_REORDER", &payload, sizeof(payload));
                                        ImGui::Text("%s", channel.name.c_str());
                                        ImGui::EndDragDropSource();
                                }
                                if (ImGui::BeginDragDropTarget()) {
                                        if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("BKP_CHANNEL_REORDER")) {
                                                if (payload->DataSize == sizeof(BkpChannelReorderPayload)) {
                                                        const auto &move =
                                                            *static_cast<const BkpChannelReorderPayload *>(payload->Data);
                                                        if (move.owner == this) {
                                                                reorderSource = static_cast<int>(move.index);
                                                                reorderTarget = static_cast<int>(i);
                                                        }
                                                }
                                        }
                                        ImGui::EndDragDropTarget();
                                }
                                if (ImGui::BeginPopupContextItem("##bkp_channel_context")) {
                                        if (ImGui::MenuItem(tr("Delete channel", "删除通道")))
                                                deleteIndex = static_cast<int>(i);
                                        ImGui::EndPopup();
                                }

                                ImGui::TableSetColumnIndex(1);
                                const std::string slotLabel = "ch" + std::to_string(channel.slot);
                                ImGui::SetNextItemWidth(-1.0f);
                                if (ImGui::BeginCombo("##slot", slotLabel.c_str())) {
                                        for (std::uint32_t candidate = 0; candidate < kMaxChannels; ++candidate) {
                                                const bool used = std::any_of(
                                                    channels_.begin(), channels_.end(), [&](const ChannelConfig &other) {
                                                            return &other != &channel && other.slot == candidate;
                                                    });
                                                const std::string candidateLabel = "ch" + std::to_string(candidate);
                                                if (ImGui::Selectable(candidateLabel.c_str(),
                                                                      candidate == channel.slot,
                                                                      used ? ImGuiSelectableFlags_Disabled : 0)) {
                                                        channel.slot = candidate;
                                                        modified_.store(true, std::memory_order_release);
                                                }
                                        }
                                        ImGui::EndCombo();
                                }
                                ImGui::TableSetColumnIndex(2);
                                char name[128];
                                std::snprintf(name, sizeof(name), "%s", channel.name.c_str());
                                ImGui::SetNextItemWidth(-1.0f);
                                if (ImGui::InputText("##name", name, sizeof(name))) {
                                        channel.name = name;
                                        modified_.store(true, std::memory_order_release);
                                }
                                ImGui::TableSetColumnIndex(3);
                                ImGui::SetNextItemWidth(-1.0f);
                                const bool addressCommitted = ImGui::InputText("##address",
                                                                               channel.addressText,
                                                                               sizeof(channel.addressText),
                                                                               ImGuiInputTextFlags_EnterReturnsTrue);
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s", tr("Enter a hexadecimal address.", "输入十六进制地址。"));
                                const bool addressEdited      = ImGui::IsItemEdited();
                                const bool addressDeactivated = ImGui::IsItemDeactivatedAfterEdit();
                                if (addressEdited) {
                                        modified_.store(true, std::memory_order_release);
                                        char               *editEnd    = nullptr;
                                        const unsigned long editParsed = std::strtoul(channel.addressText, &editEnd, 16);
                                        if (editEnd != channel.addressText && *editEnd == '\0' && editParsed <= 0xfffffffful)
                                                channel.address = static_cast<std::uint32_t>(editParsed);
                                }
                                if (addressCommitted || addressDeactivated) {
                                        char               *end    = nullptr;
                                        const unsigned long parsed = std::strtoul(channel.addressText, &end, 16);
                                        if (end != channel.addressText && *end == '\0' && parsed <= 0xfffffffful) {
                                                channel.address = static_cast<std::uint32_t>(parsed);
                                                std::snprintf(channel.addressText,
                                                              sizeof(channel.addressText),
                                                              "0x%08X",
                                                              channel.address);
                                        } else {
                                                std::snprintf(channel.addressText,
                                                              sizeof(channel.addressText),
                                                              "0x%08X",
                                                              channel.address);
                                                setStatus(tr("Invalid variable address; use e.g. 0x2000569C.",
                                                             "变量地址格式无效，请使用例如 0x2000569C。"));
                                        }
                                }
                                ImGui::TableSetColumnIndex(4);
                                int type = static_cast<int>(channel.type);
                                ImGui::SetNextItemWidth(-1.0f);
                                if (ImGui::Combo("##type", &type, "U8\0I8\0U16\0I16\0U32\0I32\0F32\0")) {
                                        channel.type = static_cast<ValueType>(type);
                                        modified_.store(true, std::memory_order_release);
                                }
                                ImGui::TableSetColumnIndex(5);
                                {
                                        std::lock_guard lock(dataMtx_);
                                        const auto     &points = runtime_[channel.slot].points;
                                        if (!points.empty())
                                                ImGui::Text("%.9g", points.back().value);
                                        else
                                                ImGui::TextDisabled("--");
                                }
                                ImGui::TableSetColumnIndex(6);
                                {
                                        std::lock_guard lock(dataMtx_);
                                        const auto     &rt = runtime_[channel.slot];
                                        if (rt.active) {
                                                ImGui::Text(tr("Sampling | Depth %u | Received %llu | Lost %llu",
                                                               "采集中 | 深度 %u | 已收 %llu | 丢点 %llu"),
                                                            rt.capacity,
                                                            static_cast<unsigned long long>(rt.validatedSamples),
                                                            static_cast<unsigned long long>(rt.droppedSamples));
                                        } else {
                                                ImGui::TextDisabled("%s", tr("Not sampling", "未采集"));
                                        }
                                        if (ImGui::IsItemHovered())
                                                ImGui::SetTooltip(tr("generation=%u  write_seq=%u  cursor=%u\n"
                                                                     "firmware overwrites=%u  cached points=%zu\n"
                                                                     "last slot seq: expected=%u observed=%u",
                                                                     "配置版本=%u  写入序号=%u  读取位置=%u\n"
                                                                     "固件覆盖=%u  当前缓存点=%zu\n"
                                                                     "最近槽位序号：期望=%u 实际=%u"),
                                                                  rt.generation,
                                                                  rt.writeSeq,
                                                                  rt.cursor,
                                                                  rt.overwrites,
                                                                  rt.points.size(),
                                                                  rt.expectedSeq,
                                                                  rt.observedSeq);
                                }
                                ImGui::PopID();
                        }
                        ImGui::EndTable();
                }
                if (deleteIndex >= 0 && deleteIndex < static_cast<int>(channels_.size())) {
                        channels_.erase(channels_.begin() + deleteIndex);
                        modified_.store(true, std::memory_order_release);
                } else if (reorderSource >= 0 && reorderTarget >= 0 && reorderSource != reorderTarget &&
                           reorderSource < static_cast<int>(channels_.size()) &&
                           reorderTarget < static_cast<int>(channels_.size())) {
                        ChannelConfig moved = std::move(channels_[reorderSource]);
                        channels_.erase(channels_.begin() + reorderSource);
                        channels_.insert(channels_.begin() + reorderTarget, std::move(moved));
                        modified_.store(true, std::memory_order_release);
                }
                if (channels_.size() < kMaxChannels && ImGui::Button(tr("Add channel", "添加通道"))) {
                        std::uint32_t slot = 0u;
                        while (slot < kMaxChannels && std::any_of(
                                                          channels_.begin(),
                                                          channels_.end(),
                                                          [&](const ChannelConfig &channel) { return channel.slot == slot; }))
                                ++slot;
                        ChannelConfig channel;
                        channel.name = "Channel " + std::to_string(slot + 1u);
                        channel.slot = slot;
                        channels_.push_back(std::move(channel));
                        modified_.store(true, std::memory_order_release);
                }
                acceptNewChannelDrop();
        }

        ImGui::EndChild();

        // The border is an attention signal, so normal sampling stays visually
        // quiet. A globally paused acquisition is yellow; transition/error
        // states remain distinguishable.
        ImVec4 borderColor{};
        bool   drawStateBorder = false;
        if (active && paused) {
                borderColor     = ImVec4(1.0f, 0.78f, 0.18f, 1.0f);
                drawStateBorder = true;
        } else if (state == State::Stopping) {
                borderColor     = ImVec4(1.0f, 0.55f, 0.12f, 1.0f);
                drawStateBorder = true;
        } else if (state == State::Error) {
                borderColor     = ImVec4(1.0f, 0.22f, 0.22f, 1.0f);
                drawStateBorder = true;
        }
        if (drawStateBorder) {
                ImGuiWindow *window = ImGui::GetCurrentWindow();
                ImRect       rect   = window->DockNode
                                          ? window->DockNode->Rect()
                                          : ImRect(window->Pos, ImVec2(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y));
                rect.Expand(-1.5f);
                ImGui::GetForegroundDrawList(window->Viewport)
                    ->AddRect(rect.Min, rect.Max, ImGui::GetColorU32(borderColor), 1.5f, 0, 3.0f);
        }

        ImGui::End();
}

void
BkpSramDebugger::saveSession(void *node) const
{
        cJSON *root = static_cast<cJSON *>(node);
        if (!cJSON_IsObject(root))
                return;
        cJSON *object = cJSON_CreateObject();
        cJSON_AddBoolToObject(object, "windowOpen", windowOpen_);
        cJSON_AddNumberToObject(object, "sampleDiv", sampleDiv_);
        cJSON_AddNumberToObject(object, "pollIntervalMs", pollIntervalMs_.load(std::memory_order_relaxed));
        cJSON_AddNumberToObject(object, "historySeconds", historySeconds_.load(std::memory_order_relaxed));
        cJSON_AddNumberToObject(object, "maxDisplayPoints", maxDisplayPoints_);
        cJSON_AddNumberToObject(object, "viewMode", static_cast<int>(viewMode_));
        cJSON_AddNumberToObject(object, "plotFollowSpan", plotFollowSpan_);
        cJSON_AddNumberToObject(object, "plotPaneHeight", plotPaneHeight_);
        cJSON_AddNumberToObject(object, "plotPaneRatio", plotPaneRatio_);
        cJSON_AddNumberToObject(object, "configPaneRatio", 1.0f - plotPaneRatio_);
        cJSON_AddBoolToObject(object, "configExpanded", configExpanded_);
        cJSON *channels = cJSON_CreateArray();
        for (const auto &channel : channels_) {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "name", channel.name.c_str());
                cJSON_AddNumberToObject(item, "slot", channel.slot);
                cJSON_AddStringToObject(item, "address", hexAddress(channel.address).c_str());
                cJSON_AddNumberToObject(item, "type", static_cast<std::uint32_t>(channel.type));
                cJSON_AddBoolToObject(item, "autoColor", channel.useAutoColor);
                cJSON_AddNumberToObject(item, "lineWeight", channel.lineWeight);
                cJSON_AddBoolToObject(item, "markers", channel.showMarkers);
                cJSON_AddBoolToObject(item, "show", channel.show);
                cJSON *color = cJSON_CreateArray();
                for (const float component : channel.color)
                        cJSON_AddItemToArray(color, cJSON_CreateNumber(component));
                cJSON_AddItemToObject(item, "color", color);
                cJSON_AddItemToArray(channels, item);
        }
        cJSON_AddItemToObject(object, "channels", channels);
        cJSON_AddItemToObject(root, "bkpSramCapture", object);
}

void
BkpSramDebugger::loadSession(const void *node)
{
        resetSession();
        const cJSON *root   = static_cast<const cJSON *>(node);
        const cJSON *object = cJSON_GetObjectItem(root, "bkpSramCapture");
        if (!cJSON_IsObject(object)) {
                clearModified();
                return;
        }
        if (const cJSON *v = cJSON_GetObjectItem(object, "windowOpen"); cJSON_IsBool(v))
                windowOpen_ = cJSON_IsTrue(v);
        if (const cJSON *v = cJSON_GetObjectItem(object, "sampleDiv"); cJSON_IsNumber(v))
                sampleDiv_ = static_cast<std::uint32_t>(std::clamp(v->valueint, 1, 32));
        if (const cJSON *v = cJSON_GetObjectItem(object, "pollIntervalMs"); cJSON_IsNumber(v))
                pollIntervalMs_.store(std::clamp(v->valueint, 1, 100), std::memory_order_relaxed);
        if (const cJSON *v = cJSON_GetObjectItem(object, "historySeconds"); cJSON_IsNumber(v))
                historySeconds_.store(std::clamp(static_cast<float>(v->valuedouble), 0.0f, 3600.0f), std::memory_order_relaxed);
        if (const cJSON *v = cJSON_GetObjectItem(object, "maxDisplayPoints"); cJSON_IsNumber(v))
                maxDisplayPoints_ = static_cast<std::uint32_t>(std::clamp(v->valueint, 100, 100000));
        if (const cJSON *v = cJSON_GetObjectItem(object, "viewMode"); cJSON_IsNumber(v))
                viewMode_ = static_cast<MonitorViewMode>(std::clamp(v->valueint, 0, 2));
        else if (const cJSON *v = cJSON_GetObjectItem(object, "plotFollow"); cJSON_IsBool(v))
                viewMode_ = cJSON_IsTrue(v) ? MonitorViewMode::FOLLOW : MonitorViewMode::MANUAL;
        if (const cJSON *v = cJSON_GetObjectItem(object, "plotFollowSpan"); cJSON_IsNumber(v))
                plotFollowSpan_ = std::max(1.0, v->valuedouble);
        if (const cJSON *v = cJSON_GetObjectItem(object, "plotPaneHeight"); cJSON_IsNumber(v))
                plotPaneHeight_ = std::max(100.0f, static_cast<float>(v->valuedouble));
        if (const cJSON *v = cJSON_GetObjectItem(object, "plotPaneRatio"); cJSON_IsNumber(v))
                plotPaneRatio_ = std::clamp(static_cast<float>(v->valuedouble), 0.05f, 0.95f);
        else if (const cJSON *v = cJSON_GetObjectItem(object, "configPaneRatio"); cJSON_IsNumber(v))
                plotPaneRatio_ = std::clamp(1.0f - static_cast<float>(v->valuedouble), 0.05f, 0.95f);
        if (const cJSON *v = cJSON_GetObjectItem(object, "configExpanded"); cJSON_IsBool(v))
                configExpanded_ = cJSON_IsTrue(v);
        if (const cJSON *array = cJSON_GetObjectItem(object, "channels"); cJSON_IsArray(array)) {
                channels_.clear();
                const cJSON *item = nullptr;
                cJSON_ArrayForEach(item, array)
                {
                        if (channels_.size() >= kMaxChannels || !cJSON_IsObject(item))
                                break;
                        ChannelConfig channel;
                        channel.name = "Channel " + std::to_string(channels_.size() + 1u);
                        channel.slot = static_cast<std::uint32_t>(channels_.size());
                        if (const cJSON *v = cJSON_GetObjectItem(item, "name"); cJSON_IsString(v))
                                channel.name = v->valuestring;
                        if (const cJSON *v = cJSON_GetObjectItem(item, "slot"); cJSON_IsNumber(v))
                                channel.slot = static_cast<std::uint32_t>(std::clamp(v->valueint, 0, 15));
                        if (const cJSON *v = cJSON_GetObjectItem(item, "address"); cJSON_IsString(v))
                                channel.address = static_cast<std::uint32_t>(std::strtoul(v->valuestring, nullptr, 0));
                        std::snprintf(channel.addressText, sizeof(channel.addressText), "0x%08X", channel.address);
                        if (const cJSON *v = cJSON_GetObjectItem(item, "type"); cJSON_IsNumber(v))
                                channel.type = static_cast<ValueType>(std::clamp(v->valueint, 0, 6));
                        if (const cJSON *v = cJSON_GetObjectItem(item, "autoColor"); cJSON_IsBool(v))
                                channel.useAutoColor = cJSON_IsTrue(v);
                        if (const cJSON *v = cJSON_GetObjectItem(item, "lineWeight"); cJSON_IsNumber(v))
                                channel.lineWeight = std::clamp(static_cast<float>(v->valuedouble), 0.5f, 5.0f);
                        if (const cJSON *v = cJSON_GetObjectItem(item, "markers"); cJSON_IsBool(v))
                                channel.showMarkers = cJSON_IsTrue(v);
                        if (const cJSON *v = cJSON_GetObjectItem(item, "show"); cJSON_IsBool(v))
                                channel.show = cJSON_IsTrue(v);
                        if (const cJSON *color = cJSON_GetObjectItem(item, "color"); cJSON_IsArray(color)) {
                                for (int component = 0; component < 4; ++component) {
                                        if (const cJSON *v = cJSON_GetArrayItem(color, component); cJSON_IsNumber(v))
                                                channel.color[component] = static_cast<float>(v->valuedouble);
                                }
                        }
                        channels_.push_back(std::move(channel));
                }
        }
        clearModified();
}
