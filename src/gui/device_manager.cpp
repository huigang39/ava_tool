#include "gui/device_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "cJSON.h"
#include "core/jlink_port.hpp"
#include "gui/i18n.hpp"
#include "gui/monitor.hpp"
#include "gui/monitor_types.hpp"
#include "imgui.h"
#include "platform/native_dlg.hpp"

namespace
{

constexpr int kDeviceFormatVersion = 3;

struct DevicePropertyMovePayload {
        uint64_t instanceId{0};
        uint64_t propertyId{0};
};

struct CTypeChoice {
        CType       type;
        const char *name;
};

constexpr CTypeChoice kPropertyTypes[] = {
    {CType::Bool, "bool"},
    {CType::I8, "i8"},
    {CType::I16, "i16"},
    {CType::I32, "i32"},
    {CType::I64, "i64"},
    {CType::U8, "u8"},
    {CType::U16, "u16"},
    {CType::U32, "u32"},
    {CType::U64, "u64"},
    {CType::F32, "f32"},
    {CType::F64, "f64"},
};

std::string
trimCopy(const std::string &text)
{
        const size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
                return {};
        const size_t last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
}

std::string
lowerCopy(std::string text)
{
        std::transform(
            text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
}

std::string
hexAddress(uint32_t address)
{
        char text[16];
        std::snprintf(text, sizeof(text), "0x%08X", address);
        return text;
}

bool
isScalarType(CType type)
{
        return type == CType::Bool || type == CType::I8 || type == CType::I16 || type == CType::I32 || type == CType::I64 ||
               type == CType::U8 || type == CType::U16 || type == CType::U32 || type == CType::U64 || type == CType::F32 ||
               type == CType::F64;
}

bool
isCapacityType(CType type)
{
        return type == CType::I32 || type == CType::I64 || type == CType::U32 || type == CType::U64;
}

uint32_t
scalarTypeSize(CType type)
{
        switch (type) {
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
                default:
                        return 0;
        }
}

const char *
stableTypeName(CType type)
{
        for (const auto &choice : kPropertyTypes)
                if (choice.type == type)
                        return choice.name;
        return "unknown";
}

std::optional<CType>
stableTypeFromName(const std::string &name)
{
        const std::string key = lowerCopy(trimCopy(name));
        for (const auto &choice : kPropertyTypes)
                if (key == choice.name)
                        return choice.type;

        // Accept ctypeLabel spellings for early development files.
        if (key == "int8")
                return CType::I8;
        if (key == "int16")
                return CType::I16;
        if (key == "int32")
                return CType::I32;
        if (key == "int64")
                return CType::I64;
        if (key == "uint8")
                return CType::U8;
        if (key == "uint16")
                return CType::U16;
        if (key == "uint32")
                return CType::U32;
        if (key == "uint64")
                return CType::U64;
        if (key == "float")
                return CType::F32;
        if (key == "double")
                return CType::F64;
        return std::nullopt;
}

template <typename T>
T
readScalar(const std::array<uint8_t, 16> &data)
{
        T value{};
        static_assert(sizeof(T) <= 16);
        std::memcpy(&value, data.data(), sizeof(T));
        return value;
}

template <typename T>
void
writeScalar(std::array<uint8_t, 16> &data, T value)
{
        static_assert(sizeof(T) <= 16);
        data.fill(0);
        std::memcpy(data.data(), &value, sizeof(T));
}

bool
parseSigned(const std::string &text, int64_t minimum, int64_t maximum, int64_t &value)
{
        const std::string cleaned = trimCopy(text);
        if (cleaned.empty())
                return false;
        errno                  = 0;
        char           *end    = nullptr;
        const long long parsed = std::strtoll(cleaned.c_str(), &end, 0);
        if (errno == ERANGE || !end || end == cleaned.c_str() || *end != '\0')
                return false;
        if (parsed < minimum || parsed > maximum)
                return false;
        value = static_cast<int64_t>(parsed);
        return true;
}

bool
parseUnsigned(const std::string &text, uint64_t maximum, uint64_t &value)
{
        const std::string cleaned = trimCopy(text);
        if (cleaned.empty() || cleaned.front() == '-')
                return false;
        errno                           = 0;
        char                    *end    = nullptr;
        const unsigned long long parsed = std::strtoull(cleaned.c_str(), &end, 0);
        if (errno == ERANGE || !end || end == cleaned.c_str() || *end != '\0' || parsed > maximum)
                return false;
        value = static_cast<uint64_t>(parsed);
        return true;
}

bool
parseFloat32(const std::string &text, float &value)
{
        const std::string cleaned = trimCopy(text);
        if (cleaned.empty())
                return false;
        errno              = 0;
        char       *end    = nullptr;
        const float parsed = std::strtof(cleaned.c_str(), &end);
        if (errno == ERANGE || !end || end == cleaned.c_str() || *end != '\0' || !std::isfinite(parsed))
                return false;
        value = parsed;
        return true;
}

bool
parseFloat64(const std::string &text, double &value)
{
        const std::string cleaned = trimCopy(text);
        if (cleaned.empty())
                return false;
        errno               = 0;
        char        *end    = nullptr;
        const double parsed = std::strtod(cleaned.c_str(), &end);
        if (errno == ERANGE || !end || end == cleaned.c_str() || *end != '\0' || !std::isfinite(parsed))
                return false;
        value = parsed;
        return true;
}

bool
parseBool(const std::string &text, bool &value)
{
        const std::string cleaned = lowerCopy(trimCopy(text));
        if (cleaned == "true" || cleaned == "1") {
                value = true;
                return true;
        }
        if (cleaned == "false" || cleaned == "0") {
                value = false;
                return true;
        }
        return false;
}

std::string
normalizeTypeText(const std::string &raw, CType fallback)
{
        std::string text = trimCopy(raw.empty() ? std::string(ctypeLabel(fallback)) : raw);
        std::string out;
        out.reserve(text.size());
        bool pendingSpace = false;
        auto punctuation  = [](char c) {
                return c == '*' || c == '&' || c == ',' || c == '[' || c == ']' || c == '(' || c == ')';
        };
        for (size_t i = 0; i < text.size(); ++i) {
                const unsigned char uc = static_cast<unsigned char>(text[i]);
                if (std::isspace(uc)) {
                        pendingSpace = !out.empty();
                        continue;
                }
                const char c = text[i];
                if (c == ':' && i + 1 < text.size() && text[i + 1] == ':') {
                        while (!out.empty() && out.back() == ' ')
                                out.pop_back();
                        out += "::";
                        ++i;
                        pendingSpace = false;
                        continue;
                }
                if (punctuation(c)) {
                        while (!out.empty() && out.back() == ' ')
                                out.pop_back();
                        out          += c;
                        pendingSpace  = false;
                        continue;
                }
                if (pendingSpace && !out.empty() && out.back() != ':' && !punctuation(out.back()))
                        out += ' ';
                out          += c;
                pendingSpace  = false;
        }
        return out;
}

bool
eraseWord(std::string &text, const std::string &word)
{
        bool   erased = false;
        size_t pos    = 0;
        while ((pos = text.find(word, pos)) != std::string::npos) {
                const bool leftOk =
                    pos == 0 || !std::isalnum(static_cast<unsigned char>(text[pos - 1])) && text[pos - 1] != '_';
                const size_t end = pos + word.size();
                const bool   rightOk =
                    end == text.size() || !std::isalnum(static_cast<unsigned char>(text[end])) && text[end] != '_';
                if (leftOk && rightOk) {
                        text.erase(pos, word.size());
                        erased = true;
                } else {
                        pos = end;
                }
        }
        return erased;
}

std::optional<CType>
pointerPointeeType(const CParam &param, bool &isConst)
{
        std::string raw = trimCopy(param.rawType);
        isConst         = false;
        if (raw.empty() || raw.find('*') == std::string::npos || raw.find("**") != std::string::npos)
                return std::nullopt;

        const size_t star = raw.find('*');
        if (raw.find('*', star + 1) != std::string::npos)
                return std::nullopt;
        std::string after = trimCopy(raw.substr(star + 1));
        if (!after.empty() && after != "const" && after != "volatile")
                return std::nullopt;
        raw.resize(star);
        raw     = lowerCopy(trimCopy(raw));
        isConst = eraseWord(raw, "const");
        eraseWord(raw, "volatile");
        eraseWord(raw, "restrict");
        eraseWord(raw, "__restrict");
        raw = trimCopy(raw);
        while (raw.find("  ") != std::string::npos)
                raw.replace(raw.find("  "), 2, " ");
        if (raw.rfind("std::", 0) == 0)
                raw.erase(0, 5);

        if (raw == "bool")
                return CType::Bool;
        if (raw == "char" || raw == "signed char" || raw == "int8_t" || raw == "int8")
                return CType::I8;
        if (raw == "unsigned char" || raw == "uint8_t" || raw == "uint8")
                return CType::U8;
        if (raw == "short" || raw == "short int" || raw == "signed short" || raw == "signed short int" || raw == "int16_t" ||
            raw == "int16")
                return CType::I16;
        if (raw == "unsigned short" || raw == "unsigned short int" || raw == "uint16_t" || raw == "uint16")
                return CType::U16;
        if (raw == "int" || raw == "signed" || raw == "signed int" || raw == "int32_t" || raw == "int32")
                return CType::I32;
        if (raw == "unsigned" || raw == "unsigned int" || raw == "uint32_t" || raw == "uint32")
                return CType::U32;
        if (raw == "long long" || raw == "long long int" || raw == "signed long long" || raw == "signed long long int" ||
            raw == "int64_t" || raw == "int64")
                return CType::I64;
        if (raw == "unsigned long long" || raw == "unsigned long long int" || raw == "uint64_t" || raw == "uint64")
                return CType::U64;
        if (raw == "long" || raw == "long int" || raw == "signed long" || raw == "signed long int")
                return sizeof(long) == 8 ? CType::I64 : CType::I32;
        if (raw == "unsigned long" || raw == "unsigned long int")
                return sizeof(unsigned long) == 8 ? CType::U64 : CType::U32;
        if (raw == "float")
                return CType::F32;
        if (raw == "double")
                return CType::F64;
        return std::nullopt;
}

std::string
pointerBaseText(const CParam &param)
{
        std::string  raw  = lowerCopy(trimCopy(param.rawType));
        const size_t star = raw.find('*');
        if (star == std::string::npos)
                return {};
        raw.resize(star);
        eraseWord(raw, "const");
        eraseWord(raw, "volatile");
        eraseWord(raw, "restrict");
        eraseWord(raw, "__restrict");
        raw = trimCopy(raw);
        while (raw.find("  ") != std::string::npos)
                raw.replace(raw.find("  "), 2, " ");
        return raw;
}

bool
isCharacterPointer(const CParam &param)
{
        const std::string base = pointerBaseText(param);
        return base == "char" || base == "signed char" || base == "unsigned char";
}

bool
isStrictConstCharPointer(const CParam &param)
{
        if (param.type != CType::Ptr || param.rawType.find('*') == std::string::npos ||
            param.rawType.find('*') != param.rawType.rfind('*'))
                return false;
        std::string before = lowerCopy(trimCopy(param.rawType.substr(0, param.rawType.find('*'))));
        while (before.find("  ") != std::string::npos)
                before.replace(before.find("  "), 2, " ");
        return before == "const char" || before == "char const";
}

template <typename UsedPredicate>
uint64_t
takeStableId(uint64_t &next, UsedPredicate used)
{
        uint64_t       candidate = next == 0 ? 1 : next;
        const uint64_t start     = candidate;
        do {
                if (!used(candidate)) {
                        next = candidate == std::numeric_limits<uint64_t>::max() ? 1 : candidate + 1;
                        return candidate;
                }
                candidate = candidate == std::numeric_limits<uint64_t>::max() ? 1 : candidate + 1;
        } while (candidate != start);
        return 0;
}

int
resizeStringCallback(ImGuiInputTextCallbackData *data)
{
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                auto *text = static_cast<std::string *>(data->UserData);
                text->resize(static_cast<size_t>(data->BufTextLen));
                data->Buf = text->data();
        }
        return 0;
}

bool
inputTextString(const char *label, std::string &text, ImGuiInputTextFlags flags = 0)
{
        flags |= ImGuiInputTextFlags_CallbackResize;
        return ImGui::InputText(label, text.data(), text.capacity() + 1, flags, resizeStringCallback, &text);
}

std::string
pathForStorage(const std::string &path, const std::string &baseDir)
{
        if (path.empty())
                return path;
        std::error_code             ec;
        const std::filesystem::path base = baseDir.empty() ? std::filesystem::current_path(ec) : std::filesystem::path(baseDir);
        if (ec || base.empty())
                return path;
        const auto rel = std::filesystem::relative(std::filesystem::path(path), base, ec);
        if (ec || rel.empty())
                return path;
        return rel.generic_string();
}

std::string
pathFromStorage(const char *stored, const std::string &baseDir)
{
        if (!stored || !stored[0])
                return {};
        const std::filesystem::path path(stored);
        if (path.is_absolute())
                return path.lexically_normal().string();
        std::error_code             ec;
        const std::filesystem::path base = baseDir.empty() ? std::filesystem::current_path(ec) : std::filesystem::path(baseDir);
        if (ec || base.empty())
                return path.lexically_normal().string();
        return (base / path).lexically_normal().string();
}

void
addId(cJSON *object, const char *key, uint64_t id)
{
        const std::string text = std::to_string(id);
        cJSON_AddStringToObject(object, key, text.c_str());
}

bool
readId(const cJSON *object, const char *key, uint64_t &id, bool required = true)
{
        const cJSON *item = cJSON_GetObjectItem(object, key);
        if (!item)
                return !required;
        if (cJSON_IsString(item)) {
                uint64_t parsed = 0;
                if (!parseUnsigned(item->valuestring ? item->valuestring : "", std::numeric_limits<uint64_t>::max(), parsed))
                        return false;
                id = parsed;
                return true;
        }
        if (cJSON_IsNumber(item) && item->valuedouble >= 0.0 && item->valuedouble <= 9007199254740991.0) {
                id = static_cast<uint64_t>(item->valuedouble);
                return static_cast<double>(id) == item->valuedouble;
        }
        return false;
}

const char *
argSourceName(int source)
{
        switch (source) {
                case 0:
                        return "literal";
                case 1:
                        return "propertyValue";
                case 2:
                        return "propertyAddress";
                default:
                        return "literal";
        }
}

std::optional<int>
argSourceFromName(const char *name)
{
        if (!name)
                return std::nullopt;
        if (std::strcmp(name, "literal") == 0)
                return 0;
        if (std::strcmp(name, "propertyValue") == 0)
                return 1;
        if (std::strcmp(name, "propertyAddress") == 0)
                return 2;
        return std::nullopt;
}

bool
hasExtension(const std::string &path, const std::vector<std::string> &extensions)
{
        const std::string lower = lowerCopy(path);
        for (const auto &extension : extensions) {
                if (lower.size() >= extension.size() &&
                    lower.compare(lower.size() - extension.size(), extension.size(), extension) == 0)
                        return true;
        }
        return false;
}

} // namespace

DeviceManager::~DeviceManager()
{
        stopPollThread();
}

void
DeviceManager::startPollThread()
{
        if (poll_)
                return;
        poll_       = std::make_shared<PollState>();
        auto state  = poll_;
        pollThread_ = std::thread([state]() {
                try {
                        while (state->running.load(std::memory_order_acquire)) {
                                std::vector<PollReq> requests;
                                std::deque<WriteReq> writes;
                                {
                                        std::lock_guard lock(state->mutex);
                                        requests = state->requests;
                                        writes.swap(state->writes);
                                }

                                for (const WriteReq &request : writes) {
                                        bool ok = false;
                                        if (request.size > 0u && request.size <= request.data.size() &&
                                            JLinkPort::instance().isConnected()) {
                                                std::array<uint8_t, 8> output = request.data;
                                                if (request.bitSize != 0u) {
                                                        std::array<uint8_t, 8> current{};
                                                        ok = request.bitOffset < 64u &&
                                                             request.bitSize <= 64u - request.bitOffset &&
                                                             JLinkPort::instance().readMem(
                                                                 request.address, request.size, current.data());
                                                        if (ok) {
                                                                uint64_t oldRaw = 0;
                                                                uint64_t newRaw = 0;
                                                                std::memcpy(&oldRaw, current.data(), request.size);
                                                                std::memcpy(&newRaw, request.data.data(), request.size);
                                                                const uint64_t valueMask =
                                                                    request.bitSize == 64u
                                                                        ? std::numeric_limits<uint64_t>::max()
                                                                        : ((uint64_t{1} << request.bitSize) - 1u);
                                                                const uint64_t fieldMask = valueMask << request.bitOffset;
                                                                const uint64_t merged =
                                                                    (oldRaw & ~fieldMask) |
                                                                    ((newRaw & valueMask) << request.bitOffset);
                                                                std::memcpy(output.data(), &merged, request.size);
                                                        }
                                                } else {
                                                        ok = true;
                                                }
                                                if (ok)
                                                        ok = JLinkPort::instance().writeMem(
                                                            request.address, request.size, output.data());
                                        }
                                        std::lock_guard lock(state->mutex);
                                        state->writeResults[request.token] = ok;
                                }

                                if (JLinkPort::instance().isConnected()) {
                                        for (const PollReq &request : requests) {
                                                if (!state->running.load(std::memory_order_acquire))
                                                        break;
                                                PollVal value;
                                                value.size = request.size;
                                                value.ok   = request.size > 0u && request.size <= value.data.size() &&
                                                           JLinkPort::instance().readMem(
                                                               request.address, request.size, value.data.data());
                                                std::lock_guard lock(state->mutex);
                                                state->values[request.address] = value;
                                        }
                                }

                                const uint32_t interval =
                                    std::clamp(state->intervalMs.load(std::memory_order_relaxed), 20u, 2000u);
                                for (uint32_t slept  = 0; slept < interval && state->running.load(std::memory_order_acquire);
                                     slept          += 20u)
                                        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(20u, interval - slept)));
                        }
                } catch (...) {
                        // A provider failure must never terminate the application.
                }
        });
}

void
DeviceManager::stopPollThread()
{
        if (poll_)
                poll_->running.store(false, std::memory_order_release);
        if (pollThread_.joinable())
                pollThread_.join();
        poll_.reset();
}

void
DeviceManager::clear()
{
        if (poll_) {
                std::lock_guard lock(poll_->mutex);
                poll_->requests.clear();
                poll_->values.clear();
                poll_->writes.clear();
                poll_->writeResults.clear();
        }
        devices_.clear();
        nextDeviceId_            = 1;
        selectedDevice_          = -1;
        selectedInstance_        = -1;
        selectedFunction_        = -1;
        pendingDeleteTypeId_     = 0;
        pendingDeleteInstanceId_ = 0;
        pendingDeletePropertyId_ = 0;
        pendingDeleteMethodId_   = 0;
        newDeviceName_[0]        = '\0';
        pendingDropFiles_.clear();
        managerStatus_.clear();
        managerStatusIsError_ = false;
        open_                 = false;
        modified_             = false;
}

void
DeviceManager::refreshJLinkProperties()
{
        startPollThread();

        const uint64_t nowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        if (symbolResolver_ && nowMs - lastSymbolResolveMs_ >= 1000u) {
                lastSymbolResolveMs_ = nowMs;
                auto resolve         = [&](Property &property) {
                        if (property.source != PropertySource::JLink || property.symbol.empty())
                                return;
                        uint32_t address        = 0u;
                        property.symbolResolved = symbolResolver_(property.symbol, address) && address != 0u;
                        if (property.symbolResolved)
                                property.address = address;
                };
                for (auto &device : devices_) {
                        for (Property &property : device->properties)
                                resolve(property);
                        for (DeviceInstance &instance : device->instances)
                                for (Property &property : instance.properties)
                                        resolve(property);
                }
        }

        std::unordered_map<uint32_t, uint32_t> sizes;
        auto                                   collect = [&](const Property &property) {
                if (property.source != PropertySource::JLink || property.address == 0u)
                        return;
                const uint32_t size = scalarTypeSize(property.type);
                if (size != 0u)
                        sizes[property.address] = std::max(sizes[property.address], size);
        };
        for (const auto &device : devices_) {
                for (const Property &property : device->properties)
                        collect(property);
                for (const DeviceInstance &instance : device->instances)
                        for (const Property &property : instance.properties)
                                collect(property);
        }

        std::vector<PollReq> requests;
        requests.reserve(sizes.size());
        for (const auto &[address, size] : sizes)
                requests.push_back({address, size});

        std::unordered_map<uint32_t, PollVal> values;
        std::unordered_map<uint64_t, bool>    writeResults;
        {
                std::lock_guard lock(poll_->mutex);
                poll_->requests = std::move(requests);
                values          = poll_->values;
                writeResults.swap(poll_->writeResults);
        }

        const bool connected = JLinkPort::instance().isConnected();
        auto       publish   = [&](Property &property) {
                if (property.source != PropertySource::JLink)
                        return;

                if (property.writePending) {
                        const auto result = writeResults.find(property.writeToken);
                        if (result != writeResults.end()) {
                                property.writePending = false;
                                property.lastWriteOk  = result->second;
                        }
                }
                if (!connected || property.address == 0u) {
                        property.liveValueKnown = true;
                        property.liveReadOk     = false;
                        return;
                }
                const auto found = values.find(property.address);
                if (found == values.end())
                        return;
                const PollVal &value    = found->second;
                property.liveValueKnown = true;
                property.liveReadOk     = value.ok;
                const uint32_t size     = scalarTypeSize(property.type);
                if (!value.ok || size == 0u || value.size < size || property.writePending)
                        return;

                if (property.bitSize == 0u) {
                        property.data.fill(0);
                        std::memcpy(property.data.data(), value.data.data(), size);
                        if (property.type == CType::Bool)
                                writeScalar<uint8_t>(property.data, readScalar<uint8_t>(property.data) ? 1u : 0u);
                } else if (property.bitOffset < 64u && property.bitSize <= 64u - property.bitOffset) {
                        uint64_t raw = 0;
                        std::memcpy(&raw, value.data.data(), size);
                        const uint64_t mask  = property.bitSize == 64u ? std::numeric_limits<uint64_t>::max()
                                                                               : ((uint64_t{1} << property.bitSize) - 1u);
                        const uint64_t field = (raw >> property.bitOffset) & mask;
                        std::string    text;
                        const bool     signedType = property.type == CType::I8 || property.type == CType::I16 ||
                                                property.type == CType::I32 || property.type == CType::I64;
                        if (signedType && property.bitSize != 0u && property.bitSize < 64u &&
                            (field & (uint64_t{1} << (property.bitSize - 1u))) != 0u)
                                text = std::to_string(static_cast<int64_t>(field | ~mask));
                        else
                                text = std::to_string(field);
                        Property parsed = property;
                        if (setPropertyValue(parsed, text))
                                property.data = parsed.data;
                }
                if (!property.editActive) {
                        property.editValue        = propertyValueString(property);
                        property.editValueInvalid = false;
                }
        };
        for (auto &device : devices_) {
                for (Property &property : device->properties)
                        publish(property);
                for (DeviceInstance &instance : device->instances)
                        for (Property &property : instance.properties)
                                publish(property);
        }
}

void
DeviceManager::queuePropertyWrite(Property &property)
{
        if (!poll_ || property.source != PropertySource::JLink || !property.writable || property.address == 0u)
                return;
        const uint32_t size = scalarTypeSize(property.type);
        if (size == 0u || size > 8u)
                return;
        WriteReq request;
        request.token = nextWriteToken_++;
        if (request.token == 0u)
                request.token = nextWriteToken_++;
        request.address   = property.address;
        request.size      = size;
        request.bitOffset = property.bitOffset;
        request.bitSize   = property.bitSize;
        std::memcpy(request.data.data(), property.data.data(), size);
        {
                std::lock_guard lock(poll_->mutex);
                poll_->writes.push_back(request);
        }
        property.writeToken   = request.token;
        property.writePending = true;
        property.lastWriteOk  = true;
}

DeviceManager::Property *
DeviceManager::findProperty(Device &device, uint64_t id)
{
        for (auto &property : device.properties)
                if (property.id == id)
                        return &property;
        return nullptr;
}

const DeviceManager::Property *
DeviceManager::findProperty(const Device &device, uint64_t id)
{
        for (const auto &property : device.properties)
                if (property.id == id)
                        return &property;
        return nullptr;
}

DeviceManager::Property *
DeviceManager::findProperty(DeviceInstance &instance, uint64_t id)
{
        for (auto &property : instance.properties)
                if (property.id == id)
                        return &property;
        return nullptr;
}

const DeviceManager::Property *
DeviceManager::findProperty(const DeviceInstance &instance, uint64_t id)
{
        for (const auto &property : instance.properties)
                if (property.id == id)
                        return &property;
        return nullptr;
}

std::string
DeviceManager::functionSignature(const CFuncDecl &function)
{
        std::string signature  = normalizeTypeText(function.retRaw, function.retType);
        signature             += '(';
        for (size_t i = 0; i < function.params.size(); ++i) {
                if (i)
                        signature += ',';
                signature += normalizeTypeText(function.params[i].rawType, function.params[i].type);
        }
        signature += ')';
        return signature;
}

std::string
DeviceManager::functionLabel(const CFuncDecl &function)
{
        return function.name + " : " + functionSignature(function);
}

const CFuncDecl *
DeviceManager::findFunction(const Device &device, const MethodBinding &method)
{
        for (const auto &function : device.declarations.functions)
                if (function.name == method.functionName && functionSignature(function) == method.signature)
                        return &function;
        return nullptr;
}

std::string
DeviceManager::propertyValueString(const Property &property)
{
        switch (property.type) {
                case CType::Bool:
                        return readScalar<uint8_t>(property.data) ? "true" : "false";
                case CType::I8:
                        return std::to_string(static_cast<int>(readScalar<int8_t>(property.data)));
                case CType::I16:
                        return std::to_string(readScalar<int16_t>(property.data));
                case CType::I32:
                        return std::to_string(readScalar<int32_t>(property.data));
                case CType::I64:
                        return std::to_string(readScalar<int64_t>(property.data));
                case CType::U8:
                        return std::to_string(static_cast<unsigned>(readScalar<uint8_t>(property.data)));
                case CType::U16:
                        return std::to_string(readScalar<uint16_t>(property.data));
                case CType::U32:
                        return std::to_string(readScalar<uint32_t>(property.data));
                case CType::U64:
                        return std::to_string(readScalar<uint64_t>(property.data));
                case CType::F32: {
                        std::ostringstream stream;
                        stream << std::setprecision(std::numeric_limits<float>::max_digits10)
                               << readScalar<float>(property.data);
                        return stream.str();
                }
                case CType::F64: {
                        std::ostringstream stream;
                        stream << std::setprecision(std::numeric_limits<double>::max_digits10)
                               << readScalar<double>(property.data);
                        return stream.str();
                }
                default:
                        return {};
        }
}

bool
DeviceManager::setPropertyValue(Property &property, const std::string &value)
{
        switch (property.type) {
                case CType::Bool: {
                        bool parsed = false;
                        if (!parseBool(value, parsed))
                                return false;
                        writeScalar<uint8_t>(property.data, parsed ? 1 : 0);
                        return true;
                }
                case CType::I8: {
                        int64_t parsed = 0;
                        if (!parseSigned(value, std::numeric_limits<int8_t>::min(), std::numeric_limits<int8_t>::max(), parsed))
                                return false;
                        writeScalar<int8_t>(property.data, static_cast<int8_t>(parsed));
                        return true;
                }
                case CType::I16: {
                        int64_t parsed = 0;
                        if (!parseSigned(
                                value, std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max(), parsed))
                                return false;
                        writeScalar<int16_t>(property.data, static_cast<int16_t>(parsed));
                        return true;
                }
                case CType::I32: {
                        int64_t parsed = 0;
                        if (!parseSigned(
                                value, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max(), parsed))
                                return false;
                        writeScalar<int32_t>(property.data, static_cast<int32_t>(parsed));
                        return true;
                }
                case CType::I64: {
                        int64_t parsed = 0;
                        if (!parseSigned(
                                value, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(), parsed))
                                return false;
                        writeScalar<int64_t>(property.data, parsed);
                        return true;
                }
                case CType::U8: {
                        uint64_t parsed = 0;
                        if (!parseUnsigned(value, std::numeric_limits<uint8_t>::max(), parsed))
                                return false;
                        writeScalar<uint8_t>(property.data, static_cast<uint8_t>(parsed));
                        return true;
                }
                case CType::U16: {
                        uint64_t parsed = 0;
                        if (!parseUnsigned(value, std::numeric_limits<uint16_t>::max(), parsed))
                                return false;
                        writeScalar<uint16_t>(property.data, static_cast<uint16_t>(parsed));
                        return true;
                }
                case CType::U32: {
                        uint64_t parsed = 0;
                        if (!parseUnsigned(value, std::numeric_limits<uint32_t>::max(), parsed))
                                return false;
                        writeScalar<uint32_t>(property.data, static_cast<uint32_t>(parsed));
                        return true;
                }
                case CType::U64: {
                        uint64_t parsed = 0;
                        if (!parseUnsigned(value, std::numeric_limits<uint64_t>::max(), parsed))
                                return false;
                        writeScalar<uint64_t>(property.data, parsed);
                        return true;
                }
                case CType::F32: {
                        float parsed = 0.0f;
                        if (!parseFloat32(value, parsed))
                                return false;
                        writeScalar<float>(property.data, parsed);
                        return true;
                }
                case CType::F64: {
                        double parsed = 0.0;
                        if (!parseFloat64(value, parsed))
                                return false;
                        writeScalar<double>(property.data, parsed);
                        return true;
                }
                default:
                        return false;
        }
}

bool
DeviceManager::assignResult(Property &property, const CallResult &result, CType returnType)
{
        if (property.type != returnType || !isScalarType(returnType))
                return false;
        const auto before = property.data;
        switch (returnType) {
                case CType::Bool:
                        writeScalar<uint8_t>(property.data, result.rawU64 ? 1 : 0);
                        break;
                case CType::I8:
                        writeScalar<int8_t>(property.data, static_cast<int8_t>(result.rawU64));
                        break;
                case CType::I16:
                        writeScalar<int16_t>(property.data, static_cast<int16_t>(result.rawU64));
                        break;
                case CType::I32:
                        writeScalar<int32_t>(property.data, static_cast<int32_t>(result.rawU64));
                        break;
                case CType::I64:
                        writeScalar<int64_t>(property.data, static_cast<int64_t>(result.rawU64));
                        break;
                case CType::U8:
                        writeScalar<uint8_t>(property.data, static_cast<uint8_t>(result.rawU64));
                        break;
                case CType::U16:
                        writeScalar<uint16_t>(property.data, static_cast<uint16_t>(result.rawU64));
                        break;
                case CType::U32:
                        writeScalar<uint32_t>(property.data, static_cast<uint32_t>(result.rawU64));
                        break;
                case CType::U64:
                        writeScalar<uint64_t>(property.data, result.rawU64);
                        break;
                case CType::F32:
                        writeScalar<float>(property.data, static_cast<float>(result.rawF64));
                        break;
                case CType::F64:
                        writeScalar<double>(property.data, result.rawF64);
                        break;
                default:
                        return false;
        }
        property.editValue        = propertyValueString(property);
        property.editValueInvalid = false;
        return property.data != before;
}

void
DeviceManager::createDevice(const std::string &requestedName)
{
        auto device = std::make_unique<Device>();
        device->id  = takeStableId(nextDeviceId_, [&](uint64_t id) {
                return std::any_of(devices_.begin(), devices_.end(), [&](const auto &existing) { return existing->id == id; });
        });
        if (device->id == 0) {
                managerStatus_        = tr("No device id is available.", "没有可用的设备 ID。");
                managerStatusIsError_ = true;
                return;
        }
        device->name = trimCopy(requestedName);
        if (device->name.empty())
                device->name = std::string(tr("Device Type", "设备类型")) + " " + std::to_string(device->id);
        devices_.push_back(std::move(device));
        selectedDevice_   = static_cast<int>(devices_.size()) - 1;
        selectedInstance_ = -1;
        selectedFunction_ = -1;
        newDeviceName_[0] = '\0';
        modified_         = true;
}

void
DeviceManager::deleteDevice(int index)
{
        if (index < 0 || index >= static_cast<int>(devices_.size()))
                return;
        devices_.erase(devices_.begin() + index);
        if (devices_.empty()) {
                selectedDevice_ = -1;
        } else if (selectedDevice_ > index) {
                --selectedDevice_;
        } else if (selectedDevice_ == index || selectedDevice_ >= static_cast<int>(devices_.size())) {
                selectedDevice_ = static_cast<int>(devices_.size()) - 1;
        }
        selectedInstance_ = -1;
        selectedFunction_ = -1;
        modified_         = true;
}

void
DeviceManager::createInstance(Device &device, const std::string &requestedName)
{
        DeviceInstance instance;
        instance.id = takeStableId(device.nextInstanceId, [&](uint64_t id) {
                return std::any_of(device.instances.begin(), device.instances.end(), [&](const DeviceInstance &existing) {
                        return existing.id == id;
                });
        });
        if (instance.id == 0) {
                device.status        = tr("No device-instance id is available.", "没有可用的具体设备 ID。");
                device.statusIsError = true;
                return;
        }
        instance.name = trimCopy(requestedName);
        if (instance.name.empty())
                instance.name = std::string(tr("Device", "设备")) + " " + std::to_string(instance.id);
        instance.properties = device.properties;
        for (auto &property : instance.properties) {
                property.editValue        = propertyValueString(property);
                property.editValueInvalid = false;
                property.editActive       = false;
                property.liveValueKnown   = false;
                property.liveReadOk       = false;
                property.writePending     = false;
                property.lastWriteOk      = true;
                property.writeToken       = 0u;
        }
        device.instances.push_back(std::move(instance));
        selectedInstance_ = static_cast<int>(device.instances.size()) - 1;
        modified_         = true;
}

void
DeviceManager::deleteInstance(Device &device, int index)
{
        if (index < 0 || index >= static_cast<int>(device.instances.size()))
                return;
        device.instances.erase(device.instances.begin() + index);
        selectedInstance_ = -1;
        modified_         = true;
}

void
DeviceManager::loadLibrary(Device &device)
{
        device.loader.unload();
        if (device.libraryPath.empty()) {
                device.status        = tr("Choose a dynamic library first.", "请先选择动态库。");
                device.statusIsError = true;
                return;
        }
        if (!device.loader.load(device.libraryPath)) {
                device.status        = device.loader.lastError();
                device.statusIsError = true;
                return;
        }
        device.status        = tr("Dynamic library loaded.", "动态库已加载。");
        device.statusIsError = false;
}

void
DeviceManager::loadHeader(Device &device)
{
        device.headerSource.clear();
        device.declarations = {};
        selectedFunction_   = -1;
        if (device.headerPath.empty()) {
                device.status        = tr("Choose a header file first.", "请先选择头文件。");
                device.statusIsError = true;
                return;
        }

        const std::filesystem::path path(reinterpret_cast<const char8_t *>(device.headerPath.c_str()));
        std::ifstream               input(path, std::ios::binary);
        if (!input) {
                device.status        = tr("Cannot open header file.", "无法打开头文件。");
                device.statusIsError = true;
                return;
        }
        std::ostringstream stream;
        stream << input.rdbuf();
        device.headerSource = stream.str();
        if (device.headerSource.empty()) {
                device.status        = tr("Header file is empty.", "头文件为空。");
                device.statusIsError = true;
                return;
        }
        device.declarations = parseHeaderFull(device.headerSource);
        char message[192];
        std::snprintf(message,
                      sizeof(message),
                      tr("Parsed %d top-level C function(s).", "已解析 %d 个顶层 C 函数。"),
                      static_cast<int>(device.declarations.functions.size()));
        device.status        = message;
        device.statusIsError = device.declarations.functions.empty();
}

void
DeviceManager::bindFunction(Device &device, const CFuncDecl &function)
{
        if (function.retType == CType::Unknown ||
            std::any_of(function.params.begin(), function.params.end(), [](const CParam &parameter) {
                    return parameter.type == CType::Unknown;
            })) {
                device.status        = tr("Functions with unknown parameter or return types cannot be bound.",
                                   "含未知参数或返回类型的函数无法绑定。");
                device.statusIsError = true;
                return;
        }
        if (function.isVariadic) {
                device.status        = tr("Variadic functions cannot be bound safely.", "可变参数函数无法安全绑定。");
                device.statusIsError = true;
                return;
        }
        MethodBinding method;
        method.id = takeStableId(device.nextMethodId, [&](uint64_t id) {
                return std::any_of(device.methods.begin(), device.methods.end(), [&](const MethodBinding &existing) {
                        return existing.id == id;
                });
        });
        if (method.id == 0) {
                device.status        = tr("No method id is available.", "没有可用的方法 ID。");
                device.statusIsError = true;
                return;
        }
        method.name         = function.name;
        method.functionName = function.name;
        method.exportSymbol = function.name;
        method.signature    = functionSignature(function);
        method.arguments.resize(function.params.size());
        device.methods.push_back(std::move(method));
        modified_ = true;
}

void
DeviceManager::callMethod(Device &device, DeviceInstance *instance, MethodBinding &method)
{
        method.lastResult.clear();
        method.lastResultOk = false;
        if (method.kind == MethodKind::Instance && !instance) {
                method.lastResult =
                    tr("Select a concrete device before calling this method.", "调用此方法前请选择一个具体设备。");
                return;
        }
        if (!device.loader.isLoaded()) {
                method.lastResult = tr("Dynamic library is not loaded.", "动态库未加载。");
                return;
        }
        const CFuncDecl *headerFunction = findFunction(device, method);
        if (!headerFunction) {
                method.lastResult = tr("The bound header function/signature is unresolved.", "绑定的头文件函数/签名无法解析。");
                return;
        }
        if (headerFunction->retType == CType::Unknown) {
                method.lastResult = tr("Unknown return type is not callable.", "未知返回类型不可调用。");
                return;
        }
        if (method.kind == MethodKind::Discovery && headerFunction->retType != CType::I32) {
                method.lastResult = tr("Discovery functions must return int32_t.", "发现函数必须返回 int32_t。");
                return;
        }
        if (headerFunction->isVariadic) {
                method.lastResult = tr("Variadic functions are not callable.", "可变参数函数不可调用。");
                return;
        }
        if (method.arguments.size() != headerFunction->params.size()) {
                method.lastResult = tr("Saved argument count does not match the header.", "保存的参数数量与头文件不匹配。");
                return;
        }

        auto propertyById = [&](uint64_t id) -> Property * {
                return instance ? findProperty(*instance, id) : findProperty(device, id);
        };

        std::vector<char> discoveryBuffer;
        if (method.kind == MethodKind::Discovery) {
                if (method.discoveryBufferArg < 0 || method.discoveryCapacityArg < 0 ||
                    method.discoveryBufferArg == method.discoveryCapacityArg ||
                    method.discoveryBufferArg >= static_cast<int>(headerFunction->params.size()) ||
                    method.discoveryCapacityArg >= static_cast<int>(headerFunction->params.size())) {
                        method.lastResult = tr("Configure distinct JSON-buffer and capacity arguments.",
                                               "请配置不同的 JSON 缓冲区参数和容量参数。");
                        return;
                }
                const CParam &bufferParam = headerFunction->params[method.discoveryBufferArg];
                bool          bufferConst = false;
                const auto    bufferType  = pointerPointeeType(bufferParam, bufferConst);
                if (!bufferType || bufferConst || !isCharacterPointer(bufferParam)) {
                        method.lastResult =
                            tr("The discovery buffer must be a writable char pointer.", "发现缓冲区必须是可写 char 指针。");
                        return;
                }
                if (!isCapacityType(headerFunction->params[method.discoveryCapacityArg].type)) {
                        method.lastResult =
                            tr("The discovery capacity must be an integer scalar argument.", "发现容量必须是整数标量参数。");
                        return;
                }
                const uint32_t size = std::clamp(method.discoveryBufferSize, 1024u, 4u * 1024u * 1024u);
                discoveryBuffer.assign(size, '\x7f');
                discoveryBuffer.back() = '\0';
        }

        Property *resultProperty = nullptr;
        if (method.resultPropertyId != 0) {
                resultProperty = propertyById(method.resultPropertyId);
                if (!resultProperty) {
                        method.lastResult = tr("Return target property no longer exists.", "返回值目标属性已不存在。");
                        return;
                }
                if (!isScalarType(headerFunction->retType) || resultProperty->type != headerFunction->retType) {
                        method.lastResult = tr("Return target type does not exactly match the function return type.",
                                               "返回值目标类型与函数返回类型不完全匹配。");
                        return;
                }
        }

        struct alignas(16) Scratch {
                uint64_t propertyId{0};
                CType    type{CType::Unknown};
                alignas(16) std::array<uint8_t, 16> data{};
        };

        std::vector<std::string> arguments(headerFunction->params.size());
        std::vector<void *>      pointerOverrides(headerFunction->params.size(), nullptr);
        std::vector<Scratch>     scratch;
        scratch.reserve(headerFunction->params.size());
        std::unordered_map<uint64_t, size_t> scratchByProperty;

        auto scalarArgument = [](const Property &property) {
                if (property.type == CType::Bool)
                        return readScalar<uint8_t>(property.data) ? std::string("1") : std::string("0");
                return propertyValueString(property);
        };

        // First validate and create all scratch buffers. Nothing in the live property
        // model is exposed to the DLL before every binding has passed validation.
        for (size_t i = 0; i < headerFunction->params.size(); ++i) {
                const CParam          &parameter = headerFunction->params[i];
                const ArgumentBinding &binding   = method.arguments[i];
                if (method.kind == MethodKind::Discovery && static_cast<int>(i) == method.discoveryBufferArg) {
                        arguments[i] = "0";
                        continue;
                }
                if (method.kind == MethodKind::Discovery && static_cast<int>(i) == method.discoveryCapacityArg) {
                        arguments[i] = std::to_string(discoveryBuffer.size());
                        continue;
                }
                if (method.kind == MethodKind::Discovery && binding.source == ArgSource::PropertyAddress) {
                        method.lastResult = tr("Discovery methods cannot write type properties through pointer bindings.",
                                               "发现方法不能通过指针绑定写入类型属性。");
                        return;
                }
                if (parameter.type == CType::Unknown) {
                        method.lastResult = std::string(tr("Unknown parameter type: ", "未知参数类型：")) +
                                            (parameter.name.empty() ? std::to_string(i) : parameter.name);
                        return;
                }
                if (binding.source == ArgSource::Literal) {
                        if (isScalarType(parameter.type)) {
                                Property parsed;
                                parsed.type = parameter.type;
                                if (!setPropertyValue(parsed, binding.literal)) {
                                        method.lastResult =
                                            std::string(tr("Invalid literal for parameter: ", "参数常量无效：")) +
                                            (parameter.name.empty() ? std::to_string(i) : parameter.name);
                                        return;
                                }
                                arguments[i] = scalarArgument(parsed);
                        } else if (isStrictConstCharPointer(parameter)) {
                                arguments[i] = binding.literal;
                        } else if (parameter.type == CType::Ptr) {
                                const std::string literal = lowerCopy(trimCopy(binding.literal));
                                if (!literal.empty() && literal != "0" && literal != "null" && literal != "nullptr") {
                                        method.lastResult =
                                            tr("Pointer literals only accept null; bind a property address instead.",
                                               "指针常量只允许空指针；请改为绑定属性地址。");
                                        return;
                                }
                                arguments[i] = "0";
                        } else {
                                method.lastResult =
                                    tr("Unknown parameter types cannot be marshalled.", "未知参数类型无法封送。");
                                return;
                        }
                } else if (binding.source == ArgSource::PropertyValue) {
                        const Property *property = propertyById(binding.propertyId);
                        if (!property) {
                                method.lastResult = tr("A value-bound property no longer exists.", "按值绑定的属性已不存在。");
                                return;
                        }
                        if (!isScalarType(parameter.type) || parameter.type != property->type) {
                                method.lastResult = tr("A value-bound property must exactly match the scalar parameter type.",
                                                       "按值绑定的属性必须与标量参数类型完全匹配。");
                                return;
                        }
                        arguments[i] = scalarArgument(*property);
                } else {
                        Property *property = propertyById(binding.propertyId);
                        if (!property) {
                                method.lastResult =
                                    tr("An address-bound property no longer exists.", "按地址绑定的属性已不存在。");
                                return;
                        }
                        bool       pointeeConst = false;
                        const auto pointee      = pointerPointeeType(parameter, pointeeConst);
                        if (!pointee || pointeeConst || isCharacterPointer(parameter) || *pointee != property->type) {
                                method.lastResult =
                                    tr("PropertyAddress requires a writable, recognized scalar pointer of the exact same type.",
                                       "属性地址要求可写、可识别且类型完全相同的标量指针。");
                                return;
                        }
                        auto [it, inserted] = scratchByProperty.emplace(property->id, scratch.size());
                        if (inserted) {
                                Scratch entry;
                                entry.propertyId = property->id;
                                entry.type       = property->type;
                                entry.data       = property->data;
                                scratch.push_back(entry);
                        }
                        arguments[i] = "0";
                }
        }

        for (size_t i = 0; i < headerFunction->params.size(); ++i) {
                const auto &binding = method.arguments[i];
                if (method.kind == MethodKind::Discovery &&
                    (static_cast<int>(i) == method.discoveryBufferArg || static_cast<int>(i) == method.discoveryCapacityArg))
                        continue;
                if (binding.source == ArgSource::PropertyAddress) {
                        const auto found = scratchByProperty.find(binding.propertyId);
                        if (found == scratchByProperty.end()) {
                                method.lastResult = tr("Internal scratch binding error.", "内部临时缓冲绑定错误。");
                                return;
                        }
                        pointerOverrides[i] = scratch[found->second].data.data();
                }
        }
        if (method.kind == MethodKind::Discovery)
                pointerOverrides[static_cast<size_t>(method.discoveryBufferArg)] = discoveryBuffer.data();

        CFuncDecl callable = *headerFunction;
        callable.name      = trimCopy(method.exportSymbol);
        if (callable.name.empty()) {
                method.lastResult = tr("Export symbol cannot be empty.", "导出符号不能为空。");
                return;
        }
        const CallResult result = device.loader.call(callable, arguments, pointerOverrides);
        method.lastResult       = result.ok ? result.display : result.error;
        method.lastResultOk     = result.ok;
        if (!result.ok)
                return;

        // A non-finite value cannot round-trip through the strict persisted-value
        // parser. Reject the whole transaction before exposing any DLL writes.
        for (auto &entry : scratch) {
                if (entry.type == CType::F32 && !std::isfinite(readScalar<float>(entry.data))) {
                        method.lastResult   = tr("Call produced a non-finite float; no property was updated.",
                                               "调用产生非有限 float；未更新任何属性。");
                        method.lastResultOk = false;
                        return;
                }
                if (entry.type == CType::F64 && !std::isfinite(readScalar<double>(entry.data))) {
                        method.lastResult   = tr("Call produced a non-finite double; no property was updated.",
                                               "调用产生非有限 double；未更新任何属性。");
                        method.lastResultOk = false;
                        return;
                }
                if (entry.type == CType::Bool)
                        writeScalar<uint8_t>(entry.data, readScalar<uint8_t>(entry.data) ? 1 : 0);
        }
        if (resultProperty && (headerFunction->retType == CType::F32 || headerFunction->retType == CType::F64) &&
            !std::isfinite(result.rawF64)) {
                method.lastResult =
                    tr("Call returned a non-finite value; no property was updated.", "调用返回非有限值；未更新任何属性。");
                method.lastResultOk = false;
                return;
        }

        if (method.kind == MethodKind::Discovery) {
                const bool signedReturn = headerFunction->retType == CType::I8 || headerFunction->retType == CType::I16 ||
                                          headerFunction->retType == CType::I32 || headerFunction->retType == CType::I64;
                if (signedReturn && static_cast<int64_t>(result.rawU64) < 0) {
                        method.lastResult =
                            std::string(tr("Discovery function returned an error: ", "发现函数返回错误：")) + result.display;
                        method.lastResultOk = false;
                        return;
                }
                if (std::find(discoveryBuffer.begin(), discoveryBuffer.end(), '\0') == discoveryBuffer.end()) {
                        method.lastResult   = tr("Discovery output was not NUL-terminated within the configured capacity.",
                                               "发现在配置容量内未输出 NUL 结尾的 JSON。");
                        method.lastResultOk = false;
                        return;
                }
                std::string    discoveryError;
                bool           discoveryChanged = false;
                const uint32_t expectedCount    = static_cast<uint32_t>(static_cast<int32_t>(result.rawU64));
                if (!applyDiscoveryJson(device, discoveryBuffer.data(), expectedCount, discoveryError, discoveryChanged)) {
                        method.lastResult   = discoveryError;
                        method.lastResultOk = false;
                        return;
                }
                method.lastResult   = discoveryError;
                method.lastResultOk = true;
                modified_           = modified_ || discoveryChanged;
        }

        bool changed = false;
        for (const auto &entry : scratch) {
                if (Property *property = propertyById(entry.propertyId)) {
                        if (property->data != entry.data) {
                                property->data             = entry.data;
                                property->editValue        = propertyValueString(*property);
                                property->editValueInvalid = false;
                                if (property->source == PropertySource::JLink)
                                        queuePropertyWrite(*property);
                                changed = true;
                        }
                }
        }
        if (resultProperty) {
                const bool resultChanged = assignResult(*resultProperty, result, headerFunction->retType);
                if (resultChanged && resultProperty->source == PropertySource::JLink)
                        queuePropertyWrite(*resultProperty);
                changed = resultChanged || changed;
        }
        modified_ = modified_ || changed;
}

bool
DeviceManager::applyDiscoveryJson(Device &device, const char *json, uint32_t expectedCount, std::string &error, bool &changed)
{
        changed           = false;
        const char *input = json ? json : "";
        cJSON      *root  = cJSON_ParseWithLengthOpts(input, std::strlen(input) + 1, nullptr, true);
        if (!root) {
                error = tr("Discovery returned invalid JSON.", "发现方法返回了无效 JSON。");
                return false;
        }
        const cJSON *items = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "devices");
        if (!cJSON_IsArray(items)) {
                cJSON_Delete(root);
                error = tr("Discovery JSON must be an array or contain a devices array.",
                           "发现 JSON 必须是数组，或包含 devices 数组。");
                return false;
        }

        struct Discovered {
                uint64_t              id{0};
                std::string           key;
                std::string           name;
                std::vector<Property> properties;
        };
        std::vector<Discovered>         staged;
        std::unordered_set<std::string> keys;
        std::unordered_set<std::string> propertyKeys;
        for (const auto &property : device.properties) {
                if (trimCopy(property.discoveryKey).empty() || !propertyKeys.insert(property.discoveryKey).second) {
                        cJSON_Delete(root);
                        error = tr("Property discovery keys must be non-empty and unique.", "属性发现字段键必须非空且唯一。");
                        return false;
                }
        }
        for (const cJSON *item = items->child; item; item = item->next) {
                if (!cJSON_IsObject(item)) {
                        cJSON_Delete(root);
                        error = tr("Every discovered device must be a JSON object.", "每个发现的设备都必须是 JSON 对象。");
                        return false;
                }
                const cJSON *key = cJSON_GetObjectItemCaseSensitive(item, "key");
                if (!cJSON_IsString(key) || !key->valuestring || trimCopy(key->valuestring).empty()) {
                        cJSON_Delete(root);
                        error =
                            tr("Every discovered device needs a non-empty string key.", "每个发现的设备都需要非空字符串 key。");
                        return false;
                }
                Discovered entry;
                entry.key = trimCopy(key->valuestring);
                if (!keys.insert(entry.key).second) {
                        cJSON_Delete(root);
                        error = tr("Discovery JSON contains duplicate device keys.", "发现 JSON 中包含重复的设备 key。");
                        return false;
                }
                const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
                if (name && (!cJSON_IsString(name) || !name->valuestring || trimCopy(name->valuestring).empty())) {
                        cJSON_Delete(root);
                        error =
                            tr("A discovered device name must be a non-empty string.", "发现设备的 name 必须是非空字符串。");
                        return false;
                }
                entry.name = name ? trimCopy(name->valuestring) : entry.key;
                const auto existing =
                    std::find_if(device.instances.begin(), device.instances.end(), [&](const DeviceInstance &instance) {
                            return instance.discoveryKey == entry.key;
                    });
                entry.properties    = existing == device.instances.end() ? device.properties : existing->properties;
                const cJSON *values = cJSON_GetObjectItemCaseSensitive(item, "properties");
                if (values && !cJSON_IsObject(values)) {
                        cJSON_Delete(root);
                        error = tr("Discovered properties must be a JSON object.", "发现设备的 properties 必须是 JSON 对象。");
                        return false;
                }
                if (cJSON_IsObject(values)) {
                        std::unordered_set<std::string> valueKeys;
                        for (const cJSON *value = values->child; value; value = value->next) {
                                const std::string valueKey = value->string ? value->string : "";
                                if (valueKey.empty() || !valueKeys.insert(valueKey).second ||
                                    propertyKeys.count(valueKey) == 0) {
                                        cJSON_Delete(root);
                                        error = std::string(tr("Unknown or duplicate discovered property key: ",
                                                               "未知或重复的发现属性字段键：")) +
                                                valueKey;
                                        return false;
                                }
                        }
                        for (auto &property : entry.properties) {
                                const cJSON *value = cJSON_GetObjectItemCaseSensitive(values, property.discoveryKey.c_str());
                                if (!value)
                                        continue;
                                std::string text;
                                if (cJSON_IsString(value) && value->valuestring) {
                                        text = value->valuestring;
                                } else if (cJSON_IsBool(value)) {
                                        text = cJSON_IsTrue(value) ? "true" : "false";
                                } else if (cJSON_IsNumber(value)) {
                                        if (property.type == CType::I64 || property.type == CType::U64) {
                                                cJSON_Delete(root);
                                                error = std::string(tr("64-bit discovered integers must be JSON strings: ",
                                                                       "64 位发现整数必须使用 JSON 字符串：")) +
                                                        property.discoveryKey;
                                                return false;
                                        }
                                        char number[64];
                                        std::snprintf(number, sizeof(number), "%.17g", value->valuedouble);
                                        text = number;
                                } else {
                                        cJSON_Delete(root);
                                        error = std::string(tr("Unsupported discovered value for property: ",
                                                               "发现属性值类型不受支持：")) +
                                                property.name;
                                        return false;
                                }
                                if (!setPropertyValue(property, text)) {
                                        cJSON_Delete(root);
                                        error = std::string(tr("Invalid discovered value for property: ", "发现属性值无效：")) +
                                                property.name;
                                        return false;
                                }
                                property.editValue = propertyValueString(property);
                        }
                }
                staged.push_back(std::move(entry));
        }
        cJSON_Delete(root);
        if (staged.size() != expectedCount) {
                error = tr("Discovery return count does not match the JSON device count.",
                           "发现函数返回数量与 JSON 设备数量不一致。");
                return false;
        }

        uint64_t                     nextInstanceId = device.nextInstanceId;
        std::unordered_set<uint64_t> allocatedIds;
        for (auto &entry : staged) {
                const bool exists =
                    std::any_of(device.instances.begin(), device.instances.end(), [&](const DeviceInstance &instance) {
                            return instance.discoveryKey == entry.key;
                    });
                if (exists)
                        continue;
                entry.id = takeStableId(nextInstanceId, [&](uint64_t id) {
                        return allocatedIds.count(id) != 0 ||
                               std::any_of(device.instances.begin(),
                                           device.instances.end(),
                                           [&](const DeviceInstance &instance) { return instance.id == id; });
                });
                if (entry.id == 0) {
                        error = tr("No device-instance id is available for discovery.", "没有可用于发现设备的具体设备 ID。");
                        return false;
                }
                allocatedIds.insert(entry.id);
        }

        for (auto &instance : device.instances)
                if (!instance.discoveryKey.empty())
                        instance.online = false;
        auto propertiesEqual = [](const std::vector<Property> &left, const std::vector<Property> &right) {
                if (left.size() != right.size())
                        return false;
                for (size_t index = 0; index < left.size(); ++index) {
                        if (left[index].id != right[index].id || left[index].name != right[index].name ||
                            left[index].discoveryKey != right[index].discoveryKey || left[index].type != right[index].type ||
                            left[index].data != right[index].data)
                                return false;
                }
                return true;
        };
        int created = 0;
        int updated = 0;
        for (auto &entry : staged) {
                auto found = std::find_if(device.instances.begin(),
                                          device.instances.end(),
                                          [&](const DeviceInstance &instance) { return instance.discoveryKey == entry.key; });
                if (found != device.instances.end()) {
                        if (found->discoveredName != entry.name || (!found->customName && found->name != entry.name) ||
                            !propertiesEqual(found->properties, entry.properties))
                                changed = true;
                        found->discoveredName = entry.name;
                        if (!found->customName)
                                found->name = entry.name;
                        found->properties = std::move(entry.properties);
                        found->online     = true;
                        ++updated;
                        continue;
                }
                DeviceInstance instance;
                instance.id             = entry.id;
                instance.name           = entry.name;
                instance.discoveredName = std::move(entry.name);
                instance.discoveryKey   = std::move(entry.key);
                instance.properties     = std::move(entry.properties);
                instance.online         = true;
                device.instances.push_back(std::move(instance));
                changed = true;
                ++created;
        }
        device.nextInstanceId = nextInstanceId;
        char message[192];
        std::snprintf(message,
                      sizeof(message),
                      tr("Discovery complete: %d new, %d updated.", "发现完成：新增 %d 个，更新 %d 个。"),
                      created,
                      updated);
        error = message;
        return true;
}

void
DeviceManager::saveDevice(void *node, const Device &device, const std::string &baseDir) const
{
        cJSON *object = static_cast<cJSON *>(node);
        addId(object, "id", device.id);
        const std::string deviceName =
            trimCopy(device.name).empty() ? std::string("Device Type ") + std::to_string(device.id) : device.name;
        cJSON_AddStringToObject(object, "name", deviceName.c_str());
        cJSON_AddStringToObject(object, "libraryPath", pathForStorage(device.libraryPath, baseDir).c_str());
        cJSON_AddStringToObject(object, "headerPath", pathForStorage(device.headerPath, baseDir).c_str());
        addId(object, "nextPropertyId", device.nextPropertyId);
        addId(object, "nextMethodId", device.nextMethodId);
        addId(object, "nextInstanceId", device.nextInstanceId);

        cJSON *properties = cJSON_CreateArray();
        for (const auto &property : device.properties) {
                cJSON *item = cJSON_CreateObject();
                addId(item, "id", property.id);
                const std::string propertyName =
                    trimCopy(property.name).empty() ? std::string("Property ") + std::to_string(property.id) : property.name;
                cJSON_AddStringToObject(item, "name", propertyName.c_str());
                const std::string propertyDiscoveryKey = trimCopy(property.discoveryKey).empty()
                                                             ? "property_" + std::to_string(property.id)
                                                             : trimCopy(property.discoveryKey);
                cJSON_AddStringToObject(item, "discoveryKey", propertyDiscoveryKey.c_str());
                cJSON_AddStringToObject(item, "type", stableTypeName(property.type));
                const std::string value = propertyValueString(property);
                cJSON_AddStringToObject(item, "value", value.c_str());
                cJSON_AddStringToObject(item, "source", property.source == PropertySource::JLink ? "jlink" : "manual");
                cJSON_AddStringToObject(item, "symbol", property.symbol.c_str());
                cJSON_AddStringToObject(item, "address", hexAddress(property.address).c_str());
                cJSON_AddBoolToObject(item, "writable", property.writable);
                cJSON_AddNumberToObject(item, "bitOffset", property.bitOffset);
                cJSON_AddNumberToObject(item, "bitSize", property.bitSize);
                cJSON_AddItemToArray(properties, item);
        }
        cJSON_AddItemToObject(object, "properties", properties);

        cJSON *methods = cJSON_CreateArray();
        for (const auto &method : device.methods) {
                cJSON *item = cJSON_CreateObject();
                addId(item, "id", method.id);
                const std::string methodName = trimCopy(method.name).empty() ? method.functionName : method.name;
                const std::string exportSymbol =
                    trimCopy(method.exportSymbol).empty() ? method.functionName : method.exportSymbol;
                cJSON_AddStringToObject(item, "name", methodName.c_str());
                cJSON_AddStringToObject(item, "functionName", method.functionName.c_str());
                cJSON_AddStringToObject(item, "signature", method.signature.c_str());
                cJSON_AddStringToObject(item, "exportSymbol", exportSymbol.c_str());
                addId(item, "resultPropertyId", method.resultPropertyId);
                cJSON_AddStringToObject(item, "kind", method.kind == MethodKind::Discovery ? "discoveryJson" : "instance");
                cJSON_AddNumberToObject(item, "discoveryBufferArg", method.discoveryBufferArg);
                cJSON_AddNumberToObject(item, "discoveryCapacityArg", method.discoveryCapacityArg);
                cJSON_AddNumberToObject(item, "discoveryBufferSize", method.discoveryBufferSize);

                cJSON *arguments = cJSON_CreateArray();
                for (const auto &argument : method.arguments) {
                        cJSON *arg = cJSON_CreateObject();
                        cJSON_AddStringToObject(arg, "source", argSourceName(static_cast<int>(argument.source)));
                        cJSON_AddStringToObject(arg, "literal", argument.literal.c_str());
                        addId(arg, "propertyId", argument.propertyId);
                        cJSON_AddItemToArray(arguments, arg);
                }
                cJSON_AddItemToObject(item, "arguments", arguments);
                cJSON_AddItemToArray(methods, item);
        }
        cJSON_AddItemToObject(object, "methods", methods);

        cJSON *instances = cJSON_CreateArray();
        for (const auto &instance : device.instances) {
                cJSON *item = cJSON_CreateObject();
                addId(item, "id", instance.id);
                const std::string instanceName =
                    trimCopy(instance.name).empty() ? std::string("Device ") + std::to_string(instance.id) : instance.name;
                cJSON_AddStringToObject(item, "name", instanceName.c_str());
                cJSON_AddStringToObject(item, "discoveryKey", instance.discoveryKey.c_str());
                cJSON_AddStringToObject(item, "discoveredName", instance.discoveredName.c_str());
                cJSON_AddBoolToObject(item, "customName", instance.customName);
                cJSON *values = cJSON_CreateArray();
                for (const auto &property : instance.properties) {
                        cJSON *value = cJSON_CreateObject();
                        addId(value, "propertyId", property.id);
                        const std::string text = propertyValueString(property);
                        cJSON_AddStringToObject(value, "value", text.c_str());
                        cJSON_AddStringToObject(value, "source", property.source == PropertySource::JLink ? "jlink" : "manual");
                        cJSON_AddStringToObject(value, "symbol", property.symbol.c_str());
                        cJSON_AddStringToObject(value, "address", hexAddress(property.address).c_str());
                        cJSON_AddBoolToObject(value, "writable", property.writable);
                        cJSON_AddNumberToObject(value, "bitOffset", property.bitOffset);
                        cJSON_AddNumberToObject(value, "bitSize", property.bitSize);
                        cJSON_AddItemToArray(values, value);
                }
                cJSON_AddItemToObject(item, "values", values);
                cJSON_AddItemToArray(instances, item);
        }
        cJSON_AddItemToObject(object, "instances", instances);
}

std::unique_ptr<DeviceManager::Device>
DeviceManager::loadDevice(const void *node, const std::string &baseDir, std::string &error)
{
        const cJSON *object = static_cast<const cJSON *>(node);
        if (!cJSON_IsObject(object)) {
                error = tr("Device entry is not an object.", "设备条目不是对象。");
                return nullptr;
        }

        auto device = std::make_unique<Device>();
        if (!readId(object, "id", device->id) || device->id == 0) {
                error = tr("Device has an invalid id.", "设备 ID 无效。");
                return nullptr;
        }
        const cJSON *name = cJSON_GetObjectItem(object, "name");
        if (!cJSON_IsString(name) || !name->valuestring || trimCopy(name->valuestring).empty()) {
                error = tr("Device has an invalid name.", "设备名称无效。");
                return nullptr;
        }
        device->name = name->valuestring;

        if (const cJSON *path = cJSON_GetObjectItem(object, "libraryPath"); path) {
                if (!cJSON_IsString(path)) {
                        error = tr("Device libraryPath must be a string.", "设备 libraryPath 必须是字符串。");
                        return nullptr;
                }
                device->libraryPath = pathFromStorage(path->valuestring, baseDir);
        }
        if (const cJSON *path = cJSON_GetObjectItem(object, "headerPath"); path) {
                if (!cJSON_IsString(path)) {
                        error = tr("Device headerPath must be a string.", "设备 headerPath 必须是字符串。");
                        return nullptr;
                }
                device->headerPath = pathFromStorage(path->valuestring, baseDir);
        }

        auto readPropertyBinding = [&](const cJSON *item, Property &property) {
                const cJSON *source = cJSON_GetObjectItem(item, "source");
                if (!source)
                        return true;
                if (!cJSON_IsString(source) || !source->valuestring) {
                        error = tr("Property source must be a string.", "属性 source 必须是字符串。");
                        return false;
                }
                const std::string sourceName = lowerCopy(trimCopy(source->valuestring));
                if (sourceName == "manual") {
                        property.source = PropertySource::Manual;
                        property.symbol.clear();
                        property.address   = 0u;
                        property.bitOffset = 0u;
                        property.bitSize   = 0u;
                        return true;
                }
                if (sourceName != "jlink") {
                        error = tr("Property uses an unsupported data source.", "属性使用了不支持的数据源。");
                        return false;
                }
                const cJSON *symbol  = cJSON_GetObjectItem(item, "symbol");
                const cJSON *address = cJSON_GetObjectItem(item, "address");
                if (!cJSON_IsString(symbol) || !symbol->valuestring || trimCopy(symbol->valuestring).empty() ||
                    (!cJSON_IsString(address) && !cJSON_IsNumber(address))) {
                        error = tr("J-Link property binding is incomplete.", "J-Link 属性绑定不完整。");
                        return false;
                }
                uint64_t parsedAddress = 0u;
                if (cJSON_IsString(address)) {
                        char       *end  = nullptr;
                        const char *text = address->valuestring ? address->valuestring : "";
                        parsedAddress    = std::strtoull(text, &end, 0);
                        if (!end || end == text || *end != 0)
                                parsedAddress = 0u;
                } else if (std::isfinite(address->valuedouble) && address->valuedouble >= 0.0 &&
                           address->valuedouble <= static_cast<double>(std::numeric_limits<uint32_t>::max()) &&
                           std::floor(address->valuedouble) == address->valuedouble) {
                        parsedAddress = static_cast<uint64_t>(address->valuedouble);
                }
                if (parsedAddress == 0u || parsedAddress > std::numeric_limits<uint32_t>::max()) {
                        error = tr("J-Link property address is invalid.", "J-Link 属性地址无效。");
                        return false;
                }
                auto readU32 = [&](const char *key, uint32_t &destination) {
                        const cJSON *value = cJSON_GetObjectItem(item, key);
                        if (!value)
                                return true;
                        if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble < 0.0 ||
                            value->valuedouble > static_cast<double>(std::numeric_limits<uint32_t>::max()) ||
                            std::floor(value->valuedouble) != value->valuedouble)
                                return false;
                        destination = static_cast<uint32_t>(value->valuedouble);
                        return true;
                };
                uint32_t       bitOffset = 0u;
                uint32_t       bitSize   = 0u;
                const uint32_t width     = scalarTypeSize(property.type) * 8u;
                if (!readU32("bitOffset", bitOffset) || !readU32("bitSize", bitSize) ||
                    (bitSize != 0u && (bitOffset >= width || bitSize > width - bitOffset))) {
                        error = tr("J-Link property bit-field is invalid.", "J-Link 属性位域无效。");
                        return false;
                }
                bool writable = true;
                if (const cJSON *value = cJSON_GetObjectItem(item, "writable"); value) {
                        if (!cJSON_IsBool(value)) {
                                error = tr("Property writable flag must be boolean.", "属性 writable 必须是布尔值。");
                                return false;
                        }
                        writable = cJSON_IsTrue(value);
                }
                property.source         = PropertySource::JLink;
                property.symbol         = trimCopy(symbol->valuestring);
                property.address        = static_cast<uint32_t>(parsedAddress);
                property.bitOffset      = bitOffset;
                property.bitSize        = bitSize;
                property.writable       = writable;
                property.symbolResolved = false;
                return true;
        };

        std::unordered_set<uint64_t>    propertyIds;
        std::unordered_set<std::string> propertyDiscoveryKeys;
        const cJSON                    *properties = cJSON_GetObjectItem(object, "properties");
        if (properties && !cJSON_IsArray(properties)) {
                error = tr("Device properties must be an array.", "设备属性必须是数组。");
                return nullptr;
        }
        if (cJSON_IsArray(properties)) {
                for (const cJSON *item = properties->child; item; item = item->next) {
                        if (!cJSON_IsObject(item)) {
                                error = tr("Property entry is not an object.", "属性条目不是对象。");
                                return nullptr;
                        }
                        Property property;
                        if (!readId(item, "id", property.id) || property.id == 0 || !propertyIds.insert(property.id).second) {
                                error = tr("Property has a missing, zero, or duplicate id.", "属性 ID 缺失、为零或重复。");
                                return nullptr;
                        }
                        const cJSON *propertyName  = cJSON_GetObjectItem(item, "name");
                        const cJSON *propertyType  = cJSON_GetObjectItem(item, "type");
                        const cJSON *propertyValue = cJSON_GetObjectItem(item, "value");
                        if (!cJSON_IsString(propertyName) || !propertyName->valuestring ||
                            trimCopy(propertyName->valuestring).empty() || !cJSON_IsString(propertyType) ||
                            !propertyType->valuestring || !cJSON_IsString(propertyValue) || !propertyValue->valuestring) {
                                error = tr("Property name, type, and value must be valid strings.",
                                           "属性名称、类型和值必须是有效字符串。");
                                return nullptr;
                        }
                        const auto type = stableTypeFromName(propertyType->valuestring);
                        if (!type || !isScalarType(*type)) {
                                error = tr("Property uses an unknown or unsupported type.", "属性使用未知或不支持的类型。");
                                return nullptr;
                        }
                        property.name = propertyName->valuestring;
                        if (const cJSON *key = cJSON_GetObjectItem(item, "discoveryKey"); key) {
                                if (!cJSON_IsString(key) || !key->valuestring || trimCopy(key->valuestring).empty()) {
                                        error = tr("Property discoveryKey must be a non-empty string.",
                                                   "属性 discoveryKey 必须是非空字符串。");
                                        return nullptr;
                                }
                                property.discoveryKey = trimCopy(key->valuestring);
                        } else {
                                // v1 migration: old discovery JSON used the editable property name.
                                property.discoveryKey = property.name;
                        }
                        if (!propertyDiscoveryKeys.insert(property.discoveryKey).second) {
                                error = tr("Property discoveryKey must be unique within its type.",
                                           "同一设备类型内的属性 discoveryKey 必须唯一。");
                                return nullptr;
                        }
                        property.type = *type;
                        if (!setPropertyValue(property, propertyValue->valuestring)) {
                                error = std::string(tr("Invalid exact property value: ", "无效的精确属性值：")) + property.name;
                                return nullptr;
                        }
                        if (!readPropertyBinding(item, property))
                                return nullptr;
                        property.editValue = propertyValueString(property);
                        device->properties.push_back(std::move(property));
                }
        }

        std::unordered_set<uint64_t> methodIds;
        const cJSON                 *methods = cJSON_GetObjectItem(object, "methods");
        if (methods && !cJSON_IsArray(methods)) {
                error = tr("Device methods must be an array.", "设备方法必须是数组。");
                return nullptr;
        }
        if (cJSON_IsArray(methods)) {
                for (const cJSON *item = methods->child; item; item = item->next) {
                        if (!cJSON_IsObject(item)) {
                                error = tr("Method entry is not an object.", "方法条目不是对象。");
                                return nullptr;
                        }
                        MethodBinding method;
                        if (!readId(item, "id", method.id) || method.id == 0 || !methodIds.insert(method.id).second) {
                                error = tr("Method has a missing, zero, or duplicate id.", "方法 ID 缺失、为零或重复。");
                                return nullptr;
                        }
                        auto readNonEmpty = [&](const char *key, std::string &destination) {
                                const cJSON *value = cJSON_GetObjectItem(item, key);
                                if (!cJSON_IsString(value) || !value->valuestring || trimCopy(value->valuestring).empty())
                                        return false;
                                destination = value->valuestring;
                                return true;
                        };
                        if (!readNonEmpty("name", method.name) || !readNonEmpty("functionName", method.functionName) ||
                            !readNonEmpty("signature", method.signature) ||
                            !readNonEmpty("exportSymbol", method.exportSymbol)) {
                                error = tr("Method name/function/signature/export symbol is invalid.",
                                           "方法名称/函数/签名/导出符号无效。");
                                return nullptr;
                        }
                        if (!readId(item, "resultPropertyId", method.resultPropertyId, false)) {
                                error = tr("Method resultPropertyId is invalid.", "方法 resultPropertyId 无效。");
                                return nullptr;
                        }
                        if (const cJSON *kind = cJSON_GetObjectItem(item, "kind"); kind) {
                                if (!cJSON_IsString(kind) || !kind->valuestring) {
                                        error = tr("Method kind is invalid.", "方法 kind 无效。");
                                        return nullptr;
                                }
                                if (std::strcmp(kind->valuestring, "instance") == 0)
                                        method.kind = MethodKind::Instance;
                                else if (std::strcmp(kind->valuestring, "discoveryJson") == 0)
                                        method.kind = MethodKind::Discovery;
                                else {
                                        error = tr("Method kind is unknown.", "方法 kind 未知。");
                                        return nullptr;
                                }
                        }
                        auto readOptionalInt = [&](const char *key, int &destination) {
                                const cJSON *value = cJSON_GetObjectItem(item, key);
                                if (!value)
                                        return true;
                                if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
                                    value->valuedouble < static_cast<double>(std::numeric_limits<int>::min()) ||
                                    value->valuedouble > static_cast<double>(std::numeric_limits<int>::max()) ||
                                    std::floor(value->valuedouble) != value->valuedouble)
                                        return false;
                                destination = static_cast<int>(value->valuedouble);
                                return true;
                        };
                        if (!readOptionalInt("discoveryBufferArg", method.discoveryBufferArg) ||
                            !readOptionalInt("discoveryCapacityArg", method.discoveryCapacityArg)) {
                                error = tr("Discovery argument index is invalid.", "发现参数索引无效。");
                                return nullptr;
                        }
                        if (const cJSON *size = cJSON_GetObjectItem(item, "discoveryBufferSize"); size) {
                                if (!cJSON_IsNumber(size) || std::floor(size->valuedouble) != size->valuedouble ||
                                    size->valuedouble < 1024.0 || size->valuedouble > 4.0 * 1024.0 * 1024.0) {
                                        error = tr("Discovery buffer size is invalid.", "发现缓冲区大小无效。");
                                        return nullptr;
                                }
                                method.discoveryBufferSize = static_cast<uint32_t>(size->valuedouble);
                        }

                        const cJSON *arguments = cJSON_GetObjectItem(item, "arguments");
                        if (!cJSON_IsArray(arguments)) {
                                error = tr("Method arguments must be an array.", "方法参数必须是数组。");
                                return nullptr;
                        }
                        for (const cJSON *arg = arguments->child; arg; arg = arg->next) {
                                if (!cJSON_IsObject(arg)) {
                                        error = tr("Method argument entry is not an object.", "方法参数条目不是对象。");
                                        return nullptr;
                                }
                                const cJSON *source = cJSON_GetObjectItem(arg, "source");
                                if (!cJSON_IsString(source)) {
                                        error = tr("Method argument source is invalid.", "方法参数来源无效。");
                                        return nullptr;
                                }
                                const auto parsedSource = argSourceFromName(source->valuestring);
                                if (!parsedSource) {
                                        error = tr("Method argument source is unknown.", "方法参数来源未知。");
                                        return nullptr;
                                }
                                ArgumentBinding binding;
                                binding.source = static_cast<ArgSource>(*parsedSource);
                                if (const cJSON *literal = cJSON_GetObjectItem(arg, "literal"); literal) {
                                        if (!cJSON_IsString(literal)) {
                                                error = tr("Method literal must be a string.", "方法常量必须是字符串。");
                                                return nullptr;
                                        }
                                        binding.literal = literal->valuestring ? literal->valuestring : "";
                                }
                                if (!readId(arg, "propertyId", binding.propertyId, false)) {
                                        error = tr("Method argument propertyId is invalid.", "方法参数 propertyId 无效。");
                                        return nullptr;
                                }
                                method.arguments.push_back(std::move(binding));
                        }
                        device->methods.push_back(std::move(method));
                }
        }

        // Validate all saved references after the property table is complete.
        for (const auto &method : device->methods) {
                if (method.resultPropertyId != 0 && propertyIds.count(method.resultPropertyId) == 0) {
                        error = tr("Method contains a dangling return property reference.", "方法包含悬空的返回属性引用。");
                        return nullptr;
                }
                for (const auto &argument : method.arguments) {
                        if (argument.source != ArgSource::Literal &&
                            (argument.propertyId == 0 || propertyIds.count(argument.propertyId) == 0)) {
                                error = tr("Method contains a dangling argument property reference.",
                                           "方法包含悬空的参数属性引用。");
                                return nullptr;
                        }
                }
        }

        std::unordered_set<uint64_t>    instanceIds;
        std::unordered_set<std::string> discoveryKeys;
        const cJSON                    *instances = cJSON_GetObjectItem(object, "instances");
        if (instances && !cJSON_IsArray(instances)) {
                error = tr("Device-type instances must be an array.", "设备类型的实例必须是数组。");
                return nullptr;
        }
        if (cJSON_IsArray(instances)) {
                for (const cJSON *item = instances->child; item; item = item->next) {
                        if (!cJSON_IsObject(item)) {
                                error = tr("Device-instance entry is not an object.", "具体设备条目不是对象。");
                                return nullptr;
                        }
                        DeviceInstance instance;
                        if (!readId(item, "id", instance.id) || instance.id == 0 || !instanceIds.insert(instance.id).second) {
                                error = tr("Device instance has an invalid or duplicate id.", "具体设备 ID 无效或重复。");
                                return nullptr;
                        }
                        const cJSON *instanceName = cJSON_GetObjectItem(item, "name");
                        if (!cJSON_IsString(instanceName) || !instanceName->valuestring ||
                            trimCopy(instanceName->valuestring).empty()) {
                                error = tr("Device instance has an invalid name.", "具体设备名称无效。");
                                return nullptr;
                        }
                        instance.name = instanceName->valuestring;
                        if (const cJSON *key = cJSON_GetObjectItem(item, "discoveryKey"); key) {
                                if (!cJSON_IsString(key) || !key->valuestring) {
                                        error = tr("Device discoveryKey must be a string.", "设备 discoveryKey 必须是字符串。");
                                        return nullptr;
                                }
                                instance.discoveryKey = trimCopy(key->valuestring);
                                if (key->valuestring[0] != '\0' && instance.discoveryKey.empty()) {
                                        error = tr("Device discoveryKey cannot contain only whitespace.",
                                                   "设备 discoveryKey 不能仅包含空白字符。");
                                        return nullptr;
                                }
                                if (!instance.discoveryKey.empty() && !discoveryKeys.insert(instance.discoveryKey).second) {
                                        error = tr("Device discoveryKey must be unique within its type.",
                                                   "同一设备类型内的 discoveryKey 必须唯一。");
                                        return nullptr;
                                }
                        }
                        if (const cJSON *discoveredName = cJSON_GetObjectItem(item, "discoveredName"); discoveredName) {
                                if (!cJSON_IsString(discoveredName) || !discoveredName->valuestring) {
                                        error =
                                            tr("Device discoveredName must be a string.", "设备 discoveredName 必须是字符串。");
                                        return nullptr;
                                }
                                instance.discoveredName = discoveredName->valuestring;
                        } else if (!instance.discoveryKey.empty()) {
                                instance.discoveredName = instance.name;
                        }
                        if (const cJSON *customName = cJSON_GetObjectItem(item, "customName"); customName) {
                                if (!cJSON_IsBool(customName)) {
                                        error = tr("Device customName must be a boolean.", "设备 customName 必须是布尔值。");
                                        return nullptr;
                                }
                                instance.customName = cJSON_IsTrue(customName);
                        }
                        instance.properties = device->properties;
                        const cJSON *values = cJSON_GetObjectItem(item, "values");
                        if (!cJSON_IsArray(values)) {
                                error = tr("Device instance values must be an array.", "具体设备 values 必须是数组。");
                                return nullptr;
                        }
                        std::unordered_set<uint64_t> valueIds;
                        for (const cJSON *value = values->child; value; value = value->next) {
                                if (!cJSON_IsObject(value)) {
                                        error = tr("Device instance contains an invalid property value.",
                                                   "具体设备包含无效的属性值。");
                                        return nullptr;
                                }
                                uint64_t     propertyId = 0;
                                const cJSON *text       = cJSON_GetObjectItem(value, "value");
                                if (!readId(value, "propertyId", propertyId) || propertyId == 0 ||
                                    !valueIds.insert(propertyId).second || !cJSON_IsString(text) || !text->valuestring) {
                                        error = tr("Device instance contains an invalid property value.",
                                                   "具体设备包含无效的属性值。");
                                        return nullptr;
                                }
                                Property *property = findProperty(instance, propertyId);
                                if (!property || !setPropertyValue(*property, text->valuestring)) {
                                        error = tr("Device instance property value does not match its definition.",
                                                   "具体设备属性值与其定义不匹配。");
                                        return nullptr;
                                }
                                if (!readPropertyBinding(value, *property))
                                        return nullptr;
                                property->editValue = propertyValueString(*property);
                        }
                        device->instances.push_back(std::move(instance));
                }
        }

        uint64_t nextProperty = 1;
        uint64_t nextMethod   = 1;
        uint64_t nextInstance = 1;
        for (const auto &property : device->properties)
                nextProperty =
                    std::max(nextProperty, property.id == std::numeric_limits<uint64_t>::max() ? property.id : property.id + 1);
        for (const auto &method : device->methods)
                nextMethod =
                    std::max(nextMethod, method.id == std::numeric_limits<uint64_t>::max() ? method.id : method.id + 1);
        for (const auto &instance : device->instances)
                nextInstance =
                    std::max(nextInstance, instance.id == std::numeric_limits<uint64_t>::max() ? instance.id : instance.id + 1);
        uint64_t savedNext = 0;
        if (readId(object, "nextPropertyId", savedNext, false) && savedNext != 0)
                nextProperty = std::max(nextProperty, savedNext);
        savedNext = 0;
        if (readId(object, "nextMethodId", savedNext, false) && savedNext != 0)
                nextMethod = std::max(nextMethod, savedNext);
        savedNext = 0;
        if (readId(object, "nextInstanceId", savedNext, false) && savedNext != 0)
                nextInstance = std::max(nextInstance, savedNext);
        device->nextPropertyId = nextProperty;
        device->nextMethodId   = nextMethod;
        device->nextInstanceId = nextInstance;

        return device;
}

void
DeviceManager::save(void *node, const std::string &baseDir) const
{
        cJSON *object = static_cast<cJSON *>(node);
        if (!object)
                return;
        cJSON_AddNumberToObject(object, "version", kDeviceFormatVersion);
        cJSON_AddBoolToObject(object, "open", open_);
        cJSON_AddNumberToObject(object, "sidebarWidth", sidebarWidth_);
        addId(object, "nextDeviceTypeId", nextDeviceId_);
        uint64_t selectedTypeId     = 0;
        uint64_t selectedInstanceId = 0;
        if (selectedDevice_ >= 0 && selectedDevice_ < static_cast<int>(devices_.size())) {
                selectedTypeId        = devices_[static_cast<size_t>(selectedDevice_)]->id;
                const auto &instances = devices_[static_cast<size_t>(selectedDevice_)]->instances;
                if (selectedInstance_ >= 0 && selectedInstance_ < static_cast<int>(instances.size()))
                        selectedInstanceId = instances[static_cast<size_t>(selectedInstance_)].id;
        }
        addId(object, "selectedDeviceTypeId", selectedTypeId);
        addId(object, "selectedInstanceId", selectedInstanceId);
        cJSON *devices = cJSON_CreateArray();
        for (const auto &device : devices_) {
                cJSON *item = cJSON_CreateObject();
                saveDevice(item, *device, baseDir);
                cJSON_AddItemToArray(devices, item);
        }
        cJSON_AddItemToObject(object, "deviceTypes", devices);
}

bool
DeviceManager::load(const void *node, const std::string &baseDir)
{
        const cJSON *object = static_cast<const cJSON *>(node);
        auto         fail   = [&](std::string message) {
                managerStatus_        = std::move(message);
                managerStatusIsError_ = true;
                open_                 = true;
                modified_             = true;
                return false;
        };
        if (!cJSON_IsObject(object))
                return fail(tr("Device-manager data must be an object.", "设备管理器数据必须是对象。"));
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(object, "version");
        if (!cJSON_IsNumber(version) || !std::isfinite(version->valuedouble) ||
            (version->valuedouble != 1.0 && version->valuedouble != 2.0 &&
             version->valuedouble != static_cast<double>(kDeviceFormatVersion))) {
                return fail(tr("Unsupported device-manager data version.", "不支持的设备管理器数据版本。"));
        }
        const bool legacy    = version->valuedouble == 1.0;
        bool       savedOpen = false;
        if (const cJSON *open = cJSON_GetObjectItemCaseSensitive(object, "open"); open) {
                if (!cJSON_IsBool(open))
                        return fail(tr("Device-manager open state must be boolean.", "设备管理器 open 状态必须是布尔值。"));
                savedOpen = cJSON_IsTrue(open);
        }
        if (const cJSON *width = cJSON_GetObjectItemCaseSensitive(object, "sidebarWidth"); width) {
                if (!cJSON_IsNumber(width) || !std::isfinite(width->valuedouble))
                        return fail(tr("Device-manager sidebar width is invalid.", "设备管理器侧栏宽度无效。"));
                sidebarWidth_ = std::clamp(static_cast<float>(width->valuedouble), 100.0f, 2000.0f);
        }

        const cJSON *devices = cJSON_GetObjectItemCaseSensitive(object, legacy ? "devices" : "deviceTypes");
        if (!cJSON_IsArray(devices))
                return fail(tr("Device-type list must be an array.", "设备类型列表必须是数组。"));

        std::vector<std::unique_ptr<Device>> loadedDevices;
        std::unordered_set<uint64_t>         ids;
        for (const cJSON *item = devices->child; item; item = item->next) {
                std::string error;
                auto        device = loadDevice(item, baseDir, error);
                if (!device)
                        return fail(error.empty() ? tr("Invalid device-type entry.", "设备类型条目无效。") : error);
                if (!ids.insert(device->id).second)
                        return fail(tr("Duplicate device-type id.", "设备类型 ID 重复。"));
                if (legacy && device->instances.empty()) {
                        DeviceInstance instance;
                        instance.id         = device->id;
                        instance.name       = device->name;
                        instance.properties = device->properties;
                        device->instances.push_back(std::move(instance));
                        device->nextInstanceId = device->id == std::numeric_limits<uint64_t>::max() ? 1 : device->id + 1;
                }
                loadedDevices.push_back(std::move(device));
        }

        uint64_t computedNext = 1;
        for (const auto &device : loadedDevices)
                computedNext =
                    std::max(computedNext, device->id == std::numeric_limits<uint64_t>::max() ? device->id : device->id + 1);
        uint64_t    savedNext = 0;
        const char *nextKey   = legacy ? "nextDeviceId" : "nextDeviceTypeId";
        if (!readId(object, nextKey, savedNext, false))
                return fail(tr("Next device-type id is invalid.", "下一个设备类型 ID 无效。"));
        if (savedNext != 0)
                computedNext = std::max(computedNext, savedNext);

        uint64_t selectedTypeId     = 0;
        uint64_t selectedInstanceId = 0;
        if (!legacy) {
                if (!readId(object, "selectedDeviceTypeId", selectedTypeId, false) ||
                    !readId(object, "selectedInstanceId", selectedInstanceId, false))
                        return fail(tr("Saved device selection is invalid.", "保存的设备选择无效。"));
        }

        devices_          = std::move(loadedDevices);
        nextDeviceId_     = computedNext;
        selectedDevice_   = -1;
        selectedInstance_ = -1;
        if (!devices_.empty()) {
                selectedDevice_ = 0;
                if (selectedTypeId != 0) {
                        const auto selected = std::find_if(
                            devices_.begin(), devices_.end(), [&](const auto &device) { return device->id == selectedTypeId; });
                        if (selected != devices_.end())
                                selectedDevice_ = static_cast<int>(std::distance(devices_.begin(), selected));
                }
                auto &instances = devices_[static_cast<size_t>(selectedDevice_)]->instances;
                if (legacy && !instances.empty()) {
                        selectedInstance_ = 0;
                } else if (selectedInstanceId != 0) {
                        const auto selected =
                            std::find_if(instances.begin(), instances.end(), [&](const DeviceInstance &instance) {
                                    return instance.id == selectedInstanceId;
                            });
                        if (selected != instances.end())
                                selectedInstance_ = static_cast<int>(std::distance(instances.begin(), selected));
                }
        }
        selectedFunction_        = -1;
        pendingDeleteTypeId_     = 0;
        pendingDeleteInstanceId_ = 0;
        pendingDeletePropertyId_ = 0;
        pendingDeleteMethodId_   = 0;
        managerStatus_.clear();
        managerStatusIsError_ = false;
        open_                 = savedOpen;
        modified_             = false;
        return true;
}

void
DeviceManager::exportDevice(const Device &device)
{
        std::string path =
            nativeDlgSave(tr("Export Device Type", "导出设备类型"), {{"AvA Device", {"avadev"}}}, device.name + ".avadev");
        if (path.empty())
                return;
        if (!hasExtension(path, {".avadev"}))
                path += ".avadev";

        const std::filesystem::path fsPath(reinterpret_cast<const char8_t *>(path.c_str()));

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "format", "ava-device-type");
        cJSON_AddNumberToObject(root, "version", kDeviceFormatVersion);
        cJSON *item = cJSON_CreateObject();
        saveDevice(item, device, fsPath.parent_path().string());
        cJSON_AddItemToObject(root, "deviceType", item);
        char *output = cJSON_Print(root);
        cJSON_Delete(root);

        bool written = false;
        if (output) {
                std::ofstream file(fsPath, std::ios::binary | std::ios::trunc);
                if (file) {
                        file << output;
                        written = file.good();
                }
                cJSON_free(output);
        }
        managerStatus_        = written ? tr("Device type exported.", "设备类型已导出。")
                                        : tr("Failed to export device type.", "设备类型导出失败。");
        managerStatusIsError_ = !written;
}

void
DeviceManager::importDevice()
{
        const std::string path = nativeDlgOpen(tr("Import Device Type", "导入设备类型"), {{"AvA Device", {"avadev"}}});
        if (path.empty())
                return;
        const std::filesystem::path fsPath(reinterpret_cast<const char8_t *>(path.c_str()));
        std::ifstream               file(fsPath, std::ios::binary);
        if (!file) {
                managerStatus_        = tr("Cannot open device file.", "无法打开设备文件。");
                managerStatusIsError_ = true;
                return;
        }
        std::ostringstream stream;
        stream << file.rdbuf();
        const std::string input = stream.str();
        cJSON            *root  = cJSON_ParseWithLengthOpts(input.c_str(), input.size() + 1, nullptr, true);
        if (!root) {
                managerStatus_        = tr("Invalid .avadev JSON.", ".avadev JSON 无效。");
                managerStatusIsError_ = true;
                return;
        }
        const cJSON *format  = cJSON_GetObjectItemCaseSensitive(root, "format");
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
        const bool   legacy  = cJSON_IsNumber(version) && version->valuedouble == 1.0;
        const bool   current = cJSON_IsNumber(version) &&
                             (version->valuedouble == 2.0 || version->valuedouble == static_cast<double>(kDeviceFormatVersion));
        const bool formatOk = cJSON_IsString(format) && format->valuestring &&
                              ((legacy && std::strcmp(format->valuestring, "ava-device") == 0) ||
                               (current && (std::strcmp(format->valuestring, "ava-device-type") == 0 ||
                                            std::strcmp(format->valuestring, "ava-device") == 0)));
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, legacy ? "device" : "deviceType");
        if (!formatOk || !cJSON_IsObject(item)) {
                cJSON_Delete(root);
                managerStatus_        = tr("Unsupported or malformed .avadev file.", "不支持或格式错误的 .avadev 文件。");
                managerStatusIsError_ = true;
                return;
        }

        std::string error;
        auto        device = loadDevice(item, fsPath.parent_path().string(), error);
        cJSON_Delete(root);
        if (!device) {
                managerStatus_        = error.empty() ? tr("Invalid device definition.", "设备定义无效。") : error;
                managerStatusIsError_ = true;
                return;
        }
        if (legacy && device->instances.empty()) {
                DeviceInstance instance;
                instance.id         = device->id;
                instance.name       = device->name;
                instance.properties = device->properties;
                device->instances.push_back(std::move(instance));
                device->nextInstanceId = device->id == std::numeric_limits<uint64_t>::max() ? 1 : device->id + 1;
        }

        const bool collision =
            std::any_of(devices_.begin(), devices_.end(), [&](const auto &existing) { return existing->id == device->id; });
        if (collision || device->id == 0) {
                device->id = takeStableId(nextDeviceId_, [&](uint64_t id) {
                        return std::any_of(
                            devices_.begin(), devices_.end(), [&](const auto &existing) { return existing->id == id; });
                });
                if (device->id == 0) {
                        managerStatus_        = tr("No device id is available for import.", "没有可用于导入的设备 ID。");
                        managerStatusIsError_ = true;
                        return;
                }
        } else if (device->id < std::numeric_limits<uint64_t>::max()) {
                nextDeviceId_ = std::max(nextDeviceId_, device->id + 1);
        }
        devices_.push_back(std::move(device));
        selectedDevice_   = static_cast<int>(devices_.size()) - 1;
        selectedInstance_ = legacy ? 0 : -1;
        selectedFunction_ = -1;
        modified_         = true;
        managerStatus_ =
            tr("Device type imported. The dynamic library remains unloaded.", "设备类型已导入；动态库保持未加载状态。");
        managerStatusIsError_ = false;
}

void
DeviceManager::pushDroppedFiles(const std::vector<std::string> &paths)
{
        if (!open_)
                return;
        pendingDropFiles_.insert(pendingDropFiles_.end(), paths.begin(), paths.end());
}

void
DeviceManager::drawDeviceList()
{
        ImGui::BeginChild("##device_list", ImVec2(sidebarWidth_, 0.0f), true);
        ImGui::TextUnformatted(tr("Device Types and Devices", "设备类型与设备"));
        ImGui::Separator();
        for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
                const auto &device = devices_[i];
                ImGui::PushID(("device_type_" + std::to_string(device->id)).c_str());
                std::string label         = device->name.empty() ? std::string(tr("Unnamed type", "未命名类型")) : device->name;
                label                    += " (" + std::to_string(device->instances.size()) + ")";
                label                    += "###type_node";
                ImGuiTreeNodeFlags flags  = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                           ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
                if (selectedDevice_ == i && selectedInstance_ < 0)
                        flags |= ImGuiTreeNodeFlags_Selected;
                const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
                        if (selectedDevice_ != i || selectedInstance_ != -1)
                                for (auto &method : device->methods)
                                        method.lastResult.clear();
                        selectedDevice_   = i;
                        selectedInstance_ = -1;
                        selectedFunction_ = -1;
                }
                if (open) {
                        for (int j = 0; j < static_cast<int>(device->instances.size()); ++j) {
                                const DeviceInstance &instance = device->instances[j];
                                ImGui::PushID(("instance_" + std::to_string(instance.id)).c_str());
                                std::string instanceLabel =
                                    instance.name.empty() ? std::string(tr("Unnamed device", "未命名设备")) : instance.name;
                                if (!instance.discoveryKey.empty() && !instance.online)
                                        instanceLabel += tr(" (offline)", "（离线）");
                                instanceLabel                    += "###instance_node";
                                ImGuiTreeNodeFlags instanceFlags  = ImGuiTreeNodeFlags_Leaf |
                                                                   ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                                   ImGuiTreeNodeFlags_SpanAvailWidth;
                                if (selectedDevice_ == i && selectedInstance_ == j)
                                        instanceFlags |= ImGuiTreeNodeFlags_Selected;
                                ImGui::TreeNodeEx(instanceLabel.c_str(), instanceFlags);
                                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                                        if (selectedDevice_ != i || selectedInstance_ != j)
                                                for (auto &method : device->methods)
                                                        method.lastResult.clear();
                                        selectedDevice_   = i;
                                        selectedInstance_ = j;
                                        selectedFunction_ = -1;
                                }
                                ImGui::PopID();
                        }
                        ImGui::TreePop();
                }
                ImGui::PopID();
        }

        ImGui::Separator();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint(
            "##new_device_name", tr("New device-type name", "新设备类型名称"), newDeviceName_, sizeof(newDeviceName_));
        if (ImGui::Button(tr("New Device Type", "新建设备类型"), ImVec2(-1.0f, 0.0f)))
                createDevice(newDeviceName_);
        const bool hasType = selectedDevice_ >= 0 && selectedDevice_ < static_cast<int>(devices_.size());
        ImGui::BeginDisabled(!hasType);
        if (ImGui::Button(tr("Add Device to Type", "向类型添加设备"), ImVec2(-1.0f, 0.0f)) && hasType)
                createInstance(*devices_[selectedDevice_]);
        ImGui::EndDisabled();
        if (ImGui::Button(tr("Import .avadev", "导入 .avadev"), ImVec2(-1.0f, 0.0f)))
                importDevice();
        ImGui::EndChild();
}

bool
DeviceManager::addBoundProperty(Device         &device,
                                DeviceInstance *instance,
                                const char     *name,
                                uint64_t        address,
                                const char     *type,
                                const char     *port,
                                bool            writable,
                                uint32_t        bitOffset,
                                uint32_t        bitSize)
{
        if (!port || std::strcmp(port, "JLINK") != 0) {
                managerStatus_ =
                    tr("Device properties currently accept J-Link variables only.", "设备属性当前仅接受 J-Link 变量。");
                managerStatusIsError_ = true;
                return false;
        }
        if (address == 0u || address > std::numeric_limits<uint32_t>::max()) {
                managerStatus_ = tr("The dropped J-Link variable has an invalid address.", "拖入的 J-Link 变量地址无效。");
                managerStatusIsError_ = true;
                return false;
        }
        const auto parsedType = stableTypeFromName(type ? type : "");
        if (!parsedType || !isScalarType(*parsedType)) {
                managerStatus_ =
                    tr("The dropped variable type is not a supported scalar type.", "拖入变量不是受支持的标量类型。");
                managerStatusIsError_ = true;
                return false;
        }
        const uint32_t size = scalarTypeSize(*parsedType);
        if (bitSize != 0u && (bitOffset >= size * 8u || bitSize > size * 8u - bitOffset)) {
                managerStatus_        = tr("The dropped variable has an invalid bit-field range.", "拖入变量的位域范围无效。");
                managerStatusIsError_ = true;
                return false;
        }

        const std::string symbol = name && name[0] ? name : "J-Link variable";
        auto     definition = std::find_if(device.properties.begin(), device.properties.end(), [&](const Property &property) {
                return property.name == symbol;
        });
        uint64_t propertyId = 0;
        if (definition == device.properties.end()) {
                Property property;
                property.id = takeStableId(device.nextPropertyId, [&](uint64_t id) {
                        return std::any_of(device.properties.begin(), device.properties.end(), [&](const Property &existing) {
                                return existing.id == id;
                        });
                });
                if (property.id == 0u)
                        return false;
                property.name         = symbol;
                property.discoveryKey = "property_" + std::to_string(property.id);
                property.type         = *parsedType;
                property.editValue    = "0";
                if (!instance) {
                        property.source    = PropertySource::JLink;
                        property.symbol    = symbol;
                        property.address   = static_cast<uint32_t>(address);
                        property.bitOffset = bitOffset;
                        property.bitSize   = bitSize;
                        property.writable  = writable;
                }
                propertyId = property.id;
                device.properties.push_back(property);
                for (DeviceInstance &item : device.instances)
                        item.properties.push_back(property);
                definition = std::prev(device.properties.end());
        } else {
                propertyId = definition->id;
                if (definition->type != *parsedType) {
                        managerStatus_        = tr("A property with the same name already exists with another type.",
                                            "同名属性已存在，但数据类型不同。");
                        managerStatusIsError_ = true;
                        return false;
                }
        }

        auto bind = [&](Property &property) {
                property.source         = PropertySource::JLink;
                property.symbol         = symbol;
                property.address        = static_cast<uint32_t>(address);
                property.bitOffset      = bitOffset;
                property.bitSize        = bitSize;
                property.writable       = writable;
                property.symbolResolved = true;
                property.liveValueKnown = false;
                property.liveReadOk     = false;
                property.writePending   = false;
        };
        if (instance) {
                if (Property *target = findProperty(*instance, propertyId))
                        bind(*target);
        } else {
                bind(*definition);
                for (DeviceInstance &item : device.instances)
                        if (Property *target = findProperty(item, propertyId))
                                bind(*target);
        }
        modified_             = true;
        managerStatus_        = tr("J-Link variable bound to the device property.", "J-Link 变量已绑定到设备属性。");
        managerStatusIsError_ = false;
        return true;
}

void
DeviceManager::acceptPropertyDrop(Device &device, DeviceInstance *instance)
{
        if (!ImGui::BeginDragDropTarget())
                return;
        const ImGuiPayload *scalar = ImGui::AcceptDragDropPayload("SYMBOL_CHANNEL");
        if (!scalar)
                scalar = ImGui::AcceptDragDropPayload("CHANNEL");
        if (scalar && scalar->DataSize == sizeof(ChannelDropPayload)) {
                const auto &source = *static_cast<const ChannelDropPayload *>(scalar->Data);
                addBoundProperty(device,
                                 instance,
                                 source.name,
                                 source.addr,
                                 source.type,
                                 source.device,
                                 source.writable,
                                 source.bitOffset,
                                 source.bitSize);
        }
        if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("STRUCT_CHANNEL")) {
                if (payload->DataSize == sizeof(StructChannelPayload)) {
                        const auto &source = *static_cast<const StructChannelPayload *>(payload->Data);
                        for (int i = 0; i < source.count; ++i) {
                                const auto &entry = source.entries[i];
                                addBoundProperty(device,
                                                 instance,
                                                 entry.name,
                                                 entry.addr,
                                                 entry.type,
                                                 source.device,
                                                 entry.writable,
                                                 entry.bitOffset,
                                                 entry.bitSize);
                        }
                }
        }
        if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("DND_CHANNEL_MOVE")) {
                if (payload->DataSize == sizeof(ChannelMovePayload)) {
                        const auto &source = *static_cast<const ChannelMovePayload *>(payload->Data);
                        if (source.srcScope && !source.isGroup) {
                                const auto found = source.srcScope->getChannels().find(source.chName);
                                if (found != source.srcScope->getChannels().end() && found->second) {
                                        auto &channel = *found->second;
                                        addBoundProperty(device,
                                                         instance,
                                                         source.chName,
                                                         channel.getAddr(),
                                                         channel.getType().c_str(),
                                                         channel.getDevice().c_str(),
                                                         channel.isWritable(),
                                                         channel.getBitOffset(),
                                                         channel.getBitSize());
                                }
                        } else {
                                managerStatus_        = tr("Drop a scalar monitor channel, not a group.",
                                                    "请拖入监视器中的标量通道，而不是变量组。");
                                managerStatusIsError_ = true;
                        }
                }
        }
        ImGui::EndDragDropTarget();
}

void
DeviceManager::drawProperties(Device &device)
{
        if (ImGui::Button(tr("Add Property", "添加属性"))) {
                Property property;
                property.id = takeStableId(device.nextPropertyId, [&](uint64_t id) {
                        return std::any_of(device.properties.begin(), device.properties.end(), [&](const Property &existing) {
                                return existing.id == id;
                        });
                });
                if (property.id == 0) {
                        device.status        = tr("No property id is available.", "没有可用的属性 ID。");
                        device.statusIsError = true;
                        return;
                }
                property.name         = std::string(tr("Property", "属性")) + " " + std::to_string(property.id);
                property.discoveryKey = "property_" + std::to_string(property.id);
                property.type         = CType::F64;
                property.editValue    = "0";
                device.properties.push_back(std::move(property));
                for (auto &instance : device.instances)
                        instance.properties.push_back(device.properties.back());
                modified_ = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(
            "%s", tr("Drop J-Link variables here to create binding templates.", "将 J-Link 变量拖到这里可创建绑定模板。"));

        if (!ImGui::BeginTable("##properties",
                               8,
                               ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                   ImGuiTableFlags_SizingStretchProp))
                return;
        ImGui::TableSetupColumn(tr("ID", "ID"), ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn(tr("Name", "名称"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(tr("Discovery Key", "发现字段键"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(tr("Type", "类型"), ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn(tr("Source", "来源"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(tr("Default", "默认值"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn(tr("State", "状态"), ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableHeadersRow();

        int  eraseIndex         = -1;
        bool requestDeletePopup = false;
        for (int i = 0; i < static_cast<int>(device.properties.size()); ++i) {
                Property         &property = device.properties[i];
                const std::string id       = std::to_string(property.id);
                ImGui::PushID(id.c_str());
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(id.c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-1.0f);
                if (inputTextString("##property_name", property.name)) {
                        for (auto &instance : device.instances)
                                if (Property *value = findProperty(instance, property.id))
                                        value->name = property.name;
                        modified_ = true;
                }
                if (ImGui::IsItemDeactivatedAfterEdit() && trimCopy(property.name).empty()) {
                        property.name = std::string(tr("Property", "属性")) + " " + id;
                        for (auto &instance : device.instances)
                                if (Property *value = findProperty(instance, property.id))
                                        value->name = property.name;
                        modified_ = true;
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::SetNextItemWidth(-1.0f);
                const std::string previousDiscoveryKey = property.discoveryKey;
                if (inputTextString("##property_discovery_key", property.discoveryKey)) {
                        const std::string candidateKey = trimCopy(property.discoveryKey);
                        const bool        duplicate =
                            !candidateKey.empty() &&
                            std::any_of(device.properties.begin(), device.properties.end(), [&](const Property &other) {
                                    return other.id != property.id && other.discoveryKey == candidateKey;
                            });
                        if (duplicate) {
                                property.discoveryKey = previousDiscoveryKey;
                                device.status         = tr("Discovery keys must be unique within a device type.",
                                                   "同一设备类型内的发现字段键必须唯一。");
                                device.statusIsError  = true;
                        } else {
                                for (auto &instance : device.instances)
                                        if (Property *value = findProperty(instance, property.id))
                                                value->discoveryKey = property.discoveryKey;
                                modified_ = true;
                        }
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                        property.discoveryKey = trimCopy(property.discoveryKey);
                        if (property.discoveryKey.empty())
                                property.discoveryKey = "property_" + id;
                        for (auto &instance : device.instances)
                                if (Property *value = findProperty(instance, property.id))
                                        value->discoveryKey = property.discoveryKey;
                }

                ImGui::TableSetColumnIndex(3);
                const char *preview = stableTypeName(property.type);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##property_type", preview)) {
                        for (const auto &choice : kPropertyTypes) {
                                const bool selected = property.type == choice.type;
                                if (ImGui::Selectable(choice.name, selected) && !selected) {
                                        property.type   = choice.type;
                                        property.source = PropertySource::Manual;
                                        property.symbol.clear();
                                        property.address   = 0u;
                                        property.bitOffset = 0u;
                                        property.bitSize   = 0u;
                                        property.data.fill(0);
                                        property.editValue        = propertyValueString(property);
                                        property.editValueInvalid = false;
                                        for (auto &instance : device.instances) {
                                                if (Property *value = findProperty(instance, property.id)) {
                                                        value->type   = property.type;
                                                        value->source = PropertySource::Manual;
                                                        value->symbol.clear();
                                                        value->address   = 0u;
                                                        value->bitOffset = 0u;
                                                        value->bitSize   = 0u;
                                                        value->data.fill(0);
                                                        value->editValue        = propertyValueString(*value);
                                                        value->editValueInvalid = false;
                                                }
                                        }
                                        for (auto &method : device.methods) {
                                                if (method.resultPropertyId == property.id)
                                                        method.resultPropertyId = 0;
                                                for (auto &argument : method.arguments) {
                                                        if (argument.propertyId == property.id) {
                                                                argument.source     = ArgSource::Literal;
                                                                argument.literal    = "0";
                                                                argument.propertyId = 0;
                                                        }
                                                }
                                        }
                                        modified_ = true;
                                }
                                if (selected)
                                        ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                }

                ImGui::TableSetColumnIndex(4);
                if (property.source == PropertySource::JLink) {
                        ImGui::Text("J-Link 0x%08X", property.address);
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", property.symbol.c_str());
                } else {
                        ImGui::TextDisabled("%s", tr("Manual", "手动"));
                }

                ImGui::TableSetColumnIndex(5);
                if (property.editValue.empty())
                        property.editValue = propertyValueString(property);
                const bool valueWasInvalid = property.editValueInvalid;
                if (valueWasInvalid)
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                ImGui::SetNextItemWidth(-1.0f);
                if (inputTextString("##property_value", property.editValue)) {
                        Property parsed = property;
                        if (setPropertyValue(parsed, property.editValue)) {
                                property.data             = parsed.data;
                                property.editValueInvalid = false;
                                modified_                 = true;
                        } else {
                                property.editValueInvalid = true;
                        }
                }
                if (valueWasInvalid)
                        ImGui::PopStyleColor();

                ImGui::TableSetColumnIndex(6);
                if (property.editValueInvalid)
                        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", tr("Invalid", "无效"));
                else
                        ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.45f, 1.0f),
                                           "%s",
                                           property.source == PropertySource::JLink ? tr("Template", "模板")
                                                                                    : tr("Valid", "有效"));

                ImGui::TableSetColumnIndex(7);
                if (ImGui::SmallButton("X")) {
                        pendingDeletePropertyId_ = property.id;
                        requestDeletePopup       = true;
                }
                ImGui::PopID();
        }
        ImGui::EndTable();
        acceptPropertyDrop(device, nullptr);

        if (requestDeletePopup)
                ImGui::OpenPopup(tr("Delete Property?###ConfirmDeleteProperty", "删除属性？###ConfirmDeleteProperty"));
        if (ImGui::BeginPopupModal(tr("Delete Property?###ConfirmDeleteProperty", "删除属性？###ConfirmDeleteProperty"),
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextWrapped("%s",
                                   tr("Delete this property from the type and every concrete device? "
                                      "All method bindings that reference it will be cleared.",
                                      "确定从该类型及每个具体设备中删除此属性吗？"
                                      "所有引用它的方法绑定都会被清除。"));
                if (ImGui::Button(tr("Delete", "删除"))) {
                        const auto found =
                            std::find_if(device.properties.begin(), device.properties.end(), [&](const Property &property) {
                                    return property.id == pendingDeletePropertyId_;
                            });
                        if (found != device.properties.end())
                                eraseIndex = static_cast<int>(std::distance(device.properties.begin(), found));
                        pendingDeletePropertyId_ = 0;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Cancel", "取消"))) {
                        pendingDeletePropertyId_ = 0;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }

        if (eraseIndex >= 0) {
                const uint64_t removedId = device.properties[eraseIndex].id;
                device.properties.erase(device.properties.begin() + eraseIndex);
                for (auto &instance : device.instances) {
                        instance.properties.erase(
                            std::remove_if(instance.properties.begin(),
                                           instance.properties.end(),
                                           [&](const Property &property) { return property.id == removedId; }),
                            instance.properties.end());
                }
                for (auto &method : device.methods) {
                        if (method.resultPropertyId == removedId)
                                method.resultPropertyId = 0;
                        for (auto &argument : method.arguments) {
                                if (argument.propertyId == removedId) {
                                        argument.source     = ArgSource::Literal;
                                        argument.literal    = "0";
                                        argument.propertyId = 0;
                                }
                        }
                }
                modified_ = true;
        }
}

void
DeviceManager::drawMethods(Device &device, DeviceInstance *instance)
{
        const auto &functions = device.declarations.functions;
        if (!instance) {
                const char *preview = tr("Select a top-level C function", "选择顶层 C 函数");
                std::string selectedLabel;
                if (selectedFunction_ >= 0 && selectedFunction_ < static_cast<int>(functions.size())) {
                        selectedLabel = functionLabel(functions[selectedFunction_]);
                        preview       = selectedLabel.c_str();
                }
                ImGui::SetNextItemWidth(std::max(260.0f, ImGui::GetContentRegionAvail().x - 150.0f));
                if (ImGui::BeginCombo("##header_function", preview)) {
                        for (int i = 0; i < static_cast<int>(functions.size()); ++i) {
                                const std::string label    = functionLabel(functions[i]);
                                const bool        selected = i == selectedFunction_;
                                if (ImGui::Selectable(label.c_str(), selected))
                                        selectedFunction_ = i;
                                if (selected)
                                        ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                }
                ImGui::SameLine();
                const bool canBind = selectedFunction_ >= 0 && selectedFunction_ < static_cast<int>(functions.size());
                const bool selectedHasUnknown =
                    canBind && (functions[selectedFunction_].retType == CType::Unknown ||
                                std::any_of(functions[selectedFunction_].params.begin(),
                                            functions[selectedFunction_].params.end(),
                                            [](const CParam &parameter) { return parameter.type == CType::Unknown; }));
                const bool selectedIsVariadic = canBind && functions[selectedFunction_].isVariadic;
                const bool bindingSupported   = canBind && !selectedHasUnknown && !selectedIsVariadic;
                ImGui::BeginDisabled(!bindingSupported);
                if (ImGui::Button(tr("Bind Method", "绑定方法")) && bindingSupported)
                        bindFunction(device, functions[selectedFunction_]);
                ImGui::EndDisabled();
                if (selectedHasUnknown)
                        ImGui::TextColored(
                            ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
                            "%s",
                            tr("Cannot bind: unknown parameter or return type.", "无法绑定：存在未知参数或返回类型。"));
                else if (selectedIsVariadic)
                        ImGui::TextColored(
                            ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
                            "%s",
                            tr("Cannot bind: variadic functions are unsupported.", "无法绑定：不支持可变参数函数。"));

                if (functions.empty())
                        ImGui::TextDisabled("%s",
                                            tr("Load a header containing top-level C declarations first.",
                                               "请先加载含顶层 C 函数声明的头文件。"));
        }

        int  eraseIndex         = -1;
        bool requestDeletePopup = false;
        for (int methodIndex = 0; methodIndex < static_cast<int>(device.methods.size()); ++methodIndex) {
                MethodBinding &method = device.methods[methodIndex];
                if (instance && method.kind != MethodKind::Instance)
                        continue;
                ImGui::PushID(("method_" + std::to_string(method.id)).c_str());
                bool              keep = true;
                const std::string header =
                    (method.name.empty() ? method.functionName : method.name) + "###bound_method_" + std::to_string(method.id);
                const bool expanded = instance ? ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)
                                               : ImGui::CollapsingHeader(header.c_str(), &keep, ImGuiTreeNodeFlags_DefaultOpen);
                if (!keep) {
                        pendingDeleteMethodId_ = method.id;
                        requestDeletePopup     = true;
                }
                if (expanded && keep) {
                        const CFuncDecl *function = findFunction(device, method);
                        if (!function) {
                                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
                                                   "%s",
                                                   tr("Unresolved: the function name/signature no longer matches the header.",
                                                      "无法解析：函数名称/签名已与头文件不匹配。"));
                        } else if (function->isVariadic) {
                                ImGui::TextColored(
                                    ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
                                    "%s",
                                    tr("Variadic functions are intentionally blocked.", "可变参数函数已被主动禁止。"));
                        }

                        ImGui::TextDisabled("ID: %llu", static_cast<unsigned long long>(method.id));
                        ImGui::Text("%s: %s", tr("Header function", "头文件函数"), method.functionName.c_str());
                        ImGui::Text("%s: %s", tr("Signature", "签名"), method.signature.c_str());

                        if (!instance) {
                                const char *kindLabels[] = {
                                    tr("Instance Method", "实例方法"),
                                    tr("Discovery (JSON)", "发现方法（JSON）"),
                                };
                                int kind = method.kind == MethodKind::Discovery ? 1 : 0;
                                ImGui::SetNextItemWidth(-1.0f);
                                if (ImGui::Combo(tr("Method Purpose", "方法用途"), &kind, kindLabels, 2)) {
                                        method.kind = kind == 1 ? MethodKind::Discovery : MethodKind::Instance;
                                        if (method.kind == MethodKind::Discovery)
                                                method.resultPropertyId = 0;
                                        modified_ = true;
                                }

                                if (method.kind == MethodKind::Discovery && function) {
                                        if (function->retType != CType::I32)
                                                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
                                                                   "%s",
                                                                   tr("Discovery functions must return int32_t.",
                                                                      "发现函数必须返回 int32_t。"));
                                        auto argumentCombo = [&](const char *label, int &selected, bool buffer) {
                                                std::string previewText = tr("Not configured", "未配置");
                                                if (selected >= 0 && selected < static_cast<int>(function->params.size())) {
                                                        const auto &parameter = function->params[static_cast<size_t>(selected)];
                                                        previewText = (parameter.name.empty() ? "arg" + std::to_string(selected)
                                                                                              : parameter.name) +
                                                                      " (" + std::to_string(selected) + ")";
                                                }
                                                ImGui::SetNextItemWidth(-1.0f);
                                                if (ImGui::BeginCombo(label, previewText.c_str())) {
                                                        for (int index = 0; index < static_cast<int>(function->params.size());
                                                             ++index) {
                                                                const CParam &parameter =
                                                                    function->params[static_cast<size_t>(index)];
                                                                bool       parameterConst = false;
                                                                const auto pointee =
                                                                    pointerPointeeType(parameter, parameterConst);
                                                                const bool compatible =
                                                                    buffer ? pointee.has_value() && !parameterConst &&
                                                                                 isCharacterPointer(parameter)
                                                                           : isCapacityType(parameter.type);
                                                                if (!compatible)
                                                                        continue;
                                                                const std::string option =
                                                                    (parameter.name.empty() ? "arg" + std::to_string(index)
                                                                                            : parameter.name) +
                                                                    " (" + std::to_string(index) + ")";
                                                                if (ImGui::Selectable(option.c_str(), selected == index)) {
                                                                        selected = index;
                                                                        if (static_cast<size_t>(index) <
                                                                            method.arguments.size())
                                                                                method.arguments[static_cast<size_t>(index)] =
                                                                                    ArgumentBinding{};
                                                                        modified_ = true;
                                                                }
                                                        }
                                                        ImGui::EndCombo();
                                                }
                                        };
                                        argumentCombo(
                                            tr("JSON Buffer Argument", "JSON 缓冲区参数"), method.discoveryBufferArg, true);
                                        argumentCombo(tr("Capacity Argument", "容量参数"), method.discoveryCapacityArg, false);
                                        uint32_t bufferSize = method.discoveryBufferSize;
                                        ImGui::SetNextItemWidth(-1.0f);
                                        if (ImGui::InputScalar(tr("Buffer Size (bytes)", "缓冲区大小（字节）"),
                                                               ImGuiDataType_U32,
                                                               &bufferSize)) {
                                                method.discoveryBufferSize = std::clamp(bufferSize, 1024u, 4u * 1024u * 1024u);
                                                modified_                  = true;
                                        }
                                        ImGui::TextWrapped(
                                            "%s",
                                            tr("The function writes UTF-8 JSON into the selected char* buffer; "
                                               "the manager supplies the selected integer capacity automatically.",
                                               "函数向所选 char* 缓冲区写入 UTF-8 JSON；管理器自动传入所选整数容量。"));
                                }
                        }

                        ImGui::BeginDisabled(instance != nullptr);

                        ImGui::SetNextItemWidth(-1.0f);
                        if (inputTextString(tr("Display Name", "显示名称"), method.name))
                                modified_ = true;
                        if (ImGui::IsItemDeactivatedAfterEdit() && trimCopy(method.name).empty()) {
                                method.name = method.functionName;
                                modified_   = true;
                        }
                        ImGui::SetNextItemWidth(-1.0f);
                        if (inputTextString(tr("Export Symbol", "导出符号"), method.exportSymbol))
                                modified_ = true;
                        if (ImGui::IsItemDeactivatedAfterEdit() && trimCopy(method.exportSymbol).empty()) {
                                method.exportSymbol = method.functionName;
                                modified_           = true;
                        }

                        if (function) {
                                ImGui::SeparatorText(tr("Arguments", "参数"));
                                for (size_t argumentIndex = 0; argumentIndex < function->params.size(); ++argumentIndex) {
                                        const CParam &parameter = function->params[argumentIndex];
                                        if (argumentIndex >= method.arguments.size())
                                                break;
                                        ArgumentBinding &binding = method.arguments[argumentIndex];
                                        ImGui::PushID(static_cast<int>(argumentIndex));
                                        const std::string parameterName =
                                            parameter.name.empty() ? "arg" + std::to_string(argumentIndex) : parameter.name;
                                        ImGui::Text("%s  (%s)",
                                                    parameterName.c_str(),
                                                    normalizeTypeText(parameter.rawType, parameter.type).c_str());

                                        if (method.kind == MethodKind::Discovery &&
                                            static_cast<int>(argumentIndex) == method.discoveryBufferArg) {
                                                ImGui::TextDisabled("%s",
                                                                    tr("Automatic JSON output buffer", "自动 JSON 输出缓冲区"));
                                                ImGui::PopID();
                                                continue;
                                        }
                                        if (method.kind == MethodKind::Discovery &&
                                            static_cast<int>(argumentIndex) == method.discoveryCapacityArg) {
                                                ImGui::TextDisabled("%s", tr("Automatic buffer capacity", "自动缓冲区容量"));
                                                ImGui::PopID();
                                                continue;
                                        }

                                        const char *sourceLabels[] = {
                                            tr("Literal", "常量"),
                                            tr("Property Value", "属性值"),
                                            tr("Property Address", "属性地址"),
                                        };
                                        int sourceIndex = static_cast<int>(binding.source);
                                        ImGui::SetNextItemWidth(160.0f);
                                        if (ImGui::Combo("##source", &sourceIndex, sourceLabels, 3)) {
                                                binding.source     = static_cast<ArgSource>(sourceIndex);
                                                binding.propertyId = 0;
                                                modified_          = true;
                                        }
                                        ImGui::SameLine();
                                        if (binding.source == ArgSource::Literal) {
                                                ImGui::SetNextItemWidth(-1.0f);
                                                if (inputTextString("##literal", binding.literal))
                                                        modified_ = true;
                                        } else {
                                                bool            pointeeConst     = false;
                                                const auto      pointee          = pointerPointeeType(parameter, pointeeConst);
                                                const CType     requiredType     = binding.source == ArgSource::PropertyValue
                                                                                       ? parameter.type
                                                                                       : (pointee ? *pointee : CType::Unknown);
                                                const bool      sourceSupported  = binding.source == ArgSource::PropertyValue
                                                                                       ? isScalarType(parameter.type)
                                                                                       : pointee.has_value() && !pointeeConst &&
                                                                                       !isCharacterPointer(parameter);
                                                const Property *selectedProperty = findProperty(device, binding.propertyId);
                                                const char     *propertyPreview =
                                                    selectedProperty ? selectedProperty->name.c_str()
                                                                         : tr("Select compatible property", "选择兼容属性");
                                                ImGui::SetNextItemWidth(-1.0f);
                                                ImGui::BeginDisabled(!sourceSupported);
                                                if (ImGui::BeginCombo("##property", propertyPreview)) {
                                                        for (const auto &property : device.properties) {
                                                                if (property.type != requiredType)
                                                                        continue;
                                                                const bool        selected = binding.propertyId == property.id;
                                                                const std::string label =
                                                                    property.name + " [" + std::to_string(property.id) + "]";
                                                                if (ImGui::Selectable(label.c_str(), selected)) {
                                                                        binding.propertyId = property.id;
                                                                        modified_          = true;
                                                                }
                                                                if (selected)
                                                                        ImGui::SetItemDefaultFocus();
                                                        }
                                                        ImGui::EndCombo();
                                                }
                                                ImGui::EndDisabled();
                                                if (!sourceSupported) {
                                                        ImGui::TextColored(
                                                            ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
                                                            "%s",
                                                            binding.source == ArgSource::PropertyAddress
                                                                ? tr("Requires a writable recognized scalar pointer.",
                                                                     "需要可写且可识别的标量指针。")
                                                                : tr("Requires a recognized scalar parameter.",
                                                                     "需要可识别的标量参数。"));
                                                }
                                        }
                                        ImGui::PopID();
                                }

                                if (method.kind != MethodKind::Discovery) {
                                        ImGui::SeparatorText(tr("Return Value", "返回值"));
                                        const Property *resultProperty = findProperty(device, method.resultPropertyId);
                                        const char     *resultPreview =
                                            resultProperty ? resultProperty->name.c_str() : tr("Do not write back", "不回写");
                                        ImGui::SetNextItemWidth(-1.0f);
                                        if (ImGui::BeginCombo("##result_property", resultPreview)) {
                                                if (ImGui::Selectable(tr("Do not write back", "不回写"),
                                                                      method.resultPropertyId == 0)) {
                                                        method.resultPropertyId = 0;
                                                        modified_               = true;
                                                }
                                                if (isScalarType(function->retType)) {
                                                        for (const auto &property : device.properties) {
                                                                if (property.type != function->retType)
                                                                        continue;
                                                                const bool selected = method.resultPropertyId == property.id;
                                                                const std::string label =
                                                                    property.name + " [" + std::to_string(property.id) + "]";
                                                                if (ImGui::Selectable(label.c_str(), selected)) {
                                                                        method.resultPropertyId = property.id;
                                                                        modified_               = true;
                                                                }
                                                        }
                                                }
                                                ImGui::EndCombo();
                                        }
                                }
                        }
                        ImGui::EndDisabled();

                        const bool callable =
                            function && device.loader.isLoaded() && function->retType != CType::Unknown &&
                            (method.kind != MethodKind::Discovery || function->retType == CType::I32) &&
                            std::none_of(function->params.begin(),
                                         function->params.end(),
                                         [](const CParam &parameter) { return parameter.type == CType::Unknown; }) &&
                            !function->isVariadic;
                        ImGui::BeginDisabled(!callable);
                        const bool correctContext =
                            method.kind == MethodKind::Discovery ? instance == nullptr : instance != nullptr;
                        ImGui::BeginDisabled(!correctContext);
                        if (ImGui::Button(method.kind == MethodKind::Discovery ? tr("Discover Devices", "发现设备")
                                                                               : tr("Call", "调用")) &&
                            callable && correctContext)
                                callMethod(device, instance, method);
                        ImGui::EndDisabled();
                        ImGui::EndDisabled();
                        if (!method.lastResult.empty()) {
                                ImGui::SameLine();
                                ImGui::TextColored(method.lastResultOk ? ImVec4(0.35f, 0.9f, 0.45f, 1.0f)
                                                                       : ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
                                                   "%s",
                                                   method.lastResult.c_str());
                        }
                }
                ImGui::PopID();
        }
        if (!instance && requestDeletePopup)
                ImGui::OpenPopup(tr("Delete Method?###ConfirmDeleteMethod", "删除方法？###ConfirmDeleteMethod"));
        if (!instance && ImGui::BeginPopupModal(tr("Delete Method?###ConfirmDeleteMethod", "删除方法？###ConfirmDeleteMethod"),
                                                nullptr,
                                                ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextWrapped("%s", tr("Delete this method binding?", "确定删除这个方法绑定吗？"));
                if (ImGui::Button(tr("Delete", "删除"))) {
                        const auto found =
                            std::find_if(device.methods.begin(), device.methods.end(), [&](const MethodBinding &method) {
                                    return method.id == pendingDeleteMethodId_;
                            });
                        if (found != device.methods.end())
                                eraseIndex = static_cast<int>(std::distance(device.methods.begin(), found));
                        pendingDeleteMethodId_ = 0;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Cancel", "取消"))) {
                        pendingDeleteMethodId_ = 0;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }
        if (!instance && eraseIndex >= 0) {
                device.methods.erase(device.methods.begin() + eraseIndex);
                modified_ = true;
        }
}

void
DeviceManager::drawInstanceProperties(Device &device, DeviceInstance &instance)
{
        if (!ImGui::BeginTable("##instance_properties",
                               6,
                               ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                   ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate))
                return;
        // Keep the concrete-device view aligned with Monitor's TABLE mode. The final
        // column contains binding operations rather than a wave generator.
        ImGui::TableSetupColumn(tr("Name###col_name", "名称###col_name"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Value###col_value", "数值###col_value"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Type###col_type", "类型###col_type"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Address###col_addr", "地址###col_addr"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Port###col_port", "端口###col_port"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Binding###col_binding", "绑定###col_binding"),
                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                                100.0f);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs *sort = ImGui::TableGetSortSpecs(); sort && sort->SpecsDirty) {
                if (sort->SpecsCount > 0) {
                        const ImGuiTableColumnSortSpecs spec         = sort->Specs[0];
                        auto                            numericValue = [](const Property &p) -> long double {
                                switch (p.type) {
                                        case CType::Bool:
                                                return readScalar<uint8_t>(p.data) ? 1.0L : 0.0L;
                                        case CType::I8:
                                                return readScalar<int8_t>(p.data);
                                        case CType::I16:
                                                return readScalar<int16_t>(p.data);
                                        case CType::I32:
                                                return readScalar<int32_t>(p.data);
                                        case CType::I64:
                                                return static_cast<long double>(readScalar<int64_t>(p.data));
                                        case CType::U8:
                                                return readScalar<uint8_t>(p.data);
                                        case CType::U16:
                                                return readScalar<uint16_t>(p.data);
                                        case CType::U32:
                                                return readScalar<uint32_t>(p.data);
                                        case CType::U64:
                                                return static_cast<long double>(readScalar<uint64_t>(p.data));
                                        case CType::F32:
                                                return readScalar<float>(p.data);
                                        case CType::F64:
                                                return readScalar<double>(p.data);
                                        default:
                                                return 0.0L;
                                }
                        };
                        std::sort(
                            instance.properties.begin(), instance.properties.end(), [&](const Property &a, const Property &b) {
                                    int cmp = 0;
                                    switch (spec.ColumnIndex) {
                                            case 0:
                                                    cmp = a.name.compare(b.name);
                                                    break;
                                            case 1: {
                                                    const long double av = numericValue(a), bv = numericValue(b);
                                                    cmp = av < bv ? -1 : (av > bv ? 1 : 0);
                                                    break;
                                            }
                                            case 2:
                                                    cmp = std::strcmp(stableTypeName(a.type), stableTypeName(b.type));
                                                    break;
                                            case 3:
                                                    cmp = a.address < b.address ? -1 : (a.address > b.address ? 1 : 0);
                                                    break;
                                            case 4:
                                                    cmp = static_cast<int>(a.source) - static_cast<int>(b.source);
                                                    break;
                                            default:
                                                    break;
                                    }
                                    if (cmp == 0)
                                            cmp = a.id < b.id ? -1 : (a.id > b.id ? 1 : 0);
                                    return spec.SortDirection == ImGuiSortDirection_Ascending ? cmp < 0 : cmp > 0;
                            });
                        // SpecsDirty is also set on the first frame when ImGui restores
                        // table state. Sorting is presentation state, not a user data edit.
                }
                sort->SpecsDirty = false;
        }

        uint64_t moveSource = 0;
        uint64_t moveTarget = 0;
        bool     moveAfter  = false;
        uint64_t deleteId   = 0;
        for (auto &property : instance.properties) {
                ImGui::PushID(("instance_property_" + std::to_string(property.id)).c_str());
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                ImGui::SmallButton("=##property_grip");
                if (ImGui::BeginDragDropSource()) {
                        const DevicePropertyMovePayload payload{instance.id, property.id};
                        ImGui::SetDragDropPayload("DND_DEVICE_PROPERTY_MOVE", &payload, sizeof(payload));
                        ImGui::Text(tr("Drag: %s", "拖动: %s"), property.name.c_str());
                        ImGui::EndDragDropSource();
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Drag to reorder", "拖动排序"));
                ImGui::SameLine();
                ImGui::Selectable(property.name.empty() ? tr("Unnamed", "未命名") : property.name.c_str(),
                                  false,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
                if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload *drop = ImGui::AcceptDragDropPayload("DND_DEVICE_PROPERTY_MOVE")) {
                                if (drop->DataSize == sizeof(DevicePropertyMovePayload)) {
                                        const auto &payload = *static_cast<const DevicePropertyMovePayload *>(drop->Data);
                                        if (payload.instanceId == instance.id && payload.propertyId != property.id) {
                                                moveSource = payload.propertyId;
                                                moveTarget = property.id;
                                                moveAfter  = ImGui::GetMousePos().y >=
                                                            (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
                                        }
                                }
                        }
                        ImGui::EndDragDropTarget();
                }

                auto drawContextMenu = [&]() {
                        if (ImGui::MenuItem(tr("Delete", "删除")))
                                deleteId = property.id;
                        if (property.source == PropertySource::JLink) {
                                ImGui::Separator();
                                ImGui::TextDisabled("%s",
                                                    property.symbol.empty() ? property.name.c_str() : property.symbol.c_str());
                                ImGui::TextDisabled("0x%08X", property.address);
                                if (ImGui::MenuItem(tr("Unbind J-Link", "解除 J-Link 绑定"))) {
                                        property.source = PropertySource::Manual;
                                        property.symbol.clear();
                                        property.address        = 0;
                                        property.liveValueKnown = false;
                                        property.liveReadOk     = false;
                                        property.writePending   = false;
                                        modified_               = true;
                                }
                        }
                };
                if (ImGui::BeginPopupContextItem("##property_context")) {
                        drawContextMenu();
                        ImGui::EndPopup();
                }

                ImGui::TableSetColumnIndex(1);
                if (property.editValue.empty())
                        property.editValue = propertyValueString(property);
                const bool valueWasInvalid = property.editValueInvalid;
                if (valueWasInvalid)
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                ImGui::BeginDisabled(property.source == PropertySource::JLink && !property.writable);
                ImGui::SetNextItemWidth(-1.0f);
                if (inputTextString("##value", property.editValue)) {
                        Property parsed = property;
                        if (setPropertyValue(parsed, property.editValue)) {
                                property.data             = parsed.data;
                                property.editValueInvalid = false;
                                modified_                 = true;
                        } else {
                                property.editValueInvalid = true;
                        }
                }
                property.editActive    = ImGui::IsItemActive();
                const bool commitWrite = ImGui::IsItemDeactivatedAfterEdit() && !property.editValueInvalid;
                ImGui::EndDisabled();
                if (valueWasInvalid)
                        ImGui::PopStyleColor();
                if (commitWrite && property.source == PropertySource::JLink)
                        queuePropertyWrite(property);

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(stableTypeName(property.type));

                ImGui::TableSetColumnIndex(3);
                if (property.source == PropertySource::JLink && property.address != 0)
                        ImGui::Text("0x%08X", property.address);
                else
                        ImGui::TextDisabled("%s", tr("UNKNOWN", "未知"));

                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(property.source == PropertySource::JLink ? "J-Link" : "LOCAL");

                ImGui::TableSetColumnIndex(5);
                const bool hasProblem = property.editValueInvalid || !property.lastWriteOk ||
                                        (property.source == PropertySource::JLink && !property.liveReadOk);
                const bool isBusy = property.writePending;
                if (hasProblem)
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.18f, 0.18f, 1.0f));
                else if (isBusy)
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.48f, 0.12f, 1.0f));
                if (ImGui::Button("...", ImVec2(-FLT_MIN, 0)))
                        ImGui::OpenPopup("##binding_menu");
                if (hasProblem || isBusy)
                        ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                        const char *state = property.editValueInvalid ? tr("Invalid value", "数值无效")
                                            : property.writePending   ? tr("Writing", "写入中")
                                            : !property.lastWriteOk   ? tr("Write failed", "写入失败")
                                            : (property.source == PropertySource::JLink && !property.liveReadOk)
                                                ? (property.liveValueKnown ? tr("J-Link offline", "J-Link 离线")
                                                                           : tr("Waiting for data", "等待数据"))
                                                : tr("Binding options", "绑定选项");
                        ImGui::SetTooltip("%s", state);
                }
                if (ImGui::BeginPopup("##binding_menu")) {
                        drawContextMenu();
                        ImGui::EndPopup();
                }
                ImGui::PopID();
        }
        ImGui::EndTable();

        if (moveSource != 0 && moveTarget != 0) {
                auto source = std::find_if(instance.properties.begin(), instance.properties.end(), [&](const Property &p) {
                        return p.id == moveSource;
                });
                auto target = std::find_if(instance.properties.begin(), instance.properties.end(), [&](const Property &p) {
                        return p.id == moveTarget;
                });
                if (source != instance.properties.end() && target != instance.properties.end()) {
                        const size_t sourceIndex = static_cast<size_t>(source - instance.properties.begin());
                        size_t       targetIndex = static_cast<size_t>(target - instance.properties.begin());
                        Property     moved       = std::move(*source);
                        instance.properties.erase(instance.properties.begin() + sourceIndex);
                        if (sourceIndex < targetIndex)
                                --targetIndex;
                        if (moveAfter)
                                ++targetIndex;
                        targetIndex = std::min(targetIndex, instance.properties.size());
                        instance.properties.insert(instance.properties.begin() + targetIndex, std::move(moved));
                        modified_ = true;
                }
        }
        if (deleteId != 0) {
                instance.properties.erase(std::remove_if(instance.properties.begin(),
                                                         instance.properties.end(),
                                                         [&](const Property &p) { return p.id == deleteId; }),
                                          instance.properties.end());
                modified_ = true;
        }
        acceptPropertyDrop(device, &instance);
}

void
DeviceManager::drawInstance(Device &device, DeviceInstance &instance)
{
        ImGui::TextDisabled("%s ID: %llu", tr("Device", "设备"), static_cast<unsigned long long>(instance.id));
        const std::string previousKey = instance.discoveryKey;
        ImGui::SetNextItemWidth(-1.0f);
        if (inputTextString(tr("Discovery Key (optional)", "发现键（可选）"), instance.discoveryKey)) {
                const std::string candidateKey = trimCopy(instance.discoveryKey);
                const bool        duplicate =
                    !candidateKey.empty() &&
                    std::any_of(device.instances.begin(), device.instances.end(), [&](const DeviceInstance &other) {
                            return other.id != instance.id && other.discoveryKey == candidateKey;
                    });
                if (duplicate) {
                        instance.discoveryKey = previousKey;
                        device.status =
                            tr("Discovery keys must be unique within a device type.", "同一设备类型内的设备发现键必须唯一。");
                        device.statusIsError = true;
                } else {
                        if (previousKey.empty() && !instance.discoveryKey.empty())
                                instance.customName = true;
                        instance.online = false;
                        modified_       = true;
                }
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
                instance.discoveryKey = trimCopy(instance.discoveryKey);
        if (!instance.discoveryKey.empty()) {
                ImGui::TextColored(instance.online ? ImVec4(0.35f, 0.9f, 0.45f, 1.0f) : ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                                   "%s",
                                   instance.online ? tr("Online", "在线")
                                                   : tr("Offline / not rediscovered", "离线 / 尚未重新发现"));
        }
        ImGui::SetNextItemWidth(-1.0f);
        if (inputTextString(tr("Device Name", "设备名称"), instance.name)) {
                if (!instance.discoveryKey.empty())
                        instance.customName = true;
                modified_ = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && trimCopy(instance.name).empty()) {
                instance.name = std::string(tr("Device", "设备")) + " " + std::to_string(instance.id);
                if (!instance.discoveryKey.empty())
                        instance.customName = true;
                modified_ = true;
        }
        if (instance.customName && !instance.discoveredName.empty()) {
                if (ImGui::SmallButton(tr("Use Discovered Name", "使用发现名称"))) {
                        instance.name       = instance.discoveredName;
                        instance.customName = false;
                        modified_           = true;
                }
        }

        if (ImGui::BeginTabBar("##instance_tabs")) {
                if (ImGui::BeginTabItem(tr("Properties", "属性"))) {
                        drawInstanceProperties(device, instance);
                        ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(tr("Methods", "方法"))) {
                        drawMethods(device, &instance);
                        ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
        }
}

void
DeviceManager::drawDevice(Device &device)
{
        ImGui::TextDisabled("ID: %llu", static_cast<unsigned long long>(device.id));
        ImGui::SetNextItemWidth(-1.0f);
        if (inputTextString(tr("Device Type Name", "设备类型名称"), device.name))
                modified_ = true;
        if (ImGui::IsItemDeactivatedAfterEdit() && trimCopy(device.name).empty()) {
                device.name = std::string(tr("Device Type", "设备类型")) + " " + std::to_string(device.id);
                modified_   = true;
        }

        ImGui::SeparatorText(tr("Files", "文件"));
        if (ImGui::Button(tr("Browse Library", "浏览动态库"))) {
                std::string path = nativeDlgOpen(tr("Select Dynamic Library", "选择动态库"),
#if defined(_WIN32)
                                                 {
                                                         {"DLL", {"dll"}},
                                                         {
                                                                 "All Files", {"*"}
                                                         }
                                                 }
#elif defined(__APPLE__)
                                                 {{"Dynamic Library", {"dylib"}}, {"All Files", {"*"}}}
#else
                                                 {{"Shared Library", {"so"}}, {"All Files", {"*"}}}
#endif
                );
                if (!path.empty() && path != device.libraryPath) {
                        device.loader.unload();
                        device.libraryPath = std::move(path);
                        modified_          = true;
                }
        }
        ImGui::SameLine();
        if (device.loader.isLoaded()) {
                if (ImGui::Button(tr("Unload", "卸载"))) {
                        device.loader.unload();
                        device.status        = tr("Dynamic library unloaded.", "动态库已卸载。");
                        device.statusIsError = false;
                }
        } else {
                ImGui::BeginDisabled(device.libraryPath.empty());
                if (ImGui::Button(tr("Load", "加载")))
                        loadLibrary(device);
                ImGui::EndDisabled();
        }
        ImGui::TextWrapped("%s: %s",
                           tr("Library", "动态库"),
                           device.libraryPath.empty() ? tr("(not selected)", "（未选择）") : device.libraryPath.c_str());

        if (ImGui::Button(tr("Browse Header", "浏览头文件"))) {
                std::string path = nativeDlgOpen(tr("Select C Header", "选择 C 头文件"),
                                                 {{"C/C++ Header", {"h", "hpp", "hh", "hxx"}}, {"All Files", {"*"}}});
                if (!path.empty()) {
                        if (path != device.headerPath) {
                                device.headerPath = std::move(path);
                                modified_         = true;
                        }
                        loadHeader(device);
                }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(device.headerPath.empty());
        if (ImGui::Button(tr("Reparse", "重新解析")))
                loadHeader(device);
        ImGui::EndDisabled();
        ImGui::TextWrapped("%s: %s",
                           tr("Header", "头文件"),
                           device.headerPath.empty() ? tr("(not selected)", "（未选择）") : device.headerPath.c_str());

        if (!device.status.empty()) {
                ImGui::TextColored(device.statusIsError ? ImVec4(1.0f, 0.4f, 0.35f, 1.0f) : ImVec4(0.35f, 0.9f, 0.45f, 1.0f),
                                   "%s",
                                   device.status.c_str());
        }

        ImGui::Separator();
        if (ImGui::BeginTabBar("##device_tabs")) {
                if (ImGui::BeginTabItem(tr("Properties", "属性"))) {
                        drawProperties(device);
                        ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(tr("Methods", "方法"))) {
                        drawMethods(device);
                        ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
        }
}

void
DeviceManager::draw()
{
        if (!open_) {
                if (poll_) {
                        std::lock_guard lock(poll_->mutex);
                        poll_->requests.clear();
                }
                return;
        }
        refreshJLinkProperties();
        ImGui::SetNextWindowSize(ImVec2(1050.0f, 680.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(tr("Device Manager###DeviceManager", "设备管理器###DeviceManager"), &open_)) {
                pendingDropFiles_.clear();
                ImGui::End();
                return;
        }

        if (!pendingDropFiles_.empty()) {
                const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_RootWindow);
                if (!hovered) {
                        // Every interested tool receives the OS drop; only the hovered
                        // window consumes it. Always clear to avoid a stale later apply.
                } else if (selectedDevice_ < 0 || selectedDevice_ >= static_cast<int>(devices_.size())) {
                        managerStatus_ =
                            tr("Create or select a device type before dropping files.", "拖入文件前请先新建或选择设备类型。");
                        managerStatusIsError_ = true;
                } else {
                        Device &device = *devices_[selectedDevice_];
                        for (const auto &path : pendingDropFiles_) {
                                const std::string lower = lowerCopy(path);
                                if (hasExtension(lower, {".h", ".hpp", ".hh", ".hxx"})) {
                                        if (device.headerPath != path) {
                                                device.headerPath = path;
                                                modified_         = true;
                                        }
                                        loadHeader(device);
                                } else if (hasExtension(lower, {".dll", ".dylib", ".so"}) ||
                                           lower.find(".so.") != std::string::npos) {
                                        if (device.libraryPath != path) {
                                                device.loader.unload();
                                                device.libraryPath = path;
                                                modified_          = true;
                                        }
                                        loadLibrary(device);
                                } else {
                                        managerStatus_        = tr("Dropped file is not a supported library or header.",
                                                            "拖入的文件不是受支持的动态库或头文件。");
                                        managerStatusIsError_ = true;
                                }
                        }
                }
                pendingDropFiles_.clear();
        }

        if (!managerStatus_.empty()) {
                ImGui::TextColored(managerStatusIsError_ ? ImVec4(1.0f, 0.4f, 0.35f, 1.0f) : ImVec4(0.35f, 0.9f, 0.45f, 1.0f),
                                   "%s",
                                   managerStatus_.c_str());
                ImGui::Separator();
        }

        const ImVec2    available     = ImGui::GetContentRegionAvail();
        constexpr float splitterWidth = 6.0f;
        const float     maxSidebar    = std::max(100.0f, available.x - splitterWidth - 360.0f);
        const float     minSidebar    = std::min(180.0f, maxSidebar);
        sidebarWidth_                 = std::clamp(sidebarWidth_, minSidebar, maxSidebar);
        drawDeviceList();
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::InvisibleButton("##device_manager_splitter", ImVec2(splitterWidth, std::max(1.0f, available.y)));
        const bool splitterHovered = ImGui::IsItemHovered();
        const bool splitterActive  = ImGui::IsItemActive();
        if (splitterHovered || splitterActive)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (splitterActive) {
                sidebarWidth_ = std::clamp(sidebarWidth_ + ImGui::GetIO().MouseDelta.x, minSidebar, maxSidebar);
                modified_     = true;
        }
        if (splitterHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                sidebarWidth_ = std::clamp(260.0f, minSidebar, maxSidebar);
                modified_     = true;
        }
        const ImVec2 splitterMin   = ImGui::GetItemRectMin();
        const ImVec2 splitterMax   = ImGui::GetItemRectMax();
        const ImU32  splitterColor = ImGui::GetColorU32(splitterActive    ? ImGuiCol_SeparatorActive
                                                       : splitterHovered ? ImGuiCol_SeparatorHovered
                                                                         : ImGuiCol_Separator);
        const float  splitterX     = (splitterMin.x + splitterMax.x) * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(splitterX, splitterMin.y), ImVec2(splitterX, splitterMax.y), splitterColor, splitterActive ? 2.0f : 1.0f);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::BeginChild("##device_detail", ImVec2(0.0f, 0.0f), true);
        if (selectedDevice_ < 0 || selectedDevice_ >= static_cast<int>(devices_.size())) {
                ImGui::TextDisabled("%s", tr("Create or select a device type.", "请新建或选择设备类型。"));
        } else {
                Device &device = *devices_[selectedDevice_];
                if (selectedInstance_ >= static_cast<int>(device.instances.size()))
                        selectedInstance_ = -1;
                if (selectedInstance_ >= 0) {
                        if (ImGui::Button(tr("Delete Device", "删除设备"))) {
                                pendingDeleteInstanceId_ = device.instances[static_cast<size_t>(selectedInstance_)].id;
                                ImGui::OpenPopup(
                                    tr("Delete Device?###ConfirmDeleteDevice", "删除设备？###ConfirmDeleteDevice"));
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(tr("Back to Device Type", "返回设备类型")))
                                selectedInstance_ = -1;
                        bool instanceDeleted = false;
                        if (ImGui::BeginPopupModal(
                                tr("Delete Device?###ConfirmDeleteDevice", "删除设备？###ConfirmDeleteDevice"),
                                nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
                                ImGui::TextWrapped("%s",
                                                   tr("Delete this concrete device and all of its property values?",
                                                      "确定删除这个具体设备及其全部属性值吗？"));
                                if (ImGui::Button(tr("Delete", "删除"))) {
                                        const auto found = std::find_if(device.instances.begin(),
                                                                        device.instances.end(),
                                                                        [&](const DeviceInstance &candidate) {
                                                                                return candidate.id == pendingDeleteInstanceId_;
                                                                        });
                                        if (found != device.instances.end()) {
                                                deleteInstance(
                                                    device, static_cast<int>(std::distance(device.instances.begin(), found)));
                                                instanceDeleted = true;
                                        }
                                        pendingDeleteInstanceId_ = 0;
                                        ImGui::CloseCurrentPopup();
                                }
                                ImGui::SameLine();
                                if (ImGui::Button(tr("Cancel", "取消"))) {
                                        pendingDeleteInstanceId_ = 0;
                                        ImGui::CloseCurrentPopup();
                                }
                                ImGui::EndPopup();
                        }
                        ImGui::Separator();
                        if (!instanceDeleted && selectedInstance_ >= 0) {
                                drawInstance(device, device.instances[static_cast<size_t>(selectedInstance_)]);
                        }
                } else {
                        if (ImGui::Button(tr("Export Type .avadev", "导出类型 .avadev")))
                                exportDevice(device);
                        ImGui::SameLine();
                        if (ImGui::Button(tr("Add Device", "添加设备")))
                                createInstance(device);
                        ImGui::SameLine();
                        if (ImGui::Button(tr("Delete Device Type", "删除设备类型"))) {
                                pendingDeleteTypeId_ = device.id;
                                ImGui::OpenPopup(tr("Delete Device Type?###ConfirmDeleteDeviceType",
                                                    "删除设备类型？###ConfirmDeleteDeviceType"));
                        }
                        bool typeDeleted = false;
                        if (ImGui::BeginPopupModal(
                                tr("Delete Device Type?###ConfirmDeleteDeviceType", "删除设备类型？###ConfirmDeleteDeviceType"),
                                nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
                                ImGui::TextWrapped(
                                    tr("Delete this device type, all concrete devices, properties, and method bindings?",
                                       "确定删除该设备类型以及其全部具体设备、属性和方法绑定吗？"));
                                if (ImGui::Button(tr("Delete", "删除"))) {
                                        const auto found =
                                            std::find_if(devices_.begin(), devices_.end(), [&](const auto &candidate) {
                                                    return candidate->id == pendingDeleteTypeId_;
                                            });
                                        if (found != devices_.end()) {
                                                deleteDevice(static_cast<int>(std::distance(devices_.begin(), found)));
                                                typeDeleted = true;
                                        }
                                        pendingDeleteTypeId_ = 0;
                                        ImGui::CloseCurrentPopup();
                                }
                                ImGui::SameLine();
                                if (ImGui::Button(tr("Cancel", "取消"))) {
                                        pendingDeleteTypeId_ = 0;
                                        ImGui::CloseCurrentPopup();
                                }
                                ImGui::EndPopup();
                        }
                        ImGui::Separator();
                        if (!typeDeleted)
                                drawDevice(device);
                }
        }
        ImGui::EndChild();
        ImGui::End();
}
