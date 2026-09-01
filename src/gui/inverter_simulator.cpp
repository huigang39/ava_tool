#include "gui/inverter_simulator.hpp"
#include "cJSON.h"
#include "gui/choice_control.hpp"
#include "gui/dock_close.hpp"
#include "gui/i18n.hpp"
#include "gui/log_view.hpp"
#include "gui/ui_theme.hpp"
#include "imgui.h"
#include "implot.h"
#include "platform/native_dlg.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>

namespace
{
struct Parameter {
        const char *key, *en, *cn;
        double ava_spice::Config::*member;
        const char                *helpEn, *helpCn;
};
const Parameter parameters[] = {
    {"bus",
     "DC bus (V)",
     "直流母线 (V)",
     &ava_spice::Config::bus,
     "Supply voltage. Raising it raises off-state VDS and usually switching energy; it also increases load-current rise during "
     "each pulse.",
     "直流供电电压。增大后关断时的 VDS 基准升高，开关能量通常增大，脉冲期间负载电流上升也更快。主要观察 VDS、Iload "
     "和端口功率。"},
    {"current",
     "Initial current (A)",
     "初始负载电流 (A)",
     &ava_spice::Config::current,
     "Inductor current before the first pulse, not a constant-current command. Current evolves during the pulses. Higher "
     "current usually increases switching stress and energy.",
     "第一脉冲之前的电感初始电流，不是全程恒流目标。之后电流会随脉冲变化。增大后通常提高开关应力和开关能量，主要观察 "
     "ID、Iload、VDS 尖峰。"},
    {"gateOn",
     "Gate ON (V)",
     "栅极开通电压 (V)",
     &ava_spice::Config::gateOn,
     "Driver high level, not the actual gate voltage. Raising it can improve conduction and speed turn-on; the response "
     "depends on the model. Check the part's VGS rating.",
     "驱动源的高电平，不等于器件实际 VGS。增大后可能降低导通电阻、加快开通，具体取决于模型。观察 VGS 平台、ID 边沿和导通 "
     "VDS；按器件数据手册选择电压。"},
    {"gateOff",
     "Gate OFF (V)",
     "栅极关断电压 (V)",
     &ava_spice::Config::gateOff,
     "Driver low level for the DUT and the held-off high-side device. A more negative level can improve off-state margin but "
     "increases gate-voltage swing. Check the negative VGS rating.",
     "被测管关断电平，同时用于保持高侧管关断。更负的电压可增加关断裕量，但会增大栅极电压摆幅。观察 VGS "
     "负向尖峰；负压是否适用取决于 MOS/GaN 型号。"},
    {"rg",
     "Gate resistance (Ohm)",
     "栅极电阻 (Ohm)",
     &ava_spice::Config::rg,
     "External series gate resistor, added to model-internal resistance. Increasing it usually slows switching and damps "
     "ringing, at the cost of higher switching energy. Compare VGS, VDS and power.",
     "外接串联栅极电阻，与模型内部电阻叠加。增大通常使开关变慢、振铃减弱，但开关能量可能增大；减小则边沿更快，尖峰和振铃可能更"
     "明显。观察 VGS、VDS 和端口功率。"},
    {"loopNh",
     "Commutation loop (nH)",
     "换流回路电感 (nH)",
     &ava_spice::Config::loopNh,
     "External power-loop parasitic inductance. Increasing it usually increases VDS overshoot and changes ringing frequency. "
     "Model-internal package parasitics remain included.",
     "功率换流路径上的外部寄生电感。增大通常使 VDS 过冲更明显，并改变振铃频率。它会与厂商模型自带的封装寄生参数叠加，主要观察 "
     "VDS 尖峰和振铃。"},
    {"sourceNh",
     "Common source L (nH)",
     "共源寄生电感 (nH)",
     &ava_spice::Config::sourceNh,
     "External source return inductance. Its voltage follows di/dt and changes effective VGS, switching speed and source "
     "bounce. Compare VGS and ID edges.",
     "源极回流路径上的外部寄生电感。电流快速变化时产生源极电压跳变，影响有效 "
     "VGS、开关速度和振铃。增大后这些影响通常更明显，主要观察 VGS 和 ID 边沿。"},
    {"gateNh",
     "Gate loop L (nH)",
     "栅极回路电感 (nH)",
     &ava_spice::Config::gateNh,
     "External gate-path inductance. It resonates with gate capacitances; increasing it changes ringing frequency and may "
     "increase gate overshoot. Gate resistance provides damping.",
     "驱动到栅极路径上的外部寄生电感，与栅极电容形成谐振。增大会改变振铃频率，并可能增大 VGS "
     "过冲；栅极电阻会影响阻尼。主要观察 VGS 与 Vdriver 的差异。"},
    {"loopR",
     "Loop resistance (Ohm)",
     "回路寄生电阻 (Ohm)",
     &ava_spice::Config::loopR,
     "External power-loop series resistance. Increasing it adds voltage drop and resistive dissipation and generally damps "
     "ringing. This external resistor's loss is not included in device terminal energy.",
     "功率回路外部串联电阻。增大会增加压降和电阻耗能，通常也会抑制振铃。观察 VDS、Iload；该外部电阻的耗能不计入器件端口能量。"},
    {"loadUh",
     "Load inductance (uH)",
     "负载电感 (uH)",
     &ava_spice::Config::loadUh,
     "Test load inductance. For the same voltage, a larger value reduces current change during a pulse (di/dt approximately "
     "V/L). It does not change the configured initial current.",
     "双脉冲测试的负载电感。同样电压下，增大后脉冲期间电流变化更慢（di/dt 约为 "
     "V/L），更接近恒流负载。不会改变设置的初始电流，主要观察 Iload 斜率。"},
    {"pulse1Us",
     "First pulse (us)",
     "第一脉冲 (us)",
     &ava_spice::Config::pulse1Us,
     "Duration of the first ON pulse, beginning at 0.5 us. Increasing it usually raises current before the first turn-off and "
     "shifts all later switching events.",
     "第一段开通脉冲时长，固定从 0.5 us 开始。延长通常使第一次关断前电流更大，并将后续关断、第二次开通整体向后移动。观察 Iload "
     "和第一次关断的 VDS。"},
    {"gapUs",
     "Pulse gap (us)",
     "脉冲间隔 (us)",
     &ava_spice::Config::gapUs,
     "OFF interval between pulses. Increasing it allows longer freewheeling and ringing decay and delays the second turn-on. "
     "It is not a complementary PWM dead-time setting.",
     "两次开通之间的关断时间。延长会增加续流时间，让振铃有更多时间衰减，并推迟第二次开通；它不是互补 PWM 的死区设置。观察 "
     "Ifreewheel 和第二次开通瞬态。"},
    {"pulse2Us",
     "Second pulse (us)",
     "第二脉冲 (us)",
     &ava_spice::Config::pulse2Us,
     "Duration of the second ON pulse. Increasing it prolongs conduction and current rise and delays the final turn-off; it "
     "does not move the second turn-on.",
     "第二段开通脉冲时长。延长会增加导通时间和电流上升量，并推迟最后一次关断，不改变第二次开通时刻。主要观察第二脉冲的 "
     "ID、Iload 和累计端口能量。"},
    {"edgeNs",
     "Driver edge (ns)",
     "驱动边沿 (ns)",
     &ava_spice::Config::edgeNs,
     "Rise and fall time of the ideal driver source, not MOSFET switching time. Smaller values drive faster edges; actual "
     "VGS/VDS also depend on Rg, parasitics and the model.",
     "理想驱动电压源的上升和下降时间，不是 MOS 的实际开关时间。减小会使驱动边沿更陡；实际 VGS/VDS "
     "还受栅极电阻、寄生参数和器件模型影响。"},
    {"stepNs",
     "Maximum step (ns)",
     "最大时间步长 (ns)",
     &ava_spice::Config::stepNs,
     "Upper limit on the solver's adaptive time step. Reducing it resolves fast spikes and ringing better but increases "
     "runtime and data size. It is a numerical setting, not a circuit component.",
     "ngspice "
     "自适应求解时间步长的上限。减小可更细致地解析尖峰和振铃，但计算更慢、数据更多。这是数值精度设置，不改变电路参数；可减半后"
     "比较峰值与能量是否收敛。"},
    {"temperature",
     "Temperature (C)",
     "温度 (C)",
     &ava_spice::Config::temperature,
     "Fixed model temperature. Temperature-dependent resistance, threshold and switching behavior follow the selected model. "
     "This simulation does not calculate self-heating or temperature rise.",
     "模型固定温度。导通电阻、阈值及开关特性如何变化取决于所选模型是否实现温度依赖。当前仿真不计算自发热或随时间变化的温升。"},
    {"pwmHz",
     "PWM frequency (Hz)",
     "PWM 频率 (Hz)",
     &ava_spice::Config::pwmHz,
     "Center-aligned sinusoidal PWM carrier frequency. Higher frequency reduces current ripple but adds switching events and "
     "solver work.",
     "中心对齐正弦 PWM 的载波频率。周期数固定时，频率越高仿真时长越短，绕组电流纹波通常也更小。"},
    {"pwmCycles",
     "PWM cycles",
     "PWM 周期数",
     &ava_spice::Config::pwmCycles,
     "Number of complete PWM carrier cycles. PWM duration = cycles / frequency; the run includes an additional 1 us "
     "initialization interval.",
     "完整 PWM 载波周期的数量。PWM 仿真时长 = 周期数 / 频率，另外包含 1 us "
     "初始化时间。增加周期数可观察更长的响应，但会增加求解时间和数据量。"},
    {"modulation",
     "Modulation index",
     "调制比",
     &ava_spice::Config::modulation,
     "Sinusoidal PWM amplitude, 0 to 0.98. Changes average winding voltage and current. This is open-loop drive, not FOC.",
     "正弦 PWM 幅值，范围 0～0.98。改变平均绕组电压和电流；当前为开环驱动，不包含 FOC 电流闭环。"},
    {"deadNs",
     "Dead time (ns)",
     "死区时间 (ns)",
     &ava_spice::Config::deadNs,
     "Delay before turning on the complementary MOS after the other driver finishes turning off. Actual gate ringing can still "
     "cause cross conduction.",
     "一侧驱动关断完成后，另一侧开通之前的等待时间。增大可增加关断裕量，但改变续流和平均电压；实际 VGS "
     "振铃仍可能导致交叉导通。"},
    {"windingR",
     "Phase resistance (Ohm)",
     "每相绕组电阻 (Ohm)",
     &ava_spice::Config::windingR,
     "Resistance of each identical phase winding, not line-to-line resistance. Higher R increases damping and voltage drop.",
     "三相相同绕组的每相电阻，不是线间电阻。增大后压降和阻尼更大，电流响应也会变化。"},
    {"windingUh",
     "Phase inductance (uH)",
     "每相绕组电感 (uH)",
     &ava_spice::Config::windingUh,
     "Per-phase uncoupled inductance. Higher L reduces current slew and ripple. Mutual inductance, saturation and saliency are "
     "not modeled.",
     "每相独立电感。增大后电流变化更慢、纹波通常更小；此 R-L 模型未包含相间互感、磁饱和和凸极效应。"},
    {"rpm",
     "Fixed speed (rpm)",
     "固定转速 (rpm)",
     &ava_spice::Config::rpm,
     "Imposed mechanical speed, not a solved mechanical state. Electrical frequency = rpm * pole pairs / 60; negative speed "
     "reverses phase rotation.",
     "外部指定的固定机械转速，不是动力学求解结果。电频率 = 转速 × 极对数 / 60；负转速反转相序。不会自动产生加速或负载响应。"},
    {"polePairs",
     "Pole pairs",
     "极对数",
     &ava_spice::Config::polePairs,
     "Positive integer converting mechanical speed to electrical frequency. More pole pairs give more electrical cycles at the "
     "same rpm.",
     "将机械转速换算为电频率的正整数。同样转速下，极对数越大，电周期越多。"},
    {"emfPeak",
     "Phase EMF peak (V)",
     "每相反电动势峰值 (V)",
     &ava_spice::Config::emfPeak,
     "Explicit phase-to-neutral sinusoidal EMF peak at the selected speed, not RMS or line voltage. It is independent of rpm; "
     "set 0 to test a pure R-L load.",
     "指定转速下每相对中性点的正弦反电动势峰值，不是有效值或线电压。幅值在指定转速下单独设置；零转速时反电动势为零，负转速反转"
     "极性。幅值设为 0 即纯 R-L 负载。"},
    {"phaseLeadDeg",
     "Voltage lead (electrical deg)",
     "电压超前角 (电角度)",
     &ava_spice::Config::phaseLeadDeg,
     "PWM sinusoidal voltage phase relative to EMF. Changes current phase, magnitude and power flow. There is no current "
     "controller.",
     "PWM 正弦电压相对反电动势的电角度。会改变电流相位、大小及能量流向；当前没有电流控制器。"},
};
void
help(const char *en, const char *cn)
{
        if (!ImGui::IsItemHovered())
                return;
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(tr(en, cn));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
}
bool
input(const char *label, std::string &val)
{
        char buffer[2048]{};
        std::snprintf(buffer, sizeof(buffer), "%s", val.c_str());
        if (ImGui::InputText(label, buffer, sizeof(buffer))) {
                val = buffer;
                return true;
        }
        return false;
}
} // namespace
InverterSimulator::InverterSimulator()
{
        draft_.topology = 1;
        draft_.stepNs   = 50;
        draft_.library  = ava_spice::defaultLibrary();
        models_         = ava_spice::models(draft_.library);
}
void
InverterSimulator::start()
{
        try {
                pending_ = draft_;
                if (pending_.executable.empty())
                        pending_.executable = ava_spice::defaultExecutable();
                runner_.start(pending_);
                error_.clear();
        } catch (const std::exception &e) {
                error_ = e.what();
        }
}
void
InverterSimulator::acceptResult(ava_spice::Result result)
{
        error_ = result.error;
        if (error_.empty() && (!result.samples.empty() || !result.times.empty())) {
                previous_       = std::move(result_);
                previousConfig_ = applied_;
                applied_        = pending_;
                result_         = std::move(result);
                lastAttempt_    = {};
                syncScopes();
                viewMode_ = MonitorViewMode::FULL;
                minUs_    = 0;
                maxUs_    = applied_.topology == 1 ? result_.times.back() * 1e6 : result_.samples.back().time * 1e6;
                if (compare_ && previousConfig_.topology == applied_.topology) {
                        if (!previous_.samples.empty())
                                maxUs_ = std::max(maxUs_, previous_.samples.back().time * 1e6);
                        if (!previous_.times.empty())
                                maxUs_ = std::max(maxUs_, previous_.times.back() * 1e6);
                }
                for (auto &scope : scopes_)
                        if (scope)
                                scope->resetYFit();
        } else {
                lastAttempt_ = std::move(result);
        }
}
void
InverterSimulator::draw()
{
        ava_spice::Result completed;
        if (runner_.poll(completed))
                acceptResult(std::move(completed));
        if (!show_)
                return;
        if (!dock_close::begin(tr("Inverter Simulation###InverterSimulation", "逆变器仿真###InverterSimulation"), &show_)) {
                ImGui::End();
                return;
        }
        if (ImGui::BeginTable("##inverter_layout",
                              2,
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn(tr("Waveforms / comparison", "波形 / 对比"),
                                        ImGuiTableColumnFlags_WidthStretch,
                                        draft_.topology == 1 ? .5f : .65f);
                ImGui::TableSetupColumn(
                    tr("Parameters", "参数"), ImGuiTableColumnFlags_WidthStretch, draft_.topology == 1 ? .5f : .35f);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::BeginChild("##inverter_waveforms", ImVec2(0, 0));
                drawWaveforms();
                ImGui::EndChild();
                ImGui::TableNextColumn();
                ImGui::BeginChild("##inverter_parameters", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
                drawParameters();
                ImGui::EndChild();
                ImGui::EndTable();
        }
        ImGui::End();
}
void
InverterSimulator::drawParameters()
{
        ImGui::BeginDisabled(runner_.busy());
        if (ImGui::Button(tr("Run ngspice", "运行 ngspice")))
                start();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel", "取消")))
                runner_.cancel();
        if (runner_.busy())
                ImGui::TextWrapped("%s",
                                   tr("ngspice is solving... displayed waveforms are the last successful result.",
                                      "ngspice 正在求解... 当前保留显示上一次成功运行的波形。"));
        if (!error_.empty())
                ui::ErrorText("%s", error_.c_str());
        ImGui::TextUnformatted(tr("Circuit", "电路"));
        ImGui::SetNextItemWidth(-1);
        if (ui::Choice("##circuit",
                       &draft_.topology,
                       tr("Double-pulse test\0Three-phase inverter + windings\0", "双脉冲测试\0三相逆变器 + 电机绕组\0"))) {
                modified_     = true;
                draft_.stepNs = draft_.topology == 1 ? 50 : .5;
        }
        if (draft_.topology == 1 &&
            ImGui::CollapsingHeader(tr("Circuit / click to probe", "电路 / 点击放置探针"), ImGuiTreeNodeFlags_DefaultOpen))
                schematic();
        ImGui::Separator();
        ImGui::TextWrapped("%s",
                           draft_.topology == 1
                               ? tr("ngspice | 3-phase star R-L-EMF", "ngspice | 三相星形 R-L-反电动势")
                               : tr("ngspice | MOSFET / GaN double-pulse test", "ngspice | MOSFET / GaN 双脉冲测试"));
        ImGui::TextUnformatted(tr("Device model", "器件模型"));
        ImGui::SetNextItemWidth(-1);
        if (ui::Choice("##device_model",
                       &draft_.device,
                       tr("Generic MOS demo (not a real part)\0Manufacturer MOSFET model\0Manufacturer GaN model\0",
                          "通用 MOS 演示（不是具体器件）\0厂商 MOSFET 模型\0厂商 GaN 模型\0")))
                modified_ = true;
        help("Select the device physics model. Generic MOS is only a demo; MOSFET and GaN require the matching manufacturer "
             "model.",
             "选择器件物理模型。通用 MOS 仅用于演示；MOSFET 和 GaN 需使用对应厂商模型，选择类别不会自动把 MOS 模型变成 GaN。");
        auto exe = draft_.executable.empty() ? ava_spice::defaultExecutable() : draft_.executable;
        ImGui::TextWrapped("ngspice: %s", exe.empty() ? tr("Not found", "未找到") : exe.c_str());
        if (ImGui::Button(tr("Select ngspice...", "选择 ngspice..."))) {
                auto path = nativeDlgOpen("ngspice", {{"Executable", {"exe"}}});
                if (!path.empty()) {
                        draft_.executable = path;
                        modified_         = true;
                }
        }
        if (draft_.device) {
                if (ImGui::Button(tr("Import SPICE model...", "导入 SPICE 模型..."))) {
                        auto path =
                            nativeDlgOpen(tr("Device library", "器件模型库"), {{"SPICE", {"lib", "cir", "sub", "mod", "txt"}}});
                        if (!path.empty()) {
                                draft_.library = path;
                                models_        = ava_spice::models(path);
                                modified_      = true;
                        }
                }
                ImGui::TextWrapped("%s: %s", tr("Library", "模型文件"), draft_.library.c_str());
                ImGui::TextUnformatted(tr("Subcircuit", "子电路"));
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##subcircuit", draft_.model.c_str())) {
                        for (const auto &m : models_)
                                if (ImGui::Selectable(m.name.c_str(), m.name == draft_.model)) {
                                        draft_.model = m.name;
                                        modified_    = true;
                                }
                        ImGui::EndCombo();
                }
                help("Choose the manufacturer's subcircuit. Model level determines electrical details, pin order and thermal "
                     "support.",
                     "选择厂商提供的子电路。模型等级会影响电气细节、引脚顺序和热模型支持；需使用与你的器件一致的型号。");
                ImGui::TextUnformatted(tr("Model name", "模型名称"));
                ImGui::SetNextItemWidth(-1);
                if (input("##model_name", draft_.model))
                        modified_ = true;
                help("Select a .SUBCKT from the imported library. Different model levels may have different pins and thermal "
                     "support; check the declared pin order.",
                     "选择导入库中的 .SUBCKT。不同模型等级可能有不同引脚和热模型支持，需核对下方声明的引脚顺序。");
                for (const auto &m : models_)
                        if (m.name == draft_.model)
                                ImGui::TextWrapped(".SUBCKT %s %s", m.name.c_str(), m.pins.c_str());
                if (ImGui::Checkbox(tr("One integrated half-bridge package", "单个集成半桥封装"), &draft_.package)) {
                        modified_   = true;
                        draft_.pins = draft_.package ? "BUS HG SW G S" : "D G S";
                }
                help("Enable for a model containing both high-side and low-side devices in one package. Disable for a "
                     "single-device model, instantiated twice. Recheck pin mapping after changing this.",
                     "模型包含同一封装内的高侧和低侧两只管时启用；单管模型关闭后会实例化两只管。ISG0616N10NM5HSC "
                     "属于集成半桥，切换后需重新核对引脚映射。");
                ImGui::TextUnformatted(tr("Pin mapping (model order)", "引脚映射（模型顺序）"));
                ImGui::SetNextItemWidth(-1);
                if (input("##pin_mapping", draft_.pins))
                        modified_ = true;
                help("Assign one circuit role per model pin, in .SUBCKT order. Wrong mapping can cause solver errors or "
                     "misleading waveforms.",
                     "严格按 .SUBCKT "
                     "引脚顺序，逐个填写对应的电路角色。映射错误会导致求解报错或波形不符合实际；集成半桥与单管的映射不同。");
                ImGui::TextWrapped(
                    "%s",
                    draft_.package ? tr("Package roles: BUS, HG, SW, G (low gate), S (low source), K (ground). Map every model "
                                        "pin explicitly.",
                                        "封装映射：BUS 母线、HG 高侧栅极、SW 开关节点、G 低侧栅极、S 低侧源极、K "
                                        "地。按模型顺序填写每个引脚。")
                                   : tr("Single-device roles: D, G, S, K (Kelvin source). Same model is used for DUT and the "
                                        "off-state freewheel device.",
                                        "单管映射：D 漏极、G 栅极、S 源极、K 开尔文源极。被测管与关断续流管使用同一模型。"));
        }
        if (ImGui::BeginTable("##spice_parameters", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.65f);
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                for (const auto &p : parameters) {
                        const bool        motor = &p >= &parameters[16];
                        const std::string key   = p.key;
                        const bool        pulse =
                            key == "current" || key == "loadUh" || key == "pulse1Us" || key == "gapUs" || key == "pulse2Us";
                        if ((draft_.topology == 1 && pulse) || (draft_.topology == 0 && motor))
                                continue;
                        ImGui::PushID(p.key);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextWrapped("%s", tr(p.en, p.cn));
                        help(p.helpEn, p.helpCn);
                        ImGui::TableNextColumn();
                        ImGui::SetNextItemWidth(-1);
                        if (p.member == &ava_spice::Config::pwmCycles) {
                                int cycles = std::isfinite(draft_.pwmCycles)
                                                 ? static_cast<int>(std::clamp(draft_.pwmCycles, 1.0, 10000.0))
                                                 : 2;
                                if (ImGui::InputInt("##value", &cycles)) {
                                        draft_.pwmCycles = std::clamp(cycles, 1, 10000);
                                        modified_        = true;
                                }
                        } else if (ImGui::InputDouble("##value", &(draft_.*p.member), 0, 0, "%.3f"))
                                modified_ = true;
                        help(p.helpEn, p.helpCn);
                        ImGui::PopID();
                }
                ImGui::EndTable();
        }
        if (draft_.topology == 1 && std::isfinite(draft_.pwmHz) && draft_.pwmHz > 0 && std::isfinite(draft_.pwmCycles))
                ImGui::TextWrapped(tr("PWM period %.3f us | total %.3f us (including 1 us initialization)",
                                      "PWM 周期 %.3f us | 总仿真时长 %.3f us（含 1 us 初始化）"),
                                   1e6 / draft_.pwmHz,
                                   ava_spice::threePhaseEnd(draft_) * 1e6);
        ImGui::TextWrapped("%s",
                           tr("Hover a parameter for its effect. Changes take effect on the next run.",
                              "鼠标悬停参数可查看影响；修改后需重新运行才会更新波形。"));
        ImGui::Checkbox(tr("Solver log / netlist directory", "求解日志 / 网表目录"), &showLog_);
        if (showLog_) {
                const auto &logResult = lastAttempt_.directory.empty() ? result_ : lastAttempt_;
                std::string log       = logResult.directory + "\n" + logResult.log;
                ui::CopyLogButton(log);
                ui::LogView("##log", log, ImVec2(0, 180));
        }
}
void
InverterSimulator::drawWaveforms()
{
        ImGui::BeginDisabled(applied_.topology == 1);
        if (ImGui::Button(tr("Second turn-on", "第二次开通"))) {
                viewMode_ = MonitorViewMode::MANUAL;
                auto edge = .5 + applied_.pulse1Us + applied_.gapUs;
                minUs_    = edge - .15;
                maxUs_    = edge + .65;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ui::Button(tr("Export CSV", "导出 CSV"), ui::BtnStyle::Success) &&
            (!result_.samples.empty() || !result_.times.empty()))
                exportCsv();
        if (!result_.times.empty()) {
                ImGui::TextWrapped("%s: %s | VDS max %.3f V | Iphase max %.3f A | DC energy %.3f mJ | %zu pts",
                                   tr("Result", "结果"),
                                   applied_.device == 0 ? "Generic MOS" : applied_.model.c_str(),
                                   result_.peakV,
                                   result_.peakCurrent,
                                   result_.dcEnergy * 1e3,
                                   result_.times.size());
                ImGui::TextWrapped(
                    "%s",
                    tr("Fixed-speed R-L-EMF load; DC energy includes winding/field energy exchange, not just device loss.",
                       "固定转速 R-L-反电动势负载；直流端能量包含绕组和电源间的能量交换，不等于器件损耗。"));
                comparison();
                if (ImGui::Button(tr("Clear probes", "清除探针"))) {
                        probes_.clear();
                        syncScopes();
                }
                std::string probeNames;
                for (const auto &key : probes_) {
                        if (!probeNames.empty())
                                probeNames += ", ";
                        probeNames += key;
                }
                ImGui::TextWrapped("%s: %s", tr("Selected probes", "已选探针"), probeNames.c_str());
                plot(tr("Voltage probes", "电压探针"), 0);
                plot(tr("Current probes", "电流探针"), 1);
                return;
        }
        if (result_.samples.empty()) {
                ImGui::TextWrapped("%s",
                                   tr("Run ngspice, then click circuit nodes or components to select voltage/current traces.",
                                      "运行 ngspice 后，点击电路节点或元件，选择要查看的电压、电流波形。"));
                return;
        }
        if (!result_.samples.empty()) {
                ImGui::Text("%s: %s",
                            tr("Result model", "结果模型"),
                            applied_.device == 0 ? tr("Generic MOS demo", "通用 MOS 演示") : applied_.model.c_str());
                ImGui::TextWrapped("Vds peak %.3f V | Eon(0.5us) %.3f uJ | Eoff(0.5us) %.3f uJ | %zu pts",
                                   result_.peakV,
                                   result_.eon * 1e6,
                                   result_.eoff * 1e6,
                                   result_.samples.size());
                ImGui::TextWrapped("%s",
                                   tr("Energies integrate signed device terminal power for 0.5 us after each edge; stored "
                                      "capacitive energy is included. They are not a thermal solve.",
                                      "能量积分为边沿后0.5 us内的有符号器件端口功率，包含电容储能交换，不是热模型计算。"));
        }
        comparison();
        plot("VGS / Vdriver", 0);
        plot("VDS", 1);
        plot("ID / Iload / Ifreewheel", 2);
        plot(tr("Device terminal power", "器件端口功率"), 3);
        plot(tr("Integrated terminal energy", "累计端口能量"), 4);
}
void
InverterSimulator::comparison()
{
        if (previous_.samples.empty() && previous_.times.empty()) {
                ImGui::TextDisabled(
                    "%s",
                    tr("Comparison becomes available after two successful runs.", "成功运行两次后，可对比本次与上一次波形。"));
                return;
        }
        if (previousConfig_.topology != applied_.topology) {
                ImGui::TextDisabled(
                    "%s",
                    tr("Previous run uses a different circuit topology; no overlay.", "上一次电路类型不同，不叠加对比。"));
                return;
        }
        if (ImGui::Checkbox(tr("Overlay previous run", "叠加上一次波形"), &compare_))
                syncScopes();
        help("Previous successful run is shown with thinner, faded lines. Both runs use absolute simulation time; changing "
             "pulse timing shifts the edges. Failed or cancelled runs do not replace results.",
             "上一次成功运行使用较细、较淡的曲线。两次按实际仿真时间叠加，改变脉冲时长会移动开关边沿。失败或取消不会替换成功结"
             "果。对比仅保留本次会话的上一次成功运行。");
        if (!ImGui::CollapsingHeader(tr("Run comparison", "运行对比分析")))
                return;
        if (ImGui::BeginTable("##run_comparison", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                for (auto label : {tr("Metric", "指标"),
                                   tr("Previous", "上一次"),
                                   tr("Current", "本次"),
                                   tr("Change", "变化量"),
                                   tr("Change (%)", "变化 (%)")})
                        ImGui::TableSetupColumn(label);
                ImGui::TableHeadersRow();
                auto row = [&](const char *label, double before, double after) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(label);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.4g", before);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.4g", after);
                        ImGui::TableNextColumn();
                        ImGui::Text("%+.4g", after - before);
                        ImGui::TableNextColumn();
                        if (std::abs(before) > 1e-12)
                                ImGui::Text("%+.2f%%", 100 * (after - before) / std::abs(before));
                        else
                                ImGui::TextUnformatted("--");
                };
                row(tr("VDS peak (V)", "VDS 峰值 (V)"), previous_.peakV, result_.peakV);
                if (applied_.topology == 0) {
                        row(tr("Eon, terminal (uJ)", "开通端口能量 (uJ)"), previous_.eon * 1e6, result_.eon * 1e6);
                        row(tr("Eoff, terminal (uJ)", "关断端口能量 (uJ)"), previous_.eoff * 1e6, result_.eoff * 1e6);
                } else {
                        row(tr("Phase current peak (A)", "相电流峰值 (A)"), previous_.peakCurrent, result_.peakCurrent);
                        row(tr("DC energy (mJ)", "直流端能量 (mJ)"), previous_.dcEnergy * 1e3, result_.dcEnergy * 1e3);
                }
                for (const auto &p : parameters)
                        if (previousConfig_.*p.member != applied_.*p.member)
                                row(tr(p.en, p.cn), previousConfig_.*p.member, applied_.*p.member);
                ImGui::EndTable();
        }
        ImGui::TextWrapped("%s: %s -> %s",
                           tr("Model", "模型"),
                           previousConfig_.device == 0 ? "Generic MOS" : previousConfig_.model.c_str(),
                           applied_.device == 0 ? "Generic MOS" : applied_.model.c_str());
        if (previousConfig_.library != applied_.library || previousConfig_.pins != applied_.pins ||
            previousConfig_.package != applied_.package || previousConfig_.device != applied_.device)
                ImGui::TextWrapped("%s",
                                   tr("Model source, device type or pin configuration changed between runs.",
                                      "两次运行的模型来源、器件类别或引脚配置发生变化。"));
        ImGui::TextDisabled("%s",
                            tr("Energy deltas compare signed terminal energy, not calculated heat loss.",
                               "能量变化比较的是有符号端口能量，不代表计算出的发热损耗。"));
}
void
InverterSimulator::toggleProbe(const std::string &key)
{
        probePreset_ = 0;
        auto it      = std::find(probes_.begin(), probes_.end(), key);
        if (it == probes_.end())
                probes_.push_back(key);
        else
                probes_.erase(it);
        modified_ = true;
        if (!result_.times.empty())
                syncScopes();
}
void
InverterSimulator::applyProbePreset(int preset)
{
        static const std::vector<std::string> choices[] = {{},
                                                           {"vds_la", "vgs_la", "id_la"},
                                                           {"vds_ha", "vgs_ha", "id_ha"},
                                                           {"vds_la", "vgs_la", "vds_lb", "vgs_lb", "vds_lc", "vgs_lc"},
                                                           {"i_a", "i_b", "i_c"},
                                                           {"winding_a", "winding_b", "winding_c", "i_a", "i_b", "i_c"},
                                                           {"bus", "ibus", "vab", "vbc", "vca"}};
        if (preset <= 0 || preset >= static_cast<int>(std::size(choices)))
                return;
        probePreset_ = preset;
        probes_      = choices[preset];
        modified_    = true;
        if (!result_.times.empty())
                syncScopes();
}
void
InverterSimulator::syncProbeScopes()
{
        for (int kind = 0; kind < 2; ++kind) {
                if (!scopes_[kind])
                        scopes_[kind] = std::make_unique<MonitorScope>(kind == 0 ? "Voltage probes" : "Current probes");
                auto &scope = *scopes_[kind];
                scope.getChannels().clear();
                int colorIndex = 0;
                for (const auto &key : probes_) {
                        auto it = std::find_if(
                            result_.traces.begin(), result_.traces.end(), [&](const auto &trace) { return trace.key == key; });
                        if (it == result_.traces.end() || (it->unit == "A" ? 1 : 0) != kind)
                                continue;
                        auto color = ImPlot::GetColormapColor(colorIndex++);
                        auto load  = [&](const ava_spice::Result &run, bool previous) {
                                auto trace = std::find_if(
                                    run.traces.begin(), run.traces.end(), [&](const auto &t) { return t.key == key; });
                                if (trace == run.traces.end())
                                        return;
                                std::string name = (previous ? "previous_" : "current_") + key;
                                scope.addChannel(name);
                                auto *ch = scope.findChannel(name);
                                ch->setType("f32");
                                ch->setDevice("SPICE");
                                ch->setWritable(false);
                                ch->historySeconds_ = 0;
                                ch->setLabel(trace->label +
                                             (previous ? tr(" (previous)", " (上一次)") : tr(" (current)", " (本次)")));
                                auto c = color;
                                c.w    = previous ? .45f : 1;
                                std::copy_n(&c.x, 4, ch->getColor());
                                ch->useAutoColor()  = false;
                                ch->getLineWeight() = previous ? 1 : 2;
                                for (size_t i = 0; i < run.times.size(); ++i)
                                        ch->setRVal(static_cast<float>(trace->values[i]), run.times[i] * 1e6);
                                ch->publishSnapshot();
                        };
                        load(result_, false);
                        if (compare_ && previousConfig_.topology == 1)
                                load(previous_, true);
                }
                scope.resetYFit();
        }
}
void
InverterSimulator::schematic()
{
        ImGui::TextWrapped("%s",
                           tr("Click a dot/wire for voltage to ground. Click a MOS or circular motor winding to choose "
                              "terminal voltage/current. "
                              "Click selected probes again to remove them.",
                              "点击节点/导线看对地电压；点击 MOS 或圆形电机绕组选择端电压、电流。再次点击已选探针可移除。"));
        ImGui::TextWrapped("%s",
                           tr("MOS current: drain to source. Winding current: bridge to neutral. DC current: supply to bridge.",
                              "电流正向：MOS 漏极→源极，绕组 桥臂→中性点，母线 电源→逆变器。"));
        ImGui::TextUnformatted(tr("Probe preset", "预设探针"));
        ImGui::SetNextItemWidth(-1);
        int preset = probePreset_;
        if (ui::Choice("##probe_preset",
                       &preset,
                       tr("Custom\0QAL: VDS / VGS / ID\0QAH: VDS / VGS / ID\0Three low-side MOS: VDS / VGS\0Phase currents: "
                          "IA / IB / IC\0Motor winding voltages / currents\0DC bus / line voltages\0",
                          "自定义\0QAL：A 相低侧 VDS / VGS / ID\0QAH：A 相高侧 VDS / VGS / ID\0三相低侧 MOS：VDS / "
                          "VGS\0三相电流：IA / IB / IC\0电机绕组：三相电压 / 电流\0直流母线 / 线电压\0"))) {
                if (preset == 0)
                        probePreset_ = 0;
                else
                        applyProbePreset(preset);
        }
        help("A preset replaces selected probes. Click wires or windings to add or remove individual probes without rerunning "
             "SPICE.",
             "预设会替换当前探针组合。之后仍可点击导线或绕组增删单个探针，不需要重新求解。");
        const auto  origin = ImGui::GetCursorScreenPos();
        const float width  = std::max(600.0f, ImGui::GetContentRegionAvail().x);
        ImGui::InvisibleButton("##three_phase_circuit", ImVec2(width, 420));
        const bool  active = ImGui::IsItemHovered();
        auto       *draw   = ImGui::GetWindowDrawList();
        const ImU32 wire = IM_COL32(155, 180, 210, 255), selected = IM_COL32(65, 220, 190, 255),
                    hover = IM_COL32(255, 205, 70, 255);
        auto        point = [&](float x, float y) { return ImVec2(origin.x + x, origin.y + y); };
        std::string clickNode, clickPart;
        auto        line = [&](float x1, float y1, float x2, float y2, const std::string &key = std::string{}) {
                bool over = false;
                if (active && !key.empty()) {
                        auto        mouse = ImGui::GetIO().MousePos;
                        const float dx = x2 - x1, dy = y2 - y1, length = dx * dx + dy * dy;
                        const float t =
                            length > 0 ? std::clamp(((mouse.x - origin.x - x1) * dx + (mouse.y - origin.y - y1) * dy) / length,
                                                    0.0f,
                                                    1.0f)
                                              : 0;
                        const float rx = mouse.x - origin.x - x1 - t * dx, ry = mouse.y - origin.y - y1 - t * dy;
                        over = rx * rx + ry * ry < 25;
                }
                bool chosen = std::find(probes_.begin(), probes_.end(), key) != probes_.end();
                draw->AddLine(point(x1, y1), point(x2, y2), over ? hover : chosen ? selected : wire, 1.5f);
                if (over) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        ImGui::SetTooltip("%s: %s", tr("Voltage probe", "电压探针"), key.c_str());
                        if (ImGui::IsMouseClicked(0))
                                clickNode = key;
                }
        };
        auto text = [&](float x, float y, const std::string &label) {
                draw->AddText(point(x, y), IM_COL32(220, 225, 235, 255), label.c_str());
        };
        auto node = [&](float x, float y, const std::string &key, const std::string &label) {
                bool over   = active && ImGui::IsMouseHoveringRect(point(x - 10, y - 10), point(x + 10, y + 10));
                bool chosen = std::find(probes_.begin(), probes_.end(), key) != probes_.end();
                draw->AddCircleFilled(point(x, y), 4, over ? hover : chosen ? selected : wire);
                if (!label.empty())
                        text(x + 6, y - 8, label);
                if (over) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        ImGui::SetTooltip("%s: %s", tr("Voltage probe", "电压探针"), key.c_str());
                        if (ImGui::IsMouseClicked(0))
                                clickNode = key;
                }
        };
        auto part = [&](float x, float y, const std::string &id, const std::string &label) {
                bool       over   = active && ImGui::IsMouseHoveringRect(point(x - 19, y - 13), point(x + 19, y + 13));
                const bool probed = std::any_of(probes_.begin(), probes_.end(), [&](const auto &key) {
                        return key == "vds_" + id || key == "vgs_" + id || key == "id_" + id ||
                               (id == "bus" && (key == "bus" || key == "ibus"));
                });
                draw->AddRectFilled(point(x - 19, y - 13), point(x + 19, y + 13), IM_COL32(32, 48, 68, 255), 3);
                draw->AddRect(point(x - 19, y - 13), point(x + 19, y + 13), over ? hover : probed ? selected : wire, 3);
                auto size = ImGui::CalcTextSize(label.c_str());
                text(x - size.x / 2, y - size.y / 2, label);
                if (over) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        ImGui::SetTooltip("%s", tr("Click to select voltage/current probes", "点击选择电压/电流探针"));
                        if (ImGui::IsMouseClicked(0))
                                clickPart = id;
                }
        };
        const float bridgeWidth = width * .48f;
        line(12, 35, bridgeWidth - 12, 35, "bus");
        node(18, 35, "bus", "DC+");
        part(bridgeWidth - 35, 15, "bus", "I DC");
        line(12, 375, bridgeWidth - 12, 375, "ground");
        node(18, 375, "ground", "GND");
        const float cell = (bridgeWidth - 24) / 3;
        const float mx = width * .78f, my = 210, radius = std::min(130.f, width * .19f);
        const ImU32 phaseColors[] = {IM_COL32(83, 145, 230, 255), IM_COL32(238, 162, 78, 255), IM_COL32(90, 197, 138, 255)};
        draw->AddCircleFilled(point(mx, my), radius, IM_COL32(23, 30, 40, 255), 96);
        draw->AddCircle(point(mx, my), radius, wire, 96, 2);
        draw->AddCircle(point(mx, my), radius - 9, IM_COL32(80, 92, 112, 255), 96, 1);
        text(mx - radius + 10, my - radius - 25, tr("Motor / star windings", "电机 / 星形绕组"));
        text(12, 398, tr("3-phase inverter", "三相逆变器"));
        text(mx - radius + 10, my + radius + 13, "R + L + EMF");
        for (int phase = 0; phase < 3; ++phase) {
                std::string p(1, 'a' + phase), label(1, 'A' + phase);
                const float x = 12 + cell * (phase + .5f), sw = 150 + phase * 35;
                line(x, 35, x, 48, "bus");
                line(x, 48, x, 96);
                text(x + 4, 50, "R/Lp");
                node(x, 80, "d_" + p, "");
                part(x, 110, "h" + p, "Q" + label + "H");
                line(x, 123, x, 272, p);
                node(x, sw, p, label);
                line(x - 24, 110, x - 19, 110, "g_h" + p);
                node(x - 24, 110, "g_h" + p, "");
                part(x, 285, "l" + p, "Q" + label + "L");
                line(x - 24, 285, x - 19, 285, "g_l" + p);
                node(x - 24, 285, "g_l" + p, "");
                line(x, 298, x, 330, "s_" + p);
                node(x, 314, "s_" + p, "");
                text(x + 4, 335, "Ls");
                line(x, 330, x, 375);
                const float angle = (-120 + phase * 120) * 3.14159265358979323846f / 180;
                const float ux = std::cos(angle), uy = std::sin(angle), tx = mx + radius * ux, ty = my + radius * uy;
                // Output paths cross other branches without junctions. A background
                // stroke keeps these crossings distinct, with phase colors as a guide
                std::vector<ImVec2> path;
                const float         lane = bridgeWidth + 12 + phase * 14;
                if (phase == 1)
                        path = {point(x, sw),
                                point(lane, sw),
                                point(lane, 385),
                                point(width - 10, 385),
                                point(width - 10, ty),
                                point(tx, ty)};
                else
                        path = {point(x, sw), point(lane, sw), point(lane, ty), point(tx, ty)};
                bool overPath = false;
                if (active) {
                        const auto mouse = ImGui::GetIO().MousePos;
                        for (size_t segment = 1; segment < path.size(); ++segment) {
                                const auto  a = path[segment - 1], b = path[segment];
                                const float dx = b.x - a.x, dy = b.y - a.y, length = dx * dx + dy * dy;
                                const float f =
                                    length > 0 ? std::clamp(((mouse.x - a.x) * dx + (mouse.y - a.y) * dy) / length, 0.f, 1.f)
                                               : 0;
                                const float ex = mouse.x - a.x - f * dx, ey = mouse.y - a.y - f * dy;
                                overPath |= ex * ex + ey * ey < 25;
                        }
                }
                draw->AddPolyline(path.data(), static_cast<int>(path.size()), IM_COL32(15, 15, 15, 255), 0, 5);
                draw->AddPolyline(path.data(), static_cast<int>(path.size()), overPath ? hover : phaseColors[phase], 0, 2);
                if (overPath) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        ImGui::SetTooltip("%s: %s", tr("Voltage probe", "电压探针"), label.c_str());
                        if (ImGui::IsMouseClicked(0))
                                clickNode = p;
                }
                // Coil turns are a visual representation of each lumped R-L-EMF
                // winding; they do not change the electrical model or its parameters
                std::vector<ImVec2> coil;
                for (int j = 0; j <= 100; ++j) {
                        float f = j / 100.f, r = 22 + (radius - 30) * f;
                        float offset = 8 * std::sin(f * 14 * 3.14159265358979323846f) * std::sin(f * 3.14159265358979323846f);
                        coil.push_back(point(mx + r * ux - offset * uy, my + r * uy + offset * ux));
                }
                draw->AddPolyline(coil.data(), static_cast<int>(coil.size()), phaseColors[phase], 0, 2.5f);
                line(mx, my, mx + 22 * ux, my + 22 * uy, "neutral");
                line(mx + (radius - 8) * ux, my + (radius - 8) * uy, tx, ty, p);
                text(mx + (radius * .58f) * ux - 8, my + (radius * .58f) * uy - 18, label);
                bool over = false;
                if (active) {
                        auto  mouse = ImGui::GetIO().MousePos;
                        float dx = mouse.x - origin.x - mx, dy = mouse.y - origin.y - my;
                        float along = dx * ux + dy * uy, side = -dx * uy + dy * ux;
                        over = along > 24 && along < radius - 14 && std::abs(side) < 14;
                }
                if (over) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        ImGui::SetTooltip(
                            "%s %s", label.c_str(), tr("winding: voltage / current probes", "相绕组：点击选择电压 / 电流探针"));
                        draw->AddPolyline(coil.data(), static_cast<int>(coil.size()), hover, 0, 3);
                        if (ImGui::IsMouseClicked(0))
                                clickPart = "winding" + p;
                }
                node(tx, ty, p, label);
                // Draw the output dot again above the colored connection path
                node(x, sw, p, "");
        }
        node(mx, my, "neutral", "N");
        if (clickPart.empty() && !clickNode.empty()) {
                if (clickNode == "ground") { // Ground is explicitly available as a zero-reference trace
                        toggleProbe("ground");
                } else
                        toggleProbe(clickNode);
        }
        if (!clickPart.empty()) {
                probePart_ = clickPart;
                ImGui::OpenPopup("##probe_choices");
        }
        if (ImGui::BeginPopup("##probe_choices")) {
                std::vector<std::string> keys;
                if (probePart_ == "bus")
                        keys = {"bus", "ibus"};
                else if (probePart_.size() == 2 && (probePart_[0] == 'h' || probePart_[0] == 'l'))
                        keys = {"vds_" + probePart_, "vgs_" + probePart_, "id_" + probePart_};
                else {
                        std::string p = probePart_.substr(probePart_.size() - 1);
                        if (probePart_.starts_with("winding"))
                                keys = {"winding_" + p, "i_" + p, "vr_" + p, "vl_" + p, "emf_" + p, "r_" + p, "e_" + p};
                        else if (probePart_.starts_with("r"))
                                keys = {"vr_" + p, "i_" + p};
                        else if (probePart_.starts_with("lwind"))
                                keys = {"vl_" + p, "i_" + p};
                        else
                                keys = {"emf_" + p, "i_" + p, "winding_" + p};
                }
                auto definitions = ava_spice::probeDefinitions(draft_);
                for (const auto &key : keys) {
                        auto it =
                            std::find_if(definitions.begin(), definitions.end(), [&](const auto &t) { return t.key == key; });
                        if (it != definitions.end() &&
                            ImGui::Selectable((it->label + " (" + it->unit + ")").c_str(),
                                              std::find(probes_.begin(), probes_.end(), key) != probes_.end(),
                                              ImGuiSelectableFlags_DontClosePopups))
                                toggleProbe(key);
                }
                ImGui::EndPopup();
        }
        if (!result_.times.empty() && applied_.topology == 1) {
                ImGui::TextDisabled("%s",
                                    tr("Probe values refer to the last successful run; rerun after parameter changes.",
                                       "探针显示上一次成功运行的数据，修改参数后需重新运行。"));
        }
        // Line-to-line probes are also selectable without introducing a ground reference
        bool firstLineProbe = true;
        for (const auto &key : {"vab", "vbc", "vca"}) {
                if (!firstLineProbe)
                        ImGui::SameLine();
                firstLineProbe = false;
                bool on        = std::find(probes_.begin(), probes_.end(), key) != probes_.end();
                if (ImGui::Checkbox(key, &on))
                        toggleProbe(key);
        }
}
void
InverterSimulator::syncScopes()
{
        if (scopesThreePhase_ != (applied_.topology == 1)) {
                for (auto &scope : scopes_)
                        scope.reset();
                scopesThreePhase_ = applied_.topology == 1;
        }
        if (applied_.topology == 1) {
                syncProbeScopes();
                return;
        }
        const char *titles[]    = {"VGS / Vdriver", "VDS", "ID / Iload / Ifreewheel", "Pterminal", "Eterminal"};
        const char *names[5][3] = {
            {"VGS", "Vdriver"}, {"VDS"}, {"ID (terminal)", "Iload", "Ifreewheel"}, {"Pterminal"}, {"Eterminal"}};
        auto val = [](const ava_spice::Sample &sample, int kind, int channel) {
                switch (kind) {
                        case 0:
                                return channel ? sample.drive : sample.vgs;
                        case 1:
                                return sample.vds;
                        case 2:
                                return channel == 0 ? sample.id : channel == 1 ? sample.iload : sample.ifreewheel;
                        case 3:
                                return sample.power;
                        default:
                                return sample.energy * 1e6;
                }
        };
        for (int kind = 0; kind < 5; ++kind) {
                if (!scopes_[kind])
                        scopes_[kind] = std::make_unique<MonitorScope>(titles[kind]);
                auto &scope = *scopes_[kind];
                // Existing current-channel styles survive reruns. Hidden previous channels
                // are removed so they do not appear in tables or automatic axis fitting
                std::erase_if(scope.getChannels(), [](const auto &entry) { return entry.first.starts_with("previous_"); });
                auto load = [&](const ava_spice::Result &run, bool previous) {
                        if (run.samples.empty())
                                return;
                        for (int channel = 0; channel < (kind == 0 ? 2 : kind == 2 ? 3 : 1); ++channel) {
                                std::string key = std::string(previous ? "previous_" : "current_") + std::to_string(channel);
                                auto       *ch  = scope.findChannel(key);
                                if (!ch) {
                                        scope.addChannel(key);
                                        ch         = scope.findChannel(key);
                                        auto color = ImPlot::GetColormapColor(channel);
                                        color.w    = previous ? .45f : 1.0f;
                                        std::copy_n(&color.x, 4, ch->getColor());
                                        ch->useAutoColor()  = false;
                                        ch->getLineWeight() = previous ? 1.0f : 2.0f;
                                }
                                ch->clearData();
                                ch->setType("f32");
                                ch->setDevice("SPICE");
                                ch->setWritable(false);
                                ch->historySeconds_ = 0;
                                ch->setLabel(std::string(names[kind][channel]) +
                                             (previous ? tr(" (previous)", " (上一次)") : tr(" (current)", " (本次)")));
                                for (const auto &sample : run.samples)
                                        ch->setRVal(static_cast<float>(val(sample, kind, channel)), sample.time * 1e6);
                                ch->publishSnapshot();
                        }
                };
                load(result_, false);
                if (compare_ && previousConfig_.topology == 0)
                        load(previous_, true);
                scope.resetYFit();
        }
}
void
InverterSimulator::plot(const char *title, int kind)
{
        if (!scopes_[kind])
                return;
        auto &scope = *scopes_[kind];
        scope.setLabel(title);
        scope.setAxisLabels(tr("Time (us)", "时间 (us)"),
                            applied_.topology == 1 ? (kind == 0 ? "V" : "A")
                            : kind < 2             ? "V"
                            : kind == 2            ? "A"
                            : kind == 3            ? "W"
                                                   : "uJ");
        ImGui::PushID(kind);
        if (ImGui::BeginChild("##spice_scope", ImVec2(0, applied_.topology == 1 ? 340 : 280), true)) {
                scope.menu(true);
                scope.draw(&minUs_, &maxUs_, 5000, viewMode_);
        }
        ImGui::EndChild();
        ImGui::PopID();
}
void
InverterSimulator::exportCsv()
{
        auto path = nativeDlgSave(tr("Export SPICE waveform", "导出 SPICE 波形"), {{"CSV", {"csv"}}}, "spice_switching.csv");
        if (path.empty())
                return;
        std::ofstream f(std::filesystem::u8path(path));
        f.imbue(std::locale::classic());
        f << std::setprecision(15);
        if (!result_.times.empty()) {
                f << "time_s";
                for (const auto &trace : result_.traces)
                        f << ',' << trace.key << '_' << trace.unit;
                f << '\n';
                for (size_t i = 0; i < result_.times.size(); ++i) {
                        f << result_.times[i];
                        for (const auto &trace : result_.traces)
                                f << ',' << trace.values[i];
                        f << '\n';
                }
        } else {
                f << "time_s,drive_V,vgs_V,vds_V,id_A,iload_A,ifreewheel_A,igate_A,power_W,terminal_energy_J\n";
                for (const auto &s : result_.samples)
                        f << s.time << ',' << s.drive << ',' << s.vgs << ',' << s.vds << ',' << s.id << ',' << s.iload << ','
                          << s.ifreewheel << ',' << s.ig << ',' << s.power << ',' << s.energy << '\n';
        }
        f.close();
        if (!f)
                error_ = tr("CSV write failed", "CSV 写入失败");
}
void
InverterSimulator::saveSession(void *root) const
{
        auto *obj = cJSON_AddObjectToObject(static_cast<cJSON *>(root), "inverterSimulation");
        cJSON_AddBoolToObject(obj, "show", show_);
        cJSON_AddNumberToObject(obj, "device", draft_.device);
        cJSON_AddNumberToObject(obj, "topology", draft_.topology);
        cJSON_AddNumberToObject(obj, "probePreset", probePreset_);
        auto *probes = cJSON_AddArrayToObject(obj, "probes");
        for (const auto &key : probes_)
                cJSON_AddItemToArray(probes, cJSON_CreateString(key.c_str()));
        cJSON_AddBoolToObject(obj, "package", draft_.package);
        cJSON_AddStringToObject(obj, "executable", draft_.executable.c_str());
        cJSON_AddStringToObject(obj, "library", draft_.library.c_str());
        cJSON_AddStringToObject(obj, "model", draft_.model.c_str());
        cJSON_AddStringToObject(obj, "pins", draft_.pins.c_str());
        for (const auto &p : parameters)
                cJSON_AddNumberToObject(obj, p.key, draft_.*p.member);
}
void
InverterSimulator::loadSession(const void *root)
{
        auto *obj = cJSON_GetObjectItemCaseSensitive(static_cast<const cJSON *>(root), "inverterSimulation");
        if (!cJSON_IsObject(obj))
                return;
        draft_.topology = 0; // Existing saved sessions remain double-pulse tests
        if (auto *v = cJSON_GetObjectItemCaseSensitive(obj, "topology"); cJSON_IsNumber(v))
                draft_.topology = v->valueint == 1 ? 1 : 0;
        if (auto *v = cJSON_GetObjectItemCaseSensitive(obj, "probes"); cJSON_IsArray(v)) {
                probes_.clear();
                for (auto *item = v->child; item; item = item->next)
                        if (cJSON_IsString(item) && probes_.size() < 64 &&
                            std::find(probes_.begin(), probes_.end(), item->valuestring) == probes_.end())
                                probes_.emplace_back(item->valuestring);
        }
        if (auto *v = cJSON_GetObjectItemCaseSensitive(obj, "probePreset");
            cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint <= 6) {
                probePreset_ = v->valueint;
                if (probePreset_ > 0)
                        applyProbePreset(probePreset_);
        }
        if (auto *v = cJSON_GetObjectItemCaseSensitive(obj, "show"); cJSON_IsBool(v))
                show_ = cJSON_IsTrue(v);
        if (auto *v = cJSON_GetObjectItemCaseSensitive(obj, "device"); cJSON_IsNumber(v))
                draft_.device = v->valueint >= 0 && v->valueint <= 2 ? v->valueint : 1;
        if (auto *v = cJSON_GetObjectItemCaseSensitive(obj, "package"); cJSON_IsBool(v))
                draft_.package = cJSON_IsTrue(v);
        for (auto entry : {std::pair{"executable", &draft_.executable},
                           {"library", &draft_.library},
                           {"model", &draft_.model},
                           {"pins", &draft_.pins}})
                if (auto *v = cJSON_GetObjectItemCaseSensitive(obj, entry.first); cJSON_IsString(v))
                        *entry.second = v->valuestring;
        for (const auto &p : parameters)
                if (auto *v = cJSON_GetObjectItemCaseSensitive(obj, p.key); cJSON_IsNumber(v))
                        draft_.*p.member = v->valuedouble;
        if (!cJSON_GetObjectItemCaseSensitive(obj, "pwmCycles")) {
                if (auto *duration = cJSON_GetObjectItemCaseSensitive(obj, "durationUs");
                    cJSON_IsNumber(duration) && std::isfinite(duration->valuedouble) && std::isfinite(draft_.pwmHz) &&
                    draft_.pwmHz > 0)
                        draft_.pwmCycles = std::clamp(std::ceil(duration->valuedouble * draft_.pwmHz * 1e-6), 1.0, 10000.0);
        }
        draft_.pwmCycles = std::isfinite(draft_.pwmCycles) ? std::clamp(std::round(draft_.pwmCycles), 1.0, 10000.0) : 2;
        models_          = ava_spice::models(draft_.library);
        modified_        = false;
}
