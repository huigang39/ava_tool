#include "gui/sdk_panel.hpp"
#include "gui/sequence_editor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "cJSON.h"

#include "core/export_enum.hpp"
#include "core/session_time.hpp"
#include "gui/i18n.hpp"
#include "gui/ui_theme.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "platform/native_dlg.hpp"
#include "timeops.h"

// ─── drawSdkStructFieldRows ───────────────────────────────────────────────────
// Shared struct-field editor used by both the SDK caller and the sequence editor.

bool
drawSdkStructFieldRows(const CStructDecl &sd, uint8_t *buf, size_t bufSize, int idSalt)
{
        if (!buf || bufSize < sd.totalSize)
                return false;

        bool edited = false;
        for (size_t fi = 0; fi < sd.fields.size(); ++fi) {
                const auto &field = sd.fields[fi];
                if (field.isArray) {
                        size_t esz = ctypeSize(field.arrayElemType);
                        for (size_t ai = 0; ai < field.arrayCount; ++ai) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextDisabled("  .%s[%zu]  %s", field.name.c_str(), ai, ctypeLabel(field.arrayElemType));
                                ImGui::TableSetColumnIndex(2);
                                char fid[64];
                                snprintf(fid, sizeof(fid), "##af%d_%zu_%zu", idSalt, fi, ai);
                                ImGui::SetNextItemWidth(-FLT_MIN);
                                uint8_t *p2 = buf + field.offset + ai * esz;
                                if (field.arrayElemType == CType::F64) {
                                        double v;
                                        memcpy(&v, p2, 8);
                                        if (ImGui::InputDouble(fid, &v, 0, 0, "%.6g")) {
                                                memcpy(p2, &v, 8);
                                                edited = true;
                                        }
                                } else if (field.arrayElemType == CType::F32) {
                                        float v;
                                        memcpy(&v, p2, 4);
                                        if (ImGui::InputFloat(fid, &v)) {
                                                memcpy(p2, &v, 4);
                                                edited = true;
                                        }
                                } else {
                                        int iv = 0;
                                        memcpy(&iv, p2, std::min(esz, (size_t)4));
                                        if (ImGui::InputInt(fid, &iv)) {
                                                memcpy(p2, &iv, std::min(esz, (size_t)4));
                                                edited = true;
                                        }
                                }
                        }
                } else if (field.type == CType::F64) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextDisabled("  .%s  double", field.name.c_str());
                        ImGui::TableSetColumnIndex(2);
                        char fid[64];
                        snprintf(fid, sizeof(fid), "##df%d_%zu", idSalt, fi);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        double v;
                        memcpy(&v, buf + field.offset, 8);
                        if (ImGui::InputDouble(fid, &v, 0, 0, "%.6g")) {
                                memcpy(buf + field.offset, &v, 8);
                                edited = true;
                        }
                } else if (field.type == CType::F32) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextDisabled("  .%s  float", field.name.c_str());
                        ImGui::TableSetColumnIndex(2);
                        char fid[64];
                        snprintf(fid, sizeof(fid), "##ff%d_%zu", idSalt, fi);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        float v;
                        memcpy(&v, buf + field.offset, 4);
                        if (ImGui::InputFloat(fid, &v)) {
                                memcpy(buf + field.offset, &v, 4);
                                edited = true;
                        }
                } else {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextDisabled("  .%s  %s", field.name.c_str(), ctypeLabel(field.type));
                        ImGui::TableSetColumnIndex(2);
                        char fid[64];
                        snprintf(fid, sizeof(fid), "##if%d_%zu", idSalt, fi);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        int64_t v = 0;
                        memcpy(&v, buf + field.offset, std::min(field.size, (size_t)8));
                        int iv = (int)v;
                        if (ImGui::InputInt(fid, &iv)) {
                                v = iv;
                                memcpy(buf + field.offset, &v, std::min(field.size, (size_t)8));
                                edited = true;
                        }
                }
        }
        return edited;
}

// ─── helpers ──────────────────────────────────────────────────────────────────

void
SdkPanel::setStatus(const std::string &msg, bool isErr)
{
        statusMsg_   = msg;
        statusIsErr_ = isErr;
}

// ─── doLoadDll ────────────────────────────────────────────────────────────────

void
SdkPanel::doLoadDll()
{
        if (loader_.isLoaded())
                loader_.unload();

        if (!loader_.load(dllPath_)) {
                setStatus(loader_.lastError(), true);
                return;
        }

        // On Windows, enumerateExports falls back to GetModuleHandleA(path) when
        // handle is nullptr — works because SdkLoader::load already loaded it.
        // On POSIX, it reads the file directly and ignores handle.
        auto syms = enumerateExports(nullptr, dllPath_);
        loader_.setExports(syms);

        char buf[128];
        snprintf(buf, sizeof(buf), tr("Loaded: %s  (%d exports)", "已加载: %s  (%d 个导出符号)"), dllPath_, (int)syms.size());
        setStatus(buf, false);
}

// ─── doParseHeader ────────────────────────────────────────────────────────────

void
SdkPanel::doParseHeader()
{
        std::ifstream f(headerPath_);
        if (!f) {
                setStatus(tr("Cannot open header file", "无法打开头文件"), true);
                return;
        }
        std::ostringstream ss;
        ss << f.rdbuf();

        parseResult_       = parseHeaderFull(ss.str());
        selectedFnIdx_     = -1;
        selectedClassIdx_  = -1;
        selectedMethodIdx_ = -1;
        selectedObjIdx_    = -1;
        fnArgBufs_.clear();
        methArgBufs_.clear();
        structBufs_.clear();
        fnArgBufsCache_.clear();
        methArgBufsCache_.clear();
        structBufsCache_.clear();
        fnLastResult_.clear();

        int  totalFn  = (int)parseResult_.functions.size();
        int  totalCls = (int)parseResult_.classes.size();
        char buf[256];
        snprintf(buf, sizeof(buf), tr("Parsed: %d function(s), %d class(es)", "解析: %d 个函数, %d 个类"), totalFn, totalCls);
        setStatus(buf, (totalFn + totalCls) == 0);
}

// ─── selectFn / selectMethod ──────────────────────────────────────────────────

void
SdkPanel::selectFn(int idx)
{
        // Save current args before switching
        if (selectedFnIdx_ >= 0 && selectedFnIdx_ < (int)parseResult_.functions.size())
                fnArgBufsCache_[selectedFnIdx_] = fnArgBufs_;

        selectedFnIdx_ = idx;
        fnLastResult_.clear();
        fnLastResultOk_ = false;
        fnResultVar_[0] = '\0'; // result-var name is per-function, not shared
        if (idx < 0 || idx >= (int)parseResult_.functions.size()) {
                fnArgBufs_.clear();
                return;
        }
        const CFuncDecl &fn = parseResult_.functions[idx];
        auto             it = fnArgBufsCache_.find(idx);
        if (it != fnArgBufsCache_.end() && it->second.size() == fn.params.size()) {
                fnArgBufs_ = it->second;
        } else {
                fnArgBufs_.resize(fn.params.size());
                for (auto &b : fnArgBufs_)
                        b = {};
        }
}

void
SdkPanel::selectMethod(int classIdx, int methodIdx)
{
        // Save current args before switching
        if (selectedClassIdx_ >= 0 && selectedMethodIdx_ >= 0) {
                int key                = selectedClassIdx_ * 10000 + selectedMethodIdx_;
                methArgBufsCache_[key] = methArgBufs_;
                structBufsCache_[key]  = structBufs_;
        }

        selectedClassIdx_  = classIdx;
        selectedMethodIdx_ = methodIdx;
        methResultVar_[0]  = '\0'; // result-var name is per-method, not shared

        if (classIdx < 0 || classIdx >= (int)parseResult_.classes.size()) {
                methArgBufs_.clear();
                structBufs_.clear();
                return;
        }
        const CClassDecl &cls = parseResult_.classes[classIdx];
        if (methodIdx < 0 || methodIdx >= (int)cls.methods.size()) {
                methArgBufs_.clear();
                structBufs_.clear();
                return;
        }

        const CMethodDecl &meth = cls.methods[methodIdx];
        int                key  = classIdx * 10000 + methodIdx;

        auto ita = methArgBufsCache_.find(key);
        if (ita != methArgBufsCache_.end() && ita->second.size() == meth.params.size()) {
                methArgBufs_ = ita->second;
                auto its     = structBufsCache_.find(key);
                if (its != structBufsCache_.end() && its->second.size() == meth.params.size())
                        structBufs_ = its->second;
                else {
                        structBufs_.resize(meth.params.size());
                        for (size_t i = 0; i < meth.params.size(); ++i) {
                                if (!structBufs_[i].decl) {
                                        const CStructDecl *sd = findParamStruct(meth.params[i]);
                                        if (sd && sd->isPOD && sd->totalSize > 0) {
                                                structBufs_[i].decl = sd;
                                                if (structBufs_[i].data.empty())
                                                        structBufs_[i].data.assign(sd->totalSize, 0);
                                        }
                                }
                        }
                }
        } else {
                methArgBufs_.resize(meth.params.size());
                structBufs_.resize(meth.params.size());
                for (size_t i = 0; i < meth.params.size(); ++i) {
                        methArgBufs_[i]       = {};
                        structBufs_[i]        = {};
                        const CStructDecl *sd = findParamStruct(meth.params[i]);
                        if (sd && sd->isPOD && sd->totalSize > 0) {
                                structBufs_[i].decl = sd;
                                structBufs_[i].data.assign(sd->totalSize, 0);
                        }
                }
        }
}

// ─── doCallC ─────────────────────────────────────────────────────────────────

void
SdkPanel::doCallC()
{
        if (selectedFnIdx_ < 0 || selectedFnIdx_ >= (int)parseResult_.functions.size())
                return;
        if (!loader_.isLoaded()) {
                setStatus(tr("No DLL loaded", "未加载 DLL"), true);
                return;
        }

        const CFuncDecl         &fn = parseResult_.functions[selectedFnIdx_];
        std::vector<std::string> args(fn.params.size());
        for (size_t i = 0; i < fn.params.size(); ++i)
                args[i] = fnArgBufs_[i].text;

        CallResult res = loader_.call(fn, args);

        std::string callStr = fn.name + "(";
        for (size_t i = 0; i < args.size(); ++i) {
                if (i)
                        callStr += ", ";
                callStr += args[i];
        }
        callStr += ")";

        fnLastResult_   = res.ok ? res.display : res.error;
        fnLastResultOk_ = res.ok;

        double tsSec = sessionTimeSec();
        pushHistory(callStr, fnLastResult_, res.ok, tsSec);
        // Store the return value only into the user-chosen result variable (created
        // manually via the "+" button); no auto-creation.
        if (res.ok)
                writeResultVar(fnResultVar_, res, fn.retType);
}

// ─── doCallMethod ─────────────────────────────────────────────────────────────

void
SdkPanel::doCallMethod()
{
        if (selectedClassIdx_ < 0 || selectedMethodIdx_ < 0)
                return;
        if (!loader_.isLoaded()) {
                setStatus(tr("No DLL loaded", "未加载 DLL"), true);
                return;
        }

        const CClassDecl  &cls  = parseResult_.classes[selectedClassIdx_];
        const CMethodDecl &meth = cls.methods[selectedMethodIdx_];

        // Determine `this` pointer.
        void *thisPtr = nullptr;
        if (!meth.isCtor) {
                if (selectedObjIdx_ < 0 || selectedObjIdx_ >= (int)objects_.size()) {
                        setStatus(tr("Select an object first", "请先选择一个对象"), true);
                        return;
                }
                thisPtr = objects_[selectedObjIdx_].ptr;
        }

        std::vector<std::string> args(meth.params.size());
        for (size_t i = 0; i < meth.params.size(); ++i)
                args[i] = methArgBufs_[i].text;

        std::vector<void *> structPtrs(meth.params.size(), nullptr);
        for (size_t i = 0; i < structBufs_.size(); ++i) {
                if (structBufs_[i].decl && !structBufs_[i].data.empty())
                        structPtrs[i] = structBufs_[i].data.data();
        }

        CallResult res = loader_.callMethod(thisPtr, meth, args, structPtrs);

        // Build call string.
        std::string callStr = cls.name + "::" + meth.name + "(";
        for (size_t i = 0; i < args.size(); ++i) {
                if (i)
                        callStr += ", ";
                if (structPtrs[i])
                        callStr += "[struct]";
                else
                        callStr += args[i];
        }
        callStr += ")";

        // Look up enum name for integer return values.
        std::string display = res.ok ? res.display : res.error;
        if (res.ok && !meth.retRaw.empty()) {
                const CEnumDecl *ed = findEnum(parseResult_, meth.retRaw, cls.fullName);
                if (ed) {
                        for (const auto &ev : ed->values) {
                                if (ev.value == (int64_t)res.rawU64) {
                                        char tmp[64];
                                        snprintf(tmp, sizeof(tmp), "%lld (%s)", (long long)ev.value, ev.name.c_str());
                                        display = tmp;
                                        break;
                                }
                        }
                }
        }

        double tsSec = sessionTimeSec();
        pushHistory(callStr, display, res.ok, tsSec);
        if (!res.ok)
                setStatus(res.error, true);
        // Store the return value only into the user-chosen result variable.
        if (res.ok)
                writeResultVar(methResultVar_, res, meth.retType);
}

// ─── doNewObject ──────────────────────────────────────────────────────────────

void
SdkPanel::doNewObject(int classIdx)
{
        if (classIdx < 0 || classIdx >= (int)parseResult_.classes.size())
                return;
        if (!loader_.isLoaded()) {
                setStatus(tr("No DLL loaded", "未加载 DLL"), true);
                return;
        }

        const CClassDecl &cls = parseResult_.classes[classIdx];

        // Allocate a generous buffer for the object.
        size_t sz  = cls.instanceSize > 0 ? cls.instanceSize : 4096;
        void  *mem = malloc(sz);
        if (!mem) {
                setStatus("malloc failed", true);
                return;
        }
        memset(mem, 0, sz);

        // Find and call the default constructor.
        const CMethodDecl *ctorDecl = nullptr;
        for (const auto &m : cls.methods) {
                if (m.isCtor && m.params.empty()) {
                        ctorDecl = &m;
                        break;
                }
        }
        if (!ctorDecl) {
                // Try any constructor (might take args).
                for (const auto &m : cls.methods) {
                        if (m.isCtor) {
                                ctorDecl = &m;
                                break;
                        }
                }
        }

        int  fieldsSet           = 0;
        bool ctorCalledViaSymbol = false;

        if (ctorDecl) {
                std::vector<std::string> noArgs(ctorDecl->params.size(), "0");
                std::vector<void *>      noPtrs(ctorDecl->params.size(), nullptr);
                CallResult               r = loader_.callMethod(mem, *ctorDecl, noArgs, noPtrs);
                if (r.ok) {
                        ctorCalledViaSymbol = true;
                } else {
                        // Symbol not found → constructor is likely inline (defined in the
                        // header, not a standalone export).  Apply inline body assignments
                        // to emulate the constructor.
                        fieldsSet = applyInlineBody(mem, sz, cls, *ctorDecl, parseResult_);
                }
        }
        // No ctor decl or inline ctor → zero-initialized buffer is used.

        ObjInstance obj;
        snprintf(obj.label, sizeof(obj.label), "obj%d", (int)objects_.size());
        obj.ptr       = mem;
        obj.className = cls.fullName;
        objects_.push_back(std::move(obj));
        selectedObjIdx_ = (int)objects_.size() - 1;

        char buf[256];
        if (ctorCalledViaSymbol) {
                snprintf(buf,
                         sizeof(buf),
                         tr("Created %s @ %p (ctor called)", "已创建 %s @ %p (已调用构造函数)"),
                         cls.name.c_str(),
                         mem);
        } else if (fieldsSet > 0) {
                snprintf(buf,
                         sizeof(buf),
                         tr("Created %s @ %p (inline ctor: %d fields set)", "已创建 %s @ %p (内联构造: %d 字段已写入)"),
                         cls.name.c_str(),
                         mem,
                         fieldsSet);
        } else {
                snprintf(buf,
                         sizeof(buf),
                         tr("Created %s @ %p (zero-init, ctor not found)", "已创建 %s @ %p (零初始化, 未找到构造函数)"),
                         cls.name.c_str(),
                         mem);
        }
        setStatus(buf, fieldsSet == 0 && !ctorCalledViaSymbol);
}

// ─── doDeleteObject ───────────────────────────────────────────────────────────

void
SdkPanel::doDeleteObject(int idx)
{
        if (idx < 0 || idx >= (int)objects_.size())
                return;

        // Try to call destructor.
        if (selectedClassIdx_ >= 0 && selectedClassIdx_ < (int)parseResult_.classes.size()) {
                const CClassDecl &cls = parseResult_.classes[selectedClassIdx_];
                for (const auto &m : cls.methods) {
                        if (m.isDtor) {
                                std::vector<std::string> noArgs;
                                std::vector<void *>      noPtrs;
                                loader_.callMethod(objects_[idx].ptr, m, noArgs, noPtrs);
                                break;
                        }
                }
        }
        free(objects_[idx].ptr);
        objects_.erase(objects_.begin() + idx);
        if (selectedObjIdx_ >= (int)objects_.size())
                selectedObjIdx_ = (int)objects_.size() - 1;
}

// ─── lookup helpers ───────────────────────────────────────────────────────────

const CEnumDecl *
SdkPanel::findParamEnum(const CParam &p) const
{
        std::string raw = p.rawType;
        // Strip trailing *, &, const, spaces.
        while (!raw.empty() && (raw.back() == '*' || raw.back() == '&' || raw.back() == ' ' || raw.back() == '\t'))
                raw.pop_back();
        // Strip leading const.
        if (raw.size() > 6 && raw.substr(0, 6) == "const ")
                raw = raw.substr(6);
        std::string classFullName;
        if (selectedClassIdx_ >= 0 && selectedClassIdx_ < (int)parseResult_.classes.size())
                classFullName = parseResult_.classes[selectedClassIdx_].fullName;
        return findEnum(parseResult_, raw, classFullName);
}

const CEnumDecl *
SdkPanel::getParamEnumDecl(bool isCFunc, int classIdx, int methodIdx, int paramIdx) const
{
        std::string raw;
        std::string classFullName;
        if (isCFunc) {
                if (methodIdx < 0 || methodIdx >= (int)parseResult_.functions.size())
                        return nullptr;
                const CFuncDecl &fn = parseResult_.functions[methodIdx];
                if (paramIdx < 0 || paramIdx >= (int)fn.params.size())
                        return nullptr;
                raw = fn.params[paramIdx].rawType;
        } else {
                if (classIdx < 0 || classIdx >= (int)parseResult_.classes.size())
                        return nullptr;
                const CClassDecl &cls = parseResult_.classes[classIdx];
                classFullName         = cls.fullName;
                if (methodIdx < 0 || methodIdx >= (int)cls.methods.size())
                        return nullptr;
                const CMethodDecl &m = cls.methods[methodIdx];
                if (paramIdx < 0 || paramIdx >= (int)m.params.size())
                        return nullptr;
                raw = m.params[paramIdx].rawType;
        }
        while (!raw.empty() && (raw.back() == '*' || raw.back() == '&' || raw.back() == ' ' || raw.back() == '\t'))
                raw.pop_back();
        if (raw.size() > 6 && raw.substr(0, 6) == "const ")
                raw = raw.substr(6);
        return findEnum(parseResult_, raw, classFullName);
}

const CStructDecl *
SdkPanel::findParamStruct(const CParam &p) const
{
        std::string raw = p.rawType;
        // Must be a reference/pointer type to be a struct param.
        if (raw.find('&') == std::string::npos && raw.find('*') == std::string::npos)
                return nullptr;
        while (!raw.empty() && (raw.back() == '*' || raw.back() == '&' || raw.back() == ' ' || raw.back() == '\t'))
                raw.pop_back();
        if (raw.size() > 6 && raw.substr(0, 6) == "const ")
                raw = raw.substr(6);
        return findStruct(parseResult_, raw);
}

// ─── Python runner script (embedded) ─────────────────────────────────────────

static const char kPyRunnerScript[] =
    "import sys, json, importlib.util, traceback, ast, os\n"
    "_module = None\n"
    "_objs   = {}\n"
    "_nid    = 0\n"
    // Duplicate stdout FD before any user code can change it.
    // All RPC replies go through _rpc_fd so subprocess/print from user code
    // cannot pollute the JSON-RPC pipe even if FD 1 is temporarily redirected.
    "_rpc_fd = os.dup(sys.stdout.fileno())\n"
    "\n"
    "def _send(obj):\n"
    "    os.write(_rpc_fd, (json.dumps(obj) + '\\n').encode())\n"
    "\n"
    "def _cvt(v):\n"
    "    if not isinstance(v, str): return v\n"
    "    try: return ast.literal_eval(v)\n"
    "    except Exception: return v\n"
    "\n"
    // Redirect FD 1 -> stderr during user-code execution so that user print()
    // calls and subprocess stdout cannot leak into the RPC pipe.
    "def _isolated(fn):\n"
    "    saved = os.dup(1)\n"
    "    os.dup2(2, 1)\n"
    "    try:\n"
    "        return fn()\n"
    "    finally:\n"
    "        os.dup2(saved, 1)\n"
    "        os.close(saved)\n"
    "\n"
    "def _run():\n"
    "    global _module, _objs, _nid\n"
    "    for raw in sys.stdin:\n"
    "        raw = raw.strip()\n"
    "        if not raw: continue\n"
    "        try: cmd = json.loads(raw)\n"
    "        except Exception:\n"
    "            _send({'ok': False, 'error': 'bad JSON'})\n"
    "            continue\n"
    "        a = cmd.get('action', '')\n"
    "        try:\n"
    "            if a == 'ping':\n"
    "                _send({'ok': True, 'result': 'pong'})\n"
    "            elif a == 'load':\n"
    "                spec = importlib.util.spec_from_file_location('_ava_user', cmd['path'])\n"
    "                mod  = importlib.util.module_from_spec(spec)\n"
    "                _isolated(lambda: spec.loader.exec_module(mod))\n"
    "                _module = mod\n"
    "                _send({'ok': True, 'result': 'loaded'})\n"
    "            elif a == 'call_func':\n"
    "                fn   = getattr(_module, cmd['name'])\n"
    "                args = [_cvt(v) for v in cmd.get('args', [])]\n"
    "                r    = _isolated(lambda: fn(*args))\n"
    "                _send({'ok': True, 'result': str(r) if r is not None else 'None'})\n"
    "            elif a == 'new_obj':\n"
    "                cls  = getattr(_module, cmd['class'])\n"
    "                args = [_cvt(v) for v in cmd.get('args', [])]\n"
    "                oid  = _nid; _nid += 1\n"
    "                _objs[oid] = _isolated(lambda: cls(*args))\n"
    "                _send({'ok': True, 'result': '{}#{}'.format(cmd['class'], oid), 'id': oid})\n"
    "            elif a == 'call_meth':\n"
    "                obj = _objs.get(cmd['id'])\n"
    "                if obj is None:\n"
    "                    _send({'ok': False, 'error': 'object not found: id={}'.format(cmd['id'])})\n"
    "                    continue\n"
    "                m    = getattr(obj, cmd['name'])\n"
    "                args = [_cvt(v) for v in cmd.get('args', [])]\n"
    "                r    = _isolated(lambda: m(*args))\n"
    "                _send({'ok': True, 'result': str(r) if r is not None else 'None'})\n"
    "            elif a == 'del_obj':\n"
    "                _objs.pop(cmd['id'], None)\n"
    "                _send({'ok': True, 'result': 'deleted'})\n"
    "            else:\n"
    "                _send({'ok': False, 'error': 'unknown: ' + a})\n"
    "        except Exception:\n"
    "            _send({'ok': False, 'error': traceback.format_exc()})\n"
    "\n"
    "_run()\n";

// ─── PyRunner — persistent Python subprocess ──────────────────────────────────

struct SdkPanel::PyRunner {
#ifdef _WIN32
        HANDLE hProc{nullptr};
        HANDLE hStdinW{nullptr};
        HANDLE hStdoutR{nullptr};
#endif
        bool alive_{false};

        ~PyRunner() { shutdown(); }

        bool launch(const std::string &scriptPath, const std::string &pyExe)
        {
#ifdef _WIN32
                SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
                HANDLE              hStdinR = nullptr, hStdoutW = nullptr;
                if (!CreatePipe(&hStdinR, &hStdinW, &sa, 0))
                        return false;
                if (!CreatePipe(&hStdoutR, &hStdoutW, &sa, 0)) {
                        CloseHandle(hStdinR);
                        CloseHandle(hStdinW);
                        hStdinW = nullptr;
                        return false;
                }
                SetHandleInformation(hStdinW, HANDLE_FLAG_INHERIT, 0);
                SetHandleInformation(hStdoutR, HANDLE_FLAG_INHERIT, 0);

                std::string cmd = "\"" + pyExe + "\" -u \"" + scriptPath + "\"";

                STARTUPINFOA si{};
                si.cb         = sizeof(si);
                si.dwFlags    = STARTF_USESTDHANDLES;
                si.hStdInput  = hStdinR;
                si.hStdOutput = hStdoutW;
                si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

                PROCESS_INFORMATION pi{};
                bool                ok = CreateProcessA(
                              nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0;
                CloseHandle(hStdinR);
                CloseHandle(hStdoutW);

                if (!ok) {
                        CloseHandle(hStdinW);
                        CloseHandle(hStdoutR);
                        hStdinW = hStdoutR = nullptr;
                        return false;
                }
                hProc = pi.hProcess;
                CloseHandle(pi.hThread);
                alive_ = true;
                // Ping to verify
                auto r = xact(R"({"action":"ping"})", 3000);
                if (r.find("pong") == std::string::npos) {
                        shutdown();
                        return false;
                }
                return true;
#else
                (void)scriptPath;
                (void)pyExe;
                return false;
#endif
        }

        void shutdown()
        {
#ifdef _WIN32
                if (hProc) {
                        TerminateProcess(hProc, 0);
                        WaitForSingleObject(hProc, 2000);
                        CloseHandle(hProc);
                        hProc = nullptr;
                }
                if (hStdinW) {
                        CloseHandle(hStdinW);
                        hStdinW = nullptr;
                }
                if (hStdoutR) {
                        CloseHandle(hStdoutR);
                        hStdoutR = nullptr;
                }
#endif
                alive_ = false;
        }

        bool isAlive()
        {
#ifdef _WIN32
                if (!alive_ || !hProc)
                        return false;
                DWORD exit = 0;
                if (GetExitCodeProcess(hProc, &exit) && exit != STILL_ACTIVE)
                        alive_ = false;
#endif
                return alive_;
        }

        std::string xact(const std::string &jsonLine, int timeoutMs = 5000)
        {
                if (!alive_)
                        return R"({"ok":false,"error":"runner not started"})";
#ifdef _WIN32
                std::string msg = jsonLine + "\n";
                DWORD       written;
                if (!WriteFile(hStdinW, msg.c_str(), (DWORD)msg.size(), &written, nullptr) || written != (DWORD)msg.size()) {
                        alive_ = false;
                        return R"({"ok":false,"error":"write failed"})";
                }
                // Read response line with timeout via PeekNamedPipe polling.
                std::string result;
                char        c;
                DWORD       rd;
                DWORD       deadline = GetTickCount() + (DWORD)timeoutMs;
                while (true) {
                        DWORD avail = 0;
                        if (!PeekNamedPipe(hStdoutR, nullptr, 0, nullptr, &avail, nullptr)) {
                                alive_ = false;
                                break;
                        }
                        if (avail == 0) {
                                DWORD ex = 0;
                                if (GetExitCodeProcess(hProc, &ex) && ex != STILL_ACTIVE) {
                                        alive_ = false;
                                        break;
                                }
                                if (GetTickCount() >= deadline)
                                        return R"({"ok":false,"error":"timeout"})";
                                Sleep(2);
                                continue;
                        }
                        if (!ReadFile(hStdoutR, &c, 1, &rd, nullptr) || rd == 0) {
                                alive_ = false;
                                break;
                        }
                        if (c == '\n')
                                break;
                        if (c != '\r')
                                result += c;
                }
                return result.empty() ? R"({"ok":false,"error":"no response"})" : result;
#else
                return R"({"ok":false,"error":"not supported"})";
#endif
        }
};

// ─── Python response parser ───────────────────────────────────────────────────

struct PyResp {
        bool        ok{false};
        std::string result;
        std::string error;
        int         id{-1};
};

static PyResp
parsePyResp(const std::string &json)
{
        PyResp r;
        cJSON *obj = cJSON_Parse(json.c_str());
        if (!obj) {
                r.error = "invalid JSON: " + json;
                return r;
        }
        if (const cJSON *v = cJSON_GetObjectItem(obj, "ok"))
                r.ok = cJSON_IsTrue(v);
        if (const cJSON *v = cJSON_GetObjectItem(obj, "result"); cJSON_IsString(v))
                r.result = v->valuestring;
        if (const cJSON *v = cJSON_GetObjectItem(obj, "error"); cJSON_IsString(v))
                r.error = v->valuestring;
        if (const cJSON *v = cJSON_GetObjectItem(obj, "id"); cJSON_IsNumber(v))
                r.id = v->valueint;
        cJSON_Delete(obj);
        return r;
}

// ─── Python file parser ───────────────────────────────────────────────────────

static void
parsePyParams(const std::string &raw, std::vector<PyParam> &out, bool skipSelf)
{
        bool   first = skipSelf;
        size_t i     = 0;
        while (i <= raw.size()) {
                // Find next comma at depth 0
                size_t j     = i;
                int    depth = 0;
                while (j < raw.size()) {
                        char ch = raw[j];
                        if (ch == '(' || ch == '[' || ch == '{')
                                ++depth;
                        else if (ch == ')' || ch == ']' || ch == '}')
                                --depth;
                        else if (ch == ',' && depth == 0)
                                break;
                        ++j;
                }
                std::string tok = raw.substr(i, j - i);
                i               = j + 1;

                // Trim
                while (!tok.empty() &&
                       (tok.front() == ' ' || tok.front() == '\t' || tok.front() == '\n' || tok.front() == '\r'))
                        tok.erase(tok.begin());
                while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t' || tok.back() == '\n' || tok.back() == '\r'))
                        tok.pop_back();

                if (tok.empty() || tok[0] == '*')
                        continue;
                if (first && (tok == "self" || tok == "cls")) {
                        first = false;
                        continue;
                }
                first = false;

                PyParam p;
                size_t  eqPos = tok.find('=');
                if (eqPos != std::string::npos) {
                        p.defaultVal = tok.substr(eqPos + 1);
                        tok          = tok.substr(0, eqPos);
                        while (!p.defaultVal.empty() && p.defaultVal.front() == ' ')
                                p.defaultVal.erase(p.defaultVal.begin());
                        while (!tok.empty() && tok.back() == ' ')
                                tok.pop_back();
                }
                size_t colonPos = tok.find(':');
                if (colonPos != std::string::npos)
                        tok = tok.substr(0, colonPos);
                while (!tok.empty() && tok.back() == ' ')
                        tok.pop_back();

                p.name = tok;
                if (!p.name.empty())
                        out.push_back(std::move(p));
        }
}

static PyParseResult
parsePyFile(const std::string &src)
{
        PyParseResult r;
        // Normalize CRLF
        std::string s;
        s.reserve(src.size());
        for (char c : src)
                if (c != '\r')
                        s += c;

        std::istringstream iss(s);
        std::string        line;
        int                classIdx = -1;

        while (std::getline(iss, line)) {
                if (line.empty())
                        continue;

                // Count leading indent (tab = 4 for column tracking)
                int    indent = 0;
                size_t p      = 0;
                while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) {
                        if (line[p] == '\t')
                                indent = ((indent / 4) + 1) * 4;
                        else
                                ++indent;
                        ++p;
                }
                std::string trimmed = line.substr(p);
                if (trimmed.empty() || trimmed[0] == '#')
                        continue;

                // Leaving a class body when we see a top-level non-empty line
                if (classIdx >= 0 && indent == 0)
                        classIdx = -1;

                if (indent == 0 && trimmed.size() > 6 && trimmed.substr(0, 6) == "class ") {
                        size_t      ni = 6;
                        std::string name;
                        while (ni < trimmed.size() && (isalnum((unsigned char)trimmed[ni]) || trimmed[ni] == '_'))
                                name += trimmed[ni++];
                        if (!name.empty()) {
                                PyClassDecl cls;
                                cls.name = name;
                                classIdx = (int)r.classes.size();
                                r.classes.push_back(std::move(cls));
                        }
                } else if (trimmed.size() > 4 && trimmed.substr(0, 4) == "def ") {
                        size_t      ni = 4;
                        std::string name;
                        while (ni < trimmed.size() && (isalnum((unsigned char)trimmed[ni]) || trimmed[ni] == '_'))
                                name += trimmed[ni++];
                        if (name.empty())
                                continue;

                        // Extract param string between '(' and matching ')'
                        size_t pOpen  = trimmed.find('(', ni);
                        size_t pClose = std::string::npos;
                        if (pOpen != std::string::npos) {
                                int depth = 0;
                                for (size_t k = pOpen + 1; k < trimmed.size(); ++k) {
                                        if (trimmed[k] == '(')
                                                ++depth;
                                        else if (trimmed[k] == ')') {
                                                if (depth == 0) {
                                                        pClose = k;
                                                        break;
                                                }
                                                --depth;
                                        }
                                }
                        }
                        std::string paramStr;
                        if (pOpen != std::string::npos && pClose != std::string::npos)
                                paramStr = trimmed.substr(pOpen + 1, pClose - pOpen - 1);

                        PyFuncDecl fn;
                        fn.name = name;
                        parsePyParams(paramStr, fn.params, classIdx >= 0);

                        if (classIdx >= 0)
                                r.classes[classIdx].methods.push_back(std::move(fn));
                        else
                                r.functions.push_back(std::move(fn));
                }
        }
        return r;
}

// ─── SdkPanel destructor ─────────────────────────────────────────────────────

SdkPanel::SdkPanel()  = default; // PyRunner complete here — unique_ptr can be default-init'd
SdkPanel::~SdkPanel() = default; // PyRunner shut down via unique_ptr destructor

// ─── doStartPyRunner ─────────────────────────────────────────────────────────

void
SdkPanel::doStartPyRunner()
{
        if (!pyRunner_)
                pyRunner_ = std::make_unique<PyRunner>();
        if (pyRunner_->isAlive())
                return;

        // Write runner script to temp dir.
        std::filesystem::path tmpScript = std::filesystem::temp_directory_path() / "ava_py_runner.py";
        {
                std::ofstream f(tmpScript);
                if (!f) {
                        setStatus(tr("Failed to write Python runner script", "无法写入 Python 运行脚本"), true);
                        return;
                }
                f << kPyRunnerScript;
        }

        for (const char *exe : {"python", "python3"}) {
                if (pyRunner_->launch(tmpScript.string(), exe)) {
                        setStatus(tr("Python runner started", "Python 已启动"), false);
                        return;
                }
        }
        setStatus(tr("Python not found — install Python and add to PATH", "未找到 Python — 请安装 Python 并添加到 PATH"), true);
}

// ─── doLoadPy ────────────────────────────────────────────────────────────────

void
SdkPanel::doLoadPy()
{
        if (!pyPath_[0])
                return;
        std::ifstream f(std::filesystem::u8path(pyPath_));
        if (!f) {
                setStatus(tr("Cannot open Python file", "无法打开 Python 文件"), true);
                return;
        }
        std::ostringstream ss;
        ss << f.rdbuf();

        pyResult_   = parsePyFile(ss.str());
        pySelFnIdx_ = pySelClsIdx_ = pySelMethIdx_ = pySelObjIdx_ = -1;
        pyFnArgBufs_.clear();
        pyMethArgBufs_.clear();
        pyFnArgBufsCache_.clear();
        pyMethArgBufsCache_.clear();
        pyObjects_.clear();

        if (!pyRunner_ || !pyRunner_->isAlive())
                doStartPyRunner();

        if (pyRunner_ && pyRunner_->isAlive()) {
                cJSON *cmd = cJSON_CreateObject();
                cJSON_AddStringToObject(cmd, "action", "load");
                // Python prefers forward slashes
                std::string fwdPath = pyPath_;
                for (char &c : fwdPath)
                        if (c == '\\')
                                c = '/';
                cJSON_AddStringToObject(cmd, "path", fwdPath.c_str());
                char *s  = cJSON_PrintUnformatted(cmd);
                auto  rr = parsePyResp(pyRunner_->xact(s));
                cJSON_free(s);
                cJSON_Delete(cmd);
                if (!rr.ok) {
                        setStatus(tr("Python load error: ", "Python 加载错误: ") + rr.error, true);
                        return;
                }
        }

        char buf[256];
        snprintf(buf,
                 sizeof(buf),
                 tr("Python: %d function(s), %d class(es)", "Python: %d 个函数, %d 个类"),
                 (int)pyResult_.functions.size(),
                 (int)pyResult_.classes.size());
        setStatus(buf, false);
}

// ─── doCallPyFunc ────────────────────────────────────────────────────────────

void
SdkPanel::doCallPyFunc()
{
        if (pySelFnIdx_ < 0 || pySelFnIdx_ >= (int)pyResult_.functions.size())
                return;
        if (!pyRunner_ || !pyRunner_->isAlive()) {
                setStatus(tr("Python not running", "Python 未运行"), true);
                return;
        }
        const PyFuncDecl &fn = pyResult_.functions[pySelFnIdx_];

        cJSON *args = cJSON_CreateArray();
        for (size_t i = 0; i < fn.params.size(); ++i)
                cJSON_AddItemToArray(args, cJSON_CreateString(i < pyFnArgBufs_.size() ? pyFnArgBufs_[i].text : ""));

        cJSON *cmd = cJSON_CreateObject();
        cJSON_AddStringToObject(cmd, "action", "call_func");
        cJSON_AddStringToObject(cmd, "name", fn.name.c_str());
        cJSON_AddItemToObject(cmd, "args", args);
        char *s  = cJSON_PrintUnformatted(cmd);
        auto  rr = parsePyResp(pyRunner_->xact(s, 10000));
        cJSON_free(s);
        cJSON_Delete(cmd);

        pyLastResult_   = rr.ok ? rr.result : rr.error;
        pyLastResultOk_ = rr.ok;

        std::string call = fn.name + "(";
        for (size_t i = 0; i < fn.params.size(); ++i) {
                if (i)
                        call += ", ";
                call += (i < pyFnArgBufs_.size()) ? pyFnArgBufs_[i].text : "";
        }
        call += ")";
        pushHistory(call, pyLastResult_, rr.ok, sessionTimeSec());

        if (rr.ok && pyFnResultVar_[0] != '\0' && onGetVarBuf_) {
                void *buf = onGetVarBuf_(pyFnResultVar_);
                if (buf) {
                        int64_t v = (int64_t)strtoll(rr.result.c_str(), nullptr, 0);
                        memcpy(buf, &v, sizeof(int64_t));
                        if (onVarWritten_)
                                onVarWritten_(pyFnResultVar_);
                }
        }
}

// ─── doNewPyObject / doNewPyObjectWithArgs ────────────────────────────────────

void
SdkPanel::doNewPyObject()
{
        doNewPyObjectWithArgs({});
}

void
SdkPanel::doNewPyObjectWithArgs(const std::vector<std::string> &ctorArgs)
{
        if (pySelClsIdx_ < 0 || pySelClsIdx_ >= (int)pyResult_.classes.size())
                return;
        if (!pyRunner_ || !pyRunner_->isAlive()) {
                setStatus(tr("Python not running", "Python 未运行"), true);
                return;
        }
        const PyClassDecl &cls = pyResult_.classes[pySelClsIdx_];

        cJSON *args = cJSON_CreateArray();
        for (const auto &a : ctorArgs)
                cJSON_AddItemToArray(args, cJSON_CreateString(a.c_str()));

        cJSON *cmd = cJSON_CreateObject();
        cJSON_AddStringToObject(cmd, "action", "new_obj");
        cJSON_AddStringToObject(cmd, "class", cls.name.c_str());
        cJSON_AddItemToObject(cmd, "args", args);
        char *s  = cJSON_PrintUnformatted(cmd);
        auto  rr = parsePyResp(pyRunner_->xact(s));
        cJSON_free(s);
        cJSON_Delete(cmd);

        if (!rr.ok) {
                setStatus(tr("Create object failed: ", "创建对象失败: ") + rr.error, true);
                return;
        }
        PyObjEntry e;
        e.pyId      = rr.id;
        e.className = cls.name;
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "obj%d", (int)pyObjects_.size());
        e.label = lbl;
        pyObjects_.push_back(std::move(e));
        pySelObjIdx_ = (int)pyObjects_.size() - 1;

        char buf[128];
        snprintf(buf, sizeof(buf), tr("Created %s#%d", "已创建 %s#%d"), cls.name.c_str(), rr.id);
        setStatus(buf, false);
}

// ─── doCallPyMeth ────────────────────────────────────────────────────────────

void
SdkPanel::doCallPyMeth()
{
        if (pySelClsIdx_ < 0 || pySelMethIdx_ < 0)
                return;
        const PyClassDecl &cls = pyResult_.classes[pySelClsIdx_];
        if (pySelMethIdx_ >= (int)cls.methods.size())
                return;
        const PyFuncDecl &meth   = cls.methods[pySelMethIdx_];
        bool              isInit = (meth.name == "__init__");

        if (!pyRunner_ || !pyRunner_->isAlive()) {
                setStatus(tr("Python not running", "Python 未运行"), true);
                return;
        }

        if (isInit) {
                // Delegate to new-object path passing the __init__ args.
                std::vector<std::string> ctorArgs;
                for (size_t i = 0; i < meth.params.size(); ++i)
                        ctorArgs.push_back(i < pyMethArgBufs_.size() ? pyMethArgBufs_[i].text : "");
                doNewPyObjectWithArgs(ctorArgs);
                return;
        }

        if (pySelObjIdx_ < 0 || pySelObjIdx_ >= (int)pyObjects_.size()) {
                setStatus(tr("Select an object first", "请先选择一个对象"), true);
                return;
        }

        cJSON *args = cJSON_CreateArray();
        for (size_t i = 0; i < meth.params.size(); ++i)
                cJSON_AddItemToArray(args, cJSON_CreateString(i < pyMethArgBufs_.size() ? pyMethArgBufs_[i].text : ""));

        cJSON *cmd = cJSON_CreateObject();
        cJSON_AddStringToObject(cmd, "action", "call_meth");
        cJSON_AddNumberToObject(cmd, "id", pyObjects_[pySelObjIdx_].pyId);
        cJSON_AddStringToObject(cmd, "name", meth.name.c_str());
        cJSON_AddItemToObject(cmd, "args", args);
        char *s  = cJSON_PrintUnformatted(cmd);
        auto  rr = parsePyResp(pyRunner_->xact(s, 10000));
        cJSON_free(s);
        cJSON_Delete(cmd);

        std::string result = rr.ok ? rr.result : rr.error;
        std::string call   = cls.name + "." + meth.name + "(";
        for (size_t i = 0; i < meth.params.size(); ++i) {
                if (i)
                        call += ", ";
                call += (i < pyMethArgBufs_.size()) ? pyMethArgBufs_[i].text : "";
        }
        call += ")";
        pushHistory(call, result, rr.ok, sessionTimeSec());
        if (!rr.ok)
                setStatus(rr.error, true);
}

// ─── doDeletePyObject ────────────────────────────────────────────────────────

void
SdkPanel::doDeletePyObject(int idx)
{
        if (idx < 0 || idx >= (int)pyObjects_.size())
                return;
        if (pyRunner_ && pyRunner_->isAlive()) {
                cJSON *cmd = cJSON_CreateObject();
                cJSON_AddStringToObject(cmd, "action", "del_obj");
                cJSON_AddNumberToObject(cmd, "id", pyObjects_[idx].pyId);
                char *s = cJSON_PrintUnformatted(cmd);
                pyRunner_->xact(s);
                cJSON_free(s);
                cJSON_Delete(cmd);
        }
        pyObjects_.erase(pyObjects_.begin() + idx);
        if (pySelObjIdx_ >= (int)pyObjects_.size())
                pySelObjIdx_ = (int)pyObjects_.size() - 1;
}

// ─── drawCFunctionsTab ────────────────────────────────────────────────────────

// ─── Splitter helper ──────────────────────────────────────────────────────────

static void
splitterV(const char *id, float *w, float minW, float maxW)
{
        ImGui::SameLine(0, 0);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.20f, 0.20f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.40f, 0.60f, 0.90f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.40f, 0.60f, 0.90f, 1.f));
        ImGui::Button(id, ImVec2(4.0f, -1.0f));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive())
                *w = std::clamp(*w + ImGui::GetIO().MouseDelta.x, minW, maxW);
        ImGui::SameLine(0, 0);
        ImGui::PopStyleColor(3);
}

static void
splitterH(const char *id, float *h, float minH, float maxH)
{
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.20f, 0.20f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.40f, 0.60f, 0.90f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.40f, 0.60f, 0.90f, 1.f));
        ImGui::Button(id, ImVec2(-1.0f, 4.0f));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive())
                *h = std::clamp(*h + ImGui::GetIO().MouseDelta.y, minH, maxH);
        ImGui::PopStyleColor(3);
}

// ─── drawCFunctionsTab ────────────────────────────────────────────────────────

static float sFnSplitW = 220.0f;

void
SdkPanel::drawCFunctionsTab()
{
        float &listWidth = sFnSplitW;
        ImGui::BeginChild("##fnlist", ImVec2(listWidth, 0), true);
        ImGui::TextDisabled("%s (%d)", tr("Functions", "函数列表"), (int)parseResult_.functions.size());
        ImGui::Separator();
        for (int i = 0; i < (int)parseResult_.functions.size(); ++i) {
                const CFuncDecl &fn  = parseResult_.functions[i];
                bool             sel = (i == selectedFnIdx_);
                char             label[256];
                snprintf(label,
                         sizeof(label),
                         "%s  %s",
                         fn.retRaw.empty() ? ctypeLabel(fn.retType) : fn.retRaw.c_str(),
                         fn.name.c_str());
                if (ImGui::Selectable(label, sel) && !sel)
                        selectFn(i);
                // Drag to SequenceEditor to create an SDK call step
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                        SdkDragPayload p;
                        p.panelWinId = winId_;
                        p.isCFunc    = true;
                        p.methodIdx  = i;
                        ImGui::SetDragDropPayload("SDK_CALL", &p, sizeof(p));
                        ImGui::TextUnformatted(fn.name.c_str());
                        ImGui::EndDragDropSource();
                }
        }
        ImGui::EndChild();
        splitterV("##fnsplit", &listWidth, 80.0f, 500.0f);

        ImGui::BeginChild("##callui", ImVec2(0, 0), false);
        if (selectedFnIdx_ < 0 || selectedFnIdx_ >= (int)parseResult_.functions.size()) {
                ImGui::TextDisabled("%s", tr("← Select a function", "← 选择左侧函数"));
        } else {
                const CFuncDecl &fn = parseResult_.functions[selectedFnIdx_];
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.85f, 1.0f, 1.0f));
                ImGui::Text("%s  %s", fn.retRaw.c_str(), fn.name.c_str());
                ImGui::PopStyleColor();

                {
                        static ImGuiTableFlags tblFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
                        if (ImGui::BeginTable("##cfarg_tbl", 3, tblFlags)) {
                                ImGui::TableSetupColumn(tr("#", "#"), ImGuiTableColumnFlags_WidthFixed, 75.0f);
                                ImGui::TableSetupColumn(tr("Name", "名称"), ImGuiTableColumnFlags_WidthStretch, 0.35f);
                                ImGui::TableSetupColumn(
                                    tr("Value / Args", "值/参数"), ImGuiTableColumnFlags_WidthStretch, 0.65f);
                                ImGui::TableHeadersRow();

                                for (size_t i = 0; i < fn.params.size(); ++i) {
                                        const CParam &p       = fn.params[i];
                                        bool          isCharP = ctypeIsCharPtr(p.type, p.rawType);
                                        char          id[64];
                                        snprintf(id, sizeof(id), "##farg%zu", i);
                                        const char *rawT = p.rawType.empty() ? ctypeLabel(p.type) : p.rawType.c_str();

                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0);
                                        ImGui::TextUnformatted(rawT);
                                        ImGui::TableSetColumnIndex(1);
                                        if (!p.name.empty())
                                                ImGui::TextUnformatted(p.name.c_str());
                                        else
                                                ImGui::Text("[%zu]", i);
                                        ImGui::TableSetColumnIndex(2);
                                        if (i < fnArgBufs_.size()) {
                                                bool hasCreate = (bool)onCreateLocalVar_;
                                                ImGui::SetNextItemWidth(
                                                    hasCreate ? ImGui::GetContentRegionAvail().x - 24.0f : -FLT_MIN);
                                                if (isCharP)
                                                        ImGui::InputText(id, fnArgBufs_[i].text, sizeof(ArgBuf::text));
                                                else if (ctypeIsFloat(p.type))
                                                        ImGui::InputText(id,
                                                                         fnArgBufs_[i].text,
                                                                         sizeof(ArgBuf::text),
                                                                         ImGuiInputTextFlags_CharsDecimal);
                                                else
                                                        ImGui::InputText(id,
                                                                         fnArgBufs_[i].text,
                                                                         sizeof(ArgBuf::text),
                                                                         ImGuiInputTextFlags_CharsHexadecimal |
                                                                             ImGuiInputTextFlags_CharsDecimal);
                                                if (hasCreate) {
                                                        ImGui::SameLine(0, 2);
                                                        char nbId[32];
                                                        snprintf(nbId, sizeof(nbId), "+##fnewp%zu", i);
                                                        if (ImGui::SmallButton(nbId)) {
                                                                std::string vn =
                                                                    p.name.empty() ? ("arg" + std::to_string(i)) : p.name;
                                                                onCreateLocalVar_(vn, p.type, findParamStruct(p));
                                                        }
                                                        if (ImGui::IsItemHovered())
                                                                ImGui::SetTooltip(
                                                                    "%s",
                                                                    tr("Add this parameter as a variable",
                                                                       "把该参数添加为变量"));
                                                }
                                        }
                                }

                                // ── Return value row ──
                                {
                                        std::string retT = fn.retRaw.empty() ? std::string(ctypeLabel(fn.retType)) : fn.retRaw;
                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0);
                                        ImGui::TextUnformatted(retT.c_str());
                                        ImGui::TableSetColumnIndex(1);
                                        ImGui::TextUnformatted(tr("return", "返回值"));
                                        ImGui::TableSetColumnIndex(2);
                                        std::vector<std::string> localVars;
                                        if (onListLocalVars_)
                                                localVars = onListLocalVars_();
                                        bool  hasCreate = onCreateLocalVar_ && !ctypeIsVoid(fn.retType);
                                        bool  hasPick   = !localVars.empty();
                                        float reserve   = (hasCreate ? 24.0f : 0.0f) + (hasPick ? 24.0f : 0.0f);
                                        float rvW       = reserve > 0.0f ? ImGui::GetContentRegionAvail().x - reserve : -FLT_MIN;
                                        ImGui::SetNextItemWidth(rvW);
                                        ImGui::InputTextWithHint(
                                            "##fnrv", tr("variable name", "变量名"), fnResultVar_, sizeof(fnResultVar_));
                                        if (hasCreate) {
                                                ImGui::SameLine(0, 2);
                                                if (ImGui::SmallButton("+##fnrvnew")) {
                                                        std::string vn = fnResultVar_[0] ? std::string(fnResultVar_) : fn.name;
                                                        onCreateLocalVar_(vn, fn.retType, nullptr);
                                                        strncpy(fnResultVar_, vn.c_str(), sizeof(fnResultVar_) - 1);
                                                        fnResultVar_[sizeof(fnResultVar_) - 1] = '\0';
                                                }
                                                if (ImGui::IsItemHovered())
                                                        ImGui::SetTooltip("%s",
                                                                          tr("Create result variable", "创建结果变量"));
                                        }
                                        if (hasPick) {
                                                ImGui::SameLine(0, 2);
                                                if (ImGui::SmallButton("v##fnrvpick"))
                                                        ImGui::OpenPopup("##fnrvpop");
                                                if (ImGui::BeginPopup("##fnrvpop")) {
                                                        for (const auto &vn : localVars)
                                                                if (ImGui::Selectable(vn.c_str())) {
                                                                        strncpy(fnResultVar_, vn.c_str(), sizeof(fnResultVar_) - 1);
                                                                        fnResultVar_[sizeof(fnResultVar_) - 1] = '\0';
                                                                }
                                                        ImGui::EndPopup();
                                                }
                                        }
                                }

                                ImGui::EndTable();
                        }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                bool canCall = loader_.isLoaded();
                if (!canCall)
                        ImGui::BeginDisabled();
                if (ui::Button(tr("  CALL  ", "  调用  "), ui::BtnStyle::Success))
                        doCallC();
                if (!canCall) {
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", tr("(load a DLL first)", "(请先加载 DLL)"));
                }

                if (!fnLastResult_.empty()) {
                        ImGui::Spacing();
                        ImGui::Text("%s", tr("Result:", "返回值:"));
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              fnLastResultOk_ ? ImVec4(0.3f, 1.f, 0.5f, 1.f) : ImVec4(1.f, 0.4f, 0.4f, 1.f));
                        ImGui::TextUnformatted(fnLastResult_.c_str());
                        ImGui::PopStyleColor();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextDisabled("%s", tr("History", "调用历史"));
                float histH = ImGui::GetContentRegionAvail().y;
                ImGui::BeginChild("##fhist", ImVec2(0, histH), true);
                std::vector<HistEntry> histSnap;
                {
                        std::lock_guard<std::mutex> lk(histMtx_);
                        histSnap = history_;
                }
                for (int i = (int)histSnap.size() - 1; i >= 0; --i) {
                        const HistEntry &e = histSnap[i];
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
                                              e.ok ? ImVec4(0.85f, 0.85f, 0.85f, 1.f) : ImVec4(1.f, 0.4f, 0.4f, 1.f));
                        ImGui::TextUnformatted(e.call.c_str());
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              e.ok ? ImVec4(0.3f, 1.f, 0.5f, 1.f) : ImVec4(1.f, 0.5f, 0.3f, 1.f));
                        ImGui::TextUnformatted(e.result.c_str());
                        ImGui::PopStyleColor();
                }
                ImGui::EndChild();
        }
        ImGui::EndChild();
}

// ─── drawCppClassesTab ────────────────────────────────────────────────────────

void
SdkPanel::drawCppClassesTab()
{
        if (parseResult_.classes.empty()) {
                ImGui::TextDisabled("%s", tr("No classes found in header.", "头文件中未找到类定义。"));
                return;
        }

        float &leftW = clsSplitW_;
        ImGui::BeginChild("##clslist", ImVec2(leftW, 0), true);

        // ── class list ──
        ImGui::TextDisabled("%s (%d)", tr("Classes", "类列表"), (int)parseResult_.classes.size());
        ImGui::Separator();
        for (int ci = 0; ci < (int)parseResult_.classes.size(); ++ci) {
                const CClassDecl &cls    = parseResult_.classes[ci];
                bool              selCls = (ci == selectedClassIdx_);
                char              clsLbl[128];
                snprintf(clsLbl, sizeof(clsLbl), "%s (%d)", cls.name.c_str(), (int)cls.methods.size());
                if (ImGui::Selectable(clsLbl, selCls, ImGuiSelectableFlags_None) && !selCls) {
                        if (selectedClassIdx_ >= 0 && selectedMethodIdx_ >= 0) {
                                int key                = selectedClassIdx_ * 10000 + selectedMethodIdx_;
                                methArgBufsCache_[key] = methArgBufs_;
                                structBufsCache_[key]  = structBufs_;
                        }
                        selectedClassIdx_  = ci;
                        selectedMethodIdx_ = -1;
                        selectedObjIdx_    = -1;
                        methArgBufs_.clear();
                        structBufs_.clear();
                }
        }

        // ── method list for selected class ──
        if (selectedClassIdx_ >= 0 && selectedClassIdx_ < (int)parseResult_.classes.size()) {
                const CClassDecl &cls = parseResult_.classes[selectedClassIdx_];
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextDisabled("%s (%d)", tr("Methods", "方法列表"), (int)cls.methods.size());
                ImGui::Separator();
                for (int mi = 0; mi < (int)cls.methods.size(); ++mi) {
                        const CMethodDecl &m    = cls.methods[mi];
                        bool               selM = (mi == selectedMethodIdx_);
                        char               mLbl[256];
                        const char        *prefix = m.isCtor ? "[ctor]" : m.isDtor ? "[dtor]" : "";
                        if (m.isCtor || m.isDtor)
                                snprintf(mLbl, sizeof(mLbl), "%s %s", prefix, m.name.c_str());
                        else
                                snprintf(mLbl,
                                         sizeof(mLbl),
                                         "%s  %s",
                                         m.retRaw.empty() ? ctypeLabel(m.retType) : m.retRaw.c_str(),
                                         m.name.c_str());
                        if (ImGui::Selectable(mLbl, selM) && !selM)
                                selectMethod(selectedClassIdx_, mi);
                        // Drag to SequenceEditor to create an SDK call step
                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                                SdkDragPayload p;
                                p.panelWinId = winId_;
                                p.isCFunc    = false;
                                p.classIdx   = selectedClassIdx_;
                                p.methodIdx  = mi;
                                p.objIdx     = (selectedObjIdx_ >= 0) ? selectedObjIdx_ : 0;
                                ImGui::SetDragDropPayload("SDK_CALL", &p, sizeof(p));
                                ImGui::Text("%s::%s", cls.name.c_str(), m.name.c_str());
                                ImGui::EndDragDropSource();
                        }
                }
        }
        ImGui::EndChild();
        splitterV("##clssplit", &leftW, 80.0f, 500.0f);

        ImGui::BeginChild("##methcall", ImVec2(0, 0), false);

        if (selectedClassIdx_ < 0 || selectedClassIdx_ >= (int)parseResult_.classes.size()) {
                ImGui::TextDisabled("%s", tr("← Select a class", "← 选择左侧类"));
                ImGui::EndChild();
                return;
        }

        const CClassDecl &cls = parseResult_.classes[selectedClassIdx_];
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.7f, 0.3f, 1.f));
        ImGui::Text("class %s", cls.fullName.c_str());
        ImGui::PopStyleColor();

        ImGui::Separator();

        if (selectedMethodIdx_ < 0 || selectedMethodIdx_ >= (int)cls.methods.size()) {
                ImGui::TextDisabled("%s", tr("← Select a method", "← 选择左侧方法"));
                ImGui::EndChild();
                return;
        }

        const CMethodDecl &meth = cls.methods[selectedMethodIdx_];

        // ── method signature ──
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.85f, 1.0f, 1.0f));
        if (meth.isCtor)
                ImGui::Text("[constructor] %s(...)", meth.name.c_str());
        else if (meth.isDtor)
                ImGui::Text("[destructor] %s()", meth.name.c_str());
        else
                ImGui::Text("%s  %s", meth.retRaw.empty() ? "?" : meth.retRaw.c_str(), meth.name.c_str());
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // ── parameters (+ object + return value) ──
        {
                const bool             showObjRow = !meth.isCtor && !meth.isDtor;
                const bool             showRetRow = !meth.isDtor;
                static ImGuiTableFlags tblFlags   = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
                if (ImGui::BeginTable("##mparg_tbl", 3, tblFlags)) {
                        ImGui::TableSetupColumn(tr("#", "#"), ImGuiTableColumnFlags_WidthFixed, 75.0f);
                        ImGui::TableSetupColumn(tr("Name", "名称"), ImGuiTableColumnFlags_WidthStretch, 0.35f);
                        ImGui::TableSetupColumn(tr("Value / Args", "值/参数"), ImGuiTableColumnFlags_WidthStretch, 0.65f);
                        ImGui::TableHeadersRow();

                        // ── Object (this) row ──
                        if (showObjRow) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextUnformatted(cls.name.c_str());
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextUnformatted(tr("Object", "对象"));
                                ImGui::TableSetColumnIndex(2);
                                ImGui::SetNextItemWidth(-FLT_MIN);
                                const char *objPrev = (selectedObjIdx_ >= 0 && selectedObjIdx_ < (int)objects_.size())
                                                          ? objects_[selectedObjIdx_].label
                                                          : tr("(none)", "(无)");
                                if (ImGui::BeginCombo("##obj_sel", objPrev)) {
                                        if (loader_.isLoaded() &&
                                            ImGui::Selectable(tr("[+ New Object]", "[+ 新建对象]"), false))
                                                doNewObject(selectedClassIdx_);
                                        for (int oi = 0; oi < (int)objects_.size(); ++oi) {
                                                bool sel = (oi == selectedObjIdx_);
                                                char objLbl[128];
                                                snprintf(
                                                    objLbl, sizeof(objLbl), "%s  @%p", objects_[oi].label, objects_[oi].ptr);
                                                if (ImGui::Selectable(objLbl, sel))
                                                        selectedObjIdx_ = oi;
                                                if (sel)
                                                        ImGui::SetItemDefaultFocus();
                                        }
                                        ImGui::EndCombo();
                                }
                        }

                        if (meth.params.empty() && !showObjRow && !showRetRow) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextDisabled("(%s)", tr("no parameters", "无参数"));
                        }

                        for (size_t pi = 0; pi < meth.params.size(); ++pi) {
                                const CParam &p = meth.params[pi];
                                char          rowId[64];
                                snprintf(rowId, sizeof(rowId), "##mp%zu", pi);

                                // Struct reference parameter → header row + field sub-rows.
                                if (pi < structBufs_.size() && structBufs_[pi].decl) {
                                        const CStructDecl    *sd       = structBufs_[pi].decl;
                                        std::vector<uint8_t> &buf      = structBufs_[pi].data;
                                        bool                 &expanded = structBufs_[pi].expanded;

                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0);
                                        ImGui::TextUnformatted(sd->name.c_str());
                                        ImGui::TableSetColumnIndex(1);
                                        char hdr[128];
                                        snprintf(
                                            hdr, sizeof(hdr), "%s##shdr%zu", p.name.empty() ? "param" : p.name.c_str(), pi);
                                        ImGui::SetNextItemOpen(expanded, ImGuiCond_Always);
                                        expanded = ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_SpanFullWidth);

                                        if (expanded) {
                                                drawSdkStructFieldRows(*sd, buf.data(), buf.size(), (int)pi);
                                                ImGui::TreePop();
                                        }
                                        continue;
                                }

                                // Enum parameter → dropdown row.
                                const CEnumDecl *ed = findParamEnum(p);
                                if (ed && !ed->values.empty()) {
                                        int64_t curVal = 0;
                                        if (pi < methArgBufs_.size())
                                                curVal = strtoll(methArgBufs_[pi].text, nullptr, 0);
                                        const char *preview2 = ed->values[0].name.c_str();
                                        int         curIdx   = 0;
                                        for (int ei = 0; ei < (int)ed->values.size(); ++ei) {
                                                if (ed->values[ei].value == curVal) {
                                                        preview2 = ed->values[ei].name.c_str();
                                                        curIdx   = ei;
                                                        break;
                                                }
                                        }
                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0);
                                        ImGui::TextUnformatted(ed->name.c_str());
                                        ImGui::TableSetColumnIndex(1);
                                        ImGui::TextUnformatted(p.name.empty() ? "?" : p.name.c_str());
                                        ImGui::TableSetColumnIndex(2);
                                        char cmId[64];
                                        snprintf(cmId, sizeof(cmId), "##ec%zu", pi);
                                        ImGui::SetNextItemWidth(-FLT_MIN);
                                        if (ImGui::BeginCombo(cmId, preview2)) {
                                                for (int ei = 0; ei < (int)ed->values.size(); ++ei) {
                                                        bool sel2 = (ei == curIdx);
                                                        char evLbl[128];
                                                        snprintf(evLbl,
                                                                 sizeof(evLbl),
                                                                 "%s (%lld)",
                                                                 ed->values[ei].name.c_str(),
                                                                 (long long)ed->values[ei].value);
                                                        if (ImGui::Selectable(evLbl, sel2) && pi < methArgBufs_.size()) {
                                                                snprintf(methArgBufs_[pi].text,
                                                                         sizeof(ArgBuf::text),
                                                                         "%lld",
                                                                         (long long)ed->values[ei].value);
                                                        }
                                                        if (sel2)
                                                                ImGui::SetItemDefaultFocus();
                                                }
                                                ImGui::EndCombo();
                                        }
                                        continue;
                                }

                                // Plain parameter → text input row.
                                const char *rawT2 = p.rawType.empty() ? ctypeLabel(p.type) : p.rawType.c_str();

                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextUnformatted(rawT2);
                                ImGui::TableSetColumnIndex(1);
                                if (!p.name.empty())
                                        ImGui::TextUnformatted(p.name.c_str());
                                else
                                        ImGui::Text("[%zu]", pi);
                                ImGui::TableSetColumnIndex(2);
                                if (pi < methArgBufs_.size()) {
                                        bool isCharP   = ctypeIsCharPtr(p.type, p.rawType);
                                        bool hasCreate = (bool)onCreateLocalVar_;
                                        ImGui::SetNextItemWidth(
                                            hasCreate ? ImGui::GetContentRegionAvail().x - 24.0f : -FLT_MIN);
                                        if (isCharP)
                                                ImGui::InputText(rowId, methArgBufs_[pi].text, sizeof(ArgBuf::text));
                                        else if (ctypeIsFloat(p.type))
                                                ImGui::InputText(rowId,
                                                                 methArgBufs_[pi].text,
                                                                 sizeof(ArgBuf::text),
                                                                 ImGuiInputTextFlags_CharsDecimal);
                                        else
                                                ImGui::InputText(rowId,
                                                                 methArgBufs_[pi].text,
                                                                 sizeof(ArgBuf::text),
                                                                 ImGuiInputTextFlags_CharsHexadecimal |
                                                                     ImGuiInputTextFlags_CharsDecimal);
                                        if (hasCreate) {
                                                ImGui::SameLine(0, 2);
                                                char nbId[32];
                                                snprintf(nbId, sizeof(nbId), "+##mnewp%zu", pi);
                                                if (ImGui::SmallButton(nbId)) {
                                                        std::string vn =
                                                            p.name.empty() ? ("arg" + std::to_string(pi)) : p.name;
                                                        onCreateLocalVar_(vn, p.type, findParamStruct(p));
                                                }
                                                if (ImGui::IsItemHovered())
                                                        ImGui::SetTooltip("%s",
                                                                          tr("Add this parameter as a variable",
                                                                             "把该参数添加为变量"));
                                        }
                                }
                        }

                        // ── Return value row ──
                        if (showRetRow) {
                                std::string retT = meth.retRaw.empty() ? std::string(ctypeLabel(meth.retType)) : meth.retRaw;
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextUnformatted(retT.c_str());
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextUnformatted(tr("return", "返回值"));
                                ImGui::TableSetColumnIndex(2);
                                std::vector<std::string> localVars;
                                if (onListLocalVars_)
                                        localVars = onListLocalVars_();
                                bool  hasCreate = onCreateLocalVar_ && !ctypeIsVoid(meth.retType);
                                bool  hasPick   = !localVars.empty();
                                float reserve   = (hasCreate ? 24.0f : 0.0f) + (hasPick ? 24.0f : 0.0f);
                                float rvW       = reserve > 0.0f ? ImGui::GetContentRegionAvail().x - reserve : -FLT_MIN;
                                ImGui::SetNextItemWidth(rvW);
                                ImGui::InputTextWithHint(
                                    "##methrv", tr("variable name", "变量名"), methResultVar_, sizeof(methResultVar_));
                                if (hasCreate) {
                                        ImGui::SameLine(0, 2);
                                        if (ImGui::SmallButton("+##methrvnew")) {
                                                std::string vn =
                                                    methResultVar_[0] ? std::string(methResultVar_) : meth.name;
                                                onCreateLocalVar_(vn, meth.retType, nullptr);
                                                strncpy(methResultVar_, vn.c_str(), sizeof(methResultVar_) - 1);
                                                methResultVar_[sizeof(methResultVar_) - 1] = '\0';
                                        }
                                        if (ImGui::IsItemHovered())
                                                ImGui::SetTooltip("%s", tr("Create result variable", "创建结果变量"));
                                }
                                if (hasPick) {
                                        ImGui::SameLine(0, 2);
                                        if (ImGui::SmallButton("v##methrvpick"))
                                                ImGui::OpenPopup("##methrvpop");
                                        if (ImGui::BeginPopup("##methrvpop")) {
                                                for (const auto &vn : localVars)
                                                        if (ImGui::Selectable(vn.c_str())) {
                                                                strncpy(methResultVar_, vn.c_str(), sizeof(methResultVar_) - 1);
                                                                methResultVar_[sizeof(methResultVar_) - 1] = '\0';
                                                        }
                                                ImGui::EndPopup();
                                        }
                                }
                        }

                        ImGui::EndTable();
                }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool canCall = loader_.isLoaded() && (meth.isCtor || (selectedObjIdx_ >= 0 && selectedObjIdx_ < (int)objects_.size()));
        if (!canCall)
                ImGui::BeginDisabled();
        if (ui::Button(tr("  CALL  ", "  调用  "), ui::BtnStyle::Success))
                doCallMethod();
        if (!canCall)
                ImGui::EndDisabled();

        // ── history ──
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("History", "调用历史"));
        float histH = ImGui::GetContentRegionAvail().y;
        ImGui::BeginChild("##mhist", ImVec2(0, histH), true);
        std::vector<HistEntry> histSnapM;
        {
                std::lock_guard<std::mutex> lk(histMtx_);
                histSnapM = history_;
        }
        for (int i = (int)histSnapM.size() - 1; i >= 0; --i) {
                const HistEntry &e = histSnapM[i];
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
                ImGui::PushStyleColor(ImGuiCol_Text, e.ok ? ImVec4(0.85f, 0.85f, 0.85f, 1.f) : ImVec4(1.f, 0.4f, 0.4f, 1.f));
                ImGui::TextUnformatted(e.call.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, e.ok ? ImVec4(0.3f, 1.f, 0.5f, 1.f) : ImVec4(1.f, 0.5f, 0.3f, 1.f));
                ImGui::TextUnformatted(e.result.c_str());
                ImGui::PopStyleColor();
        }
        ImGui::EndChild();

        ImGui::EndChild(); // ##methcall
}

// ─── drawPythonTab ────────────────────────────────────────────────────────────

void
SdkPanel::drawPythonTab()
{
        // ── Python file path row ──────────────────────────────────────────────
        if (ImGui::Button(tr("Browse##py", "浏览##py"))) {
                std::string p =
                    nativeDlgOpen(tr("Select Python script", "选择 Python 脚本"), {{"Python", {"py"}}, {"All Files", {"*"}}});
                if (!p.empty()) {
                        strncpy(pyPath_, p.c_str(), sizeof(pyPath_) - 1);
                        pyPath_[sizeof(pyPath_) - 1] = '\0';
                        doLoadPy();
                }
        }
        ImGui::SameLine();
        bool runnerOk = pyRunner_ && pyRunner_->isAlive();
        ImGui::TextDisabled(runnerOk ? tr("[py: online]", "[py: 在线]") : tr("[py: offline]", "[py: 离线]"));
        if (pyPath_[0]) {
                ImGui::SameLine();
                ImGui::TextDisabled("— %s", pyPath_);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (pyResult_.functions.empty() && pyResult_.classes.empty()) {
                ImGui::TextDisabled("%s", tr("Load a .py file to begin.", "加载 .py 文件开始使用。"));
                return;
        }

        if (!ImGui::BeginTabBar("##pytabs"))
                return;

        // ── Functions sub-tab ─────────────────────────────────────────────────
        if (ImGui::BeginTabItem(tr("Functions##pyf", "函数##pyf"))) {
                ImGui::Spacing();
                if (pyResult_.functions.empty()) {
                        ImGui::TextDisabled("%s", tr("No top-level functions found.", "未找到顶层函数。"));
                } else {
                        float &lw = pySplitW_;
                        ImGui::BeginChild("##pyfnlist", ImVec2(lw, 0), true);
                        ImGui::TextDisabled("%s (%d)", tr("Functions", "函数列表"), (int)pyResult_.functions.size());
                        ImGui::Separator();
                        for (int i = 0; i < (int)pyResult_.functions.size(); ++i) {
                                bool sel = (i == pySelFnIdx_);
                                if (ImGui::Selectable(pyResult_.functions[i].name.c_str(), sel) && !sel) {
                                        if (pySelFnIdx_ >= 0 && pySelFnIdx_ < (int)pyResult_.functions.size())
                                                pyFnArgBufsCache_[pySelFnIdx_] = pyFnArgBufs_;
                                        pySelFnIdx_ = i;
                                        pyLastResult_.clear();
                                        const auto &fn2 = pyResult_.functions[i];
                                        auto        cit = pyFnArgBufsCache_.find(i);
                                        if (cit != pyFnArgBufsCache_.end() && cit->second.size() == fn2.params.size()) {
                                                pyFnArgBufs_ = cit->second;
                                        } else {
                                                pyFnArgBufs_.clear();
                                                pyFnArgBufs_.resize(fn2.params.size());
                                                for (size_t j = 0; j < fn2.params.size(); ++j) {
                                                        if (!fn2.params[j].defaultVal.empty())
                                                                strncpy(pyFnArgBufs_[j].text,
                                                                        fn2.params[j].defaultVal.c_str(),
                                                                        sizeof(ArgBuf::text) - 1);
                                                }
                                        }
                                }
                                // Drag to SequenceEditor
                                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                                        SdkDragPayload p;
                                        p.panelWinId = winId_;
                                        p.isPython   = true;
                                        strncpy(p.pyName, pyResult_.functions[i].name.c_str(), sizeof(p.pyName) - 1);
                                        ImGui::SetDragDropPayload("SDK_CALL", &p, sizeof(p));
                                        ImGui::TextUnformatted(pyResult_.functions[i].name.c_str());
                                        ImGui::EndDragDropSource();
                                }
                        }
                        ImGui::EndChild();
                        splitterV("##pyfnsplit", &lw, 80.0f, 500.0f);

                        ImGui::BeginChild("##pyfncall", ImVec2(0, 0), false);
                        if (pySelFnIdx_ < 0 || pySelFnIdx_ >= (int)pyResult_.functions.size()) {
                                ImGui::TextDisabled("%s", tr("← Select a function", "← 选择左侧函数"));
                        } else {
                                const PyFuncDecl &fn = pyResult_.functions[pySelFnIdx_];
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.85f, 1.0f, 1.0f));
                                ImGui::Text("def %s(...)", fn.name.c_str());
                                ImGui::PopStyleColor();

                                if (fn.params.empty()) {
                                        ImGui::TextDisabled("  (%s)", tr("no parameters", "无参数"));
                                } else {
                                        static ImGuiTableFlags pyFTblFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                                             ImGuiTableFlags_SizingStretchProp |
                                                                             ImGuiTableFlags_Resizable;
                                        if (ImGui::BeginTable("##pyfarg_tbl", 3, pyFTblFlags)) {
                                                ImGui::TableSetupColumn(tr("#", "#"), ImGuiTableColumnFlags_WidthFixed, 28.0f);
                                                ImGui::TableSetupColumn(
                                                    tr("Name", "名称"), ImGuiTableColumnFlags_WidthStretch, 0.35f);
                                                ImGui::TableSetupColumn(
                                                    tr("Value / Args", "值/参数"), ImGuiTableColumnFlags_WidthStretch, 0.65f);
                                                ImGui::TableHeadersRow();
                                                for (size_t i = 0; i < fn.params.size(); ++i) {
                                                        char id[64];
                                                        snprintf(id, sizeof(id), "##pyfa%zu", i);
                                                        ImGui::TableNextRow();
                                                        ImGui::TableSetColumnIndex(0);
                                                        ImGui::Text("%d", (int)(i + 1));
                                                        ImGui::TableSetColumnIndex(1);
                                                        ImGui::TextUnformatted(fn.params[i].name.c_str());
                                                        ImGui::TableSetColumnIndex(2);
                                                        if (i < pyFnArgBufs_.size()) {
                                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                                ImGui::InputText(
                                                                    id, pyFnArgBufs_[i].text, sizeof(ArgBuf::text));
                                                        }
                                                }
                                                ImGui::EndTable();
                                        }
                                }

                                ImGui::Spacing();
                                ImGui::Separator();
                                ImGui::Spacing();
                                if (!runnerOk)
                                        ImGui::BeginDisabled();
                                if (ui::Button(tr("  CALL  ", "  调用  "), ui::BtnStyle::Success))
                                        doCallPyFunc();
                                if (!runnerOk) {
                                        ImGui::EndDisabled();
                                        ImGui::SameLine();
                                        ImGui::TextDisabled("%s", tr("(Python not running)", "(Python 未运行)"));
                                }
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(120.0f);
                                ImGui::InputTextWithHint(
                                    "##pyrv", tr("variable name", "变量名"), pyFnResultVar_, sizeof(pyFnResultVar_));
                                if (onListLocalVars_) {
                                        auto localVars = onListLocalVars_();
                                        if (!localVars.empty()) {
                                                ImGui::SameLine();
                                                if (ImGui::SmallButton("v##pyrvpick"))
                                                        ImGui::OpenPopup("##pyrvpop");
                                                if (ImGui::IsItemHovered())
                                                        ImGui::SetTooltip("%s", tr("Pick LOCAL variable", "选择 LOCAL 变量"));
                                                if (ImGui::BeginPopup("##pyrvpop")) {
                                                        for (const auto &vn : localVars)
                                                                if (ImGui::Selectable(vn.c_str()))
                                                                        strncpy(pyFnResultVar_,
                                                                                vn.c_str(),
                                                                                sizeof(pyFnResultVar_) - 1);
                                                        ImGui::EndPopup();
                                                }
                                        }
                                }

                                if (!pyLastResult_.empty()) {
                                        ImGui::Spacing();
                                        ImGui::Text("%s", tr("Result:", "返回值:"));
                                        ImGui::SameLine();
                                        ImGui::PushStyleColor(ImGuiCol_Text,
                                                              pyLastResultOk_ ? ImVec4(0.3f, 1.f, 0.5f, 1.f)
                                                                              : ImVec4(1.f, 0.4f, 0.4f, 1.f));
                                        ImGui::TextUnformatted(pyLastResult_.c_str());
                                        ImGui::PopStyleColor();
                                }

                                ImGui::Spacing();
                                ImGui::Separator();
                                ImGui::TextDisabled("%s", tr("History", "调用历史"));
                                ImGui::BeginChild("##pyfhist", ImVec2(0, ImGui::GetContentRegionAvail().y), true);
                                std::vector<HistEntry> snap;
                                {
                                        std::lock_guard<std::mutex> lk(histMtx_);
                                        snap = history_;
                                }
                                for (int i = (int)snap.size() - 1; i >= 0; --i) {
                                        const HistEntry &e = snap[i];
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
                                        ImGui::TextUnformatted(e.call.c_str());
                                        ImGui::PopStyleColor();
                                        ImGui::SameLine();
                                        ImGui::PushStyleColor(
                                            ImGuiCol_Text, e.ok ? ImVec4(0.3f, 1.f, 0.5f, 1.f) : ImVec4(1.f, 0.5f, 0.3f, 1.f));
                                        ImGui::TextUnformatted(e.result.c_str());
                                        ImGui::PopStyleColor();
                                }
                                ImGui::EndChild();
                        }
                        ImGui::EndChild();
                }
                ImGui::EndTabItem();
        }

        // ── Classes sub-tab ───────────────────────────────────────────────────
        if (ImGui::BeginTabItem(tr("Classes##pyc", "类##pyc"))) {
                ImGui::Spacing();
                if (pyResult_.classes.empty()) {
                        ImGui::TextDisabled("%s", tr("No classes found.", "未找到类定义。"));
                } else {
                        float &lw = pySplitW_;
                        ImGui::BeginChild("##pyclslist", ImVec2(lw, 0), true);
                        ImGui::TextDisabled("%s (%d)", tr("Classes", "类列表"), (int)pyResult_.classes.size());
                        ImGui::Separator();
                        for (int ci = 0; ci < (int)pyResult_.classes.size(); ++ci) {
                                const PyClassDecl &cls    = pyResult_.classes[ci];
                                bool               selCls = (ci == pySelClsIdx_);
                                char               lbl[128];
                                snprintf(lbl, sizeof(lbl), "%s (%d)", cls.name.c_str(), (int)cls.methods.size());
                                if (ImGui::Selectable(lbl, selCls) && !selCls) {
                                        if (pySelClsIdx_ >= 0 && pySelMethIdx_ >= 0)
                                                pyMethArgBufsCache_[pySelClsIdx_ * 1000 + pySelMethIdx_] = pyMethArgBufs_;
                                        pySelClsIdx_  = ci;
                                        pySelMethIdx_ = -1;
                                        pyMethArgBufs_.clear();
                                }
                        }
                        if (pySelClsIdx_ >= 0 && pySelClsIdx_ < (int)pyResult_.classes.size()) {
                                const PyClassDecl &cls = pyResult_.classes[pySelClsIdx_];
                                ImGui::Spacing();
                                ImGui::Separator();
                                ImGui::TextDisabled("%s (%d)", tr("Methods", "方法列表"), (int)cls.methods.size());
                                ImGui::Separator();
                                for (int mi = 0; mi < (int)cls.methods.size(); ++mi) {
                                        bool selM = (mi == pySelMethIdx_);
                                        char mlbl[256];
                                        snprintf(mlbl, sizeof(mlbl), "def %s", cls.methods[mi].name.c_str());
                                        if (ImGui::Selectable(mlbl, selM) && !selM) {
                                                if (pySelMethIdx_ >= 0 && pySelClsIdx_ >= 0)
                                                        pyMethArgBufsCache_[pySelClsIdx_ * 1000 + pySelMethIdx_] =
                                                            pyMethArgBufs_;
                                                pySelMethIdx_     = mi;
                                                const auto &meth2 = cls.methods[mi];
                                                int         mkey  = pySelClsIdx_ * 1000 + mi;
                                                auto        mcit  = pyMethArgBufsCache_.find(mkey);
                                                if (mcit != pyMethArgBufsCache_.end() &&
                                                    mcit->second.size() == meth2.params.size()) {
                                                        pyMethArgBufs_ = mcit->second;
                                                } else {
                                                        pyMethArgBufs_.clear();
                                                        pyMethArgBufs_.resize(meth2.params.size());
                                                        for (size_t j = 0; j < meth2.params.size(); ++j) {
                                                                if (!meth2.params[j].defaultVal.empty())
                                                                        strncpy(pyMethArgBufs_[j].text,
                                                                                meth2.params[j].defaultVal.c_str(),
                                                                                sizeof(ArgBuf::text) - 1);
                                                        }
                                                }
                                        }
                                }
                        }
                        ImGui::EndChild();
                        splitterV("##pyclssplit", &lw, 80.0f, 500.0f);

                        ImGui::BeginChild("##pyclscall", ImVec2(0, 0), false);
                        if (pySelClsIdx_ < 0 || pySelClsIdx_ >= (int)pyResult_.classes.size()) {
                                ImGui::TextDisabled("%s", tr("← Select a class", "← 选择左侧类"));
                                ImGui::EndChild();
                                ImGui::EndTabItem();
                                ImGui::EndTabBar();
                                return;
                        }
                        const PyClassDecl &cls = pyResult_.classes[pySelClsIdx_];
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.7f, 0.3f, 1.f));
                        ImGui::Text("class %s", cls.name.c_str());
                        ImGui::PopStyleColor();

                        // Object manager
                        ImGui::Spacing();
                        ImGui::Text("%s:", tr("Objects", "对象管理"));
                        ImGui::SameLine();
                        if (!runnerOk)
                                ImGui::BeginDisabled();
                        if (ImGui::Button(tr("+ New", "+ 新建")))
                                doNewPyObject();
                        if (!runnerOk)
                                ImGui::EndDisabled();
                        if (!pyObjects_.empty()) {
                                ImGui::SameLine();
                                bool canDel = (pySelObjIdx_ >= 0 && pySelObjIdx_ < (int)pyObjects_.size());
                                if (!canDel)
                                        ImGui::BeginDisabled();
                                if (ImGui::Button(tr("Delete", "删除")))
                                        doDeletePyObject(pySelObjIdx_);
                                if (!canDel)
                                        ImGui::EndDisabled();

                                ImGui::BeginChild("##pyobjlist", ImVec2(0, 60.0f), true);
                                for (int oi = 0; oi < (int)pyObjects_.size(); ++oi) {
                                        bool sel = (oi == pySelObjIdx_);
                                        char olbl[128];
                                        snprintf(olbl,
                                                 sizeof(olbl),
                                                 "%s  [id=%d]",
                                                 pyObjects_[oi].label.c_str(),
                                                 pyObjects_[oi].pyId);
                                        if (ImGui::Selectable(olbl, sel))
                                                pySelObjIdx_ = oi;
                                }
                                ImGui::EndChild();
                        }
                        ImGui::Separator();

                        if (pySelMethIdx_ < 0 || pySelMethIdx_ >= (int)cls.methods.size()) {
                                ImGui::TextDisabled("%s", tr("← Select a method", "← 选择左侧方法"));
                        } else {
                                const PyFuncDecl &meth   = cls.methods[pySelMethIdx_];
                                bool              isInit = (meth.name == "__init__");
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.85f, 1.0f, 1.0f));
                                ImGui::Text("def %s.%s(...)", cls.name.c_str(), meth.name.c_str());
                                ImGui::PopStyleColor();

                                if (meth.params.empty()) {
                                        ImGui::TextDisabled("  (%s)", tr("no parameters", "无参数"));
                                } else {
                                        static ImGuiTableFlags pyMTblFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                                             ImGuiTableFlags_SizingStretchProp |
                                                                             ImGuiTableFlags_Resizable;
                                        if (ImGui::BeginTable("##pymarg_tbl", 3, pyMTblFlags)) {
                                                ImGui::TableSetupColumn(tr("#", "#"), ImGuiTableColumnFlags_WidthFixed, 28.0f);
                                                ImGui::TableSetupColumn(
                                                    tr("Name", "名称"), ImGuiTableColumnFlags_WidthStretch, 0.35f);
                                                ImGui::TableSetupColumn(
                                                    tr("Value / Args", "值/参数"), ImGuiTableColumnFlags_WidthStretch, 0.65f);
                                                ImGui::TableHeadersRow();
                                                for (size_t i = 0; i < meth.params.size(); ++i) {
                                                        char id[64];
                                                        snprintf(id, sizeof(id), "##pyma%zu", i);
                                                        ImGui::TableNextRow();
                                                        ImGui::TableSetColumnIndex(0);
                                                        ImGui::Text("%d", (int)(i + 1));
                                                        ImGui::TableSetColumnIndex(1);
                                                        ImGui::TextUnformatted(meth.params[i].name.c_str());
                                                        ImGui::TableSetColumnIndex(2);
                                                        if (i < pyMethArgBufs_.size()) {
                                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                                ImGui::InputText(
                                                                    id, pyMethArgBufs_[i].text, sizeof(ArgBuf::text));
                                                        }
                                                }
                                                ImGui::EndTable();
                                        }
                                }

                                ImGui::Spacing();
                                ImGui::Separator();
                                ImGui::Spacing();
                                bool canCall = runnerOk && (isInit || (!pyObjects_.empty() && pySelObjIdx_ >= 0));
                                if (!canCall)
                                        ImGui::BeginDisabled();
                                if (ui::Button(isInit ? tr("  CREATE  ", "  创建  ") : tr("  CALL  ", "  调用  "),
                                               ui::BtnStyle::Success))
                                        doCallPyMeth();
                                if (!canCall)
                                        ImGui::EndDisabled();
                                if (!canCall && runnerOk && !isInit && pyObjects_.empty()) {
                                        ImGui::SameLine();
                                        ImGui::TextDisabled("%s", tr("(create an object first)", "(请先创建对象)"));
                                }

                                ImGui::Spacing();
                                ImGui::Separator();
                                ImGui::TextDisabled("%s", tr("History", "调用历史"));
                                ImGui::BeginChild("##pymhist", ImVec2(0, ImGui::GetContentRegionAvail().y), true);
                                std::vector<HistEntry> snap;
                                {
                                        std::lock_guard<std::mutex> lk(histMtx_);
                                        snap = history_;
                                }
                                for (int i = (int)snap.size() - 1; i >= 0; --i) {
                                        const HistEntry &e = snap[i];
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
                                        ImGui::TextUnformatted(e.call.c_str());
                                        ImGui::PopStyleColor();
                                        ImGui::SameLine();
                                        ImGui::PushStyleColor(
                                            ImGuiCol_Text, e.ok ? ImVec4(0.3f, 1.f, 0.5f, 1.f) : ImVec4(1.f, 0.5f, 0.3f, 1.f));
                                        ImGui::TextUnformatted(e.result.c_str());
                                        ImGui::PopStyleColor();
                                }
                                ImGui::EndChild();
                        }
                        ImGui::EndChild();
                }
                ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
}

// ─── draw ──────────────────────────────────────────────────────────────────────

void
SdkPanel::draw()
{
        if (!open_)
                return;

        ImGui::SetNextWindowSize(ImVec2(980, 620), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(titleBuf_, &open_)) {
                ImGui::End();
                return;
        }

        // Double-click the title bar (or dock tab) to rename this window.
        {
                ImGuiWindow *win       = ImGui::GetCurrentWindow();
                ImRect       titleRect = win->DockIsActive ? win->DC.DockTabItemRect : win->TitleBarRect();
                if (ImGui::IsMouseHoveringRect(titleRect.Min, titleRect.Max, false) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        snprintf(renameBuf_, sizeof(renameBuf_), "%s", userLabel_);
                        ImGui::OpenPopup("Rename SDK");
                }
                if (ImGui::BeginPopup("Rename SDK")) {
                        ImGui::TextDisabled("%s", tr("Rename", "重命名"));
                        ImGui::SetNextItemWidth(220.0f);
                        if (ImGui::IsWindowAppearing())
                                ImGui::SetKeyboardFocusHere();
                        bool commit =
                            ImGui::InputText("##renameSdk",
                                             renameBuf_,
                                             sizeof(renameBuf_),
                                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                        ImGui::SameLine();
                        if (ImGui::Button("OK"))
                                commit = true;
                        if (commit) {
                                if (renameBuf_[0] != '\0') {
                                        strncpy(userLabel_, renameBuf_, sizeof(userLabel_) - 1);
                                        userLabel_[sizeof(userLabel_) - 1] = '\0';
                                        snprintf(titleBuf_, sizeof(titleBuf_), "%s###SdkPanel%d", userLabel_, winId_);
                                }
                                ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                }
        }

        // Apply OS-level file drops if this window is hovered.
        if (!pendingDropFiles_.empty()) {
                if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_RootWindow)) {
                        for (const auto &f : pendingDropFiles_) {
                                if (f.size() > 4 && f.substr(f.size() - 4) == ".dll") {
                                        strncpy(dllPath_, f.c_str(), sizeof(dllPath_) - 1);
                                        dllPath_[sizeof(dllPath_) - 1] = '\0';
                                        doLoadDll();
                                } else if ((f.size() > 2 && f.substr(f.size() - 2) == ".h") ||
                                           (f.size() > 4 && f.substr(f.size() - 4) == ".hpp")) {
                                        strncpy(headerPath_, f.c_str(), sizeof(headerPath_) - 1);
                                        headerPath_[sizeof(headerPath_) - 1] = '\0';
                                        doParseHeader();
                                } else if (f.size() > 3 && f.substr(f.size() - 3) == ".py") {
                                        strncpy(pyPath_, f.c_str(), sizeof(pyPath_) - 1);
                                        pyPath_[sizeof(pyPath_) - 1] = '\0';
                                        doLoadPy();
                                }
                        }
                }
                pendingDropFiles_.clear();
        }

        // ── Status bar ──
        if (!statusMsg_.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      statusIsErr_ ? ImVec4(1.f, 0.4f, 0.4f, 1.f) : ImVec4(0.4f, 0.9f, 0.4f, 1.f));
                ImGui::TextUnformatted(statusMsg_.c_str());
                ImGui::PopStyleColor();
        } else {
                ImGui::NewLine();
        }
        ImGui::Separator();

        // ── Tabs ──
        if (ImGui::BeginTabBar("##sdktabs")) {
                // ── C/C++ tab ────────────────────────────────────────────────
                if (ImGui::BeginTabItem(tr("C/C++", "C/C++"))) {
                        ImGui::Spacing();

                        // DLL browse row
                        if (ImGui::Button(tr("Browse DLL##dll", "浏览动态库##dll"))) {
                                std::string p = nativeDlgOpen(tr("Select library", "选择动态库"),
#if defined(_WIN32)
                                                              {{"DLL", {"dll"}}, {"All Files", {"*"}}});
#elif defined(__APPLE__)
                                                              {{"dylib", {"dylib"}}, {"All Files", {"*"}}});
#else
                                                              {{"Shared Library", {"so"}}, {"All Files", {"*"}}});
#endif
                                if (!p.empty()) {
                                        strncpy(dllPath_, p.c_str(), sizeof(dllPath_) - 1);
                                        doLoadDll();
                                }
                        }
                        if (loader_.isLoaded()) {
                                ImGui::SameLine();
                                if (ImGui::Button(tr("View Symbols##sym", "查看符号##sym")))
                                        showSymbolsWindow_ = true;
                                ImGui::SameLine();
                                if (ui::Button(tr("Unload", "卸载"), ui::BtnStyle::Danger)) {
                                        loader_.unload();
                                        setStatus(tr("Unloaded", "已卸载"), false);
                                }
                        }
                        if (dllPath_[0]) {
                                ImGui::SameLine();
                                ImGui::TextDisabled("— %s", dllPath_);
                        }

                        // Header browse row
                        if (ImGui::Button(tr("Browse Header##hdr", "浏览头文件##hdr"))) {
                                std::string p = nativeDlgOpen(tr("Select header file", "选择头文件"),
                                                              {{"C/C++ Header", {"h", "hpp"}}, {"All Files", {"*"}}});
                                if (!p.empty()) {
                                        strncpy(headerPath_, p.c_str(), sizeof(headerPath_) - 1);
                                        doParseHeader();
                                }
                        }
                        if (headerPath_[0]) {
                                ImGui::SameLine();
                                ImGui::TextDisabled("— %s", headerPath_);
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        bool hasFn  = !parseResult_.functions.empty();
                        bool hasCls = !parseResult_.classes.empty();
                        if (!hasFn && !hasCls) {
                                ImGui::TextDisabled(
                                    "%s", tr("Browse a DLL and header file to begin.", "浏览动态库和头文件以开始使用。"));
                        } else if (ImGui::BeginTabBar("##ccpptabs")) {
                                if (hasFn && ImGui::BeginTabItem(tr("Functions", "C 函数"))) {
                                        drawCFunctionsTab();
                                        ImGui::EndTabItem();
                                }
                                if (hasCls && ImGui::BeginTabItem(tr("Classes", "C++ 类"))) {
                                        drawCppClassesTab();
                                        ImGui::EndTabItem();
                                }
                                ImGui::EndTabBar();
                        }

                        ImGui::EndTabItem();
                }
                // ── Python tab ───────────────────────────────────────────────
                if (ImGui::BeginTabItem(tr("Python", "Python"))) {
                        ImGui::Spacing();
                        drawPythonTab();
                        ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
        }

        ImGui::End();

        // Symbols viewer (separate dockable window).
        drawSymbolsWindow();
}

// ─── drawSymbolsWindow ───────────────────────────────────────────────────────

void
SdkPanel::drawSymbolsWindow()
{
        if (!showSymbolsWindow_)
                return;

        char winTitle[96];
        snprintf(winTitle, sizeof(winTitle), "%s###SdkSymbols%d", tr("SDK Symbols", "SDK 符号"), winId_);
        ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(winTitle, &showSymbolsWindow_)) {
                ImGui::End();
                return;
        }

        const auto &syms = loader_.exports();

        ImGui::Text("%s: %d", tr("Total symbols", "符号总数"), (int)syms.size());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##symfilter", tr("Filter symbols…", "过滤符号…"), symbolFilter_, sizeof(symbolFilter_));

        // Case-insensitive substring match.
        auto containsCI = [](const std::string &hay, const char *needle) -> bool {
                if (!needle || !needle[0])
                        return true;
                std::string h = hay, n = needle;
                std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return (char)tolower(c); });
                std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)tolower(c); });
                return h.find(n) != std::string::npos;
        };

        static ImGuiTableFlags tblFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##symtbl", 3, tblFlags)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn(tr("#", "#"), ImGuiTableColumnFlags_WidthFixed, 50.0f);
                ImGui::TableSetupColumn(tr("Demangled", "解析名称"), ImGuiTableColumnFlags_WidthStretch, 0.60f);
                ImGui::TableSetupColumn(tr("Mangled", "符号名"), ImGuiTableColumnFlags_WidthStretch, 0.40f);
                ImGui::TableHeadersRow();

                int shown = 0;
                for (int i = 0; i < (int)syms.size(); ++i) {
                        const ExportedSymbol &s = syms[i];
                        if (!containsCI(s.demangled, symbolFilter_) && !containsCI(s.mangled, symbolFilter_))
                                continue;
                        ++shown;
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%d", i);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(s.demangled.c_str());
                        // Click a row's demangled name to copy it to the clipboard.
                        if (ImGui::IsItemClicked())
                                ImGui::SetClipboardText(s.demangled.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextDisabled("%s", s.mangled.c_str());
                        if (ImGui::IsItemClicked())
                                ImGui::SetClipboardText(s.mangled.c_str());
                }
                ImGui::EndTable();

                if (symbolFilter_[0])
                        ImGui::TextDisabled("%s: %d", tr("Matched", "匹配"), shown);
        }

        ImGui::End();
}

// ─── pushHistory ─────────────────────────────────────────────────────────────

void
SdkPanel::pushHistory(const std::string &call, const std::string &result, bool ok, double tsSec)
{
        HistEntry e;
        e.call   = call;
        e.result = result;
        e.ok     = ok;
        e.tsMs =
            (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        e.tsSec = tsSec;
        std::lock_guard<std::mutex> lk(histMtx_);
        if (history_.size() >= 400)
                history_.erase(history_.begin());
        history_.push_back(std::move(e));
}

// ─── writeResultVar ──────────────────────────────────────────────────────────

void
SdkPanel::writeResultVar(const std::string &name, const CallResult &res, CType retType)
{
        if (name.empty() || !onWriteLocalScalar_)
                return;
        if (ctypeIsInteger(retType))
                onWriteLocalScalar_(name, (double)(int64_t)res.rawU64, false);
        else if (ctypeIsFloat(retType))
                onWriteLocalScalar_(name, res.rawF64, true);
}

// ─── evalCond ────────────────────────────────────────────────────────────────

bool
SdkPanel::evalCond(const SdkSeqStep &step, const std::unordered_map<std::string, int64_t> &vars)
{
        int64_t lhs = 0;
        {
                auto it = vars.find(step.condVar);
                if (it != vars.end())
                        lhs = it->second;
        }
        int64_t rhs = step.condVal;
        if (strcmp(step.condOp, "==") == 0)
                return lhs == rhs;
        else if (strcmp(step.condOp, "!=") == 0)
                return lhs != rhs;
        else if (strcmp(step.condOp, "<") == 0)
                return lhs < rhs;
        else if (strcmp(step.condOp, ">") == 0)
                return lhs > rhs;
        else if (strcmp(step.condOp, "<=") == 0)
                return lhs <= rhs;
        else if (strcmp(step.condOp, ">=") == 0)
                return lhs >= rhs;
        return false;
}

// ─── execStep / execSteps ─────────────────────────────────────────────────────

SdkPanel::StepResult
SdkPanel::execStep(const SdkSeqStep &step, SeqCtx &ctx)
{
        if (ctx.stopReq)
                return StepResult::Stop;

        switch (step.kind) {

                case SdkStepKind::Call: {
                        // Collect args from step.
                        std::vector<std::string> args = step.args;
                        // Expand $var references in args.
                        for (auto &a : args) {
                                if (!a.empty() && a[0] == '$') {
                                        auto it = ctx.vars.find(a.substr(1));
                                        if (it != ctx.vars.end())
                                                a = std::to_string(it->second);
                                }
                        }
                        CallResult  res;
                        double      ts = sessionTimeSec();
                        std::string callStr;
                        if (step.isCFunc) {
                                if (step.classIdx >= 0 && step.classIdx < (int)parseResult_.functions.size()) {
                                        const CFuncDecl &fn = parseResult_.functions[step.classIdx];
                                        args.resize(fn.params.size());
                                        res     = loader_.call(fn, args);
                                        callStr = fn.name + "(";
                                        for (size_t i = 0; i < args.size(); ++i) {
                                                if (i)
                                                        callStr += ",";
                                                callStr += args[i];
                                        }
                                        callStr += ")";
                                }
                        } else {
                                if (step.classIdx >= 0 && step.classIdx < (int)parseResult_.classes.size()) {
                                        const CClassDecl &cls = parseResult_.classes[step.classIdx];
                                        if (step.methodIdx >= 0 && step.methodIdx < (int)cls.methods.size()) {
                                                const CMethodDecl &meth    = cls.methods[step.methodIdx];
                                                void              *thisPtr = nullptr;
                                                if (!meth.isCtor && step.objIdx >= 0 && step.objIdx < (int)objects_.size())
                                                        thisPtr = objects_[step.objIdx].ptr;
                                                args.resize(meth.params.size());
                                                // Build pointer-arg table: &varname args → Variable LOCAL buffers.
                                                std::vector<void *>      structPtrs(args.size(), nullptr);
                                                std::vector<std::string> ptrVarNames(args.size());
                                                for (size_t i = 0; i < args.size(); ++i) {
                                                        if (!args[i].empty() && args[i][0] == '&' && onGetVarBuf_) {
                                                                std::string vn  = args[i].substr(1);
                                                                void       *buf = onGetVarBuf_(vn);
                                                                if (buf) {
                                                                        structPtrs[i]  = buf;
                                                                        ptrVarNames[i] = vn;
                                                                        args[i]        = "&" + vn; // keep display text
                                                                }
                                                        }
                                                }
                                                res = loader_.callMethod(thisPtr, meth, args, structPtrs);
                                                // Notify Variable to refresh display for each pointer output.
                                                if (res.ok && onVarWritten_) {
                                                        for (const auto &vn : ptrVarNames)
                                                                if (!vn.empty())
                                                                        onVarWritten_(vn);
                                                }
                                                callStr = cls.name + "::" + meth.name + "(";
                                                for (size_t i = 0; i < args.size(); ++i) {
                                                        if (i)
                                                                callStr += ",";
                                                        callStr += args[i];
                                                }
                                                callStr += ")";
                                        }
                                }
                        }
                        std::string display = res.ok ? res.display : res.error;
                        pushHistory(callStr, display, res.ok, ts);

                        // Store result in variable.
                        if (step.resultVar[0] != '\0') {
                                int64_t v = 0;
                                if (res.ok)
                                        memcpy(&v, &res.rawU64, sizeof(int64_t));
                                ctx.vars[step.resultVar] = v;
                        }

                        // Append to seq log.
                        {
                                std::lock_guard<std::mutex> lk(seqMtx_);
                                seqLog_.push_back(callStr + " → " + display);
                                if (seqLog_.size() > 200)
                                        seqLog_.erase(seqLog_.begin());
                        }
                        break;
                }

                case SdkStepKind::Sleep: {
                        int remaining = step.sleepMs;
                        while (remaining > 0 && !ctx.stopReq) {
                                int chunk = std::min(remaining, 50);
                                std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
                                remaining -= chunk;
                        }
                        break;
                }

                case SdkStepKind::If: {
                        if (evalCond(step, ctx.vars))
                                return execSteps(step.body, ctx);
                        else if (step.hasElse)
                                return execSteps(step.elseBody, ctx);
                        break;
                }

                case SdkStepKind::While: {
                        while (!ctx.stopReq && evalCond(step, ctx.vars)) {
                                StepResult r = execSteps(step.body, ctx);
                                if (r == StepResult::Stop)
                                        return StepResult::Stop;
                                if (r == StepResult::Break)
                                        break;
                        }
                        break;
                }

                case SdkStepKind::For: {
                        int64_t &v = ctx.vars[step.forVar];
                        v          = step.forFrom;
                        while (!ctx.stopReq) {
                                if (step.forStep > 0 && v >= step.forTo)
                                        break;
                                if (step.forStep < 0 && v <= step.forTo)
                                        break;
                                StepResult r = execSteps(step.body, ctx);
                                if (r == StepResult::Stop)
                                        return StepResult::Stop;
                                if (r == StepResult::Break)
                                        break;
                                v += step.forStep;
                        }
                        break;
                }

                case SdkStepKind::Break:
                        return StepResult::Break;

                case SdkStepKind::Print: {
                        std::string msg = step.message;
                        // Expand $var references.
                        for (auto &[k, v] : ctx.vars) {
                                std::string token = "$" + k;
                                size_t      pos   = 0;
                                while ((pos = msg.find(token, pos)) != std::string::npos) {
                                        msg.replace(pos, token.size(), std::to_string(v));
                                        pos += std::to_string(v).size();
                                }
                        }
                        std::lock_guard<std::mutex> lk(seqMtx_);
                        seqLog_.push_back("[Print] " + msg);
                        if (seqLog_.size() > 200)
                                seqLog_.erase(seqLog_.begin());
                        break;
                }

                default:
                        break;
        }
        return StepResult::Ok;
}

SdkPanel::StepResult
SdkPanel::execSteps(const std::vector<SdkSeqStep> &steps, SeqCtx &ctx)
{
        for (const auto &step : steps) {
                if (ctx.stopReq)
                        return StepResult::Stop;
                StepResult r = execStep(step, ctx);
                if (r != StepResult::Ok)
                        return r;
        }
        return StepResult::Ok;
}

// ─── drawStepEditor ──────────────────────────────────────────────────────────

void
SdkPanel::drawStepEditor(SdkSeqStep &step, int /*depth*/)
{

        // Kind selector
        const char *kinds[] = {"Call", "Sleep", "If", "While", "For", "Break", "Print"};
        int         kindIdx = (int)step.kind;
        ImGui::SetNextItemWidth(90.0f);
        char kindId[32];
        snprintf(kindId, sizeof(kindId), "##k%p", &step);
        if (ImGui::Combo(kindId, &kindIdx, kinds, 7))
                step.kind = (SdkStepKind)kindIdx;
        ImGui::SameLine();

        switch (step.kind) {

                case SdkStepKind::Call: {
                        // Class selector
                        if (!parseResult_.classes.empty()) {
                                const char *clsPrev = (step.classIdx >= 0 && step.classIdx < (int)parseResult_.classes.size())
                                                          ? parseResult_.classes[step.classIdx].name.c_str()
                                                          : "---";
                                char        clsId[32];
                                snprintf(clsId, sizeof(clsId), "##cls%p", &step);
                                ImGui::SetNextItemWidth(100.0f);
                                if (ImGui::BeginCombo(clsId, clsPrev)) {
                                        for (int ci = 0; ci < (int)parseResult_.classes.size(); ++ci) {
                                                bool sel = (ci == step.classIdx);
                                                if (ImGui::Selectable(parseResult_.classes[ci].name.c_str(), sel)) {
                                                        step.classIdx  = ci;
                                                        step.methodIdx = 0;
                                                }
                                                if (sel)
                                                        ImGui::SetItemDefaultFocus();
                                        }
                                        ImGui::EndCombo();
                                }
                                ImGui::SameLine();
                        }
                        // Method selector
                        if (step.classIdx >= 0 && step.classIdx < (int)parseResult_.classes.size()) {
                                const CClassDecl &cls      = parseResult_.classes[step.classIdx];
                                const char       *methPrev = (step.methodIdx >= 0 && step.methodIdx < (int)cls.methods.size())
                                                                 ? cls.methods[step.methodIdx].name.c_str()
                                                                 : "---";
                                char              mId[32];
                                snprintf(mId, sizeof(mId), "##m%p", &step);
                                ImGui::SetNextItemWidth(120.0f);
                                if (ImGui::BeginCombo(mId, methPrev)) {
                                        for (int mi = 0; mi < (int)cls.methods.size(); ++mi) {
                                                bool sel = (mi == step.methodIdx);
                                                if (ImGui::Selectable(cls.methods[mi].name.c_str(), sel))
                                                        step.methodIdx = mi;
                                                if (sel)
                                                        ImGui::SetItemDefaultFocus();
                                        }
                                        ImGui::EndCombo();
                                }
                                ImGui::SameLine();
                                // Args: ensure correct size
                                int nparams = (step.methodIdx >= 0 && step.methodIdx < (int)cls.methods.size())
                                                  ? (int)cls.methods[step.methodIdx].params.size()
                                                  : 0;
                                step.args.resize(nparams);
                                for (int ai = 0; ai < nparams; ++ai) {
                                        const CMethodDecl &cm = cls.methods[step.methodIdx];
                                        // Detect pointer (T*) and reference (T& / T&&) params — both are passed as address.
                                        bool isPtr = (ai < (int)cm.params.size()) &&
                                                     (!cm.params[ai].rawType.empty() && (cm.params[ai].rawType.back() == '*' ||
                                                                                         cm.params[ai].rawType.back() == '&'));
                                        char argId[32];
                                        snprintf(argId, sizeof(argId), "##a%p_%d", &step, ai);
                                        ImGui::SetNextItemWidth(isPtr ? 100.0f : 80.0f);
                                        if (isPtr)
                                                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.25f, 0.4f, 1.f));
                                        char argBuf[512];
                                        strncpy(argBuf, step.args[ai].c_str(), 511);
                                        argBuf[511] = '\0';
                                        if (ImGui::InputTextWithHint(argId, isPtr ? "&varname" : "value", argBuf, 512))
                                                step.args[ai] = argBuf;
                                        if (isPtr) {
                                                ImGui::PopStyleColor();
                                                if (ImGui::IsItemHovered())
                                                        ImGui::SetTooltip("%s",
                                                                          tr("Pointer/reference param — type &varname to pass\n"
                                                                             "a LOCAL Variable buffer (Variable manager).\n"
                                                                             "Or enter a plain value to pass directly.",
                                                                             "指针/引用参数 — 输入 &varname 传入变量管理器的\n"
                                                                             "LOCAL 变量缓冲区，或直接输入数值。"));
                                                // Variable picker: shows all LOCAL variable names as a dropdown.
                                                if (onListLocalVars_) {
                                                        char popId[56];
                                                        snprintf(popId, sizeof(popId), "##vp%p_%d", &step, ai);
                                                        char btnId[56];
                                                        snprintf(btnId, sizeof(btnId), "v##vb%p_%d", &step, ai);
                                                        ImGui::SameLine(0, 2);
                                                        if (ImGui::SmallButton(btnId))
                                                                ImGui::OpenPopup(popId);
                                                        if (ImGui::IsItemHovered())
                                                                ImGui::SetTooltip("%s",
                                                                                  tr("Pick LOCAL variable", "选择 LOCAL 变量"));
                                                        if (ImGui::BeginPopup(popId)) {
                                                                auto localVars = onListLocalVars_();
                                                                if (localVars.empty())
                                                                        ImGui::TextDisabled(
                                                                            "%s", tr("No LOCAL variables", "暂无 LOCAL 变量"));
                                                                else
                                                                        for (const auto &vn : localVars)
                                                                                if (ImGui::Selectable(vn.c_str()))
                                                                                        step.args[ai] = "&" + vn;
                                                                ImGui::EndPopup();
                                                        }
                                                }
                                        }
                                        ImGui::SameLine();
                                }
                        }
                        // Result var
                        ImGui::SetNextItemWidth(80.0f);
                        char rvId[32];
                        snprintf(rvId, sizeof(rvId), "##rv%p", &step);
                        ImGui::InputText(rvId, step.resultVar, sizeof(step.resultVar));
                        break;
                }

                case SdkStepKind::Sleep: {
                        char sid[32];
                        snprintf(sid, sizeof(sid), "##sl%p", &step);
                        ImGui::SetNextItemWidth(80.0f);
                        ImGui::InputInt(sid, &step.sleepMs);
                        ImGui::SameLine();
                        ImGui::TextDisabled("ms");
                        break;
                }

                case SdkStepKind::If:
                case SdkStepKind::While: {
                        char vid[32];
                        snprintf(vid, sizeof(vid), "##cv%p", &step);
                        ImGui::SetNextItemWidth(70.0f);
                        ImGui::InputText(vid, step.condVar, sizeof(step.condVar));
                        ImGui::SameLine();
                        const char *ops[] = {"==", "!=", "<", ">", "<=", ">="};
                        int         opIdx = 0;
                        for (int oi = 0; oi < 6; ++oi)
                                if (strcmp(step.condOp, ops[oi]) == 0) {
                                        opIdx = oi;
                                        break;
                                }
                        char opId[32];
                        snprintf(opId, sizeof(opId), "##op%p", &step);
                        ImGui::SetNextItemWidth(50.0f);
                        if (ImGui::Combo(opId, &opIdx, ops, 6))
                                strncpy(step.condOp, ops[opIdx], 3);
                        ImGui::SameLine();
                        char vId[32];
                        snprintf(vId, sizeof(vId), "##cval%p", &step);
                        ImGui::SetNextItemWidth(70.0f);
                        int iv = (int)step.condVal;
                        if (ImGui::InputInt(vId, &iv))
                                step.condVal = iv;
                        if (step.kind == SdkStepKind::If) {
                                ImGui::SameLine();
                                char cbId[32];
                                snprintf(cbId, sizeof(cbId), "##else%p", &step);
                                ImGui::Checkbox(tr("else", "否则"), &step.hasElse);
                        }
                        break;
                }

                case SdkStepKind::For: {
                        char fvId[32];
                        snprintf(fvId, sizeof(fvId), "##fv%p", &step);
                        ImGui::SetNextItemWidth(60.0f);
                        ImGui::InputText(fvId, step.forVar, sizeof(step.forVar));
                        ImGui::SameLine();
                        ImGui::TextDisabled("=");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(55.0f);
                        char ffId[32];
                        snprintf(ffId, sizeof(ffId), "##ff%p", &step);
                        int fi = (int)step.forFrom;
                        if (ImGui::InputInt(ffId, &fi))
                                step.forFrom = fi;
                        ImGui::SameLine();
                        ImGui::TextDisabled(";..<");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(55.0f);
                        char ftId[32];
                        snprintf(ftId, sizeof(ftId), "##ft%p", &step);
                        int ft = (int)step.forTo;
                        if (ImGui::InputInt(ftId, &ft))
                                step.forTo = ft;
                        ImGui::SameLine();
                        ImGui::TextDisabled(";+=");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(45.0f);
                        char fsId[32];
                        snprintf(fsId, sizeof(fsId), "##fs%p", &step);
                        int fs = (int)step.forStep;
                        if (ImGui::InputInt(fsId, &fs))
                                step.forStep = fs ? fs : 1;
                        break;
                }

                case SdkStepKind::Print: {
                        char msgId[32];
                        snprintf(msgId, sizeof(msgId), "##msg%p", &step);
                        ImGui::SetNextItemWidth(300.0f);
                        ImGui::InputText(msgId, step.message, sizeof(step.message));
                        break;
                }

                default:
                        break;
        }
}

// ─── drawStepList ─────────────────────────────────────────────────────────────

void
SdkPanel::drawStepList(std::vector<SdkSeqStep> &steps, int depth, std::vector<SdkSeqStep> **insertParent, int *insertAfter)
{
        for (int si = 0; si < (int)steps.size(); ++si) {
                SdkSeqStep &step = steps[si];

                // Row: expand arrow + kind badge + editor inline
                ImGui::PushID(&step);
                const char  *kinds[]      = {"CALL", "SLEEP", "IF", "WHILE", "FOR", "BREAK", "PRINT"};
                const ImVec4 kindColors[] = {{0.3f, 0.8f, 1.0f, 1.f},
                                             {0.8f, 0.8f, 0.3f, 1.f},
                                             {1.f, 0.6f, 0.3f, 1.f},
                                             {1.f, 0.4f, 0.8f, 1.f},
                                             {0.4f, 1.f, 0.6f, 1.f},
                                             {1.f, 0.3f, 0.3f, 1.f},
                                             {0.7f, 0.7f, 0.7f, 1.f}};
                int          ki           = (int)step.kind;

                // Delete button
                const float rowIndent = float(depth) * 16.0f;
                if (rowIndent > 0.0f)
                        ImGui::Indent(rowIndent);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.2f, 0.2f, 1.f));
                bool del = ImGui::SmallButton("✕");
                ImGui::PopStyleColor();
                ImGui::SameLine();

                // Toggle expand for compound steps
                bool hasBody =
                    (step.kind == SdkStepKind::If || step.kind == SdkStepKind::While || step.kind == SdkStepKind::For);
                if (hasBody) {
                        if (ImGui::SmallButton(step.expanded ? "▼" : "▶"))
                                step.expanded = !step.expanded;
                        ImGui::SameLine();
                }

                // Kind badge
                ImGui::PushStyleColor(ImGuiCol_Text, kindColors[ki]);
                ImGui::TextUnformatted(kinds[ki]);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (rowIndent > 0.0f)
                        ImGui::Unindent(rowIndent);

                // Inline editor (cursor is already positioned right after the badge via SameLine)
                drawStepEditor(step, 0);

                // Add-after button
                ImGui::Indent(float(depth) * 16.0f + 20.0f);
                char addId[32];
                snprintf(addId, sizeof(addId), "+##add%d", si);
                if (ImGui::SmallButton(addId)) {
                        *insertParent = &steps;
                        *insertAfter  = si;
                }
                ImGui::SameLine();
                ImGui::Unindent(float(depth) * 16.0f + 20.0f);
                ImGui::Separator();

                // Nested body
                if (hasBody && step.expanded) {
                        drawStepList(step.body, depth + 1, insertParent, insertAfter);
                        // Add-to-body button
                        ImGui::Indent(float(depth + 1) * 16.0f);
                        char bodyId[32];
                        snprintf(bodyId, sizeof(bodyId), "+ body##ab%p", &step);
                        if (ImGui::SmallButton(bodyId)) {
                                *insertParent = &step.body;
                                *insertAfter  = (int)step.body.size() - 1;
                        }
                        ImGui::Unindent(float(depth + 1) * 16.0f);

                        // Else body
                        if (step.kind == SdkStepKind::If && step.hasElse) {
                                if (rowIndent > 0.0f)
                                        ImGui::Indent(rowIndent);
                                ImGui::TextDisabled("else:");
                                if (rowIndent > 0.0f)
                                        ImGui::Unindent(rowIndent);
                                drawStepList(step.elseBody, depth + 1, insertParent, insertAfter);
                                ImGui::Indent(float(depth + 1) * 16.0f);
                                char elseId[32];
                                snprintf(elseId, sizeof(elseId), "+ else##ae%p", &step);
                                if (ImGui::SmallButton(elseId)) {
                                        *insertParent = &step.elseBody;
                                        *insertAfter  = (int)step.elseBody.size() - 1;
                                }
                                ImGui::Unindent(float(depth + 1) * 16.0f);
                        }
                }

                ImGui::PopID();

                if (del) {
                        steps.erase(steps.begin() + si);
                        break;
                }
        }
}

// ─── Public info helpers (used by SequenceEditor) ────────────────────────────

std::string
SdkPanel::getClassName(int classIdx) const
{
        if (classIdx < 0 || classIdx >= (int)parseResult_.classes.size())
                return "";
        return parseResult_.classes[classIdx].name;
}

std::string
SdkPanel::getCallLabel(int ci, int mi) const
{
        if (ci < 0 || ci >= (int)parseResult_.classes.size())
                return "?::?";
        const CClassDecl &cls = parseResult_.classes[ci];
        if (mi < 0 || mi >= (int)cls.methods.size())
                return cls.name + "::?";
        return cls.name + "::" + cls.methods[mi].name;
}

std::string
SdkPanel::getCFuncLabel(int fi) const
{
        if (fi < 0 || fi >= (int)parseResult_.functions.size())
                return "?";
        return parseResult_.functions[fi].name;
}

int
SdkPanel::getParamCount(int ci, int mi) const
{
        if (ci < 0 || ci >= (int)parseResult_.classes.size())
                return 0;
        const CClassDecl &cls = parseResult_.classes[ci];
        if (mi < 0 || mi >= (int)cls.methods.size())
                return 0;
        return (int)cls.methods[mi].params.size();
}

int
SdkPanel::getCFuncParamCount(int fi) const
{
        if (fi < 0 || fi >= (int)parseResult_.functions.size())
                return 0;
        return (int)parseResult_.functions[fi].params.size();
}

bool
SdkPanel::isParamPtrOrRef(int ci, int mi, int pi) const
{
        if (ci < 0 || ci >= (int)parseResult_.classes.size())
                return false;
        const CClassDecl &cls = parseResult_.classes[ci];
        if (mi < 0 || mi >= (int)cls.methods.size())
                return false;
        const CMethodDecl &m = cls.methods[mi];
        if (pi < 0 || pi >= (int)m.params.size())
                return false;
        const auto &rt = m.params[pi].rawType;
        return !rt.empty() && (rt.back() == '*' || rt.back() == '&');
}

bool
SdkPanel::isCFuncParamPtrOrRef(int fi, int pi) const
{
        if (fi < 0 || fi >= (int)parseResult_.functions.size())
                return false;
        const CFuncDecl &fn = parseResult_.functions[fi];
        if (pi < 0 || pi >= (int)fn.params.size())
                return false;
        const auto &rt = fn.params[pi].rawType;
        return !rt.empty() && (rt.back() == '*' || rt.back() == '&');
}

// ─── getParamName / getCFuncParamName / getParamRawType / getCFuncParamRawType ─

std::string
SdkPanel::getParamName(int ci, int mi, int pi) const
{
        if (ci < 0 || ci >= (int)parseResult_.classes.size())
                return "";
        const CClassDecl &cls = parseResult_.classes[ci];
        if (mi < 0 || mi >= (int)cls.methods.size())
                return "";
        const CMethodDecl &m = cls.methods[mi];
        if (pi < 0 || pi >= (int)m.params.size())
                return "";
        return m.params[pi].name;
}

std::string
SdkPanel::getCFuncParamName(int fi, int pi) const
{
        if (fi < 0 || fi >= (int)parseResult_.functions.size())
                return "";
        const CFuncDecl &fn = parseResult_.functions[fi];
        if (pi < 0 || pi >= (int)fn.params.size())
                return "";
        return fn.params[pi].name;
}

std::string
SdkPanel::getParamRawType(int ci, int mi, int pi) const
{
        if (ci < 0 || ci >= (int)parseResult_.classes.size())
                return "";
        const CClassDecl &cls = parseResult_.classes[ci];
        if (mi < 0 || mi >= (int)cls.methods.size())
                return "";
        const CMethodDecl &m = cls.methods[mi];
        if (pi < 0 || pi >= (int)m.params.size())
                return "";
        return m.params[pi].rawType;
}

std::string
SdkPanel::getCFuncParamRawType(int fi, int pi) const
{
        if (fi < 0 || fi >= (int)parseResult_.functions.size())
                return "";
        const CFuncDecl &fn = parseResult_.functions[fi];
        if (pi < 0 || pi >= (int)fn.params.size())
                return "";
        return fn.params[pi].rawType;
}

std::string
SdkPanel::getCallReturnType(bool isCFunc, bool isPython, int classIdx, int methodIdx, const std::string & /*pyFuncName*/) const
{
        if (isPython)
                return "";
        if (isCFunc) {
                if (methodIdx < 0 || methodIdx >= (int)parseResult_.functions.size())
                        return "";
                const CFuncDecl &fn = parseResult_.functions[methodIdx];
                return fn.retRaw.empty() ? std::string(ctypeLabel(fn.retType)) : fn.retRaw;
        }
        if (classIdx < 0 || classIdx >= (int)parseResult_.classes.size())
                return "";
        const CClassDecl &cls = parseResult_.classes[classIdx];
        if (methodIdx < 0 || methodIdx >= (int)cls.methods.size())
                return "";
        const CMethodDecl &m = cls.methods[methodIdx];
        return m.retRaw.empty() ? std::string(ctypeLabel(m.retType)) : m.retRaw;
}

const CStructDecl *
SdkPanel::getParamStructDecl(bool isCFunc, int ci, int mi, int pi) const
{
        if (isCFunc) {
                // mi is the function index (same convention as getCFuncParamRawType)
                if (mi < 0 || mi >= (int)parseResult_.functions.size())
                        return nullptr;
                const CFuncDecl &fn = parseResult_.functions[mi];
                if (pi < 0 || pi >= (int)fn.params.size())
                        return nullptr;
                return findParamStruct(fn.params[pi]);
        }
        if (ci < 0 || ci >= (int)parseResult_.classes.size())
                return nullptr;
        const CClassDecl &cls = parseResult_.classes[ci];
        if (mi < 0 || mi >= (int)cls.methods.size())
                return nullptr;
        const CMethodDecl &m = cls.methods[mi];
        if (pi < 0 || pi >= (int)m.params.size())
                return nullptr;
        return findParamStruct(m.params[pi]);
}

// ─── listObjects / newObject ─────────────────────────────────────────────────

std::vector<SdkPanel::ObjInfo>
SdkPanel::listObjects(int classIdx) const
{
        std::vector<ObjInfo> res;
        std::string          filterClass;
        if (classIdx >= 0 && classIdx < (int)parseResult_.classes.size())
                filterClass = parseResult_.classes[classIdx].fullName;
        for (int i = 0; i < (int)objects_.size(); ++i) {
                const ObjInstance &o = objects_[i];
                if (!filterClass.empty() && o.className != filterClass)
                        continue;
                res.push_back({i, std::string(o.label), o.className});
        }
        return res;
}

int
SdkPanel::newObject(int classIdx)
{
        int prev = (int)objects_.size();
        doNewObject(classIdx);
        return ((int)objects_.size() > prev) ? (int)objects_.size() - 1 : -1;
}

// ─── directCall / directCallC ────────────────────────────────────────────────

SdkPanel::DirectCallResult
SdkPanel::directCall(int ci, int mi, int objIdx, const std::vector<std::string> &args)
{
        if (ci < 0 || ci >= (int)parseResult_.classes.size())
                return {false, "invalid class"};
        const CClassDecl &cls = parseResult_.classes[ci];
        if (mi < 0 || mi >= (int)cls.methods.size())
                return {false, "invalid method"};
        const CMethodDecl &meth = cls.methods[mi];

        void *thisPtr = nullptr;
        if (!meth.isCtor && objIdx >= 0 && objIdx < (int)objects_.size())
                thisPtr = objects_[objIdx].ptr;

        if (!meth.isCtor && !meth.isStatic && !thisPtr)
                return {false, "no object — create one first"};

        auto effArgs = args;
        effArgs.resize(meth.params.size());
        std::vector<void *>      structPtrs(effArgs.size(), nullptr);
        std::vector<std::string> ptrVarNames(effArgs.size());
        for (size_t i = 0; i < effArgs.size(); ++i) {
                if (!effArgs[i].empty() && effArgs[i][0] == '&' && onGetVarBuf_) {
                        std::string vn  = effArgs[i].substr(1);
                        void       *buf = onGetVarBuf_(vn);
                        if (buf) {
                                structPtrs[i]  = buf;
                                ptrVarNames[i] = vn;
                        }
                }
        }

        auto res = loader_.callMethod(thisPtr, meth, effArgs, structPtrs);
        if (res.ok && onVarWritten_)
                for (const auto &vn : ptrVarNames)
                        if (!vn.empty())
                                onVarWritten_(vn);

        std::string disp = res.ok ? res.display : res.error;
        // Sequence-editor calls keep their own log (seqLog_) and result-var handling.

        return {res.ok, disp, res.ok ? (int64_t)res.rawU64 : 0};
}

SdkPanel::DirectCallResult
SdkPanel::directCallC(int fi, const std::vector<std::string> &args)
{
        if (fi < 0 || fi >= (int)parseResult_.functions.size())
                return {false, "invalid function"};
        const CFuncDecl &fn = parseResult_.functions[fi];

        auto effArgs = args;
        effArgs.resize(fn.params.size());
        std::vector<std::string> ptrVarNames(effArgs.size());
        for (size_t i = 0; i < effArgs.size(); ++i) {
                if (!effArgs[i].empty() && effArgs[i][0] == '&' && onGetVarBuf_) {
                        std::string vn  = effArgs[i].substr(1);
                        void       *buf = onGetVarBuf_(vn);
                        if (buf) {
                                char addrStr[32];
                                snprintf(addrStr, sizeof(addrStr), "0x%llx", (unsigned long long)(uintptr_t)buf);
                                effArgs[i]     = addrStr;
                                ptrVarNames[i] = vn;
                        }
                }
        }

        auto res = loader_.call(fn, effArgs);
        if (res.ok && onVarWritten_)
                for (const auto &vn : ptrVarNames)
                        if (!vn.empty())
                                onVarWritten_(vn);

        std::string disp = res.ok ? res.display : res.error;
        // Sequence-editor calls keep their own log (seqLog_) and result-var handling.

        return {res.ok, disp, res.ok ? (int64_t)res.rawU64 : 0};
}

// ─── directCallPy / Python info helpers ──────────────────────────────────────

int
SdkPanel::getPyFuncParamCount(const std::string &funcName) const
{
        for (const auto &fn : pyResult_.functions)
                if (fn.name == funcName)
                        return (int)fn.params.size();
        return 0;
}

std::string
SdkPanel::getPyFuncParamName(const std::string &funcName, int i) const
{
        for (const auto &fn : pyResult_.functions)
                if (fn.name == funcName)
                        return (i >= 0 && i < (int)fn.params.size()) ? fn.params[i].name : "";
        return "";
}

std::string
SdkPanel::getPyFuncLabel(const std::string &funcName) const
{
        return funcName;
}

SdkPanel::DirectCallResult
SdkPanel::directCallPy(const std::string &funcName, const std::vector<std::string> &args)
{
        // Start runner on demand if not running.
        if (!pyRunner_ || !pyRunner_->isAlive())
                doStartPyRunner();
        if (!pyRunner_ || !pyRunner_->isAlive())
                return {false, "Python not running"};

        const PyFuncDecl *fn = nullptr;
        for (const auto &f : pyResult_.functions)
                if (f.name == funcName) {
                        fn = &f;
                        break;
                }
        if (!fn)
                return {false, "function not found: " + funcName};

        cJSON *jargs = cJSON_CreateArray();
        for (size_t i = 0; i < fn->params.size(); ++i)
                cJSON_AddItemToArray(jargs, cJSON_CreateString(i < args.size() ? args[i].c_str() : ""));
        cJSON *cmd = cJSON_CreateObject();
        cJSON_AddStringToObject(cmd, "action", "call_func");
        cJSON_AddStringToObject(cmd, "name", funcName.c_str());
        cJSON_AddItemToObject(cmd, "args", jargs);
        char *s  = cJSON_PrintUnformatted(cmd);
        auto  rr = parsePyResp(pyRunner_->xact(s, 10000));
        cJSON_free(s);
        cJSON_Delete(cmd);

        std::string result = rr.ok ? rr.result : rr.error;
        std::string call   = funcName + "(";
        for (size_t i = 0; i < fn->params.size(); ++i) {
                if (i)
                        call += ", ";
                call += (i < args.size() ? args[i] : "");
        }
        call += ")";
        // Sequence-editor calls keep their own log (seqLog_); don't mirror into the
        // SDK panel's call history.
        // Parse result for variable write-back.
        int64_t rawV = 0;
        double  rawD = 0.0;
        if (rr.ok && !rr.result.empty()) {
                rawV = (int64_t)strtoll(rr.result.c_str(), nullptr, 0);
                rawD = strtod(rr.result.c_str(), nullptr);
        }
        return {rr.ok, result, rawV, rawD};
}

// ─── save / load ──────────────────────────────────────────────────────────────

void
SdkPanel::save(void *node, const std::string & /*baseDir*/) const
{
        cJSON *obj = static_cast<cJSON *>(node);
        cJSON_AddNumberToObject(obj, "winId", winId_);
        cJSON_AddStringToObject(obj, "userLabel", userLabel_);

        cJSON_AddStringToObject(obj, "dllPath", dllPath_);
        cJSON_AddStringToObject(obj, "headerPath", headerPath_);
        cJSON_AddStringToObject(obj, "pyPath", pyPath_);
}

void
SdkPanel::load(const void *node, const std::string &baseDir)
{
        const cJSON *obj = static_cast<const cJSON *>(node);

        if (const cJSON *w = cJSON_GetObjectItem(obj, "winId"); cJSON_IsNumber(w))
                setWindowId(w->valueint);
        if (const cJSON *l = cJSON_GetObjectItem(obj, "userLabel"); cJSON_IsString(l) && l->valuestring[0]) {
                strncpy(userLabel_, l->valuestring, sizeof(userLabel_) - 1);
                userLabel_[sizeof(userLabel_) - 1] = '\0';
                snprintf(titleBuf_, sizeof(titleBuf_), "%s###SdkPanel%d", userLabel_, winId_);
        }

        auto toAbs = [&](const char *stored) -> std::string {
                if (!stored || !stored[0])
                        return "";
                // If the stored path is already absolute (new format), use it directly.
                std::error_code       ec;
                std::filesystem::path fp(stored);
                if (fp.is_absolute()) {
                        auto abs = std::filesystem::canonical(fp, ec);
                        return ec ? stored : abs.generic_string();
                }
                // Legacy: stored as relative path → resolve from baseDir.
                if (baseDir.empty())
                        return stored;
                std::filesystem::path joined = std::filesystem::path(baseDir) / fp;
                auto                  abs    = std::filesystem::canonical(joined, ec);
                return ec ? joined.generic_string() : abs.generic_string();
        };

        if (const cJSON *d = cJSON_GetObjectItem(obj, "dllPath"); cJSON_IsString(d) && d->valuestring[0]) {
                std::string abs = toAbs(d->valuestring);
                strncpy(dllPath_, abs.c_str(), sizeof(dllPath_) - 1);
                dllPath_[sizeof(dllPath_) - 1] = '\0';
                doLoadDll();
        }
        if (const cJSON *h = cJSON_GetObjectItem(obj, "headerPath"); cJSON_IsString(h) && h->valuestring[0]) {
                std::string abs = toAbs(h->valuestring);
                strncpy(headerPath_, abs.c_str(), sizeof(headerPath_) - 1);
                headerPath_[sizeof(headerPath_) - 1] = '\0';
                doParseHeader();
        }
        if (const cJSON *p = cJSON_GetObjectItem(obj, "pyPath"); cJSON_IsString(p) && p->valuestring[0]) {
                std::string abs = toAbs(p->valuestring);
                strncpy(pyPath_, abs.c_str(), sizeof(pyPath_) - 1);
                pyPath_[sizeof(pyPath_) - 1] = '\0';
                doLoadPy();
        }
}
