#pragma once
#include "core/dso_controller.hpp"
#include "gui/monitor.hpp"
#include <deque>
#include <future>
struct cJSON;

class DsoScope
{
      public:
        ~DsoScope();
        bool isOpen() const { return open_; }
        void setOpen(bool open);
        void draw();
        void drawEmbedded();
        void tick() { poll(); }
        void shutdown();
        void save(cJSON *root) const;
        void load(const cJSON *root);
        bool consumeModified()
        {
                bool v    = modified_;
                modified_ = false;
                return v;
        }

      private:
        friend struct DsoScopeTestAccess;
        bool                                 open_ = false, modified_ = false, connected_ = false, disconnectPending_ = false;
        bool                                 autoRead_ = false;
        int                                  interval_ = 500;
        double                               nextRead_ = 0;
        char                                 host_[256]{}, resource_[512]{}, command_[1024] = "*IDN?";
        dso::Config                          config_;
        dso::Controller                      controller_;
        std::future<dso::Result>             pending_;
        dso::Action                          active_ = dso::Action::Connect;
        std::string                          identity_, status_ = "未连接";
        int                                  error_ = 0;
        std::deque<std::string>              log_;
        std::array<std::array<double, 3>, 4> measurements_{};
        bool                                 measurementsValid_ = false;
        std::unique_ptr<Monitor>             monitor_;
        std::future<void>                    fft_;
        std::mutex                           monitorMutex_;
        bool                                 busy() const { return pending_.valid(); }
        void                                 submit(dso::Action action);
        void                                 poll();
        void                                 disconnect();
        void                                 publish(const dso::Result &result);
        void                                 ensureMonitor();
        void                                 controls();
        void                                 waveforms();
};
