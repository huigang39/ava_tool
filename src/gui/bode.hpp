#ifndef BODE_HPP
#define BODE_HPP

#include <string>
#include <vector>

#include "module.h"
#include "timeops.h"

class MonitorChannel;
class Monitor;

class Bode
{
      public:
        bool show_{false};
        void updateDisplay();
        void saveSession(void *root) const;
        void loadSession(const void *root);
        bool isModified() const { return modified_; }
        void clearModified() { modified_ = false; }

      private:
        // Sweep: drive a live swept-sine and measure H per step.
        // FromData: offline FFT of the data already displayed in the plots.
        enum class Mode { Sweep, FromData };

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
        bool                   modified_{false};

        // ---- From-data (offline FFT) mode state ----
        Mode        bodeMode_{Mode::Sweep};
        float       bodeOfflineThreshPct_{2.0f}; // keep only bins where |input| >= peak * pct%
        std::string bodeOfflineStatus_{};

        void            generateBodeFreqs_();
        MonitorChannel *findChannelByKey_(const char *key);
        Monitor        *findMonitorByKey_(const char *key);
        void            draw_();
        void            advanceSweep_();
        void            computeFromData_();
};

#endif // !BODE_HPP
