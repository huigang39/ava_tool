#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace firmware_identity
{
// All addresses below come from the selected ELF, never from JSON offsets
struct Image {
        struct Segment {
                uint32_t address, offset, size;
        };
        std::vector<uint8_t>    bytes;
        std::vector<Segment>    segments;
        std::array<uint8_t, 92> descriptor{};
        uint32_t                descriptorAddress{};

        static uint32_t word(const uint8_t *p)
        {
                return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        }
        uint32_t     field(size_t offset) const { return word(descriptor.data() + offset); }
        static Image load(const std::filesystem::path &path, uint32_t address)
        {
                Image         image;
                std::ifstream input(path, std::ios::binary | std::ios::ate);
                const auto    length = input.tellg();
                if (!input || length < 52 || length > 64 * 1024 * 1024)
                        throw std::runtime_error("Cannot read firmware AXF");
                image.bytes.resize(static_cast<size_t>(length));
                input.seekg(0);
                input.read(reinterpret_cast<char *>(image.bytes.data()), length);
                const auto *data = image.bytes.data();
                if (!input || !std::equal(data,
                                          data + 6,
                                          "\x7f"
                                          "ELF\x01\x01"))
                        throw std::runtime_error("Parameter identity requires little-endian ELF32");
                const uint32_t table  = word(data + 28);
                const uint32_t stride = data[42] | uint32_t(data[43]) << 8;
                const uint32_t count  = data[44] | uint32_t(data[45]) << 8;
                if (stride < 32 || uint64_t(table) + uint64_t(stride) * count > image.bytes.size())
                        throw std::runtime_error("Invalid AXF load table");
                bool found = false;
                for (uint32_t index = 0; index < count; ++index) {
                        const auto *entry = data + table + stride * index;
                        if (word(entry) != 1)
                                continue;
                        const uint32_t offset = word(entry + 4), virtualAddress = word(entry + 8);
                        const uint32_t physicalAddress = word(entry + 12), size = word(entry + 16);
                        if (uint64_t(offset) + size > image.bytes.size())
                                throw std::runtime_error("Invalid AXF load range");
                        if (!size || physicalAddress < 0x08000000U || uint64_t(physicalAddress) + size > 0x08200000U)
                                continue;
                        image.segments.push_back({physicalAddress, offset, size});
                        if (address >= virtualAddress && uint64_t(address - virtualAddress) + 92 <= size) {
                                std::copy_n(data + offset + address - virtualAddress, 92, image.descriptor.begin());
                                image.descriptorAddress = physicalAddress + address - virtualAddress;
                                found                   = true;
                        }
                }
                if (!found || image.field(0) != 0x50434F46U || image.field(4) != 1 || image.field(8) != 1)
                        throw std::runtime_error("AXF has no supported FOC parameter identity; rebuild firmware");
                return image;
        }
        void validateManifest(uint32_t schema, uint32_t size, uint32_t fingerprint, uint32_t count) const
        {
                if (field(8) != schema || field(12) != size || field(16) != fingerprint || field(20) != count)
                        throw std::runtime_error("Parameter manifest does not match AXF; use one firmware package");
        }
        template <class Read> void verify(Read read) const
        {
                std::array<uint8_t, 92> buffer{};
                if (!read(descriptorAddress, uint32_t(buffer.size()), buffer.data()))
                        throw std::runtime_error("Cannot read device parameter descriptor; check J-Link connection and retry");
                // Parameter compatibility depends on the ABI and the RAM addresses used by this AXF
                // Defaults (24..55), firmware version (56..59), and build time (68..91)
                // may differ between compatible builds. Executable bytes need not match either
                if (!std::equal(descriptor.begin(), descriptor.begin() + 24, buffer.begin()) ||
                    !std::equal(descriptor.begin() + 60, descriptor.begin() + 68, buffer.begin() + 60))
                        throw std::runtime_error(
                            "Device parameter layout or addresses do not match AXF; load a compatible AXF");
        }
};
} // namespace firmware_identity
