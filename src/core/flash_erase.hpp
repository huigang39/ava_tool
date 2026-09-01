#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace flasherase
{
struct Sector {
        unsigned bank, index;
        uint32_t address, size;
};
/**
 * @brief STM32H745xI: RM0399, main Flash = two 1 MiB banks, eight 128 KiB sectors per bank
 *        Deliberately do not apply this layout to other STM32H7 variants or option bytes
 */
inline std::vector<Sector>
sectors(std::string device)
{
        std::transform(device.begin(), device.end(), device.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        if (device != "STM32H745II" && device != "STM32H745II_M7" && device != "STM32H745II_M4")
                return {};
        std::vector<Sector> out;
        for (unsigned bank = 0; bank < 2; ++bank)
                for (unsigned sector = 0; sector < 8; ++sector)
                        out.push_back({bank + 1, sector, 0x08000000u + bank * 0x100000u + sector * 0x20000u, 0x20000u});
        return out;
}
inline std::string
script(const std::string &device, bool entireChip, uint32_t mask)
{
        const auto layout = sectors(device);
        if (!entireChip && (layout.empty() || !mask || (mask & ~0xffffu)))
                throw std::invalid_argument("未选择有效的 Flash 扇区");
        std::string out = "ExitOnError 1\nexec SetRestartOnClose = 0\nRSetType 0\nr\nh\n";
        if (entireChip)
                out += "Erase\n";
        else {
                for (size_t i = 0; i < layout.size(); ++i) {
                        if (!(mask & (1u << i)))
                                continue;
                        const auto &sector = layout[i];
                        char        command[96];
                        // End lies within the selected sector, never on the next sector's boundary
                        snprintf(command,
                                 sizeof(command),
                                 "Erase 0x%08X 0x%08X noreset\n",
                                 sector.address,
                                 sector.address + sector.size - 1);
                        out += command;
                }
        }
        return out + "h\nexit\n";
}
} // namespace flasherase
