#ifndef C_HEADER_PARSER_HPP
#define C_HEADER_PARSER_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

enum class CType { Void, Bool, I8, I16, I32, I64, U8, U16, U32, U64, F32, F64, Ptr, Unknown };

const char *ctypeLabel(CType t);
bool        ctypeIsInteger(CType t);
bool        ctypeIsFloat(CType t);
bool        ctypeIsVoid(CType t);
bool        ctypeIsCharPtr(CType t, const std::string &rawType);

struct CParam {
        std::string name;
        CType       type{CType::Unknown};
        std::string rawType;
};

struct CFuncDecl {
        std::string         name;
        CType               retType{CType::Unknown};
        std::string         retRaw;
        std::vector<CParam> params;
        bool                isVariadic{false};
};

// Parse a C header source string and return all function declarations found.
std::vector<CFuncDecl> parseHeader(const std::string &src);

// ─── type layout helpers ──────────────────────────────────────────────────────

size_t ctypeSize(CType t);  // byte size (0 for Void/Unknown)
size_t ctypeAlign(CType t); // alignment (1 for Void/Unknown)

// ─── C++ types ────────────────────────────────────────────────────────────────

struct CFieldDecl {
        std::string name;
        CType       type{CType::Unknown};
        std::string rawType;
        size_t      offset{0};
        size_t      size{0};
        // std::array<T,N> support
        bool   isArray{false};
        size_t arrayCount{0};
        CType  arrayElemType{CType::Unknown};
};

struct CStructDecl {
        std::string             name;
        std::string             fullName; // namespace-qualified
        std::vector<CFieldDecl> fields;
        size_t                  totalSize{0};
        bool                    isPOD{false}; // all fields have known layout
};

struct CEnumValue {
        std::string name;
        int64_t     value{0};
};

struct CEnumDecl {
        std::string             name;
        std::string             fullName;
        CType                   baseType{CType::I32};
        std::vector<CEnumValue> values;
};

struct CMethodDecl {
        std::string         name;
        std::string         className;
        std::string         fullClassName; // e.g. "AC3::FSA"
        CType               retType{CType::Unknown};
        std::string         retRaw;
        std::vector<CParam> params;
        bool                isStatic{false};
        bool                isCtor{false};
        bool                isDtor{false};
        std::string         inlineBody; // raw body text when defined inline in the header
};

struct CClassDecl {
        std::string              name;
        std::string              fullName;
        std::vector<CMethodDecl> methods;
        std::vector<CStructDecl> innerStructs;
        std::vector<CEnumDecl>   innerEnums;
        size_t                   instanceSize{4096}; // conservative allocation hint
        // All data members in declaration order (public + private), with offsets.
        // Used for inline constructor body emulation.
        std::vector<CFieldDecl> allFields;
};

// Preprocessor macro definition extracted from a #define line.
struct MacroDef {
        bool                     isFunctionLike{false};
        std::vector<std::string> params; // parameter names (function-like only)
        std::string              body;   // expansion body / value text
};

struct ParseResult {
        std::vector<CFuncDecl>   functions; // top-level C-style functions
        std::vector<CClassDecl>  classes;
        std::vector<CStructDecl> structs;
        std::vector<CEnumDecl>   enums;
        // Macro table extracted from #define lines (populated by parseHeaderFull).
        std::unordered_map<std::string, MacroDef> macros;
        // Constexpr/const aggregate variables: varName → {typeName, {initVal0, …}}
        // Used to evaluate expressions like SDK_CONFIG.MIN_M4_VER.
        std::unordered_map<std::string, std::pair<std::string, std::vector<std::string>>> constexprVars;
};

// Parse a C/C++ header and return all declarations (classes, structs, enums, functions).
ParseResult parseHeaderFull(const std::string &src);

// Lookup helpers used by the panel.
const CStructDecl *findStruct(const ParseResult &pr, const std::string &name);
const CEnumDecl   *findEnum(const ParseResult &pr, const std::string &name, const std::string &classFullName = "");

// Evaluate a C integer expression using the macro table from ParseResult.
// Handles decimal/hex literals, bitwise ops, shifts, casts, and macro expansion.
// Returns false only on programmer error; sets result to 0 on parse failure.
bool evalIntExpr(const std::string &expr, const std::unordered_map<std::string, MacroDef> &macros, int64_t &result);

// Emulate an inline constructor or method by applying its body's simple
// field-assignment statements to a pre-allocated object buffer.
// Returns the number of fields successfully written.
int applyInlineBody(void *buf, size_t bufSize, const CClassDecl &cls, const CMethodDecl &method, const ParseResult &pr);

#endif // C_HEADER_PARSER_HPP
