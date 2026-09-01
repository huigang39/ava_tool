#pragma once

#include "core/firmware_match.hpp"
#include "gui/i18n.hpp"
#include <string_view>

/**
 * @brief 区分文件加载, 暂时无法校验和已确认不匹配
 */
inline const char *
firmwareStatusLabel(const firmware_match::Status &status)
{
        using firmware_match::State;
        switch (status.state) {
                case State::Matched:
                        return tr("Device / AXF verified", "设备与 AXF 已匹配");
                case State::Mismatch:
                        return status.forced ? tr("AXF/ELF forced: device firmware does not match",
                                                  "已强制使用 AXF/ELF: 与设备固件不匹配")
                                             : tr("Device / AXF/ELF mismatch: variable access paused",
                                                  "设备固件与 AXF/ELF 不匹配, 变量读写和采样已暂停");
                case State::Loading:
                        return tr("Loading AXF/ELF: variable access paused", "正在加载 AXF/ELF, 变量读写和采样暂时暂停");
                case State::Unavailable:
                        return tr("Firmware identity unavailable: variable access paused",
                                  "暂时无法校验固件身份, 变量读写和采样已暂停");
                case State::Unverified:
                        return tr("AXF unverified: legacy debugging remains available", "AXF 未校验: 旧固件仍可正常调试");
        }
        return "";
}

/**
 * @brief 在界面翻译身份校验原因, 未知原因及不匹配文件路径原样保留
 */
inline const char *
firmwareStatusDetail(const firmware_match::Status &status)
{
        if (g_lang != Lang::ZH || status.state == firmware_match::State::Mismatch)
                return status.detail.c_str();
        static constexpr struct {
                std::string_view en;
                const char      *zh;
        } translations[] = {
            {"AXF is loading", "正在加载符号文件, 完成后会重新校验"},
            {"Cannot read device startup identity", "读取运行标识失败. 请检查调试连接, 所选 CPU 内核及 MCU 状态"},
            {"Firmware supports identity; waiting for startup publication",
             "Flash 中已有固件标识, 但尚未读到运行标识. 请检查 MCU 是否已正常启动及运行标识是否成功发布"},
            {"Device identity became unavailable; reconnect or wait for startup",
             "运行标识暂时无法读取. 请等待 MCU 启动完成, 或重新连接"},
            {"Device identity protocol is unsupported", "不支持设备的固件标识版本"},
            {"Device identity is incomplete", "运行标识尚未写入完整, 正在等待下一次校验"},
            {"Device identity contains an invalid Flash address", "运行标识中的 Flash 地址无效"},
            {"Cannot read device Flash identity", "无法读取 Flash 中的固件标识. 请检查调试连接"},
            {"Cannot read active interrupt vector table", "无法读取当前中断向量表地址. 请检查调试连接"},
            {"Cannot read the active reset vector", "无法读取当前复位向量. 请检查 MCU 状态及调试连接"},
            {"Device reset or firmware changed during identity verification",
             "校验时运行标识发生变化或与 Flash 不一致. 请等待 MCU 启动完成后重新校验"},
            {"Retained identity does not describe the current firmware; legacy debugging remains available after reconnect",
             "RAM 中保留的标识不属于当前固件. 若已换回旧固件, 可重新连接后继续调试"},
            {"Retained identity belongs to another firmware image; legacy debugging remains available after reconnect",
             "RAM 标识与当前运行固件不一致. 若已换回旧固件, 可重新连接后继续调试"},
            {"AXF has no build identity; legacy debugging remains available", "AXF 未提供构建标识, 可继续调试旧固件"},
            {"Device has no build identity; legacy debugging remains available", "设备未提供构建标识, 可继续调试旧固件"},
            {"Identified AXF matches; other legacy AXF files remain unverified", "带标识的 AXF 已匹配, 其他旧 AXF 尚未校验"},
            {"Device and AXF build identities match", "设备与 AXF 的构建标识一致"},
        };
        for (const auto &translation : translations)
                if (status.detail == translation.en)
                        return translation.zh;
        return status.detail.c_str();
}
