#ifndef SDK_PANEL_HPP
#define SDK_PANEL_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/c_header_parser.hpp"
#include "core/export_enum.hpp"
#include "core/sdk_loader.hpp"

// ─── Sequence step ────────────────────────────────────────────────────────────

enum class SdkStepKind { Call, Sleep, If, While, For, Break, Print };

struct SdkSeqStep {
        SdkStepKind kind{SdkStepKind::Call};
        char        label[64]{};

        // ── Call ──
        bool                     isCFunc{false}; // true = C function, false = C++ method
        int                      classIdx{-1};
        int                      methodIdx{-1};
        int                      objIdx{0};       // index into SdkPanel::objects_
        char                     resultVar[32]{}; // store numeric result here (e.g. "result")
        std::vector<std::string> args;

        // ── Condition (If / While) ──
        char    condVar[32]{};   // variable name (no $)
        char    condOp[4]{"=="}; // ==  !=  <  >  <=  >=
        int64_t condVal{0};

        // ── For: for(var = from; var < to; var += step) ──
        char    forVar[32]{};
        int64_t forFrom{0}, forTo{10}, forStep{1};

        // ── Sleep ──
        int sleepMs{200};

        // ── Print ──
        char message[256]{};

        // ── Nested body ──
        std::vector<SdkSeqStep> body;     // If-true / While / For body
        std::vector<SdkSeqStep> elseBody; // If-false

        // ── UI ──
        bool expanded{true};
        bool hasElse{false};
};

// ─── SdkPanel ─────────────────────────────────────────────────────────────────

class SdkPanel
{
      public:
        bool open_{true}; // false = window closed → Gui prunes it from the list

        // Set by Gui after construction to enable monitor/variable push.
        // Callback: (channelName, value, sessionTimeSec) — called under caller's lock.
        std::function<void(const std::string &, float, double)> onMonitorPush_;

        // Set by Gui to connect SDK pointer/reference args to Variable LOCAL buffers.
        // onGetVarBuf_("varname") → returns stable buffer ptr (pre-allocated size), or nullptr.
        std::function<void *(const std::string &)> onGetVarBuf_;
        // Called after SDK writes to the buffer so the Variable display can refresh.
        std::function<void(const std::string &)> onVarWritten_;
        // Returns names of all LOCAL variables across all Variable windows (for picker dropdown).
        std::function<std::vector<std::string>()> onListLocalVars_;

        void        draw();
        const char *title() const { return titleBuf_; }
        int         getWinId() const { return winId_; }

        // Called by Gui when OS-level file drops occur; panel applies them if hovered.
        void pushDroppedFiles(const std::vector<std::string> &paths)
        {
                for (const auto &p : paths)
                        pendingDropFiles_.push_back(p);
        }

        // Info helpers — used by SequenceEditor to populate dragged steps.
        std::string getCallLabel(int classIdx, int methodIdx) const;
        std::string getCFuncLabel(int fnIdx) const;
        int         getParamCount(int classIdx, int methodIdx) const;
        int         getCFuncParamCount(int fnIdx) const;
        bool        isParamPtrOrRef(int classIdx, int methodIdx, int paramIdx) const;
        bool        isCFuncParamPtrOrRef(int fnIdx, int paramIdx) const;
        std::string getParamName(int classIdx, int methodIdx, int paramIdx) const;
        std::string getCFuncParamName(int fnIdx, int paramIdx) const;
        std::string getParamRawType(int classIdx, int methodIdx, int paramIdx) const;
        std::string getCFuncParamRawType(int fnIdx, int paramIdx) const;

        // Object list (for SequenceEditor object-selector dropdown).
        struct ObjInfo {
                int         idx;
                std::string label;
                std::string className;
        };
        std::vector<ObjInfo> listObjects(int classIdx = -1) const;
        int                  newObject(int classIdx); // returns new obj idx, -1 on fail

        // Execute a single call synchronously (called from SequenceEditor run loop).
        struct DirectCallResult {
                bool        ok;
                std::string text;
        };
        DirectCallResult directCall(int classIdx, int methodIdx, int objIdx, const std::vector<std::string> &args);
        DirectCallResult directCallC(int fnIdx, const std::vector<std::string> &args);

        // Window identity — set once by Gui at creation time.
        void setWindowId(int id)
        {
                winId_ = id;
                snprintf(titleBuf_, sizeof(titleBuf_), "SDK Debug [%d]###SdkPanel%d", id, id);
        }

      private:
        // ── window identity ───────────────────────────────────────────────────────
        int  winId_{0};
        char titleBuf_[64]{"SDK Debug [0]###SdkPanel0"};

        // ── file paths ────────────────────────────────────────────────────────────
        char                     dllPath_[512]{};
        char                     headerPath_[512]{};
        std::vector<std::string> pendingDropFiles_;

        // ── loader + parse result ─────────────────────────────────────────────────
        SdkLoader   loader_;
        ParseResult parseResult_;

        // ── C functions tab ───────────────────────────────────────────────────────
        int selectedFnIdx_{-1};
        struct ArgBuf {
                char text[512]{};
        };
        std::vector<ArgBuf> fnArgBufs_;
        std::string         fnLastResult_;
        bool                fnLastResultOk_{false};

        // ── C++ classes tab ───────────────────────────────────────────────────────
        int   selectedClassIdx_{-1};
        int   selectedMethodIdx_{-1};
        int   selectedObjIdx_{-1};
        float clsSplitW_{220.0f}; // resizable left panel width

        std::vector<ArgBuf> methArgBufs_;

        struct StructBuf {
                std::vector<uint8_t> data;
                const CStructDecl   *decl{nullptr};
                bool                 expanded{false};
        };
        std::vector<StructBuf> structBufs_;

        struct ObjInstance {
                char        label[64]{};
                void       *ptr{nullptr};
                std::string className;
        };
        std::vector<ObjInstance> objects_;

        // ── history ───────────────────────────────────────────────────────────────
        struct HistEntry {
                std::string call, result;
                bool        ok{false};
                uint64_t    tsMs{0};    // get_mono_ts_ms()
                double      tsSec{0.0}; // sessionTimeSec() for monitor push
        };
        std::vector<HistEntry> history_;
        std::mutex             histMtx_; // protects history_ (background thread writes)

        // ── monitor push ─────────────────────────────────────────────────────────
        // Channel name → latest value (for display in the history header)
        std::unordered_map<std::string, float> pinnedChannels_;
        bool                                   monitorPushEnabled_{true};

        // ── sequence ──────────────────────────────────────────────────────────────
        std::vector<SdkSeqStep> seqSteps_;
        int                     seqSelectedStep_{-1};
        float                   seqSplitH_{300.0f}; // editor / log split

        // Execution state (accessed under seqMtx_)
        std::mutex                               seqMtx_;
        std::thread                              seqThread_;
        std::atomic<bool>                        seqRunning_{false};
        std::atomic<bool>                        seqStopReq_{false};
        std::vector<std::string>                 seqLog_;
        std::unordered_map<std::string, int64_t> seqVars_;

        // ── status bar ────────────────────────────────────────────────────────────
        std::string statusMsg_;
        bool        statusIsErr_{false};

        // ── helpers ───────────────────────────────────────────────────────────────
        void doLoadDll();
        void doParseHeader();
        void doCallC();
        void doCallMethod();
        void doNewObject(int classIdx);
        void doDeleteObject(int idx);
        void selectFn(int idx);
        void selectMethod(int classIdx, int methodIdx);
        void setStatus(const std::string &msg, bool isErr);
        void pushHistory(const std::string &call, const std::string &result, bool ok, double tsSec);
        void pushToMonitor(const std::string &chanKey, float val, double tsSec);

        const CEnumDecl   *findParamEnum(const CParam &p) const;
        const CStructDecl *findParamStruct(const CParam &p) const;

        void drawCFunctionsTab();
        void drawCppClassesTab();

        // Sequence execution helpers (run inside seqThread_)
        struct SeqCtx {
                std::unordered_map<std::string, int64_t> vars;
                bool                                     stopReq{false};
                SdkPanel                                *panel{nullptr};
        };
        enum class StepResult { Ok, Break, Stop };
        StepResult execStep(const SdkSeqStep &step, SeqCtx &ctx);
        StepResult execSteps(const std::vector<SdkSeqStep> &steps, SeqCtx &ctx);
        void drawStepList(std::vector<SdkSeqStep> &steps, int depth, std::vector<SdkSeqStep> **insertParent, int *insertAfter);
        void drawStepEditor(SdkSeqStep &step, int depth);
        static bool evalCond(const SdkSeqStep &step, const std::unordered_map<std::string, int64_t> &vars);
};

#endif // SDK_PANEL_HPP
