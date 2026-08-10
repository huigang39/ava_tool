#include "core/c_header_parser.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ─── helpers ──────────────────────────────────────────────────────────────────

static std::string
trim(const std::string &s)
{
        auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
                return "";
        auto e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
}

static std::string
collapseSpaces(const std::string &s)
{
        std::string r;
        r.reserve(s.size());
        bool sp = false;
        for (char c : s) {
                if (std::isspace((unsigned char)c)) {
                        if (!sp && !r.empty()) {
                                r  += ' ';
                                sp  = true;
                        }
                } else {
                        r  += c;
                        sp  = false;
                }
        }
        return trim(r);
}

static bool
isIdChar(char c)
{
        return std::isalnum((unsigned char)c) || c == '_';
}

// Strip // and /* */ comments, preserving newlines.
static std::string
stripComments(const std::string &src)
{
        std::string out;
        out.reserve(src.size());
        size_t i = 0;
        while (i < src.size()) {
                if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '/') {
                        while (i < src.size() && src[i] != '\n')
                                ++i;
                } else if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '*') {
                        i += 2;
                        while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) {
                                if (src[i] == '\n')
                                        out += '\n';
                                ++i;
                        }
                        i += 2;
                } else if (src[i] == '"') {
                        // Skip string literals so we don't mistake quoted text for code.
                        out += src[i++];
                        while (i < src.size() && src[i] != '"') {
                                if (src[i] == '\\' && i + 1 < src.size())
                                        out += src[i++];
                                if (i < src.size())
                                        out += src[i++];
                        }
                        if (i < src.size())
                                out += src[i++];
                } else {
                        out += src[i++];
                }
        }
        return out;
}

// Remove preprocessor lines (#...) including line-continuation backslashes.
static std::string
stripPreprocessor(const std::string &src)
{
        std::string out;
        out.reserve(src.size());
        size_t i = 0;
        while (i < src.size()) {
                size_t lineStart = i;
                while (i < src.size() && (src[i] == ' ' || src[i] == '\t'))
                        ++i;
                if (i < src.size() && src[i] == '#') {
                        while (i < src.size()) {
                                if (src[i] == '\\' && i + 1 < src.size() && src[i + 1] == '\n') {
                                        out += '\n';
                                        i   += 2;
                                } else if (src[i] == '\n') {
                                        out += '\n';
                                        ++i;
                                        break;
                                } else {
                                        ++i;
                                }
                        }
                } else {
                        i = lineStart;
                        while (i < src.size() && src[i] != '\n')
                                out += src[i++];
                        if (i < src.size())
                                out += src[i++];
                }
        }
        return out;
}

// Remove `extern "C" { ... }` wrapper braces while keeping content inside.
static std::string
stripExternCBraces(const std::string &src)
{
        std::string out;
        size_t      i = 0;
        while (i < src.size()) {
                // Match: extern[ws]"C"[ws]{
                if (i + 6 <= src.size() && src.substr(i, 6) == "extern") {
                        size_t j = i + 6;
                        while (j < src.size() && std::isspace((unsigned char)src[j]))
                                ++j;
                        if (j + 3 <= src.size() && src[j] == '"' && src[j + 1] == 'C' && src[j + 2] == '"') {
                                j += 3;
                                while (j < src.size() && std::isspace((unsigned char)src[j]))
                                        ++j;
                                if (j < src.size() && src[j] == '{') {
                                        ++j; // consume '{'
                                        // Find matching '}' and include inner content, dropping just the braces.
                                        int depth = 1;
                                        while (j < src.size() && depth > 0) {
                                                if (src[j] == '{')
                                                        ++depth;
                                                else if (src[j] == '}') {
                                                        --depth;
                                                        if (depth == 0) {
                                                                ++j;
                                                                break;
                                                        }
                                                }
                                                out += src[j++];
                                        }
                                        i = j;
                                        continue;
                                }
                        }
                }
                out += src[i++];
        }
        return out;
}

// ─── type normalization ────────────────────────────────────────────────────────

static const std::unordered_map<std::string, CType> kTypeTable = {
    {"void", CType::Void},
    {"bool", CType::Bool},
    {"_Bool", CType::Bool},
    {"char", CType::I8},
    {"int8_t", CType::I8},
    {"signed char", CType::I8},
    {"short", CType::I16},
    {"short int", CType::I16},
    {"int16_t", CType::I16},
    {"signed short", CType::I16},
    {"signed short int", CType::I16},
    {"int", CType::I32},
    {"int32_t", CType::I32},
    {"signed", CType::I32},
    {"signed int", CType::I32},
    {"long", sizeof(long) == 8 ? CType::I64 : CType::I32},
    {"long int", sizeof(long) == 8 ? CType::I64 : CType::I32},
    {"long long", CType::I64},
    {"long long int", CType::I64},
    {"int64_t", CType::I64},
    {"signed long long", CType::I64},
    {"signed long long int", CType::I64},
    {"unsigned char", CType::U8},
    {"uint8_t", CType::U8},
    {"unsigned short", CType::U16},
    {"unsigned short int", CType::U16},
    {"uint16_t", CType::U16},
    {"unsigned", CType::U32},
    {"unsigned int", CType::U32},
    {"uint32_t", CType::U32},
    {"unsigned long", sizeof(unsigned long) == 8 ? CType::U64 : CType::U32},
    {"unsigned long int", sizeof(unsigned long) == 8 ? CType::U64 : CType::U32},
    {"unsigned long long", CType::U64},
    {"unsigned long long int", CType::U64},
    {"uint64_t", CType::U64},
    {"size_t", CType::U64},
    {"ptrdiff_t", CType::I64},
    {"float", CType::F32},
    {"double", CType::F64},
};

static CType
normalizeType(const std::string &raw)
{
        if (raw.find('*') != std::string::npos || raw.find('[') != std::string::npos)
                return CType::Ptr;

        static const char *quals[] = {
            "const", "volatile", "restrict", "extern", "static", "inline", "__cdecl", "__stdcall", "__fastcall", nullptr};
        std::string s = raw;
        for (int q = 0; quals[q]; ++q) {
                std::string qw(quals[q]);
                size_t      pos = 0;
                while ((pos = s.find(qw, pos)) != std::string::npos) {
                        bool   before = (pos == 0 || !isIdChar(s[pos - 1]));
                        size_t end    = pos + qw.size();
                        bool   after  = (end >= s.size() || !isIdChar(s[end]));
                        if (before && after)
                                s.replace(pos, qw.size(), " ");
                        else
                                pos += qw.size();
                }
        }
        s       = collapseSpaces(s);
        auto it = kTypeTable.find(s);
        return it != kTypeTable.end() ? it->second : CType::Unknown;
}

// ─── split "rettype funcname" ──────────────────────────────────────────────────

static const char *kKeywords[] = {"int",      "char",   "void",   "float",    "double", "short", "long",
                                  "unsigned", "signed", "const",  "volatile", "struct", "enum",  "union",
                                  "typedef",  "extern", "static", "inline",   "return", "bool",  nullptr};

static bool
isKeyword(const std::string &w)
{
        for (int i = 0; kKeywords[i]; ++i)
                if (w == kKeywords[i])
                        return true;
        return false;
}

static bool
splitReturnAndName(const std::string &s, std::string &retRaw, std::string &name)
{
        int i = (int)s.size() - 1;
        while (i >= 0 && std::isspace((unsigned char)s[i]))
                --i;
        if (i < 0 || !isIdChar(s[i]))
                return false;

        int nameEnd = i;
        while (i >= 0 && isIdChar(s[i]))
                --i;
        int nameStart = i + 1;

        name   = s.substr(nameStart, nameEnd - nameStart + 1);
        retRaw = trim(s.substr(0, nameStart));

        if (name.empty() || std::isdigit((unsigned char)name[0]))
                return false;
        if (isKeyword(name))
                return false;
        if (retRaw.empty())
                return false;

        return true;
}

// ─── parameter list ────────────────────────────────────────────────────────────

static std::vector<CParam>
parseParams(const std::string &paramStr, bool *isVariadic = nullptr)
{
        std::vector<CParam> out;
        std::string         s = trim(paramStr);
        if (isVariadic)
                *isVariadic = false;
        if (s == "...") {
                if (isVariadic)
                        *isVariadic = true;
                return out;
        }
        if (s.empty() || s == "void")
                return out;

        std::vector<std::string> parts;
        {
                int    depth = 0;
                size_t start = 0;
                for (size_t k = 0; k < s.size(); ++k) {
                        if (s[k] == '(' || s[k] == '[')
                                ++depth;
                        else if (s[k] == ')' || s[k] == ']')
                                --depth;
                        else if (s[k] == ',' && depth == 0) {
                                parts.push_back(trim(s.substr(start, k - start)));
                                start = k + 1;
                        }
                }
                parts.push_back(trim(s.substr(start)));
        }

        for (auto &p : parts) {
                if (p.empty())
                        continue;
                if (p == "...") {
                        if (isVariadic)
                                *isVariadic = true;
                        continue;
                }
                CParam      param;
                std::string retRaw, pname;
                if (splitReturnAndName(p, retRaw, pname)) {
                        param.name    = pname;
                        param.rawType = retRaw;
                } else {
                        param.rawType = p;
                }
                // Move leading '*' from name back to type (e.g. "int *p" parsed as "*p")
                while (!param.name.empty() && param.name.front() == '*') {
                        param.rawType += '*';
                        param.name     = param.name.substr(1);
                }
                param.type = normalizeType(param.rawType);
                out.push_back(std::move(param));
        }
        return out;
}

// ─── main entry point ─────────────────────────────────────────────────────────

std::vector<CFuncDecl>
parseHeader(const std::string &src)
{
        std::string s = stripComments(src);
        s             = stripPreprocessor(s);
        s             = stripExternCBraces(s);

        // Collect ';' positions at brace depth 0.
        // Brace depth > 0 means we're inside struct/union/enum/function bodies.
        std::vector<size_t> semis;
        {
                int depth = 0;
                for (size_t k = 0; k < s.size(); ++k) {
                        if (s[k] == '{')
                                ++depth;
                        else if (s[k] == '}') {
                                if (depth > 0)
                                        --depth;
                        } else if (s[k] == ';' && depth == 0)
                                semis.push_back(k);
                }
        }

        std::vector<CFuncDecl> result;
        size_t                 prev = 0;
        for (size_t semi : semis) {
                std::string decl = trim(s.substr(prev, semi - prev));
                prev             = semi + 1;

                if (decl.empty())
                        continue;

                // Skip keywords that begin struct/union/enum/typedef declarations.
                {
                        std::string        first;
                        std::istringstream ss(decl);
                        ss >> first;
                        if (first == "typedef" || first == "struct" || first == "union" || first == "enum")
                                continue;
                }

                // Must contain '(' to be a function declaration.
                size_t lp = decl.find('(');
                if (lp == std::string::npos)
                        continue;

                // Find the matching closing ')' at depth 0 from lp.
                size_t rp    = std::string::npos;
                int    depth = 0;
                for (size_t k = lp; k < decl.size(); ++k) {
                        if (decl[k] == '(')
                                ++depth;
                        else if (decl[k] == ')') {
                                --depth;
                                if (depth == 0) {
                                        rp = k;
                                        break;
                                }
                        }
                }
                if (rp == std::string::npos)
                        continue;

                std::string beforeParen = trim(decl.substr(0, lp));
                std::string paramStr    = trim(decl.substr(lp + 1, rp - lp - 1));

                // Skip if the return-type area contains a function pointer indicator.
                if (beforeParen.find("(*") != std::string::npos)
                        continue;

                std::string retRaw, funcName;
                if (!splitReturnAndName(beforeParen, retRaw, funcName))
                        continue;

                CFuncDecl fd;
                fd.name    = std::move(funcName);
                fd.retRaw  = retRaw;
                fd.retType = normalizeType(retRaw);
                fd.params  = parseParams(paramStr, &fd.isVariadic);
                result.push_back(std::move(fd));
        }
        return result;
}

// ─── type helpers ─────────────────────────────────────────────────────────────

const char *
ctypeLabel(CType t)
{
        switch (t) {
                case CType::Void:
                        return "void";
                case CType::Bool:
                        return "bool";
                case CType::I8:
                        return "int8";
                case CType::I16:
                        return "int16";
                case CType::I32:
                        return "int32";
                case CType::I64:
                        return "int64";
                case CType::U8:
                        return "uint8";
                case CType::U16:
                        return "uint16";
                case CType::U32:
                        return "uint32";
                case CType::U64:
                        return "uint64";
                case CType::F32:
                        return "float";
                case CType::F64:
                        return "double";
                case CType::Ptr:
                        return "ptr";
                case CType::Unknown:
                        return "?";
        }
        return "?";
}

bool
ctypeIsInteger(CType t)
{
        return t == CType::Bool || t == CType::I8 || t == CType::I16 || t == CType::I32 || t == CType::I64 || t == CType::U8 ||
               t == CType::U16 || t == CType::U32 || t == CType::U64;
}
bool
ctypeIsFloat(CType t)
{
        return t == CType::F32 || t == CType::F64;
}
bool
ctypeIsVoid(CType t)
{
        return t == CType::Void;
}

bool
ctypeIsCharPtr(CType t, const std::string &rawType)
{
        if (t != CType::Ptr)
                return false;
        if (rawType.find("char") != std::string::npos)
                return true;
        // Windows string typedef aliases
        static const char *aliases[] = {
            "LPSTR",
            "LPCSTR",
            "LPWSTR",
            "LPCWSTR",
            "LPTSTR",
            "LPCTSTR",
            "PSTR",
            "PCSTR",
            "PWSTR",
            "PCWSTR",
            "PTSTR",
            "PCTSTR",
            "BSTR",
            "OLECHAR",
        };
        for (const char *a : aliases)
                if (rawType.find(a) != std::string::npos)
                        return true;
        return false;
}

// ─── type size / alignment ────────────────────────────────────────────────────

size_t
ctypeSize(CType t)
{
        switch (t) {
                case CType::Bool:
                case CType::I8:
                case CType::U8:
                        return 1;
                case CType::I16:
                case CType::U16:
                        return 2;
                case CType::I32:
                case CType::U32:
                case CType::F32:
                        return 4;
                case CType::I64:
                case CType::U64:
                case CType::F64:
                        return 8;
                case CType::Ptr:
                case CType::Unknown:
                        return sizeof(void *);
                case CType::Void:
                default:
                        return 0;
        }
}

size_t
ctypeAlign(CType t)
{
        size_t sz = ctypeSize(t);
        return (sz == 0) ? 1 : sz;
}

// Also add & → Ptr to normalizeType (patch applied inline below via separate function).
// normalizeType is already defined above; we replicate its updated version:

static CType
normalizeTypeCpp(const std::string &raw)
{
        if (raw.find('*') != std::string::npos || raw.find('[') != std::string::npos || raw.find('&') != std::string::npos)
                return CType::Ptr;

        static const char *quals[] = {
            "const", "volatile", "restrict", "extern", "static", "inline", "__cdecl", "__stdcall", "__fastcall", nullptr};
        std::string s = raw;
        for (int q = 0; quals[q]; ++q) {
                std::string qw(quals[q]);
                size_t      pos = 0;
                while ((pos = s.find(qw, pos)) != std::string::npos) {
                        bool   before = (pos == 0 || !isIdChar(s[pos - 1]));
                        size_t end    = pos + qw.size();
                        bool   after  = (end >= s.size() || !isIdChar(s[end]));
                        if (before && after)
                                s.replace(pos, qw.size(), " ");
                        else
                                pos += qw.size();
                }
        }
        s       = collapseSpaces(s);
        auto it = kTypeTable.find(s);
        return it != kTypeTable.end() ? it->second : CType::Unknown;
}

// Strip "= default_value" from a parameter string (handles depth of parentheses).
static std::string
stripDefault(const std::string &param)
{
        int depth = 0;
        for (size_t i = 0; i < param.size(); ++i) {
                char c = param[i];
                if (c == '(')
                        ++depth;
                else if (c == ')')
                        --depth;
                else if (c == '=' && depth == 0)
                        return trim(param.substr(0, i));
        }
        return param;
}

// ─── C++ parser helpers ───────────────────────────────────────────────────────

static size_t
findMatchingBrace(const std::string &s, size_t openPos)
{
        int    depth = 1;
        size_t i     = openPos + 1;
        while (i < s.size()) {
                if (s[i] == '{')
                        ++depth;
                else if (s[i] == '}') {
                        if (--depth == 0)
                                return i;
                }
                ++i;
        }
        return std::string::npos;
}

// Replace every character inside {..} blocks with a space (preserves newlines).
// The closing '}' is replaced with ';' so that inline method bodies act as
// natural ';'-terminators for the declaration that precedes them:
//   FSA() { body }  →  FSA()        ;
// Without this, FSA() and ~FSA() (which has its own ';') would merge into
// one chunk and the destructor would never be parsed.
static std::string
stripBlocks(const std::string &text)
{
        std::string out = text;
        size_t      i   = 0;
        while (i < out.size()) {
                if (out[i] == '{') {
                        size_t cl = findMatchingBrace(out, i);
                        if (cl == std::string::npos) {
                                ++i;
                                continue;
                        }
                        for (size_t k = i; k <= cl; ++k) {
                                if (out[k] != '\n')
                                        out[k] = ' ';
                        }
                        out[cl] = ';'; // closing brace acts as declaration terminator
                        i       = cl + 1;
                } else {
                        ++i;
                }
        }
        return out;
}

// Scope type for the recursive scope parser.
enum class ScopeKind { Unknown, Namespace, Class, Struct, Enum };
struct ScopeInfo {
        ScopeKind   kind{ScopeKind::Unknown};
        std::string name;
        std::string baseType;               // enum class : <baseType>
        bool        isPublicDefault{false}; // true for struct, false for class
};

// Tokenize text into identifier tokens and single-character tokens.
static std::vector<std::string>
tokenize(const std::string &s)
{
        std::vector<std::string> toks;
        for (size_t i = 0; i < s.size();) {
                if (std::isspace((unsigned char)s[i])) {
                        ++i;
                        continue;
                }
                if (isIdChar(s[i])) {
                        size_t start = i;
                        while (i < s.size() && isIdChar(s[i]))
                                ++i;
                        toks.push_back(s.substr(start, i - start));
                } else {
                        toks.push_back(std::string(1, s[i++]));
                }
        }
        return toks;
}

// Classify what type of scope a block-preamble introduces.
// preamble = all text between the previous '};' and the opening '{'.
static ScopeInfo
classifyPreamble(const std::string &preamble)
{
        ScopeInfo info;

        // Take the text after the LAST ';' in the preamble (that's the new declaration).
        size_t      lastSemi = preamble.rfind(';');
        std::string stmt     = trim(lastSemi == std::string::npos ? preamble : preamble.substr(lastSemi + 1));
        stmt                 = collapseSpaces(stmt);

        auto toks = tokenize(stmt);

        for (size_t idx = 0; idx < toks.size(); ++idx) {
                if (toks[idx] == "namespace") {
                        info.kind = ScopeKind::Namespace;
                        if (idx + 1 < toks.size() && isIdChar(toks[idx + 1][0]))
                                info.name = toks[idx + 1];
                        return info;
                }
                if (toks[idx] == "enum") {
                        info.kind = ScopeKind::Enum;
                        size_t ni = idx + 1;
                        if (ni < toks.size() && toks[ni] == "class")
                                ++ni; // skip "class" in "enum class"
                        if (ni < toks.size() && !toks[ni].empty() && isIdChar(toks[ni][0]))
                                info.name = toks[ni];
                        // look for ": type" after the name
                        for (size_t k = ni + 1; k + 1 < toks.size(); ++k) {
                                if (toks[k] == ":" && (k == 0 || toks[k - 1] != ":")) {
                                        if (isIdChar(toks[k + 1][0]))
                                                info.baseType = toks[k + 1];
                                        break;
                                }
                        }
                        return info;
                }
                if (toks[idx] == "struct" || toks[idx] == "class") {
                        // Guard: don't trigger on "enum class" (we'd have seen "enum" first)
                        info.kind            = toks[idx] == "struct" ? ScopeKind::Struct : ScopeKind::Class;
                        info.isPublicDefault = toks[idx] == "struct";
                        // The class/struct name is the LAST identifier before the base-clause
                        // ':' (or end of preamble), skipping any leading export/attribute
                        // macros such as `class NEOFSA_API NeoFSA` or `struct __declspec(...) S`.
                        for (size_t k = idx + 1; k < toks.size(); ++k) {
                                if (toks[k] == ":") // start of base-clause → name already found
                                        break;
                                if (!toks[k].empty() && isIdChar(toks[k][0]) && toks[k] != "public" && toks[k] != "private" &&
                                    toks[k] != "protected" && toks[k] != "virtual" && toks[k] != "final")
                                        info.name = toks[k];
                        }
                        return info;
                }
        }
        return info;
}

// ─── enum body parser ─────────────────────────────────────────────────────────

static CEnumDecl
parseEnumContent(const std::string &body, const std::string &name, const std::string &fullName, CType baseType)
{
        CEnumDecl ed;
        ed.name     = name;
        ed.fullName = fullName;
        ed.baseType = baseType;

        int64_t nextVal = 0;
        size_t  start   = 0;
        bool    done    = false;

        auto flush = [&](const std::string &part) {
                std::string p = trim(part);
                if (p.empty())
                        return;
                CEnumValue val;
                size_t     eq = p.find('=');
                if (eq != std::string::npos) {
                        val.name        = trim(p.substr(0, eq));
                        std::string vs  = trim(p.substr(eq + 1));
                        char       *end = nullptr;
                        int64_t     v   = (vs.size() > 2 && vs[0] == '0' && (vs[1] == 'x' || vs[1] == 'X'))
                                              ? (int64_t)strtoull(vs.c_str(), &end, 16)
                                              : strtoll(vs.c_str(), &end, 10);
                        val.value       = v;
                        nextVal         = v + 1;
                } else {
                        val.name  = p;
                        val.value = nextVal++;
                }
                if (!val.name.empty() && (std::isalpha((unsigned char)val.name[0]) || val.name[0] == '_'))
                        ed.values.push_back(val);
        };

        for (size_t k = 0; k <= body.size() && !done; ++k) {
                if (k == body.size()) {
                        flush(body.substr(start));
                        break;
                }
                if (body[k] == ',') {
                        flush(body.substr(start, k - start));
                        start = k + 1;
                }
        }
        return ed;
}

// ─── struct body parser ───────────────────────────────────────────────────────

static CStructDecl
parseStructContent(const std::string &body, const std::string &name, const std::string &fullName)
{
        CStructDecl sd;
        sd.name     = name;
        sd.fullName = fullName;
        sd.isPOD    = true;

        size_t offset   = 0;
        size_t maxAlign = 1;

        // Collect ';'-terminated field declarations at brace depth 0.
        std::vector<std::string> fieldDecls;
        {
                int    depth = 0;
                size_t st    = 0;
                for (size_t k = 0; k < body.size(); ++k) {
                        if (body[k] == '{')
                                ++depth;
                        else if (body[k] == '}') {
                                if (depth > 0)
                                        --depth;
                        } else if (body[k] == ';' && depth == 0) {
                                fieldDecls.push_back(trim(body.substr(st, k - st)));
                                st = k + 1;
                        }
                }
        }

        for (const auto &fd : fieldDecls) {
                if (fd.empty())
                        continue;

                // Skip nested type definitions.
                {
                        std::string        first;
                        std::istringstream ss(fd);
                        ss >> first;
                        if (first == "struct" || first == "class" || first == "union" || first == "enum" || first == "typedef")
                                continue;
                }

                // Handle std::array<T, N> or array<T, N>
                size_t arrayKw = fd.find("array");
                if (arrayKw != std::string::npos) {
                        size_t lt = fd.find('<', arrayKw);
                        size_t gt = fd.rfind('>');
                        if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
                                std::string templ = fd.substr(lt + 1, gt - lt - 1);
                                size_t      comma = templ.rfind(',');
                                if (comma != std::string::npos) {
                                        std::string elemStr  = trim(templ.substr(0, comma));
                                        std::string countStr = trim(templ.substr(comma + 1));
                                        CType       elemType = normalizeTypeCpp(elemStr);
                                        if (elemType == CType::Unknown || elemType == CType::Void) {
                                                sd.isPOD = false;
                                                continue;
                                        }
                                        size_t count = (size_t)strtoull(countStr.c_str(), nullptr, 10);
                                        if (count == 0) {
                                                sd.isPOD = false;
                                                continue;
                                        }

                                        // Field name comes after '>'
                                        std::string afterGt = trim(fd.substr(gt + 1));
                                        if (afterGt.empty())
                                                continue;
                                        std::string fname, fretRaw;
                                        if (!splitReturnAndName(afterGt, fretRaw, fname)) {
                                                fname = afterGt;
                                        }

                                        size_t esz = ctypeSize(elemType);
                                        size_t eal = ctypeAlign(elemType);
                                        offset     = (offset + eal - 1) & ~(eal - 1);

                                        CFieldDecl f;
                                        f.name           = fname;
                                        f.rawType        = fd.substr(0, gt + 1);
                                        f.type           = CType::Ptr; // treat array as "opaque ptr-like"
                                        f.isArray        = true;
                                        f.arrayCount     = count;
                                        f.arrayElemType  = elemType;
                                        f.offset         = offset;
                                        f.size           = esz * count;
                                        offset          += f.size;
                                        if (eal > maxAlign)
                                                maxAlign = eal;
                                        sd.fields.push_back(f);
                                }
                        }
                        continue;
                }

                // Skip fields with complex C++ types.
                if (fd.find("std::") != std::string::npos) {
                        sd.isPOD = false;
                        continue;
                }

                // Simple primitive field: "type name"
                std::string retRaw, fname;
                if (!splitReturnAndName(fd, retRaw, fname))
                        continue;

                CType ftype = normalizeTypeCpp(retRaw);
                if (ftype == CType::Unknown || ftype == CType::Void) {
                        sd.isPOD = false;
                        continue;
                }

                size_t fsz = ctypeSize(ftype);
                size_t fal = ctypeAlign(ftype);
                if (fsz == 0) {
                        sd.isPOD = false;
                        continue;
                }

                offset = (offset + fal - 1) & ~(fal - 1);

                CFieldDecl f;
                f.name     = fname;
                f.rawType  = retRaw;
                f.type     = ftype;
                f.offset   = offset;
                f.size     = fsz;
                offset    += fsz;
                if (fal > maxAlign)
                        maxAlign = fal;
                sd.fields.push_back(f);
        }

        // Round total size up to struct alignment.
        if (maxAlign > 0 && offset > 0)
                sd.totalSize = (offset + maxAlign - 1) & ~(maxAlign - 1);
        else
                sd.totalSize = offset;

        return sd;
}

// ─── method declaration parser ────────────────────────────────────────────────

static bool
parseMethodDecl(const std::string &rawDecl, const std::string &className, const std::string &fullClassName, CMethodDecl &method)
{
        std::string decl = collapseSpaces(trim(rawDecl));
        if (decl.empty())
                return false;

        // Skip deleted/defaulted functions.
        if (decl.find("= delete") != std::string::npos)
                return false;
        if (decl.find("= default") != std::string::npos)
                return false;

        // Remove pure-virtual marker.
        if (decl.size() >= 3 && decl.back() == '0') {
                size_t ep = decl.rfind("= 0");
                if (ep != std::string::npos)
                        decl = trim(decl.substr(0, ep));
        }

        // Strip trailing modifiers after the last ')'.
        auto stripTrailing = [&](const std::string &kw) {
                size_t rp = decl.rfind(')');
                if (rp == std::string::npos)
                        return;
                std::string after = trim(decl.substr(rp + 1));
                if (after == kw)
                        decl = trim(decl.substr(0, rp + 1));
        };
        stripTrailing("const");
        stripTrailing("noexcept");
        stripTrailing("override");
        stripTrailing("final");

        // Strip leading modifiers.
        bool isStatic = false;
        for (bool changed = true; changed;) {
                changed = false;
                for (const char *kw : {"virtual", "static", "explicit", "inline", "friend"}) {
                        std::string kwStr(kw);
                        if (decl.size() > kwStr.size() + 1 && decl.substr(0, kwStr.size()) == kwStr &&
                            std::isspace((unsigned char)decl[kwStr.size()])) {
                                if (kwStr == "static")
                                        isStatic = true;
                                decl    = trim(decl.substr(kwStr.size()));
                                changed = true;
                                break;
                        }
                }
        }

        // Must contain '('.
        size_t lp = decl.find('(');
        if (lp == std::string::npos)
                return false;

        // Find matching ')'.
        size_t rp = std::string::npos;
        {
                int d = 0;
                for (size_t k = lp; k < decl.size(); ++k) {
                        if (decl[k] == '(')
                                ++d;
                        else if (decl[k] == ')') {
                                if (--d == 0) {
                                        rp = k;
                                        break;
                                }
                        }
                }
        }
        if (rp == std::string::npos)
                return false;

        std::string beforeParen = trim(decl.substr(0, lp));
        std::string paramStr    = trim(decl.substr(lp + 1, rp - lp - 1));

        // ── Destructor ──
        if (!beforeParen.empty() && beforeParen[0] == '~') {
                method.isDtor        = true;
                method.name          = beforeParen;
                method.className     = className;
                method.fullClassName = fullClassName;
                method.retType       = CType::Void;
                return true;
        }

        // ── Constructor ── (beforeParen == className or ends with className)
        // Strip any namespace prefix to get just the simple name.
        {
                std::string simple = beforeParen;
                size_t      ns     = simple.rfind("::");
                if (ns != std::string::npos)
                        simple = simple.substr(ns + 2);
                if (simple == className) {
                        method.isCtor        = true;
                        method.name          = className;
                        method.className     = className;
                        method.fullClassName = fullClassName;
                        method.retType       = CType::Void;
                        // Strip defaults and parse params
                        std::string ps = paramStr;
                        method.params  = parseParams(ps);
                        return true;
                }
        }

        // ── Normal method ──
        std::string retRaw, funcName;
        if (!splitReturnAndName(beforeParen, retRaw, funcName))
                return false;
        if (funcName.empty() || isKeyword(funcName))
                return false;

        method.name          = funcName;
        method.className     = className;
        method.fullClassName = fullClassName;
        method.retRaw        = retRaw;
        method.retType       = normalizeTypeCpp(retRaw);
        method.isStatic      = isStatic;

        // Parse params with default-value stripping.
        std::vector<std::string> rawParts;
        {
                int    depth2 = 0;
                size_t st     = 0;
                for (size_t k = 0; k < paramStr.size(); ++k) {
                        if (paramStr[k] == '(' || paramStr[k] == '[')
                                ++depth2;
                        else if (paramStr[k] == ')' || paramStr[k] == ']')
                                --depth2;
                        else if (paramStr[k] == ',' && depth2 == 0) {
                                rawParts.push_back(stripDefault(trim(paramStr.substr(st, k - st))));
                                st = k + 1;
                        }
                }
                if (st < paramStr.size())
                        rawParts.push_back(stripDefault(trim(paramStr.substr(st))));
        }
        for (const auto &rp2 : rawParts) {
                if (rp2.empty() || rp2 == "void" || rp2 == "...")
                        continue;
                CParam      p;
                std::string pret, pname;
                if (splitReturnAndName(rp2, pret, pname)) {
                        p.name    = pname;
                        p.rawType = pret;
                } else {
                        p.rawType = rp2;
                }
                while (!p.name.empty() && (p.name.front() == '*' || p.name.front() == '&')) {
                        p.rawType += p.name.front();
                        p.name     = p.name.substr(1);
                }
                p.type = normalizeTypeCpp(p.rawType);
                method.params.push_back(std::move(p));
        }
        return true;
}

// ─── field declaration parser (for class allFields) ──────────────────────────

// Parse one field declaration string into a CFieldDecl and update the running
// byte offset.  Handles std::array<T,N>, pointers, and plain scalar types.
static void
parseOneField(const std::string &rawPart, std::vector<CFieldDecl> &fields, size_t &offset)
{
        std::string part = stripDefault(trim(rawPart));
        // Strip leading non-type qualifiers.
        for (const char *kw :
             {"static", "constexpr", "const", "volatile", "mutable", "friend", "extern", "inline", "explicit"}) {
                std::string kws(kw);
                kws += ' ';
                for (bool changed = true; changed;) {
                        changed = false;
                        if (part.size() > kws.size() && part.substr(0, kws.size()) == kws) {
                                part    = trim(part.substr(kws.size()));
                                changed = true;
                        }
                }
        }
        if (part.empty())
                return;
        // Skip type declarations and function-pointer lines.
        for (const char *kw : {"struct ", "enum ", "class ", "union ", "typedef ", "using "}) {
                if (part.size() >= strlen(kw) && part.substr(0, strlen(kw)) == kw)
                        return;
        }
        if (part.find("operator") != std::string::npos)
                return;

        // Handle std::array<T, N> name  /  array<T, N> name
        if (part.find("array<") != std::string::npos) {
                size_t lt = part.find('<'), gt = part.rfind('>');
                if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
                        std::string inner = part.substr(lt + 1, gt - lt - 1);
                        size_t      comma = inner.rfind(',');
                        if (comma != std::string::npos) {
                                std::string et  = trim(inner.substr(0, comma));
                                size_t      cnt = (size_t)std::strtoul(trim(inner.substr(comma + 1)).c_str(), nullptr, 10);
                                std::string fieldName = trim(part.substr(gt + 1));
                                if (fieldName.empty() || cnt == 0)
                                        return;
                                CType  et2 = normalizeTypeCpp(et);
                                size_t esz = ctypeSize(et2);
                                if (esz == 0)
                                        esz = sizeof(void *);
                                size_t al = ctypeAlign(et2);
                                if (al == 0)
                                        al = esz;
                                offset = (offset + al - 1) & ~(al - 1);
                                CFieldDecl f;
                                f.name           = fieldName;
                                f.rawType        = et;
                                f.type           = CType::Unknown;
                                f.isArray        = true;
                                f.arrayCount     = cnt;
                                f.arrayElemType  = et2;
                                f.size           = esz * cnt;
                                f.offset         = offset;
                                offset          += f.size;
                                fields.push_back(std::move(f));
                        }
                }
                return;
        }

        // General case: extract name as last identifier, rest is type.
        size_t end = part.size();
        while (end > 0 && std::isspace((unsigned char)part[end - 1]))
                --end;
        size_t k = end;
        while (k > 0 && isIdChar(part[k - 1]))
                --k;
        if (k == end)
                return;
        std::string name    = part.substr(k, end - k);
        std::string rawType = trim(part.substr(0, k));
        // Absorb leading * or & on name into type.
        while (!name.empty() && (name.front() == '*' || name.front() == '&')) {
                rawType += name.front();
                name     = name.substr(1);
        }
        while (!rawType.empty() && (rawType.back() == '*' || rawType.back() == '&' || rawType.back() == ' '))
                rawType.pop_back();
        if (name.empty() || rawType.empty())
                return;
        // Skip obvious non-field names.
        for (const char *kw : {"nullptr", "NULL", "true", "false"})
                if (name == kw)
                        return;

        CType  ct = normalizeTypeCpp(rawType);
        size_t sz = ctypeSize(ct);
        if (sz == 0)
                sz = sizeof(void *);
        size_t al = ctypeAlign(ct);
        if (al == 0)
                al = sz > 8 ? 8 : sz;
        if (al == 0)
                al = 1;
        offset = (offset + al - 1) & ~(al - 1);
        CFieldDecl f;
        f.name     = name;
        f.rawType  = rawType;
        f.type     = ct;
        f.size     = sz;
        f.offset   = offset;
        offset    += sz;
        fields.push_back(std::move(f));
}

// ─── class body parser ────────────────────────────────────────────────────────

static void parseScopeContent(const std::string &, const std::string &, ParseResult &);

static CClassDecl
parseClassContent(const std::string &body,
                  const std::string &className,
                  const std::string &fullName,
                  bool               isPublicDefault,
                  ParseResult       &outerResult)
{
        CClassDecl cls;
        cls.name     = className;
        cls.fullName = fullName;

        // name → raw body text for inline method definitions
        std::unordered_map<std::string, std::string> inlineBodies;

        // ── Pass 1: find and parse nested struct/enum/class blocks ──
        {
                size_t i = 0;
                while (i < body.size()) {
                        if (body[i] != '{') {
                                ++i;
                                continue;
                        }
                        size_t cl = findMatchingBrace(body, i);
                        if (cl == std::string::npos)
                                break;

                        // Preamble: text since the previous '}' (or start of body).
                        // Find the previous closing brace before position i.
                        size_t prevClose = 0;
                        for (size_t k = 0; k < i;) {
                                if (body[k] == '{') {
                                        size_t mc = findMatchingBrace(body, k);
                                        if (mc != std::string::npos && mc < i) {
                                                prevClose = mc + 1;
                                                k         = mc + 1;
                                        } else
                                                break;
                                } else
                                        ++k;
                        }
                        std::string preamble  = trim(body.substr(prevClose, i - prevClose));
                        ScopeInfo   info      = classifyPreamble(preamble);
                        std::string blockBody = body.substr(i + 1, cl - i - 1);
                        std::string innerFull = fullName + "::" + info.name;

                        switch (info.kind) {
                                case ScopeKind::Struct:
                                        cls.innerStructs.push_back(parseStructContent(blockBody, info.name, innerFull));
                                        break;
                                case ScopeKind::Enum: {
                                        CType bt = normalizeTypeCpp(info.baseType);
                                        if (bt == CType::Unknown || bt == CType::Void)
                                                bt = CType::I32;
                                        cls.innerEnums.push_back(parseEnumContent(blockBody, info.name, innerFull, bt));
                                        break;
                                }
                                case ScopeKind::Class:
                                        outerResult.classes.push_back(
                                            parseClassContent(blockBody, info.name, innerFull, false, outerResult));
                                        break;
                                default:
                                        // Inline method definition: extract function name from preamble.
                                        if (preamble.find('(') != std::string::npos) {
                                                size_t lp = preamble.rfind('(');
                                                size_t k  = lp;
                                                while (k > 0 && std::isspace((unsigned char)preamble[k - 1]))
                                                        --k;
                                                size_t nameEnd = k;
                                                while (k > 0 && isIdChar(preamble[k - 1]))
                                                        --k;
                                                std::string mname = preamble.substr(k, nameEnd - k);
                                                if (!mname.empty())
                                                        inlineBodies[mname] = blockBody;
                                        }
                                        break;
                        }

                        i = cl + 1;
                }
        }

        // ── Pass 2: parse method declarations and collect all data fields ──
        // Strip nested blocks (inline bodies replaced by ';') so only declarations remain.
        {
                std::string stripped    = stripBlocks(body);
                bool        isPublic    = isPublicDefault;
                size_t      fieldOffset = 0; // running byte offset for allFields

                // Process ';'-terminated declarations.
                size_t start = 0;
                for (size_t k = 0; k <= stripped.size(); ++k) {
                        bool atEnd = (k == stripped.size());
                        if (!atEnd && stripped[k] != ';')
                                continue;

                        std::string part = collapseSpaces(trim(stripped.substr(start, k - start)));
                        start            = k + 1;
                        if (part.empty())
                                continue;

                        // Update access level from any specifiers in this part.
                        auto checkSpec = [&](const std::string &kw, bool pub) {
                                size_t p = 0;
                                while ((p = part.find(kw, p)) != std::string::npos) {
                                        bool   before = (p == 0 || !isIdChar(part[p - 1]));
                                        size_t endp   = p + kw.size();
                                        bool   after  = (endp < part.size() && part[endp] == ':');
                                        if (before && after)
                                                isPublic = pub;
                                        p += kw.size();
                                }
                        };
                        checkSpec("public", true);
                        checkSpec("private", false);
                        checkSpec("protected", false);

                        // Strip leading access-specifier content up to the last occurrence.
                        {
                                size_t lastSpec = 0;
                                bool   found    = false;
                                for (const char *kw : {"public:", "private:", "protected:"}) {
                                        size_t p = 0;
                                        while ((p = part.find(kw, p)) != std::string::npos) {
                                                bool before = (p == 0 || !isIdChar(part[p - 1]));
                                                if (before) {
                                                        size_t ep = p + strlen(kw);
                                                        if (!found || ep > lastSpec) {
                                                                lastSpec = ep;
                                                                found    = true;
                                                        }
                                                }
                                                p += strlen(kw);
                                        }
                                }
                                if (found)
                                        part = collapseSpaces(trim(part.substr(lastSpec)));
                        }

                        if (part.empty())
                                continue;

                        if (part.find('(') == std::string::npos) {
                                // No '(' → could be a data field (public or private).
                                parseOneField(part, cls.allFields, fieldOffset);
                                continue;
                        }

                        // Method declaration: only public ones go into cls.methods.
                        if (!isPublic)
                                continue;

                        CMethodDecl method;
                        if (parseMethodDecl(part, className, fullName, method)) {
                                auto bit = inlineBodies.find(method.name);
                                if (bit != inlineBodies.end())
                                        method.inlineBody = bit->second;
                                cls.methods.push_back(std::move(method));
                        }
                }
        }

        return cls;
}

// ─── top-level scope content parser ──────────────────────────────────────────

static void
parseScopeContent(const std::string &text, const std::string &nsPrefix, ParseResult &result)
{
        size_t prevEnd = 0;
        size_t i       = 0;

        while (i < text.size()) {
                if (text[i] != '{') {
                        ++i;
                        continue;
                }

                size_t cl = findMatchingBrace(text, i);
                if (cl == std::string::npos)
                        break;

                // All text from prevEnd to i is the preamble.
                std::string preamble = text.substr(prevEnd, i - prevEnd);

                // Parse any ';'-terminated C-style declarations in the preamble.
                {
                        int    depth = 0;
                        size_t st    = 0;
                        for (size_t k = 0; k < preamble.size(); ++k) {
                                if (preamble[k] == ';' && depth == 0) {
                                        std::string decl = trim(preamble.substr(st, k - st));
                                        st               = k + 1;
                                        if (decl.empty())
                                                continue;
                                        std::string        first;
                                        std::istringstream ss(decl);
                                        ss >> first;
                                        if (first == "typedef" || first == "struct" || first == "union" || first == "enum")
                                                continue;
                                        if (decl.find('(') == std::string::npos)
                                                continue;
                                        // Use existing function-decl logic.
                                        size_t lp = decl.find('(');
                                        size_t rp = std::string::npos;
                                        {
                                                int d = 0;
                                                for (size_t kk = lp; kk < decl.size(); ++kk) {
                                                        if (decl[kk] == '(')
                                                                ++d;
                                                        else if (decl[kk] == ')') {
                                                                if (--d == 0) {
                                                                        rp = kk;
                                                                        break;
                                                                }
                                                        }
                                                }
                                        }
                                        if (rp == std::string::npos)
                                                continue;
                                        std::string bef = trim(decl.substr(0, lp));
                                        std::string ps  = trim(decl.substr(lp + 1, rp - lp - 1));
                                        if (bef.find("(*") != std::string::npos)
                                                continue;
                                        std::string retRaw, funcName;
                                        if (!splitReturnAndName(bef, retRaw, funcName))
                                                continue;
                                        CFuncDecl fd;
                                        fd.name    = std::move(funcName);
                                        fd.retRaw  = retRaw;
                                        fd.retType = normalizeTypeCpp(retRaw);
                                        fd.params  = parseParams(ps, &fd.isVariadic);
                                        result.functions.push_back(std::move(fd));
                                }
                        }
                }

                ScopeInfo   info      = classifyPreamble(preamble);
                std::string blockBody = text.substr(i + 1, cl - i - 1);
                std::string fullName  = nsPrefix.empty() ? info.name : nsPrefix + info.name;

                switch (info.kind) {
                        case ScopeKind::Namespace:
                                parseScopeContent(blockBody, info.name.empty() ? nsPrefix : fullName + "::", result);
                                break;
                        case ScopeKind::Class:
                                if (!info.name.empty()) {
                                        auto cls = parseClassContent(blockBody, info.name, fullName, false, result);
                                        result.classes.push_back(std::move(cls));
                                }
                                break;
                        case ScopeKind::Struct:
                                if (!info.name.empty()) {
                                        auto cls = parseClassContent(blockBody, info.name, fullName, true, result);
                                        // Mirror allFields into a CStructDecl so findStruct() can find
                                        // this type by name (e.g. for constexpr-var field lookup).
                                        CStructDecl sd;
                                        sd.name      = cls.name;
                                        sd.fullName  = cls.fullName;
                                        sd.fields    = cls.allFields;
                                        sd.totalSize = cls.instanceSize;
                                        sd.isPOD     = true;
                                        result.structs.push_back(std::move(sd));
                                        result.classes.push_back(std::move(cls));
                                }
                                break;
                        case ScopeKind::Enum: {
                                CType bt = normalizeTypeCpp(info.baseType);
                                if (bt == CType::Unknown || bt == CType::Void)
                                        bt = CType::I32;
                                if (!info.name.empty())
                                        result.enums.push_back(parseEnumContent(blockBody, info.name, fullName, bt));
                                break;
                        }
                        default: {
                                // Detect: static constexpr TYPE NAME = { init0, init1, ... };
                                std::string tp = trim(preamble);
                                if (!tp.empty() && tp.back() == '=') {
                                        std::string declPart = trim(tp.substr(0, tp.size() - 1));
                                        // Strip leading qualifiers.
                                        for (const char *q : {"static ", "constexpr ", "const ", "inline ", "extern "}) {
                                                std::string qs(q);
                                                for (bool ch = true; ch;) {
                                                        ch = false;
                                                        if (declPart.size() >= qs.size() &&
                                                            declPart.substr(0, qs.size()) == qs) {
                                                                declPart = trim(declPart.substr(qs.size()));
                                                                ch       = true;
                                                        }
                                                }
                                        }
                                        // Last identifier = var name; rest = type name.
                                        size_t vEnd = declPart.size();
                                        while (vEnd > 0 && std::isspace((unsigned char)declPart[vEnd - 1]))
                                                --vEnd;
                                        size_t vk = vEnd;
                                        while (vk > 0 && isIdChar(declPart[vk - 1]))
                                                --vk;
                                        std::string varName  = declPart.substr(vk, vEnd - vk);
                                        std::string typeName = trim(declPart.substr(0, vk));
                                        if (!varName.empty() && !typeName.empty()) {
                                                // Parse the brace-initializer as comma-separated values.
                                                std::vector<std::string> vals;
                                                int                      d  = 0;
                                                size_t                   vs = 0;
                                                for (size_t vi = 0; vi < blockBody.size(); ++vi) {
                                                        char c = blockBody[vi];
                                                        if (c == '(' || c == '{')
                                                                ++d;
                                                        else if (c == ')' || c == '}')
                                                                --d;
                                                        else if (c == ',' && d == 0) {
                                                                vals.push_back(trim(blockBody.substr(vs, vi - vs)));
                                                                vs = vi + 1;
                                                        }
                                                }
                                                if (vs < blockBody.size())
                                                        vals.push_back(trim(blockBody.substr(vs)));
                                                std::string qualName = nsPrefix.empty() ? varName : nsPrefix + varName;
                                                result.constexprVars[qualName] = {typeName, vals};
                                                result.constexprVars[varName]  = {typeName, vals};
                                        }
                                }
                                break;
                        }
                }

                // Advance past closing '}' and optional ';'.
                prevEnd = cl + 1;
                while (prevEnd < text.size() && std::isspace((unsigned char)text[prevEnd]))
                        ++prevEnd;
                if (prevEnd < text.size() && text[prevEnd] == ';')
                        ++prevEnd;
                i = prevEnd;
        }

        // Remaining text after all blocks.
        if (prevEnd < text.size()) {
                std::string tail = text.substr(prevEnd);
                // Parse any remaining C-style function declarations.
                int    depth = 0;
                size_t st    = 0;
                for (size_t k = 0; k < tail.size(); ++k) {
                        if (tail[k] == '{')
                                ++depth;
                        else if (tail[k] == '}') {
                                if (depth > 0)
                                        --depth;
                        } else if (tail[k] == ';' && depth == 0) {
                                std::string decl = trim(tail.substr(st, k - st));
                                st               = k + 1;
                                if (decl.empty())
                                        continue;
                                std::string        first;
                                std::istringstream ss(decl);
                                ss >> first;
                                if (first == "typedef" || first == "struct" || first == "union" || first == "enum")
                                        continue;
                                if (decl.find('(') == std::string::npos)
                                        continue;
                                size_t lp = decl.find('('), rp = std::string::npos;
                                {
                                        int d = 0;
                                        for (size_t kk = lp; kk < decl.size(); ++kk) {
                                                if (decl[kk] == '(')
                                                        ++d;
                                                else if (decl[kk] == ')') {
                                                        if (--d == 0) {
                                                                rp = kk;
                                                                break;
                                                        }
                                                }
                                        }
                                }
                                if (rp == std::string::npos)
                                        continue;
                                std::string bef = trim(decl.substr(0, lp));
                                std::string ps  = trim(decl.substr(lp + 1, rp - lp - 1));
                                if (bef.find("(*") != std::string::npos)
                                        continue;
                                std::string retRaw, funcName;
                                if (!splitReturnAndName(bef, retRaw, funcName))
                                        continue;
                                CFuncDecl fd;
                                fd.name    = std::move(funcName);
                                fd.retRaw  = retRaw;
                                fd.retType = normalizeTypeCpp(retRaw);
                                fd.params  = parseParams(ps, &fd.isVariadic);
                                result.functions.push_back(std::move(fd));
                        }
                }
        }
}

// ─── parseHeaderFull ─────────────────────────────────────────────────────────

// Forward declarations for helpers defined later in this file.
static std::string                        joinContinuationLines(const std::string &);
std::unordered_map<std::string, MacroDef> parseMacroTable(const std::string &);

ParseResult
parseHeaderFull(const std::string &srcOrig)
{
        ParseResult result;
        result.macros = parseMacroTable(srcOrig); // must run before stripPreprocessor

        std::string s = stripComments(srcOrig);
        s             = stripPreprocessor(s);
        s             = stripExternCBraces(s);

        parseScopeContent(s, "", result);
        return result;
}

// ─── parseMacroTable ─────────────────────────────────────────────────────────

static std::string
joinContinuationLines(const std::string &src)
{
        std::string out;
        out.reserve(src.size());
        for (size_t i = 0; i < src.size(); ++i) {
                if (src[i] == '\\' && i + 1 < src.size() && src[i + 1] == '\n') {
                        out += ' ';
                        ++i; // skip both chars
                } else {
                        out += src[i];
                }
        }
        return out;
}

std::unordered_map<std::string, MacroDef>
parseMacroTable(const std::string &src)
{
        std::unordered_map<std::string, MacroDef> result;

        // Work on a line-continuation-joined copy, with block comments stripped
        // (but keep line structure so we can split on '\n').
        std::string work = joinContinuationLines(src);

        // Minimal block-comment strip: replace /* ... */ with spaces.
        for (size_t i = 0; i < work.size();) {
                if (i + 1 < work.size() && work[i] == '/' && work[i + 1] == '*') {
                        work[i]      = ' ';
                        work[i + 1]  = ' ';
                        i           += 2;
                        while (i + 1 < work.size()) {
                                if (work[i] == '*' && work[i + 1] == '/') {
                                        work[i]      = ' ';
                                        work[i + 1]  = ' ';
                                        i           += 2;
                                        break;
                                }
                                if (work[i] != '\n')
                                        work[i] = ' ';
                                ++i;
                        }
                } else {
                        ++i;
                }
        }

        std::istringstream ss(work);
        std::string        line;
        while (std::getline(ss, line)) {
                // Strip line comment.
                {
                        size_t lc = line.find("//");
                        if (lc != std::string::npos)
                                line = line.substr(0, lc);
                }
                std::string tl = trim(line);
                if (tl.empty() || tl[0] != '#')
                        continue;

                // Strip '#' and optional whitespace.
                std::string rest = trim(tl.substr(1));
                if (rest.size() < 6 || rest.substr(0, 6) != "define")
                        continue;
                rest = trim(rest.substr(6));
                if (rest.empty())
                        continue;

                // Extract macro name (and optional parameter list).
                size_t nend = 0;
                while (nend < rest.size() && (isIdChar(rest[nend]) || rest[nend] == '$'))
                        ++nend;
                std::string macroName = rest.substr(0, nend);
                if (macroName.empty())
                        continue;

                rest = rest.substr(nend); // do NOT trim – '(' must immediately follow name

                MacroDef def;
                if (!rest.empty() && rest[0] == '(') {
                        // Function-like macro.
                        def.isFunctionLike = true;
                        size_t rp          = rest.find(')');
                        if (rp == std::string::npos)
                                continue;
                        std::string paramStr = rest.substr(1, rp - 1);
                        rest                 = trim(rest.substr(rp + 1));

                        // Split params by ','.
                        std::istringstream ps(paramStr);
                        std::string        p;
                        while (std::getline(ps, p, ',')) {
                                std::string pt = trim(p);
                                if (!pt.empty())
                                        def.params.push_back(pt);
                        }
                } else {
                        rest = trim(rest);
                }
                def.body          = rest;
                result[macroName] = std::move(def);
        }
        return result;
}

// ─── expandMacros ────────────────────────────────────────────────────────────

static std::string
expandMacros(const std::string &expr, const std::unordered_map<std::string, MacroDef> &macros, int depth = 0)
{
        if (depth > 20)
                return expr;

        std::string out;
        out.reserve(expr.size());
        size_t i = 0;

        while (i < expr.size()) {
                // Read an identifier.
                if (std::isalpha((unsigned char)expr[i]) || expr[i] == '_') {
                        size_t j = i;
                        while (j < expr.size() && isIdChar(expr[j]))
                                ++j;
                        std::string ident = expr.substr(i, j - i);

                        auto it = macros.find(ident);
                        if (it == macros.end()) {
                                out += ident;
                                i    = j;
                                continue;
                        }
                        const MacroDef &def = it->second;

                        if (!def.isFunctionLike) {
                                // Simple replacement.
                                out += expandMacros(def.body, macros, depth + 1);
                                i    = j;
                                continue;
                        }

                        // Function-like: consume argument list.
                        size_t k = j;
                        while (k < expr.size() && std::isspace((unsigned char)expr[k]))
                                ++k;
                        if (k >= expr.size() || expr[k] != '(') {
                                // No argument list — emit as-is.
                                out += ident;
                                i    = j;
                                continue;
                        }

                        // Find matching ')'.
                        int    depth2   = 0;
                        size_t argStart = k + 1;
                        size_t kk       = k;
                        size_t rp       = std::string::npos;
                        while (kk < expr.size()) {
                                if (expr[kk] == '(')
                                        ++depth2;
                                else if (expr[kk] == ')') {
                                        if (--depth2 == 0) {
                                                rp = kk;
                                                break;
                                        }
                                }
                                ++kk;
                        }
                        if (rp == std::string::npos) {
                                out += ident;
                                i    = j;
                                continue;
                        }

                        // Split argument list (respecting nested parens).
                        std::vector<std::string> args;
                        {
                                int    d2 = 0;
                                size_t s2 = argStart;
                                for (size_t ai = argStart; ai < rp; ++ai) {
                                        if (expr[ai] == '(' || expr[ai] == '{')
                                                ++d2;
                                        else if (expr[ai] == ')' || expr[ai] == '}')
                                                --d2;
                                        else if (expr[ai] == ',' && d2 == 0) {
                                                args.push_back(trim(expr.substr(s2, ai - s2)));
                                                s2 = ai + 1;
                                        }
                                }
                                if (s2 <= rp)
                                        args.push_back(trim(expr.substr(s2, rp - s2)));
                        }

                        // Substitute parameters into body.
                        std::string expanded = def.body;
                        for (size_t pi = 0; pi < def.params.size() && pi < args.size(); ++pi) {
                                std::string        replaced;
                                size_t             pos   = 0;
                                const std::string &pname = def.params[pi];
                                while (pos < expanded.size()) {
                                        size_t found = expanded.find(pname, pos);
                                        if (found == std::string::npos) {
                                                replaced += expanded.substr(pos);
                                                break;
                                        }
                                        // Ensure word boundary.
                                        bool leftOk  = (found == 0 || !isIdChar(expanded[found - 1]));
                                        bool rightOk = (found + pname.size() >= expanded.size() ||
                                                        !isIdChar(expanded[found + pname.size()]));
                                        if (leftOk && rightOk) {
                                                replaced += expanded.substr(pos, found - pos);
                                                replaced += args[pi];
                                                pos       = found + pname.size();
                                        } else {
                                                replaced += expanded[pos];
                                                ++pos;
                                        }
                                }
                                expanded = std::move(replaced);
                        }
                        out += expandMacros(expanded, macros, depth + 1);
                        i    = rp + 1;
                } else {
                        out += expr[i++];
                }
        }
        return out;
}

// ─── evalIntExpr ─────────────────────────────────────────────────────────────

struct ExprParser {
        const char *s;
        const char *end;

        void skipWs()
        {
                while (s < end && std::isspace((unsigned char)*s))
                        ++s;
        }

        bool done()
        {
                skipWs();
                return s >= end;
        }

        // Parse an optional leading cast like (uint32_t) or (int) and discard it.
        void tryCast()
        {
                skipWs();
                if (s >= end || *s != '(')
                        return;
                const char *save = s;
                ++s;
                skipWs();
                size_t nlen = 0;
                while (s + nlen < end && isIdChar(*(s + nlen)))
                        ++nlen;
                if (nlen == 0) {
                        s = save;
                        return;
                }
                const char *after = s + nlen;
                while (after < end && std::isspace((unsigned char)*after))
                        ++after;
                if (after < end && *after == ')') {
                        // Check if it looks like a type (not an expression start).
                        std::string        tok(s, nlen);
                        static const char *types[] = {"int",
                                                      "uint8_t",
                                                      "uint16_t",
                                                      "uint32_t",
                                                      "uint64_t",
                                                      "int8_t",
                                                      "int16_t",
                                                      "int32_t",
                                                      "int64_t",
                                                      "char",
                                                      "short",
                                                      "long",
                                                      "unsigned",
                                                      "signed",
                                                      "bool",
                                                      nullptr};
                        for (int ti = 0; types[ti]; ++ti) {
                                if (tok == types[ti]) {
                                        s = after + 1;
                                        return;
                                }
                        }
                }
                s = save;
        }

        int64_t parsePrimary()
        {
                skipWs();
                tryCast();
                skipWs();
                if (s >= end)
                        return 0;

                // Parenthesized expression.
                if (*s == '(') {
                        ++s;
                        int64_t v = parseExpr();
                        skipWs();
                        if (s < end && *s == ')')
                                ++s;
                        return v;
                }

                // Unary operators.
                if (*s == '-') {
                        ++s;
                        return -parsePrimary();
                }
                if (*s == '+') {
                        ++s;
                        return parsePrimary();
                }
                if (*s == '~') {
                        ++s;
                        return ~parsePrimary();
                }
                if (*s == '!') {
                        ++s;
                        return !parsePrimary();
                }

                // Hex literal.
                if (s + 1 < end && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                        s         += 2;
                        int64_t v  = 0;
                        while (s < end && std::isxdigit((unsigned char)*s)) {
                                int d =
                                    std::isdigit((unsigned char)*s) ? (*s - '0') : (std::tolower((unsigned char)*s) - 'a' + 10);
                                v = v * 16 + d;
                                ++s;
                        }
                        // Strip optional UL suffix.
                        while (s < end && (*s == 'u' || *s == 'U' || *s == 'l' || *s == 'L'))
                                ++s;
                        return v;
                }

                // Decimal literal.
                if (std::isdigit((unsigned char)*s)) {
                        int64_t v = 0;
                        while (s < end && std::isdigit((unsigned char)*s))
                                v = v * 10 + (*s++ - '0');
                        while (s < end && (*s == 'u' || *s == 'U' || *s == 'l' || *s == 'L'))
                                ++s;
                        return v;
                }

                // Identifier – treat as 0 (already expanded by expandMacros before eval).
                while (s < end && isIdChar(*s))
                        ++s;
                return 0;
        }

        int64_t parseMul()
        {
                int64_t v = parsePrimary();
                while (!done()) {
                        if (*s == '*') {
                                ++s;
                                v *= parsePrimary();
                        } else if (*s == '/') {
                                ++s;
                                int64_t r = parsePrimary();
                                v         = r ? v / r : 0;
                        } else if (*s == '%') {
                                ++s;
                                int64_t r = parsePrimary();
                                v         = r ? v % r : 0;
                        } else
                                break;
                }
                return v;
        }

        int64_t parseAdd()
        {
                int64_t v = parseMul();
                while (!done()) {
                        if (*s == '+') {
                                ++s;
                                v += parseMul();
                        } else if (*s == '-') {
                                ++s;
                                v -= parseMul();
                        } else
                                break;
                }
                return v;
        }

        int64_t parseShift()
        {
                int64_t v = parseAdd();
                while (!done()) {
                        if (s + 1 < end && s[0] == '<' && s[1] == '<') {
                                s  += 2;
                                v <<= parseAdd();
                        } else if (s + 1 < end && s[0] == '>' && s[1] == '>') {
                                s  += 2;
                                v >>= parseAdd();
                        } else
                                break;
                }
                return v;
        }

        int64_t parseBitAnd()
        {
                int64_t v = parseShift();
                while (!done() && *s == '&' && (s + 1 >= end || s[1] != '&')) {
                        ++s;
                        v &= parseShift();
                }
                return v;
        }

        int64_t parseBitXor()
        {
                int64_t v = parseBitAnd();
                while (!done() && *s == '^') {
                        ++s;
                        v ^= parseBitAnd();
                }
                return v;
        }

        int64_t parseBitOr()
        {
                int64_t v = parseBitXor();
                while (!done() && *s == '|' && (s + 1 >= end || s[1] != '|')) {
                        ++s;
                        v |= parseBitXor();
                }
                return v;
        }

        int64_t parseExpr() { return parseBitOr(); }
};

bool
evalIntExpr(const std::string &expr, const std::unordered_map<std::string, MacroDef> &macros, int64_t &result)
{
        std::string expanded = expandMacros(expr, macros);
        ExprParser  ep;
        ep.s   = expanded.c_str();
        ep.end = ep.s + expanded.size();
        result = ep.parseExpr();
        return true;
}

// ─── applyInlineBody ─────────────────────────────────────────────────────────

int
applyInlineBody(void *buf, size_t bufSize, const CClassDecl &cls, const CMethodDecl &method, const ParseResult &pr)
{
        if (method.inlineBody.empty())
                return 0;

        int written = 0;
        // Split body on ';' and process each statement.
        const std::string &body = method.inlineBody;
        size_t             i    = 0;
        while (i < body.size()) {
                // Find next ';'.
                size_t      semi = body.find(';', i);
                std::string stmt = trim(body.substr(i, semi == std::string::npos ? body.size() - i : semi - i));
                i                = (semi == std::string::npos) ? body.size() : semi + 1;
                if (stmt.empty())
                        continue;

                // We only handle simple assignments: lhs = rhs
                // The lhs is a plain member name (no dereference, no indexing).
                size_t eq = stmt.find('=');
                if (eq == std::string::npos)
                        continue;
                // Avoid '==' and '!=' etc.
                if (eq + 1 < stmt.size() && stmt[eq + 1] == '=')
                        continue;
                if (eq > 0 && (stmt[eq - 1] == '!' || stmt[eq - 1] == '<' || stmt[eq - 1] == '>' || stmt[eq - 1] == '+' ||
                               stmt[eq - 1] == '-'))
                        continue;

                std::string lhs = trim(stmt.substr(0, eq));
                std::string rhs = trim(stmt.substr(eq + 1));
                if (lhs.empty() || rhs.empty())
                        continue;

                // Strip leading "this->" from lhs.
                if (lhs.size() > 6 && lhs.substr(0, 6) == "this->")
                        lhs = trim(lhs.substr(6));

                // Find the field in cls.allFields.
                const CFieldDecl *field = nullptr;
                for (const auto &f : cls.allFields) {
                        if (f.name == lhs) {
                                field = &f;
                                break;
                        }
                }
                if (!field || field->size == 0)
                        continue;
                if (field->offset + field->size > bufSize)
                        continue;

                // Resolve the rhs value.
                // Handle "VAR.MEMBER" lookups into constexprVars.
                std::string resolvedRhs = rhs;
                {
                        size_t dot = rhs.find('.');
                        if (dot != std::string::npos) {
                                std::string varName    = trim(rhs.substr(0, dot));
                                std::string memberName = trim(rhs.substr(dot + 1));
                                auto        cvit       = pr.constexprVars.find(varName);
                                if (cvit != pr.constexprVars.end()) {
                                        const std::string &typeName = cvit->second.first;
                                        const auto        &vals     = cvit->second.second;
                                        // Find field index in the struct definition.
                                        const CStructDecl *sd = findStruct(pr, typeName);
                                        if (sd) {
                                                for (size_t fi = 0; fi < sd->fields.size(); ++fi) {
                                                        if (sd->fields[fi].name == memberName && fi < vals.size()) {
                                                                resolvedRhs = vals[fi];
                                                                break;
                                                        }
                                                }
                                        } else {
                                                // typeName not found as struct — try index via name pattern
                                                // or just use first value as fallback.
                                                if (!vals.empty())
                                                        resolvedRhs = vals[0];
                                        }
                                }
                        }
                }

                int64_t val = 0;
                evalIntExpr(resolvedRhs, pr.macros, val);

                // Write the value into the buffer respecting field size.
                uint8_t *dest = static_cast<uint8_t *>(buf) + field->offset;
                switch (field->size) {
                        case 1: {
                                uint8_t v = static_cast<uint8_t>(val);
                                memcpy(dest, &v, 1);
                                break;
                        }
                        case 2: {
                                uint16_t v = static_cast<uint16_t>(val);
                                memcpy(dest, &v, 2);
                                break;
                        }
                        case 4: {
                                uint32_t v = static_cast<uint32_t>(val);
                                memcpy(dest, &v, 4);
                                break;
                        }
                        case 8: {
                                uint64_t v = static_cast<uint64_t>(val);
                                memcpy(dest, &v, 8);
                                break;
                        }
                        default:
                                break;
                }
                ++written;
        }
        return written;
}

// ─── lookup helpers ───────────────────────────────────────────────────────────

const CStructDecl *
findStruct(const ParseResult &pr, const std::string &name)
{
        for (const auto &s : pr.structs)
                if (s.name == name || s.fullName == name)
                        return &s;
        for (const auto &cls : pr.classes)
                for (const auto &s : cls.innerStructs)
                        if (s.name == name || s.fullName == name)
                                return &s;
        return nullptr;
}

const CEnumDecl *
findEnum(const ParseResult &pr, const std::string &name, const std::string &classFullName)
{
        // Prefer inner enums of the specified class.
        if (!classFullName.empty()) {
                for (const auto &cls : pr.classes) {
                        if (cls.fullName != classFullName)
                                continue;
                        for (const auto &e : cls.innerEnums)
                                if (e.name == name || e.fullName == name)
                                        return &e;
                }
        }
        // Fall back to global enums.
        for (const auto &e : pr.enums)
                if (e.name == name || e.fullName == name)
                        return &e;
        // Finally, any class's inner enums.
        for (const auto &cls : pr.classes)
                for (const auto &e : cls.innerEnums)
                        if (e.name == name || e.fullName == name)
                                return &e;
        return nullptr;
}
