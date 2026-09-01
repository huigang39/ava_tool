#pragma once
#include "../../sdk/ava_eth/include/ava/controller.h"
#include "motor_fault_definitions.hpp"
#include <cstdio>
#include <string>
#include <vector>

namespace motor_faults
{
enum class Kind { Fault, Warning, Calibration, Unknown };
struct Entry {
        unsigned      page, bit;
        Kind          kind;
        const char   *symbol, *zh;
        const Detail *detail = nullptr;
};
inline std::vector<Entry>
decode(const uint32_t (&pages)[8])
{
        std::vector<Entry> entries;
        for (unsigned page = 0; page < 8; ++page)
                for (unsigned bit = 0; bit < 32; ++bit) {
                        if (!(pages[page] & (uint32_t(1) << bit)))
                                continue;
                        const Definition *found = nullptr;
                        for (const auto &def : definitions)
                                if (def.page == page && def.bit == bit) {
                                        found = &def;
                                        break;
                                }
                        entries.push_back(
                            {page,
                             bit,
                             !found      ? Kind::Unknown
                             : page == 2 ? Kind::Warning
                             : page == 3 ? Kind::Calibration
                                         : Kind::Fault,
                             found ? found->symbol : "unknown",
                             found ? found->zh : "当前 FocDriver 未定义此协议位,保留原始值;可能来自 M4 或其他固件版本",
                             found ? found->detail : nullptr});
                }
        return entries;
}
inline const char *
controllerReason(uint32_t code)
{
        switch (code) {
                case 0:
                        return "无控制器错误";
                case AVA_ETH_OFFLINE:
                        return "通信已停止或设备离线";
                case AVA_ETH_EXPIRED:
                        return "控制命令看门狗超时";
                case AVA_ETH_ERR_RX_TIMEOUT:
                        return "PDO 接收超时";
                case AVA_ETH_ERR_WKC:
                        return "PDO 工作计数 WKC 异常";
                case AVA_ETH_ERR_AL:
                        return "从站 EtherCAT AL 状态异常";
                case AVA_ETH_ERR_SEND:
                        return "PDO 发送失败";
                case AVA_ETH_ERR_DATA:
                        return "PDO 数据无效";
                case AVA_ETH_ERR_DRIVE_FAULT:
                        return "驱动器处于 CiA402 故障状态,具体原因见固件错误页";
                default:
                        return "未识别的控制器错误(不是 FocDriver 错误位图)";
        }
}
inline std::string
describe(const Entry &entry)
{
        char source[80];
        std::snprintf(source,
                      sizeof(source),
                      "Error%u 0x%04X bit%u (0x%08X): ",
                      entry.page + 1,
                      0x2030 + entry.page * 4,
                      entry.bit,
                      uint32_t(1) << entry.bit);
        return std::string(source) + entry.zh + " [" + entry.symbol + "]";
}
} // namespace motor_faults
