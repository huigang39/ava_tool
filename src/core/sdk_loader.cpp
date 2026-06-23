#include "core/sdk_loader.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ffi.h>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define SDK_LOAD(p)   (void *)LoadLibraryA(p)
#define SDK_SYM(h, n) (void *)GetProcAddress((HMODULE)(h), (n))
#define SDK_FREE(h)   FreeLibrary((HMODULE)(h))
#else
#include <dlfcn.h>
#define SDK_LOAD(p)   dlopen((p), RTLD_LAZY | RTLD_LOCAL)
#define SDK_SYM(h, n) dlsym((h), (n))
#define SDK_FREE(h)   dlclose(h)
#endif

// ─── load / unload ────────────────────────────────────────────────────────────

bool
SdkLoader::load(const std::string &path)
{
        unload();
        handle_ = SDK_LOAD(path.c_str());
        if (!handle_) {
#if defined(_WIN32)
                char buf[256];
                snprintf(buf, sizeof(buf), "LoadLibraryA failed (error %lu)", GetLastError());
                lastError_ = buf;
#else
                const char *e = dlerror();
                lastError_    = e ? e : "dlopen failed";
#endif
                return false;
        }
        path_      = path;
        lastError_ = {};
        return true;
}

void
SdkLoader::unload()
{
        if (handle_) {
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
                lastError_ = buf;
#else
                const char *e = dlerror();
                lastError_    = e ? e : ("symbol '" + name + "' not found");
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
                        // Try to parse as a numeric pointer address (e.g. "0x12345678").
                        // If the ENTIRE string is consumed, use it as-is.
                        // Otherwise (e.g. "192.168.137.53", "hello") treat it as a C string —
                        // this covers Windows typedef strings (LPCSTR, LPSTR, PSTR …) that the
                        // header parser doesn't recognise as char* from the raw type name alone.
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
SdkLoader::call(const CFuncDecl &decl, const std::vector<std::string> &argStrs)
{
        CallResult res;

        void *fn = resolve(decl.name);
        if (!fn) {
                res.error = lastError_;
                return res;
        }

        size_t nArgs = decl.params.size();
        if (argStrs.size() < nArgs) {
                res.error = "not enough arguments";
                return res;
        }

        // Build ffi_type arrays
        std::vector<ffi_type *> argTypes(nArgs);
        for (size_t i = 0; i < nArgs; ++i)
                argTypes[i] = toFfiType(decl.params[i].type);

        ffi_cif    cif;
        ffi_status status =
            ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)nArgs, toFfiType(decl.retType), nArgs ? argTypes.data() : nullptr);

        if (status != FFI_OK) {
                res.error = "ffi_prep_cif failed";
                return res;
        }

        // Marshal arguments
        std::vector<ArgStore> stores(nArgs);
        std::vector<void *>   argPtrs(nArgs);
        for (size_t i = 0; i < nArgs; ++i) {
                if (!marshalArg(decl.params[i], argStrs[i], stores[i])) {
                        res.error = "failed to marshal arg " + std::to_string(i);
                        return res;
                }
                argPtrs[i] = &stores[i].v;
        }

        // Return value storage — large enough for any return type.
        // libffi stores small integer returns zero/sign-extended in ffi_arg.
        alignas(16) uint8_t retBuf[16] = {};

        ffi_call(&cif, (void (*)())fn, retBuf, nArgs ? argPtrs.data() : nullptr);

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
        exports_ = syms;
}

static bool
isIdChar2(char c)
{
        return std::isalnum((unsigned char)c) || c == '_';
}

const std::string *
SdkLoader::findMangledForMethod(const CMethodDecl &method) const
{
        // Build the qualified name we expect to appear in the demangled symbol.
        // Constructor: "ClassName::ClassName"  Destructor: "ClassName::~ClassName"
        std::string qualName  = method.fullClassName + "::" + method.name;
        std::string shortName = method.className + "::" + method.name;

        // Score-based matching: prefer full qualified-name match (scores ≥ 10),
        // then fall back to class-name + method-name match ignoring outer namespace
        // (scores 1–5, used when the DLL was compiled with a different namespace).
        const ExportedSymbol *best      = nullptr;
        int                   bestScore = 0;

        auto scoreAndUpdate = [&](const ExportedSymbol &sym, const std::string &needle, int baseScore) {
                const std::string &dem = sym.demangled;
                size_t             pos = dem.find(needle);
                if (pos == std::string::npos)
                        return;
                bool boundAfter = (pos + needle.size() >= dem.size() || !isIdChar2(dem[pos + needle.size()]));
                if (!boundAfter)
                        return;

                int score = baseScore;
                if (dem == needle || dem.find(needle + "(") != std::string::npos)
                        score = baseScore + 2;

                // Prefer matching parameter count when we can parse it quickly.
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

        // First pass: full qualified name (namespace included) — high base score.
        for (const auto &sym : exports_)
                scoreAndUpdate(sym, qualName, 10);

        // Second pass: class name only, ignoring outer namespace — lower base score.
        // Only runs when the first pass found nothing (DLL compiled with different namespace).
        if (!best && shortName != qualName) {
                for (const auto &sym : exports_)
                        scoreAndUpdate(sym, shortName, 1);
        }

        return best ? &best->mangled : nullptr;
}

// ─── C++ method call ──────────────────────────────────────────────────────────

CallResult
SdkLoader::callMethod(void                           *thisPtr,
                      const CMethodDecl              &method,
                      const std::vector<std::string> &argStrs,
                      const std::vector<void *>      &structPtrs)
{
        CallResult res;

        // Find the symbol.
        const std::string *mangled = findMangledForMethod(method);
        if (!mangled) {
                res.error = "symbol for " + method.fullClassName + "::" + method.name + " not found";
                return res;
        }

        void *fn = resolve(*mangled);
        if (!fn) {
                res.error = lastError_;
                return res;
        }

        // Build the full argument list: [this, param0, param1, ...]
        // Static methods and constructors (called on a raw buffer) still pass `this`.
        const size_t nUser = method.params.size();
        const size_t nAll  = nUser + 1; // +1 for hidden `this`

        std::vector<ffi_type *> argTypes(nAll);
        argTypes[0] = &ffi_type_pointer; // `this`
        for (size_t i = 0; i < nUser; ++i)
                argTypes[i + 1] = toFfiType(method.params[i].type);

        ffi_cif    cif;
        ffi_type  *retFfi = method.isCtor ? &ffi_type_void : toFfiType(method.retType);
        ffi_status status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)nAll, retFfi, argTypes.data());
        if (status != FFI_OK) {
                res.error = "ffi_prep_cif failed for method call";
                return res;
        }

        // Marshal arguments.
        std::vector<ArgStore> stores(nAll);
        std::vector<void *>   argPtrs(nAll);

        // Slot 0: `this`
        stores[0].v.ptr = thisPtr;
        argPtrs[0]      = &stores[0].v.ptr;

        for (size_t i = 0; i < nUser; ++i) {
                if (i < structPtrs.size() && structPtrs[i] != nullptr) {
                        // Struct-reference param: pass the buffer address as a pointer.
                        stores[i + 1].v.ptr = structPtrs[i];
                        argPtrs[i + 1]      = &stores[i + 1].v.ptr;
                } else {
                        const std::string &s = (i < argStrs.size()) ? argStrs[i] : std::string();
                        if (!marshalArg(method.params[i], s, stores[i + 1])) {
                                res.error = "failed to marshal arg " + std::to_string(i);
                                return res;
                        }
                        argPtrs[i + 1] = &stores[i + 1].v;
                }
        }

        alignas(16) uint8_t retBuf[16] = {};
        ffi_call(&cif, reinterpret_cast<void (*)()>(fn), retBuf, argPtrs.data());

        // Format result (reuse the same logic as call()).
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
