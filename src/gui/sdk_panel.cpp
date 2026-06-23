#include "gui/sdk_panel.hpp"
#include "gui/sequence_editor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include "platform/native_dlg.hpp"
#include "timeops.h"

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
        selectedFnIdx_ = idx;
        fnLastResult_.clear();
        fnLastResultOk_ = false;
        if (idx < 0 || idx >= (int)parseResult_.functions.size()) {
                fnArgBufs_.clear();
                return;
        }
        const CFuncDecl &fn = parseResult_.functions[idx];
        fnArgBufs_.resize(fn.params.size());
        for (auto &b : fnArgBufs_)
                b = {};
}

void
SdkPanel::selectMethod(int classIdx, int methodIdx)
{
        selectedClassIdx_  = classIdx;
        selectedMethodIdx_ = methodIdx;
        methArgBufs_.clear();
        structBufs_.clear();

        if (classIdx < 0 || classIdx >= (int)parseResult_.classes.size())
                return;
        const CClassDecl &cls = parseResult_.classes[classIdx];
        if (methodIdx < 0 || methodIdx >= (int)cls.methods.size())
                return;

        const CMethodDecl &meth = cls.methods[methodIdx];
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
        if (res.ok && ctypeIsInteger(fn.retType) && monitorPushEnabled_)
                pushToMonitor(fn.name, static_cast<float>(res.rawU64), tsSec);
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
        if (res.ok && ctypeIsInteger(meth.retType) && monitorPushEnabled_) {
                std::string chKey = cls.name + "::" + meth.name;
                pushToMonitor(chKey, static_cast<float>(res.rawU64), tsSec);
        }
        if (!res.ok)
                setStatus(res.error, true);
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

                if (fn.params.empty()) {
                        ImGui::TextDisabled("  (%s)", tr("no parameters", "无参数"));
                } else {
                        for (size_t i = 0; i < fn.params.size(); ++i) {
                                const CParam &p       = fn.params[i];
                                bool          isCharP = ctypeIsCharPtr(p.type, p.rawType);
                                char          lbl[128], id[64];
                                if (!p.name.empty())
                                        snprintf(lbl, sizeof(lbl), "%s %s", ctypeLabel(p.type), p.name.c_str());
                                else
                                        snprintf(lbl, sizeof(lbl), "%s [%zu]", ctypeLabel(p.type), i);
                                snprintf(id, sizeof(id), "##farg%zu", i);
                                ImGui::SetNextItemWidth(280.0f);
                                if (i < fnArgBufs_.size()) {
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
                                        ImGui::SameLine();
                                        ImGui::TextDisabled("%s", lbl);
                                }
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
                        // Timestamp badge
                        {
                                uint64_t ms  = e.tsMs % 86400000ULL;
                                int      hh  = (int)(ms / 3600000);
                                ms          %= 3600000;
                                int mm       = (int)(ms / 60000);
                                ms          %= 60000;
                                int ss       = (int)(ms / 1000);
                                ms          %= 1000;
                                char ts[24];
                                snprintf(ts, sizeof(ts), "[%02d:%02d:%02d.%03d] ", hh, mm, ss, (int)ms);
                                ImGui::TextDisabled("%s", ts);
                                ImGui::SameLine(0, 0);
                        }
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              e.ok ? ImVec4(0.85f, 0.85f, 0.85f, 1.f) : ImVec4(1.f, 0.4f, 0.4f, 1.f));
                        ImGui::TextUnformatted(e.call.c_str());
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        ImGui::TextDisabled(" → ");
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

        // ── object manager ──
        ImGui::Spacing();
        ImGui::Text("%s:", tr("Objects", "对象管理"));
        ImGui::SameLine();
        bool canNew = loader_.isLoaded();
        if (!canNew)
                ImGui::BeginDisabled();
        if (ImGui::Button(tr("+ New", "+ 新建")))
                doNewObject(selectedClassIdx_);
        if (!canNew)
                ImGui::EndDisabled();

        if (!objects_.empty()) {
                ImGui::SameLine();
                bool canDel = (selectedObjIdx_ >= 0 && selectedObjIdx_ < (int)objects_.size());
                if (!canDel)
                        ImGui::BeginDisabled();
                if (ImGui::Button(tr("Delete", "删除")))
                        doDeleteObject(selectedObjIdx_);
                if (!canDel)
                        ImGui::EndDisabled();

                ImGui::BeginChild("##objlist", ImVec2(0, 60.0f), true);
                for (int oi = 0; oi < (int)objects_.size(); ++oi) {
                        const ObjInstance &obj  = objects_[oi];
                        bool               selO = (oi == selectedObjIdx_);
                        char               objLbl[128];
                        snprintf(objLbl, sizeof(objLbl), "%s  @0x%p", obj.label, obj.ptr);
                        if (ImGui::Selectable(objLbl, selO))
                                selectedObjIdx_ = oi;
                }
                ImGui::EndChild();
        }

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

        // ── object selector (non-ctor) ──
        if (!meth.isCtor && !meth.isDtor) {
                ImGui::SetNextItemWidth(260.0f);
                const char *preview = (selectedObjIdx_ >= 0 && selectedObjIdx_ < (int)objects_.size())
                                          ? objects_[selectedObjIdx_].label
                                          : tr("(none)", "(无)");
                if (ImGui::BeginCombo(tr("Object (this)##obj", "对象 (this)##obj"), preview)) {
                        for (int oi = 0; oi < (int)objects_.size(); ++oi) {
                                bool sel = (oi == selectedObjIdx_);
                                char lbl[128];
                                snprintf(lbl, sizeof(lbl), "%s  @0x%p", objects_[oi].label, objects_[oi].ptr);
                                if (ImGui::Selectable(lbl, sel))
                                        selectedObjIdx_ = oi;
                                if (sel)
                                        ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                }
        }

        ImGui::Spacing();

        // ── parameters ──
        if (meth.params.empty()) {
                ImGui::TextDisabled("  (%s)", tr("no parameters", "无参数"));
        } else {
                for (size_t pi = 0; pi < meth.params.size(); ++pi) {
                        const CParam &p = meth.params[pi];
                        char          rowId[64];
                        snprintf(rowId, sizeof(rowId), "##mp%zu", pi);

                        // Struct reference parameter → show struct field editor.
                        if (pi < structBufs_.size() && structBufs_[pi].decl) {
                                const CStructDecl    *sd       = structBufs_[pi].decl;
                                std::vector<uint8_t> &buf      = structBufs_[pi].data;
                                bool                 &expanded = structBufs_[pi].expanded;

                                char hdr[128];
                                snprintf(hdr,
                                         sizeof(hdr),
                                         "%s: [%s]  ##shdr%zu",
                                         p.name.empty() ? "param" : p.name.c_str(),
                                         sd->name.c_str(),
                                         pi);
                                ImGui::SetNextItemOpen(expanded, ImGuiCond_Always);
                                expanded = ImGui::CollapsingHeader(hdr);
                                if (expanded && buf.size() >= sd->totalSize) {
                                        ImGui::Indent(16.0f);
                                        for (const auto &field : sd->fields) {
                                                if (field.isArray) {
                                                        // Show array elements.
                                                        ImGui::TextDisabled("[array %zu × %s]",
                                                                            field.arrayCount,
                                                                            ctypeLabel(field.arrayElemType));
                                                        size_t esz = ctypeSize(field.arrayElemType);
                                                        for (size_t ai = 0; ai < field.arrayCount; ++ai) {
                                                                char fid[64];
                                                                snprintf(fid,
                                                                         sizeof(fid),
                                                                         "##af%zu_%zu_%zu",
                                                                         pi,
                                                                         &field - sd->fields.data(),
                                                                         ai);
                                                                ImGui::SetNextItemWidth(140.0f);
                                                                uint8_t *p2 = buf.data() + field.offset + ai * esz;
                                                                if (field.arrayElemType == CType::F64) {
                                                                        double v;
                                                                        memcpy(&v, p2, 8);
                                                                        if (ImGui::InputDouble(fid, &v, 0, 0, "%.6g"))
                                                                                memcpy(p2, &v, 8);
                                                                } else if (field.arrayElemType == CType::F32) {
                                                                        float v;
                                                                        memcpy(&v, p2, 4);
                                                                        if (ImGui::InputFloat(fid, &v))
                                                                                memcpy(p2, &v, 4);
                                                                } else {
                                                                        int iv = 0;
                                                                        memcpy(&iv, p2, std::min(esz, (size_t)4));
                                                                        if (ImGui::InputInt(fid, &iv))
                                                                                memcpy(p2, &iv, std::min(esz, (size_t)4));
                                                                }
                                                                ImGui::SameLine();
                                                                ImGui::TextDisabled("%s[%zu]", field.name.c_str(), ai);
                                                        }
                                                } else if (field.type == CType::F64) {
                                                        char fid[64];
                                                        snprintf(
                                                            fid, sizeof(fid), "##df%zu_%zu", pi, &field - sd->fields.data());
                                                        ImGui::SetNextItemWidth(180.0f);
                                                        double v;
                                                        memcpy(&v, buf.data() + field.offset, 8);
                                                        if (ImGui::InputDouble(fid, &v, 0, 0, "%.6g"))
                                                                memcpy(buf.data() + field.offset, &v, 8);
                                                        ImGui::SameLine();
                                                        ImGui::TextDisabled("double %s", field.name.c_str());
                                                } else if (field.type == CType::F32) {
                                                        char fid[64];
                                                        snprintf(
                                                            fid, sizeof(fid), "##ff%zu_%zu", pi, &field - sd->fields.data());
                                                        ImGui::SetNextItemWidth(180.0f);
                                                        float v;
                                                        memcpy(&v, buf.data() + field.offset, 4);
                                                        if (ImGui::InputFloat(fid, &v))
                                                                memcpy(buf.data() + field.offset, &v, 4);
                                                        ImGui::SameLine();
                                                        ImGui::TextDisabled("float %s", field.name.c_str());
                                                } else {
                                                        char fid[64];
                                                        snprintf(
                                                            fid, sizeof(fid), "##if%zu_%zu", pi, &field - sd->fields.data());
                                                        ImGui::SetNextItemWidth(180.0f);
                                                        int64_t v = 0;
                                                        memcpy(&v, buf.data() + field.offset, std::min(field.size, (size_t)8));
                                                        int iv = (int)v;
                                                        if (ImGui::InputInt(fid, &iv)) {
                                                                v = iv;
                                                                memcpy(buf.data() + field.offset,
                                                                       &v,
                                                                       std::min(field.size, (size_t)8));
                                                        }
                                                        ImGui::SameLine();
                                                        ImGui::TextDisabled(
                                                            "%s %s", ctypeLabel(field.type), field.name.c_str());
                                                }
                                        }
                                        ImGui::Unindent(16.0f);
                                }
                                continue;
                        }

                        // Enum parameter → dropdown.
                        const CEnumDecl *ed = findParamEnum(p);
                        if (ed && !ed->values.empty()) {
                                // Find current value as index.
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
                                char cmId[64];
                                snprintf(cmId, sizeof(cmId), "##ec%zu", pi);
                                ImGui::SetNextItemWidth(260.0f);
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
                                ImGui::SameLine();
                                ImGui::TextDisabled("%s %s", ed->name.c_str(), p.name.empty() ? "" : p.name.c_str());
                                continue;
                        }

                        // Plain parameter → text input.
                        char lbl[128];
                        if (!p.name.empty())
                                snprintf(lbl, sizeof(lbl), "%s %s", ctypeLabel(p.type), p.name.c_str());
                        else
                                snprintf(lbl, sizeof(lbl), "%s [%zu]", ctypeLabel(p.type), pi);

                        ImGui::SetNextItemWidth(260.0f);
                        if (pi < methArgBufs_.size()) {
                                bool isCharP = ctypeIsCharPtr(p.type, p.rawType);
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
                                ImGui::SameLine();
                                ImGui::TextDisabled("%s", lbl);
                        }
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
        if (!canCall && loader_.isLoaded() && !meth.isCtor && objects_.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", tr("(create an object first)", "(请先创建对象)"));
        }

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
                        uint64_t ms  = e.tsMs % 86400000ULL;
                        int      hh  = (int)(ms / 3600000);
                        ms          %= 3600000;
                        int mm       = (int)(ms / 60000);
                        ms          %= 60000;
                        int ss       = (int)(ms / 1000);
                        ms          %= 1000;
                        char ts[24];
                        snprintf(ts, sizeof(ts), "[%02d:%02d:%02d.%03d] ", hh, mm, ss, (int)ms);
                        ImGui::TextDisabled("%s", ts);
                        ImGui::SameLine(0, 0);
                }
                ImGui::PushStyleColor(ImGuiCol_Text, e.ok ? ImVec4(0.85f, 0.85f, 0.85f, 1.f) : ImVec4(1.f, 0.4f, 0.4f, 1.f));
                ImGui::TextUnformatted(e.call.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextDisabled(" → ");
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
                std::string fullPath(pyPath_);
                auto        slash = fullPath.find_last_of("/\\");
                std::string fname = (slash != std::string::npos) ? fullPath.substr(slash + 1) : fullPath;
                ImGui::SameLine();
                ImGui::TextDisabled("— %s", fname.c_str());
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
                                        pySelFnIdx_ = i;
                                        pyFnArgBufs_.clear();
                                        pyFnArgBufs_.resize(pyResult_.functions[i].params.size());
                                        for (size_t j = 0; j < pyResult_.functions[i].params.size(); ++j) {
                                                const auto &parm = pyResult_.functions[i].params[j];
                                                if (!parm.defaultVal.empty())
                                                        strncpy(pyFnArgBufs_[j].text,
                                                                parm.defaultVal.c_str(),
                                                                sizeof(ArgBuf::text) - 1);
                                        }
                                        pyLastResult_.clear();
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
                                        for (size_t i = 0; i < fn.params.size(); ++i) {
                                                char id[64];
                                                snprintf(id, sizeof(id), "##pyfa%zu", i);
                                                ImGui::SetNextItemWidth(260.0f);
                                                if (i < pyFnArgBufs_.size())
                                                        ImGui::InputText(id, pyFnArgBufs_[i].text, sizeof(ArgBuf::text));
                                                ImGui::SameLine();
                                                ImGui::TextDisabled("%s", fn.params[i].name.c_str());
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
                                        const HistEntry &e  = snap[i];
                                        uint64_t         ms = e.tsMs % 86400000ULL;
                                        char             ts[24];
                                        snprintf(ts,
                                                 sizeof(ts),
                                                 "[%02d:%02d:%02d.%03d] ",
                                                 (int)(ms / 3600000),
                                                 (int)(ms % 3600000 / 60000),
                                                 (int)(ms % 60000 / 1000),
                                                 (int)(ms % 1000));
                                        ImGui::TextDisabled("%s", ts);
                                        ImGui::SameLine(0, 0);
                                        ImGui::PushStyleColor(ImGuiCol_Text,
                                                              e.ok ? ImVec4(0.85f, 0.85f, 0.85f, 1.f)
                                                                   : ImVec4(1.f, 0.4f, 0.4f, 1.f));
                                        ImGui::TextUnformatted(e.call.c_str());
                                        ImGui::PopStyleColor();
                                        ImGui::SameLine();
                                        ImGui::TextDisabled(" → ");
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
                                                pySelMethIdx_ = mi;
                                                pyMethArgBufs_.clear();
                                                pyMethArgBufs_.resize(cls.methods[mi].params.size());
                                                for (size_t j = 0; j < cls.methods[mi].params.size(); ++j) {
                                                        const auto &parm = cls.methods[mi].params[j];
                                                        if (!parm.defaultVal.empty())
                                                                strncpy(pyMethArgBufs_[j].text,
                                                                        parm.defaultVal.c_str(),
                                                                        sizeof(ArgBuf::text) - 1);
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
                                        for (size_t i = 0; i < meth.params.size(); ++i) {
                                                char id[64];
                                                snprintf(id, sizeof(id), "##pyma%zu", i);
                                                ImGui::SetNextItemWidth(260.0f);
                                                if (i < pyMethArgBufs_.size())
                                                        ImGui::InputText(id, pyMethArgBufs_[i].text, sizeof(ArgBuf::text));
                                                ImGui::SameLine();
                                                ImGui::TextDisabled("%s", meth.params[i].name.c_str());
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
                                        const HistEntry &e  = snap[i];
                                        uint64_t         ms = e.tsMs % 86400000ULL;
                                        char             ts[24];
                                        snprintf(ts,
                                                 sizeof(ts),
                                                 "[%02d:%02d:%02d.%03d] ",
                                                 (int)(ms / 3600000),
                                                 (int)(ms % 3600000 / 60000),
                                                 (int)(ms % 60000 / 1000),
                                                 (int)(ms % 1000));
                                        ImGui::TextDisabled("%s", ts);
                                        ImGui::SameLine(0, 0);
                                        ImGui::PushStyleColor(ImGuiCol_Text,
                                                              e.ok ? ImVec4(0.85f, 0.85f, 0.85f, 1.f)
                                                                   : ImVec4(1.f, 0.4f, 0.4f, 1.f));
                                        ImGui::TextUnformatted(e.call.c_str());
                                        ImGui::PopStyleColor();
                                        ImGui::SameLine();
                                        ImGui::TextDisabled(" → ");
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
                                if (ui::Button(tr("Unload", "卸载"), ui::BtnStyle::Danger)) {
                                        loader_.unload();
                                        setStatus(tr("Unloaded", "已卸载"), false);
                                }
                        }
                        if (dllPath_[0]) {
                                std::string dp(dllPath_);
                                auto        sl = dp.find_last_of("/\\");
                                ImGui::SameLine();
                                ImGui::TextDisabled("— %s", (sl != std::string::npos ? dp.substr(sl + 1) : dp).c_str());
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
                                std::string hp(headerPath_);
                                auto        sl = hp.find_last_of("/\\");
                                ImGui::SameLine();
                                ImGui::TextDisabled("— %s", (sl != std::string::npos ? hp.substr(sl + 1) : hp).c_str());
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
                // Monitor-push toggle (right-aligned)
                {
                        float avail = ImGui::GetContentRegionAvail().x;
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - 180.0f);
                        ImGui::PushStyleColor(
                            ImGuiCol_Text, monitorPushEnabled_ ? ImVec4(0.3f, 1.f, 0.5f, 1.f) : ImVec4(0.5f, 0.5f, 0.5f, 1.f));
                        if (ImGui::SmallButton(monitorPushEnabled_ ? tr("■ Monitor Push ON", "■ 监视器推送 开")
                                                                   : tr("□ Monitor Push OFF", "□ 监视器推送 关")))
                                monitorPushEnabled_ = !monitorPushEnabled_;
                        ImGui::PopStyleColor();
                }
                ImGui::EndTabBar();
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
        e.tsMs   = get_mono_ts_ms();
        e.tsSec  = tsSec;
        std::lock_guard<std::mutex> lk(histMtx_);
        if (history_.size() >= 400)
                history_.erase(history_.begin());
        history_.push_back(std::move(e));
}

// ─── pushToMonitor ────────────────────────────────────────────────────────────

void
SdkPanel::pushToMonitor(const std::string &chanKey, float val, double tsSec)
{
        pinnedChannels_[chanKey] = val;
        if (onMonitorPush_)
                onMonitorPush_(chanKey, val, tsSec);
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
                                                if (res.ok && ctypeIsInteger(meth.retType) && monitorPushEnabled_)
                                                        pushToMonitor(
                                                            cls.name + "::" + meth.name, static_cast<float>(res.rawU64), ts);
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
                        ImGui::SameLine();
                        ImGui::TextDisabled("→$var");
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

        std::string disp  = res.ok ? res.display : res.error;
        double      tsSec = sessionTimeSec();
        pushHistory(cls.name + "::" + meth.name, disp, res.ok, tsSec);
        if (res.ok && ctypeIsInteger(meth.retType) && monitorPushEnabled_)
                pushToMonitor(cls.name + "::" + meth.name, static_cast<float>(res.rawU64), tsSec);

        return {res.ok, disp};
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

        std::string disp  = res.ok ? res.display : res.error;
        double      tsSec = sessionTimeSec();
        pushHistory(fn.name, disp, res.ok, tsSec);
        if (res.ok && ctypeIsInteger(fn.retType) && monitorPushEnabled_)
                pushToMonitor(fn.name, static_cast<float>(res.rawU64), tsSec);

        return {res.ok, disp};
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
        pushHistory(call, result, rr.ok, sessionTimeSec());
        return {rr.ok, result};
}

// ─── save / load ──────────────────────────────────────────────────────────────

void
SdkPanel::save(void *node, const std::string &baseDir) const
{
        cJSON *obj = static_cast<cJSON *>(node);
        cJSON_AddNumberToObject(obj, "winId", winId_);

        auto toRel = [&](const char *path) -> std::string {
                if (!path || !path[0] || baseDir.empty())
                        return path ? path : "";
                std::error_code       ec;
                std::filesystem::path rel = std::filesystem::relative(path, baseDir, ec);
                if (ec || rel.empty())
                        return path;
                return rel.generic_string();
        };

        cJSON_AddStringToObject(obj, "dllPath", toRel(dllPath_).c_str());
        cJSON_AddStringToObject(obj, "headerPath", toRel(headerPath_).c_str());
        cJSON_AddStringToObject(obj, "pyPath", toRel(pyPath_).c_str());
}

void
SdkPanel::load(const void *node, const std::string &baseDir)
{
        const cJSON *obj = static_cast<const cJSON *>(node);

        if (const cJSON *w = cJSON_GetObjectItem(obj, "winId"); cJSON_IsNumber(w))
                setWindowId(w->valueint);

        auto toAbs = [&](const char *rel) -> std::string {
                if (!rel || !rel[0])
                        return "";
                if (baseDir.empty())
                        return rel;
                std::filesystem::path p = std::filesystem::path(baseDir) / rel;
                std::error_code       ec;
                auto                  abs = std::filesystem::canonical(p, ec);
                return ec ? p.generic_string() : abs.generic_string();
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
