#ifndef EXPORT_ENUM_HPP
#define EXPORT_ENUM_HPP

#include <string>
#include <vector>

struct ExportedSymbol {
        std::string mangled;   // raw export name as it appears in the binary
        std::string demangled; // human-readable (equals mangled if demangling fails)
};

// Enumerate all exported function symbols from an already-loaded library.
// handle : opaque handle from LoadLibraryA / dlopen
// path   : on-disk path of the library (used for ELF/Mach-O file parsing)
std::vector<ExportedSymbol> enumerateExports(void *handle, const std::string &path);

// Demangle a single C++ mangled symbol name (platform-specific).
std::string demangleSymbol(const std::string &mangled);

#endif // EXPORT_ENUM_HPP
