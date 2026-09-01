#pragma once
#include "core/safety.hpp"
#include "gui/hover_reveal.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
class Variable;
struct cJSON;

// 保留原类名和会话键, 兼容电机页面入口及旧会话
class FaultDecoder
{
      public:
        bool open       = false;
        using Variables = std::unordered_map<std::string, std::shared_ptr<Variable>>;
        void draw(const Variables &variables = {});
        void openOverview() { open = overview_ = focus_ = modeSelect_ = true; }
        void inspect(unsigned wirePage, uint32_t val, unsigned bit);
        bool bind(const Variables &, const std::string &manager, const std::string &name);
        bool refresh(const Variables &);
        bool bindGroup(const Variables &, unsigned group, const std::string &manager, const std::string &root);
        void detectGroups(const Variables &);
        void refreshGroups(const Variables &);
        bool loadProfile(const std::string &path);
        void save(cJSON *root) const;
        void load(const cJSON *root);
        friend struct FaultDecoderTestAccess;

      private:
        struct Binding {
                std::string        manager, name, status;
                diagnostics::Value val;
                bool               valid = false, hasValue = false;
                bool               requested = true; // 上一帧展开且符合文本和类型筛选
        };
        struct GroupRow {
                diagnostics::Field field;
                diagnostics::Value val;
                std::string        title, decoder, note, unit, status;
                bool               valid = false, hasValue = false;
                bool               requested = true; // 上一帧展开且符合文本和类型筛选
        };
        struct Group {
                std::string           id, manager, root, title, note, status;
                uint64_t              layoutGeneration = 0;
                bool                  available        = false;
                std::vector<GroupRow> rows;
        };
        struct RootBinding {
                std::string id, manager, root;
        };
        safety::Profile profile_;
        ui::HoverReveal profileHover_;
        void            drawProfileTools();
        bool            modeSelect_ = true, groupTabSelect_ = true;
        std::string     groupTab_;
        bool            overview_ = true, focus_ = false, groupActiveOnly_ = false, diagnosticsOnly_ = false;
        std::string profilePath_, profileDirectory_, profileError_, directoryError_, decoderId_, bindingError_, selectedRow_;
        std::vector<std::string> profileFiles_;
        char                     profileInput_[1024]{}, objectSearch_[128]{}, fieldSearch_[128]{}, manualInput_[128]{};
        char                     variableSearch_[128]{};
        bool                     searchScroll_  = false;
        double                   nextDetection_ = 0;
        std::unordered_map<std::string, Binding> bindings_;
        std::vector<RootBinding>                 roots_;
        std::vector<Group>                       groups_;
        void                                     scanProfileDirectory();
        void                                     rebuildGroup(Group &, Variable &, const safety::Group &);
        void                                     drawGroups(const Variables &);
        void                                     drawManual(const Variables &);
        std::string                              groupReport(const Group &, const GroupRow *only = nullptr) const;
};
inline FaultDecoder &
faultDecoder()
{
        static FaultDecoder decoder;
        return decoder;
}
