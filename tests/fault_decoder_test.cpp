#include "core/fault_decoder.hpp"
#include <cassert>
#include <iostream>
int main()
{
    using namespace motor_faults;
    uint32_t word = 123;
    for (const auto *bad : {"", "0x", "-1", "+1", "1.0", "1e3", "0b10", "FF", "0x100000000", "4294967296", "1 2"}) {
        assert(!parseWord(bad, word)); assert(word == 123);
    }
    assert(parseWord(" \t0X80000001\r\n", word) && word == 0x80000001u);
    assert(parseWord("4294967295", word) && word == UINT32_MAX);
    assert(parseWord("00010", word) && word == 10);
    assert(decodeWord(*findPage("ext2"), 8).unknownMask == 8);
    assert(decodeWord(*findPage("ext1"), 1u<<14).unknownMask == 1u<<14);
    assert(std::string(findDetail(*findPage("ext4"), 7)->symbol) == "tor_sensor_cali_timeout");
    assert(std::string(findDetail(*findPage("fault.cali"), 7)->symbol) != "tor_sensor_cali_timeout");
    auto pre = decodeWord(*findPage("predrv.err"), 0x80000003);
    assert(pre.items.size() == 1 && pre.commCode == 3 && pre.unknownMask == 0);
    assert(decodeWord(*findPage("cali.reason"), 3).items.size() == 1);
    assert(decodeWord(*findPage("cali.reason"), UINT32_MAX).unknownValue);
    assert(!decodeWord(*findPage("cali.reason"), 0).items.empty());
    std::string id, val;
    while (std::cin >> id >> val) {
        const auto *page = findPage(id);
        assert(page && parseWord(val, word));
        const auto result = decodeWord(*page, word);
        std::cout << result.unknownMask << ' ' << result.commCode << ' ' << result.unknownValue;
        for (const auto *item : result.items) std::cout << ' ' << item->val;
        std::cout << '\n';
    }
}
