#pragma once
#include "core/spice_runner.hpp"
#include "gui/monitor_scope.hpp"
class InverterSimulator
{
      public:
        InverterSimulator();
        bool show_ = false;
        void draw();
        void saveSession(void *root) const;
        void loadSession(const void *root);
        bool isModified() const { return modified_; }
        void clearModified() { modified_ = false; }

      private:
        friend struct InverterSimulatorTestAccess;
        ava_spice::Config             draft_, applied_, pending_, previousConfig_;
        ava_spice::Runner             runner_;
        ava_spice::Result             result_, previous_, lastAttempt_;
        std::vector<ava_spice::Model> models_;
        bool                          modified_ = false, showLog_ = false;
        bool                          compare_ = true;
        double                        minUs_ = 0, maxUs_ = 6;
        std::string                   error_;
        std::unique_ptr<MonitorScope> scopes_[5];
        MonitorViewMode               viewMode_ = MonitorViewMode::FULL;
        std::vector<std::string>      probes_{"a", "b", "c", "i_a", "i_b", "i_c"};
        bool                          scopesThreePhase_ = false;
        std::string                   probePart_;
        int                           probePreset_ = 0;
        void                          start();
        void                          acceptResult(ava_spice::Result result);
        void                          drawParameters();
        void                          drawWaveforms();
        void                          comparison();
        void                          syncScopes();
        void                          syncProbeScopes();
        void                          schematic();
        void                          toggleProbe(const std::string &key);
        void                          applyProbePreset(int preset);
        void                          plot(const char *title, int kind);
        void                          exportCsv();
};
