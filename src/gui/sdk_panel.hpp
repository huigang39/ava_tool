#ifndef SDK_PANEL_HPP
#define SDK_PANEL_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/c_header_parser.hpp"
#include "core/export_enum.hpp"
#include "core/sdk_loader.hpp"

// Render a struct's fields as editable sub-rows of the *current* 3-column ImGui
// table (column 1 = ".field  type", column 2 = value input), writing edits into
// `buf` at each field's offset. `idSalt` disambiguates widget IDs across params.
// Shared by the SDK caller and the sequence editor so both look identical.
// Returns true if any field value was edited this frame.
bool drawSdkStructFieldRows(const CStructDecl &sd, uint8_t *buf, size_t bufSize, int idSalt);

// ─── Python parse result ──────────────────────────────────────────────────────

struct PyParam {
        std::string name;
        std::string defaultVal;
};

struct PyFuncDecl {
        std::string          name;
        std::vector<PyParam> params;
};

struct PyClassDecl {
        std::string             name;
        std::vector<PyFuncDecl> methods;
};

struct PyParseResult {
        std::vector<PyFuncDecl>  functions;
        std::vector<PyClassDecl> classes;
};

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

        // Set by Gui to write a numeric value into an EXISTING LOCAL variable (type-aware,
        // never creates). Used to store a call's return value into the user-chosen result
        // variable. No-op if no LOCAL variable with that name exists.
        std::function<void(const std::string &name, double value, bool isFloat)> onWriteLocalScalar_;

        // Set by Gui to create a LOCAL variable in the variable manager (the "+" buttons).
        // For a struct type pass structDecl != nullptr; otherwise scalarType is used.
        std::function<void(const std::string &name, CType scalarType, const CStructDecl *structDecl)> onCreateLocalVar_;

        // Set by Gui to read the current value of a LOCAL variable (for "$var" value args).
        // Returns false if no such LOCAL variable exists.
        std::function<bool(const std::string &name, double &out)> onReadLocalVar_;

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
        bool        isModified() const { return modified_; }
        void        clearModified() { modified_ = false; }

        // Called by Gui when OS-level file drops occur; panel applies them if hovered.
        void pushDroppedFiles(const std::vector<std::string> &paths)
        {
                for (const auto &p : paths)
                        pendingDropFiles_.push_back(p);
        }

        // Info helpers — used by SequenceEditor to populate dragged steps.
        std::string getClassName(int classIdx) const;
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
        // Raw return type string (e.g. "int", "float*"). Empty string for void/Python.
        std::string
        getCallReturnType(bool isCFunc, bool isPython, int classIdx, int methodIdx, const std::string &pyFuncName = "") const;
        // Returns the CEnumDecl for a parameter if its type is an enum, nullptr otherwise.
        const CEnumDecl *getParamEnumDecl(bool isCFunc, int classIdx, int methodIdx, int paramIdx) const;
        // Returns the CStructDecl for the pointed-to type of a parameter, or nullptr.
        const CStructDecl *getParamStructDecl(bool isCFunc, int classIdx, int methodIdx, int paramIdx) const;

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
                int64_t     rawValue{0};  // raw bits of return value (memcpy-safe for int/float)
                double      rawDouble{0}; // parsed as double (valid for Python string results)
        };
        DirectCallResult directCall(int classIdx, int methodIdx, int objIdx, const std::vector<std::string> &args);
        DirectCallResult directCallC(int fnIdx, const std::vector<std::string> &args);
        DirectCallResult directCallPy(const std::string &funcName, const std::vector<std::string> &args);

        // Python info helpers — used by SequenceEditor to populate dragged steps.
        int         getPyFuncParamCount(const std::string &funcName) const;
        std::string getPyFuncParamName(const std::string &funcName, int paramIdx) const;
        std::string getPyFuncLabel(const std::string &funcName) const;

        // Session persistence — node is cJSON*, baseDir is the .ava file's directory.
        void save(void *node, const std::string &baseDir) const;
        void load(const void *node, const std::string &baseDir);

        SdkPanel(); // defined in sdk_panel.cpp (PyRunner is incomplete in header)
        ~SdkPanel();

        // Window identity — set once by Gui at creation time.
        void setWindowId(int id)
        {
                winId_ = id;
                snprintf(userLabel_, sizeof(userLabel_), "SDK 调用器 [%d]", id);
                snprintf(titleBuf_, sizeof(titleBuf_), "%s###SdkPanel%d", userLabel_, id);
        }

      private:
        // ── window identity ───────────────────────────────────────────────────────
        int  winId_{0};
        char userLabel_[64]{"SDK 调用器 [0]"};
        char renameBuf_[64]{};
        char titleBuf_[64]{"SDK 调用器 [0]###SdkPanel0"};

        // ── file paths ────────────────────────────────────────────────────────────
        char                     dllPath_[512]{};
        char                     headerPath_[512]{};
        std::vector<std::string> pendingDropFiles_;

        // ── loader + parse result ─────────────────────────────────────────────────
        SdkLoader   loader_;
        ParseResult parseResult_;

        // ── C functions tab ───────────────────────────────────────────────────────
        int  selectedFnIdx_{-1};
        char fnResultVar_[64]{}; // LOCAL variable to store C function return value
        struct ArgBuf {
                char text[512]{};
        };
        std::vector<ArgBuf>                          fnArgBufs_;
        std::unordered_map<int, std::vector<ArgBuf>> fnArgBufsCache_; // persists args across fn switches
        std::string                                  fnLastResult_;
        bool                                         fnLastResultOk_{false};

        // ── C++ classes tab ───────────────────────────────────────────────────────
        int   selectedClassIdx_{-1};
        int   selectedMethodIdx_{-1};
        int   selectedObjIdx_{-1};
        float clsSplitW_{220.0f};   // resizable left panel width
        char  methResultVar_[64]{}; // LOCAL variable to store C++ method return value

        std::vector<ArgBuf>                          methArgBufs_;
        std::unordered_map<int, std::vector<ArgBuf>> methArgBufsCache_; // key: clsIdx*10000+methIdx

        struct StructBuf {
                std::vector<uint8_t> data;
                const CStructDecl   *decl{nullptr};
                bool                 expanded{false};
        };
        std::vector<StructBuf>                          structBufs_;
        std::unordered_map<int, std::vector<StructBuf>> structBufsCache_; // key: clsIdx*10000+methIdx

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
                uint64_t    tsMs{0};    // ms since epoch (wall-clock)
                double      tsSec{0.0}; // sessionTimeSec() for monitor push
        };
        std::vector<HistEntry> history_;
        std::mutex             histMtx_; // protects history_ (background thread writes)
        bool                   historyExpanded_{true};
        bool                   modified_{false};

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

        // ── symbols viewer ──────────────────────────────────────────────────────────
        bool showSymbolsWindow_{false};
        char symbolFilter_[128]{};

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
        bool drawHistoryHeader(const char *id);
        void pushHistory(const std::string &call, const std::string &result, bool ok, double tsSec);
        // Store a successful call's integer/float return value into the LOCAL variable
        // named `name` (no-op if name is empty or the variable does not exist).
        void writeResultVar(const std::string &name, const CallResult &res, CType retType);

        const CEnumDecl   *findParamEnum(const CParam &p) const;
        const CStructDecl *findParamStruct(const CParam &p) const;

        // Resolve a "&var" (→ LOCAL buffer address) or "$var" (→ current value) argument
        // string against the variable manager. Plain literals pass through unchanged.
        std::string resolveArgVar(const std::string &raw, const CParam &p) const;

        // Renders [+][v] buttons (SameLine) after a parameter's value widget. "+" creates a
        // LOCAL variable for the parameter; "v" picks an existing LOCAL variable and binds it
        // into argBuf as "&var" (pointer/reference param) or "$var" (value param). `salt`
        // makes widget IDs unique. Width for the buttons must be reserved by the caller
        // (see kParamBtnsW).
        void             drawParamVarButtons(char *argBuf, size_t argBufSz, const CParam &p, int salt);
        static const int kParamBtnsW = 48; // px to reserve for the [+][v] buttons

        void drawCFunctionsTab();
        void drawCppClassesTab();
        void drawPythonTab();
        void drawSymbolsWindow();

        // ── Python tab ───────────────────────────────────────────────────────────
        char                pyPath_[512]{};
        PyParseResult       pyResult_;
        char                pyFnResultVar_[64]{}; // LOCAL variable to store Python function return value
        int                 pySelFnIdx_{-1};
        int                 pySelClsIdx_{-1};
        int                 pySelMethIdx_{-1};
        int                 pySelObjIdx_{-1};
        float               pySplitW_{220.0f};
        std::vector<ArgBuf> pyFnArgBufs_;
        std::unordered_map<int, std::vector<ArgBuf>> pyFnArgBufsCache_;
        std::vector<ArgBuf>                          pyMethArgBufs_;
        std::unordered_map<int, std::vector<ArgBuf>> pyMethArgBufsCache_; // key: clsIdx*1000+methIdx
        std::string                                  pyLastResult_;
        bool                                         pyLastResultOk_{false};

        struct PyObjEntry {
                int         pyId{-1};
                std::string label;
                std::string className;
        };
        std::vector<PyObjEntry> pyObjects_;

        struct PyRunner; // defined in sdk_panel.cpp
        std::unique_ptr<PyRunner> pyRunner_;

        void doStartPyRunner();
        void doLoadPy();
        void doCallPyFunc();
        void doNewPyObject();
        void doNewPyObjectWithArgs(const std::vector<std::string> &args);
        void doCallPyMeth();
        void doDeletePyObject(int idx);

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
