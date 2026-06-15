#ifndef SEQUENCE_EDITOR_HPP
#define SEQUENCE_EDITOR_HPP

#include "cJSON.h"
#include "dwarf_parser.hpp"
#include "jlink_port.hpp"
#include "module.h"
#include "monitor_types.hpp"
#include "parser.hpp"
#include "shm.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class SdkPanel;

struct SequenceAction {
        std::string                              name;
        u64                                      addr;
        DataType                                 type;
        std::string                              port;
        std::string                              shmName;
        u64                                      typeOff;
        u32                                      bitOffset;
        u32                                      bitSize;
        std::string                              targetValue;
        bool                                     isEnum;
        std::vector<std::pair<i64, std::string>> enumDefs;
};

// Payload for dragging a function/method out of an SDK Debug window.
struct SdkDragPayload {
        int  panelWinId{-1};
        bool isCFunc{false};
        int  classIdx{-1};
        int  methodIdx{-1};
        int  objIdx{0};
};

// SDK call step info.
struct SdkStepInfo {
        int                      panelWinId{-1};
        bool                     isCFunc{false};
        int                      classIdx{-1};
        int                      methodIdx{-1};
        int                      objIdx{0};
        std::vector<std::string> args;
        std::string              label;
};

enum class SeqStepKind { Action, Sleep, If, While, For, Break, SdkCall };

struct SequenceStep {
        std::string name;
        SeqStepKind kind{SeqStepKind::Action};
        u32         delayMs{0};

        std::vector<SequenceAction> actions;   // for Action
        SdkStepInfo                 sdkCall{}; // for SdkCall

        int sleepMs{500}; // for Sleep

        char    condVar[64]{""}; // LOCAL var name (empty = always true)
        char    condOp[4]{"=="};
        int64_t condVal{0};

        char    forVar[32]{"i"};
        int64_t forFrom{0}, forTo{10}, forStep{1};

        std::vector<SequenceStep> body;
        std::vector<SequenceStep> elseBody;
        bool                      hasElse{false};
        bool                      expanded{true};
};

class SequenceEditor
{
      public:
        // Wired by Gui: read a LOCAL variable buffer for condition evaluation.
        std::function<void *(const std::string &)> onGetLocalBuf_;

        SequenceEditor();
        ~SequenceEditor();

        void draw();

        bool isOpen() const { return show_; }
        void setOpen(bool o) { show_ = o; }

        bool isModified() const { return isModified_; }
        void clearModified() { isModified_ = false; }

        std::vector<SequenceStep> &getSteps() { return steps_; }
        void                       setSteps(const std::vector<SequenceStep> &s)
        {
                steps_      = s;
                isModified_ = true;
        }

        void saveSession(cJSON *root);
        void loadSession(cJSON *root);
        void exportToFile(const std::string &path);
        void importFromFile(const std::string &path);

        void registerSdkPanel(std::weak_ptr<SdkPanel> sp);

      private:
        void                      writeAction(const SequenceAction &action);
        void                      refreshSeqFiles();
        std::shared_ptr<SdkPanel> findSdkPanel(int winId) const;

        // UI helpers
        void          drawStepList();
        void          drawStepDetail(SequenceStep &step);
        void          drawBodySteps(std::vector<SequenceStep> &steps, int depth);
        void          drawSdkStepDetail(SdkStepInfo &sdk, std::shared_ptr<SdkPanel> panel);
        void          acceptSdkPayload(const void *data, int insertAfter);
        SequenceStep *selectedStepPtr();

        // Background execution
        struct SeqCtx {
                SequenceEditor *editor;
        };
        enum class SExecResult { Ok, Break, Stop };
        SExecResult seqExecStep(const SequenceStep &step, SeqCtx &ctx);
        SExecResult seqExecSteps(const std::vector<SequenceStep> &steps, SeqCtx &ctx);
        bool        seqEvalCond(const SequenceStep &step);

        std::string              seqFolder_ = ".";
        std::vector<std::string> seqFiles_;
        int                      selectedSeqIdx_ = -1;

        std::vector<SequenceStep> steps_;
        int                       selectedStep_ = -1;

        char savePathBuf_[256] = "";
        bool show_             = false;
        bool isModified_       = false;

        // Background execution
        std::mutex               seqMtx_;
        std::thread              seqThread_;
        std::atomic<bool>        seqRunning_{false};
        std::atomic<bool>        seqStopReq_{false};
        std::atomic<bool>        seqDone_{false};
        std::vector<std::string> seqLog_;

        std::vector<std::weak_ptr<SdkPanel>> sdkPanels_;
};

#endif // SEQUENCE_EDITOR_HPP
