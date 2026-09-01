#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace firmware_match
{

constexpr uint32_t magic          = 0x42415641U;
constexpr uint32_t mailboxAddress = 0x2001FFC0U;
constexpr size_t   infoSize       = 64;
using Bytes                       = std::array<uint8_t, infoSize>;
using Digest                      = std::array<uint8_t, 32>;

// PSC3M5 secure instruction/data aliases refer to the same 256 KiB Flash.
inline bool
inRange(uint64_t address, uint32_t base, uint32_t size, size_t length)
{
        return length <= size && address >= base && address - base <= size - length;
}
inline bool
psc3Flash(uint64_t address, size_t length = infoSize)
{
        return inRange(address, 0x12000000U, 0x40000U, length) || inRange(address, 0x32000000U, 0x40000U, length);
}
inline uint32_t
codeAddress(uint32_t address)
{
        return inRange(address, 0x32000000U, 0x40000U, 1) ? address - 0x20000000U : address;
}
inline bool
flashRange(uint64_t address, bool psc3, size_t length = infoSize)
{
        return psc3 ? psc3Flash(address, length) : inRange(address, 0x08000000U, 0x200000U, length);
}
inline uint32_t
mailboxFor(uint32_t address)
{
        return psc3Flash(address) ? 0x3400FFC0U : mailboxAddress;
}

// 在同一文件快照上解析符号和构建标识, 避免自动编译替换 AXF 时混用两个版本
class FileSnapshot
{
      public:
        explicit FileSnapshot(const std::filesystem::path &source)
        {
                static std::atomic<uint64_t> sequence{};
                const auto                   stamp = std::chrono::steady_clock::now().time_since_epoch().count();
                const auto                   base  = std::filesystem::temp_directory_path();
                for (;;) {
                        directory_ = base / ("ava-axf-" + std::to_string(stamp) + "-" + std::to_string(sequence.fetch_add(1)));
                        if (std::filesystem::create_directory(directory_))
                                break;
                }
                path = directory_ / "symbols.axf";
                try {
                        std::filesystem::copy_file(source, path);
                } catch (...) {
                        cleanup();
                        throw;
                }
        }
        FileSnapshot(const FileSnapshot &)            = delete;
        FileSnapshot &operator=(const FileSnapshot &) = delete;
        ~FileSnapshot() { cleanup(); }
        std::filesystem::path path;

      private:
        std::filesystem::path directory_;
        void                  cleanup()
        {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
                std::filesystem::remove(directory_, ignored);
        }
};

inline uint32_t
word(const uint8_t *p)
{
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

inline std::string
hex(const uint8_t *data, size_t size)
{
        static constexpr char digits[] = "0123456789abcdef";
        std::string           result(size * 2, '0');
        for (size_t i = 0; i < size; ++i) {
                result[i * 2]     = digits[data[i] >> 4];
                result[i * 2 + 1] = digits[data[i] & 15];
        }
        return result;
}

inline Digest
sha256(std::vector<uint8_t> bytes)
{
        constexpr std::array<uint32_t, 64> constants = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::array<uint32_t, 8> hash = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        const uint64_t bits = uint64_t(bytes.size()) * 8;
        bytes.push_back(0x80);
        while (bytes.size() % 64 != 56)
                bytes.push_back(0);
        for (int shift = 56; shift >= 0; shift -= 8)
                bytes.push_back(uint8_t(bits >> shift));
        for (size_t offset = 0; offset < bytes.size(); offset += 64) {
                std::array<uint32_t, 64> words{};
                for (size_t i = 0; i < 16; ++i) {
                        const auto *p = bytes.data() + offset + i * 4;
                        words[i]      = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
                }
                for (size_t i = 16; i < 64; ++i) {
                        const uint32_t a = words[i - 15], b = words[i - 2];
                        words[i] = words[i - 16] + (std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3)) + words[i - 7] +
                                   (std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10));
                }
                auto state = hash;
                for (size_t i = 0; i < 64; ++i) {
                        const uint32_t e = state[4], a = state[0];
                        const uint32_t t1 = state[7] + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                                            ((e & state[5]) ^ (~e & state[6])) + constants[i] + words[i];
                        const uint32_t t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) +
                                            ((a & state[1]) ^ (a & state[2]) ^ (state[1] & state[2]));
                        for (size_t j = 7; j > 0; --j)
                                state[j] = state[j - 1];
                        state[4] += t1;
                        state[0]  = t1 + t2;
                }
                for (size_t i = 0; i < hash.size(); ++i)
                        hash[i] += state[i];
        }
        Digest result{};
        for (size_t i = 0; i < result.size(); ++i)
                result[i] = uint8_t(hash[i / 4] >> (24 - 8 * (i % 4)));
        return result;
}

struct Image {
        Bytes       info{};
        uint32_t    address{};
        std::string path;

        std::string id() const { return hex(info.data() + 16, 32); }

        static Image load(const std::filesystem::path &path, uint32_t symbolAddress)
        {
                std::ifstream stream(path, std::ios::binary | std::ios::ate);
                const auto    size = stream.tellg();
                if (!stream || size < 52 || size > 64 * 1024 * 1024)
                        throw std::runtime_error("Cannot read AXF build identity");
                std::vector<uint8_t> bytes(static_cast<size_t>(size));
                stream.seekg(0);
                stream.read(reinterpret_cast<char *>(bytes.data()), size);
                if (!stream || !std::equal(bytes.begin(),
                                           bytes.begin() + 6,
                                           "\x7f"
                                           "ELF\x01\x01"))
                        throw std::runtime_error("Build identity requires little-endian ELF32");
                const uint32_t table  = word(bytes.data() + 28);
                const uint32_t stride = bytes[42] | uint32_t(bytes[43]) << 8;
                const uint32_t count  = bytes[44] | uint32_t(bytes[45]) << 8;
                if (stride < 32 || uint64_t(table) + uint64_t(stride) * count > bytes.size())
                        throw std::runtime_error("Invalid AXF load table");
                for (uint32_t i = 0; i < count; ++i) {
                        const auto *entry = bytes.data() + table + i * stride;
                        if (word(entry) != 1)
                                continue;
                        const uint32_t fileOffset = word(entry + 4), virtualAddress = word(entry + 8);
                        const uint32_t physicalAddress = word(entry + 12), length = word(entry + 16);
                        if (uint64_t(fileOffset) + length > bytes.size())
                                throw std::runtime_error("Invalid AXF load range");
                        if (symbolAddress < virtualAddress || uint64_t(symbolAddress - virtualAddress) + infoSize > length)
                                continue;
                        const size_t   offset = size_t(fileOffset) + symbolAddress - virtualAddress;
                        Image          image;
                        const uint64_t loadAddress = uint64_t(physicalAddress) + symbolAddress - virtualAddress;
                        const bool     psc3        = psc3Flash(symbolAddress);
                        if (!flashRange(loadAddress, psc3) ||
                            (psc3 && codeAddress(static_cast<uint32_t>(loadAddress)) != codeAddress(symbolAddress)))
                                throw std::runtime_error("AXF build information must reside in Flash");
                        image.address = psc3 ? codeAddress(symbolAddress) : static_cast<uint32_t>(loadAddress);
                        image.path    = path.generic_string();
                        std::copy_n(bytes.data() + offset, infoSize, image.info.begin());
                        if (!flashRange(image.address, psc3) || word(image.info.data()) != magic ||
                            word(image.info.data() + 4) != 1 || word(image.info.data() + 8) != infoSize ||
                            word(image.info.data() + 12) != image.address)
                                throw std::runtime_error("Invalid AXF build information");
                        std::fill_n(bytes.data() + offset + 16, 32, uint8_t(0));
                        auto digest = sha256(bytes);
                        if (psc3 && !std::equal(digest.begin(), digest.end(), image.info.begin() + 16)) {
                                // build_psc3 seals the ELF before producing the Keil AXF.
                                // Reverse only its physical-address alias transformation;
                                // all other bytes remain covered by the original digest.
                                for (uint32_t j = 0; j < count; ++j) {
                                        auto          *entry     = bytes.data() + table + j * stride;
                                        const uint32_t physical  = word(entry + 12);
                                        const uint32_t canonical = codeAddress(physical);
                                        for (unsigned k = 0; k < 4; ++k)
                                                entry[12 + k] = uint8_t(canonical >> (8 * k));
                                }
                                digest = sha256(std::move(bytes));
                        }
                        if (!std::equal(digest.begin(), digest.end(), image.info.begin() + 16))
                                throw std::runtime_error("AXF build identity is invalid; rebuild firmware");
                        return image;
                }
                throw std::runtime_error("AXF build information has no Flash load range");
        }
};

enum class State { Unverified, Matched, Mismatch, Loading, Unavailable };

struct Status {
        State       state{State::Unverified};
        std::string detail;
        std::string deviceId;
        std::string expectedId;
        bool        forced{}; // 用户已允许当前设备与 AXF 不匹配时继续访问
        bool        allowsAccess() const
        {
                return state == State::Unverified || state == State::Matched || (state == State::Mismatch && forced);
        }
};

class Gate
{
      public:
        struct Source {
                std::optional<Image> image;
                bool                 loading{};
        };
        void setSource(uintptr_t owner, std::optional<Image> image, bool loading = false)
        {
                sources_[owner] = {std::move(image), loading};
                status_         = {};
                clearMismatchOverride();
        }
        void removeSource(uintptr_t owner)
        {
                sources_.erase(owner);
                status_ = {};
                clearMismatchOverride();
        }
        void resetConnection()
        {
                recognizedDevice_ = false;
                status_           = {};
                clearMismatchOverride();
        }
        const Status &status() const { return status_; }

        /**
         * @brief 仅为已确认不匹配的当前设备与 AXF 开启或关闭强制访问
         */
        bool setMismatchOverride(bool enabled)
        {
                if (!enabled) {
                        overrideIdentity_.reset();
                        status_.forced = false;
                        return true;
                }
                if (status_.state != State::Mismatch || !checkedIdentity_)
                        return false;
                overrideIdentity_ = checkedIdentity_;
                status_.forced    = true;
                return true;
        }

        /**
         * @brief 校验设备身份; 强制访问只对同一次启动及同一组 AXF 生效
         */
        template <class Read> Status check(Read read)
        {
                checkIdentity(std::move(read));
                status_.forced = status_.state == State::Mismatch && overrideIdentity_ && checkedIdentity_ &&
                                 *overrideIdentity_ == *checkedIdentity_;
                if (!status_.forced)
                        overrideIdentity_.reset();
                return status_;
        }

      private:
        /**
         * @brief 符号来源或连接变化后清除强制访问及上次校验身份
         */
        void clearMismatchOverride()
        {
                overrideIdentity_.reset();
                checkedIdentity_.reset();
                status_.forced = false;
        }

        /**
         * @brief 未提供身份标识的旧固件保持可调试, 暂时无法验证时暂停访问
         */
        template <class Read> Status checkIdentity(Read read)
        {
                checkedIdentity_.reset();
                status_          = {};
                bool hasIdentity = false;
                bool hasLegacy   = false;
                for (const auto &[owner, source] : sources_) {
                        (void)owner;
                        if (source.loading)
                                return status_ = {State::Loading, "AXF is loading"};
                        hasIdentity |= source.image.has_value();
                        hasLegacy   |= !source.image.has_value();
                }
                if (!hasIdentity)
                        return status_ = {State::Unverified, "AXF has no build identity; legacy debugging remains available"};
                uint32_t runtimeMailbox = 0;
                for (const auto &[owner, source] : sources_) {
                        (void)owner;
                        if (!source.image)
                                continue;
                        const auto candidate = mailboxFor(source.image->address);
                        if (runtimeMailbox && runtimeMailbox != candidate)
                                return status_ = {State::Unavailable, "AXF files target incompatible memory maps"};
                        runtimeMailbox = candidate;
                }
                const bool psc3 = runtimeMailbox == 0x3400FFC0U;
                Bytes      runtime{};
                const bool runtimeReadable = read(runtimeMailbox, uint32_t(runtime.size()), runtime.data());
                if (!runtimeReadable || word(runtime.data()) != magic) {
                        if (!recognizedDevice_) {
                                for (const auto &[owner, source] : sources_) {
                                        (void)owner;
                                        if (!source.image)
                                                continue;
                                        Bytes pending{};
                                        if (read(source.image->address, uint32_t(pending.size()), pending.data()) &&
                                            word(pending.data()) == magic && word(pending.data() + 4) == 1 &&
                                            word(pending.data() + 8) == infoSize &&
                                            word(pending.data() + 12) == source.image->address)
                                                return status_ = {
                                                           State::Unavailable,
                                                           runtimeReadable
                                                               ? "Firmware supports identity; waiting for startup publication"
                                                               : "Cannot read device startup identity"};
                                }
                        }
                        return status_ = {recognizedDevice_ ? State::Unavailable : State::Unverified,
                                          recognizedDevice_
                                              ? (runtimeReadable
                                                     ? "Device identity became unavailable; reconnect or wait for startup"
                                                     : "Cannot read device startup identity")
                                              : "Device has no build identity; legacy debugging remains available"};
                }
                if (word(runtime.data() + 4) != 1 || word(runtime.data() + 8) != infoSize)
                        return status_ = {recognizedDevice_ ? State::Unavailable : State::Unverified,
                                          "Device identity protocol is unsupported"};
                if (word(runtime.data() + 52) != ~word(runtime.data() + 48))
                        return status_ = {State::Unavailable, "Device identity is incomplete"};
                const uint32_t address = word(runtime.data() + 12);
                status_.deviceId       = hex(runtime.data() + 16, 32);
                if (!flashRange(address, psc3))
                        return status_ = {State::Unavailable, "Device identity contains an invalid Flash address"};
                Bytes flash{}, again{};
                if (!read(address, uint32_t(flash.size()), flash.data()))
                        return status_ = {State::Unavailable, "Cannot read device Flash identity"};
                // 换回旧固件时 RAM 可能保留上次身份, 必须检查 Flash 和当前中断向量
                if (word(flash.data()) != magic || word(flash.data() + 4) != 1 || word(flash.data() + 8) != infoSize ||
                    word(flash.data() + 12) != address)
                        return status_ = {recognizedDevice_ ? State::Unavailable : State::Unverified,
                                          "Retained identity does not describe the current firmware; legacy debugging remains "
                                          "available after reconnect"};
                std::array<uint8_t, 4> vectorTable{}, resetVector{};
                if (!read(0xE000ED08U, 4, vectorTable.data()))
                        return status_ = {State::Unavailable, "Cannot read active interrupt vector table"};
                const uint32_t vectorAddress = word(vectorTable.data());
                const bool     vectorInMemory =
                    psc3 ? (psc3Flash(vectorAddress, 8) || inRange(vectorAddress, 0x34000000U, 0x10000U, 8))
                             : (vectorAddress < 0x20000U || inRange(vectorAddress, 0x08000000U, 0x200000U, 8) ||
                            inRange(vectorAddress, 0x20000000U, 0x20000U, 8) ||
                            inRange(vectorAddress, 0x24000000U, 0x80000U, 8));
                if (!vectorInMemory || (vectorAddress & 127U) || !read(vectorAddress + 4U, 4, resetVector.data()))
                        return status_ = {State::Unavailable, "Cannot read the active reset vector"};
                if (word(resetVector.data()) != word(flash.data() + 56))
                        return status_ = {recognizedDevice_ ? State::Unavailable : State::Unverified,
                                          "Retained identity belongs to another firmware image; legacy debugging remains "
                                          "available after reconnect"};
                recognizedDevice_ = true;
                if (!read(runtimeMailbox, uint32_t(again.size()), again.data()) || again != runtime ||
                    !std::equal(runtime.begin(), runtime.begin() + 48, flash.begin()) ||
                    !std::equal(runtime.begin() + 56, runtime.end(), flash.begin() + 56))
                        return status_ = {State::Unavailable, "Device reset or firmware changed during identity verification"};
                checkedIdentity_ = runtime;
                for (const auto &[owner, source] : sources_) {
                        (void)owner;
                        if (!source.image)
                                continue;
                        const auto &image = *source.image;
                        if (image.address != address ||
                            !std::equal(image.info.begin(), image.info.begin() + 48, runtime.begin())) {
                                status_.state      = State::Mismatch;
                                status_.detail     = image.path;
                                status_.expectedId = image.id();
                                return status_;
                        }
                }
                status_.state  = State::Matched;
                status_.detail = hasLegacy ? "Identified AXF matches; other legacy AXF files remain unverified"
                                           : "Device and AXF build identities match";
                return status_;
        }

        std::map<uintptr_t, Source> sources_;
        bool                        recognizedDevice_{};
        Status                      status_;
        std::optional<Bytes>        checkedIdentity_;
        std::optional<Bytes>        overrideIdentity_;
};

} // namespace firmware_match
