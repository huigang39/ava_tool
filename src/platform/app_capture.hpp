#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace app_capture
{
// OpenGL RGBA pixels, starting at the bottom left. No desktop or audio capture.
struct Frame {
        int                  width{}, height{};
        std::vector<uint8_t> rgba;
        int64_t              timeUs{};
        bool                 valid() const;
};
int64_t              nowUs();
std::vector<uint8_t> clipboardDib(const Frame &frame);
bool                 copyToClipboard(const Frame &frame, void *owner, std::string &error);
// Fixed, even output dimensions; resizes are letterboxed, never stretched.
std::vector<uint8_t> toNv12(const Frame &frame, int width, int height);

class Recorder
{
      public:
        enum class State { Idle, Starting, Recording, Finishing };
        struct Result {
                bool        ok{};
                std::string path, error;
        };
        ~Recorder();
        bool                  start(const std::string &path, int width, int height);
        void                  stop();
        void                  finish();
        bool                  wantsFrame(int64_t timeUs) const;
        void                  submit(Frame frame);
        State                 state() const { return state_.load(); }
        double                elapsed() const;
        std::optional<Result> takeResult();

      private:
        void                    run(std::string path, int width, int height);
        mutable std::mutex      mutex_;
        std::condition_variable ready_;
        std::deque<Frame>       frames_;
        std::thread             worker_;
        std::atomic<State>      state_{State::Idle};
        std::atomic<int64_t>    startedUs_{0}, lastFrameUs_{0};
        int64_t                 stoppedUs_{};
        bool                    stopping_{};
        std::optional<Result>   result_;
};
} // namespace app_capture
