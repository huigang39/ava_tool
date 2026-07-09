#ifndef AUDIO_INPUT_HPP
#define AUDIO_INPUT_HPP

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct AudioSample {
        double ts{0.0};
        float  value{0.0f};
};

class AudioInput
{
      public:
        struct Stream;

        struct DeviceInfo {
                int         index{0};
                std::string name;
        };

        static AudioInput &instance();

        void                           refreshDevices();
        const std::vector<DeviceInfo> &devices() const { return devices_; }
        int                            defaultDeviceIndex() const;
        std::string                    deviceName(int index) const;

        void        setActiveDevices(const std::vector<int> &indices);
        bool        drainSamples(int deviceIndex, std::vector<AudioSample> &out);
        bool        latestSample(int deviceIndex, AudioSample &out) const;
        std::string lastError(int deviceIndex) const;

      private:
        AudioInput();
        ~AudioInput();

        bool startDevice(int index);
        void stopDevice(int index);
        void stopAll();

        std::vector<DeviceInfo>                          devices_;
        std::unordered_map<int, std::unique_ptr<Stream>> streams_;
};

#endif // AUDIO_INPUT_HPP
