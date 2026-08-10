#ifndef MIDI_TOOL_HPP
#define MIDI_TOOL_HPP

#include <cstdint>
#include <string>
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
        };

        void draw();
        bool loadFile(const std::string &path);
        void clear();

        bool isOpen() const { return open_; }
        void setOpen(bool open);
        bool consumeModified();

        void save(void *node) const;
        void load(const void *node);

      private:
        bool exportCsv(const std::string &path) const;

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

        double carrierHz_{40000.0};
        double speedScale_{1.0};
        int    transpose_{0};
};

#endif
