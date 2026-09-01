#include "core/motor_jlink.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>

namespace motor_jlink
{
namespace
{
const char *labels[Count] = {"控制模式（CiA402）",
                             "控制字（使能/失能）",
                             "控制状态（CiA402 状态字）",
                             "反馈模式",
                             "特殊指令",
                             "目标位置",
                             "目标速度",
                             "目标电流",
                             "目标转矩",
                             "目标模式",
                             "目标有效",
                             "位置锁存",
                             "速度前馈",
                             "电流前馈",
                             "转矩前馈",
                             "反馈位置",
                             "反馈速度",
                             "反馈电流",
                             "负载转矩",
                             "电磁转矩",
                             "母线电压",
                             "绕组温度",
                             "逆变器温度",
                             "共享内存使能",
                             "CiA402 使能",
                             "自动控制使能",
                             "生效配置有效",
                             "生效方向",
                             "固件速度单位",
                             "驱动保活计数"};
const char *paths[Count]  = {"g_cia402.in.mode_of_op",
                             "g_cia402.in.ctrl_word.val",
                             "g_cia402.out.status_word.val",
                             "g_cia402.out.mode_display",
                             "g_cia402.in.special_cmd",
                             "g_cia402.in.target.pos",
                             "g_cia402.in.target.vel",
                             "g_cia402.in.target.cur",
                             "g_cia402.in.target.tor",
                             "g_cia402.in.target_mode",
                             "g_cia402.in.target_valid",
                             "g_cia402.lo.pos_latch",
                             "g_cia402.in.target.ffd_vel",
                             "g_cia402.in.target.ffd_cur",
                             "g_cia402.in.target.ffd_tor",
                             "g_mc.out.fdb_pvct.pos",
                             "g_mc.out.fdb_pvct.vel",
                             "g_mc.out.fdb_pvct.cur",
                             "g_mc.out.fdb_pvct.load_tor",
                             "g_mc.out.fdb_pvct.elec_tor",
                             "g_mc.in.v_bus",
                             "g_mc.in.temp.stator.uv",
                             "g_mc.in.temp.inverter",
                             "g_comm_shm.cfg.enable",
                             "g_cia402.cfg.enable",
                             "g_auto_ctrl.cfg.enable",
                             "g_mc.active_cfg_valid",
                             "g_mc.active_cfg.sw.ctrl.dir",
                             "g_mc.active_cfg.sw.ctrl.vel_unit",
                             "g_psc3_drive_host_heartbeat"};
bool
positionMode(int mode)
{
        return mode == 1 || mode == -1 || mode == 8 || mode == -8 || mode == -128;
}
bool
pp(int mode)
{
        return mode == 1 || mode == -1;
}
bool
supportedMode(int mode)
{
        constexpr int modes[] = {0, 1, -1, 3, -3, 4, 10, 6, 8, -8, 9, -9, -10, -128, -126, -125, -124};
        return std::find(std::begin(modes), std::end(modes), mode) != std::end(modes);
}
const dwarf::Type *
type(const dwarf::Info &info, u64 id)
{
        for (int depth = 0; depth < 32; ++depth) {
                const auto it = info.types.find(id);
                if (it == info.types.end())
                        return nullptr;
                if (it->second.kind != dwarf::TypeKind::TYPEDEF && it->second.kind != dwarf::TypeKind::MODIFIER)
                        return &it->second;
                id = it->second.inner;
        }
        return nullptr;
}
void
flatten(const dwarf::Info   &info,
        u64                  id,
        u64                  address,
        const std::string   &name,
        const std::string   &source,
        int                  depth,
        std::vector<Symbol> &out)
{
        if (depth > 16 || !address || address > UINT32_MAX || out.size() >= 100000)
                return;
        const auto *t = type(info, id);
        if (!t)
                return;
        if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
                for (const auto &m : t->members)
                        if (!m.bitSize && !m.name.empty())
                                flatten(info, m.type, address + m.offset, name + "." + m.name, source, depth + 1, out);
        } else if (t->kind == dwarf::TypeKind::ARRAY) {
                const auto *element = type(info, t->inner);
                u64         count   = 1;
                if (!element || !element->size || t->dims.empty())
                        return;
                for (auto dim : t->dims) {
                        if (!dim || dim > 4096 || count > 4096 / dim)
                                return;
                        count *= dim;
                }
                for (u64 i = 0; i < count; ++i) {
                        auto        index = i;
                        std::string suffix;
                        for (auto dim = t->dims.rbegin(); dim != t->dims.rend(); ++dim) {
                                suffix  = "[" + std::to_string(index % *dim) + "]" + suffix;
                                index  /= *dim;
                        }
                        flatten(info, t->inner, address + i * element->size, name + suffix, source, depth + 1, out);
                }
        } else if (t->kind == dwarf::TypeKind::BASE || t->kind == dwarf::TypeKind::ENUM) {
                auto size     = t->size;
                auto encoding = t->encoding;
                if (t->kind == dwarf::TypeKind::ENUM && t->inner) {
                        if (const auto *base = type(info, t->inner)) {
                                if (!size)
                                        size = base->size;
                                encoding = base->encoding;
                        }
                }
                const bool floating = encoding == 4;
                const bool sign     = encoding == 5 || encoding == 6 ||
                                  (t->kind == dwarf::TypeKind::ENUM &&
                                   std::any_of(t->enums.begin(), t->enums.end(), [](const auto &e) { return e.val < 0; }));
                if ((floating && (size == 4 || size == 8)) || (!floating && (size == 1 || size == 2 || size == 4)))
                        out.push_back({source, name, uint32_t(address), uint8_t(size), floating, sign});
        }
}
} // namespace
const char *
label(Field f)
{
        return labels[f];
}
const char *
preset(Field f)
{
        return paths[f];
}
bool
optional(Field f)
{
        return (f >= LoadTorque && f <= InverterTemperature) || f == DriveHeartbeat;
}
std::vector<Symbol>
symbols(const dwarf::Info &info, const std::string &source)
{
        std::vector<Symbol> out;
        for (const auto &v : info.variables)
                flatten(info, v.type, v.addr, v.name, source, 0, out);
        std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
        out.erase(std::unique(out.begin(),
                              out.end(),
                              [](const auto &a, const auto &b) { return a.name == b.name && a.address == b.address; }),
                  out.end());
        return out;
}
void
validate(const Config &c)
{
        for (int i = 0; i < Count; ++i) {
                const auto &s = c.fields[i];
                if (!s.address && optional(Field(i)))
                        continue;
                if (!s.address || (s.size != 1 && s.size != 2 && s.size != 4 && s.size != 8) ||
                    uint64_t(s.address) + s.size > uint64_t(UINT32_MAX) + 1)
                        throw std::runtime_error(std::string("J-Link 变量未解析或类型不支持：") + labels[i]);
                const bool analog =
                    (i >= TargetPosition && i <= TargetTorque) || (i >= FeedVelocity && i <= InverterTemperature);
                if (s.floating != analog || (analog && s.size != 4 && s.size != 8) || (!analog && s.size > 4))
                        throw std::runtime_error(std::string("J-Link 变量类型不匹配：") + labels[i]);
                if ((i == Mode || i == TargetMode || i == ActualMode) && !s.signedValue)
                        throw std::runtime_error(std::string("模式变量必须支持有符号 CiA402 编码：") + labels[i]);
                if ((i == Control || i == Status) && s.size < 2)
                        throw std::runtime_error("CiA402 控制字和状态字至少需要 16 位");
                for (int j = 0; j < i; ++j) {
                        const auto &other = c.fields[j];
                        if (other.address && s.address < uint64_t(other.address) + other.size &&
                            other.address < uint64_t(s.address) + s.size)
                                throw std::runtime_error(std::string("J-Link 映射地址重叠：") + labels[i] + " / " + labels[j]);
                }
        }
}
double
Session::read(Field field)
{
        const auto &s = config_.fields[field];
        if (!s.address && optional(field))
                return std::numeric_limits<double>::quiet_NaN();
        uint64_t raw = 0;
        if (!io_.read(epoch_, s.address, s.size, &raw))
                throw std::runtime_error(std::string("J-Link 读取失败或目标已重连：") + labels[field]);
        if (s.floating) {
                if (s.size == 4) {
                        float val;
                        std::memcpy(&val, &raw, 4);
                        return val;
                }
                double val;
                std::memcpy(&val, &raw, 8);
                return val;
        }
        if (s.signedValue) {
                const int shift = 64 - s.size * 8;
                return double(int64_t(raw << shift) >> shift);
        }
        return double(raw);
}
void
Session::write(Field field, double val)
{
        const auto &s = config_.fields[field];
        if (!std::isfinite(val))
                throw std::runtime_error("J-Link 指令不是有限数值");
        uint64_t raw = 0;
        if (s.floating) {
                if (s.size == 4) {
                        const float v = float(val);
                        if (!std::isfinite(v))
                                throw std::runtime_error("J-Link 指令超出 float 范围");
                        std::memcpy(&raw, &v, 4);
                } else
                        std::memcpy(&raw, &val, 8);
        } else {
                const double limit = std::ldexp(1.0, s.size * 8 - (s.signedValue ? 1 : 0));
                if (val != std::trunc(val) || val < (s.signedValue ? -limit : 0) || val >= limit)
                        throw std::runtime_error("J-Link 整数指令超出变量范围");
                raw = uint64_t(int64_t(val));
        }
        if (!io_.write(epoch_, s.address, s.size, &raw))
                throw std::runtime_error(std::string("J-Link 写入失败或目标已重连：") + labels[field]);
}
void
Session::guard()
{
        if (!connected_)
                throw std::runtime_error("J-Link 电机控制未连接");
        if (read(ShmEnable) != 0 || read(CiaEnable) != 1 || read(AutoEnable) != 0 || read(ActiveConfigValid) != 1)
                throw std::runtime_error("J-Link 控制权变化：检查共享内存、CiA402、自动控制和生效配置");
        const auto direction = read(Direction);
        if (direction != 1 && direction != -1)
                throw std::runtime_error("FocDriver 方向配置无效");
}
double
Session::velocityScale()
{
        const auto unit = read(VelocityUnit);
        switch (int(unit)) {
                case 0:
                        return 1;
                case 1:
                        return 3.141592653589793 / 180;
                case 2:
                        return 3.141592653589793 / 30;
                case 3:
                        return 2 * 3.141592653589793;
                default:
                        throw std::runtime_error("FocDriver 速度单位配置无效");
        }
}
void
Session::waitFor(const std::function<bool()> &condition, const char *operation)
{
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        do {
                if (condition())
                        return;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error(std::string("J-Link 固件确认超时：") + operation);
}
void
Session::connect(const Config &config)
{
        validate(config);
        config_ = config;
        epoch_  = io_.epoch();
        if (!epoch_)
                throw std::runtime_error("请先在顶部连接 J-Link，再连接电机控制");
        if (read(CiaEnable) != 1 || read(AutoEnable) != 0 || read(ActiveConfigValid) != 1)
                throw std::runtime_error("请先启用 CiA402、关闭自动控制，并完成固件初始化");
        const auto status = uint32_t(read(Status));
        if ((status & 4) || read(Special) == AVA_ETH_SPECIAL_CALI)
                throw std::runtime_error("请先失能电机并退出校准，再接入 J-Link 控制");
        // Deliberately do not restore SHM on close: its old command could re-enable the motor
        write(ShmEnable, 0);
        connected_ = true;
        try {
                stop();
                guard();
                (void)feedback();
        } catch (...) {
                try {
                        stop();
                } catch (...) {
                }
                connected_ = false;
                throw;
        }
}
void
Session::stop()
{
        if (!connected_)
                return;
        write(Control, 0);
        write(Special, AVA_ETH_SPECIAL_DISABLE);
        waitFor([&] { return (uint32_t(read(Status)) & 4u) == 0; }, "失能");
        write(Special, 0);
}
void
Session::disconnect()
{
        if (!connected_)
                return;
        try {
                stop();
        } catch (...) {
                connected_ = false;
                throw;
        }
        connected_ = false;
}
AvaEthFeedback
Session::feedback()
{
        guard();
        // New commissioning firmware shuts down if control feedback polling stops.
        // Absent in older firmware: preserve compatibility without a write to address zero.
        if (config_.fields[DriveHeartbeat].address)
                write(DriveHeartbeat, uint32_t(uint32_t(read(DriveHeartbeat)) + 1u));
        AvaEthFeedback f{};
        f.timestamp_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto status = uint32_t(read(Status));
        f.flags           = AVA_ETH_ONLINE | AVA_ETH_FEEDBACK_VALID;
        if (status & 4)
                f.flags |= AVA_ETH_ENABLED;
        if (status & 8) {
                f.flags      |= AVA_ETH_FAULT;
                f.error_code  = AVA_ETH_ERR_DRIVE_FAULT;
        }
        f.mode                   = int32_t(read(ActualMode));
        f.special_cmd            = uint32_t(read(Special));
        const auto direction     = read(Direction);
        f.position               = float(read(Position) * direction);
        f.velocity               = float(read(Velocity) * direction * velocityScale());
        f.current                = float(read(Current) * direction);
        f.torque                 = float(read(LoadTorque) * direction);
        f.electromagnetic_torque = float(read(ElectromagneticTorque) * direction);
        f.vbus                   = float(read(BusVoltage));
        f.motor_temperature      = float(read(WindingTemperature));
        f.drive_temperature      = float(read(InverterTemperature));
        if (!std::isfinite(f.position) || !std::isfinite(f.velocity) || !std::isfinite(f.current))
                throw std::runtime_error("J-Link PVCT 反馈无效");
        if (std::isfinite(f.electromagnetic_torque))
                f.flags |= AVA_ETH_ELECTROMAGNETIC_TORQUE_VALID;
        if (std::isfinite(f.motor_temperature) && std::isfinite(f.drive_temperature))
                f.flags |= AVA_ETH_TEMPERATURE_VALID;
        return f;
}
AvaEthTarget
Session::target()
{
        guard();
        return {0,
                int32_t(read(ActualMode)),
                float(read(TargetPosition)),
                float(read(TargetVelocity) * velocityScale()),
                float(read(TargetCurrent)),
                float(read(TargetTorque))};
}
void
Session::writeTarget(const AvaEthTarget &t)
{
        if (t.axis || !supportedMode(t.mode) || !std::isfinite(t.position) || !std::isfinite(t.velocity) ||
            !std::isfinite(t.current) || !std::isfinite(t.torque))
                throw std::runtime_error("J-Link 目标参数无效");
        // Suppress target consumption while updating its separate scalar fields
        write(TargetValid, 0);
        write(TargetPosition, t.position);
        write(TargetVelocity, t.velocity / velocityScale());
        write(TargetCurrent, t.current);
        write(TargetTorque, t.torque);
        write(FeedVelocity, 0);
        write(FeedCurrent, 0);
        write(FeedTorque, 0);
        write(TargetMode, t.mode);
        write(PositionLatch, 0);
        write(TargetValid, 1);
}
AvaEthTarget
Session::setMode(AvaEthTarget t)
{
        guard();
        const auto f = feedback();
        if ((f.flags & AVA_ETH_FAULT) || f.special_cmd)
                throw std::runtime_error("切换模式前请清除故障及特殊指令");
        if (!supportedMode(t.mode))
                throw std::runtime_error("不支持的 CiA402 模式");
        // Prime position before changing mode; firmware latches fresh feedback on entry
        t.position = f.position;
        t.velocity = t.current = t.torque = 0;
        writeTarget(t);
        write(Mode, t.mode);
        waitFor(
            [&] {
                    guard();
                    return read(ActualMode) == t.mode;
            },
            "模式切换");
        if (positionMode(t.mode)) {
                // Use firmware's latch (when entering position from a non-position mode)
                if (read(PositionLatch))
                        t.position = float(read(TargetPosition));
                if (!std::isfinite(t.position))
                        throw std::runtime_error("固件位置锁存值无效");
        }
        // Do not replay the pre-switch GUI draft or send a PP new-setpoint edge here
        return t;
}
void
Session::apply(const AvaEthTarget &t)
{
        guard();
        const auto f = feedback();
        if (f.mode != t.mode || (f.flags & AVA_ETH_FAULT) || f.special_cmd)
                throw std::runtime_error("J-Link 目标模式未确认、存在故障或特殊指令仍生效");
        if (pp(t.mode) && (f.flags & AVA_ETH_ENABLED)) {
                write(Control, 15);
                waitFor([&] { return (uint32_t(read(Status)) & 0x1000u) == 0; }, "PP 清除目标确认");
        }
        writeTarget(t);
        if (pp(t.mode) && (f.flags & AVA_ETH_ENABLED)) {
                write(Control, 0x3f);
                waitFor([&] { return (uint32_t(read(Status)) & 0x1000u) != 0; }, "PP 接收新目标");
        }
}
void
Session::enable()
{
        guard();
        const auto f = feedback();
        if (!f.mode || (f.flags & AVA_ETH_FAULT) || f.special_cmd)
                throw std::runtime_error("使能前请选择模式、清除故障及特殊指令");
        for (const auto step : {std::pair{6, 0x21}, std::pair{7, 0x23}, std::pair{15, 0x27}}) {
                write(Control, step.first);
                waitFor(
                    [&] {
                            guard();
                            const auto status = uint32_t(read(Status));
                            if (status & 8)
                                    throw std::runtime_error("固件拒绝使能：驱动故障");
                            return (status & 0x6fu) == uint32_t(step.second);
                    },
                    "使能状态机");
        }
}
void
Session::special(uint32_t command)
{
        guard();
        if (command > 7)
                throw std::runtime_error("特殊指令无效");
        if (command == AVA_ETH_SPECIAL_DISABLE) {
                stop();
                return;
        }
        if (command == AVA_ETH_SPECIAL_ENABLE) {
                enable();
                return;
        }
        // NONE must not revive a stale enable control word after calibration/clear fault
        if (command == 0 || command == AVA_ETH_SPECIAL_CLEAR_FAULT)
                write(Control, 0);
        write(Special, command);
        waitFor([&] { return read(Special) == command; }, "特殊指令写入");
}
} // namespace motor_jlink
