#ifndef MIDI_TOOL_HPP
#define MIDI_TOOL_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class MidiTool
{
      public:
        struct Note {
                std::uint32_t track{0};
                std::uint8_t  channel{0};
                std::uint8_t  midiNote{0};
                std::uint8_t  velocity{0};
                std::uint8_t  program{0};
                double        startSec{0.0};
                double        durationSec{0.0};
                double        frequencyHz{0.0};
                std::string   trackName;
                int           sourceRow{-1};
        };

        MidiTool() = default;
        ~MidiTool();

        MidiTool(const MidiTool &)            = delete;
        MidiTool &operator=(const MidiTool &) = delete;

        void draw();
        bool loadFile(const std::string &path);
        void clear();
        void stop() { stopPlayback(); }
        void setSymbolResolver(std::function<bool(const std::string &, std::uint32_t &)> resolver)
        {
                symbolResolver_ = std::move(resolver);
        }

        bool isOpen() const { return open_; }
        void setOpen(bool open);
        bool consumeModified();

        void save(void *node) const;
        void load(const void *node);

      private:
        struct Target {
                std::string   name;
                std::string   symbol;
                std::string   type{"F32"};
                std::uint32_t address{0};
                std::uint32_t numBytes{4};
                std::uint32_t bitOffset{0};
                std::uint32_t bitSize{0};
                bool          writable{true};
        };

        bool              exportCsv(const std::string &path) const;
        bool              exportC(const std::string &path) const;
        std::vector<Note> makePlaybackNotes() const;
        void              drawTarget(const char *label, const char *id, Target &target);
        bool              acceptTargetDrop(Target &target);
        bool              validateTarget(Target &target, std::string &error);
        bool              writeTarget(const Target &target, float value);
        void              startPlayback();
        void              stopPlayback();
        void              playbackMain(std::vector<Note> notes,
                                       Target            frequency,
                                       Target            amplitude,
                                       double            minVoltage,
                                       double            maxVoltage,
                                       int               polyphonyMode);

        bool              open_{false};
        bool              modified_{false};
        std::string       sourcePath_;
        std::string       status_;
        bool              statusError_{false};
        std::vector<Note> notes_;
        std::uint16_t     format_{0};
        std::uint16_t     division_{0};
        std::uint16_t     trackCount_{0};
        std::size_t       tempoCount_{0};
        double            durationSec_{0.0};
        double            minBpm_{120.0};
        double            maxBpm_{120.0};

        double speedScale_{1.0};
        int    transpose_{0};
        double minVoltage_{0.0};
        double maxVoltage_{1.0};
        int    trackFilter_{-1};
        int    channelFilter_{-1};
        int    polyphonyMode_{0}; // 0 highest note, 1 lowest note, 2 latest note

        Target                                                    frequencyTarget_{};
        Target                                                    amplitudeTarget_{};
        std::function<bool(const std::string &, std::uint32_t &)> symbolResolver_{};

        std::thread             playbackThread_{};
        std::mutex              playbackMutex_{};
        std::condition_variable playbackCv_{};
        std::atomic<int>        playbackState_{0}; // 0 stopped, 1 playing, 2 paused
        std::atomic<bool>       stopRequested_{false};
        std::atomic<double>     playbackTimeSec_{0.0};
        std::atomic<int>        playbackMidiNote_{-1};
        std::atomic<int>        playbackRowIndex_{-1};
        std::atomic<float>      playbackFrequencyHz_{0.0f};
        std::atomic<float>      playbackVoltage_{0.0f};
        std::atomic<bool>       playbackWriteFailed_{false};
        int                     lastAutoScrolledRow_{-1};
};

#endif
