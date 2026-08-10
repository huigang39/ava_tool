#ifndef SDK_LOADER_HPP
#define SDK_LOADER_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "core/c_header_parser.hpp"
#include "core/export_enum.hpp"

struct CallResult {
        bool        ok{false};
        std::string display; // formatted return value for the UI
        uint64_t    rawU64{};
        double      rawF64{};
        std::string error;
};

class SdkLoader
{
      public:
        SdkLoader() = default;
        ~SdkLoader() { unload(); }

        SdkLoader(const SdkLoader &)            = delete;
        SdkLoader &operator=(const SdkLoader &) = delete;

        bool        load(const std::string &path);
        void        unload();
        bool        isLoaded() const { return handle_ != nullptr; }
        std::string loadedPath() const { return path_; }

        // ── C function call ──────────────────────────────────────────────────
        // argStrs[i] corresponds to decl.params[i].
        CallResult
        call(const CFuncDecl &decl, const std::vector<std::string> &argStrs, const std::vector<void *> &pointerOverrides = {});

        // ── C++ method call ──────────────────────────────────────────────────
        // thisPtr    : pointer to the class instance (nullptr for static/ctor)
        // method     : parsed method declaration
        // argStrs    : one string per parameter (same order as method.params)
        // structPtrs : parallel to argStrs; non-null entry overrides the arg
        //              with a raw pointer (used for struct-reference parameters)
        CallResult callMethod(void                           *thisPtr,
                              const CMethodDecl              &method,
                              const std::vector<std::string> &argStrs,
                              const std::vector<void *>      &structPtrs);

        // ── Export enumeration ───────────────────────────────────────────────
        // Call after load() to populate the internal symbol table.
        void setExports(const std::vector<ExportedSymbol> &syms);

        // Find the mangled export name that best matches the given method.
        // Returns nullptr if no match is found.
        const std::string *findMangledForMethod(const CMethodDecl &method) const;

        const std::vector<ExportedSymbol> &exports() const { return exports_; }

        std::string lastError() const { return lastError_; }

      private:
        void       *handle_{nullptr};
        std::string path_;
        std::string lastError_;

        std::vector<ExportedSymbol> exports_;

        void *resolve(const std::string &name);
};

#endif // SDK_LOADER_HPP
