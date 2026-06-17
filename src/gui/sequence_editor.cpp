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
#include <fstream>
#include <sstream>
#include <thread>

// ─── helpers ─────────────────────────────────────────────────────────────────

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
                        return "do";
                case SeqStepKind::Sleep:
                        return "delay";
                case SeqStepKind::If:
                        return "if";
                case SeqStepKind::While:
                        return "循环";
                case SeqStepKind::For:
                        return "For";
                case SeqStepKind::Break:
                        return "Break";
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
                return;
        }
        if (sdk.isCFunc) {
                auto r = panel->directCallC(sdk.methodIdx, sdk.args);
                LOG_I("[SeqEditor] SdkOp C result ok=%d: %s", (int)r.ok, r.text.c_str());
        } else {
                auto r = panel->directCall(sdk.classIdx, sdk.methodIdx, sdk.objIdx, sdk.args);
                LOG_I("[SeqEditor] SdkOp C++ result ok=%d: %s", (int)r.ok, r.text.c_str());
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
                                SExecResult r = seqExecSteps(step.body, ctx);
                                if (r == SExecResult::Stop)
                                        return SExecResult::Stop;
                                if (r == SExecResult::Break)
                                        break;
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

// ─── drawBodySteps ────────────────────────────────────────────────────────────
// Renders an inline editable list of steps (used for If/While/For bodies).

void
SequenceEditor::drawBodySteps(std::vector<SequenceStep> &steps, int depth)
{
        static const char *kinds[]   = {"do", "delay", "if", "While", "For", "Break"};
        static const char *kindsCn[] = {"do", "delay", "if", "循环", "For", "Break"};
        static const char *ops[]     = {"==", "!=", "<", ">", "<=", ">="};

        int toDelete = -1;
        for (int i = 0; i < (int)steps.size(); ++i) {
                SequenceStep &s = steps[i];
                ImGui::PushID(i + depth * 1000);

                // Kind selector
                int ki = (int)s.kind;
                ImGui::SetNextItemWidth(60.0f);
                if (ImGui::Combo("##k", &ki, g_lang == Lang::ZH ? kindsCn : kinds, 6)) {
                        s.kind      = (SeqStepKind)ki;
                        isModified_ = true;
                }
                ImGui::SameLine();

                // Kind-specific compact fields
                switch (s.kind) {
                        case SeqStepKind::Sleep:
                                ImGui::SetNextItemWidth(55.0f);
                                if (ImGui::InputInt("##ms", &s.sleepMs, 0))
                                        isModified_ = true;
                                ImGui::SameLine();
                                ImGui::TextDisabled("ms");
                                break;
                        case SeqStepKind::Action:
                                ImGui::TextDisabled("(%d)", (int)s.ops.size());
                                break;
                        case SeqStepKind::If:
                        case SeqStepKind::While: {
                                ImGui::SetNextItemWidth(72.0f);
                                if (ImGui::InputText("##cv", s.condVar, sizeof(s.condVar)))
                                        isModified_ = true;
                                ImGui::SameLine();
                                int opi = 0;
                                for (int oi = 0; oi < 6; ++oi)
                                        if (strcmp(s.condOp, ops[oi]) == 0) {
                                                opi = oi;
                                                break;
                                        }
                                ImGui::SetNextItemWidth(46.0f);
                                if (ImGui::Combo("##op", &opi, ops, 6)) {
                                        strncpy(s.condOp, ops[opi], sizeof(s.condOp) - 1);
                                        isModified_ = true;
                                }
                                ImGui::SameLine();
                                int64_t cv = s.condVal;
                                ImGui::SetNextItemWidth(52.0f);
                                if (ImGui::InputScalar("##cv2", ImGuiDataType_S64, &cv)) {
                                        s.condVal   = cv;
                                        isModified_ = true;
                                }
                                break;
                        }
                        case SeqStepKind::For: {
                                ImGui::SetNextItemWidth(40.0f);
                                if (ImGui::InputText("##fv", s.forVar, sizeof(s.forVar)))
                                        isModified_ = true;
                                ImGui::SameLine();
                                ImGui::TextDisabled("=");
                                int64_t ff = s.forFrom, ft = s.forTo, fs = s.forStep;
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(44.0f);
                                if (ImGui::InputScalar("##ff", ImGuiDataType_S64, &ff)) {
                                        s.forFrom   = ff;
                                        isModified_ = true;
                                }
                                ImGui::SameLine();
                                ImGui::TextDisabled("..");
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(44.0f);
                                if (ImGui::InputScalar("##ft", ImGuiDataType_S64, &ft)) {
                                        s.forTo     = ft;
                                        isModified_ = true;
                                }
                                ImGui::SameLine();
                                ImGui::TextDisabled("+");
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(36.0f);
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

                ImGui::SameLine();
                if (ImGui::SmallButton("X"))
                        toDelete = i;

                // Nested body for compound steps
                if (s.kind == SeqStepKind::If || s.kind == SeqStepKind::While || s.kind == SeqStepKind::For) {
                        if (depth < 4) {
                                ImGui::Indent(16.0f);
                                drawBodySteps(s.body, depth + 1);
                                if (ImGui::SmallButton(tr("+##bb", "+##bb"))) {
                                        s.body.push_back(SequenceStep{});
                                        isModified_ = true;
                                }
                                if (s.kind == SeqStepKind::If) {
                                        ImGui::SameLine();
                                        bool he = s.hasElse;
                                        if (ImGui::Checkbox(tr("else", "else"), &he)) {
                                                s.hasElse   = he;
                                                isModified_ = true;
                                        }
                                        if (s.hasElse) {
                                                ImGui::TextDisabled("else:");
                                                drawBodySteps(s.elseBody, depth + 1);
                                                if (ImGui::SmallButton(tr("+##be", "+##be"))) {
                                                        s.elseBody.push_back(SequenceStep{});
                                                        isModified_ = true;
                                                }
                                        }
                                }
                                ImGui::Unindent(16.0f);
                        }
                }
                ImGui::PopID();
        }
        if (toDelete >= 0) {
                steps.erase(steps.begin() + toDelete);
                isModified_ = true;
        }
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
        out.sdk.classIdx   = pl.classIdx;
        out.sdk.methodIdx  = pl.methodIdx;
        out.sdk.objIdx     = pl.objIdx;
        out.sdk.label = pl.isCFunc ? panel->getCFuncLabel(pl.methodIdx) : panel->getCallLabel(pl.classIdx, pl.methodIdx);
        int np = pl.isCFunc ? panel->getCFuncParamCount(pl.methodIdx) : panel->getParamCount(pl.classIdx, pl.methodIdx);
        out.sdk.args.resize(np);
        return true;
}

// Within an active BeginDragDropTarget(): accept SDK functions / variables and
// forward each resulting op to `sink`. Returns true if anything was consumed.
bool
SequenceEditor::acceptOpDrops(const std::function<void(SeqOp)> &sink)
{
        if (const ImGuiPayload *dp = ImGui::AcceptDragDropPayload("SDK_CALL")) {
                SeqOp op;
                if (makeSdkOp(*static_cast<const SdkDragPayload *>(dp->Data), op)) {
                        sink(std::move(op));
                        return true;
                }
                return false;
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
SequenceEditor::drawStepList()
{
        bool running = seqRunning_.load();

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

                // Drop target on step item: SDK functions / variables go into a "do"
                // step's body, or into the body of an If/While/For (as a nested "do" step).
                if (!running && ImGui::BeginDragDropTarget()) {
                        SequenceStep &st  = steps_[i];
                        bool          mod = false;
                        if (st.kind == SeqStepKind::Action)
                                mod = acceptOpDrops([&](SeqOp op) { st.ops.push_back(std::move(op)); });
                        else if (st.kind == SeqStepKind::If || st.kind == SeqStepKind::While ||
                                 st.kind == SeqStepKind::For)
                                mod = acceptOpDrops([&](SeqOp op) { appendOpToBody(st.body, std::move(op)); });
                        if (mod) {
                                selectedStep_ = i;
                                isModified_   = true;
                        }
                        ImGui::EndDragDropTarget();
                }

                ImGui::PopID();
        }

        // ── Invisible drop zone fills remaining height so any empty area accepts SDK drops ──
        float dropH = ImGui::GetContentRegionAvail().y;
        if (dropH < 4.0f)
                dropH = 4.0f;
        ImGui::InvisibleButton("##sdk_drop_area", ImVec2(-FLT_MIN, dropH));
        if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload *dp = ImGui::AcceptDragDropPayload("SDK_CALL"))
                        acceptSdkPayload(dp->Data, -1);
                ImGui::EndDragDropTarget();
        }
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
                static const char *kindsCn[] = {"do", "delay", "if", "循环", "For", "Break"};
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
                        int delay = (int)step.delayMs;
                        if (ImGui::InputInt(tr("Pre-delay (ms)", "前置延时 (ms)"), &delay, 10, 100)) {
                                if (delay < 0)
                                        delay = 0;
                                step.delayMs = (u32)delay;
                                isModified_  = true;
                        }
                        ImGui::Spacing();
                        ImGui::Text("%s",
                                    tr("Actions (drag variables / SDK functions):", "动作（拖入变量 / SDK 函数）："));
                        if (ImGui::BeginChild("OpList", ImVec2(0, 0), true)) {
                                int toDelete = -1;
                                for (int i = 0; i < (int)step.ops.size(); ++i) {
                                        ImGui::PushID(i);
                                        SeqOp &op = step.ops[i];
                                        if (op.kind == SeqOpKind::Write) {
                                                SequenceAction &a = op.action;
                                                ImGui::AlignTextToFramePadding();
                                                ImGui::TextUnformatted(a.name.c_str());
                                                ImGui::SameLine();
                                                ImGui::SetNextItemWidth(180.0f);
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
                                                ImGui::SameLine();
                                                if (ui::Button(tr("Del", "删"), ui::BtnStyle::Danger))
                                                        toDelete = i;
                                        } else {
                                                // SDK call op — header + (when expanded) argument editor.
                                                auto panel = findSdkPanel(op.sdk.panelWinId);
                                                bool open  = ImGui::TreeNodeEx(
                                                    "##sdkop", ImGuiTreeNodeFlags_DefaultOpen, "[SDK] %s", op.sdk.label.c_str());
                                                ImGui::SameLine();
                                                if (ui::Button(tr("Del", "删"), ui::BtnStyle::Danger))
                                                        toDelete = i;
                                                if (open) {
                                                        if (panel)
                                                                drawSdkStepDetail(op.sdk, panel);
                                                        else
                                                                ImGui::TextDisabled(
                                                                    tr("SDK panel (id=%d) closed.", "SDK 窗口 (id=%d) 已关闭。"),
                                                                    op.sdk.panelWinId);
                                                        ImGui::TreePop();
                                                }
                                        }
                                        ImGui::Separator();
                                        ImGui::PopID();
                                }
                                if (step.ops.empty())
                                        ImGui::TextDisabled(
                                            "%s",
                                            tr("Drag variables or SDK functions here.", "把变量或 SDK 函数拖到这里。"));
                                if (toDelete >= 0) {
                                        step.ops.erase(step.ops.begin() + toDelete);
                                        isModified_ = true;
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
                        if (ImGui::BeginChild("BodyList", ImVec2(0, step.hasElse ? -160.0f : -60.0f), true)) {
                                drawBodySteps(step.body, 0);
                        }
                        ImGui::EndChild();
                        // Dropping a variable / SDK function here adds it as a nested "do" step.
                        if (ImGui::BeginDragDropTarget()) {
                                if (acceptOpDrops([&](SeqOp op) { appendOpToBody(step.body, std::move(op)); }))
                                        isModified_ = true;
                                ImGui::EndDragDropTarget();
                        }
                        if (ImGui::Button(tr("+ Add Body Step", "+ 添加步骤"))) {
                                step.body.push_back(SequenceStep{});
                                isModified_ = true;
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
                        if (ImGui::BeginChild("ForBody", ImVec2(0, -60.0f), true)) {
                                drawBodySteps(step.body, 0);
                        }
                        ImGui::EndChild();
                        if (ImGui::BeginDragDropTarget()) {
                                if (acceptOpDrops([&](SeqOp op) { appendOpToBody(step.body, std::move(op)); }))
                                        isModified_ = true;
                                ImGui::EndDragDropTarget();
                        }
                        if (ImGui::Button(tr("+ Add Body Step", "+ 添加步骤"))) {
                                step.body.push_back(SequenceStep{});
                                isModified_ = true;
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
        if (!ImGui::Begin(tr("Sequence Editor###SeqEditor", "序列编辑器###SeqEditor"), &show_)) {
                if (seqRunning_.load())
                        seqStopReq_.store(true);
                ImGui::End();
                return;
        }
        // Window-level drop target — catches drops anywhere on the window background.
        if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload *dp = ImGui::AcceptDragDropPayload("SDK_CALL"))
                        acceptSdkPayload(dp->Data, -1);
                ImGui::EndDragDropTarget();
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
                                                std::lock_guard<std::mutex> lk(seqMtx_);
                                                seqLog_.push_back(std::string("EXCEPTION: ") + ex.what());
                                        } catch (...) {
                                                LOG_E("[SeqEditor] unknown exception");
                                                std::lock_guard<std::mutex> lk(seqMtx_);
                                                seqLog_.push_back("EXCEPTION: unknown");
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
                if (ImGui::Button(tr("+ do", "+ do"))) {
                        SequenceStep s;
                        s.kind    = SeqStepKind::Action;
                        s.delayMs = 0;
                        steps_.push_back(s);
                        selectedStep_ = (int)steps_.size() - 1;
                        isModified_   = true;
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("+ delay", "+ delay"))) {
                        SequenceStep s;
                        s.kind = SeqStepKind::Sleep;
                        steps_.push_back(s);
                        selectedStep_ = (int)steps_.size() - 1;
                        isModified_   = true;
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("+ if", "+ if"))) {
                        SequenceStep s;
                        s.kind = SeqStepKind::If;
                        steps_.push_back(s);
                        selectedStep_ = (int)steps_.size() - 1;
                        isModified_   = true;
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("+ While", "+ While"))) {
                        SequenceStep s;
                        s.kind = SeqStepKind::While;
                        steps_.push_back(s);
                        selectedStep_ = (int)steps_.size() - 1;
                        isModified_   = true;
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("+ For", "+ For"))) {
                        SequenceStep s;
                        s.kind = SeqStepKind::For;
                        steps_.push_back(s);
                        selectedStep_ = (int)steps_.size() - 1;
                        isModified_   = true;
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("+ Break", "+ Break"))) {
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
        static ImGuiTableFlags splitFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
        if (!ImGui::BeginTable("SplitView", 2, splitFlags)) {
                ImGui::End();
                return;
        }
        ImGui::TableSetupColumn("Left", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("Right", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();

        // ── Left: Step list ───────────────────────────────────────────────────
        ImGui::TableSetColumnIndex(0);
        if (ImGui::BeginChild("StepsList", ImVec2(0, 0), true)) {
                drawStepList();
        }
        ImGui::EndChild();

        // Also accept drops on the entire left-column area → selected step's body.
        if (!running && selectedStep_ >= 0 && selectedStep_ < (int)steps_.size() && ImGui::BeginDragDropTarget()) {
                SequenceStep &st  = steps_[selectedStep_];
                bool          mod = false;
                if (st.kind == SeqStepKind::Action)
                        mod = acceptOpDrops([&](SeqOp op) { st.ops.push_back(std::move(op)); });
                else if (st.kind == SeqStepKind::If || st.kind == SeqStepKind::While || st.kind == SeqStepKind::For)
                        mod = acceptOpDrops([&](SeqOp op) { appendOpToBody(st.body, std::move(op)); });
                if (mod)
                        isModified_ = true;
                ImGui::EndDragDropTarget();
        }

        // ── Right: Step detail ────────────────────────────────────────────────
        ImGui::TableSetColumnIndex(1);
        if (ImGui::BeginChild("StepDetails", ImVec2(0, 0), true)) {
                SequenceStep *step = selectedStepPtr();
                if (step && !running) {
                        drawStepDetail(*step);
                } else if (step && running) {
                        ImGui::Text(tr("Step %d running…", "步骤 %d 执行中…"), selectedStep_ + 1);
                } else {
                        ImGui::TextDisabled("%s", tr("Select a step to edit.", "选择一个步骤以编辑。"));
                }
        }
        ImGui::EndChild();

        ImGui::EndTable();

        // ── Log ───────────────────────────────────────────────────────────────
        {
                std::lock_guard<std::mutex> lk(seqMtx_);
                if (!seqLog_.empty()) {
                        ImGui::Separator();
                        if (ImGui::TreeNode(tr("Log##seqlog", "日志##seqlog"))) {
                                for (int i = (int)seqLog_.size() - 1; i >= 0; --i)
                                        ImGui::TextUnformatted(seqLog_[i].c_str());
                                ImGui::TreePop();
                        }
                }
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
                cJSON_AddNumberToObject(obj, "sdkClassIdx", op.sdk.classIdx);
                cJSON_AddNumberToObject(obj, "sdkMethIdx", op.sdk.methodIdx);
                cJSON_AddNumberToObject(obj, "sdkObjIdx", op.sdk.objIdx);
                cJSON_AddStringToObject(obj, "sdkLabel", op.sdk.label.c_str());
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

        // ── Object selector for C++ methods ──────────────────────────────────
        if (!sdk.isCFunc) {
                auto objs = panel->listObjects(sdk.classIdx);

                const char *curLabel = tr("(none)", "(无对象)");
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

                // Cap input width to avoid overflow on wide panels
                float availW = ImGui::GetContentRegionAvail().x;
                float inputW = availW - (isPtr ? 55.0f : 8.0f);
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

                if (isPtr && !localVars.empty()) {
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
}
