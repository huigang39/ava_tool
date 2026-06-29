#include "sequence_editor.hpp"
#include "ImGuiNotify.hpp"
#include "app_log.hpp"
#include "cJSON.h"
#include "gui/i18n.hpp"
#include "gui/sdk_panel.hpp"
#include "gui/ui_theme.hpp"
#include "imgui.h"
#include "monitor.hpp"
#include "platform/native_dlg.hpp"
#include "timeops.h"
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>

// ─── helpers ─────────────────────────────────────────────────────────────────

static uint64_t
nowStampMs()
{
        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
}

static const char *
kindLabel(SeqStepKind k)
{
        switch (k) {
                case SeqStepKind::Action:
                        return "do";
                case SeqStepKind::Sleep:
                        return "delay";
                case SeqStepKind::If:
                        return "if";
                case SeqStepKind::While:
                        return "While";
                case SeqStepKind::For:
                        return "For";
                case SeqStepKind::Break:
                        return "Break";
                case SeqStepKind::SdkCall:
                        return "SDK";
        }
        return "?";
}

static const char *
kindLabelCn(SeqStepKind k)
{
        switch (k) {
                case SeqStepKind::Action:
                        return "执行";
                case SeqStepKind::Sleep:
                        return "延时";
                case SeqStepKind::If:
                        return "判断";
                case SeqStepKind::While:
                        return "循环";
                case SeqStepKind::For:
                        return "计数";
                case SeqStepKind::Break:
                        return "退出";
                case SeqStepKind::SdkCall:
                        return "SDK";
        }
        return "?";
}

// ─── ctor/dtor ───────────────────────────────────────────────────────────────

SequenceEditor::SequenceEditor()
{
        refreshSeqFiles();
}

SequenceEditor::~SequenceEditor()
{
        seqStopReq_.store(true);
        if (seqThread_.joinable())
                seqThread_.join();
}

// ─── refreshSeqFiles ─────────────────────────────────────────────────────────

void
SequenceEditor::refreshSeqFiles()
{
        seqFiles_.clear();
        try {
                std::filesystem::path dirPath(reinterpret_cast<const char8_t *>(seqFolder_.c_str()));
                if (std::filesystem::exists(dirPath) && std::filesystem::is_directory(dirPath)) {
                        for (const auto &entry : std::filesystem::directory_iterator(dirPath)) {
                                if (entry.is_regular_file() && entry.path().extension() == ".seq") {
                                        std::u8string u8str = entry.path().filename().u8string();
                                        seqFiles_.push_back(std::string(u8str.begin(), u8str.end()));
                                }
                        }
                }
        } catch (...) {
        }
        selectedSeqIdx_ = -1;
}

// ─── writeAction ─────────────────────────────────────────────────────────────

void
SequenceEditor::writeAction(const SequenceAction &action)
{
        u8  buf[8] = {0};
        u32 sz     = Parser::typeBytes(action.type);
        if (sz == 0 || sz > 8)
                return;

        bool needRmw = (action.bitSize > 0);
        if (needRmw) {
                bool readOk = false;
                if (action.port == "JLINK" && JLinkPort::instance().isConnected()) {
                        readOk = JLinkPort::instance().readMem((u32)action.addr, sz, buf);
                }
                if (!readOk)
                        return;
        }

        i64 valI = 0;
        f64 valF = 0.0;
        try {
                if (action.isEnum) {
                        bool found = false;
                        for (const auto &[k, v] : action.enumDefs) {
                                if (v == action.targetValue) {
                                        valI  = k;
                                        found = true;
                                        break;
                                }
                        }
                        if (!found)
                                valI = std::stoll(action.targetValue);
                } else {
                        if (action.type == DataType::F32 || action.type == DataType::F64)
                                valF = std::stod(action.targetValue);
                        else
                                valI = std::stoll(action.targetValue);
                }
        } catch (...) {
                return;
        }

        if (needRmw) {
                u64 mask       = ((1ULL << action.bitSize) - 1) << action.bitOffset;
                u64 currentVal = 0;
                std::memcpy(&currentVal, buf, sz);
                currentVal &= ~mask;
                currentVal |= ((valI << action.bitOffset) & mask);
                std::memcpy(buf, &currentVal, sz);
        } else {
                if (action.type == DataType::F32)
                        *(f32 *)buf = (f32)valF;
                else if (action.type == DataType::F64)
                        *(f64 *)buf = valF;
                else if (action.type == DataType::U32)
                        *(u32 *)buf = (u32)valI;
                else if (action.type == DataType::I32)
                        *(i32 *)buf = (i32)valI;
                else if (action.type == DataType::U16)
                        *(u16 *)buf = (u16)valI;
                else if (action.type == DataType::I16)
                        *(i16 *)buf = (i16)valI;
                else if (action.type == DataType::U8)
                        *(u8 *)buf = (u8)valI;
                else if (action.type == DataType::I8)
                        *(i8 *)buf = (i8)valI;
                else if (action.type == DataType::U64)
                        *(u64 *)buf = (u64)valI;
                else if (action.type == DataType::I64)
                        *(i64 *)buf = valI;
        }

        if (action.port == "JLINK" && JLinkPort::instance().isConnected()) {
                JLinkPort::instance().writeMem((u32)action.addr, sz, buf);
        } else if (action.port == "SHM") {
                shm_t     shm_handle;
                shm_cfg_t cfg = {action.shmName.c_str(), SHM_READWRITE, 4096};
                if (shm_init(&shm_handle, cfg) == 0)
                        shm_write(&shm_handle, buf, sz);
        }

        {
                SeqLogEntry e;
                e.tsMs  = nowStampMs();
                e.desc  = "Write: " + action.name;
                e.value = "= " + action.targetValue;
                e.ok    = true;
                std::lock_guard<std::mutex> lk(seqMtx_);
                seqLog_.push_back(std::move(e));
        }
}

// ─── execSdkOp ────────────────────────────────────────────────────────────────
// Executes a single SDK-call operation from a "do" step's body (sampler/seq thread).

void
SequenceEditor::execSdkOp(const SdkStepInfo &sdk)
{
        LOG_I("[SeqEditor] SdkOp label='%s' panelId=%d isCFunc=%d ci=%d mi=%d",
              sdk.label.c_str(),
              sdk.panelWinId,
              (int)sdk.isCFunc,
              sdk.classIdx,
              sdk.methodIdx);
        auto panel = findSdkPanel(sdk.panelWinId);
        if (!panel) {
                LOG_E("[SeqEditor] SdkOp: panel id=%d not found", sdk.panelWinId);
                SeqLogEntry e;
                e.tsMs  = nowStampMs();
                e.desc  = "SDK: " + sdk.label;
                e.value = "panel not found";
                e.ok    = false;
                std::lock_guard<std::mutex> lk(seqMtx_);
                seqLog_.push_back(std::move(e));
                return;
        }
        auto writeResultVar = [&](bool ok, const SdkPanel::DirectCallResult &r) {
                if (ok && sdk.resultVar[0] != '\0' && onGetLocalBuf_) {
                        void *buf = onGetLocalBuf_(sdk.resultVar);
                        if (buf) {
                                DataType dt = DataType::I64;
                                if (onGetLocalVarDataType_)
                                        dt = onGetLocalVarDataType_(sdk.resultVar);
                                if (dt == DataType::F64) {
                                        double v = sdk.isPython ? r.rawDouble : *reinterpret_cast<const double *>(&r.rawValue);
                                        std::memcpy(buf, &v, sizeof(double));
                                } else if (dt == DataType::F32) {
                                        float v = sdk.isPython ? static_cast<float>(r.rawDouble)
                                                               : *reinterpret_cast<const float *>(&r.rawValue);
                                        std::memcpy(buf, &v, sizeof(float));
                                } else {
                                        std::memcpy(buf, &r.rawValue, sizeof(int64_t));
                                }
                                if (onLocalVarWritten_)
                                        onLocalVarWritten_(sdk.resultVar);
                        }
                }
        };
        if (sdk.isPython) {
                auto r = panel->directCallPy(sdk.pyFuncName, sdk.args);
                LOG_I("[SeqEditor] SdkOp Py result ok=%d: %s", (int)r.ok, r.text.c_str());
                writeResultVar(r.ok, r);
                SeqLogEntry e;
                e.tsMs  = nowStampMs();
                e.desc  = "SDK: " + sdk.label;
                e.ok    = r.ok;
                e.value = r.ok ? "OK" : "FAIL";
                if (!r.text.empty())
                        e.value += ": " + r.text;
                std::lock_guard<std::mutex> lk(seqMtx_);
                seqLog_.push_back(std::move(e));
        } else if (sdk.isCFunc) {
                auto r = panel->directCallC(sdk.methodIdx, sdk.args);
                LOG_I("[SeqEditor] SdkOp C result ok=%d: %s", (int)r.ok, r.text.c_str());
                writeResultVar(r.ok, r);
                SeqLogEntry e;
                e.tsMs  = nowStampMs();
                e.desc  = "SDK: " + sdk.label;
                e.ok    = r.ok;
                e.value = r.ok ? "OK" : "FAIL";
                if (!r.text.empty())
                        e.value += ": " + r.text;
                std::lock_guard<std::mutex> lk(seqMtx_);
                seqLog_.push_back(std::move(e));
        } else {
                auto r = panel->directCall(sdk.classIdx, sdk.methodIdx, sdk.objIdx, sdk.args);
                LOG_I("[SeqEditor] SdkOp C++ result ok=%d: %s", (int)r.ok, r.text.c_str());
                writeResultVar(r.ok, r);
                SeqLogEntry e;
                e.tsMs  = nowStampMs();
                e.desc  = "SDK: " + sdk.label;
                e.ok    = r.ok;
                e.value = r.ok ? "OK" : "FAIL";
                if (!r.text.empty())
                        e.value += ": " + r.text;
                std::lock_guard<std::mutex> lk(seqMtx_);
                seqLog_.push_back(std::move(e));
        }
}

// ─── Background execution ─────────────────────────────────────────────────────

bool
SequenceEditor::seqEvalCond(const SequenceStep &step)
{
        int64_t lval = 0;
        if (step.condVar[0] && onGetLocalBuf_) {
                void *buf = onGetLocalBuf_(step.condVar);
                if (buf)
                        std::memcpy(&lval, buf, sizeof(int64_t));
        }
        int64_t          rval = step.condVal;
        std::string_view op   = step.condOp;
        if (op == "==")
                return lval == rval;
        if (op == "!=")
                return lval != rval;
        if (op == "<")
                return lval < rval;
        if (op == ">")
                return lval > rval;
        if (op == "<=")
                return lval <= rval;
        if (op == ">=")
                return lval >= rval;
        return true;
}

// Cap a free-spinning loop at ~1 kHz. Each While/For iteration checks its
// condition via seqEvalCond() → onGetLocalBuf_(), which takes the GUI's
// mtxMonitors_ lock; an unthrottled loop would acquire it at unbounded rate and
// starve the render thread (which needs the same lock every frame), dragging
// the GUI to a crawl. Iterations whose body already takes ≥1 ms are unaffected.
static void
throttleLoopIteration(std::chrono::steady_clock::time_point iterStart)
{
        const auto elapsed = std::chrono::steady_clock::now() - iterStart;
        const auto floor   = std::chrono::milliseconds(1);
        if (elapsed < floor)
                std::this_thread::sleep_for(floor - elapsed);
}

SequenceEditor::SExecResult
SequenceEditor::seqExecStep(const SequenceStep &step, SeqCtx &ctx)
{
        if (ctx.editor->seqStopReq_.load())
                return SExecResult::Stop;

        LOG_I("[SeqEditor] exec kind=%d name='%s'", (int)step.kind, step.name.c_str());

        switch (step.kind) {

                case SeqStepKind::Action:
                        if (step.delayMs > 0)
                                std::this_thread::sleep_for(std::chrono::milliseconds(step.delayMs));
                        for (const auto &op : step.ops) {
                                if (ctx.editor->seqStopReq_.load())
                                        return SExecResult::Stop;
                                if (op.kind == SeqOpKind::Write) {
                                        LOG_I("[SeqEditor] do/write: %s = %s",
                                              op.action.name.c_str(),
                                              op.action.targetValue.c_str());
                                        ctx.editor->writeAction(op.action);
                                } else {
                                        ctx.editor->execSdkOp(op.sdk);
                                }
                        }
                        break;

                case SeqStepKind::Sleep:
                        LOG_I("[SeqEditor] Sleep %d ms", step.sleepMs);
                        std::this_thread::sleep_for(std::chrono::milliseconds(step.sleepMs));
                        break;

                case SeqStepKind::If: {
                        if (ctx.editor->seqEvalCond(step))
                                return seqExecSteps(step.body, ctx);
                        else if (step.hasElse)
                                return seqExecSteps(step.elseBody, ctx);
                        break;
                }

                case SeqStepKind::While: {
                        while (!ctx.editor->seqStopReq_.load() && ctx.editor->seqEvalCond(step)) {
                                const auto  iterStart = std::chrono::steady_clock::now();
                                SExecResult r         = seqExecSteps(step.body, ctx);
                                if (r == SExecResult::Stop)
                                        return SExecResult::Stop;
                                if (r == SExecResult::Break)
                                        break;
                                throttleLoopIteration(iterStart);
                        }
                        break;
                }

                case SeqStepKind::For: {
                        int64_t v = step.forFrom;
                        while (!ctx.editor->seqStopReq_.load()) {
                                if (step.forStep > 0 && v >= step.forTo)
                                        break;
                                if (step.forStep < 0 && v <= step.forTo)
                                        break;
                                if (step.forStep == 0)
                                        break;
                                SExecResult r = seqExecSteps(step.body, ctx);
                                if (r == SExecResult::Stop)
                                        return SExecResult::Stop;
                                if (r == SExecResult::Break)
                                        break;
                                v += step.forStep;
                        }
                        break;
                }

                case SeqStepKind::Break:
                        return SExecResult::Break;

                default:
                        break;
        }
        return SExecResult::Ok;
}

SequenceEditor::SExecResult
SequenceEditor::seqExecSteps(const std::vector<SequenceStep> &steps, SeqCtx &ctx)
{
        for (const auto &step : steps) {
                if (ctx.editor->seqStopReq_.load())
                        return SExecResult::Stop;
                SExecResult r = seqExecStep(step, ctx);
                if (r != SExecResult::Ok)
                        return r;
        }
        return SExecResult::Ok;
}

// ─── selectedStepPtr ─────────────────────────────────────────────────────────

SequenceStep *
SequenceEditor::selectedStepPtr()
{
        if (selectedStep_ < 0 || selectedStep_ >= (int)steps_.size())
                return nullptr;
        return &steps_[selectedStep_];
}

// Map a CType (from c_header_parser) to a DataType for LOCAL struct field storage.
static DataType
ctypeToDataType(CType t, size_t sz)
{
        switch (t) {
                case CType::I8:
                        return DataType::I8;
                case CType::I16:
                        return DataType::I16;
                case CType::I32:
                        return DataType::I32;
                case CType::I64:
                        return DataType::I64;
                case CType::U8:
                        return DataType::U8;
                case CType::U16:
                        return DataType::U16;
                case CType::U32:
                        return DataType::U32;
                case CType::U64:
                        return DataType::U64;
                case CType::F32:
                        return DataType::F32;
                case CType::F64:
                        return DataType::F64;
                case CType::Bool:
                        return DataType::U8;
                case CType::Ptr:
                        return DataType::U64;
                default:
                        break;
        }
        switch (sz) {
                case 1:
                        return DataType::U8;
                case 2:
                        return DataType::U16;
                case 8:
                        return DataType::U64;
                default:
                        return DataType::U32;
        }
}

// Build a SeqStructField list from a CStructDecl (flattens single-level arrays).
static std::vector<SeqStructField>
structDeclToFields(const CStructDecl &sd)
{
        std::vector<SeqStructField> out;
        for (const auto &f : sd.fields) {
                if (f.isArray && f.arrayCount > 0) {
                        size_t   elemSz = ctypeSize(f.arrayElemType);
                        DataType et     = ctypeToDataType(f.arrayElemType, elemSz);
                        for (size_t ai = 0; ai < f.arrayCount; ++ai) {
                                SeqStructField sf;
                                sf.name       = f.name + "[" + std::to_string(ai) + "]";
                                sf.type       = et;
                                sf.byteOffset = (uint32_t)(f.offset + ai * elemSz);
                                out.push_back(sf);
                        }
                } else {
                        SeqStructField sf;
                        sf.name       = f.name;
                        sf.type       = ctypeToDataType(f.type, f.size);
                        sf.byteOffset = (uint32_t)f.offset;
                        out.push_back(sf);
                }
        }
        return out;
}

// Map a raw C ptr/ref parameter type string to a DataType for LOCAL var creation.
static DataType
ptrTypeFromRaw(const std::string &rawType)
{
        std::string t = rawType;
        // Strip trailing ptr/ref/space qualifiers
        while (!t.empty() && (t.back() == '*' || t.back() == '&' || t.back() == ' '))
                t.pop_back();
        // Strip leading const/volatile only (keep unsigned/signed for correct mapping)
        for (bool changed = true; changed;) {
                changed = false;
                for (const char *q : {"const ", "volatile "}) {
                        size_t ql = strlen(q);
                        if (t.size() >= ql && t.compare(0, ql, q) == 0) {
                                t       = t.substr(ql);
                                changed = true;
                        }
                }
        }
        if (t == "float")
                return DataType::F32;
        if (t == "double")
                return DataType::F64;
        if (t == "int8_t" || t == "signed char")
                return DataType::I8;
        if (t == "int16_t" || t == "short" || t == "signed short")
                return DataType::I16;
        if (t == "int32_t" || t == "int" || t == "long" || t == "signed int" || t == "signed long")
                return DataType::I32;
        if (t == "int64_t" || t == "long long" || t == "signed long long")
                return DataType::I64;
        if (t == "uint8_t" || t == "char" || t == "unsigned char")
                return DataType::U8;
        if (t == "uint16_t" || t == "unsigned short")
                return DataType::U16;
        if (t == "uint32_t" || t == "unsigned int" || t == "unsigned long")
                return DataType::U32;
        if (t == "uint64_t" || t == "unsigned long long")
                return DataType::U64;
        return DataType::U32;
}

// ─── drawSdkStructArgRow ──────────────────────────────────────────────────────
// Struct-pointer argument rendered like the SDK caller: an expandable tree whose
// editable fields are backed by an auto-created LOCAL struct variable (bound via
// the "&varname" arg convention the executor already understands).

bool
SequenceEditor::drawSdkStructArgRow(SdkPanel          *panel,
                                    SdkStepInfo       &sdk,
                                    int                p,
                                    const std::string &pname,
                                    const std::string &ptype)
{
        if (!panel || sdk.isPython)
                return false;
        const CStructDecl *sd = panel->getParamStructDecl(sdk.isCFunc, sdk.classIdx, sdk.methodIdx, p);
        if (!sd)
                return false;

        // Name of the LOCAL struct variable this argument is bound to.
        std::string varName;
        if (!sdk.args[p].empty() && sdk.args[p][0] == '&')
                varName = sdk.args[p].substr(1);
        else
                varName = pname.empty() ? ("arg" + std::to_string(p)) : pname;

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(ptype.empty() ? "struct*" : ptype.c_str());

        ImGui::TableSetColumnIndex(1);
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "%s##sstk%d", pname.empty() ? "param" : pname.c_str(), p);
        bool open = ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_SpanFullWidth);

        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("&%s", varName.c_str());
        drawSdkArgButtons(panel, sdk, p, pname, ptype, /*isPtr=*/true);

        if (open) {
                // Bind to (and create) a LOCAL struct variable on first expand.
                if (sdk.args[p].empty() || sdk.args[p][0] != '&') {
                        if (onAddLocalStructVar_)
                                onAddLocalStructVar_(varName, structDeclToFields(*sd), sd->totalSize);
                        sdk.args[p] = "&" + varName;
                        isModified_ = true;
                }
                void *buf = onGetLocalBuf_ ? onGetLocalBuf_(varName) : nullptr;
                if (buf) {
                        if (drawSdkStructFieldRows(*sd, (uint8_t *)buf, sd->totalSize, p)) {
                                isModified_ = true;
                                if (onLocalVarWritten_)
                                        onLocalVarWritten_(varName);
                        }
                } else {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextDisabled("(%s)", tr("no buffer", "无缓冲"));
                }
                ImGui::TreePop();
        }
        return true;
}

// ─── drawSdkArgButtons ────────────────────────────────────────────────────────

void
SequenceEditor::drawSdkArgButtons(SdkPanel          *panel,
                                  SdkStepInfo       &sdk,
                                  int                p,
                                  const std::string &pname,
                                  const std::string &ptype,
                                  bool               isPtr)
{
        std::string pn = pname.empty() ? ("arg" + std::to_string(p)) : pname;

        // "+" — create a LOCAL variable for this parameter.
        if (onAddLocalVar_) {
                ImGui::SameLine(0, 2);
                char id[24];
                snprintf(id, sizeof(id), "+##sa%d", p);
                if (ImGui::SmallButton(id)) {
                        const CStructDecl *sd =
                            panel ? panel->getParamStructDecl(sdk.isCFunc, sdk.classIdx, sdk.methodIdx, p) : nullptr;
                        if (sd && onAddLocalStructVar_)
                                onAddLocalStructVar_(pn, structDeclToFields(*sd), sd->totalSize);
                        else {
                                DataType dt = ptrTypeFromRaw(ptype);
                                size_t   sz = Parser::typeBytes(dt);
                                if (sz == 0)
                                        sz = 4;
                                onAddLocalVar_(pn, dt, sz);
                        }
                        if (isPtr) {
                                sdk.args[p] = "&" + pn;
                                isModified_ = true;
                        }
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Add this parameter as a variable", "把该参数添加为变量"));
        }

        // "v" — pick an existing LOCAL variable and bind it to this argument.
        if (panel && panel->onListLocalVars_) {
                ImGui::SameLine(0, 2);
                char btnId[24], popId[24];
                snprintf(btnId, sizeof(btnId), "v##sap%d", p);
                snprintf(popId, sizeof(popId), "##sapp%d", p);
                if (ImGui::SmallButton(btnId))
                        ImGui::OpenPopup(popId);
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Use a variable for this argument", "用变量作为该参数"));
                if (ImGui::BeginPopup(popId)) {
                        auto vars = panel->onListLocalVars_();
                        if (vars.empty())
                                ImGui::TextDisabled("%s", tr("(no variables)", "(无变量)"));
                        for (const auto &vn : vars)
                                if (ImGui::Selectable(vn.c_str())) {
                                        sdk.args[p] = (isPtr ? "&" : "$") + vn;
                                        isModified_ = true;
                                }
                        ImGui::EndPopup();
                }
        }
}

// ─── drawBodySteps ────────────────────────────────────────────────────────────
// Renders an inline editable list of steps (used for If/While/For bodies).

void
SequenceEditor::drawBodySteps(std::vector<SequenceStep> &steps, int depth)
{
        static const char *condOps[] = {"==", "!=", "<", ">", "<=", ">="};

        bool topLevel = (depth == 0);
        if (topLevel) {
                static ImGuiTableFlags tblFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY |
                                                  ImGuiTableFlags_Resizable;
                if (!ImGui::BeginTable("##bodytbl", 3, tblFlags))
                        return;
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn(tr("#", "#"), ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn(tr("Name", "名称"), ImGuiTableColumnFlags_WidthStretch, 0.35f);
                ImGui::TableSetupColumn(tr("Value / Args", "值/参数"), ImGuiTableColumnFlags_WidthStretch, 0.65f);
                ImGui::TableHeadersRow();
        }

        float indent       = depth * 14.0f;
        int   stepToDelete = -1, stepMoveSrc = -1, stepMoveDst = -1;

        for (int i = 0; i < (int)steps.size(); ++i) {
                SequenceStep &s = steps[i];
                ImGui::PushID(i + depth * 1000);

                if (s.kind == SeqStepKind::Action) {
                        // ── Render ops directly as table rows (same format as "do" detail) ──
                        int opToDelete = -1, opMoveSrc = -1, opMoveDst = -1;
                        for (int j = 0; j < (int)s.ops.size(); ++j) {
                                ImGui::PushID(j + 20000);
                                SeqOp &op = s.ops[j];

                                if (op.kind == SeqOpKind::Write) {
                                        SequenceAction &a = op.action;
                                        ImGui::TableNextRow();

                                        ImGui::TableSetColumnIndex(0);
                                        if (indent > 0.0f)
                                                ImGui::Indent(indent);
                                        ImGui::SmallButton("=##og");
                                        if (ImGui::BeginDragDropSource()) {
                                                ImGui::SetDragDropPayload("BODYOP_MOVE", &j, sizeof(int));
                                                ImGui::Text("%d", j + 1);
                                                ImGui::EndDragDropSource();
                                        }
                                        if (ImGui::BeginDragDropTarget()) {
                                                if (const ImGuiPayload *pl = ImGui::AcceptDragDropPayload("BODYOP_MOVE")) {
                                                        opMoveSrc = *(const int *)pl->Data;
                                                        opMoveDst = j;
                                                }
                                                ImGui::EndDragDropTarget();
                                        }
                                        ImGui::OpenPopupOnItemClick("##opdelmenu", ImGuiPopupFlags_MouseButtonRight);
                                        if (ImGui::BeginPopup("##opdelmenu")) {
                                                if (ImGui::MenuItem(tr("Delete", "删除")))
                                                        opToDelete = j;
                                                ImGui::EndPopup();
                                        }
                                        ImGui::SameLine();
                                        ImGui::Text("%d", j + 1);
                                        if (indent > 0.0f)
                                                ImGui::Unindent(indent);

                                        ImGui::TableSetColumnIndex(1);
                                        ImGui::AlignTextToFramePadding();
                                        ImGui::TextUnformatted(a.name.c_str());

                                        ImGui::TableSetColumnIndex(2);
                                        ImGui::SetNextItemWidth(-FLT_MIN);
                                        if (a.isEnum && !a.enumDefs.empty()) {
                                                std::string preview = a.targetValue;
                                                for (const auto &[val, nm] : a.enumDefs)
                                                        if (nm == preview) {
                                                                preview += " (" + std::to_string(val) + ")";
                                                                break;
                                                        }
                                                if (ImGui::BeginCombo("##v", preview.c_str())) {
                                                        for (const auto &[val, nm] : a.enumDefs) {
                                                                bool        sel = (a.targetValue == nm);
                                                                std::string lbl = nm + " (" + std::to_string(val) + ")";
                                                                if (ImGui::Selectable(lbl.c_str(), sel)) {
                                                                        a.targetValue = nm;
                                                                        isModified_   = true;
                                                                }
                                                                if (sel)
                                                                        ImGui::SetItemDefaultFocus();
                                                        }
                                                        ImGui::EndCombo();
                                                }
                                        } else {
                                                char vbuf[64];
                                                strncpy(vbuf, a.targetValue.c_str(), sizeof(vbuf) - 1);
                                                vbuf[sizeof(vbuf) - 1] = '\0';
                                                if (ImGui::InputText("##v", vbuf, sizeof(vbuf))) {
                                                        a.targetValue = vbuf;
                                                        isModified_   = true;
                                                }
                                        }

                                } else {
                                        // SDK op
                                        auto panel   = findSdkPanel(op.sdk.panelWinId);
                                        int  nParams = 0;
                                        if (panel) {
                                                if (op.sdk.isPython)
                                                        nParams = panel->getPyFuncParamCount(op.sdk.pyFuncName);
                                                else if (op.sdk.isCFunc)
                                                        nParams = panel->getCFuncParamCount(op.sdk.methodIdx);
                                                else
                                                        nParams = panel->getParamCount(op.sdk.classIdx, op.sdk.methodIdx);
                                        }
                                        op.sdk.args.resize(nParams);

                                        ImGui::TableNextRow();

                                        ImGui::TableSetColumnIndex(0);
                                        if (indent > 0.0f)
                                                ImGui::Indent(indent);
                                        ImGui::SmallButton("=##og");
                                        if (ImGui::BeginDragDropSource()) {
                                                ImGui::SetDragDropPayload("BODYOP_MOVE", &j, sizeof(int));
                                                ImGui::Text("%d", j + 1);
                                                ImGui::EndDragDropSource();
                                        }
                                        if (ImGui::BeginDragDropTarget()) {
                                                if (const ImGuiPayload *pl = ImGui::AcceptDragDropPayload("BODYOP_MOVE")) {
                                                        opMoveSrc = *(const int *)pl->Data;
                                                        opMoveDst = j;
                                                }
                                                ImGui::EndDragDropTarget();
                                        }
                                        ImGui::OpenPopupOnItemClick("##opdelmenu", ImGuiPopupFlags_MouseButtonRight);
                                        if (ImGui::BeginPopup("##opdelmenu")) {
                                                if (ImGui::MenuItem(tr("Delete", "删除")))
                                                        opToDelete = j;
                                                ImGui::EndPopup();
                                        }
                                        ImGui::SameLine();
                                        ImGui::Text("%d", j + 1);
                                        if (indent > 0.0f)
                                                ImGui::Unindent(indent);

                                        ImGui::TableSetColumnIndex(1);
                                        bool open = ImGui::TreeNodeEx(
                                            "##sdkn", ImGuiTreeNodeFlags_DefaultOpen, "%s", op.sdk.label.c_str());

                                        if (open) {
                                                if (!panel) {
                                                        ImGui::TableNextRow();
                                                        ImGui::TableSetColumnIndex(1);
                                                        ImGui::Indent(indent + 16.0f);
                                                        ImGui::TextDisabled(
                                                            tr("SDK panel (id=%d) closed.", "SDK 窗口 (id=%d) 已关闭。"),
                                                            op.sdk.panelWinId);
                                                        ImGui::Unindent(indent + 16.0f);
                                                } else {
                                                        if (!op.sdk.isCFunc && !op.sdk.isPython) {
                                                                auto        objs = panel->listObjects(op.sdk.classIdx);
                                                                const char *cur  = tr("(none)", "(无)");
                                                                for (const auto &o : objs)
                                                                        if (o.idx == op.sdk.objIdx) {
                                                                                cur = o.label.c_str();
                                                                                break;
                                                                        }
                                                                ImGui::TableNextRow();
                                                                ImGui::TableSetColumnIndex(0);
                                                                {
                                                                        std::string cn = panel->getClassName(op.sdk.classIdx);
                                                                        ImGui::TextUnformatted(cn.empty() ? "?" : cn.c_str());
                                                                }
                                                                ImGui::TableSetColumnIndex(1);
                                                                ImGui::Indent(indent + 16.0f);
                                                                ImGui::TextUnformatted(tr("Object", "对象"));
                                                                ImGui::Unindent(indent + 16.0f);
                                                                ImGui::TableSetColumnIndex(2);
                                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                                if (ImGui::BeginCombo("##objsel", cur)) {
                                                                        if (ImGui::Selectable(
                                                                                tr("[+ New Object]", "[+ 新建对象]"), false)) {
                                                                                int ni = panel->newObject(op.sdk.classIdx);
                                                                                if (ni >= 0) {
                                                                                        op.sdk.objIdx = ni;
                                                                                        isModified_   = true;
                                                                                }
                                                                        }
                                                                        for (const auto &o : objs) {
                                                                                bool sel = (o.idx == op.sdk.objIdx);
                                                                                char cl[128];
                                                                                snprintf(cl,
                                                                                         sizeof(cl),
                                                                                         "%s  (%s)",
                                                                                         o.label.c_str(),
                                                                                         o.className.c_str());
                                                                                if (ImGui::Selectable(cl, sel)) {
                                                                                        op.sdk.objIdx = o.idx;
                                                                                        isModified_   = true;
                                                                                }
                                                                                if (sel)
                                                                                        ImGui::SetItemDefaultFocus();
                                                                        }
                                                                        ImGui::EndCombo();
                                                                }
                                                        }
                                                        std::vector<std::string> localVars;
                                                        if (panel->onListLocalVars_)
                                                                localVars = panel->onListLocalVars_();
                                                        for (int p = 0; p < nParams; ++p) {
                                                                ImGui::PushID(p);
                                                                std::string pname =
                                                                    op.sdk.isPython
                                                                        ? panel->getPyFuncParamName(op.sdk.pyFuncName, p)
                                                                    : op.sdk.isCFunc
                                                                        ? panel->getCFuncParamName(op.sdk.methodIdx, p)
                                                                        : panel->getParamName(
                                                                              op.sdk.classIdx, op.sdk.methodIdx, p);
                                                                std::string ptype =
                                                                    op.sdk.isCFunc
                                                                        ? panel->getCFuncParamRawType(op.sdk.methodIdx, p)
                                                                        : panel->getParamRawType(
                                                                              op.sdk.classIdx, op.sdk.methodIdx, p);
                                                                bool isPtr =
                                                                    op.sdk.isCFunc
                                                                        ? panel->isCFuncParamPtrOrRef(op.sdk.methodIdx, p)
                                                                        : panel->isParamPtrOrRef(
                                                                              op.sdk.classIdx, op.sdk.methodIdx, p);
                                                                // Struct param → expandable field editor (like the SDK caller).
                                                                if (drawSdkStructArgRow(
                                                                        panel.get(), op.sdk, p, pname, ptype)) {
                                                                        ImGui::PopID();
                                                                        continue;
                                                                }
                                                                ImGui::TableNextRow();
                                                                ImGui::TableSetColumnIndex(0);
                                                                ImGui::TextUnformatted(ptype.empty() ? "?" : ptype.c_str());
                                                                ImGui::TableSetColumnIndex(1);
                                                                ImGui::Indent(indent + 16.0f);
                                                                if (!pname.empty())
                                                                        ImGui::TextUnformatted(pname.c_str());
                                                                else
                                                                        ImGui::Text(tr("Arg %d", "参数%d"), p);
                                                                ImGui::Unindent(indent + 16.0f);
                                                                ImGui::TableSetColumnIndex(2);
                                                                char argBuf[512]{};
                                                                strncpy(argBuf, op.sdk.args[p].c_str(), sizeof(argBuf) - 1);
                                                                bool             hasPicker = !localVars.empty();
                                                                bool             hasCreate = (bool)onAddLocalVar_;
                                                                float            inputW    = (hasPicker && hasCreate)   ? -82.0f
                                                                                             : (hasPicker || hasCreate) ? -35.0f
                                                                                                                        : -FLT_MIN;
                                                                const CEnumDecl *ed =
                                                                    (!op.sdk.isPython)
                                                                        ? panel->getParamEnumDecl(op.sdk.isCFunc,
                                                                                                  op.sdk.classIdx,
                                                                                                  op.sdk.methodIdx,
                                                                                                  p)
                                                                        : nullptr;
                                                                if (ed && !ed->values.empty()) {
                                                                        int64_t curVal =
                                                                            op.sdk.args[p].empty()
                                                                                ? 0
                                                                                : strtoll(op.sdk.args[p].c_str(), nullptr, 0);
                                                                        const char *preview = ed->values[0].name.c_str();
                                                                        int         curIdx  = 0;
                                                                        for (int ei = 0; ei < (int)ed->values.size(); ++ei) {
                                                                                if (ed->values[ei].value == curVal) {
                                                                                        preview = ed->values[ei].name.c_str();
                                                                                        curIdx  = ei;
                                                                                        break;
                                                                                }
                                                                        }
                                                                        ImGui::SetNextItemWidth(
                                                                            ImGui::GetContentRegionAvail().x - 48.0f);
                                                                        char cmId[16];
                                                                        snprintf(cmId, sizeof(cmId), "##ec%d", p);
                                                                        if (ImGui::BeginCombo(cmId, preview)) {
                                                                                for (int ei = 0; ei < (int)ed->values.size();
                                                                                     ++ei) {
                                                                                        bool sel2 = (ei == curIdx);
                                                                                        char evLbl[128];
                                                                                        snprintf(
                                                                                            evLbl,
                                                                                            sizeof(evLbl),
                                                                                            "%s (%lld)",
                                                                                            ed->values[ei].name.c_str(),
                                                                                            (long long)ed->values[ei].value);
                                                                                        if (ImGui::Selectable(evLbl, sel2)) {
                                                                                                char numBuf[32];
                                                                                                snprintf(
                                                                                                    numBuf,
                                                                                                    sizeof(numBuf),
                                                                                                    "%lld",
                                                                                                    (long long)ed->values[ei]
                                                                                                        .value);
                                                                                                op.sdk.args[p] = numBuf;
                                                                                                isModified_    = true;
                                                                                        }
                                                                                        if (sel2)
                                                                                                ImGui::SetItemDefaultFocus();
                                                                                }
                                                                                ImGui::EndCombo();
                                                                        }
                                                                        drawSdkArgButtons(
                                                                            panel.get(), op.sdk, p, pname, ptype, isPtr);
                                                                } else {
                                                                        ImGui::SetNextItemWidth(inputW);
                                                                        if (ImGui::InputText("##arg", argBuf, sizeof(argBuf))) {
                                                                                op.sdk.args[p] = argBuf;
                                                                                isModified_    = true;
                                                                        }
                                                                        if (!ptype.empty() && ImGui::IsItemHovered())
                                                                                ImGui::SetTooltip("%s", ptype.c_str());
                                                                        if (hasCreate) {
                                                                                ImGui::SameLine();
                                                                                char cbtn[16];
                                                                                snprintf(cbtn, sizeof(cbtn), "+##c%d", p);
                                                                                if (ImGui::SmallButton(cbtn)) {
                                                                                        std::string pn =
                                                                                            pname.empty()
                                                                                                ? "arg" + std::to_string(p)
                                                                                                : pname;
                                                                                        DataType dt = ptrTypeFromRaw(ptype);
                                                                                        size_t   sz = Parser::typeBytes(dt);
                                                                                        if (sz == 0)
                                                                                                sz = 4;
                                                                                        const CStructDecl *sd =
                                                                                            panel->getParamStructDecl(
                                                                                                op.sdk.isCFunc,
                                                                                                op.sdk.classIdx,
                                                                                                op.sdk.methodIdx,
                                                                                                p);
                                                                                        if (sd && onAddLocalStructVar_)
                                                                                                onAddLocalStructVar_(
                                                                                                    pn,
                                                                                                    structDeclToFields(*sd),
                                                                                                    sd->totalSize);
                                                                                        else
                                                                                                onAddLocalVar_(pn, dt, sz);
                                                                                        if (isPtr)
                                                                                                op.sdk.args[p] = "&" + pn;
                                                                                        isModified_ = true;
                                                                                }
                                                                                if (ImGui::IsItemHovered())
                                                                                        ImGui::SetTooltip(
                                                                                            "%s",
                                                                                            tr("Create LOCAL variable",
                                                                                               "创建 LOCAL 变量"));
                                                                        }
                                                                        if (hasPicker) {
                                                                                ImGui::SameLine();
                                                                                char btnId[16];
                                                                                snprintf(btnId, sizeof(btnId), "v##p%d", p);
                                                                                if (ImGui::Button(btnId)) {
                                                                                        char pid[32];
                                                                                        snprintf(pid, sizeof(pid), "##vp%d", p);
                                                                                        ImGui::OpenPopup(pid);
                                                                                }
                                                                                char pid[32];
                                                                                snprintf(pid, sizeof(pid), "##vp%d", p);
                                                                                if (ImGui::BeginPopup(pid)) {
                                                                                        for (const auto &vn : localVars)
                                                                                                if (ImGui::Selectable(
                                                                                                        vn.c_str())) {
                                                                                                        op.sdk.args[p] =
                                                                                                            isPtr ? "&" + vn
                                                                                                                  : vn;
                                                                                                        isModified_ = true;
                                                                                                }
                                                                                        ImGui::EndPopup();
                                                                                }
                                                                        }
                                                                }
                                                                ImGui::PopID();
                                                        }
                                                        // Return value → LOCAL variable row
                                                        {
                                                                std::string retT = panel->getCallReturnType(op.sdk.isCFunc,
                                                                                                            op.sdk.isPython,
                                                                                                            op.sdk.classIdx,
                                                                                                            op.sdk.methodIdx,
                                                                                                            op.sdk.pyFuncName);
                                                                ImGui::TableNextRow();
                                                                ImGui::TableSetColumnIndex(0);
                                                                ImGui::TextUnformatted(retT.empty() ? "?" : retT.c_str());
                                                                ImGui::TableSetColumnIndex(1);
                                                                ImGui::Indent(indent + 16.0f);
                                                                ImGui::TextUnformatted(tr("return", "返回值"));
                                                                ImGui::Unindent(indent + 16.0f);
                                                                ImGui::TableSetColumnIndex(2);
                                                                char rvBuf[32]{};
                                                                strncpy(rvBuf, op.sdk.resultVar, sizeof(rvBuf) - 1);
                                                                bool  rvHasPicker = !localVars.empty();
                                                                bool  rvHasCreate = (bool)onAddLocalVar_;
                                                                float rvW         = (rvHasPicker && rvHasCreate)   ? -82.0f
                                                                                    : (rvHasPicker || rvHasCreate) ? -35.0f
                                                                                                                   : -FLT_MIN;
                                                                ImGui::SetNextItemWidth(rvW);
                                                                if (ImGui::InputText("##rv", rvBuf, sizeof(rvBuf))) {
                                                                        strncpy(op.sdk.resultVar,
                                                                                rvBuf,
                                                                                sizeof(op.sdk.resultVar) - 1);
                                                                        isModified_ = true;
                                                                }
                                                                if (rvHasCreate) {
                                                                        ImGui::SameLine();
                                                                        if (ImGui::SmallButton("+##rvc")) {
                                                                                std::string vn = (op.sdk.resultVar[0] != '\0')
                                                                                                     ? op.sdk.resultVar
                                                                                                     : "result";
                                                                                onAddLocalVar_(
                                                                                    vn, DataType::I64, sizeof(int64_t));
                                                                                strncpy(op.sdk.resultVar,
                                                                                        vn.c_str(),
                                                                                        sizeof(op.sdk.resultVar) - 1);
                                                                                isModified_ = true;
                                                                        }
                                                                        if (ImGui::IsItemHovered())
                                                                                ImGui::SetTooltip(
                                                                                    "%s",
                                                                                    tr("Create LOCAL variable for return value",
                                                                                       "为返回值创建 LOCAL 变量"));
                                                                }
                                                                if (rvHasPicker) {
                                                                        ImGui::SameLine();
                                                                        if (ImGui::Button("v##rvpick"))
                                                                                ImGui::OpenPopup("##rvpop");
                                                                        if (ImGui::BeginPopup("##rvpop")) {
                                                                                for (const auto &vn : localVars)
                                                                                        if (ImGui::Selectable(vn.c_str())) {
                                                                                                strncpy(
                                                                                                    op.sdk.resultVar,
                                                                                                    vn.c_str(),
                                                                                                    sizeof(op.sdk.resultVar) -
                                                                                                        1);
                                                                                                isModified_ = true;
                                                                                        }
                                                                                ImGui::EndPopup();
                                                                        }
                                                                }
                                                        }
                                                }
                                                ImGui::TreePop();
                                        }
                                }
                                ImGui::PopID();
                        }
                        if (opToDelete >= 0) {
                                s.ops.erase(s.ops.begin() + opToDelete);
                                if (s.ops.empty())
                                        stepToDelete = i;
                                isModified_ = true;
                        }
                        if (opMoveSrc >= 0 && opMoveDst >= 0 && opMoveSrc != opMoveDst) {
                                auto moved = s.ops[opMoveSrc];
                                s.ops.erase(s.ops.begin() + opMoveSrc);
                                if (opMoveDst > opMoveSrc)
                                        opMoveDst--;
                                s.ops.insert(s.ops.begin() + opMoveDst, moved);
                                isModified_ = true;
                        }

                } else {
                        // ── Control flow step: same 4-column layout as "do" ops ──────
                        ImGui::TableNextRow();

                        ImVec4 badgeCol = (s.kind == SeqStepKind::Sleep)   ? ImVec4(0.6f, 0.8f, 1.0f, 1.0f)
                                          : (s.kind == SeqStepKind::Break) ? ImVec4(1.0f, 0.5f, 0.5f, 1.0f)
                                                                           : ImVec4(1.0f, 0.85f, 0.4f, 1.0f);

                        ImGui::TableSetColumnIndex(0);
                        if (indent > 0.0f)
                                ImGui::Indent(indent);
                        ImGui::SmallButton("=##sg");
                        if (ImGui::BeginDragDropSource()) {
                                ImGui::SetDragDropPayload("BODYSTEP_MOVE", &i, sizeof(int));
                                ImGui::Text("%d", i + 1);
                                ImGui::EndDragDropSource();
                        }
                        if (ImGui::BeginDragDropTarget()) {
                                if (const ImGuiPayload *pl = ImGui::AcceptDragDropPayload("BODYSTEP_MOVE")) {
                                        stepMoveSrc = *(const int *)pl->Data;
                                        stepMoveDst = i;
                                }
                                ImGui::EndDragDropTarget();
                        }
                        ImGui::OpenPopupOnItemClick("##stepdelmenu", ImGuiPopupFlags_MouseButtonRight);
                        if (ImGui::BeginPopup("##stepdelmenu")) {
                                if (ImGui::MenuItem(tr("Delete", "删除")))
                                        stepToDelete = i;
                                ImGui::EndPopup();
                        }
                        ImGui::SameLine();
                        ImGui::TextColored(badgeCol, "%d", i + 1);
                        if (indent > 0.0f)
                                ImGui::Unindent(indent);

                        ImGui::TableSetColumnIndex(1);
                        ImGui::AlignTextToFramePadding();
                        if (indent > 0.0f)
                                ImGui::Indent(indent);
                        ImGui::TextColored(badgeCol, "%s", tr(kindLabel(s.kind), kindLabelCn(s.kind)));
                        if (indent > 0.0f)
                                ImGui::Unindent(indent);

                        ImGui::TableSetColumnIndex(2);
                        switch (s.kind) {
                                case SeqStepKind::Sleep:
                                        ImGui::SetNextItemWidth(50.0f);
                                        if (ImGui::InputInt("##ms", &s.sleepMs, 0))
                                                isModified_ = true;
                                        ImGui::SameLine();
                                        ImGui::TextDisabled("ms");
                                        break;
                                case SeqStepKind::If:
                                case SeqStepKind::While: {
                                        ImGui::SetNextItemWidth(60.0f);
                                        if (ImGui::InputText("##cv", s.condVar, sizeof(s.condVar)))
                                                isModified_ = true;
                                        ImGui::SameLine();
                                        int opi = 0;
                                        for (int oi = 0; oi < 6; ++oi)
                                                if (strcmp(s.condOp, condOps[oi]) == 0) {
                                                        opi = oi;
                                                        break;
                                                }
                                        ImGui::SetNextItemWidth(40.0f);
                                        if (ImGui::Combo("##op", &opi, condOps, 6)) {
                                                strncpy(s.condOp, condOps[opi], sizeof(s.condOp) - 1);
                                                isModified_ = true;
                                        }
                                        ImGui::SameLine();
                                        int64_t cv = s.condVal;
                                        ImGui::SetNextItemWidth(44.0f);
                                        if (ImGui::InputScalar("##cv2", ImGuiDataType_S64, &cv)) {
                                                s.condVal   = cv;
                                                isModified_ = true;
                                        }
                                        break;
                                }
                                case SeqStepKind::For: {
                                        ImGui::SetNextItemWidth(32.0f);
                                        if (ImGui::InputText("##fv", s.forVar, sizeof(s.forVar)))
                                                isModified_ = true;
                                        ImGui::SameLine();
                                        ImGui::TextDisabled("=");
                                        int64_t ff = s.forFrom, ft = s.forTo, fs = s.forStep;
                                        ImGui::SameLine();
                                        ImGui::SetNextItemWidth(36.0f);
                                        if (ImGui::InputScalar("##ff", ImGuiDataType_S64, &ff)) {
                                                s.forFrom   = ff;
                                                isModified_ = true;
                                        }
                                        ImGui::SameLine();
                                        ImGui::TextDisabled("..");
                                        ImGui::SameLine();
                                        ImGui::SetNextItemWidth(36.0f);
                                        if (ImGui::InputScalar("##ft", ImGuiDataType_S64, &ft)) {
                                                s.forTo     = ft;
                                                isModified_ = true;
                                        }
                                        ImGui::SameLine();
                                        ImGui::TextDisabled("+");
                                        ImGui::SameLine();
                                        ImGui::SetNextItemWidth(28.0f);
                                        if (ImGui::InputScalar("##fs", ImGuiDataType_S64, &fs)) {
                                                s.forStep   = fs;
                                                isModified_ = true;
                                        }
                                        break;
                                }
                                case SeqStepKind::Break:
                                        ImGui::TextDisabled("---");
                                        break;
                                default:
                                        break;
                        }

                        // ── Nested body for compound steps ────────────────────────────
                        if (depth < 4 &&
                            (s.kind == SeqStepKind::If || s.kind == SeqStepKind::While || s.kind == SeqStepKind::For)) {
                                drawBodySteps(s.body, depth + 1);

                                float subIndent = (depth + 1) * 14.0f;
                                if (s.kind == SeqStepKind::If) {
                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0);
                                        ImGui::Indent(subIndent);
                                        bool he = s.hasElse;
                                        if (ImGui::Checkbox(tr("else", "否则"), &he)) {
                                                s.hasElse   = he;
                                                isModified_ = true;
                                        }
                                        ImGui::Unindent(subIndent);
                                }

                                if (s.kind == SeqStepKind::If && s.hasElse) {
                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(1);
                                        ImGui::Indent(subIndent);
                                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", tr("else:", "否则:"));
                                        ImGui::Unindent(subIndent);

                                        drawBodySteps(s.elseBody, depth + 1);
                                }
                        }
                }

                ImGui::PopID();
        }

        if (stepToDelete >= 0) {
                steps.erase(steps.begin() + stepToDelete);
                isModified_ = true;
        }
        if (stepMoveSrc >= 0 && stepMoveDst >= 0 && stepMoveSrc != stepMoveDst) {
                auto moved = steps[stepMoveSrc];
                steps.erase(steps.begin() + stepMoveSrc);
                if (stepMoveDst > stepMoveSrc)
                        stepMoveDst--;
                steps.insert(steps.begin() + stepMoveDst, moved);
                isModified_ = true;
        }

        if (topLevel)
                ImGui::EndTable();
}

// ─── acceptSdkPayload ─────────────────────────────────────────────────────────

// Build a variable-write action from a single-channel drag payload.
static SequenceAction
actionFromChannel(const ChannelDropPayload *p)
{
        SequenceAction action;
        action.name      = p->name;
        action.addr      = p->addr;
        action.type      = Parser::strToDataType(p->type);
        action.port      = p->device;
        action.shmName   = p->shmName;
        action.typeOff   = p->typeOff;
        action.bitOffset = p->bitOffset;
        action.bitSize   = p->bitSize;
        action.isEnum    = (p->numEnums > 0);
        for (int e = 0; e < p->numEnums; ++e)
                action.enumDefs.push_back({p->enums[e].value, p->enums[e].name});
        action.targetValue = action.isEnum ? p->enums[0].name : "0";
        return action;
}

// Build an SDK-call op from a drag payload. Returns false if the source panel
// has since closed.
bool
SequenceEditor::makeSdkOp(const SdkDragPayload &pl, SeqOp &out)
{
        auto panel = findSdkPanel(pl.panelWinId);
        if (!panel)
                return false;
        out.kind           = SeqOpKind::Sdk;
        out.sdk.panelWinId = pl.panelWinId;
        out.sdk.isCFunc    = pl.isCFunc;
        out.sdk.isPython   = pl.isPython;
        out.sdk.classIdx   = pl.classIdx;
        out.sdk.methodIdx  = pl.methodIdx;
        out.sdk.objIdx     = pl.objIdx;
        if (pl.isPython) {
                out.sdk.pyFuncName = pl.pyName;
                out.sdk.label      = panel->getPyFuncLabel(pl.pyName);
                out.sdk.args.resize(panel->getPyFuncParamCount(pl.pyName));
        } else {
                out.sdk.label =
                    pl.isCFunc ? panel->getCFuncLabel(pl.methodIdx) : panel->getCallLabel(pl.classIdx, pl.methodIdx);
                int np = pl.isCFunc ? panel->getCFuncParamCount(pl.methodIdx) : panel->getParamCount(pl.classIdx, pl.methodIdx);
                out.sdk.args.resize(np);
        }
        return true;
}

// Within an active BeginDragDropTarget(): accept SDK functions / variables and
// forward each resulting op to `sink`. Returns true if anything was consumed.
// noSdk=true: only accept variable/channel drops (used by left sidebar).
bool
SequenceEditor::acceptOpDrops(const std::function<void(SeqOp)> &sink, bool noSdk)
{
        if (!noSdk) {
                if (const ImGuiPayload *dp = ImGui::AcceptDragDropPayload("SDK_CALL")) {
                        SeqOp op;
                        if (makeSdkOp(*static_cast<const SdkDragPayload *>(dp->Data), op)) {
                                sink(std::move(op));
                                return true;
                        }
                        return false;
                }
        }
        if (const ImGuiPayload *dp = ImGui::AcceptDragDropPayload("CHANNEL")) {
                sink({SeqOpKind::Write, actionFromChannel(static_cast<ChannelDropPayload *>(dp->Data)), {}});
                return true;
        }
        if (const ImGuiPayload *dp = ImGui::AcceptDragDropPayload("SYMBOL_CHANNEL")) {
                sink({SeqOpKind::Write, actionFromChannel(static_cast<ChannelDropPayload *>(dp->Data)), {}});
                return true;
        }
        if (const ImGuiPayload *dp = ImGui::AcceptDragDropPayload("STRUCT_CHANNEL")) {
                auto *sp = static_cast<StructChannelPayload *>(dp->Data);
                for (int j = 0; j < sp->count; ++j) {
                        SequenceAction action;
                        action.name      = sp->entries[j].name;
                        action.addr      = sp->entries[j].addr;
                        action.type      = Parser::strToDataType(sp->entries[j].type);
                        action.port      = sp->device;
                        action.shmName   = sp->shmName;
                        action.typeOff   = 0;
                        action.bitOffset = sp->entries[j].bitOffset;
                        action.bitSize   = sp->entries[j].bitSize;
                        action.isEnum    = (sp->entries[j].numEnums > 0);
                        for (int e = 0; e < sp->entries[j].numEnums; ++e)
                                action.enumDefs.push_back({sp->entries[j].enums[e].value, sp->entries[j].enums[e].name});
                        action.targetValue = action.isEnum ? sp->entries[j].enums[0].name : "0";
                        sink({SeqOpKind::Write, std::move(action), {}});
                }
                return sp->count > 0;
        }
        if (const ImGuiPayload *dp = ImGui::AcceptDragDropPayload("DND_CHANNEL_MOVE")) {
                auto           *m_p = static_cast<ChannelMovePayload *>(dp->Data);
                MonitorChannel *ch  = m_p->srcScope->findChannel(m_p->chName);
                if (ch) {
                        SequenceAction action;
                        action.name      = ch->getName();
                        action.addr      = ch->getAddr();
                        action.type      = Parser::strToDataType(ch->getType());
                        action.port      = ch->getDevice();
                        action.shmName   = ch->getShmRegionName();
                        action.typeOff   = 0;
                        action.bitOffset = ch->getBitOffset();
                        action.bitSize   = ch->getBitSize();
                        action.isEnum    = !ch->getEnums().empty();
                        for (const auto &e : ch->getEnums())
                                action.enumDefs.push_back({e.value, e.name});
                        action.targetValue = action.isEnum ? ch->getEnums()[0].name : "0";
                        sink({SeqOpKind::Write, std::move(action), {}});
                        return true;
                }
        }
        return false;
}

// Append an op to a step body (used by If/While/For): extend the trailing "do"
// step, or start a new one if the body is empty / ends with a non-"do" step.
static void
appendOpToBody(std::vector<SequenceStep> &body, SeqOp op)
{
        if (body.empty() || body.back().kind != SeqStepKind::Action)
                body.push_back(SequenceStep{}); // default kind == Action ("do")
        body.back().ops.push_back(std::move(op));
}

// Drop a dragged SDK function into a fresh or existing "do" step (used by the
// window-background and trailing drop zones). If stepIdx points at a "do" step
// the call is appended to it; otherwise it is wrapped in a new "do" step
// (inserted after stepIdx, or at the end when stepIdx < 0).
void
SequenceEditor::acceptSdkPayload(const void *data, int stepIdx)
{
        SeqOp op;
        if (!makeSdkOp(*static_cast<const SdkDragPayload *>(data), op))
                return;

        if (stepIdx >= 0 && stepIdx < (int)steps_.size() && steps_[stepIdx].kind == SeqStepKind::Action) {
                steps_[stepIdx].ops.push_back(std::move(op));
                selectedStep_ = stepIdx;
        } else {
                SequenceStep ns;
                ns.kind = SeqStepKind::Action;
                ns.ops.push_back(std::move(op));
                if (stepIdx < 0 || stepIdx >= (int)steps_.size()) {
                        steps_.push_back(std::move(ns));
                        selectedStep_ = (int)steps_.size() - 1;
                } else {
                        steps_.insert(steps_.begin() + stepIdx + 1, std::move(ns));
                        selectedStep_ = stepIdx + 1;
                }
        }
        isModified_ = true;
}

// ─── drawStepList ─────────────────────────────────────────────────────────────

void
SequenceEditor::moveStep(int src, int dst)
{
        if (src == dst || src < 0 || dst < 0 || src >= (int)steps_.size() || dst >= (int)steps_.size())
                return;
        SequenceStep moved = std::move(steps_[src]);
        steps_.erase(steps_.begin() + src);
        // After erase, indices above src shift down by one.
        if (dst > src)
                --dst;
        steps_.insert(steps_.begin() + dst, std::move(moved));
        selectedStep_ = dst;
        isModified_   = true;
}

void
SequenceEditor::runSingleStep(int idx)
{
        if (seqRunning_.load())
                return;
        if (idx < 0 || idx >= (int)steps_.size())
                return;

        seqStopReq_.store(false);
        seqDone_.store(false);
        seqRunning_.store(true);
        {
                std::lock_guard<std::mutex> lk(seqMtx_);
                seqLog_.clear();
        }
        if (seqThread_.joinable())
                seqThread_.join();
        seqThread_ = std::thread([this, idx]() {
                LOG_I("[SeqEditor] single-step run: idx=%d", idx);
                try {
                        SeqCtx ctx{this};
                        if (idx < (int)steps_.size())
                                seqExecStep(steps_[idx], ctx);
                } catch (const std::exception &ex) {
                        LOG_E("[SeqEditor] single-step exception: %s", ex.what());
                        SeqLogEntry le;
                        le.tsMs  = nowStampMs();
                        le.desc  = "EXCEPTION";
                        le.value = ex.what();
                        le.ok    = false;
                        std::lock_guard<std::mutex> lk(seqMtx_);
                        seqLog_.push_back(std::move(le));
                } catch (...) {
                        LOG_E("[SeqEditor] single-step unknown exception");
                }
                seqRunning_.store(false);
                seqDone_.store(true);
        });
}

void
SequenceEditor::drawStepList()
{
        bool running = seqRunning_.load();

        // Deferred reorder: applied after the loop so we never mutate steps_ mid-iteration.
        int moveSrc = -1, moveDst = -1;

        for (int i = 0; i < (int)steps_.size(); ++i) {
                SequenceStep &step = steps_[i];
                ImGui::PushID(i);

                char        label[128];
                const char *kl = tr(kindLabel(step.kind), kindLabelCn(step.kind));
                switch (step.kind) {
                        case SeqStepKind::Action:
                                if (!step.name.empty())
                                        snprintf(label,
                                                 sizeof(label),
                                                 "%d  %s (%d)",
                                                 i + 1,
                                                 step.name.c_str(),
                                                 (int)step.ops.size());
                                else
                                        snprintf(label, sizeof(label), "%d  %s (%d)", i + 1, kl, (int)step.ops.size());
                                break;
                        case SeqStepKind::Sleep:
                                snprintf(label, sizeof(label), "%d  %s  %d ms", i + 1, kl, step.sleepMs);
                                break;
                        case SeqStepKind::If:
                                snprintf(label,
                                         sizeof(label),
                                         "%d  %s  (%s %s %lld)",
                                         i + 1,
                                         kl,
                                         step.condVar[0] ? step.condVar : "?",
                                         step.condOp,
                                         (long long)step.condVal);
                                break;
                        case SeqStepKind::While:
                                snprintf(label,
                                         sizeof(label),
                                         "%d  %s  (%s %s %lld)",
                                         i + 1,
                                         kl,
                                         step.condVar[0] ? step.condVar : "?",
                                         step.condOp,
                                         (long long)step.condVal);
                                break;
                        case SeqStepKind::For:
                                snprintf(label,
                                         sizeof(label),
                                         "%d  %s  %s=%lld..%lld",
                                         i + 1,
                                         kl,
                                         step.forVar,
                                         (long long)step.forFrom,
                                         (long long)step.forTo);
                                break;
                        case SeqStepKind::Break:
                                snprintf(label, sizeof(label), "%d  %s", i + 1, kl);
                                break;
                        default:
                                snprintf(label, sizeof(label), "%d  %s", i + 1, kl);
                                break;
                }

                // Colour hints per kind
                if (step.kind == SeqStepKind::Sleep)
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
                else if (step.kind == SeqStepKind::If || step.kind == SeqStepKind::While || step.kind == SeqStepKind::For)
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.4f, 1.0f));
                else if (step.kind == SeqStepKind::SdkCall)
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 1.0f, 0.7f, 1.0f));
                else if (step.kind == SeqStepKind::Break)
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                else
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));

                if (ImGui::Selectable(label, selectedStep_ == i, ImGuiSelectableFlags_AllowOverlap)) {
                        selectedStep_ = i;
                }
                ImGui::PopStyleColor();

                // Drag source: reorder this step by dragging it onto another.
                if (!running && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                        ImGui::SetDragDropPayload("SEQ_STEP_MOVE", &i, sizeof(i));
                        ImGui::TextUnformatted(label);
                        ImGui::EndDragDropSource();
                }

                // Drop target on step item: reorder (SEQ_STEP_MOVE), else variable/SDK op drops.
                if (!running && ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload *mv = ImGui::AcceptDragDropPayload("SEQ_STEP_MOVE")) {
                                if (mv->DataSize == (int)sizeof(int)) {
                                        moveSrc = *static_cast<const int *>(mv->Data);
                                        moveDst = i;
                                }
                        } else {
                                SequenceStep &st  = steps_[i];
                                bool          mod = false;
                                if (st.kind == SeqStepKind::Action)
                                        mod = acceptOpDrops([&](SeqOp op) { st.ops.push_back(std::move(op)); }, true);
                                else if (st.kind == SeqStepKind::If || st.kind == SeqStepKind::While ||
                                         st.kind == SeqStepKind::For)
                                        mod = acceptOpDrops([&](SeqOp op) { appendOpToBody(st.body, std::move(op)); }, true);
                                if (mod) {
                                        selectedStep_ = i;
                                        isModified_   = true;
                                }
                        }
                        ImGui::EndDragDropTarget();
                }

                // Per-step single-run button (right-aligned overlay on the selectable).
                if (!running) {
                        ImGui::SameLine();
                        float availW = ImGui::GetContentRegionAvail().x;
                        if (availW > 28.0f)
                                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + availW - 24.0f);
                        if (ImGui::SmallButton(">##run"))
                                runSingleStep(i);
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", tr("Run this step only", "仅运行此步骤"));
                }

                ImGui::PopID();
        }

        if (moveSrc >= 0)
                moveStep(moveSrc, moveDst);
}

// ─── drawStepDetail ───────────────────────────────────────────────────────────

void
SequenceEditor::drawStepDetail(SequenceStep &step)
{
        static const char *ops[] = {"==", "!=", "<", ">", "<=", ">="};

        // ── Name ──
        char nameBuf[128];
        strncpy(nameBuf, step.name.c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        ImGui::SetNextItemWidth(200);
        if (ImGui::InputTextWithHint("##name", tr("Step Name", "步骤名称"), nameBuf, sizeof(nameBuf))) {
                step.name   = nameBuf;
                isModified_ = true;
        }
        ImGui::SameLine();
        if (ui::Button(tr("Remove Step", "删除步骤"), ui::BtnStyle::Danger)) {
                steps_.erase(steps_.begin() + selectedStep_);
                if (selectedStep_ >= (int)steps_.size())
                        selectedStep_ = (int)steps_.size() - 1;
                isModified_ = true;
                return;
        }

        ImGui::Text(tr("Step %d — %s", "步骤 %d — %s"), selectedStep_ + 1, tr(kindLabel(step.kind), kindLabelCn(step.kind)));
        ImGui::Separator();

        // ── Kind selector ──
        {
                static const char *kinds[]   = {"do", "delay", "if", "While", "For", "Break"};
                static const char *kindsCn[] = {"执行", "延时", "判断", "循环", "计数", "退出"};
                int                ki        = (int)step.kind;
                ImGui::SetNextItemWidth(100.0f);
                if (ImGui::Combo(tr("Kind", "类型"), &ki, g_lang == Lang::ZH ? kindsCn : kinds, 6)) {
                        step.kind   = (SeqStepKind)ki;
                        isModified_ = true;
                }
        }

        ImGui::Spacing();

        // ── Kind-specific fields ──
        switch (step.kind) {

                case SeqStepKind::Action: {
                        ImGui::Text("%s", tr("Actions (drag variables / SDK functions):", "动作（拖入变量 / SDK 函数）："));
                        if (ImGui::BeginChild("OpList", ImVec2(0, 0), true)) {
                                static ImGuiTableFlags opTblFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                                    ImGuiTableFlags_SizingStretchProp |
                                                                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
                                if (ImGui::BeginTable("##optbl", 3, opTblFlags)) {
                                        ImGui::TableSetupScrollFreeze(0, 1);
                                        ImGui::TableSetupColumn(tr("#", "#"), ImGuiTableColumnFlags_WidthFixed, 70.0f);
                                        ImGui::TableSetupColumn(tr("Name", "名称"), ImGuiTableColumnFlags_WidthStretch, 0.35f);
                                        ImGui::TableSetupColumn(
                                            tr("Value / Args", "值/参数"), ImGuiTableColumnFlags_WidthStretch, 0.65f);
                                        ImGui::TableHeadersRow();

                                        int toDelete  = -1;
                                        int opMoveSrc = -1, opMoveDst = -1;
                                        for (int i = 0; i < (int)step.ops.size(); ++i) {
                                                ImGui::PushID(i);
                                                SeqOp &op = step.ops[i];

                                                if (op.kind == SeqOpKind::Write) {
                                                        SequenceAction &a = op.action;
                                                        ImGui::TableNextRow();

                                                        ImGui::TableSetColumnIndex(0);
                                                        ImGui::SmallButton("=##opgrip");
                                                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                                                                ImGui::SetDragDropPayload("OP_ROW_MOVE", &i, sizeof(i));
                                                                ImGui::TextUnformatted(a.name.c_str());
                                                                ImGui::EndDragDropSource();
                                                        }
                                                        if (ImGui::IsItemHovered())
                                                                ImGui::SetTooltip("%s",
                                                                                  tr("Drag to reorder", "拖动以调整顺序"));
                                                        if (ImGui::BeginDragDropTarget()) {
                                                                if (const ImGuiPayload *mv =
                                                                        ImGui::AcceptDragDropPayload("OP_ROW_MOVE"))
                                                                        if (mv->DataSize == (int)sizeof(int)) {
                                                                                opMoveSrc = *static_cast<const int *>(mv->Data);
                                                                                opMoveDst = i;
                                                                        }
                                                                ImGui::EndDragDropTarget();
                                                        }
                                                        ImGui::OpenPopupOnItemClick("##op_ctx",
                                                                                    ImGuiPopupFlags_MouseButtonRight);
                                                        if (ImGui::BeginPopup("##op_ctx")) {
                                                                if (ImGui::MenuItem(tr("Delete", "删除")))
                                                                        toDelete = i;
                                                                ImGui::EndPopup();
                                                        }
                                                        ImGui::SameLine();
                                                        ImGui::Text("%d", i + 1);

                                                        ImGui::TableSetColumnIndex(1);
                                                        ImGui::AlignTextToFramePadding();
                                                        ImGui::TextUnformatted(a.name.c_str());

                                                        ImGui::TableSetColumnIndex(2);
                                                        ImGui::SetNextItemWidth(-FLT_MIN);
                                                        if (a.isEnum && !a.enumDefs.empty()) {
                                                                std::string preview = a.targetValue;
                                                                for (const auto &[val, nm] : a.enumDefs)
                                                                        if (nm == preview) {
                                                                                preview += " (" + std::to_string(val) + ")";
                                                                                break;
                                                                        }
                                                                if (ImGui::BeginCombo("##v", preview.c_str())) {
                                                                        for (const auto &[val, nm] : a.enumDefs) {
                                                                                bool        sel = (a.targetValue == nm);
                                                                                std::string lbl =
                                                                                    nm + " (" + std::to_string(val) + ")";
                                                                                if (ImGui::Selectable(lbl.c_str(), sel)) {
                                                                                        a.targetValue = nm;
                                                                                        isModified_   = true;
                                                                                }
                                                                                if (sel)
                                                                                        ImGui::SetItemDefaultFocus();
                                                                        }
                                                                        ImGui::EndCombo();
                                                                }
                                                        } else {
                                                                char vbuf[64];
                                                                strncpy(vbuf, a.targetValue.c_str(), sizeof(vbuf) - 1);
                                                                vbuf[sizeof(vbuf) - 1] = '\0';
                                                                if (ImGui::InputText("##v", vbuf, sizeof(vbuf))) {
                                                                        a.targetValue = vbuf;
                                                                        isModified_   = true;
                                                                }
                                                        }

                                                } else {
                                                        // SDK call op
                                                        auto panel   = findSdkPanel(op.sdk.panelWinId);
                                                        int  nParams = 0;
                                                        if (panel) {
                                                                if (op.sdk.isPython)
                                                                        nParams = panel->getPyFuncParamCount(op.sdk.pyFuncName);
                                                                else if (op.sdk.isCFunc)
                                                                        nParams = panel->getCFuncParamCount(op.sdk.methodIdx);
                                                                else
                                                                        nParams = panel->getParamCount(op.sdk.classIdx,
                                                                                                       op.sdk.methodIdx);
                                                        }
                                                        op.sdk.args.resize(nParams);

                                                        ImGui::TableNextRow();

                                                        ImGui::TableSetColumnIndex(0);
                                                        ImGui::SmallButton("=##opgrip");
                                                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                                                                ImGui::SetDragDropPayload("OP_ROW_MOVE", &i, sizeof(i));
                                                                ImGui::TextUnformatted(op.sdk.label.c_str());
                                                                ImGui::EndDragDropSource();
                                                        }
                                                        if (ImGui::IsItemHovered())
                                                                ImGui::SetTooltip("%s",
                                                                                  tr("Drag to reorder", "拖动以调整顺序"));
                                                        if (ImGui::BeginDragDropTarget()) {
                                                                if (const ImGuiPayload *mv =
                                                                        ImGui::AcceptDragDropPayload("OP_ROW_MOVE"))
                                                                        if (mv->DataSize == (int)sizeof(int)) {
                                                                                opMoveSrc = *static_cast<const int *>(mv->Data);
                                                                                opMoveDst = i;
                                                                        }
                                                                ImGui::EndDragDropTarget();
                                                        }
                                                        ImGui::OpenPopupOnItemClick("##op_ctx",
                                                                                    ImGuiPopupFlags_MouseButtonRight);
                                                        if (ImGui::BeginPopup("##op_ctx")) {
                                                                if (ImGui::MenuItem(tr("Delete", "删除")))
                                                                        toDelete = i;
                                                                ImGui::EndPopup();
                                                        }
                                                        ImGui::SameLine();
                                                        ImGui::Text("%d", i + 1);

                                                        ImGui::TableSetColumnIndex(1);
                                                        bool open = ImGui::TreeNodeEx("##sdkn",
                                                                                      ImGuiTreeNodeFlags_DefaultOpen,
                                                                                      "%s",
                                                                                      op.sdk.label.c_str());

                                                        if (open) {
                                                                if (!panel) {
                                                                        ImGui::TableNextRow();
                                                                        ImGui::TableSetColumnIndex(1);
                                                                        ImGui::Indent(16.0f);
                                                                        ImGui::TextDisabled(tr("SDK panel (id=%d) closed.",
                                                                                               "SDK 窗口 (id=%d) 已关闭。"),
                                                                                            op.sdk.panelWinId);
                                                                        ImGui::Unindent(16.0f);
                                                                } else {
                                                                        // Object selector for C++ methods
                                                                        if (!op.sdk.isCFunc && !op.sdk.isPython) {
                                                                                auto objs = panel->listObjects(op.sdk.classIdx);
                                                                                const char *curLabel = tr("(none)", "(无)");
                                                                                for (const auto &o : objs)
                                                                                        if (o.idx == op.sdk.objIdx) {
                                                                                                curLabel = o.label.c_str();
                                                                                                break;
                                                                                        }

                                                                                ImGui::TableNextRow();
                                                                                ImGui::TableSetColumnIndex(0);
                                                                                {
                                                                                        std::string cn = panel->getClassName(
                                                                                            op.sdk.classIdx);
                                                                                        ImGui::TextUnformatted(
                                                                                            cn.empty() ? "?" : cn.c_str());
                                                                                }
                                                                                ImGui::TableSetColumnIndex(1);
                                                                                ImGui::Indent(16.0f);
                                                                                ImGui::TextUnformatted(tr("Object", "对象"));
                                                                                ImGui::Unindent(16.0f);

                                                                                ImGui::TableSetColumnIndex(2);
                                                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                                                if (ImGui::BeginCombo("##objsel", curLabel)) {
                                                                                        if (ImGui::Selectable(
                                                                                                tr("[+ New Object]",
                                                                                                   "[+ 新建对象]"),
                                                                                                false)) {
                                                                                                int ni = panel->newObject(
                                                                                                    op.sdk.classIdx);
                                                                                                if (ni >= 0) {
                                                                                                        op.sdk.objIdx = ni;
                                                                                                        isModified_   = true;
                                                                                                }
                                                                                        }
                                                                                        for (const auto &o : objs) {
                                                                                                bool sel =
                                                                                                    (o.idx == op.sdk.objIdx);
                                                                                                char combo_label[128];
                                                                                                snprintf(combo_label,
                                                                                                         sizeof(combo_label),
                                                                                                         "%s  (%s)",
                                                                                                         o.label.c_str(),
                                                                                                         o.className.c_str());
                                                                                                if (ImGui::Selectable(
                                                                                                        combo_label, sel)) {
                                                                                                        op.sdk.objIdx = o.idx;
                                                                                                        isModified_   = true;
                                                                                                }
                                                                                                if (sel)
                                                                                                        ImGui::
                                                                                                            SetItemDefaultFocus();
                                                                                        }
                                                                                        ImGui::EndCombo();
                                                                                }
                                                                        }

                                                                        // Arg rows
                                                                        std::vector<std::string> localVars;
                                                                        if (panel->onListLocalVars_)
                                                                                localVars = panel->onListLocalVars_();

                                                                        for (int p = 0; p < nParams; ++p) {
                                                                                ImGui::PushID(p);
                                                                                std::string pname =
                                                                                    op.sdk.isPython ? panel->getPyFuncParamName(
                                                                                                          op.sdk.pyFuncName, p)
                                                                                    : op.sdk.isCFunc
                                                                                        ? panel->getCFuncParamName(
                                                                                              op.sdk.methodIdx, p)
                                                                                        : panel->getParamName(op.sdk.classIdx,
                                                                                                              op.sdk.methodIdx,
                                                                                                              p);
                                                                                std::string ptype =
                                                                                    op.sdk.isCFunc
                                                                                        ? panel->getCFuncParamRawType(
                                                                                              op.sdk.methodIdx, p)
                                                                                        : panel->getParamRawType(
                                                                                              op.sdk.classIdx,
                                                                                              op.sdk.methodIdx,
                                                                                              p);
                                                                                bool isPtr = op.sdk.isCFunc
                                                                                                 ? panel->isCFuncParamPtrOrRef(
                                                                                                       op.sdk.methodIdx, p)
                                                                                                 : panel->isParamPtrOrRef(
                                                                                                       op.sdk.classIdx,
                                                                                                       op.sdk.methodIdx,
                                                                                                       p);
                                                                                // Struct param → expandable field editor.
                                                                                if (drawSdkStructArgRow(panel.get(),
                                                                                                        op.sdk,
                                                                                                        p,
                                                                                                        pname,
                                                                                                        ptype)) {
                                                                                        ImGui::PopID();
                                                                                        continue;
                                                                                }

                                                                                ImGui::TableNextRow();

                                                                                ImGui::TableSetColumnIndex(0);
                                                                                ImGui::TextUnformatted(
                                                                                    ptype.empty() ? "?" : ptype.c_str());
                                                                                ImGui::TableSetColumnIndex(1);
                                                                                ImGui::Indent(16.0f);
                                                                                if (!pname.empty())
                                                                                        ImGui::TextUnformatted(pname.c_str());
                                                                                else
                                                                                        ImGui::Text(tr("Arg %d", "参数%d"), p);
                                                                                ImGui::Unindent(16.0f);

                                                                                ImGui::TableSetColumnIndex(2);
                                                                                char argBuf[512]{};
                                                                                strncpy(argBuf,
                                                                                        op.sdk.args[p].c_str(),
                                                                                        sizeof(argBuf) - 1);
                                                                                bool  hasPicker2 = isPtr && !localVars.empty();
                                                                                bool  hasCreate2 = (bool)onAddLocalVar_;
                                                                                float inputW =
                                                                                    (hasPicker2 && hasCreate2)   ? -82.0f
                                                                                    : (hasPicker2 || hasCreate2) ? -35.0f
                                                                                                                 : -FLT_MIN;
                                                                                const CEnumDecl *ed2 =
                                                                                    (!op.sdk.isPython)
                                                                                        ? panel->getParamEnumDecl(
                                                                                              op.sdk.isCFunc,
                                                                                              op.sdk.classIdx,
                                                                                              op.sdk.methodIdx,
                                                                                              p)
                                                                                        : nullptr;
                                                                                if (ed2 && !ed2->values.empty()) {
                                                                                        int64_t curVal2 =
                                                                                            op.sdk.args[p].empty()
                                                                                                ? 0
                                                                                                : strtoll(
                                                                                                      op.sdk.args[p].c_str(),
                                                                                                      nullptr,
                                                                                                      0);
                                                                                        const char *preview2 =
                                                                                            ed2->values[0].name.c_str();
                                                                                        int curIdx2 = 0;
                                                                                        for (int ei = 0;
                                                                                             ei < (int)ed2->values.size();
                                                                                             ++ei) {
                                                                                                if (ed2->values[ei].value ==
                                                                                                    curVal2) {
                                                                                                        preview2 =
                                                                                                            ed2->values[ei]
                                                                                                                .name.c_str();
                                                                                                        curIdx2 = ei;
                                                                                                        break;
                                                                                                }
                                                                                        }
                                                                                        ImGui::SetNextItemWidth(
                                                                                            ImGui::GetContentRegionAvail().x -
                                                                                            48.0f);
                                                                                        char cmId2[16];
                                                                                        snprintf(
                                                                                            cmId2, sizeof(cmId2), "##ec%d", p);
                                                                                        if (ImGui::BeginCombo(cmId2,
                                                                                                              preview2)) {
                                                                                                for (int ei = 0;
                                                                                                     ei <
                                                                                                     (int)ed2->values.size();
                                                                                                     ++ei) {
                                                                                                        bool sel2 =
                                                                                                            (ei == curIdx2);
                                                                                                        char evLbl[128];
                                                                                                        snprintf(
                                                                                                            evLbl,
                                                                                                            sizeof(evLbl),
                                                                                                            "%s (%lld)",
                                                                                                            ed2->values[ei]
                                                                                                                .name.c_str(),
                                                                                                            (long long)ed2
                                                                                                                ->values[ei]
                                                                                                                .value);
                                                                                                        if (ImGui::Selectable(
                                                                                                                evLbl, sel2)) {
                                                                                                                char numBuf[32];
                                                                                                                snprintf(
                                                                                                                    numBuf,
                                                                                                                    sizeof(
                                                                                                                        numBuf),
                                                                                                                    "%lld",
                                                                                                                    (long long)ed2
                                                                                                                        ->values
                                                                                                                            [ei]
                                                                                                                        .value);
                                                                                                                op.sdk.args[p] =
                                                                                                                    numBuf;
                                                                                                                isModified_ =
                                                                                                                    true;
                                                                                                        }
                                                                                                        if (sel2)
                                                                                                                ImGui::
                                                                                                                    SetItemDefaultFocus();
                                                                                                }
                                                                                                ImGui::EndCombo();
                                                                                        }
                                                                                        drawSdkArgButtons(panel.get(),
                                                                                                          op.sdk,
                                                                                                          p,
                                                                                                          pname,
                                                                                                          ptype,
                                                                                                          isPtr);
                                                                                } else {
                                                                                        ImGui::SetNextItemWidth(inputW);
                                                                                        if (ImGui::InputText("##arg",
                                                                                                             argBuf,
                                                                                                             sizeof(argBuf))) {
                                                                                                op.sdk.args[p] = argBuf;
                                                                                                isModified_    = true;
                                                                                        }
                                                                                        if (!ptype.empty() &&
                                                                                            ImGui::IsItemHovered())
                                                                                                ImGui::SetTooltip(
                                                                                                    "%s", ptype.c_str());
                                                                                        if (hasCreate2) {
                                                                                                ImGui::SameLine();
                                                                                                char cbtn[16];
                                                                                                snprintf(cbtn,
                                                                                                         sizeof(cbtn),
                                                                                                         "+##c%d",
                                                                                                         p);
                                                                                                if (ImGui::SmallButton(cbtn)) {
                                                                                                        std::string pn =
                                                                                                            pname.empty()
                                                                                                                ? "arg" +
                                                                                                                      std::
                                                                                                                          to_string(
                                                                                                                              p)
                                                                                                                : pname;
                                                                                                        DataType dt =
                                                                                                            ptrTypeFromRaw(
                                                                                                                ptype);
                                                                                                        size_t sz =
                                                                                                            Parser::typeBytes(
                                                                                                                dt);
                                                                                                        if (sz == 0)
                                                                                                                sz = 4;
                                                                                                        const CStructDecl *sd =
                                                                                                            panel->getParamStructDecl(
                                                                                                                op.sdk.isCFunc,
                                                                                                                op.sdk.classIdx,
                                                                                                                op.sdk
                                                                                                                    .methodIdx,
                                                                                                                p);
                                                                                                        if (sd &&
                                                                                                            onAddLocalStructVar_)
                                                                                                                onAddLocalStructVar_(
                                                                                                                    pn,
                                                                                                                    structDeclToFields(
                                                                                                                        *sd),
                                                                                                                    sd->totalSize);
                                                                                                        else
                                                                                                                onAddLocalVar_(
                                                                                                                    pn, dt, sz);
                                                                                                        if (isPtr)
                                                                                                                op.sdk.args[p] =
                                                                                                                    "&" + pn;
                                                                                                        isModified_ = true;
                                                                                                }
                                                                                                if (ImGui::IsItemHovered())
                                                                                                        ImGui::SetTooltip(
                                                                                                            "%s",
                                                                                                            tr("Create LOCAL "
                                                                                                               "variable",
                                                                                                               "创建 LOCAL "
                                                                                                               "变量"));
                                                                                        }
                                                                                        if (hasPicker2) {
                                                                                                ImGui::SameLine();
                                                                                                char btnId[16];
                                                                                                snprintf(btnId,
                                                                                                         sizeof(btnId),
                                                                                                         "v##p%d",
                                                                                                         p);
                                                                                                if (ImGui::Button(btnId)) {
                                                                                                        char popupId[32];
                                                                                                        snprintf(
                                                                                                            popupId,
                                                                                                            sizeof(popupId),
                                                                                                            "##vp%d",
                                                                                                            p);
                                                                                                        ImGui::OpenPopup(
                                                                                                            popupId);
                                                                                                }
                                                                                                char popupId[32];
                                                                                                snprintf(popupId,
                                                                                                         sizeof(popupId),
                                                                                                         "##vp%d",
                                                                                                         p);
                                                                                                if (ImGui::BeginPopup(
                                                                                                        popupId)) {
                                                                                                        for (const auto &vn :
                                                                                                             localVars)
                                                                                                                if (ImGui::Selectable(
                                                                                                                        vn.c_str())) {
                                                                                                                        op.sdk.args
                                                                                                                            [p] =
                                                                                                                            "&" +
                                                                                                                            vn;
                                                                                                                        isModified_ =
                                                                                                                            true;
                                                                                                                }
                                                                                                        ImGui::EndPopup();
                                                                                                }
                                                                                        }
                                                                                }
                                                                                ImGui::PopID();
                                                                        }
                                                                        // Return value → LOCAL variable row
                                                                        {
                                                                                std::string retT2 =
                                                                                    panel->getCallReturnType(op.sdk.isCFunc,
                                                                                                             op.sdk.isPython,
                                                                                                             op.sdk.classIdx,
                                                                                                             op.sdk.methodIdx,
                                                                                                             op.sdk.pyFuncName);
                                                                                ImGui::TableNextRow();
                                                                                ImGui::TableSetColumnIndex(0);
                                                                                ImGui::TextUnformatted(
                                                                                    retT2.empty() ? "?" : retT2.c_str());
                                                                                ImGui::TableSetColumnIndex(1);
                                                                                ImGui::Indent(16.0f);
                                                                                ImGui::TextUnformatted(tr("return", "返回值"));
                                                                                ImGui::Unindent(16.0f);
                                                                                ImGui::TableSetColumnIndex(2);
                                                                                char rvBuf[32]{};
                                                                                strncpy(
                                                                                    rvBuf, op.sdk.resultVar, sizeof(rvBuf) - 1);
                                                                                bool  rv2HasPicker = !localVars.empty();
                                                                                bool  rv2HasCreate = (bool)onAddLocalVar_;
                                                                                float rvW =
                                                                                    (rv2HasPicker && rv2HasCreate)   ? -82.0f
                                                                                    : (rv2HasPicker || rv2HasCreate) ? -35.0f
                                                                                                                     : -FLT_MIN;
                                                                                ImGui::SetNextItemWidth(rvW);
                                                                                if (ImGui::InputText(
                                                                                        "##rv2", rvBuf, sizeof(rvBuf))) {
                                                                                        strncpy(op.sdk.resultVar,
                                                                                                rvBuf,
                                                                                                sizeof(op.sdk.resultVar) - 1);
                                                                                        isModified_ = true;
                                                                                }
                                                                                if (rv2HasCreate) {
                                                                                        ImGui::SameLine();
                                                                                        if (ImGui::SmallButton("+##rv2c")) {
                                                                                                std::string vn =
                                                                                                    (op.sdk.resultVar[0] !=
                                                                                                     '\0')
                                                                                                        ? op.sdk.resultVar
                                                                                                        : "result";
                                                                                                onAddLocalVar_(vn,
                                                                                                               DataType::I64,
                                                                                                               sizeof(int64_t));
                                                                                                strncpy(
                                                                                                    op.sdk.resultVar,
                                                                                                    vn.c_str(),
                                                                                                    sizeof(op.sdk.resultVar) -
                                                                                                        1);
                                                                                                isModified_ = true;
                                                                                        }
                                                                                        if (ImGui::IsItemHovered())
                                                                                                ImGui::SetTooltip(
                                                                                                    "%s",
                                                                                                    tr("Create LOCAL variable "
                                                                                                       "for return value",
                                                                                                       "为返回值创建 LOCAL "
                                                                                                       "变量"));
                                                                                }
                                                                                if (rv2HasPicker) {
                                                                                        ImGui::SameLine();
                                                                                        if (ImGui::Button("v##rv2pick"))
                                                                                                ImGui::OpenPopup("##rv2pop");
                                                                                        if (ImGui::BeginPopup("##rv2pop")) {
                                                                                                for (const auto &vn : localVars)
                                                                                                        if (ImGui::Selectable(
                                                                                                                vn.c_str())) {
                                                                                                                strncpy(
                                                                                                                    op.sdk
                                                                                                                        .resultVar,
                                                                                                                    vn.c_str(),
                                                                                                                    sizeof(
                                                                                                                        op.sdk
                                                                                                                            .resultVar) -
                                                                                                                        1);
                                                                                                                isModified_ =
                                                                                                                    true;
                                                                                                        }
                                                                                                ImGui::EndPopup();
                                                                                        }
                                                                                }
                                                                        }
                                                                }
                                                                ImGui::TreePop();
                                                        }
                                                }
                                                ImGui::PopID();
                                        }

                                        if (step.ops.empty()) {
                                                ImGui::TableNextRow();
                                                ImGui::TableSetColumnIndex(1);
                                                ImGui::TextDisabled(
                                                    "%s",
                                                    tr("Drag variables or SDK functions here.", "把变量或 SDK 函数拖到这里。"));
                                        }

                                        if (opMoveSrc >= 0 && opMoveSrc != opMoveDst && opMoveSrc < (int)step.ops.size() &&
                                            opMoveDst < (int)step.ops.size()) {
                                                SeqOp moved = std::move(step.ops[opMoveSrc]);
                                                step.ops.erase(step.ops.begin() + opMoveSrc);
                                                if (opMoveDst > opMoveSrc)
                                                        --opMoveDst;
                                                step.ops.insert(step.ops.begin() + opMoveDst, std::move(moved));
                                                isModified_ = true;
                                        }
                                        if (toDelete >= 0) {
                                                step.ops.erase(step.ops.begin() + toDelete);
                                                isModified_ = true;
                                        }

                                        ImGui::EndTable();
                                }
                        }
                        ImGui::EndChild();
                        // The body child is a drop target for SDK functions and variables.
                        if (ImGui::BeginDragDropTarget()) {
                                if (acceptOpDrops([&](SeqOp op) { step.ops.push_back(std::move(op)); }))
                                        isModified_ = true;
                                ImGui::EndDragDropTarget();
                        }
                        break;
                }

                case SeqStepKind::Sleep:
                        ImGui::SetNextItemWidth(120.0f);
                        if (ImGui::InputInt(tr("Duration (ms)", "时长 (ms)"), &step.sleepMs, 100, 1000)) {
                                if (step.sleepMs < 0)
                                        step.sleepMs = 0;
                                isModified_ = true;
                        }
                        break;

                case SeqStepKind::If:
                case SeqStepKind::While: {
                        ImGui::Text("%s", tr("Condition:", "条件："));
                        ImGui::SetNextItemWidth(120.0f);
                        if (ImGui::InputTextWithHint(tr("Variable##cvar", "变量##cvar"),
                                                     tr("LOCAL var name", "LOCAL 变量名"),
                                                     step.condVar,
                                                     sizeof(step.condVar)))
                                isModified_ = true;
                        ImGui::SameLine();
                        int opi = 0;
                        for (int oi = 0; oi < 6; ++oi)
                                if (strcmp(step.condOp, ops[oi]) == 0) {
                                        opi = oi;
                                        break;
                                }
                        ImGui::SetNextItemWidth(56.0f);
                        if (ImGui::Combo("##op", &opi, ops, 6)) {
                                strncpy(step.condOp, ops[opi], sizeof(step.condOp) - 1);
                                isModified_ = true;
                        }
                        ImGui::SameLine();
                        int64_t cv = step.condVal;
                        ImGui::SetNextItemWidth(80.0f);
                        if (ImGui::InputScalar(tr("Value##cval", "值##cval"), ImGuiDataType_S64, &cv)) {
                                step.condVal = cv;
                                isModified_  = true;
                        }
                        ImGui::Spacing();

                        if (step.kind == SeqStepKind::If) {
                                bool he = step.hasElse;
                                if (ImGui::Checkbox(tr("Has else branch", "有 else 分支"), &he)) {
                                        step.hasElse = he;
                                        isModified_  = true;
                                }
                        }

                        // ── Body ──
                        ImGui::Text("%s", tr("Body:", "动作："));
                        if (ImGui::BeginChild("BodyList", ImVec2(0, step.hasElse ? -160.0f : 0.0f), true)) {
                                drawBodySteps(step.body, 0);
                        }
                        ImGui::EndChild();
                        // Dropping a variable / SDK function here adds it as a nested "do" step.
                        if (ImGui::BeginDragDropTarget()) {
                                if (acceptOpDrops([&](SeqOp op) { appendOpToBody(step.body, std::move(op)); }))
                                        isModified_ = true;
                                ImGui::EndDragDropTarget();
                        }

                        if (step.kind == SeqStepKind::If && step.hasElse) {
                                ImGui::Spacing();
                                ImGui::Text("%s", tr("Else:", "Else："));
                                if (ImGui::BeginChild("ElseList", ImVec2(0, -60.0f), true)) {
                                        drawBodySteps(step.elseBody, 0);
                                }
                                ImGui::EndChild();
                                if (ImGui::BeginDragDropTarget()) {
                                        if (acceptOpDrops([&](SeqOp op) { appendOpToBody(step.elseBody, std::move(op)); }))
                                                isModified_ = true;
                                        ImGui::EndDragDropTarget();
                                }
                                if (ImGui::Button(tr("+ Add Else Step", "+ 添加 Else 步骤"))) {
                                        step.elseBody.push_back(SequenceStep{});
                                        isModified_ = true;
                                }
                        }
                        break;
                }

                case SeqStepKind::For: {
                        ImGui::SetNextItemWidth(80.0f);
                        if (ImGui::InputText(tr("Variable##fvar", "变量##fvar"), step.forVar, sizeof(step.forVar)))
                                isModified_ = true;
                        int64_t ff = step.forFrom, ft = step.forTo, fs = step.forStep;
                        ImGui::SetNextItemWidth(80.0f);
                        if (ImGui::InputScalar(tr("From", "起始"), ImGuiDataType_S64, &ff)) {
                                step.forFrom = ff;
                                isModified_  = true;
                        }
                        ImGui::SetNextItemWidth(80.0f);
                        if (ImGui::InputScalar(tr("To (exclusive)", "结束（不含）"), ImGuiDataType_S64, &ft)) {
                                step.forTo  = ft;
                                isModified_ = true;
                        }
                        ImGui::SetNextItemWidth(80.0f);
                        if (ImGui::InputScalar(tr("Step", "步长"), ImGuiDataType_S64, &fs)) {
                                step.forStep = fs;
                                isModified_  = true;
                        }
                        ImGui::Spacing();
                        ImGui::Text("%s", tr("Body:", "动作："));
                        if (ImGui::BeginChild("ForBody", ImVec2(0, 0.0f), true)) {
                                drawBodySteps(step.body, 0);
                        }
                        ImGui::EndChild();
                        if (ImGui::BeginDragDropTarget()) {
                                if (acceptOpDrops([&](SeqOp op) { appendOpToBody(step.body, std::move(op)); }))
                                        isModified_ = true;
                                ImGui::EndDragDropTarget();
                        }
                        break;
                }

                case SeqStepKind::Break:
                        ImGui::TextDisabled(
                            "%s", tr("Breaks out of the nearest While or For loop.", "跳出最近的 While 或 For 循环。"));
                        break;

                default:
                        break;
        }
}

// ─── draw ─────────────────────────────────────────────────────────────────────

void
SequenceEditor::draw()
{
        if (!show_) {
                if (seqRunning_.load())
                        seqStopReq_.store(true);
                return;
        }

        // Completion notification (set by background thread indirectly via seqDone_)
        if (seqDone_.exchange(false)) {
                ImGui::InsertNotification(
                    {ImGuiToastType::Success, 3000, tr("Sequence execution completed!", "序列执行完成！")});
        }

        ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(tr("Sequence Editor###SeqEditor", "序列编辑器###SeqEditor"),
                          &show_,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                if (seqRunning_.load())
                        seqStopReq_.store(true);
                ImGui::End();
                return;
        }

        bool running = seqRunning_.load();

        // ── Toolbar ──────────────────────────────────────────────────────────────
        if (running) {
                if (ui::Button(tr("Stop", "停止"), ui::BtnStyle::Danger, ImVec2(80, 0))) {
                        seqStopReq_.store(true);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("●");
        } else {
                if (ui::Button(tr("Play", "播放"), ui::BtnStyle::Success, ImVec2(80, 0))) {
                        if (!steps_.empty()) {
                                seqStopReq_.store(false);
                                seqDone_.store(false);
                                seqRunning_.store(true);
                                {
                                        std::lock_guard<std::mutex> lk(seqMtx_);
                                        seqLog_.clear();
                                }
                                if (seqThread_.joinable())
                                        seqThread_.join();
                                seqThread_ = std::thread([this]() {
                                        LOG_I("[SeqEditor] thread started, %zu steps", steps_.size());
                                        try {
                                                SeqCtx ctx{this};
                                                seqExecSteps(steps_, ctx);
                                        } catch (const std::exception &ex) {
                                                LOG_E("[SeqEditor] exception: %s", ex.what());
                                                SeqLogEntry le;
                                                le.tsMs  = nowStampMs();
                                                le.desc  = "EXCEPTION";
                                                le.value = ex.what();
                                                le.ok    = false;
                                                std::lock_guard<std::mutex> lk(seqMtx_);
                                                seqLog_.push_back(std::move(le));
                                        } catch (...) {
                                                LOG_E("[SeqEditor] unknown exception");
                                                SeqLogEntry le;
                                                le.tsMs  = nowStampMs();
                                                le.desc  = "EXCEPTION";
                                                le.value = "unknown";
                                                le.ok    = false;
                                                std::lock_guard<std::mutex> lk(seqMtx_);
                                                seqLog_.push_back(std::move(le));
                                        }
                                        LOG_I("[SeqEditor] thread finished");
                                        seqRunning_.store(false);
                                        seqDone_.store(true);
                                });
                        }
                }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Import", "导入"))) {
                std::string path = nativeDlgOpen("Import Sequence", {{"Sequence Files", {"seq"}}}, "");
                if (!path.empty())
                        importFromFile(path);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Export", "导出"))) {
                std::string path = nativeDlgSave("Export Sequence", {{"Sequence Files", {"seq"}}}, "sequence.seq");
                if (!path.empty()) {
                        if (path.find(".seq") == std::string::npos)
                                path += ".seq";
                        exportToFile(path);
                }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Folder", "文件夹"))) {
                std::string path = nativeDlgPickDir("Choose Sequence Folder");
                if (!path.empty()) {
                        seqFolder_ = path;
                        refreshSeqFiles();
                }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        if (ImGui::BeginCombo("##SeqFiles",
                              selectedSeqIdx_ >= 0 && selectedSeqIdx_ < (int)seqFiles_.size()
                                  ? seqFiles_[selectedSeqIdx_].c_str()
                                  : tr("Select seq...", "选择序列..."))) {
                for (int i = 0; i < (int)seqFiles_.size(); i++) {
                        bool isSelected = (selectedSeqIdx_ == i);
                        if (ImGui::Selectable(seqFiles_[i].c_str(), isSelected)) {
                                selectedSeqIdx_ = i;
                                importFromFile(seqFolder_ + "/" + seqFiles_[i]);
                        }
                        if (isSelected)
                                ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
        }

        ImGui::Separator();

        // ── Add Step buttons ──────────────────────────────────────────────────
        if (!running) {
                if (ImGui::Button(tr("+ do", "+ 执行"))) {
                        SequenceStep s;
                        s.kind    = SeqStepKind::Action;
                        s.delayMs = 0;
                        steps_.push_back(s);
                        selectedStep_ = (int)steps_.size() - 1;
                        isModified_   = true;
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("+ delay", "+ 延时"))) {
                        SequenceStep s;
                        s.kind = SeqStepKind::Sleep;
                        steps_.push_back(s);
                        selectedStep_ = (int)steps_.size() - 1;
                        isModified_   = true;
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("+ if", "+ 判断"))) {
                        SequenceStep s;
                        s.kind = SeqStepKind::If;
                        steps_.push_back(s);
                        selectedStep_ = (int)steps_.size() - 1;
                        isModified_   = true;
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("+ while", "+ 循环"))) {
                        SequenceStep s;
                        s.kind = SeqStepKind::While;
                        steps_.push_back(s);
                        selectedStep_ = (int)steps_.size() - 1;
                        isModified_   = true;
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("+ for", "+ 计数"))) {
                        SequenceStep s;
                        s.kind = SeqStepKind::For;
                        steps_.push_back(s);
                        selectedStep_ = (int)steps_.size() - 1;
                        isModified_   = true;
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("+ break", "+ 退出"))) {
                        SequenceStep s;
                        s.kind = SeqStepKind::Break;
                        steps_.push_back(s);
                        selectedStep_ = (int)steps_.size() - 1;
                        isModified_   = true;
                }
                ImGui::SameLine();
                if (ui::Button(tr("Clear All", "全部清空"), ui::BtnStyle::Warning)) {
                        steps_.clear();
                        selectedStep_ = -1;
                        isModified_   = true;
                }
        }

        ImGui::Separator();

        // ── Split view ────────────────────────────────────────────────────────
        // BeginTable's outer_size.y only clips rendering; inner BeginChild(0,0)
        // windows still fill remaining *window* space and won't respond to the
        // table height.  Wrap in BeginChild instead — it enforces explicit height.
        static ImGuiTableFlags splitFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
        const float            is         = ImGui::GetStyle().ItemSpacing.y;
        const float            logHeaderH = ImGui::GetFrameHeightWithSpacing(); // dynamic: frame + ItemSpacing
        const float            logReserve = seqLogCollapsed_ ? logHeaderH : (6.0f + is + logHeaderH + seqLogHeight_ + is);
        // Subtract one extra ItemSpacing for what EndChild appends after SeqTopPane
        const float topH = std::max(80.0f, ImGui::GetContentRegionAvail().y - is - logReserve);
        if (ImGui::BeginChild("SeqTopPane", ImVec2(0, topH), false, ImGuiWindowFlags_NoScrollbar)) {
                if (ImGui::BeginTable("SplitView", 2, splitFlags)) {
                        ImGui::TableSetupColumn("Left", ImGuiTableColumnFlags_WidthFixed, 220.0f);
                        ImGui::TableSetupColumn("Right", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableNextRow();

                        // ── Left: Step list ───────────────────────────────────────────────
                        ImGui::TableSetColumnIndex(0);
                        if (ImGui::BeginChild("StepsList", ImVec2(0, 0), true)) {
                                drawStepList();
                        }
                        ImGui::EndChild();

                        // Also accept variable drops on the entire left-column area (no SDK).
                        if (!running && selectedStep_ >= 0 && selectedStep_ < (int)steps_.size() &&
                            ImGui::BeginDragDropTarget()) {
                                SequenceStep &st  = steps_[selectedStep_];
                                bool          mod = false;
                                if (st.kind == SeqStepKind::Action)
                                        mod = acceptOpDrops([&](SeqOp op) { st.ops.push_back(std::move(op)); }, true);
                                else if (st.kind == SeqStepKind::If || st.kind == SeqStepKind::While ||
                                         st.kind == SeqStepKind::For)
                                        mod = acceptOpDrops([&](SeqOp op) { appendOpToBody(st.body, std::move(op)); }, true);
                                if (mod)
                                        isModified_ = true;
                                ImGui::EndDragDropTarget();
                        }

                        // ── Right: Step detail ────────────────────────────────────────────
                        ImGui::TableSetColumnIndex(1);
                        if (ImGui::BeginChild("StepDetails", ImVec2(0, 0), true)) {
                                SequenceStep *step = selectedStepPtr();
                                if (step) {
                                        if (running)
                                                ImGui::BeginDisabled();
                                        drawStepDetail(*step);
                                        if (running)
                                                ImGui::EndDisabled();
                                } else {
                                        ImGui::TextDisabled("%s", tr("Select a step to edit.", "选择一个步骤以编辑。"));
                                }
                        }
                        ImGui::EndChild();

                        ImGui::EndTable();
                }
        }
        ImGui::EndChild();

        // ── Log splitter handle (only when log is expanded) ───────────────────
        if (!seqLogCollapsed_) {
                ImVec2 splitPos = ImGui::GetCursorScreenPos();
                float  w        = ImGui::GetContentRegionAvail().x;
                ImGui::InvisibleButton("##logSplit", ImVec2(w, 6.0f));
                if (ImGui::IsItemActive())
                        seqLogHeight_ = std::max(40.0f, seqLogHeight_ - ImGui::GetIO().MouseDelta.y);
                ImU32 col = ImGui::IsItemHovered() || ImGui::IsItemActive() ? ImGui::GetColorU32(ImGuiCol_SeparatorActive)
                                                                            : ImGui::GetColorU32(ImGuiCol_Separator);
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2(splitPos.x, splitPos.y + 3.0f), ImVec2(splitPos.x + w, splitPos.y + 3.0f), col, 1.0f);
                if (ImGui::IsItemHovered())
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }

        // ── Log header: collapse arrow + label + clear ────────────────────────
        ImGui::AlignTextToFramePadding();
        if (ImGui::ArrowButton("##logcol", seqLogCollapsed_ ? ImGuiDir_Right : ImGuiDir_Down))
                seqLogCollapsed_ = !seqLogCollapsed_;
        ImGui::SameLine();
        ImGui::TextUnformatted(tr("Log", "日志"));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Clear##logclear", "清空##logclear"))) {
                std::lock_guard<std::mutex> lk(seqMtx_);
                seqLog_.clear();
        }

        // ── Log content (hidden when collapsed) ───────────────────────────────
        if (!seqLogCollapsed_) {
                std::vector<SeqLogEntry> logSnapshot;
                {
                        std::lock_guard<std::mutex> lk(seqMtx_);
                        // Trim in-place to keep memory bounded, then snapshot
                        if ((int)seqLog_.size() > kMaxLogEntries)
                                seqLog_.erase(seqLog_.begin(), seqLog_.begin() + ((int)seqLog_.size() - kMaxLogEntries));
                        logSnapshot = seqLog_;
                }

                if (ImGui::BeginChild("SeqLog", ImVec2(0, seqLogHeight_), true, ImGuiWindowFlags_HorizontalScrollbar)) {
                        // Render newest-first using a clipper so only visible rows cost anything.
                        ImGuiListClipper clipper;
                        clipper.Begin((int)logSnapshot.size());
                        while (clipper.Step()) {
                                for (int ci = clipper.DisplayStart; ci < clipper.DisplayEnd; ++ci) {
                                        // Reverse: ci=0 → last entry (newest)
                                        const SeqLogEntry &e = logSnapshot[(int)logSnapshot.size() - 1 - ci];
                                        {
                                                std::time_t t   = (std::time_t)(e.tsMs / 1000);
                                                struct tm   tm_ = {};
                                                localtime_s(&tm_, &t);
                                                char ts[32];
                                                snprintf(ts,
                                                         sizeof(ts),
                                                         "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
                                                         tm_.tm_year + 1900,
                                                         tm_.tm_mon + 1,
                                                         tm_.tm_mday,
                                                         tm_.tm_hour,
                                                         tm_.tm_min,
                                                         tm_.tm_sec,
                                                         (int)(e.tsMs % 1000));
                                                ImGui::TextDisabled("%s", ts);
                                                ImGui::SameLine(0, 0);
                                        }
                                        ImGui::PushStyleColor(ImGuiCol_Text,
                                                              e.ok ? ImVec4(0.85f, 0.85f, 0.85f, 1.f)
                                                                   : ImVec4(1.f, 0.4f, 0.4f, 1.f));
                                        ImGui::TextUnformatted(e.desc.c_str());
                                        ImGui::PopStyleColor();
                                        if (!e.value.empty()) {
                                                ImGui::SameLine();
                                                ImGui::PushStyleColor(ImGuiCol_Text,
                                                                      e.ok ? ImVec4(0.3f, 1.f, 0.5f, 1.f)
                                                                           : ImVec4(1.f, 0.5f, 0.3f, 1.f));
                                                ImGui::TextUnformatted(e.value.c_str());
                                                ImGui::PopStyleColor();
                                        }
                                }
                        }
                        clipper.End();
                }
                ImGui::EndChild();
        }

        ImGui::End();
}

// ─── registerSdkPanel ────────────────────────────────────────────────────────

void
SequenceEditor::registerSdkPanel(std::weak_ptr<SdkPanel> sp)
{
        sdkPanels_.erase(
            std::remove_if(sdkPanels_.begin(), sdkPanels_.end(), [](const std::weak_ptr<SdkPanel> &w) { return w.expired(); }),
            sdkPanels_.end());
        sdkPanels_.push_back(std::move(sp));
}

// ─── JSON serialization ───────────────────────────────────────────────────────

static cJSON *
actionToObj(const SequenceAction &action)
{
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", action.name.c_str());
        cJSON_AddStringToObject(obj, "targetValue", action.targetValue.c_str());
        cJSON_AddNumberToObject(obj, "addr", static_cast<double>(action.addr));
        cJSON_AddNumberToObject(obj, "type", (int)action.type);
        cJSON_AddStringToObject(obj, "port", action.port.c_str());
        cJSON_AddStringToObject(obj, "shmName", action.shmName.c_str());
        cJSON_AddNumberToObject(obj, "typeOff", static_cast<double>(action.typeOff));
        cJSON_AddNumberToObject(obj, "bitOffset", action.bitOffset);
        cJSON_AddNumberToObject(obj, "bitSize", action.bitSize);
        cJSON_AddBoolToObject(obj, "isEnum", action.isEnum);
        if (action.isEnum) {
                cJSON *ed = cJSON_CreateObject();
                for (const auto &[val, nm] : action.enumDefs)
                        cJSON_AddStringToObject(ed, std::to_string(val).c_str(), nm.c_str());
                cJSON_AddItemToObject(obj, "enumDefs", ed);
        }
        return obj;
}

// Serialize a single "do" step operation (variable write or SDK call).
static cJSON *
saveOp(const SeqOp &op)
{
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "opKind", (int)op.kind);
        if (op.kind == SeqOpKind::Write) {
                cJSON_AddItemToObject(obj, "action", actionToObj(op.action));
        } else {
                cJSON_AddNumberToObject(obj, "sdkWinId", op.sdk.panelWinId);
                cJSON_AddBoolToObject(obj, "sdkIsCFunc", op.sdk.isCFunc);
                cJSON_AddBoolToObject(obj, "sdkIsPython", op.sdk.isPython);
                cJSON_AddNumberToObject(obj, "sdkClassIdx", op.sdk.classIdx);
                cJSON_AddNumberToObject(obj, "sdkMethIdx", op.sdk.methodIdx);
                cJSON_AddNumberToObject(obj, "sdkObjIdx", op.sdk.objIdx);
                cJSON_AddStringToObject(obj, "sdkLabel", op.sdk.label.c_str());
                cJSON_AddStringToObject(obj, "sdkPyFuncName", op.sdk.pyFuncName.c_str());
                cJSON_AddStringToObject(obj, "sdkResultVar", op.sdk.resultVar);
                cJSON *argsArr = cJSON_CreateArray();
                for (const auto &a : op.sdk.args)
                        cJSON_AddItemToArray(argsArr, cJSON_CreateString(a.c_str()));
                cJSON_AddItemToObject(obj, "sdkArgs", argsArr);
        }
        return obj;
}

static cJSON *saveStep(const SequenceStep &step);

static cJSON *
saveStep(const SequenceStep &step)
{
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", step.name.c_str());
        cJSON_AddNumberToObject(obj, "kind", (int)step.kind);
        cJSON_AddNumberToObject(obj, "delayMs", step.delayMs);

        cJSON *opsArr = cJSON_CreateArray();
        for (const auto &op : step.ops)
                cJSON_AddItemToArray(opsArr, saveOp(op));
        cJSON_AddItemToObject(obj, "ops", opsArr);

        cJSON_AddNumberToObject(obj, "sleepMs", step.sleepMs);
        cJSON_AddStringToObject(obj, "condVar", step.condVar);
        cJSON_AddStringToObject(obj, "condOp", step.condOp);
        cJSON_AddNumberToObject(obj, "condVal", static_cast<double>(step.condVal));
        cJSON_AddStringToObject(obj, "forVar", step.forVar);
        cJSON_AddNumberToObject(obj, "forFrom", static_cast<double>(step.forFrom));
        cJSON_AddNumberToObject(obj, "forTo", static_cast<double>(step.forTo));
        cJSON_AddNumberToObject(obj, "forStep", static_cast<double>(step.forStep));
        cJSON_AddBoolToObject(obj, "hasElse", step.hasElse);

        // Body
        cJSON *bodyArr = cJSON_CreateArray();
        for (const auto &b : step.body)
                cJSON_AddItemToArray(bodyArr, saveStep(b));
        cJSON_AddItemToObject(obj, "body", bodyArr);

        cJSON *elseArr = cJSON_CreateArray();
        for (const auto &b : step.elseBody)
                cJSON_AddItemToArray(elseArr, saveStep(b));
        cJSON_AddItemToObject(obj, "elseBody", elseArr);

        return obj;
}

void
SequenceEditor::saveSession(cJSON *root)
{
        cJSON *seqArr = cJSON_CreateArray();
        for (const auto &step : steps_)
                cJSON_AddItemToArray(seqArr, saveStep(step));
        cJSON_AddItemToObject(root, "sequenceEditor", seqArr);
        cJSON_AddStringToObject(root, "sequenceEditorFolder", seqFolder_.c_str());
}

static SequenceAction
loadAction(const cJSON *obj)
{
        SequenceAction a;
        if (const cJSON *n = cJSON_GetObjectItem(obj, "name"); cJSON_IsString(n))
                a.name = n->valuestring;
        if (const cJSON *tv = cJSON_GetObjectItem(obj, "targetValue"); cJSON_IsString(tv))
                a.targetValue = tv->valuestring;
        if (const cJSON *ad = cJSON_GetObjectItem(obj, "addr"); cJSON_IsNumber(ad))
                a.addr = static_cast<u64>(ad->valuedouble);
        if (const cJSON *t = cJSON_GetObjectItem(obj, "type"); cJSON_IsNumber(t))
                a.type = (DataType)t->valueint;
        if (const cJSON *p = cJSON_GetObjectItem(obj, "port"); cJSON_IsString(p))
                a.port = p->valuestring;
        if (const cJSON *sh = cJSON_GetObjectItem(obj, "shmName"); cJSON_IsString(sh))
                a.shmName = sh->valuestring;
        if (const cJSON *to = cJSON_GetObjectItem(obj, "typeOff"); cJSON_IsNumber(to))
                a.typeOff = static_cast<u64>(to->valuedouble);
        if (const cJSON *bo = cJSON_GetObjectItem(obj, "bitOffset"); cJSON_IsNumber(bo))
                a.bitOffset = bo->valueint;
        if (const cJSON *bs = cJSON_GetObjectItem(obj, "bitSize"); cJSON_IsNumber(bs))
                a.bitSize = bs->valueint;
        if (const cJSON *ie = cJSON_GetObjectItem(obj, "isEnum"); cJSON_IsBool(ie))
                a.isEnum = cJSON_IsTrue(ie);
        if (const cJSON *ed = cJSON_GetObjectItem(obj, "enumDefs"); cJSON_IsObject(ed)) {
                for (const cJSON *e = ed->child; e; e = e->next)
                        if (cJSON_IsString(e))
                                a.enumDefs.push_back({std::stoll(e->string), e->valuestring});
        }
        return a;
}

static SequenceStep
loadStep(const cJSON *obj)
{
        SequenceStep s;
        if (const cJSON *n = cJSON_GetObjectItem(obj, "name"); cJSON_IsString(n))
                s.name = n->valuestring;
        if (const cJSON *k = cJSON_GetObjectItem(obj, "kind"); cJSON_IsNumber(k))
                s.kind = (SeqStepKind)k->valueint;
        if (const cJSON *d = cJSON_GetObjectItem(obj, "delayMs"); cJSON_IsNumber(d))
                s.delayMs = (u32)d->valueint;
        if (const cJSON *sm = cJSON_GetObjectItem(obj, "sleepMs"); cJSON_IsNumber(sm))
                s.sleepMs = sm->valueint;

        // Legacy: old sessions only had Action steps with no kind field → default Action
        // condVar/Op/Val
        if (const cJSON *cv = cJSON_GetObjectItem(obj, "condVar"); cJSON_IsString(cv))
                strncpy(s.condVar, cv->valuestring, sizeof(s.condVar) - 1);
        if (const cJSON *co = cJSON_GetObjectItem(obj, "condOp"); cJSON_IsString(co))
                strncpy(s.condOp, co->valuestring, sizeof(s.condOp) - 1);
        if (const cJSON *cval = cJSON_GetObjectItem(obj, "condVal"); cJSON_IsNumber(cval))
                s.condVal = (int64_t)cval->valuedouble;

        if (const cJSON *fv = cJSON_GetObjectItem(obj, "forVar"); cJSON_IsString(fv))
                strncpy(s.forVar, fv->valuestring, sizeof(s.forVar) - 1);
        if (const cJSON *ff = cJSON_GetObjectItem(obj, "forFrom"); cJSON_IsNumber(ff))
                s.forFrom = (int64_t)ff->valuedouble;
        if (const cJSON *ft = cJSON_GetObjectItem(obj, "forTo"); cJSON_IsNumber(ft))
                s.forTo = (int64_t)ft->valuedouble;
        if (const cJSON *fs = cJSON_GetObjectItem(obj, "forStep"); cJSON_IsNumber(fs))
                s.forStep = (int64_t)fs->valuedouble;
        if (const cJSON *he = cJSON_GetObjectItem(obj, "hasElse"); cJSON_IsBool(he))
                s.hasElse = cJSON_IsTrue(he);

        // Reads the SDK-call fields (object form for ops, or the legacy top-level
        // standalone SdkCall step) into an SdkStepInfo.
        auto readSdk = [](const cJSON *o) {
                SdkStepInfo sdk;
                if (const cJSON *w = cJSON_GetObjectItem(o, "sdkWinId"); cJSON_IsNumber(w))
                        sdk.panelWinId = w->valueint;
                if (const cJSON *cf = cJSON_GetObjectItem(o, "sdkIsCFunc"); cJSON_IsBool(cf))
                        sdk.isCFunc = cJSON_IsTrue(cf);
                if (const cJSON *py = cJSON_GetObjectItem(o, "sdkIsPython"); cJSON_IsBool(py))
                        sdk.isPython = cJSON_IsTrue(py);
                if (const cJSON *pn = cJSON_GetObjectItem(o, "sdkPyFuncName"); cJSON_IsString(pn))
                        sdk.pyFuncName = pn->valuestring;
                if (const cJSON *ci = cJSON_GetObjectItem(o, "sdkClassIdx"); cJSON_IsNumber(ci))
                        sdk.classIdx = ci->valueint;
                if (const cJSON *mi = cJSON_GetObjectItem(o, "sdkMethIdx"); cJSON_IsNumber(mi))
                        sdk.methodIdx = mi->valueint;
                if (const cJSON *oi = cJSON_GetObjectItem(o, "sdkObjIdx"); cJSON_IsNumber(oi))
                        sdk.objIdx = oi->valueint;
                if (const cJSON *sl = cJSON_GetObjectItem(o, "sdkLabel"); cJSON_IsString(sl))
                        sdk.label = sl->valuestring;
                if (const cJSON *aa = cJSON_GetObjectItem(o, "sdkArgs"); cJSON_IsArray(aa))
                        for (const cJSON *a = aa->child; a; a = a->next)
                                if (cJSON_IsString(a))
                                        sdk.args.push_back(a->valuestring);
                if (const cJSON *rv = cJSON_GetObjectItem(o, "sdkResultVar"); cJSON_IsString(rv))
                        strncpy(sdk.resultVar, rv->valuestring, sizeof(sdk.resultVar) - 1);
                return sdk;
        };

        // Ops (current format): ordered mix of writes and SDK calls.
        if (const cJSON *opsArr = cJSON_GetObjectItem(obj, "ops"); cJSON_IsArray(opsArr)) {
                for (const cJSON *o = opsArr->child; o; o = o->next) {
                        SeqOp op;
                        if (const cJSON *ok = cJSON_GetObjectItem(o, "opKind"); cJSON_IsNumber(ok))
                                op.kind = (SeqOpKind)ok->valueint;
                        if (op.kind == SeqOpKind::Write) {
                                if (const cJSON *ao = cJSON_GetObjectItem(o, "action"); cJSON_IsObject(ao))
                                        op.action = loadAction(ao);
                        } else {
                                op.sdk = readSdk(o);
                        }
                        s.ops.push_back(std::move(op));
                }
        } else {
                // Legacy: a step held an "actions" array and/or was a standalone SdkCall.
                if (const cJSON *actArr = cJSON_GetObjectItem(obj, "actions"); cJSON_IsArray(actArr))
                        for (const cJSON *a = actArr->child; a; a = a->next)
                                s.ops.push_back({SeqOpKind::Write, loadAction(a), {}});
                if (s.kind == SeqStepKind::SdkCall) {
                        // Convert the standalone SDK step into a "do" step with one SDK op.
                        s.ops.push_back({SeqOpKind::Sdk, {}, readSdk(obj)});
                        s.kind = SeqStepKind::Action;
                }
        }

        // Body
        const cJSON *bodyArr = cJSON_GetObjectItem(obj, "body");
        if (cJSON_IsArray(bodyArr))
                for (const cJSON *b = bodyArr->child; b; b = b->next)
                        s.body.push_back(loadStep(b));

        const cJSON *elseArr = cJSON_GetObjectItem(obj, "elseBody");
        if (cJSON_IsArray(elseArr))
                for (const cJSON *b = elseArr->child; b; b = b->next)
                        s.elseBody.push_back(loadStep(b));

        return s;
}

void
SequenceEditor::loadSession(cJSON *root)
{
        steps_.clear();
        selectedStep_ = -1;
        isModified_   = false;

        if (const cJSON *f = cJSON_GetObjectItem(root, "sequenceEditorFolder"); cJSON_IsString(f)) {
                seqFolder_ = f->valuestring;
                refreshSeqFiles();
        }

        const cJSON *seqArr = cJSON_GetObjectItem(root, "sequenceEditor");
        if (!cJSON_IsArray(seqArr))
                return;
        for (const cJSON *stepObj = seqArr->child; stepObj; stepObj = stepObj->next)
                steps_.push_back(loadStep(stepObj));
}

// ─── exportToFile / importFromFile ────────────────────────────────────────────

void
SequenceEditor::exportToFile(const std::string &path)
{
        cJSON *root = cJSON_CreateObject();
        saveSession(root);
        char *out = cJSON_Print(root);
        if (out) {
                std::filesystem::path p(reinterpret_cast<const char8_t *>(path.c_str()));
                std::ofstream         ofs(p);
                if (ofs)
                        ofs << out;
                cJSON_free(out);
        }
        cJSON_Delete(root);
}

void
SequenceEditor::importFromFile(const std::string &path)
{
        std::filesystem::path p(reinterpret_cast<const char8_t *>(path.c_str()));
        std::ifstream         ifs(p);
        if (!ifs)
                return;
        std::stringstream ss;
        ss << ifs.rdbuf();
        const std::string content = ss.str();
        if (content.empty())
                return;
        cJSON *root = cJSON_Parse(content.c_str());
        if (root) {
                loadSession(root);
                cJSON_Delete(root);
        }
}

// ─── findSdkPanel ─────────────────────────────────────────────────────────────

std::shared_ptr<SdkPanel>
SequenceEditor::findSdkPanel(int winId) const
{
        for (const auto &w : sdkPanels_) {
                auto sp = w.lock();
                if (sp && sp->getWinId() == winId)
                        return sp;
        }
        return nullptr;
}

// ─── drawSdkStepDetail ────────────────────────────────────────────────────────

void
SequenceEditor::drawSdkStepDetail(SdkStepInfo &sdk, std::shared_ptr<SdkPanel> panel)
{
        ImGui::Text(tr("SDK Call: %s", "SDK 调用：%s"), sdk.label.c_str());
        ImGui::Separator();

        // ── Python function ───────────────────────────────────────────────────
        if (sdk.isPython) {
                int nPy = panel->getPyFuncParamCount(sdk.pyFuncName);
                sdk.args.resize(nPy);
                for (int i = 0; i < nPy; ++i) {
                        ImGui::PushID(i);
                        std::string pname = panel->getPyFuncParamName(sdk.pyFuncName, i);
                        char        argBuf[512]{};
                        strncpy(argBuf, sdk.args[i].c_str(), sizeof(argBuf) - 1);
                        char lbl[80];
                        snprintf(lbl, sizeof(lbl), "%s##p%d", pname.empty() ? "arg" : pname.c_str(), i);
                        float w = ImGui::GetContentRegionAvail().x - 8.0f;
                        if (w > 200.0f)
                                w = 200.0f;
                        if (w < 80.0f)
                                w = 80.0f;
                        ImGui::SetNextItemWidth(w);
                        if (ImGui::InputText(lbl, argBuf, sizeof(argBuf))) {
                                sdk.args[i] = argBuf;
                                isModified_ = true;
                        }
                        ImGui::PopID();
                }
                return;
        }

        // ── Object selector for C++ methods ──────────────────────────────────
        if (!sdk.isCFunc) {
                auto objs = panel->listObjects(sdk.classIdx);

                const char *curLabel = tr("(none)", "(无)");
                for (const auto &o : objs)
                        if (o.idx == sdk.objIdx) {
                                curLabel = o.label.c_str();
                                break;
                        }

                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::BeginCombo(tr("Object##objsel", "对象##objsel"), curLabel)) {
                        for (const auto &o : objs) {
                                bool sel = (o.idx == sdk.objIdx);
                                char combo_label[128];
                                snprintf(combo_label, sizeof(combo_label), "%s  (%s)", o.label.c_str(), o.className.c_str());
                                if (ImGui::Selectable(combo_label, sel)) {
                                        sdk.objIdx  = o.idx;
                                        isModified_ = true;
                                }
                                if (sel)
                                        ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("New##newobj", "新建##newobj"))) {
                        int ni = panel->newObject(sdk.classIdx);
                        if (ni >= 0) {
                                sdk.objIdx  = ni;
                                isModified_ = true;
                        }
                }
                ImGui::Spacing();
        }

        int nParams =
            sdk.isCFunc ? panel->getCFuncParamCount(sdk.methodIdx) : panel->getParamCount(sdk.classIdx, sdk.methodIdx);
        sdk.args.resize(nParams);

        std::vector<std::string> localVars;
        if (panel->onListLocalVars_)
                localVars = panel->onListLocalVars_();

        for (int i = 0; i < nParams; ++i) {
                ImGui::PushID(i);
                bool isPtr = sdk.isCFunc ? panel->isCFuncParamPtrOrRef(sdk.methodIdx, i)
                                         : panel->isParamPtrOrRef(sdk.classIdx, sdk.methodIdx, i);

                char argBuf[512]{};
                strncpy(argBuf, sdk.args[i].c_str(), sizeof(argBuf) - 1);

                // Show param name if available, fall back to "Arg N"
                std::string pname = sdk.isCFunc ? panel->getCFuncParamName(sdk.methodIdx, i)
                                                : panel->getParamName(sdk.classIdx, sdk.methodIdx, i);
                std::string ptype = sdk.isCFunc ? panel->getCFuncParamRawType(sdk.methodIdx, i)
                                                : panel->getParamRawType(sdk.classIdx, sdk.methodIdx, i);
                char        argLabel[80];
                if (!pname.empty())
                        snprintf(argLabel, sizeof(argLabel), "%s##p%d", pname.c_str(), i);
                else
                        snprintf(argLabel, sizeof(argLabel), tr("Arg %d##p%d", "参数 %d##p%d"), i, i);

                // Cap input width — account for create (+) and picker (v) buttons
                bool  dsdkHasPicker = isPtr && !localVars.empty();
                bool  dsdkHasCreate = (bool)onAddLocalVar_;
                float availW        = ImGui::GetContentRegionAvail().x;
                float btnRoom = (dsdkHasPicker && dsdkHasCreate) ? 80.0f : (dsdkHasPicker || dsdkHasCreate) ? 35.0f : 8.0f;
                float inputW  = availW - btnRoom;
                if (inputW > 200.0f)
                        inputW = 200.0f;
                if (inputW < 80.0f)
                        inputW = 80.0f;
                ImGui::SetNextItemWidth(inputW);
                if (ImGui::InputText(argLabel, argBuf, sizeof(argBuf))) {
                        sdk.args[i] = argBuf;
                        isModified_ = true;
                }
                if (!ptype.empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", ptype.c_str());

                if (dsdkHasCreate) {
                        ImGui::SameLine();
                        char cbtn[16];
                        snprintf(cbtn, sizeof(cbtn), "+##dc%d", i);
                        if (ImGui::SmallButton(cbtn)) {
                                std::string pn = pname.empty() ? "arg" + std::to_string(i) : pname;
                                DataType    dt = ptrTypeFromRaw(ptype);
                                size_t      sz = Parser::typeBytes(dt);
                                if (sz == 0)
                                        sz = 4;
                                const CStructDecl *sd = panel->getParamStructDecl(sdk.isCFunc, sdk.classIdx, sdk.methodIdx, i);
                                if (sd && onAddLocalStructVar_)
                                        onAddLocalStructVar_(pn, structDeclToFields(*sd), sd->totalSize);
                                else
                                        onAddLocalVar_(pn, dt, sz);
                                if (isPtr)
                                        sdk.args[i] = "&" + pn;
                                isModified_ = true;
                        }
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", tr("Create LOCAL variable", "创建 LOCAL 变量"));
                }
                if (dsdkHasPicker) {
                        ImGui::SameLine();
                        char btnId[16];
                        snprintf(btnId, sizeof(btnId), "v##p%d", i);
                        if (ImGui::Button(btnId)) {
                                char popupId[32];
                                snprintf(popupId, sizeof(popupId), "##vp%d", i);
                                ImGui::OpenPopup(popupId);
                        }
                        char popupId[32];
                        snprintf(popupId, sizeof(popupId), "##vp%d", i);
                        if (ImGui::BeginPopup(popupId)) {
                                for (const auto &vn : localVars) {
                                        if (ImGui::Selectable(vn.c_str())) {
                                                sdk.args[i] = "&" + vn;
                                                isModified_ = true;
                                        }
                                }
                                ImGui::EndPopup();
                        }
                }
                ImGui::PopID();
        }

        // Return value → LOCAL variable
        ImGui::Spacing();
        ImGui::TextDisabled(tr("return", "返回值"));
        ImGui::SameLine();
        char rvBuf[32]{};
        strncpy(rvBuf, sdk.resultVar, sizeof(rvBuf) - 1);
        bool  dsdkRvHasPicker = !localVars.empty();
        bool  dsdkRvHasCreate = (bool)onAddLocalVar_;
        float rvW = (dsdkRvHasPicker && dsdkRvHasCreate) ? -82.0f : (dsdkRvHasPicker || dsdkRvHasCreate) ? -38.0f : -FLT_MIN;
        ImGui::SetNextItemWidth(rvW);
        if (ImGui::InputText("##dsdkrv", rvBuf, sizeof(rvBuf))) {
                strncpy(sdk.resultVar, rvBuf, sizeof(sdk.resultVar) - 1);
                isModified_ = true;
        }
        if (dsdkRvHasCreate) {
                ImGui::SameLine();
                if (ImGui::SmallButton("+##dsdkrvc")) {
                        std::string vn = (sdk.resultVar[0] != '\0') ? sdk.resultVar : "result";
                        onAddLocalVar_(vn, DataType::I64, sizeof(int64_t));
                        strncpy(sdk.resultVar, vn.c_str(), sizeof(sdk.resultVar) - 1);
                        isModified_ = true;
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Create LOCAL variable for return value", "为返回值创建 LOCAL 变量"));
        }
        if (dsdkRvHasPicker) {
                ImGui::SameLine();
                if (ImGui::SmallButton("v##dsdkrvpick"))
                        ImGui::OpenPopup("##dsdkrvpop");
                if (ImGui::BeginPopup("##dsdkrvpop")) {
                        for (const auto &vn : localVars)
                                if (ImGui::Selectable(vn.c_str())) {
                                        strncpy(sdk.resultVar, vn.c_str(), sizeof(sdk.resultVar) - 1);
                                        isModified_ = true;
                                }
                        ImGui::EndPopup();
                }
        }
}
