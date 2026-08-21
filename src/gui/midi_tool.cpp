#include "gui/midi_tool.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <string_view>
#include <tuple>
#include <unordered_set>

#include "cJSON.h"
#include "core/jlink_port.hpp"
#include "core/type_codec.hpp"
#include "gui/i18n.hpp"
#include "gui/monitor.hpp"
#include "gui/monitor_types.hpp"
#include "imgui.h"
#include "platform/native_dlg.hpp"

namespace
{
constexpr std::size_t kMaxMidiBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxNotes     = 2000000u;

std::uint16_t
be16(const std::uint8_t *p)
{
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

std::uint32_t
be32(const std::uint8_t *p)
{
        return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

bool
readVlq(const std::vector<std::uint8_t> &data, std::size_t &pos, std::size_t end, std::uint32_t &value)
{
        value = 0;
        for (int i = 0; i < 4; ++i) {
                if (pos >= end)
                        return false;
                const std::uint8_t b = data[pos++];
                value                = (value << 7) | (b & 0x7fu);
                if ((b & 0x80u) == 0)
                        return true;
        }
        return false;
}

std::string
noteName(std::uint8_t note)
{
        static constexpr const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        return std::string(names[note % 12]) + std::to_string(static_cast<int>(note) / 12 - 1);
}

std::string
csvQuoted(const std::string &text)
{
        std::string out{'\"'};
        for (char c : text) {
                if (c == '\"')
                        out += '\"';
                out += c;
        }
        out += '\"';
        return out;
}

std::string
cFloatLiteral(double value)
{
        if (std::abs(value) < 0.0000005)
                value = 0.0;
        char text[64];
        std::snprintf(text, sizeof(text), "%.6f", value);
        std::string result(text);
        while (result.size() > 2 && result.back() == '0' && result[result.size() - 2] != '.')
                result.pop_back();
        result += 'F';
        return result;
}
} // namespace

MidiTool::~MidiTool()
{
        stopPlayback();
}

bool
MidiTool::writeTarget(const Target &target, float value)
{
        std::uint8_t bytes[8]{};
        encodeFromF32(value, target.type, bytes);
        if (target.bitSize != 0)
                return JLinkPort::instance().writeMemBitfield(
                    target.address, target.numBytes, bytes, target.bitOffset, target.bitSize);
        return JLinkPort::instance().writeMem(target.address, target.numBytes, bytes);
}

bool
MidiTool::validateTarget(Target &target, std::string &error)
{
        static constexpr const char *types[] = {"F32", "F64", "I8", "I16", "I32", "I64", "U8", "U16", "U32", "U64"};
        if (!target.symbol.empty() && symbolResolver_) {
                std::uint32_t resolved = 0;
                if (symbolResolver_(target.symbol, resolved) && resolved != 0)
                        target.address = resolved;
        }
        const bool supported =
            std::any_of(std::begin(types), std::end(types), [&](const char *type) { return target.type == type; });
        if (!supported) {
                error = tr("Only scalar numeric variables can be used.", "只能使用标量数值变量。");
                return false;
        }
        if (!target.writable) {
                error = tr("The selected variable is read-only.", "所选变量为只读变量。");
                return false;
        }
        if (target.address == 0) {
                error = tr("The selected variable has no valid J-Link address.", "所选变量没有有效的 J-Link 地址。");
                return false;
        }
        target.numBytes = target.numBytes == 0 ? typeBytes(target.type) : target.numBytes;
        if (target.numBytes == 0 || target.numBytes > 8 ||
            (target.bitSize != 0 &&
             (target.bitOffset >= target.numBytes * 8 || target.bitSize > target.numBytes * 8 - target.bitOffset))) {
                error = tr("Invalid variable width or bit-field metadata.", "变量宽度或位域信息无效。");
                return false;
        }
        return true;
}

bool
MidiTool::acceptTargetDrop(Target &target)
{
        bool accepted = false;
        if (!ImGui::BeginDragDropTarget())
                return false;
        if (const ImGuiPayload *drop = ImGui::AcceptDragDropPayload("CHANNEL")) {
                if (drop->DataSize == sizeof(ChannelDropPayload)) {
                        const auto &source = *static_cast<const ChannelDropPayload *>(drop->Data);
                        if (std::strcmp(source.device, "JLINK") == 0) {
                                stopPlayback();
                                target.name      = source.name;
                                target.symbol    = source.name;
                                target.type      = source.type;
                                target.address   = static_cast<std::uint32_t>(source.addr);
                                target.numBytes  = source.numBytes != 0 ? source.numBytes : typeBytes(target.type);
                                target.bitOffset = source.bitOffset;
                                target.bitSize   = source.bitSize;
                                target.writable  = source.writable;
                                accepted         = true;
                        } else {
                                status_ =
                                    tr("HFI playback targets must be J-Link variables.", "HFI 播放目标必须是 J-Link 变量。");
                                statusError_ = true;
                        }
                }
        }
        if (const ImGuiPayload *drop = ImGui::AcceptDragDropPayload("DND_CHANNEL_MOVE")) {
                if (drop->DataSize == sizeof(ChannelMovePayload)) {
                        const auto     &source = *static_cast<const ChannelMovePayload *>(drop->Data);
                        MonitorChannel *channel =
                            !source.isGroup && source.srcScope ? source.srcScope->findChannel(source.chName) : nullptr;
                        if (channel && channel->getDevice() == "JLINK") {
                                stopPlayback();
                                target.name = channel->getName();
                                target.symbol =
                                    channel->getSymbolName().empty() ? channel->getName() : channel->getSymbolName();
                                target.type     = channel->getType();
                                target.address  = static_cast<std::uint32_t>(channel->getAddr());
                                target.numBytes = channel->getNumBytes() != 0 ? channel->getNumBytes() : typeBytes(target.type);
                                target.bitOffset = channel->getBitOffset();
                                target.bitSize   = channel->getBitSize();
                                target.writable  = channel->isWritable();
                                accepted         = true;
                        } else {
                                status_ = tr("Drop one writable J-Link scalar channel.", "请拖入一个可写的 J-Link 标量通道。");
                                statusError_ = true;
                        }
                }
        }
        ImGui::EndDragDropTarget();
        if (accepted) {
                std::string error;
                if (!validateTarget(target, error)) {
                        status_      = error;
                        statusError_ = true;
                        target       = Target{};
                        return false;
                }
                status_      = tr("HFI target variable bound.", "HFI 目标变量已绑定。");
                statusError_ = false;
                modified_    = true;
        }
        return accepted;
}

void
MidiTool::drawTarget(const char *label, const char *id, Target &target)
{
        ImGui::TextUnformatted(label);
        ImGui::SameLine(145.0f);
        std::string button = target.address == 0 ? std::string(tr("Drop a variable here", "拖入变量到这里"))
                                                 : target.name + "  [" + target.type + "]  0x";
        if (target.address != 0) {
                char address[16];
                std::snprintf(address, sizeof(address), "%08X", target.address);
                button += address;
        }
        button += "###";
        button += id;
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Button(button.c_str(), ImVec2(-1.0f, 0.0f));
        acceptTargetDrop(target);
        if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem(tr("Clear binding", "清除绑定"))) {
                        stopPlayback();
                        target    = Target{};
                        modified_ = true;
                }
                ImGui::EndPopup();
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                                  tr("Drag from Variable Manager or Monitor; right-click to clear",
                                     "从变量管理器或变量监视器拖入；右键清除"));
}

void
MidiTool::stopPlayback()
{
        stopRequested_.store(true, std::memory_order_release);
        playbackCv_.notify_all();
        if (playbackThread_.joinable())
                playbackThread_.join();
        playbackState_.store(0, std::memory_order_release);
        playbackMidiNote_.store(-1, std::memory_order_release);
        playbackRowIndex_.store(-1, std::memory_order_release);
        lastAutoScrolledRow_ = -1;
        playbackFrequencyHz_.store(0.0f, std::memory_order_release);
        playbackVoltage_.store(0.0f, std::memory_order_release);
}

void
MidiTool::startPlayback()
{
        stopPlayback();
        std::string error;
        if (notes_.empty())
                error = tr("Load a MIDI file first.", "请先加载 MIDI 文件。");
        else if (!JLinkPort::instance().isConnected())
                error = tr("Connect J-Link before playback.", "播放前请先连接 J-Link。");
        else if (!validateTarget(frequencyTarget_, error) || !validateTarget(amplitudeTarget_, error)) {
        } else if (frequencyTarget_.address == amplitudeTarget_.address)
                error =
                    tr("Frequency and voltage amplitude must use different variables.", "注入频率和电压幅值必须绑定不同变量。");
        if (!error.empty()) {
                status_      = error;
                statusError_ = true;
                return;
        }

        std::vector<Note> playbackNotes = makePlaybackNotes();
        if (playbackNotes.empty()) {
                status_      = tr("No notes match the selected track/channel filter.", "没有音符符合当前轨道/通道筛选条件。");
                statusError_ = true;
                return;
        }

        stopRequested_.store(false, std::memory_order_release);
        playbackWriteFailed_.store(false, std::memory_order_release);
        playbackTimeSec_.store(0.0, std::memory_order_release);
        playbackRowIndex_.store(-1, std::memory_order_release);
        lastAutoScrolledRow_ = -1;
        playbackState_.store(1, std::memory_order_release);
        status_         = tr("HFI MIDI playback started.", "HFI MIDI 播放已开始。");
        statusError_    = false;
        playbackThread_ = std::thread(&MidiTool::playbackMain,
                                      this,
                                      std::move(playbackNotes),
                                      frequencyTarget_,
                                      amplitudeTarget_,
                                      minVoltage_,
                                      maxVoltage_,
                                      polyphonyMode_);
}

std::vector<MidiTool::Note>
MidiTool::makePlaybackNotes() const
{
        std::vector<Note> playbackNotes;
        playbackNotes.reserve(notes_.size());
        const double pitch = std::pow(2.0, transpose_ / 12.0);
        const double speed = std::max(0.01, speedScale_);
        for (std::size_t sourceRow = 0; sourceRow < notes_.size(); ++sourceRow) {
                Note note = notes_[sourceRow];
                if (trackFilter_ >= 0 && note.track != static_cast<std::uint32_t>(trackFilter_))
                        continue;
                if (channelFilter_ >= 0 && note.channel != static_cast<std::uint8_t>(channelFilter_))
                        continue;
                note.startSec    /= speed;
                note.durationSec /= speed;
                note.frequencyHz *= pitch;
                note.sourceRow    = static_cast<int>(sourceRow);
                playbackNotes.push_back(std::move(note));
        }
        return playbackNotes;
}

void
MidiTool::playbackMain(
    std::vector<Note> notes, Target frequency, Target amplitude, double minVoltage, double maxVoltage, int polyphonyMode)
{
        struct Event {
                double      time;
                std::size_t note;
                bool        on;
        };
        std::vector<Event> events;
        events.reserve(notes.size() * 2);
        for (std::size_t i = 0; i < notes.size(); ++i) {
                events.push_back({notes[i].startSec, i, true});
                events.push_back({notes[i].startSec + notes[i].durationSec, i, false});
        }
        std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
                if (a.time != b.time)
                        return a.time < b.time;
                return static_cast<int>(a.on) > static_cast<int>(b.on); // zero-duration note ends inactive
        });

        std::unordered_set<std::size_t> active;
        auto                            origin     = std::chrono::steady_clock::now();
        std::size_t                     eventIndex = 0;
        std::size_t                     selected   = std::numeric_limits<std::size_t>::max();

        if (!writeTarget(amplitude, 0.0f)) {
                playbackWriteFailed_.store(true, std::memory_order_release);
                playbackRowIndex_.store(-1, std::memory_order_release);
                playbackState_.store(0, std::memory_order_release);
                return;
        }

        while (eventIndex < events.size() && !stopRequested_.load(std::memory_order_acquire)) {
                if (playbackState_.load(std::memory_order_acquire) == 2) {
                        const auto        pausedAt        = std::chrono::steady_clock::now();
                        const std::size_t pausedSelection = selected;
                        if (!writeTarget(amplitude, 0.0f)) {
                                playbackWriteFailed_.store(true, std::memory_order_release);
                                break;
                        }
                        playbackVoltage_.store(0.0f, std::memory_order_release);
                        std::unique_lock lock(playbackMutex_);
                        playbackCv_.wait(lock, [&]() {
                                return stopRequested_.load(std::memory_order_acquire) ||
                                       playbackState_.load(std::memory_order_acquire) != 2;
                        });
                        origin += std::chrono::steady_clock::now() - pausedAt;
                        if (!stopRequested_.load(std::memory_order_acquire) &&
                            pausedSelection != std::numeric_limits<std::size_t>::max()) {
                                const Note &note     = notes[pausedSelection];
                                const float velocity = static_cast<float>(note.velocity) / 127.0f;
                                const float volts    = static_cast<float>(minVoltage + (maxVoltage - minVoltage) * velocity);
                                if (!writeTarget(frequency, static_cast<float>(note.frequencyHz)) ||
                                    !writeTarget(amplitude, volts)) {
                                        playbackWriteFailed_.store(true, std::memory_order_release);
                                        break;
                                }
                                playbackVoltage_.store(volts, std::memory_order_release);
                        }
                        continue;
                }

                const auto   now     = std::chrono::steady_clock::now();
                const double elapsed = std::chrono::duration<double>(now - origin).count();
                playbackTimeSec_.store(elapsed, std::memory_order_release);
                const double waitSec = events[eventIndex].time - elapsed;
                if (waitSec > 0.0005) {
                        std::unique_lock lock(playbackMutex_);
                        playbackCv_.wait_for(lock, std::chrono::duration<double>(std::min(waitSec, 0.01)));
                        continue;
                }

                const double eventTime = events[eventIndex].time;
                while (eventIndex < events.size() && std::abs(events[eventIndex].time - eventTime) < 1e-9) {
                        const Event &event = events[eventIndex++];
                        if (event.on)
                                active.insert(event.note);
                        else
                                active.erase(event.note);
                }

                std::size_t next = std::numeric_limits<std::size_t>::max();
                for (std::size_t candidate : active) {
                        if (next == std::numeric_limits<std::size_t>::max()) {
                                next = candidate;
                                continue;
                        }
                        if ((polyphonyMode == 0 && notes[candidate].midiNote > notes[next].midiNote) ||
                            (polyphonyMode == 1 && notes[candidate].midiNote < notes[next].midiNote) ||
                            (polyphonyMode == 2 && (notes[candidate].startSec > notes[next].startSec ||
                                                    (notes[candidate].startSec == notes[next].startSec && candidate > next))))
                                next = candidate;
                }

                if (next != selected) {
                        selected = next;
                        if (selected == std::numeric_limits<std::size_t>::max()) {
                                if (!writeTarget(amplitude, 0.0f)) {
                                        playbackWriteFailed_.store(true, std::memory_order_release);
                                        break;
                                }
                                playbackMidiNote_.store(-1, std::memory_order_release);
                                playbackRowIndex_.store(-1, std::memory_order_release);
                                playbackFrequencyHz_.store(0.0f, std::memory_order_release);
                                playbackVoltage_.store(0.0f, std::memory_order_release);
                        } else {
                                const Note &note     = notes[selected];
                                const float hz       = static_cast<float>(note.frequencyHz);
                                const float velocity = static_cast<float>(note.velocity) / 127.0f;
                                const float volts    = static_cast<float>(minVoltage + (maxVoltage - minVoltage) * velocity);
                                if (!writeTarget(frequency, hz) || !writeTarget(amplitude, volts)) {
                                        playbackWriteFailed_.store(true, std::memory_order_release);
                                        break;
                                }
                                playbackMidiNote_.store(note.midiNote, std::memory_order_release);
                                playbackRowIndex_.store(note.sourceRow, std::memory_order_release);
                                playbackFrequencyHz_.store(hz, std::memory_order_release);
                                playbackVoltage_.store(volts, std::memory_order_release);
                        }
                }
        }

        writeTarget(amplitude, 0.0f);
        if (!events.empty())
                playbackTimeSec_.store(std::min(playbackTimeSec_.load(), events.back().time), std::memory_order_release);
        playbackMidiNote_.store(-1, std::memory_order_release);
        playbackRowIndex_.store(-1, std::memory_order_release);
        playbackFrequencyHz_.store(0.0f, std::memory_order_release);
        playbackVoltage_.store(0.0f, std::memory_order_release);
        playbackState_.store(0, std::memory_order_release);
}

void
MidiTool::setOpen(bool open)
{
        if (open_ != open) {
                if (!open)
                        stopPlayback();
                open_     = open;
                modified_ = true;
        }
}

bool
MidiTool::consumeModified()
{
        const bool value = modified_;
        modified_        = false;
        return value;
}

void
MidiTool::clear()
{
        stopPlayback();
        notes_.clear();
        sourcePath_.clear();
        status_.clear();
        statusError_ = false;
        format_ = division_ = trackCount_ = 0;
        tempoCount_                       = 0;
        durationSec_                      = 0.0;
        minBpm_ = maxBpm_ = 120.0;
        modified_         = true;
}

bool
MidiTool::loadFile(const std::string &path)
{
        stopPlayback();
        FILE *file = nativeFopen(path, "rb");
        if (!file) {
                status_      = tr("Cannot open MIDI file.", "无法打开 MIDI 文件。");
                statusError_ = true;
                open_        = true;
                return false;
        }
        std::fseek(file, 0, SEEK_END);
        const long fileSize = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        if (fileSize < 14 || static_cast<std::size_t>(fileSize) > kMaxMidiBytes) {
                std::fclose(file);
                status_      = tr("Invalid MIDI size (maximum 64 MB).", "MIDI 文件大小无效（最大 64 MB）。");
                statusError_ = true;
                open_        = true;
                return false;
        }
        std::vector<std::uint8_t> data(static_cast<std::size_t>(fileSize));
        const bool                readOk = std::fread(data.data(), 1, data.size(), file) == data.size();
        std::fclose(file);
        if (!readOk || std::memcmp(data.data(), "MThd", 4) != 0 || be32(data.data() + 4) < 6) {
                status_      = tr("Invalid Standard MIDI File header.", "不是有效的标准 MIDI 文件。");
                statusError_ = true;
                open_        = true;
                return false;
        }

        struct RawNote {
                std::uint32_t track;
                std::uint8_t  channel, note, velocity, program;
                std::uint64_t startTick, endTick;
                std::string   trackName;
        };
        struct Tempo {
                std::uint64_t tick;
                std::uint32_t usPerQuarter;
        };
        struct Active {
                std::uint64_t tick;
                std::uint8_t  velocity;
                std::uint8_t  program;
        };

        const std::uint32_t headerLen = be32(data.data() + 4);
        const std::uint16_t format    = be16(data.data() + 8);
        const std::uint16_t nTracks   = be16(data.data() + 10);
        const std::uint16_t division  = be16(data.data() + 12);
        if (format > 2 || nTracks == 0 || division == 0 || 8u + headerLen > data.size()) {
                status_      = tr("Unsupported or corrupt MIDI header.", "MIDI 头不受支持或已损坏。");
                statusError_ = true;
                open_        = true;
                return false;
        }

        std::vector<RawNote> rawNotes;
        std::vector<Tempo>   tempos{{0, 500000}};
        std::size_t          pos          = 8u + headerLen;
        std::uint16_t        parsedTracks = 0;
        std::string          parseError;

        while (pos + 8 <= data.size() && parsedTracks < nTracks) {
                const std::uint32_t chunkLen = be32(data.data() + pos + 4);
                const std::size_t   body     = pos + 8;
                if (chunkLen > data.size() - body) {
                        parseError = tr("Truncated MIDI track.", "MIDI 轨道数据不完整。");
                        break;
                }
                const std::size_t end = body + chunkLen;
                if (std::memcmp(data.data() + pos, "MTrk", 4) != 0) {
                        pos = end;
                        continue;
                }

                const std::uint32_t          trackIndex = parsedTracks++;
                std::string                  trackName  = "Track " + std::to_string(trackIndex + 1);
                std::array<std::uint8_t, 16> programs{};
                // 4096 std::deque objects are far too large for the Windows GUI
                // thread stack when MIDI parsing is nested inside .ava loading.
                // Keep the parser workspace on the heap so restoring a session
                // cannot overflow the main-thread stack.
                using NoteQueues               = std::array<std::array<std::deque<Active>, 128>, 16>;
                auto                 active    = std::make_unique<NoteQueues>();
                auto                 sustained = std::make_unique<NoteQueues>();
                std::array<bool, 16> sustainDown{};
                std::uint64_t        tick          = 0;
                std::uint8_t         runningStatus = 0;
                std::size_t          p             = body;

                auto emitNote = [&](std::uint8_t channel, std::uint8_t note, const Active &start, std::uint64_t endTick) {
                        if (rawNotes.size() < kMaxNotes)
                                rawNotes.push_back({trackIndex,
                                                    channel,
                                                    note,
                                                    start.velocity,
                                                    start.program,
                                                    start.tick,
                                                    std::max(endTick, start.tick),
                                                    trackName});
                };
                auto finishNote = [&](std::uint8_t channel, std::uint8_t note, std::uint64_t endTick) {
                        auto &queue = (*active)[channel][note];
                        if (queue.empty())
                                return;
                        const Active start = queue.front();
                        queue.pop_front();
                        if (sustainDown[channel])
                                (*sustained)[channel][note].push_back(start);
                        else
                                emitNote(channel, note, start, endTick);
                };
                auto releaseSustain = [&](std::uint8_t channel, std::uint64_t endTick) {
                        for (std::uint8_t note = 0; note < 128; ++note) {
                                auto &queue = (*sustained)[channel][note];
                                while (!queue.empty()) {
                                        emitNote(channel, note, queue.front(), endTick);
                                        queue.pop_front();
                                }
                        }
                };

                while (p < end) {
                        std::uint32_t delta = 0;
                        if (!readVlq(data, p, end, delta)) {
                                parseError = tr("Invalid MIDI variable-length value.", "MIDI 变长数值无效。");
                                break;
                        }
                        tick += delta;
                        if (p >= end) {
                                parseError = tr("Truncated MIDI event.", "MIDI 事件不完整。");
                                break;
                        }
                        std::uint8_t status = data[p++];
                        if (status < 0x80) {
                                if (runningStatus == 0) {
                                        parseError = tr("Invalid MIDI running status.", "MIDI 运行状态无效。");
                                        break;
                                }
                                status = runningStatus;
                                --p;
                        } else if (status < 0xf0) {
                                runningStatus = status;
                        } else {
                                runningStatus = 0;
                        }

                        if (status == 0xff) {
                                if (p >= end) {
                                        parseError = tr("Truncated MIDI meta event.", "MIDI 元事件不完整。");
                                        break;
                                }
                                const std::uint8_t metaType = data[p++];
                                std::uint32_t      len      = 0;
                                if (!readVlq(data, p, end, len) || len > end - p) {
                                        parseError = tr("Invalid MIDI meta-event length.", "MIDI 元事件长度无效。");
                                        break;
                                }
                                if (metaType == 0x51 && len == 3) {
                                        const std::uint32_t us = (static_cast<std::uint32_t>(data[p]) << 16) |
                                                                 (static_cast<std::uint32_t>(data[p + 1]) << 8) | data[p + 2];
                                        if (us != 0)
                                                tempos.push_back({tick, us});
                                } else if (metaType == 0x03 && len != 0) {
                                        trackName.assign(reinterpret_cast<const char *>(data.data() + p), len);
                                }
                                p += len;
                                if (metaType == 0x2f)
                                        break;
                                continue;
                        }
                        if (status == 0xf0 || status == 0xf7) {
                                std::uint32_t len = 0;
                                if (!readVlq(data, p, end, len) || len > end - p) {
                                        parseError = tr("Invalid MIDI SysEx length.", "MIDI SysEx 长度无效。");
                                        break;
                                }
                                p += len;
                                continue;
                        }
                        if (status >= 0xf0) {
                                const int sysLen = status == 0xf2 ? 2 : ((status == 0xf1 || status == 0xf3) ? 1 : 0);
                                if (static_cast<std::size_t>(sysLen) > end - p) {
                                        parseError = tr("Truncated MIDI system event.", "MIDI 系统事件不完整。");
                                        break;
                                }
                                p += sysLen;
                                continue;
                        }

                        const std::uint8_t kind    = status & 0xf0;
                        const std::uint8_t channel = status & 0x0f;
                        const int          count   = (kind == 0xc0 || kind == 0xd0) ? 1 : 2;
                        if (static_cast<std::size_t>(count) > end - p) {
                                parseError = tr("Truncated MIDI channel event.", "MIDI 通道事件不完整。");
                                break;
                        }
                        const std::uint8_t a = data[p++];
                        const std::uint8_t b = count == 2 ? data[p++] : 0;
                        if ((kind == 0x80 || kind == 0x90) && a >= 128) {
                                parseError = tr("Invalid MIDI note number.", "MIDI 音符编号无效。");
                                break;
                        }
                        if (kind == 0x90 && b != 0)
                                (*active)[channel][a].push_back({tick, b, programs[channel]});
                        else if (kind == 0x80 || (kind == 0x90 && b == 0))
                                finishNote(channel, a, tick);
                        else if (kind == 0xc0)
                                programs[channel] = a;
                        else if (kind == 0xb0 && a == 64) {
                                const bool down = b >= 64;
                                if (sustainDown[channel] && !down)
                                        releaseSustain(channel, tick);
                                sustainDown[channel] = down;
                        } else if (kind == 0xb0 && (a == 120 || a == 123)) {
                                sustainDown[channel] = false;
                                for (std::uint8_t note = 0; note < 128; ++note)
                                        while (!(*active)[channel][note].empty())
                                                finishNote(channel, note, tick);
                                releaseSustain(channel, tick);
                        }
                }

                // Gracefully close notes missing a Note Off at the end of this track.
                for (std::uint8_t ch = 0; ch < 16; ++ch)
                        for (std::uint8_t note = 0; note < 128; ++note) {
                                sustainDown[ch] = false;
                                while (!(*active)[ch][note].empty())
                                        finishNote(ch, note, tick);
                        }
                for (std::uint8_t ch = 0; ch < 16; ++ch)
                        releaseSustain(ch, tick);
                for (auto &raw : rawNotes)
                        if (raw.track == trackIndex)
                                raw.trackName = trackName;
                pos = end;
                if (!parseError.empty())
                        break;
        }

        if (!parseError.empty() || parsedTracks != nTracks || rawNotes.size() >= kMaxNotes) {
                status_      = !parseError.empty()
                                   ? parseError
                                   : (rawNotes.size() >= kMaxNotes ? tr("MIDI note limit exceeded.", "MIDI 音符数量超过限制。")
                                                                   : tr("MIDI file contains fewer tracks than declared.",
                                                                   "MIDI 文件中的轨道少于头部声明数量。"));
                statusError_ = true;
                open_        = true;
                return false;
        }

        std::sort(tempos.begin(), tempos.end(), [](const Tempo &a, const Tempo &b) { return a.tick < b.tick; });
        std::vector<Tempo> uniqueTempos;
        for (const Tempo &tempo : tempos) {
                if (!uniqueTempos.empty() && uniqueTempos.back().tick == tempo.tick)
                        uniqueTempos.back() = tempo;
                else
                        uniqueTempos.push_back(tempo);
        }

        struct TempoSegment {
                std::uint64_t tick;
                double        seconds;
                std::uint32_t usPerQuarter;
        };
        std::vector<TempoSegment> segments;
        if ((division & 0x8000u) == 0) {
                double seconds = 0.0;
                for (std::size_t i = 0; i < uniqueTempos.size(); ++i) {
                        if (i != 0) {
                                const auto &prev  = uniqueTempos[i - 1];
                                seconds          += static_cast<double>(uniqueTempos[i].tick - prev.tick) * prev.usPerQuarter /
                                           (static_cast<double>(division) * 1000000.0);
                        }
                        segments.push_back({uniqueTempos[i].tick, seconds, uniqueTempos[i].usPerQuarter});
                }
        }
        auto tickToSeconds = [&](std::uint64_t tick) {
                if ((division & 0x8000u) != 0) {
                        const auto   smpteByte     = static_cast<std::int8_t>((division >> 8) & 0xff);
                        const double fps           = smpteByte == -29 ? 29.97 : static_cast<double>(-smpteByte);
                        const double ticksPerFrame = static_cast<double>(division & 0xffu);
                        return fps > 0.0 && ticksPerFrame > 0.0 ? tick / (fps * ticksPerFrame) : 0.0;
                }
                const auto it =
                    std::upper_bound(segments.begin(), segments.end(), tick, [](std::uint64_t value, const TempoSegment &s) {
                            return value < s.tick;
                    });
                const TempoSegment &segment = it == segments.begin() ? segments.front() : *std::prev(it);
                return segment.seconds + static_cast<double>(tick - segment.tick) * segment.usPerQuarter /
                                             (static_cast<double>(division) * 1000000.0);
        };

        std::vector<Note> parsed;
        parsed.reserve(rawNotes.size());
        double totalDuration = 0.0;
        for (const RawNote &raw : rawNotes) {
                const double start  = tickToSeconds(raw.startTick);
                const double endSec = tickToSeconds(raw.endTick);
                parsed.push_back({raw.track,
                                  raw.channel,
                                  raw.note,
                                  raw.velocity,
                                  raw.program,
                                  start,
                                  std::max(0.0, endSec - start),
                                  440.0 * std::pow(2.0, (static_cast<int>(raw.note) - 69) / 12.0),
                                  raw.trackName});
                totalDuration = std::max(totalDuration, endSec);
        }
        std::sort(parsed.begin(), parsed.end(), [](const Note &a, const Note &b) {
                return std::tie(a.startSec, a.track, a.channel, a.midiNote) <
                       std::tie(b.startSec, b.track, b.channel, b.midiNote);
        });

        double minBpm = std::numeric_limits<double>::max();
        double maxBpm = 0.0;
        for (const Tempo &tempo : uniqueTempos) {
                const double bpm = 60000000.0 / tempo.usPerQuarter;
                minBpm           = std::min(minBpm, bpm);
                maxBpm           = std::max(maxBpm, bpm);
        }

        notes_       = std::move(parsed);
        sourcePath_  = path;
        format_      = format;
        division_    = division;
        trackCount_  = parsedTracks;
        tempoCount_  = uniqueTempos.size();
        durationSec_ = totalDuration;
        minBpm_      = minBpm == std::numeric_limits<double>::max() ? 120.0 : minBpm;
        maxBpm_      = maxBpm == 0.0 ? minBpm_ : maxBpm;
        status_      = tr("MIDI parsed successfully.", "MIDI 解析成功。");
        if (format == 2)
                status_ += tr(" Format 2 tracks use independent timelines; displayed times are combined.",
                              " 格式 2 的轨道时间线彼此独立，当前按合并时间显示。");
        statusError_ = false;
        open_        = true;
        modified_    = true;
        return true;
}

bool
MidiTool::exportCsv(const std::string &path) const
{
        FILE *file = nativeFopen(path, "wb");
        if (!file)
                return false;
        std::fprintf(file,
                     "index,track,track_name,channel,program,midi_note,note_name,velocity,start_s,duration_s,end_s,"
                     "hfi_frequency_hz,hfi_voltage_v\r\n");
        const double speed = std::max(0.01, speedScale_);
        const double pitch = std::pow(2.0, transpose_ / 12.0);
        for (std::size_t i = 0; i < notes_.size(); ++i) {
                const Note       &note  = notes_[i];
                const double      hz    = note.frequencyHz * pitch;
                const double      start = note.startSec / speed;
                const double      dur   = note.durationSec / speed;
                const double      volts = minVoltage_ + (maxVoltage_ - minVoltage_) * note.velocity / 127.0;
                const std::string track = csvQuoted(note.trackName);
                std::fprintf(file,
                             "%zu,%u,%s,%u,%u,%u,%s,%u,%.9f,%.9f,%.9f,%.6f,%.6f\r\n",
                             i,
                             note.track,
                             track.c_str(),
                             static_cast<unsigned>(note.channel + 1),
                             static_cast<unsigned>(note.program),
                             static_cast<unsigned>(note.midiNote),
                             noteName(note.midiNote).c_str(),
                             static_cast<unsigned>(note.velocity),
                             start,
                             dur,
                             start + dur,
                             hz,
                             volts);
        }
        return std::fclose(file) == 0;
}

bool
MidiTool::exportC(const std::string &path) const
{
        const std::vector<Note> notes = makePlaybackNotes();
        if (notes.empty())
                return false;

        struct Event {
                double      time;
                std::size_t note;
                bool        on;
        };
        struct Step {
                double        frequency;
                double        voltage;
                std::uint32_t durationMs;
        };

        std::vector<Event> events;
        events.reserve(notes.size() * 2);
        for (std::size_t i = 0; i < notes.size(); ++i) {
                events.push_back({notes[i].startSec, i, true});
                events.push_back({notes[i].startSec + notes[i].durationSec, i, false});
        }
        std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
                if (a.time != b.time)
                        return a.time < b.time;
                return static_cast<int>(a.on) > static_cast<int>(b.on);
        });

        constexpr std::size_t           none = std::numeric_limits<std::size_t>::max();
        std::unordered_set<std::size_t> active;
        std::vector<Step>               steps;
        std::size_t                     selected   = none;
        std::uint64_t                   previousMs = 0;

        auto appendStep = [&](std::size_t noteIndex, std::uint64_t durationMs) {
                if (durationMs == 0)
                        return true;
                if (durationMs > std::numeric_limits<std::uint32_t>::max())
                        return false;
                double frequency = 0.0;
                double voltage   = 0.0;
                if (noteIndex != none) {
                        const Note &note = notes[noteIndex];
                        frequency        = note.frequencyHz;
                        voltage = minVoltage_ + (maxVoltage_ - minVoltage_) * static_cast<double>(note.velocity) / 127.0;
                }
                if (!steps.empty() && steps.back().frequency == frequency && steps.back().voltage == voltage &&
                    durationMs <= std::numeric_limits<std::uint32_t>::max() - steps.back().durationMs) {
                        steps.back().durationMs += static_cast<std::uint32_t>(durationMs);
                } else {
                        steps.push_back({frequency, voltage, static_cast<std::uint32_t>(durationMs)});
                }
                return true;
        };

        std::size_t eventIndex = 0;
        while (eventIndex < events.size()) {
                const double        eventTime = events[eventIndex].time;
                const std::uint64_t eventMs   = static_cast<std::uint64_t>(std::llround(std::max(0.0, eventTime) * 1000.0));
                if (eventMs > previousMs && !appendStep(selected, eventMs - previousMs))
                        return false;

                while (eventIndex < events.size() && std::abs(events[eventIndex].time - eventTime) < 1e-9) {
                        const Event &event = events[eventIndex++];
                        if (event.on)
                                active.insert(event.note);
                        else
                                active.erase(event.note);
                }

                selected = none;
                for (std::size_t candidate : active) {
                        if (selected == none || (polyphonyMode_ == 0 && notes[candidate].midiNote > notes[selected].midiNote) ||
                            (polyphonyMode_ == 1 && notes[candidate].midiNote < notes[selected].midiNote) ||
                            (polyphonyMode_ == 2 &&
                             (notes[candidate].startSec > notes[selected].startSec ||
                              (notes[candidate].startSec == notes[selected].startSec && candidate > selected))))
                                selected = candidate;
                }
                previousMs = eventMs;
        }

        if (steps.empty())
                return false;
        FILE *file = nativeFopen(path, "wb");
        if (!file)
                return false;
        std::fprintf(file,
                     "#ifndef ALARM_AUDIO_H\r\n"
                     "#define ALARM_AUDIO_H\r\n"
                     "\r\n"
                     "/* frequency (Hz), voltage amplitude (V), duration (ms) */\r\n"
                     "static const struct alarm_step g_default_alarm_pattern[] = {\r\n");
        for (const Step &step : steps) {
                const std::string frequency = cFloatLiteral(step.frequency);
                const std::string voltage   = cFloatLiteral(step.voltage);
                std::fprintf(
                    file, "    {%s, %s, %uU},\r\n", frequency.c_str(), voltage.c_str(), static_cast<unsigned>(step.durationMs));
        }
        std::fprintf(file, "};\r\n\r\n#endif // ALARM_AUDIO_H\r\n");
        return std::fclose(file) == 0;
}

void
MidiTool::draw()
{
        if (!open_)
                return;
        bool keepOpen = open_;
        if (ImGui::Begin(tr("MIDI Injection Parser###MidiTool", "MIDI 注入解析###MidiTool"), &keepOpen)) {
                if (ImGui::Button(tr("Open MIDI", "打开 MIDI"))) {
                        const std::string path = nativeDlgOpen("Open MIDI", {{"MIDI Files", {"mid", "midi"}}});
                        if (!path.empty())
                                loadFile(path);
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(notes_.empty());
                if (ImGui::Button(tr("Export CSV", "导出 CSV"))) {
                        std::string       defaultName = sourcePath_.empty()
                                                            ? "midi_notes.csv"
                                                            : std::filesystem::path(sourcePath_).stem().string() + "_notes.csv";
                        const std::string path        = nativeDlgSave("Export MIDI CSV", {{"CSV Files", {"csv"}}}, defaultName);
                        if (!path.empty()) {
                                statusError_ = !exportCsv(path);
                                status_      = statusError_ ? tr("CSV export failed.", "CSV 导出失败。")
                                                            : tr("CSV exported.", "CSV 已导出。");
                        }
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Export C header", "导出 C 头文件"))) {
                        const std::string defaultName = sourcePath_.empty()
                                                            ? "alarm_audio.h"
                                                            : std::filesystem::path(sourcePath_).stem().string() + "_alarm.h";
                        const std::string path =
                            nativeDlgSave("Export MIDI C Header", {{"C Header Files", {"h"}}}, defaultName);
                        if (!path.empty()) {
                                statusError_ = !exportC(path);
                                status_      = statusError_ ? tr("C header export failed.", "C 头文件导出失败。")
                                                            : tr("C header exported.", "C 头文件已导出。");
                        }
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button(tr("Clear", "清空")))
                        clear();

                ImGui::SameLine();
                ImGui::TextDisabled("%s", tr("Drop .mid/.midi anywhere", "可将 .mid/.midi 拖到任意位置"));
                if (!sourcePath_.empty()) {
                        ImGui::TextWrapped("%s", sourcePath_.c_str());
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", sourcePath_.c_str());
                }

                const int state = playbackState_.load(std::memory_order_acquire);
                ImGui::BeginDisabled(state != 0);
                drawTarget(tr("HFI frequency variable", "HFI 注入频率变量"), "hfi_frequency", frequencyTarget_);
                drawTarget(tr("HFI voltage variable", "HFI 电压幅值变量"), "hfi_voltage", amplitudeTarget_);

                const double oldSpeed      = speedScale_;
                const double oldMinVoltage = minVoltage_;
                const double oldMaxVoltage = maxVoltage_;
                const int    oldTranspose  = transpose_;
                const int    oldTrack      = trackFilter_;
                const int    oldChannel    = channelFilter_;
                const int    oldPolyphony  = polyphonyMode_;

                ImGui::SetNextItemWidth(110.0f);
                ImGui::InputDouble(tr("Min voltage", "最小电压"), &minVoltage_, 0.1, 1.0, "%.3f V");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(110.0f);
                ImGui::InputDouble(tr("Max voltage", "最大电压"), &maxVoltage_, 0.1, 1.0, "%.3f V");
                minVoltage_ = std::max(0.0, minVoltage_);
                maxVoltage_ = std::max(minVoltage_, maxVoltage_);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(110.0f);
                ImGui::SliderInt(tr("Transpose", "移调"), &transpose_, -48, 48, "%+d st");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(105.0f);
                const double minSpeed = 0.1;
                const double maxSpeed = 4.0;
                ImGui::SliderScalar(tr("Speed", "速度"), ImGuiDataType_Double, &speedScale_, &minSpeed, &maxSpeed, "%.2fx");

                ImGui::SetNextItemWidth(90.0f);
                ImGui::InputInt(tr("Track (-1=all)", "轨道（-1=全部）"), &trackFilter_);
                trackFilter_ = std::clamp(trackFilter_, -1, std::max(-1, static_cast<int>(trackCount_) - 1));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                ImGui::InputInt(tr("Channel (-1=all)", "通道（-1=全部）"), &channelFilter_);
                channelFilter_ = std::clamp(channelFilter_, -1, 15);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(145.0f);
                const char *polyphonyItems[] = {
                    tr("Highest note", "最高音"), tr("Lowest note", "最低音"), tr("Latest note", "最后按下")};
                ImGui::Combo(tr("Polyphony", "复音选择"), &polyphonyMode_, polyphonyItems, 3);

                if (speedScale_ != oldSpeed || minVoltage_ != oldMinVoltage || maxVoltage_ != oldMaxVoltage ||
                    transpose_ != oldTranspose || trackFilter_ != oldTrack || channelFilter_ != oldChannel ||
                    polyphonyMode_ != oldPolyphony)
                        modified_ = true;
                ImGui::EndDisabled();

                if (ImGui::Button(tr("Start HFI playback", "开始 HFI 播放")))
                        startPlayback();
                ImGui::SameLine();
                ImGui::BeginDisabled(state == 0);
                if (state == 2) {
                        if (ImGui::Button(tr("Resume", "继续"))) {
                                playbackState_.store(1, std::memory_order_release);
                                playbackCv_.notify_all();
                        }
                } else {
                        if (ImGui::Button(tr("Pause", "暂停"))) {
                                playbackState_.store(2, std::memory_order_release);
                                playbackCv_.notify_all();
                        }
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Stop", "停止")))
                        stopPlayback();
                ImGui::EndDisabled();

                ImGui::SameLine();
                const int liveNote = playbackMidiNote_.load(std::memory_order_acquire);
                if (playbackWriteFailed_.load(std::memory_order_acquire))
                        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                                           "%s",
                                           tr("J-Link write failed; playback stopped", "J-Link 写入失败，播放已停止"));
                else if (state != 0)
                        ImGui::Text(tr("Time %.3f s | %s | %.3f Hz | %.3f V", "时间 %.3f s | %s | %.3f Hz | %.3f V"),
                                    playbackTimeSec_.load(std::memory_order_acquire),
                                    liveNote >= 0 ? noteName(static_cast<std::uint8_t>(liveNote)).c_str() : tr("rest", "休止"),
                                    playbackFrequencyHz_.load(std::memory_order_acquire),
                                    playbackVoltage_.load(std::memory_order_acquire));

                if (!notes_.empty()) {
                        ImGui::Text(tr("Format %u | Tracks %u | TPQN/SMPTE 0x%04X | Notes %zu | Tempo events %zu | "
                                       "Duration %.3f s | BPM %.2f..%.2f",
                                       "格式 %u | 轨道 %u | TPQN/SMPTE 0x%04X | 音符 %zu | 速度事件 %zu | "
                                       "时长 %.3f s | BPM %.2f..%.2f"),
                                    format_,
                                    trackCount_,
                                    division_,
                                    notes_.size(),
                                    tempoCount_,
                                    durationSec_ / std::max(0.01, speedScale_),
                                    minBpm_,
                                    maxBpm_);
                }
                if (!status_.empty())
                        ImGui::TextColored(statusError_ ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f) : ImVec4(0.35f, 0.9f, 0.45f, 1.0f),
                                           "%s",
                                           status_.c_str());

                const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                              ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                              ImGuiTableFlags_SizingFixedFit;
                if (ImGui::BeginTable("##midi_notes", 10, flags, ImVec2(0, 0))) {
                        ImGui::TableSetupScrollFreeze(2, 1);
                        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                        ImGui::TableSetupColumn(tr("Track", "轨道"), ImGuiTableColumnFlags_WidthFixed, 65.0f);
                        ImGui::TableSetupColumn(tr("Channel", "通道"), ImGuiTableColumnFlags_WidthFixed, 65.0f);
                        ImGui::TableSetupColumn(tr("Note", "音符"), ImGuiTableColumnFlags_WidthFixed, 75.0f);
                        ImGui::TableSetupColumn(tr("Velocity", "力度"), ImGuiTableColumnFlags_WidthFixed, 65.0f);
                        ImGui::TableSetupColumn(tr("Start (s)", "开始 (s)"), ImGuiTableColumnFlags_WidthFixed, 100.0f);
                        ImGui::TableSetupColumn(tr("Duration (s)", "时长 (s)"), ImGuiTableColumnFlags_WidthFixed, 100.0f);
                        ImGui::TableSetupColumn(tr("End (s)", "结束 (s)"), ImGuiTableColumnFlags_WidthFixed, 100.0f);
                        ImGui::TableSetupColumn(
                            tr("HFI frequency Hz", "HFI 频率 Hz"), ImGuiTableColumnFlags_WidthFixed, 120.0f);
                        ImGui::TableSetupColumn(tr("HFI voltage V", "HFI 电压 V"), ImGuiTableColumnFlags_WidthFixed, 110.0f);
                        ImGui::TableHeadersRow();

                        const int liveRow = playbackRowIndex_.load(std::memory_order_acquire);
                        if (liveRow < 0)
                                lastAutoScrolledRow_ = -1;
                        const bool       scrollToLiveRow = liveRow >= 0 && liveRow != lastAutoScrolledRow_;
                        ImGuiListClipper clipper;
                        clipper.Begin(static_cast<int>(notes_.size()));
                        if (scrollToLiveRow && liveRow < static_cast<int>(notes_.size()))
                                clipper.IncludeItemByIndex(liveRow);
                        const double speed = std::max(0.01, speedScale_);
                        const double pitch = std::pow(2.0, transpose_ / 12.0);
                        while (clipper.Step()) {
                                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                                        const Note  &note  = notes_[static_cast<std::size_t>(row)];
                                        const double hz    = note.frequencyHz * pitch;
                                        const double start = note.startSec / speed;
                                        const double dur   = note.durationSec / speed;
                                        const double volts = minVoltage_ + (maxVoltage_ - minVoltage_) * note.velocity / 127.0;
                                        ImGui::TableNextRow();
                                        if (row == liveRow) {
                                                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                                                       ImGui::GetColorU32(ImVec4(1.0f, 0.68f, 0.08f, 0.42f)));
                                                if (scrollToLiveRow) {
                                                        ImGui::SetScrollHereY(0.5f);
                                                        lastAutoScrolledRow_ = liveRow;
                                                }
                                        }
                                        ImGui::TableSetColumnIndex(0);
                                        ImGui::Text("%d", row);
                                        ImGui::TableSetColumnIndex(1);
                                        ImGui::Text("%u", note.track);
                                        if (ImGui::IsItemHovered())
                                                ImGui::SetTooltip("%s", note.trackName.c_str());
                                        ImGui::TableSetColumnIndex(2);
                                        ImGui::Text("%u", static_cast<unsigned>(note.channel + 1));
                                        ImGui::TableSetColumnIndex(3);
                                        ImGui::Text("%s (%u)", noteName(note.midiNote).c_str(), note.midiNote);
                                        ImGui::TableSetColumnIndex(4);
                                        ImGui::Text("%u", note.velocity);
                                        ImGui::TableSetColumnIndex(5);
                                        ImGui::Text("%.6f", start);
                                        ImGui::TableSetColumnIndex(6);
                                        ImGui::Text("%.6f", dur);
                                        ImGui::TableSetColumnIndex(7);
                                        ImGui::Text("%.6f", start + dur);
                                        ImGui::TableSetColumnIndex(8);
                                        ImGui::Text("%.3f", hz);
                                        ImGui::TableSetColumnIndex(9);
                                        ImGui::Text("%.3f", volts);
                                }
                        }
                        ImGui::EndTable();
                }
        }
        ImGui::End();
        if (keepOpen != open_) {
                if (!keepOpen)
                        stopPlayback();
                open_     = keepOpen;
                modified_ = true;
        }
}

void
MidiTool::save(void *node) const
{
        auto  *root = static_cast<cJSON *>(node);
        cJSON *obj  = cJSON_CreateObject();
        cJSON_AddBoolToObject(obj, "open", open_);
        cJSON_AddStringToObject(obj, "sourcePath", sourcePath_.c_str());
        cJSON_AddNumberToObject(obj, "speedScale", speedScale_);
        cJSON_AddNumberToObject(obj, "transpose", transpose_);
        cJSON_AddNumberToObject(obj, "minVoltage", minVoltage_);
        cJSON_AddNumberToObject(obj, "maxVoltage", maxVoltage_);
        cJSON_AddNumberToObject(obj, "trackFilter", trackFilter_);
        cJSON_AddNumberToObject(obj, "channelFilter", channelFilter_);
        cJSON_AddNumberToObject(obj, "polyphonyMode", polyphonyMode_);
        auto saveTarget = [obj](const char *name, const Target &target) {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "name", target.name.c_str());
                cJSON_AddStringToObject(item, "symbol", target.symbol.c_str());
                cJSON_AddStringToObject(item, "type", target.type.c_str());
                cJSON_AddNumberToObject(item, "address", target.address);
                cJSON_AddNumberToObject(item, "numBytes", target.numBytes);
                cJSON_AddNumberToObject(item, "bitOffset", target.bitOffset);
                cJSON_AddNumberToObject(item, "bitSize", target.bitSize);
                cJSON_AddBoolToObject(item, "writable", target.writable);
                cJSON_AddItemToObject(obj, name, item);
        };
        saveTarget("frequencyTarget", frequencyTarget_);
        saveTarget("amplitudeTarget", amplitudeTarget_);
        cJSON_AddItemToObject(root, "midiTool", obj);
}

void
MidiTool::load(const void *node)
{
        stopPlayback();
        notes_.clear();
        sourcePath_.clear();
        status_.clear();
        statusError_      = false;
        open_             = false;
        format_           = 0;
        division_         = 0;
        trackCount_       = 0;
        tempoCount_       = 0;
        durationSec_      = 0.0;
        minBpm_           = 120.0;
        maxBpm_           = 120.0;
        speedScale_       = 1.0;
        transpose_        = 0;
        minVoltage_       = 0.0;
        maxVoltage_       = 1.0;
        trackFilter_      = -1;
        channelFilter_    = -1;
        polyphonyMode_    = 0;
        frequencyTarget_  = Target{};
        amplitudeTarget_  = Target{};
        const auto  *root = static_cast<const cJSON *>(node);
        const cJSON *obj  = cJSON_GetObjectItem(root, "midiTool");
        if (!cJSON_IsObject(obj)) {
                modified_ = false;
                return;
        }
        bool savedOpen = open_;
        if (const cJSON *v = cJSON_GetObjectItem(obj, "open"); cJSON_IsBool(v))
                savedOpen = cJSON_IsTrue(v);
        if (const cJSON *v = cJSON_GetObjectItem(obj, "speedScale"); cJSON_IsNumber(v))
                speedScale_ = std::clamp(v->valuedouble, 0.1, 4.0);
        if (const cJSON *v = cJSON_GetObjectItem(obj, "transpose"); cJSON_IsNumber(v))
                transpose_ = std::clamp(v->valueint, -48, 48);
        if (const cJSON *v = cJSON_GetObjectItem(obj, "minVoltage"); cJSON_IsNumber(v))
                minVoltage_ = std::max(0.0, v->valuedouble);
        if (const cJSON *v = cJSON_GetObjectItem(obj, "maxVoltage"); cJSON_IsNumber(v))
                maxVoltage_ = std::max(minVoltage_, v->valuedouble);
        if (const cJSON *v = cJSON_GetObjectItem(obj, "trackFilter"); cJSON_IsNumber(v))
                trackFilter_ = v->valueint;
        if (const cJSON *v = cJSON_GetObjectItem(obj, "channelFilter"); cJSON_IsNumber(v))
                channelFilter_ = std::clamp(v->valueint, -1, 15);
        if (const cJSON *v = cJSON_GetObjectItem(obj, "polyphonyMode"); cJSON_IsNumber(v))
                polyphonyMode_ = std::clamp(v->valueint, 0, 2);
        auto loadTarget = [obj](const char *name, Target &target) {
                const cJSON *item = cJSON_GetObjectItem(obj, name);
                if (!cJSON_IsObject(item))
                        return;
                if (const cJSON *v = cJSON_GetObjectItem(item, "name"); cJSON_IsString(v))
                        target.name = v->valuestring;
                if (const cJSON *v = cJSON_GetObjectItem(item, "symbol"); cJSON_IsString(v))
                        target.symbol = v->valuestring;
                if (const cJSON *v = cJSON_GetObjectItem(item, "type"); cJSON_IsString(v))
                        target.type = v->valuestring;
                if (const cJSON *v = cJSON_GetObjectItem(item, "address"); cJSON_IsNumber(v))
                        target.address = static_cast<std::uint32_t>(v->valuedouble);
                if (const cJSON *v = cJSON_GetObjectItem(item, "numBytes"); cJSON_IsNumber(v))
                        target.numBytes = v->valueint;
                if (const cJSON *v = cJSON_GetObjectItem(item, "bitOffset"); cJSON_IsNumber(v))
                        target.bitOffset = v->valueint;
                if (const cJSON *v = cJSON_GetObjectItem(item, "bitSize"); cJSON_IsNumber(v))
                        target.bitSize = v->valueint;
                if (const cJSON *v = cJSON_GetObjectItem(item, "writable"); cJSON_IsBool(v))
                        target.writable = cJSON_IsTrue(v);
        };
        loadTarget("frequencyTarget", frequencyTarget_);
        loadTarget("amplitudeTarget", amplitudeTarget_);
        if (const cJSON *v = cJSON_GetObjectItem(obj, "sourcePath"); cJSON_IsString(v) && v->valuestring[0]) {
                // nativeFopen() performs the project's UTF-8 -> UTF-16 conversion
                // on Windows. Constructing std::filesystem::path directly from a
                // UTF-8 JSON string can throw for Chinese paths under the active
                // Windows code page and used to abort the entire .ava load.
                try {
                        loadFile(v->valuestring);
                } catch (const std::exception &) {
                        notes_.clear();
                        sourcePath_  = v->valuestring;
                        status_      = tr("MIDI could not be restored from this session.", "无法从该会话恢复 MIDI 文件。");
                        statusError_ = true;
                }
        }
        open_     = savedOpen;
        modified_ = false;
}
