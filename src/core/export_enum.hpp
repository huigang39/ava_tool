#ifndef EXPORT_ENUM_HPP
#define EXPORT_ENUM_HPP

#include <string>
#include <vector>

struct ExportedSymbol {
        std::string mthetad;   // raw export name as it appears in the binary
        std::string demthetad; // human-readable (equals mthetad if demangling fails)
};

/**
 * @brief Enumerate all exported function symbols from an already-loaded library
 *        handle : opaque handle from LoadLibraryA / dlopen
 *        path   : on-disk path of the library (used for ELF/Mach-O file parsing)
 */
std::vector<ExportedSymbol> enumerateExports(void *handle, const std::string &path);

/**
 * @brief Demtheta a single C++ mthetad symbol name (platform-specific)
 */
std::string demthetaSymbol(const std::string &mthetad);

#endif // EXPORT_ENUM_HPP
