#pragma once

#include "core/motor_controller.hpp"
#include "gui/motor_gauge_range.hpp"
#include "gui/motor_units.hpp"
#include "gui/position_dial.hpp"
#include "gui/wave_input.hpp"
#include <future>
#include <optional>
struct cJSON;
class Monitor;
class MonitorChannel;

class MotorControl
{
        friend struct MotorControlTestAccess;

      public:
        MotorControl();
        ~MotorControl();
        void                                              draw();
        std::function<std::vector<motor_jlink::Symbol>()> onJlinkSymbols;
        bool                                              isOpen() const { return open_; }
        bool                                              consumeElevationRequest()
        {
                const bool requested = elevationRequested_;
                elevationRequested_  = false;
                return requested;
        }
        void setOpen(bool open);
        void shutdown() { controller_.shutdown(); }
        void save(cJSON *root) const;
        void load(const cJSON *root);
        bool consumeModified()
        {
                bool changed = modified_;
                modified_    = false;
                return changed;
        }

      private:
        static bool                             npcapAvailable();
        void                                    npcapDialog();
        bool                                    npcapPrompt_           = false;
        bool                                    elevationRequested_    = false;
        uint64_t                                seenElevationRevision_ = 0;
        void                                    connection(const MotorController::State &state);
        void                                    jlinkSettings();
        void                                    refreshJlinkSymbols(bool applyPreset);
        bool                                    resolveJlinkConfig();
        motor_jlink::Config                     jlinkConfig_;
        std::vector<motor_jlink::Symbol>        jlinkSymbols_;
        std::string                             jlinkSource_, jlinkMappingError_;
        char                                    jlinkFilter_[128]{};
        void                                    controls(const MotorController::State &state);
        void                                    positionDial(const MotorController::State &state, int mode);
        void                                    dashboard(const MotorController::State &state);
        void                                    gaugeRangeEditor(int index);
        int                                     selectedMode() const;
        void                                    targetField(const MotorController::State &state, int channel);
        void                                    setTargetEditor(const AvaEthTarget &target);
        void                                    variables(const MotorController::State &state);
        void                                    interpolationSettings(const MotorController::State &state);
        void                                    requestConfirmation(MotorController::Action action, int newMode = -1);
        void                                    confirmation(const MotorController::State &state);
        void                                    cancelConfirmation();
        void                                    diagnostics(const MotorController::State &state);
        void                                    pidParameters(const MotorController::State &state);
        AvaEthPidParameters                     pidDraft_{};
        uint32_t                                pidSelection_ = 0;
        std::string                             pidDraftContext_;
        uint64_t                                seenPidRevision_ = 0;
        std::optional<MotorController::Command> pendingPid_;
        void                                    faultDetails(const MotorController::State &state);
        void                                    command(MotorController::Action action);
        MotorController::Config                 config() const;
        void                                    findLibrary();
        void                                    disconnect(const char *reason);

        std::unique_ptr<Monitor>                        monitor_;
        std::array<std::shared_ptr<MonitorChannel>, 12> monitorChannels_{};
        bool                                            monitorConnected_ = false;
        std::mutex                                      monitorMutex_;
        std::future<void>                               monitorFft_;
        double                                          lastMonitorSample_ = -1;
        int                                             monitorAxis_       = -1;
        std::optional<MotorController::Command>         pendingCommand_;
        bool                                            openConfirmation_ = false;
        int                                             previousMode_     = 7;
        std::array<float, 4>                            previousTargets_{};
        std::array<WaveInputDraft, 4>                   targetDrafts_{};
        MotorController                                 controller_;
        bool                           open_ = false, modified_ = false, initialized_ = false, previouslyConnected_ = false;
        bool                           faultDetailsActive_ = false;
        char                           library_[1024]{}, device_[1024]{};
        std::string                    deviceName_;
        std::array<MotorGaugeRange, 9> gaugeRanges_ = defaultMotorGaugeRanges;
        MotorGaugeRange                rangeDraft_{0, 1};
        int                            positionUnit_ = 0, velocityUnit_ = 0;
        int                            monitorPositionUnit_ = -1, monitorVelocityUnit_ = -1;
        bool                           useBuiltin_{true};

        int                                    backend_ = 3, period_ = 1000, watchdog_ = 300, axis_ = 0, mode_ = 7;
        int                                    rightPageRequest_ = -1;
        bool                                   relaxed_          = true;
        float                                  position_ = 0, velocity_ = 0, current_ = 0, torque_ = 0;
        std::array<uint64_t, AVA_ETH_MAX_AXES> seenModeRevisions_{};
        uint32_t                               targetInputRevision_ = 0;
        bool                                   interpolation_       = true;
        float                                  maxPositionVelocity_ = 1, maxPositionAcceleration_ = 2;
        PositionDialDrag                       dialDrag_;
        bool                                   dialDragging_ = false, dialPending_ = false;
        double                                 dialLastSend_ = 0;
        float                                  dialCenterX_ = 0, dialCenterY_ = 0, dialRadius_ = 0;
};
