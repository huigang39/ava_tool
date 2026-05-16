#ifndef BODE_HPP
#define BODE_HPP

#include <string>
#include <vector>

#include "module.h"
#include "timeops.h"

class MonitorChannel;

class Bode
{
      public:
        bool show_{false};
        void updateDisplay();

      private:
        struct BodePoint {
                f64 freq;
                f64 magDb;
                f64 phaseDeg;
        };
        struct BodeCurveStyle {
                f32  color[4]{1.0f, 1.0f, 1.0f, 1.0f};
                bool useAutoColor{true};
                f32  lineWeight{1.5f};
                bool showMarkers{false};
        };

        char                   bodeWriteKey_[256]{};
        char                   bodeInputKey_[256]{};
        char                   bodeOutputKey_[256]{};
        float                  bodeFStart_{1.0f};
        float                  bodeFStop_{1000.0f};
        float                  bodeFStep_{10.0f};
        float                  bodeDwellSec_{1.0f};
        float                  bodeAmp_{1.0f};
        bool                   bodeSweepRunning_{false};
        int                    bodeSweepFreqIdx_{0};
        u64                    bodeSweepStepStart_{0};
        std::vector<f64>       bodeFreqList_{};
        std::vector<BodePoint> bodeData_{};
        std::vector<f64>       bodeFreqsV_{};
        std::vector<f64>       bodeMagsV_{};
        std::vector<f64>       bodePhsV_{};
        BodeCurveStyle         bodeMagStyle_{};
        BodeCurveStyle         bodePhsStyle_{};
        bool                   bodeStyleInit_{false};

        void            generateBodeFreqs_();
        MonitorChannel *findChannelByKey_(const char *key);
        void            draw_();
        void            advanceSweep_();
};

#endif // !BODE_HPP
