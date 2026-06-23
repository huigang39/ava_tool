#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "implot.h"

#include "fft.h"
#include "gui/audio_fft.hpp"
#include "gui/i18n.hpp"

static constexpr int kRingSize    = 65536; // must be power of 2
static constexpr int kNumBufs     = 2;
static constexpr int kBufSamples  = 2048;
static constexpr int kFftSizes[]  = {512, 1024, 2048, 4096, 8192};
static constexpr int kNumFftSizes = 5;

// --------------------------------------------------------------------------

struct AudioFft::Impl {
        // ── settings ──────────────────────────────────────────────────────────
        int   deviceIdx{0};
        int   fftSizeIdx{2};    // default → kFftSizes[2] = 2048
        int   sampleRateIdx{0}; // 0 = 44100 Hz, 1 = 48000 Hz
        float smoothing{0.7f};
        float floorDb{-80.0f};
        bool  logFreq{true};
        bool  peakHold{false};

        // ── WaveIn state ──────────────────────────────────────────────────────
        HWAVEIN              hWaveIn{nullptr};
        WAVEHDR              waveHdr[kNumBufs]{};
        std::vector<int16_t> waveBufs[kNumBufs];
        bool                 capturing{false};
        char                 errMsg[256]{};

        // ── lock-free ring buffer (written by callback, read by render) ───────
        std::vector<float>  ring;
        std::atomic<size_t> ringWrite{0};
        size_t              lastProcessed{0};

        // ── FFT structures (only used from render thread) ─────────────────────
        // spscBuf  → cfg.buf  (SPSC ring; fft_exec reads audio samples FROM here)
        // fftInput → cfg.in_buf  (windowed input; fft_exec writes here, then FFTW reads)
        // fftOut   → cfg.out_buf (complex output; FFTW writes here)
        // fftMagBuf→ cfg.mag_buf (|complex[i]|; fft_exec writes here)
        fft_t                             fft{};
        bool                              fftInited{false};
        std::vector<float>                spscBuf;   // N floats — the SPSC input buffer
        std::vector<float>                fftInput;  // N floats — windowed input
        std::vector<std::array<float, 2>> fftOut;    // N/2+1 complex pairs (fftwf_complex layout)
        std::vector<float>                fftMagBuf; // N/2+1 magnitudes

        // ── results (mutex-protected) ─────────────────────────────────────────
        std::mutex         mtx;
        std::vector<float> freqs;
        std::vector<float> mags;     // smoothed dB
        std::vector<float> peakMags; // peak-hold dB
        float              peakFreq{0.0f};
        float              peakMagDb{-120.0f};

        // ── device names ──────────────────────────────────────────────────────
        std::vector<std::string> deviceNames;
        int                      numDevices{0};

        // ── helpers ───────────────────────────────────────────────────────────
        int fftSize() const { return kFftSizes[fftSizeIdx]; }
        int sampleRate() const { return sampleRateIdx == 0 ? 44100 : 48000; }

        void init()
        {
                ring.resize(kRingSize, 0.0f);
                refreshDevices();
        }

        void refreshDevices()
        {
                deviceNames.clear();
                numDevices = (int)waveInGetNumDevs();
                for (int i = 0; i < numDevices; i++) {
                        WAVEINCAPSW caps{};
                        if (waveInGetDevCapsW((UINT)i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                                // Convert UTF-16 to UTF-8 so ImGui renders it correctly
                                int len = WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, nullptr, 0, nullptr, nullptr);
                                std::string name(len, '\0');
                                WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, name.data(), len, nullptr, nullptr);
                                if (!name.empty() && name.back() == '\0')
                                        name.pop_back();
                                deviceNames.push_back(std::move(name));
                        } else {
                                deviceNames.push_back("Device " + std::to_string(i));
                        }
                }
                if (deviceIdx >= numDevices)
                        deviceIdx = 0;
        }

        bool start()
        {
                stop(); // tear down any existing capture + fft
                errMsg[0] = '\0';

                const int n  = fftSize();
                const int fs = sampleRate();

                // Allocate FFT buffers.
                // spscBuf  = cfg.buf  : SPSC ring that fft_exec reads audio from.
                // fftInput = cfg.in_buf : windowed working buffer (written by fft_exec).
                // fftOut   = cfg.out_buf: complex output from FFTW (written by fft_exec).
                // fftMagBuf= cfg.mag_buf: per-bin magnitude (written by fft_exec).
                spscBuf.assign(n, 0.0f);
                fftInput.assign(n, 0.0f);
                fftOut.resize(n / 2 + 1);
                fftMagBuf.assign(n / 2 + 1, 0.0f);

                fft_cfg_t cfg{};
                cfg.fs       = (f32)fs;
                cfg.npoints  = (usize)n;
                cfg.e_window = FFT_WINDOW_HANNING;
                cfg.buf      = spscBuf.data();                                   // SPSC input
                cfg.in_buf   = fftInput.data();                                  // windowed buf
                cfg.out_buf  = reinterpret_cast<fftwf_complex *>(fftOut.data()); // complex out
                cfg.mag_buf  = fftMagBuf.data();                                 // magnitude out
                fft_init(&fft, cfg);
                fftInited = true;

                // Frequency axis
                {
                        std::lock_guard<std::mutex> lk(mtx);
                        freqs.resize(n / 2 + 1);
                        for (int i = 0; i <= n / 2; i++)
                                freqs[i] = (float)i * fs / n;
                        mags.assign(n / 2 + 1, floorDb);
                        peakMags.assign(n / 2 + 1, floorDb);
                }

                // WaveIn double-buffers
                for (int b = 0; b < kNumBufs; b++)
                        waveBufs[b].resize(kBufSamples);

                // Open device
                WAVEFORMATEX wfx{};
                wfx.wFormatTag      = WAVE_FORMAT_PCM;
                wfx.nChannels       = 1;
                wfx.nSamplesPerSec  = (DWORD)fs;
                wfx.wBitsPerSample  = 16;
                wfx.nBlockAlign     = 2;
                wfx.nAvgBytesPerSec = (DWORD)(fs * 2);

                UINT     devId = (numDevices > 0) ? (UINT)deviceIdx : WAVE_MAPPER;
                MMRESULT res   = waveInOpen(&hWaveIn, devId, &wfx, (DWORD_PTR)waveInProc, (DWORD_PTR)this, CALLBACK_FUNCTION);
                if (res != MMSYSERR_NOERROR) {
                        waveInGetErrorTextA(res, errMsg, sizeof(errMsg));
                        hWaveIn = nullptr;
                        fft_destroy(&fft);
                        fftInited = false;
                        return false;
                }

                for (int b = 0; b < kNumBufs; b++) {
                        ZeroMemory(&waveHdr[b], sizeof(WAVEHDR));
                        waveHdr[b].lpData         = (LPSTR)waveBufs[b].data();
                        waveHdr[b].dwBufferLength = (DWORD)(kBufSamples * sizeof(int16_t));
                        waveInPrepareHeader(hWaveIn, &waveHdr[b], sizeof(WAVEHDR));
                        waveInAddBuffer(hWaveIn, &waveHdr[b], sizeof(WAVEHDR));
                }

                ringWrite.store(0, std::memory_order_relaxed);
                lastProcessed = 0;
                waveInStart(hWaveIn);
                capturing = true;
                return true;
        }

        void stop()
        {
                if (!capturing || !hWaveIn)
                        return;
                waveInStop(hWaveIn);
                waveInReset(hWaveIn);
                for (int b = 0; b < kNumBufs; b++)
                        waveInUnprepareHeader(hWaveIn, &waveHdr[b], sizeof(WAVEHDR));
                waveInClose(hWaveIn);
                hWaveIn   = nullptr;
                capturing = false;
                if (fftInited) {
                        fft_destroy(&fft);
                        fftInited = false;
                }
        }

        // Audio thread — copy captured samples into ring buffer.
        void onData(WAVEHDR *hdr)
        {
                int            n       = (int)(hdr->dwBytesRecorded / sizeof(int16_t));
                const int16_t *samples = (const int16_t *)hdr->lpData;
                size_t         w       = ringWrite.load(std::memory_order_relaxed);
                for (int i = 0; i < n; i++)
                        ring[(w + i) & (kRingSize - 1)] = samples[i] * (1.0f / 32768.0f);
                ringWrite.store(w + n, std::memory_order_release);
                if (hWaveIn) {
                        hdr->dwBytesRecorded = 0;
                        waveInAddBuffer(hWaveIn, hdr, sizeof(WAVEHDR));
                }
        }

        // Render thread — run FFT when enough new samples have arrived (75 % overlap).
        void processIfReady()
        {
                if (!fftInited)
                        return;
                const size_t w = ringWrite.load(std::memory_order_acquire);
                const int    n = fftSize();
                if (w < (size_t)n || w - lastProcessed < (size_t)(n / 4))
                        return;
                lastProcessed = w;

                // Copy latest n samples from ring buffer into spscBuf (= cfg.buf).
                // fft_exec will transfer them to fftInput (cfg.in_buf), apply the
                // Hanning window, run FFTW, and write results to fftMagBuf.
                for (int i = 0; i < n; i++)
                        spscBuf[i] = ring[(w - n + i) & (kRingSize - 1)];

                fft.lo.need_exec = 1;
                fft_exec(&fft);

                // fft_exec wrote raw |complex[i]| into fftMagBuf; normalise and dB-scale.
                const int   numBins = n / 2 + 1;
                const float norm    = (float)(n / 2);

                std::lock_guard<std::mutex> lk(mtx);
                float                       peakDb  = floorDb;
                int                         peakBin = 1;
                for (int i = 1; i < numBins; i++) {
                        float mag = fftMagBuf[i] / norm;
                        float db  = (mag > 1e-9f) ? 20.0f * log10f(mag) : floorDb;
                        db        = std::max(db, floorDb);

                        mags[i] = smoothing * mags[i] + (1.0f - smoothing) * db;

                        if (peakHold)
                                peakMags[i] = std::max(peakMags[i], mags[i]);
                        else
                                peakMags[i] = mags[i];

                        if (mags[i] > peakDb) {
                                peakDb  = mags[i];
                                peakBin = i;
                        }
                }
                peakFreq  = freqs.size() > (size_t)peakBin ? freqs[peakBin] : 0.0f;
                peakMagDb = peakDb;
        }

        static void CALLBACK waveInProc(HWAVEIN, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR)
        {
                if (uMsg == WIM_DATA)
                        reinterpret_cast<Impl *>(dwInstance)->onData(reinterpret_cast<WAVEHDR *>(dwParam1));
        }
};

// --------------------------------------------------------------------------

AudioFft::AudioFft() : impl_(std::make_unique<Impl>())
{
        impl_->init();
}
AudioFft::~AudioFft()
{
        impl_->stop();
}

void
AudioFft::draw()
{
        if (!show_)
                return;

        ImGui::SetNextWindowSize(ImVec2(720.0f, 520.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(tr("Audio FFT###AudioFFT", "音频FFT###AudioFFT"), &show_)) {
                ImGui::End();
                return;
        }

        auto &d = *impl_;

        // ── Device row ────────────────────────────────────────────────────────
        if (ImGui::Button(tr("Refresh##af", "刷新##af"))) {
                const bool wasCapturing = d.capturing;
                d.stop();
                d.refreshDevices();
                if (wasCapturing && d.numDevices > 0)
                        d.start();
        }
        ImGui::SameLine();

        if (d.numDevices == 0) {
                ImGui::TextDisabled(tr("No input devices found.", "未找到输入设备。"));
        } else {
                const char *curDev = (d.deviceIdx < (int)d.deviceNames.size()) ? d.deviceNames[d.deviceIdx].c_str() : "?";
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::BeginCombo("##afdev", curDev)) {
                        for (int i = 0; i < (int)d.deviceNames.size(); i++) {
                                const bool sel = (i == d.deviceIdx);
                                if (ImGui::Selectable(d.deviceNames[i].c_str(), sel)) {
                                        d.deviceIdx = i;
                                        if (d.capturing)
                                                d.start();
                                }
                                if (sel)
                                        ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Input device", "输入设备"));
        }

        ImGui::SameLine();
        static const char *rates[] = {"44100 Hz", "48000 Hz"};
        ImGui::SetNextItemWidth(92.0f);
        if (ImGui::Combo("##afsr", &d.sampleRateIdx, rates, 2)) {
                if (d.capturing)
                        d.start();
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Sample rate", "采样率"));

        ImGui::SameLine();
        static const char *fftLabels[] = {"512", "1024", "2048", "4096", "8192"};
        ImGui::SetNextItemWidth(68.0f);
        if (ImGui::Combo("##affft", &d.fftSizeIdx, fftLabels, kNumFftSizes)) {
                if (d.capturing)
                        d.start();
        }
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("FFT size (points)", "FFT点数"));

        ImGui::SameLine();
        if (!d.capturing) {
                if (ImGui::Button(tr("  Start  ", "  开始  ")) && d.numDevices > 0)
                        d.start();
                if (d.errMsg[0]) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", d.errMsg);
                }
        } else {
                if (ImGui::Button(tr("  Stop  ", "  停止  ")))
                        d.stop();
                ImGui::SameLine();
                {
                        std::lock_guard<std::mutex> lk(d.mtx);
                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "Peak: %.1f Hz  %.1f dB", d.peakFreq, d.peakMagDb);
                }
        }

        // ── Options row ───────────────────────────────────────────────────────
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat(tr("Smooth##af", "平滑##af"), &d.smoothing, 0.0f, 0.98f, "%.2f");
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Exponential smoothing (higher = slower response)", "指数平滑（越大响应越慢）"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::SliderFloat(tr("Floor##af", "下限##af"), &d.floorDb, -120.0f, -20.0f, "%.0f dB");
        ImGui::SameLine();
        ImGui::Checkbox(tr("Log X##af", "对数X##af"), &d.logFreq);
        ImGui::SameLine();
        const bool prevPeak = d.peakHold;
        ImGui::Checkbox(tr("Peak hold##af", "峰值保持##af"), &d.peakHold);
        if (prevPeak && !d.peakHold) {
                std::lock_guard<std::mutex> lk(d.mtx);
                std::fill(d.peakMags.begin(), d.peakMags.end(), d.floorDb);
        }
        if (d.peakHold) {
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Clear##af", "清除##af"))) {
                        std::lock_guard<std::mutex> lk(d.mtx);
                        std::fill(d.peakMags.begin(), d.peakMags.end(), d.floorDb);
                }
        }

        // ── Process new audio data ────────────────────────────────────────────
        if (d.capturing)
                d.processIfReady();

        // ── Spectrum plot ─────────────────────────────────────────────────────
        {
                std::lock_guard<std::mutex> lk(d.mtx);

                if (d.freqs.empty() || d.mags.empty()) {
                        ImGui::Spacing();
                        ImGui::TextDisabled(tr("Press Start to begin capture.", "按\"开始\"启动采集。"));
                } else {
                        const int startBin = d.logFreq ? 1 : 0;
                        const int nBins    = (int)d.freqs.size() - startBin;
                        if (nBins >= 2) {
                                const float fs      = (float)d.sampleRate();
                                const float minFreq = d.logFreq ? std::max(d.freqs[startBin], 20.0f) : 0.0f;
                                const float maxFreq = fs / 2.0f;

                                if (ImPlot::BeginPlot("##afplot", ImVec2(-1.0f, -1.0f))) {
                                        ImPlot::SetupAxis(ImAxis_X1, tr("Frequency (Hz)", "频率 (Hz)"));
                                        ImPlot::SetupAxis(ImAxis_Y1, tr("Magnitude (dB)", "幅度 (dB)"));
                                        ImPlot::SetupAxisLimits(ImAxis_X1, (double)minFreq, (double)maxFreq, ImGuiCond_Once);
                                        ImPlot::SetupAxisLimits(ImAxis_Y1, (double)d.floorDb, 0.0, ImGuiCond_Once);
                                        if (d.logFreq)
                                                ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);

                                        ImPlot::SetNextLineStyle(ImVec4(0.25f, 0.85f, 0.45f, 1.0f), 1.5f);
                                        ImPlot::PlotLine(tr("Magnitude", "幅度"),
                                                         d.freqs.data() + startBin,
                                                         d.mags.data() + startBin,
                                                         nBins);

                                        if (d.peakHold && !d.peakMags.empty()) {
                                                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.45f, 0.2f, 0.75f), 1.0f);
                                                ImPlot::PlotLine(tr("Peak Hold", "峰值保持"),
                                                                 d.freqs.data() + startBin,
                                                                 d.peakMags.data() + startBin,
                                                                 nBins);
                                        }

                                        ImPlot::EndPlot();
                                }
                        }
                }
        }

        ImGui::End();
}
