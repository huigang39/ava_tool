#ifndef BKP_SRAM_DEBUGGER_HPP
#define BKP_SRAM_DEBUGGER_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gui/monitor_types.hpp"
#include "gui/time_series_buffer.hpp"

class BkpSramDebugger
{
      public:
        BkpSramDebugger();
        ~BkpSramDebugger();

        BkpSramDebugger(const BkpSramDebugger &)            = delete;
        BkpSramDebugger &operator=(const BkpSramDebugger &) = delete;

        void draw();
        void open();
        void resetSession();
        void saveSession(void *root) const;
        void loadSession(const void *root);
        bool isModified() const { return modified_.load(std::memory_order_acquire); }
        void clearModified() { modified_.store(false, std::memory_order_release); }
        void setSymbolResolver(std::function<bool(const std::string &, std::uint32_t &)> resolver);
        void requestClearData() { clearData(); }

      private:
        static constexpr std::uint32_t kDataPoolSize = 0x1000u;
        static constexpr std::uint32_t kSampleStride = 8u;
        static constexpr std::size_t   kMaxChannels  = 16u;
        enum class ValueType : std::uint32_t { U8 = 0, I8 = 1, U16 = 2, I16 = 3, U32 = 4, I32 = 5, F32 = 6 };
        enum class State { Stopped, Starting, Running, Stopping, Error };

        struct ChannelConfig {
                std::string   name{};
                std::uint32_t address{0};
                ValueType     type{ValueType::F32};
                std::uint32_t slot{0};
                char          addressText[16]{"0x00000000"};
                float         color[4]{0.4f, 0.7f, 1.0f, 1.0f};
                bool          useAutoColor{true};
                float         lineWeight{1.5f};
                bool          showMarkers{false};
                bool          show{true};
        };
        struct Point {
                std::uint32_t seq{0};
                std::uint32_t tick{0};
                std::uint32_t raw{0};
                double        plotTick{0.0};
                double        value{0.0};
                double        hostTimeSec{0.0};
        };
        struct ChannelRuntime {
                std::uint32_t      generation{0};
                std::uint32_t      cursor{0};
                std::uint32_t      writeSeq{0};
                std::uint32_t      expectedSeq{0};
                std::uint32_t      observedSeq{0};
                std::uint32_t      address{0};
                std::uint32_t      capacity{0};
                std::uint32_t      overwrites{0};
                std::uint64_t      droppedSamples{0};
                std::uint64_t      validatedSamples{0};
                bool               generationKnown{false};
                bool               preserveHistoryOnReacquire{false};
                bool               plotTickKnown{false};
                std::uint32_t      lastRawTick{0};
                double             plotTickOffset{0.0};
                bool               active{false};
                ValueType          displayType{ValueType::F32};
                std::vector<Point> points{};
                TimeSeriesBuffer   display{};
        };
        struct ProtocolLayout {
                std::uint32_t control{0};
                std::uint32_t data{0};
                std::uint32_t seq{0};
                std::uint32_t ack{0};
                std::uint32_t enableMask{0};
                std::uint32_t sampleDiv{0};
                std::uint32_t status{0};
                std::uint32_t errorChannel{0};
                std::uint32_t channelBase{0};
                std::uint32_t channelStride{8};
                std::uint32_t channelAddressOffset{0};
                std::uint32_t channelTypeOffset{4};
                std::uint32_t stateBase{0};
                std::uint32_t dataSize{0};
                std::uint32_t stateStride{32};
                std::uint32_t stateActiveOffset{8};
                std::uint32_t stateAddressOffset{0};
                std::uint32_t stateTypeOffset{4};
                std::uint32_t stateBufferOffset{12};
                std::uint32_t stateCapacityOffset{16};
                std::uint32_t stateGenerationOffset{20};
                std::uint32_t stateWriteSeqOffset{24};
                std::uint32_t stateOverwritesOffset{28};
                bool          valid() const { return control != 0u && data != 0u && dataSize != 0u; }
        };

        mutable std::mutex                                        configMtx_{};
        mutable std::mutex                                        dataMtx_{};
        mutable std::mutex                                        statusMtx_{};
        std::vector<ChannelConfig>                                channels_{};
        std::array<ChannelRuntime, kMaxChannels>                  runtime_{};
        std::array<std::vector<Point>, kMaxChannels>              plotCache_{};
        std::thread                                               worker_{};
        std::atomic<bool>                                         stopRequested_{false};
        std::atomic<bool>                                         restartRequested_{false};
        std::atomic<bool>                                         modified_{false};
        std::atomic<State>                                        state_{State::Stopped};
        std::uint32_t                                             sampleDiv_{1};
        std::atomic<int>                                          pollIntervalMs_{5};
        std::atomic<float>                                        historySeconds_{0.0f};
        std::uint32_t                                             maxDisplayPoints_{5000u};
        std::atomic<float>                                        actualSampleHz_{0.0f};
        std::atomic<float>                                        actualReadHz_{0.0f};
        bool                                                      windowOpen_{false};
        bool                                                      focusRequested_{false};
        MonitorViewMode                                           viewMode_{MonitorViewMode::FOLLOW};
        double                                                    plotViewMin_{0.0};
        double                                                    plotViewMax_{1.0};
        double                                                    plotFollowSpan_{1000.0};
        float                                                     plotPaneHeight_{320.0f};
        float                                                     plotPaneRatio_{0.65f};
        bool                                                      configExpanded_{true};
        double                                                    plotCacheUpdateTime_{0.0};
        std::string                                               statusText_{};
        std::function<bool(const std::string &, std::uint32_t &)> symbolResolver_{};

        void start();
        void stop();
        void workerLoop(std::vector<ChannelConfig> config, std::uint32_t sampleDiv, ProtocolLayout layout);
        bool submitConfig(const ProtocolLayout             &layout,
                          const std::vector<ChannelConfig> &config,
                          std::uint32_t                     sampleDiv,
                          bool                              enable);
        bool readChannel(const ProtocolLayout &layout, std::size_t slot, ValueType configuredType, std::uint32_t sampleDiv);
        bool resolveProtocolLayout(ProtocolLayout &layout) const;
        void setError(const std::string &message);
        void setStatus(const std::string &message);
        std::string statusSnapshot() const;
        void        clearData();
        void        readRetainedHistory();
        void        exportCsv();
        bool
        assignDroppedChannel(std::size_t index, const char *name, std::uint64_t address, const char *type, const char *device);
        bool               appendDroppedChannel(const char *name, std::uint64_t address, const char *type, const char *device);
        void               acceptNewChannelDrop();
        static double      decodeValue(std::uint32_t raw, ValueType type);
        static const char *typeName(ValueType type);
        static bool        parseValueType(const char *text, ValueType &type);
};

#endif
