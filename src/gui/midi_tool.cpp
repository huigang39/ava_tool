#include "gui/midi_tool.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <string_view>
#include <tuple>

#include "cJSON.h"
#include "gui/i18n.hpp"
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
} // namespace

void
MidiTool::setOpen(bool open)
{
        if (open_ != open) {
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

                const std::uint32_t                                 trackIndex = parsedTracks++;
                std::string                                         trackName  = "Track " + std::to_string(trackIndex + 1);
                std::array<std::uint8_t, 16>                        programs{};
                std::array<std::array<std::deque<Active>, 128>, 16> active;
                std::array<std::array<std::deque<Active>, 128>, 16> sustained;
                std::array<bool, 16>                                sustainDown{};
                std::uint64_t                                       tick          = 0;
                std::uint8_t                                        runningStatus = 0;
                std::size_t                                         p             = body;

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
                        auto &queue = active[channel][note];
                        if (queue.empty())
                                return;
                        const Active start = queue.front();
                        queue.pop_front();
                        if (sustainDown[channel])
                                sustained[channel][note].push_back(start);
                        else
                                emitNote(channel, note, start, endTick);
                };
                auto releaseSustain = [&](std::uint8_t channel, std::uint64_t endTick) {
                        for (std::uint8_t note = 0; note < 128; ++note) {
                                auto &queue = sustained[channel][note];
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
                                active[channel][a].push_back({tick, b, programs[channel]});
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
                                        while (!active[channel][note].empty())
                                                finishNote(channel, note, tick);
                                releaseSustain(channel, tick);
                        }
                }

                // Gracefully close notes missing a Note Off at the end of this track.
                for (std::uint8_t ch = 0; ch < 16; ++ch)
                        for (std::uint8_t note = 0; note < 128; ++note) {
                                sustainDown[ch] = false;
                                while (!active[ch][note].empty())
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
                     "note_hz,carrier_hz,lower_sideband_hz,upper_sideband_hz\r\n");
        const double speed = std::max(0.01, speedScale_);
        const double pitch = std::pow(2.0, transpose_ / 12.0);
        for (std::size_t i = 0; i < notes_.size(); ++i) {
                const Note       &note  = notes_[i];
                const double      hz    = note.frequencyHz * pitch;
                const double      start = note.startSec / speed;
                const double      dur   = note.durationSec / speed;
                const std::string track = csvQuoted(note.trackName);
                std::fprintf(file,
                             "%zu,%u,%s,%u,%u,%u,%s,%u,%.9f,%.9f,%.9f,%.6f,%.6f,%.6f,%.6f\r\n",
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
                             carrierHz_,
                             std::max(0.0, carrierHz_ - hz),
                             carrierHz_ + hz);
        }
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

                const double oldCarrier   = carrierHz_;
                const double oldSpeed     = speedScale_;
                const int    oldTranspose = transpose_;
                ImGui::SetNextItemWidth(150.0f);
                ImGui::InputDouble(tr("Carrier Hz", "载波 Hz"), &carrierHz_, 1000.0, 10000.0, "%.1f");
                carrierHz_ = std::max(0.0, carrierHz_);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120.0f);
                ImGui::SliderInt(tr("Transpose", "移调"), &transpose_, -48, 48, "%+d st");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120.0f);
                const double minSpeed = 0.1;
                const double maxSpeed = 4.0;
                ImGui::SliderScalar(tr("Speed", "速度"), ImGuiDataType_Double, &speedScale_, &minSpeed, &maxSpeed, "%.2fx");
                if (carrierHz_ != oldCarrier || speedScale_ != oldSpeed || transpose_ != oldTranspose)
                        modified_ = true;

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
                if (ImGui::BeginTable("##midi_notes", 12, flags, ImVec2(0, 0))) {
                        ImGui::TableSetupScrollFreeze(2, 1);
                        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                        ImGui::TableSetupColumn(tr("Track", "轨道"), ImGuiTableColumnFlags_WidthFixed, 65.0f);
                        ImGui::TableSetupColumn(tr("Channel", "通道"), ImGuiTableColumnFlags_WidthFixed, 65.0f);
                        ImGui::TableSetupColumn(tr("Note", "音符"), ImGuiTableColumnFlags_WidthFixed, 75.0f);
                        ImGui::TableSetupColumn(tr("Velocity", "力度"), ImGuiTableColumnFlags_WidthFixed, 65.0f);
                        ImGui::TableSetupColumn(tr("Start (s)", "开始 (s)"), ImGuiTableColumnFlags_WidthFixed, 100.0f);
                        ImGui::TableSetupColumn(tr("Duration (s)", "时长 (s)"), ImGuiTableColumnFlags_WidthFixed, 100.0f);
                        ImGui::TableSetupColumn(tr("End (s)", "结束 (s)"), ImGuiTableColumnFlags_WidthFixed, 100.0f);
                        ImGui::TableSetupColumn(tr("Note Hz", "音符 Hz"), ImGuiTableColumnFlags_WidthFixed, 105.0f);
                        ImGui::TableSetupColumn(tr("Carrier Hz", "载波 Hz"), ImGuiTableColumnFlags_WidthFixed, 105.0f);
                        ImGui::TableSetupColumn(tr("Lower Hz", "下边带 Hz"), ImGuiTableColumnFlags_WidthFixed, 110.0f);
                        ImGui::TableSetupColumn(tr("Upper Hz", "上边带 Hz"), ImGuiTableColumnFlags_WidthFixed, 110.0f);
                        ImGui::TableHeadersRow();

                        ImGuiListClipper clipper;
                        clipper.Begin(static_cast<int>(notes_.size()));
                        const double speed = std::max(0.01, speedScale_);
                        const double pitch = std::pow(2.0, transpose_ / 12.0);
                        while (clipper.Step()) {
                                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                                        const Note  &note  = notes_[static_cast<std::size_t>(row)];
                                        const double hz    = note.frequencyHz * pitch;
                                        const double start = note.startSec / speed;
                                        const double dur   = note.durationSec / speed;
                                        ImGui::TableNextRow();
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
                                        ImGui::Text("%.3f", carrierHz_);
                                        ImGui::TableSetColumnIndex(10);
                                        ImGui::Text("%.3f", std::max(0.0, carrierHz_ - hz));
                                        ImGui::TableSetColumnIndex(11);
                                        ImGui::Text("%.3f", carrierHz_ + hz);
                                }
                        }
                        ImGui::EndTable();
                }
        }
        ImGui::End();
        if (keepOpen != open_) {
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
        cJSON_AddNumberToObject(obj, "carrierHz", carrierHz_);
        cJSON_AddNumberToObject(obj, "speedScale", speedScale_);
        cJSON_AddNumberToObject(obj, "transpose", transpose_);
        cJSON_AddItemToObject(root, "midiTool", obj);
}

void
MidiTool::load(const void *node)
{
        const auto  *root = static_cast<const cJSON *>(node);
        const cJSON *obj  = cJSON_GetObjectItem(root, "midiTool");
        if (!cJSON_IsObject(obj)) {
                modified_ = false;
                return;
        }
        if (const cJSON *v = cJSON_GetObjectItem(obj, "open"); cJSON_IsBool(v))
                open_ = cJSON_IsTrue(v);
        if (const cJSON *v = cJSON_GetObjectItem(obj, "carrierHz"); cJSON_IsNumber(v))
                carrierHz_ = std::max(0.0, v->valuedouble);
        if (const cJSON *v = cJSON_GetObjectItem(obj, "speedScale"); cJSON_IsNumber(v))
                speedScale_ = std::clamp(v->valuedouble, 0.1, 4.0);
        if (const cJSON *v = cJSON_GetObjectItem(obj, "transpose"); cJSON_IsNumber(v))
                transpose_ = std::clamp(v->valueint, -48, 48);
        if (const cJSON *v = cJSON_GetObjectItem(obj, "sourcePath"); cJSON_IsString(v) && v->valuestring[0]) {
                std::error_code ec;
                if (std::filesystem::is_regular_file(v->valuestring, ec))
                        loadFile(v->valuestring);
        }
        modified_ = false;
}
