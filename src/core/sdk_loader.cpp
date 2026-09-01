#include "core/sdk_loader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ffi.h>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static void *
sdkLoadWindows(const char *utf8Path)
{
        if (!utf8Path || !utf8Path[0]) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return nullptr;
        }
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path, -1, nullptr, 0);
        if (length <= 0)
                return nullptr;
        std::wstring widePath(static_cast<size_t>(length), L'\0');
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path, -1, widePath.data(), length))
                return nullptr;
        HMODULE module =
            LoadLibraryExW(widePath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (module)
                return reinterpret_cast<void *>(module);

        // Some vendor SDKs load their own secondary dependencies from DllMain
        // and fail with ERROR_MOD_NOT_FOUND under the restricted search flags
        // Temporarily add the selected DLL's directory for legacy dependency
        // lookup, then restore the process setting immediately afterwards
        static std::mutex directorySearchMutex;
        std::lock_guard   lock(directorySearchMutex);
        const size_t      slash = widePath.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
                return reinterpret_cast<void *>(LoadLibraryW(widePath.c_str()));

        const std::wstring   directory = widePath.substr(0, slash);
        const DWORD          oldLength = GetDllDirectoryW(0, nullptr);
        std::vector<wchar_t> oldDirectory(oldLength > 0 ? oldLength + 1 : 1, L'\0');
        if (oldLength > 0)
                GetDllDirectoryW(static_cast<DWORD>(oldDirectory.size()), oldDirectory.data());
        SetDllDirectoryW(directory.c_str());
        module = LoadLibraryW(widePath.c_str());
        SetDllDirectoryW(oldLength > 0 ? oldDirectory.data() : nullptr);
        return reinterpret_cast<void *>(module);
}

#define SDK_LOAD(p)   sdkLoadWindows(p)
#define SDK_SYM(h, n) (void *)GetProcAddress((HMODULE)(h), (n))
#define SDK_FREE(h)   FreeLibrary((HMODULE)(h))
#else
#include <dlfcn.h>
#define SDK_LOAD(p)   dlopen((p), RTLD_LAZY | RTLD_LOCAL)
#define SDK_SYM(h, n) dlsym((h), (n))
#define SDK_FREE(h)   dlclose(h)
#endif

// ─── crash-guarded ffi_call ───────────────────────────────────────────────────
/**
 * @brief A misbehaving SDK function (bad pointer, wrong struct size, null deref) raises a
 *        hardware exception that would otherwise tear down the whole app. On Windows we
 *        wrap the call in SEH so an access violation (etc.) is turned into a clean error
 *        return instead of a crash. This MUST stay in its own function with no C++ objects
 *        that require unwinding (a hard requirement for __try/__except)
 *
 *        Caveat: SEH catches the fault at the moment it happens. It cannot undo heap/stack
 *        corruption a buggy callee may have already done, so a later crash is still
 *        possible — but the common "the SDK call itself segfaults" case no longer kills us
 */
static bool
guardedFfiCall(ffi_cif *cif, void (*fn)(void), void *retBuf, void **argPtrs) noexcept
{
#if defined(_WIN32)
        __try {
                ffi_call(cif, fn, retBuf, argPtrs);
                return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
        }
#else
        ffi_call(cif, fn, retBuf, argPtrs);
        return true;
#endif
}

struct SdkLoader::PreparedCall {
        void                   *fn{nullptr};
        CType                   returnType{CType::Void};
        bool                    implicitThis{false};
        std::vector<CType>      paramTypes;
        std::vector<ffi_type *> argTypes;
        ffi_cif                 cif{};
};

struct SdkLoader::CallCache {
        std::unordered_map<std::string, std::vector<std::unique_ptr<PreparedCall>>> calls;
        std::unordered_map<std::string, size_t>                                     methodSymbols;
};

SdkLoader::SdkLoader() : callCache_(std::make_unique<CallCache>()) {}

SdkLoader::~SdkLoader()
{
        unload();
}

// ─── load / unload ────────────────────────────────────────────────────────────

bool
SdkLoader::load(const std::string &path)
{
        unload();
        handle_ = SDK_LOAD(path.c_str());
        if (!handle_) {
#if defined(_WIN32)
                const DWORD error = GetLastError();
                char        buf[512];
                snprintf(buf,
                         sizeof(buf),
                         "LoadLibraryExW failed (error %lu). Check that dependent DLLs are beside the selected DLL.",
                         error);
                prevError_ = buf;
#else
                const char *e = dlerror();
                prevError_    = e ? e : "dlopen failed";
#endif
                return false;
        }
        path_      = path;
        prevError_ = {};
        return true;
}

void
SdkLoader::unload()
{
        callCache_->calls.clear();
        callCache_->methodSymbols.clear();
        exports_.clear();
        if (handle_) {
                // Opt-in plugin contract: void ava_tool_sdk_shutdown(void)
                // Close cyclic sessions outside DllMain, before releasing the DLL
                if (void *shutdown = SDK_SYM(handle_, "ava_tool_sdk_shutdown")) {
                        ffi_cif cif{};
                        if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 0, &ffi_type_void, nullptr) == FFI_OK)
                                guardedFfiCall(&cif, reinterpret_cast<void (*)(void)>(shutdown), nullptr, nullptr);
                }
                SDK_FREE(handle_);
                handle_ = nullptr;
                path_.clear();
        }
}

void *
SdkLoader::resolve(const std::string &name)
{
        if (!handle_)
                return nullptr;
        void *sym = SDK_SYM(handle_, name.c_str());
        if (!sym) {
#if defined(_WIN32)
                char buf[256];
                snprintf(buf, sizeof(buf), "symbol '%s' not found (error %lu)", name.c_str(), GetLastError());
                prevError_ = buf;
#else
                const char *e = dlerror();
                prevError_    = e ? e : ("symbol '" + name + "' not found");
#endif
        }
        return sym;
}

// ─── type mapping ─────────────────────────────────────────────────────────────

static ffi_type *
toFfiType(CType t)
{
        switch (t) {
                case CType::Void:
                        return &ffi_type_void;
                case CType::Bool:
                        return &ffi_type_uint8;
                case CType::I8:
                        return &ffi_type_sint8;
                case CType::I16:
                        return &ffi_type_sint16;
                case CType::I32:
                        return &ffi_type_sint32;
                case CType::I64:
                        return &ffi_type_sint64;
                case CType::U8:
                        return &ffi_type_uint8;
                case CType::U16:
                        return &ffi_type_uint16;
                case CType::U32:
                        return &ffi_type_uint32;
                case CType::U64:
                        return &ffi_type_uint64;
                case CType::F32:
                        return &ffi_type_float;
                case CType::F64:
                        return &ffi_type_double;
                case CType::Ptr:
                        return &ffi_type_pointer;
                case CType::Unknown:
                        return &ffi_type_pointer;
        }
        return &ffi_type_pointer;
}

SdkLoader::PreparedCall *
SdkLoader::prepareCall(const std::string &name, CType returnType, const std::vector<CParam> &params, bool implicitThis)
{
        auto found = callCache_->calls.find(name);
        if (found != callCache_->calls.end()) {
                for (const auto &candidate : found->second) {
                        if (candidate->returnType != returnType || candidate->implicitThis != implicitThis ||
                            candidate->paramTypes.size() != params.size())
                                continue;
                        bool same = true;
                        for (size_t i = 0; i < params.size(); ++i)
                                same = same && candidate->paramTypes[i] == params[i].type;
                        if (same)
                                return candidate.get();
                }
        }
        void *fn = resolve(name);
        if (!fn)
                return nullptr;
        auto prepared          = std::make_unique<PreparedCall>();
        prepared->fn           = fn;
        prepared->returnType   = returnType;
        prepared->implicitThis = implicitThis;
        prepared->argTypes.reserve(params.size() + (implicitThis ? 1 : 0));
        if (implicitThis)
                prepared->argTypes.push_back(&ffi_type_pointer);
        for (const auto &param : params) {
                prepared->paramTypes.push_back(param.type);
                prepared->argTypes.push_back(toFfiType(param.type));
        }
        if (ffi_prep_cif(&prepared->cif,
                         FFI_DEFAULT_ABI,
                         static_cast<unsigned>(prepared->argTypes.size()),
                         toFfiType(returnType),
                         prepared->argTypes.empty() ? nullptr : prepared->argTypes.data()) != FFI_OK) {
                prevError_ = "ffi_prep_cif failed";
                return nullptr;
        }
        auto *result = prepared.get();
        callCache_->calls[name].push_back(std::move(prepared));
        return result;
}

// ─── argument marshaling ──────────────────────────────────────────────────────

struct ArgStore {
        union {
                int8_t   i8;
                int16_t  i16;
                int32_t  i32;
                int64_t  i64;
                uint8_t  u8;
                uint16_t u16;
                uint32_t u32;
                uint64_t u64;
                float    f32;
                double   f64;
                void    *ptr;
        } v{};
        std::vector<char> strBuf; // keeps char* alive during the call
};

// Most SDK functions take only a handful of arguments. Keep those calls off
// the allocator while retaining support for arbitrary parsed signatures
struct CallArguments {
        static constexpr size_t           kInlineArgs = 8;
        std::array<ArgStore, kInlineArgs> inlineStores;
        std::array<void *, kInlineArgs>   inlinePointers{};
        std::vector<ArgStore>             extraStores;
        std::vector<void *>               extraPointers;
        ArgStore                         *stores;
        void                            **pointers;

        explicit CallArguments(size_t count)
            : extraStores(count > kInlineArgs ? count : 0), extraPointers(count > kInlineArgs ? count : 0),
              stores(count > kInlineArgs ? extraStores.data() : inlineStores.data()),
              pointers(count > kInlineArgs ? extraPointers.data() : inlinePointers.data())
        {
        }
};

static bool
marshalArg(const CParam &param, const std::string &str, ArgStore &store)
{
        bool isCharPtr = ctypeIsCharPtr(param.type, param.rawType);

        if (isCharPtr) {
                store.strBuf.assign(str.begin(), str.end());
                store.strBuf.push_back('\0');
                store.v.ptr = store.strBuf.data();
                return true;
        }

        if (param.type == CType::Ptr || param.type == CType::Unknown) {
                if (!str.empty()) {
                        // Try to parse as a numeric pointer address (e.g. "0x12345678")
                        // If the ENTIRE string is consumed, use it as-is
                        // Otherwise (e.g. "192.168.137.53", "hello") treat it as a C string —
                        // this covers Windows typedef strings (LPCSTR, LPSTR, PSTR …) that the
                        // header parser doesn't recognise as char* from the raw type name alone
                        char    *endp = nullptr;
                        uint64_t val  = strtoull(str.c_str(), &endp, 0);
                        if (endp && *endp == '\0') {
                                store.v.ptr = reinterpret_cast<void *>(static_cast<uintptr_t>(val));
                        } else {
                                store.strBuf.assign(str.begin(), str.end());
                                store.strBuf.push_back('\0');
                                store.v.ptr = store.strBuf.data();
                        }
                } else {
                        store.v.ptr = nullptr;
                }
                return true;
        }

        if (param.type == CType::F32) {
                store.v.f32 = static_cast<float>(strtod(str.c_str(), nullptr));
                return true;
        }
        if (param.type == CType::F64) {
                store.v.f64 = strtod(str.c_str(), nullptr);
                return true;
        }

        // Integer types
        if (param.type == CType::U8 || param.type == CType::U16 || param.type == CType::U32 || param.type == CType::U64 ||
            param.type == CType::Bool) {
                uint64_t val = strtoull(str.c_str(), nullptr, 0);
                switch (param.type) {
                        case CType::Bool:
                                store.v.u8 = val ? 1 : 0;
                                break;
                        case CType::U8:
                                store.v.u8 = (uint8_t)val;
                                break;
                        case CType::U16:
                                store.v.u16 = (uint16_t)val;
                                break;
                        case CType::U32:
                                store.v.u32 = (uint32_t)val;
                                break;
                        case CType::U64:
                                store.v.u64 = val;
                                break;
                        default:
                                break;
                }
                return true;
        }

        // Signed integer types
        {
                int64_t val = strtoll(str.c_str(), nullptr, 0);
                switch (param.type) {
                        case CType::I8:
                                store.v.i8 = (int8_t)val;
                                break;
                        case CType::I16:
                                store.v.i16 = (int16_t)val;
                                break;
                        case CType::I32:
                                store.v.i32 = (int32_t)val;
                                break;
                        case CType::I64:
                                store.v.i64 = val;
                                break;
                        default:
                                break;
                }
        }
        return true;
}

// ─── call ─────────────────────────────────────────────────────────────────────

CallResult
SdkLoader::call(const CFuncDecl &decl, const std::vector<std::string> &argStrs, const std::vector<void *> &pointerOverrides)
{
        CallResult res;

        auto *prepared = prepareCall(decl.name, decl.retType, decl.params);
        if (!prepared) {
                res.error = prevError_;
                return res;
        }

        size_t nArgs = decl.params.size();
        if (argStrs.size() < nArgs) {
                res.error = "not enough arguments";
                return res;
        }

        // Marshal arguments
        CallArguments arguments(nArgs);
        auto         *stores  = arguments.stores;
        auto        **argPtrs = arguments.pointers;
        for (size_t i = 0; i < nArgs; ++i) {
                if (i < pointerOverrides.size() && pointerOverrides[i] != nullptr) {
                        if (decl.params[i].type != CType::Ptr) {
                                res.error = "pointer override used for non-pointer arg " + std::to_string(i);
                                return res;
                        }
                        stores[i].v.ptr = pointerOverrides[i];
                } else if (!marshalArg(decl.params[i], argStrs[i], stores[i])) {
                        res.error = "failed to marshal arg " + std::to_string(i);
                        return res;
                }
                argPtrs[i] = &stores[i].v;
        }

        // Return value storage — large enough for any return type
        // libffi stores small integer returns zero/sign-extended in ffi_arg
        alignas(16) uint8_t retBuf[16] = {};

        if (!guardedFfiCall(&prepared->cif, (void (*)(void))prepared->fn, retBuf, nArgs ? argPtrs : nullptr)) {
                res.ok    = false;
                res.error = "SDK function crashed (access violation) — call aborted, app kept alive";
                return res;
        }

        // Format result
        res.ok = true;
        char fmt[128];
        switch (decl.retType) {
                case CType::Void:
                        res.display = "(void)";
                        break;
                case CType::F32: {
                        float f;
                        memcpy(&f, retBuf, sizeof(f));
                        res.rawF64 = f;
                        snprintf(fmt, sizeof(fmt), "%g  (0x%08X)", f, *(uint32_t *)retBuf);
                        res.display = fmt;
                        break;
                }
                case CType::F64: {
                        double d;
                        memcpy(&d, retBuf, sizeof(d));
                        res.rawF64 = d;
                        snprintf(fmt, sizeof(fmt), "%g", d);
                        res.display = fmt;
                        break;
                }
                case CType::Bool: {
                        uint8_t v;
                        memcpy(&v, retBuf, sizeof(v));
                        res.rawU64  = v;
                        res.display = v ? "true (1)" : "false (0)";
                        break;
                }
                case CType::I8: {
                        int8_t v;
                        memcpy(&v, retBuf, sizeof(v));
                        res.rawU64 = (uint64_t)(int64_t)v;
                        snprintf(fmt, sizeof(fmt), "%d  (0x%02X)", (int)v, (uint8_t)v);
                        res.display = fmt;
                        break;
                }
                case CType::I16: {
                        int16_t v;
                        memcpy(&v, retBuf, sizeof(v));
                        res.rawU64 = (uint64_t)(int64_t)v;
                        snprintf(fmt, sizeof(fmt), "%d  (0x%04X)", (int)v, (uint16_t)v);
                        res.display = fmt;
                        break;
                }
                case CType::I32: {
                        int32_t v;
                        memcpy(&v, retBuf, sizeof(v));
                        res.rawU64 = (uint64_t)(int64_t)v;
                        snprintf(fmt, sizeof(fmt), "%d  (0x%08X)", v, (uint32_t)v);
                        res.display = fmt;
                        break;
                }
                case CType::I64: {
                        int64_t v;
                        memcpy(&v, retBuf, sizeof(v));
                        res.rawU64 = (uint64_t)v;
                        snprintf(fmt, sizeof(fmt), "%lld  (0x%016llX)", (long long)v, (unsigned long long)(uint64_t)v);
                        res.display = fmt;
                        break;
                }
                case CType::U8: {
                        uint8_t v;
                        memcpy(&v, retBuf, sizeof(v));
                        res.rawU64 = v;
                        snprintf(fmt, sizeof(fmt), "%u  (0x%02X)", v, v);
                        res.display = fmt;
                        break;
                }
                case CType::U16: {
                        uint16_t v;
                        memcpy(&v, retBuf, sizeof(v));
                        res.rawU64 = v;
                        snprintf(fmt, sizeof(fmt), "%u  (0x%04X)", v, v);
                        res.display = fmt;
                        break;
                }
                case CType::U32: {
                        uint32_t v;
                        memcpy(&v, retBuf, sizeof(v));
                        res.rawU64 = v;
                        snprintf(fmt, sizeof(fmt), "%u  (0x%08X)", v, v);
                        res.display = fmt;
                        break;
                }
                case CType::U64: {
                        uint64_t v;
                        memcpy(&v, retBuf, sizeof(v));
                        res.rawU64 = v;
                        snprintf(fmt, sizeof(fmt), "%llu  (0x%016llX)", (unsigned long long)v, (unsigned long long)v);
                        res.display = fmt;
                        break;
                }
                case CType::Ptr:
                case CType::Unknown:
                default: {
                        void *p;
                        memcpy(&p, retBuf, sizeof(p));
                        res.rawU64 = (uint64_t)(uintptr_t)p;
                        if (p) {
                                snprintf(
                                    fmt, sizeof(fmt), "0x%0*llX", (int)(sizeof(void *) * 2), (unsigned long long)(uintptr_t)p);
                        } else {
                                snprintf(fmt, sizeof(fmt), "NULL");
                        }
                        res.display = fmt;
                        break;
                }
        }

        return res;
}

// ─── export enumeration / symbol matching ─────────────────────────────────────

void
SdkLoader::setExports(const std::vector<ExportedSymbol> &syms)
{
        callCache_->calls.clear();
        callCache_->methodSymbols.clear();
        exports_ = syms;
}

static bool
isIdChar2(char c)
{
        return std::isalnum((unsigned char)c) || c == '_';
}

const std::string *
SdkLoader::findMthetadForMethod(const CMethodDecl &method) const
{
        std::string cacheKey  = method.fullClassName;
        cacheKey             += '\0';
        cacheKey             += method.className;
        cacheKey             += '\0';
        cacheKey             += method.name;
        for (const auto &param : method.params) {
                cacheKey += '\0';
                cacheKey += param.rawType;
                cacheKey += static_cast<char>(param.type);
        }
        if (auto cached = callCache_->methodSymbols.find(cacheKey); cached != callCache_->methodSymbols.end())
                return &exports_[cached->second].mthetad;
        // Build the qualified name we expect to appear in the demthetad symbol
        // Constructor: "ClassName::ClassName"  Destructor: "ClassName::~ClassName"
        std::string qualName  = method.fullClassName + "::" + method.name;
        std::string shortName = method.className + "::" + method.name;

        // Score-based matching: prefer full qualified-name match (scores ≥ 10),
        // then fall back to class-name + method-name match ignoring outer namespace
        // (scores 1–5, used when the DLL was compiled with a different namespace)
        const ExportedSymbol *best      = nullptr;
        int                   bestScore = 0;

        auto scoreAndUpdate = [&](const ExportedSymbol &sym, const std::string &needle, int baseScore) {
                const std::string &dem = sym.demthetad;
                size_t             pos = dem.find(needle);
                if (pos == std::string::npos)
                        return;
                bool boundAfter = (pos + needle.size() >= dem.size() || !isIdChar2(dem[pos + needle.size()]));
                if (!boundAfter)
                        return;

                int score = baseScore;
                if (dem == needle || dem.find(needle + "(") != std::string::npos)
                        score = baseScore + 2;

                // Prefer matching parameter count when we can parse it quickly
                size_t lp = dem.rfind('(');
                size_t rp = dem.rfind(')');
                if (lp != std::string::npos && rp != std::string::npos && rp > lp) {
                        std::string args   = dem.substr(lp + 1, rp - lp - 1);
                        int         depth2 = 0, commas = 0;
                        bool        empty = true;
                        for (char c : args) {
                                if (c == '(' || c == '<')
                                        ++depth2;
                                else if (c == ')' || c == '>')
                                        --depth2;
                                else if (c == ',' && depth2 == 0)
                                        ++commas;
                                if (!std::isspace((unsigned char)c))
                                        empty = false;
                        }
                        size_t demParamCount = empty ? 0 : (size_t)(commas + 1);
                        if (demParamCount == method.params.size() ||
                            (method.isCtor && demParamCount == 0 && method.params.empty()))
                                score += 2;
                }

                if (score > bestScore) {
                        bestScore = score;
                        best      = &sym;
                }
        };

        // First pass: full qualified name (namespace included) — high base score
        for (const auto &sym : exports_)
                scoreAndUpdate(sym, qualName, 10);

        // Second pass: class name only, ignoring outer namespace — lower base score
        // Only runs when the first pass found nothing (DLL compiled with different namespace)
        if (!best && shortName != qualName) {
                for (const auto &sym : exports_)
                        scoreAndUpdate(sym, shortName, 1);
        }

        if (!best)
                return nullptr;
        callCache_->methodSymbols.emplace(std::move(cacheKey), static_cast<size_t>(best - exports_.data()));
        return &best->mthetad;
}

// ─── C++ method call ──────────────────────────────────────────────────────────

CallResult
SdkLoader::callMethod(void                           *thisPtr,
                      const CMethodDecl              &method,
                      const std::vector<std::string> &argStrs,
                      const std::vector<void *>      &structPtrs)
{
        CallResult res;

        // Find the symbol
        const std::string *mthetad = findMthetadForMethod(method);
        if (!mthetad) {
                res.error = "symbol for " + method.fullClassName + "::" + method.name + " not found";
                return res;
        }

        const bool implicitThis = !method.isStatic || method.isCtor || method.isDtor;
        auto      *prepared = prepareCall(*mthetad, method.isCtor ? CType::Void : method.retType, method.params, implicitThis);
        if (!prepared) {
                res.error = prevError_;
                return res;
        }

        // Build the full argument list: [this, param0, param1, ...]
        // Static methods have no hidden `this`; constructors use the raw buffer
        const size_t nUser     = method.params.size();
        const size_t firstUser = implicitThis ? 1 : 0;
        const size_t nAll      = nUser + firstUser;

        // Marshal arguments
        CallArguments arguments(nAll);
        auto         *stores  = arguments.stores;
        auto        **argPtrs = arguments.pointers;

        // Slot 0: `this`
        if (implicitThis) {
                stores[0].v.ptr = thisPtr;
                argPtrs[0]      = &stores[0].v.ptr;
        }

        for (size_t i = 0; i < nUser; ++i) {
                if (i < structPtrs.size() && structPtrs[i] != nullptr) {
                        // Struct-reference param: pass the buffer address as a pointer
                        stores[i + firstUser].v.ptr = structPtrs[i];
                        argPtrs[i + firstUser]      = &stores[i + firstUser].v.ptr;
                } else {
                        const std::string &s = (i < argStrs.size()) ? argStrs[i] : std::string();
                        if (!marshalArg(method.params[i], s, stores[i + firstUser])) {
                                res.error = "failed to marshal arg " + std::to_string(i);
                                return res;
                        }
                        argPtrs[i + firstUser] = &stores[i + firstUser].v;
                }
        }

        alignas(16) uint8_t retBuf[16] = {};
        if (!guardedFfiCall(&prepared->cif, reinterpret_cast<void (*)(void)>(prepared->fn), retBuf, nAll ? argPtrs : nullptr)) {
                res.ok    = false;
                res.error = "SDK method crashed (access violation) — call aborted, app kept alive";
                return res;
        }

        // Format result (reuse the same logic as call())
        res.ok = true;
        char  fmt[128];
        CType rt = method.isCtor ? CType::Void : method.retType;
        switch (rt) {
                case CType::Void:
                        res.display = "(void)";
                        break;
                case CType::F32: {
                        float f;
                        memcpy(&f, retBuf, 4);
                        snprintf(fmt, sizeof(fmt), "%g  (0x%08X)", f, *(uint32_t *)retBuf);
                        res.rawF64  = f;
                        res.display = fmt;
                        break;
                }
                case CType::F64: {
                        double d;
                        memcpy(&d, retBuf, 8);
                        snprintf(fmt, sizeof(fmt), "%g", d);
                        res.rawF64  = d;
                        res.display = fmt;
                        break;
                }
                case CType::I32:
                case CType::Unknown: {
                        int32_t v;
                        memcpy(&v, retBuf, 4);
                        snprintf(fmt, sizeof(fmt), "%d  (0x%08X)", v, (uint32_t)v);
                        res.rawU64  = (uint64_t)(int64_t)v;
                        res.display = fmt;
                        break;
                }
                case CType::I64: {
                        int64_t v;
                        memcpy(&v, retBuf, 8);
                        snprintf(fmt, sizeof(fmt), "%lld  (0x%016llX)", (long long)v, (unsigned long long)(uint64_t)v);
                        res.rawU64  = (uint64_t)v;
                        res.display = fmt;
                        break;
                }
                case CType::U32: {
                        uint32_t v;
                        memcpy(&v, retBuf, 4);
                        snprintf(fmt, sizeof(fmt), "%u  (0x%08X)", v, v);
                        res.rawU64  = v;
                        res.display = fmt;
                        break;
                }
                case CType::U64: {
                        uint64_t v;
                        memcpy(&v, retBuf, 8);
                        snprintf(fmt, sizeof(fmt), "%llu  (0x%016llX)", (unsigned long long)v, (unsigned long long)v);
                        res.rawU64  = v;
                        res.display = fmt;
                        break;
                }
                default: {
                        int64_t v = 0;
                        memcpy(&v, retBuf, std::min((size_t)8, ctypeSize(rt)));
                        snprintf(fmt, sizeof(fmt), "%lld  (0x%llX)", (long long)v, (unsigned long long)(uint64_t)v);
                        res.rawU64  = (uint64_t)v;
                        res.display = fmt;
                        break;
                }
        }
        return res;
}
