#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mmsystem.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>

#include "app_log.hpp"
#include "core/session_time.hpp"
#include "gui/audio_input.hpp"

static constexpr int kAudioNumBufs    = 3;
static constexpr int kAudioBufSamples = 1024;
static constexpr int kAudioSampleRate = 44100;
static constexpr int kMaxQueuedBlocks = 128;

struct AudioInput::Stream {
        int                      deviceIndex{0};
        HWAVEIN                  hWaveIn{nullptr};
        WAVEHDR                  headers[kAudioNumBufs]{};
        std::vector<int16_t>     buffers[kAudioNumBufs];
        mutable std::mutex       mtx;
        std::vector<AudioSample> samples;
        AudioSample              latest;
        bool                     hasLatest{false};
        std::string              lastError;
        bool                     running{false};
};

static void CALLBACK
audioInputWaveProc(HWAVEIN, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR)
{
        if (msg != WIM_DATA || instance == 0)
                return;

        auto     *s   = reinterpret_cast<AudioInput::Stream *>(instance);
        auto     *hdr = reinterpret_cast<WAVEHDR *>(param1);
        const int n   = static_cast<int>(hdr->dwBytesRecorded / sizeof(int16_t));
        if (n > 0) {
                const int16_t *raw = reinterpret_cast<const int16_t *>(hdr->lpData);
                const double   dt  = 1.0 / static_cast<double>(kAudioSampleRate);
                const double   end = sessionTimeSec();
                const double   t0  = end - static_cast<double>(n - 1) * dt;

                std::lock_guard<std::mutex> lk(s->mtx);
                const size_t                oldSize = s->samples.size();
                s->samples.resize(oldSize + static_cast<size_t>(n));
                for (int i = 0; i < n; ++i) {
                        auto &dst = s->samples[oldSize + static_cast<size_t>(i)];
                        dst.ts    = t0 + static_cast<double>(i) * dt;
                        dst.value = static_cast<float>(raw[i]) * (1.0f / 32768.0f);
                }
                s->latest              = s->samples.back();
                s->hasLatest           = true;
                const size_t maxQueued = static_cast<size_t>(kAudioBufSamples * kMaxQueuedBlocks);
                if (s->samples.size() > maxQueued)
                        s->samples.erase(s->samples.begin(), s->samples.end() - maxQueued);
        }

        if (s->running && s->hWaveIn) {
                hdr->dwBytesRecorded = 0;
                waveInAddBuffer(s->hWaveIn, hdr, sizeof(WAVEHDR));
        }
}

AudioInput &
AudioInput::instance()
{
        static AudioInput inst;
        return inst;
}

AudioInput::AudioInput()
{
        refreshDevices();
}

AudioInput::~AudioInput()
{
        stopAll();
}

void
AudioInput::refreshDevices()
{
        devices_.clear();
        const int n = static_cast<int>(waveInGetNumDevs());
        for (int i = 0; i < n; ++i) {
                WAVEINCAPSW caps{};
                std::string name = "Audio " + std::to_string(i);
                if (waveInGetDevCapsW(static_cast<UINT>(i), &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                        int len = WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, nullptr, 0, nullptr, nullptr);
                        if (len > 0) {
                                name.assign(static_cast<size_t>(len), '\0');
                                WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, name.data(), len, nullptr, nullptr);
                                if (!name.empty() && name.back() == '\0')
                                        name.pop_back();
                        }
                }
                devices_.push_back({i, std::move(name)});
        }
}

int
AudioInput::defaultDeviceIndex() const
{
        return devices_.empty() ? -1 : devices_.front().index;
}

std::string
AudioInput::deviceName(int index) const
{
        for (const auto &d : devices_)
                if (d.index == index)
                        return d.name;
        return index >= 0 ? ("Audio " + std::to_string(index)) : "Audio";
}

void
AudioInput::setActiveDevices(const std::vector<int> &indices)
{
        std::set<int> wanted;
        for (int idx : indices)
                if (idx >= 0)
                        wanted.insert(idx);

        for (auto it = streams_.begin(); it != streams_.end();) {
                if (!wanted.contains(it->first)) {
                        stopDevice(it->first);
                        it = streams_.begin();
                } else {
                        ++it;
                }
        }

        for (int idx : wanted)
                startDevice(idx);
}

bool
AudioInput::startDevice(int index)
{
        if (index < 0)
                return false;
        if (auto it = streams_.find(index); it != streams_.end() && it->second->running)
                return true;

        auto s         = std::make_unique<Stream>();
        s->deviceIndex = index;

        WAVEFORMATEX wfx{};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = 1;
        wfx.nSamplesPerSec  = kAudioSampleRate;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = 2;
        wfx.nAvgBytesPerSec = kAudioSampleRate * 2;

        MMRESULT res = waveInOpen(&s->hWaveIn,
                                  static_cast<UINT>(index),
                                  &wfx,
                                  reinterpret_cast<DWORD_PTR>(audioInputWaveProc),
                                  reinterpret_cast<DWORD_PTR>(s.get()),
                                  CALLBACK_FUNCTION);
        if (res != MMSYSERR_NOERROR) {
                char err[256]{};
                waveInGetErrorTextA(res, err, sizeof(err));
                s->lastError    = err;
                streams_[index] = std::move(s);
                LOG_E("AudioInput start failed: device=%d err=%s", index, err);
                return false;
        }

        s->running = true;
        for (int i = 0; i < kAudioNumBufs; ++i) {
                s->buffers[i].resize(kAudioBufSamples);
                ZeroMemory(&s->headers[i], sizeof(WAVEHDR));
                s->headers[i].lpData         = reinterpret_cast<LPSTR>(s->buffers[i].data());
                s->headers[i].dwBufferLength = static_cast<DWORD>(kAudioBufSamples * sizeof(int16_t));
                waveInPrepareHeader(s->hWaveIn, &s->headers[i], sizeof(WAVEHDR));
                waveInAddBuffer(s->hWaveIn, &s->headers[i], sizeof(WAVEHDR));
        }
        waveInStart(s->hWaveIn);
        LOG_I("AudioInput started: device=%d name=%s", index, deviceName(index).c_str());
        streams_[index] = std::move(s);
        return true;
}

void
AudioInput::stopDevice(int index)
{
        auto it = streams_.find(index);
        if (it == streams_.end())
                return;
        Stream *s = it->second.get();
        if (s->hWaveIn) {
                s->running = false;
                waveInStop(s->hWaveIn);
                waveInReset(s->hWaveIn);
                for (int i = 0; i < kAudioNumBufs; ++i)
                        waveInUnprepareHeader(s->hWaveIn, &s->headers[i], sizeof(WAVEHDR));
                waveInClose(s->hWaveIn);
                s->hWaveIn = nullptr;
        }
        streams_.erase(it);
}

void
AudioInput::stopAll()
{
        while (!streams_.empty())
                stopDevice(streams_.begin()->first);
}

bool
AudioInput::drainSamples(int deviceIndex, std::vector<AudioSample> &out)
{
        auto it = streams_.find(deviceIndex);
        if (it == streams_.end() || !it->second->running)
                return false;
        std::lock_guard<std::mutex> lk(it->second->mtx);
        if (it->second->samples.empty())
                return true;
        out = std::move(it->second->samples);
        it->second->samples.clear();
        return true;
}

bool
AudioInput::latestSample(int deviceIndex, AudioSample &out) const
{
        auto it = streams_.find(deviceIndex);
        if (it == streams_.end())
                return false;
        std::lock_guard<std::mutex> lk(it->second->mtx);
        if (!it->second->hasLatest)
                return false;
        out = it->second->latest;
        return true;
}

std::string
AudioInput::lastError(int deviceIndex) const
{
        auto it = streams_.find(deviceIndex);
        if (it == streams_.end())
                return {};
        std::lock_guard<std::mutex> lk(it->second->mtx);
        return it->second->lastError;
}
