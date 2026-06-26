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

// One entry in the sequence editor's execution log.
struct SeqLogEntry {
        std::string desc;  // e.g. "Write: target.voltage" or "SDK: Init" or "EXCEPTION"
        std::string value; // e.g. "= 3.3" or "OK: 0" or "FAIL: timeout" or ex.what()
        bool        ok{true};
        uint64_t    tsMs{0}; // ms since epoch (wall-clock, for display)
};

// Payload for dragging a function/method out of an SDK Debug window.
struct SdkDragPayload {
        int  panelWinId{-1};
        bool isCFunc{false};
        bool isPython{false};
        int  classIdx{-1};
        int  methodIdx{-1};
        int  objIdx{0};
        char pyName[128]{}; // Python function name (when isPython == true)
};

// SDK call step info.
struct SdkStepInfo {
        int                      panelWinId{-1};
        bool                     isCFunc{false};
        bool                     isPython{false};
        int                      classIdx{-1};
        int                      methodIdx{-1};
        int                      objIdx{0};
        std::vector<std::string> args;
        std::string              label;
        std::string              pyFuncName;      // Python function name (when isPython == true)
        char                     resultVar[32]{}; // LOCAL variable to receive return value (empty = discard)
};

// Kept for backward compatibility: SdkCall is no longer a user-creatable step
// kind. Old .seq files / sessions storing a standalone SdkCall step are converted
// on load into a "do" (Action) step containing a single SDK operation.
enum class SeqStepKind { Action, Sleep, If, While, For, Break, SdkCall };

// A single operation inside a "do" (Action) step's execution body (动作).
// The body is an ordered mix of variable writes and SDK function calls.
enum class SeqOpKind { Write, Sdk };

struct SeqOp {
        SeqOpKind      kind{SeqOpKind::Write};
        SequenceAction action{}; // valid when kind == Write
        SdkStepInfo    sdk{};    // valid when kind == Sdk
};

struct SequenceStep {
        std::string name;
        SeqStepKind kind{SeqStepKind::Action};
        u32         delayMs{0};

        std::vector<SeqOp> ops; // execution body for "do" steps (writes + SDK calls, in order)

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

// Struct field descriptor for LOCAL struct variables created by the sequence editor.
struct SeqStructField {
        std::string name;
        DataType    type{DataType::U32};
        uint32_t    byteOffset{0};
};

class SequenceEditor
{
      public:
        // Wired by Gui: read a LOCAL variable buffer for condition evaluation.
        std::function<void *(const std::string &)> onGetLocalBuf_;
        // Wired by Gui: return the DataType of a LOCAL variable (for type-aware write-back).
        std::function<DataType(const std::string &)> onGetLocalVarDataType_;
        // Wired by Gui: notify the variable manager that a LOCAL variable's buffer was written.
        std::function<void(const std::string &)> onLocalVarWritten_;
        // Wired by Gui: create a new LOCAL scalar variable in the variable manager.
        std::function<void(const std::string &, DataType, size_t)> onAddLocalVar_;
        // Wired by Gui: create a new LOCAL struct variable in the variable manager.
        std::function<void(const std::string &, const std::vector<SeqStructField> &, size_t)> onAddLocalStructVar_;

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
        void                      execSdkOp(const SdkStepInfo &sdk);
        void                      refreshSeqFiles();
        std::shared_ptr<SdkPanel> findSdkPanel(int winId) const;

        // UI helpers
        void drawStepList();
        void drawStepDetail(SequenceStep &step);
        void drawBodySteps(std::vector<SequenceStep> &steps, int depth);
        void drawSdkStepDetail(SdkStepInfo &sdk, std::shared_ptr<SdkPanel> panel);
        // Renders a struct-pointer SDK argument as an expandable row (type / tree / editable
        // fields), like the SDK caller. Binds the param to an auto-created LOCAL struct
        // variable on first expand. Returns true if it handled the row (caller should
        // `continue`); false when param `p` is not a struct (caller renders it normally).
        bool drawSdkStructArgRow(SdkPanel *panel, SdkStepInfo &sdk, int p, const std::string &pname, const std::string &ptype);
        void acceptSdkPayload(const void *data, int insertAfter);
        // Build an SDK op from a drag payload; false if the source panel is gone.
        bool makeSdkOp(const SdkDragPayload &pl, SeqOp &out);
        // Inside an active drag-drop target: consume any SDK/variable payload and
        // hand each resulting op to `sink`. Returns true if anything was added.
        bool          acceptOpDrops(const std::function<void(SeqOp)> &sink, bool noSdk = false);
        SequenceStep *selectedStepPtr();
        // Run a single top-level step (by index) in the background, independent of
        // the full-sequence run. No-op if a run is already in progress.
        void runSingleStep(int idx);
        // Reorder a top-level step from `src` to `dst` (drag-and-drop).
        void moveStep(int src, int dst);

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
        std::vector<SeqLogEntry> seqLog_;
        float                    seqLogHeight_{120.0f};
        bool                     seqLogCollapsed_{false};
        static constexpr int     kMaxLogEntries{500};

        std::vector<std::weak_ptr<SdkPanel>> sdkPanels_;
};

#endif // SEQUENCE_EDITOR_HPP
