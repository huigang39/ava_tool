#include "core/hss_stream.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

int main()
{
        // Timestamp + one byte variable + float: an odd frame length also
        // exercises boundaries that do not coincide with USB/buffer alignment
        constexpr std::size_t frameSize = 9;
        std::vector<unsigned char> stream;
        for (std::uint32_t i = 0; i < 100; ++i) {
                const std::uint32_t timestamp = 0xffffff00u + i * 20;
                const float val = static_cast<float>(i) * 0.25f;
                std::array<unsigned char, frameSize> frame{};
                std::memcpy(frame.data(), &timestamp, 4);
                frame[4] = static_cast<unsigned char>(i);
                std::memcpy(frame.data() + 5, &val, 4);
                stream.insert(stream.end(), frame.begin(), frame.end());
        }
        for (std::size_t chunk = 1; chunk <= 64; ++chunk) {
                unsigned char buffer[128]{};
                std::size_t carry = 0, offset = 0;
                std::vector<unsigned char> decoded;
                while (offset < stream.size()) {
                        const auto bytes = std::min(chunk, stream.size() - offset);
                        std::memcpy(buffer + carry, stream.data() + offset, bytes);
                        offset += bytes;
                        const auto total = carry + bytes;
                        const auto complete = total / frameSize * frameSize;
                        decoded.insert(decoded.end(), buffer, buffer + complete);
                        carry = hss_stream::retainTail(buffer, total, frameSize);
                        // Empty reads must also preserve a previously received fragment
                        assert(hss_stream::retainTail(buffer, carry, frameSize) == carry);
                }
                assert(carry == 0 && decoded == stream);
        }
        unsigned char partial[32]{1, 2, 3};
        assert(hss_stream::retainTail(partial, 3, frameSize) == 3);
        assert(partial[0] == 1 && partial[2] == 3);
        std::cout << "PASS: HSS frames survive 1..64-byte reads, empty reads, odd frame sizes and timestamp wrap\n";
}
