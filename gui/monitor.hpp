#ifndef MONITOR_HPP
#define MONITOR_HPP

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "module.h"

class MonitorChannel
{
      public:
        enum class DeviceEnum {
                LOCAL,
                FSA,
        };

      private:
        std::string name_{};
        std::string type_{};
        usize       addr_{};
        f32         rVal_{}, wVal_{};
        std::string device_{};

        std::vector<f32> rVals_{}, wVals_{};

        shm_t shm_{};

      public:
        explicit MonitorChannel(std::string chName) : name_(std::move(chName)) {}
        MonitorChannel()  = default;
        ~MonitorChannel() = default;

        std::string &getName() { return name_; }
        void         setName(const std::string &name) { name_ = name; }
        std::string &getType() { return type_; }
        void         setType(const std::string &type) { type_ = type; }
        usize       &getAddr() { return addr_; }
        void         setAddr(const usize addr) { addr_ = addr; }
        std::string &getDevice() { return device_; }
        void         setDevice(const std::string &device) { device_ = device; }
        shm_t       &getShm() { return shm_; }

        f32 &getRVal() { return rVal_; }
        void setRVal(const f32 val)
        {
                rVal_ = val;
                rVals_.push_back(val);
        }

        f32 &getWVal() { return wVal_; }
        void setWVal(const f32 val)
        {
                wVal_ = val;
                wVals_.push_back(val);
        }

        std::vector<f32> getRVals() { return rVals_; }
        std::vector<f32> getWVals() { return wVals_; }
};

class MonitorScope
{
      public:
        enum class DrawEnum {
                PLOT,
                TABLE,
        };
        using ChannelMapType = std::unordered_map<std::string, std::unique_ptr<MonitorChannel>>;

      private:
        std::string    name_{};
        ChannelMapType chs_{};
        DrawEnum       e_draw{};

        void tableDraw();
        void tableMenu();

        void plotDraw() const;
        void plotMenu();

        static void shmInit(MonitorChannel &ch);
        void        drawTableRow(const std::string &chName, std::unique_ptr<MonitorChannel> &ch);

      public:
        explicit MonitorScope(std::string scopeName) : name_(std::move(scopeName)) {}
        MonitorScope()  = default;
        ~MonitorScope() = default;

        void menu();
        void draw();

        int addChannel(const std::string &chName);
        int setValue(const std::string &chName, f32 val);

        std::string     getName() { return name_; }
        ChannelMapType &getChannels() { return chs_; }
        MonitorChannel *findChannel(const std::string &chName);
};

class Monitor
{
      public:
        using ScopeMapType = std::unordered_map<std::string, std::unique_ptr<MonitorScope>>;

      private:
        std::string      name_{};
        std::vector<f32> timestamps_{};
        bool             paused_{false};
        ScopeMapType     scopes_{};

        void menu();

      public:
        explicit Monitor(std::string monitorName) : name_(std::move(monitorName)) { print_info(true, "Monitor()"); }
        Monitor() { print_info(true, "Monitor()"); };
        ~Monitor() { print_info(true, "~Monitor()"); };

        void updateDisplay();

        std::string     getName() { return name_; }
        int             addScope(const std::string &scopeName);
        ScopeMapType   &getScopes() { return scopes_; }
        MonitorChannel *findChannel(const std::string &scopeName, const std::string &chName);
};

#endif // !MONITOR_HPP
