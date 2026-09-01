#include "platform/app_capture.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <windows.h>
#include <wrl/client.h>
#endif

namespace app_capture
{
int64_t
nowUs()
{
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
}
bool
Frame::valid() const
{
        return width > 0 && height > 0 && width <= 16384 && height <= 16384 &&
               rgba.size() == size_t(width) * size_t(height) * 4;
}

std::vector<uint8_t>
clipboardDib(const Frame &frame)
{
        if (!frame.valid())
                return {};
        // BITMAPINFOHEADER followed by bottom-up BGRX, compatible with ordinary
        // Windows paste targets. Use explicit fields so conversion is testable.
        std::vector<uint8_t> dib(40 + frame.rgba.size(), 0);
        auto                 field = [&](size_t offset, uint32_t value) { std::memcpy(dib.data() + offset, &value, 4); };
        field(0, 40);
        field(4, frame.width);
        field(8, frame.height);
        field(12, 1u | (32u << 16)); // planes / bit count
        field(20, static_cast<uint32_t>(frame.rgba.size()));
        for (size_t i = 0; i < frame.rgba.size(); i += 4) {
                dib[40 + i] = frame.rgba[i + 2];
                dib[41 + i] = frame.rgba[i + 1];
                dib[42 + i] = frame.rgba[i];
                dib[43 + i] = 255;
        }
        return dib;
}

bool
copyToClipboard(const Frame &frame, void *owner, std::string &error)
{
#ifdef _WIN32
        const auto dib = clipboardDib(frame);
        if (dib.empty()) {
                error = "Invalid capture dimensions";
                return false;
        }
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, dib.size());
        if (!memory) {
                error = "Cannot allocate clipboard image";
                return false;
        }
        void *data = GlobalLock(memory);
        if (!data) {
                GlobalFree(memory);
                error = "Cannot lock clipboard image";
                return false;
        }
        std::memcpy(data, dib.data(), dib.size());
        GlobalUnlock(memory);
        if (!OpenClipboard(static_cast<HWND>(owner))) {
                GlobalFree(memory);
                error = "Clipboard is busy; please try again";
                return false;
        }
        const bool ok = EmptyClipboard() && SetClipboardData(CF_DIB, memory);
        CloseClipboard();
        if (!ok) {
                GlobalFree(memory);
                error = "Cannot copy image to clipboard";
        }
        return ok; // The clipboard owns memory after SetClipboardData succeeds.
#else
        error = "Image clipboard capture is available on Windows";
        return false;
#endif
}

std::vector<uint8_t>
toNv12(const Frame &frame, int width, int height)
{
        if (!frame.valid() || width < 2 || height < 2 || width > 4096 || height > 4096 || (width & 1) || (height & 1))
                return {};
        std::vector<uint8_t> result(size_t(width) * height * 3 / 2, 128);
        std::fill_n(result.begin(), size_t(width) * height, uint8_t(16));
        const double scale = std::min(double(width) / frame.width, double(height) / frame.height);
        const int    w     = std::max(2, int(frame.width * scale) & ~1);
        const int    h     = std::max(2, int(frame.height * scale) & ~1);
        const int    left = ((width - w) / 2) & ~1, top = ((height - h) / 2) & ~1;
        auto         byte = [](double value) { return uint8_t(std::clamp(int(std::lround(value)), 0, 255)); };
        for (int y = 0; y < h; y += 2) {
                for (int x = 0; x < w; x += 2) {
                        double red = 0, green = 0, blue = 0;
                        for (int dy = 0; dy < 2; ++dy)
                                for (int dx = 0; dx < 2; ++dx) {
                                        const int sx = std::min(frame.width - 1, (x + dx) * frame.width / w);
                                        const int sy =
                                            frame.height - 1 - std::min(frame.height - 1, (y + dy) * frame.height / h);
                                        const auto *p = frame.rgba.data() + (size_t(sy) * frame.width + sx) * 4;
                                        result[size_t(top + y + dy) * width + left + x + dx] =
                                            byte(16 + .182586 * p[0] + .614231 * p[1] + .062007 * p[2]);
                                        red   += p[0];
                                        green += p[1];
                                        blue  += p[2];
                                }
                        const size_t uv = size_t(width) * height + size_t((top + y) / 2) * width + left + x;
                        result[uv]      = byte(128 - .100644 * red / 4 - .338572 * green / 4 + .439216 * blue / 4);
                        result[uv + 1]  = byte(128 + .439216 * red / 4 - .398942 * green / 4 - .040274 * blue / 4);
                }
        }
        return result;
}

Recorder::~Recorder()
{
        finish();
}
bool
Recorder::start(const std::string &path, int width, int height)
{
        if (state() != State::Idle || path.empty() || width < 2 || height < 2)
                return false;
        if (worker_.joinable())
                worker_.join();
        {
                std::lock_guard lock(mutex_);
                frames_.clear();
                result_.reset();
                stopping_  = false;
                stoppedUs_ = 0;
        }
        startedUs_         = nowUs();
        lastFrameUs_       = 0;
        state_             = State::Starting;
        const double scale = std::min({1.0, 1920.0 / width, 1080.0 / height});
        try {
                worker_ = std::thread(
                    &Recorder::run, this, path, std::max(2, int(width * scale) & ~1), std::max(2, int(height * scale) & ~1));
        } catch (...) {
                state_ = State::Idle;
                return false;
        }
        return true;
}
void
Recorder::stop()
{
        std::lock_guard lock(mutex_);
        if (state() == State::Idle || stopping_)
                return;
        stopping_  = true;
        stoppedUs_ = nowUs();
        state_     = State::Finishing;
        ready_.notify_all();
}
void
Recorder::finish()
{
        stop();
        if (worker_.joinable())
                worker_.join();
}
bool
Recorder::wantsFrame(int64_t timeUs) const
{
        const auto s = state();
        if ((s != State::Recording && s != State::Starting) || timeUs - lastFrameUs_.load() < 1000000 / 30)
                return false;
        std::lock_guard lock(mutex_);
        return !stopping_ && frames_.size() < 3;
}
void
Recorder::submit(Frame frame)
{
        if (!frame.valid())
                return;
        std::lock_guard lock(mutex_);
        if (stopping_ || state() == State::Idle || frames_.size() >= 3)
                return;
        if (frame.timeUs <= lastFrameUs_.load())
                return;
        lastFrameUs_ = frame.timeUs;
        frames_.push_back(std::move(frame));
        ready_.notify_one();
}
double
Recorder::elapsed() const
{
        return state() == State::Idle ? 0.0 : double(nowUs() - startedUs_.load()) / 1000000;
}
std::optional<Recorder::Result>
Recorder::takeResult()
{
        std::lock_guard lock(mutex_);
        auto            result = std::move(result_);
        result_.reset();
        return result;
}

void
Recorder::run(std::string path, int width, int height)
{
        Result result{false, path, {}};
#ifdef _WIN32
        using Microsoft::WRL::ComPtr;
        const HRESULT         com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const HRESULT         mf  = SUCCEEDED(com) ? MFStartup(MF_VERSION) : com;
        std::filesystem::path temporary;
        auto                  check = [](HRESULT hr, const char *stage) {
                if (FAILED(hr)) {
                        char code[16];
                        snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
                        throw std::runtime_error(std::string(stage) + " (" + code + ")");
                }
        };
        try {
                check(mf, "Initialize video encoder");
                const auto destination = std::filesystem::u8path(path);
                temporary              = destination;
                temporary +=
                    L".recording-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(nowUs()) + L".mp4";
                // Commit only a finalized file; an existing user file survives any encoder failure.
                ComPtr<IMFAttributes> attributes;
                check(MFCreateAttributes(&attributes, 2), "Create encoder attributes");
                check(attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE), "Configure encoder");
                ComPtr<IMFSinkWriter> writer;
                check(MFCreateSinkWriterFromURL(temporary.c_str(), nullptr, attributes.Get(), &writer), "Create MP4 file");
                ComPtr<IMFMediaType> output, input;
                check(MFCreateMediaType(&output), "Create video format");
                check(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set video type");
                check(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "Set H.264 format");
                check(output->SetUINT32(MF_MT_AVG_BITRATE, std::max(2000000, width * height * 5)), "Set video bitrate");
                check(output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Set progressive video");
                check(MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width, height), "Set video size");
                check(MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, 30, 1), "Set video rate");
                check(MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Set pixel aspect");
                DWORD stream{};
                check(writer->AddStream(output.Get(), &stream), "Add video stream");
                check(MFCreateMediaType(&input), "Create input format");
                check(output->CopyAllItems(input.Get()), "Copy input format");
                input->DeleteItem(MF_MT_AVG_BITRATE);
                check(input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Set NV12 input");
                check(input->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709), "Set video matrix");
                check(input->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235), "Set video range");
                check(writer->SetInputMediaType(stream, input.Get(), nullptr), "Configure H.264 encoder");
                check(writer->BeginWriting(), "Start MP4 recording");
                {
                        std::lock_guard lock(mutex_);
                        if (!stopping_)
                                state_ = State::Recording;
                }
                int64_t origin = 0;
                auto    write  = [&](const Frame &frame, int64_t endUs) {
                        const auto pixels = toNv12(frame, width, height);
                        if (pixels.empty())
                                throw std::runtime_error("Invalid recording frame");
                        ComPtr<IMFMediaBuffer> buffer;
                        check(MFCreateMemoryBuffer(static_cast<DWORD>(pixels.size()), &buffer), "Allocate video sample");
                        BYTE *bytes{};
                        check(buffer->Lock(&bytes, nullptr, nullptr), "Lock video sample");
                        std::memcpy(bytes, pixels.data(), pixels.size());
                        check(buffer->Unlock(), "Unlock video sample");
                        check(buffer->SetCurrentLength(static_cast<DWORD>(pixels.size())), "Set video sample size");
                        ComPtr<IMFSample> sample;
                        check(MFCreateSample(&sample), "Create video sample");
                        check(sample->AddBuffer(buffer.Get()), "Attach video sample");
                        check(sample->SetSampleTime((frame.timeUs - origin) * 10), "Set video time");
                        check(sample->SetSampleDuration(std::max<int64_t>(1, endUs - frame.timeUs) * 10), "Set video duration");
                        check(writer->WriteSample(stream, sample.Get()), "Encode video sample");
                };
                std::optional<Frame> pending;
                for (;;) {
                        Frame next;
                        {
                                std::unique_lock lock(mutex_);
                                ready_.wait(lock, [&] { return stopping_ || !frames_.empty(); });
                                if (frames_.empty())
                                        break;
                                next = std::move(frames_.front());
                                frames_.pop_front();
                        }
                        if (!pending)
                                origin = next.timeUs;
                        else
                                write(*pending, next.timeUs);
                        pending = std::move(next);
                }
                if (!pending)
                        throw std::runtime_error("No frames were captured");
                write(*pending, std::max(stoppedUs_, pending->timeUs + 1000000 / 30));
                check(writer->Finalize(), "Finalize MP4 recording");
                writer.Reset();
                if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                        throw std::runtime_error("Cannot save finalized recording; file retained at the temporary path below");
                temporary.clear();
                result.ok = true;
        } catch (const std::exception &e) {
                result.error = e.what();
        }
        // Keep any partial file for diagnosis/recovery instead of deleting user data.
        if (!result.ok && !temporary.empty()) {
                const auto encoded  = temporary.u8string();
                result.error       += "\n" + std::string(encoded.begin(), encoded.end());
        }
        if (SUCCEEDED(mf))
                MFShutdown();
        if (SUCCEEDED(com))
                CoUninitialize();
#else
        result.error = "MP4 recording is available on Windows";
#endif
        {
                std::lock_guard lock(mutex_);
                frames_.clear();
                stopping_ = true;
                result_   = std::move(result);
                state_    = State::Idle;
        }
}
} // namespace app_capture
