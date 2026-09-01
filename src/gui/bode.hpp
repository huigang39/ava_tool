#ifndef BODE_HPP
#define BODE_HPP

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ckit.h"
#include "core/bode_analysis.hpp"
#include "timeops.h"

class MonitorChannel;
class Monitor;
class Variable;

class Bode
{
        friend struct BodeTestAccess;

      public:
        bool show_{false};
        void updateDisplay(const std::unordered_map<std::string, std::shared_ptr<Variable>> &vars);
        void saveSession(void *root) const;
        void loadSession(const void *root);
        bool isModified() const { return modified_; }
        void clearModified() { modified_ = false; }

      private:
        // Sweep: drive a live swept-sine and measure H per step
        // FromData: offline FFT of the data already displayed in the plots
        // FromVariable: read independently selected count/frequency/gain/phase variables
        enum class Mode { Sweep, FromData, FromVariable };

        using BodePoint = bode_analysis::Point;
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

        // ---- From-variable mode state ----
        char                  bodeCountPath_[256]{};
        char                  bodeFreqPath_[256]{};
        char                  bodeGainPath_[256]{};
        char                  bodePhasePath_[256]{};
        std::string           bodeVarStatus_{};
        bool                  bodeFitPending_{false};
        bool                  bodeMeasureEnabled_{false};
        int                   bodeMeasureCount_{0};
        double                bodeMeasureFrequency_[2]{};
        int                   bodeAnalysisMode_{0};
        bool                  bodeAnalysisDirty_{true};
        bode_analysis::Result bodeAnalysis_;

        void drawMeasurement_(bool magnitude);
        void drawAnalysis_();

        void            generateBodeFreqs_();
        MonitorChannel *findChannelByKey_(const char *key);
        Monitor        *findMonitorByKey_(const char *key);
        void            draw_(const std::unordered_map<std::string, std::shared_ptr<Variable>> &vars);
        void            advanceSweep_();
        void            computeFromData_();
        void            readFromVariable_(const std::unordered_map<std::string, std::shared_ptr<Variable>> &vars);
};

#endif // !BODE_HPP
