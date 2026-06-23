#ifndef AUDIO_FFT_HPP
#define AUDIO_FFT_HPP

#include <memory>

// Real-time audio input FFT analyser window.
// Uses Windows WaveIn (WinMM) for capture and FFTW3 for spectrum computation.
class AudioFft
{
      public:
        bool show_{false};

        AudioFft();
        ~AudioFft();

        void draw();

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
};

#endif // AUDIO_FFT_HPP
