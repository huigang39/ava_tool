#include "tutorial_guide.hpp"
#include "i18n.hpp"

#include <cmath>
#include <fstream>

TutorialGuide &
TutorialGuide::instance()
{
        static TutorialGuide s;
        return s;
}

TutorialGuide::TutorialGuide()
{
        // Steps use IDs only; text is resolved at draw-time via tr() for live language switching.
        steps_ = {
            {"device_btn"},
            {"speed_slider"},
            {"connect_btn"},
            {"menu_window"},
            {"variable_browser"},
            {"monitor_window"},
            {"menu_tools"},
            {"pause_btn"},
        };
}

// Returns title & description for a step, evaluated with current language.
static void
getStepText(const std::string &id, const char *&title, const char *&desc)
{
        if (id == "device_btn") {
                title = tr("Select Device", "选择设备");
                desc  = tr("Click this button to choose your MCU model (e.g. STM32H745II).\n"
                           "This tells J-Link which chip to connect to.",
                          "点击此按钮选择你的 MCU 型号（如 STM32H745II）。\n"
                           "这会告诉 J-Link 连接哪款芯片。");
        } else if (id == "speed_slider") {
                title = tr("SWD Speed", "SWD 速率");
                desc  = tr("Drag this slider to set the J-Link SWD clock speed.\n"
                           "Higher = faster sampling, but may fail on long cables.",
                          "拖动此滑块设置 J-Link SWD 时钟速度。\n"
                           "越高采样越快，但线缆过长可能失败。");
        } else if (id == "connect_btn") {
                title = tr("Connect to Target", "连接目标芯片");
                desc  = tr("Click CONNECT to establish a J-Link connection.\n"
                           "The button turns green when connected.\n"
                           "Shortcut: Ctrl+L",
                          "点击 CONNECT 建立 J-Link 连接。\n"
                           "连接成功后按钮变为绿色。\n"
                           "快捷键：Ctrl+L");
        } else if (id == "menu_window") {
                title = tr("Window Menu", "窗口菜单");
                desc  = tr("Use this menu to add the two main windows:\n"
                           "- Variable Manager: load an ELF/AXF file and browse/edit MCU variables.\n"
                           "- Variable Monitor: plot real-time waveforms of selected variables.",
                          "使用此菜单添加两个主要窗口：\n"
                           "- 变量管理器：加载 ELF/AXF 文件，浏览/编辑 MCU 变量。\n"
                           "- 变量监视器：实时绘制选中变量的波形。");
        } else if (id == "variable_browser") {
                title = tr("AXF/ELF Symbol Parsing", "AXF/ELF符号解析");
                desc  = tr("Inside the Variable Manager, after loading an ELF/AXF file this panel lets you\n"
                           "search and browse all MCU symbols (structs/arrays expand into members).\n\n"
                           "- Double-click a symbol to add it to the watch list.\n"
                           "- Drag a symbol (or a whole struct) onto a Variable Monitor to plot it —\n"
                           "  it is also added to the watch list automatically.",
                          "在变量管理器中加载 ELF/AXF 文件后，可在此面板\n"
                           "搜索并浏览所有 MCU 符号（结构体/数组可展开成成员）。\n\n"
                           "- 双击符号即可加入监视列表。\n"
                           "- 把符号（或整个结构体）拖到变量监视器即可绘图，\n"
                           "  同时会自动加入监视列表。");
        } else if (id == "monitor_window") {
                title = tr("Variable Monitor", "变量监视器");
                desc  = tr("The Variable Monitor plots real-time waveforms.\n\n"
                           "- Add Scope to create plotting areas; reorder them with the up/down arrows.\n"
                           "- Drag variables (or whole structs) here from the Variable Browser.\n"
                           "- Switch a scope between time-domain and FFT, export data to CSV, and more.",
                          "变量监视器用于实时绘制波形。\n\n"
                           "- 用「添加示波器」创建绘图区，用上下箭头调整顺序。\n"
                           "- 从变量浏览器把变量（或整个结构体）拖到这里。\n"
                           "- 可在时域/FFT 间切换、导出 CSV 等。");
        } else if (id == "menu_tools") {
                title = tr("Tools Menu", "工具菜单");
                desc  = tr("Access advanced tools here:\n"
                           "- Sequence Editor: automate variable write sequences.\n"
                           "- Bode Plot: frequency response analysis.\n"
                           "- Assembly Viewer: inspect disassembly.",
                          "在此访问高级工具：\n"
                           "- 序列编辑器：自动化变量写入序列。\n"
                           "- 伯德图：频率响应分析。\n"
                           "- 汇编查看器：查看反汇编。");
        } else if (id == "pause_btn") {
                title = tr("Pause / Resume", "暂停 / 继续");
                desc  = tr("Click to pause or resume real-time data display.\n"
                           "Data acquisition continues in the background.\n"
                           "Shortcut: Space bar.",
                          "点击暂停或继续实时数据显示。\n"
                           "后台数据采集不会中断。\n"
                           "快捷键：空格键。");
        } else {
                title = "?";
                desc  = "";
        }
}

void
TutorialGuide::mark(const char *id)
{
        if (!active_ || step_ >= (int)steps_.size())
                return;
        if (steps_[step_].id == id) {
                targetFound_ = true;
                targetMin_   = ImGui::GetItemRectMin();
                targetMax_   = ImGui::GetItemRectMax();
        }
}

void
TutorialGuide::draw()
{
        if (!active_ || step_ >= (int)steps_.size())
                return;

        pulseTime_ += ImGui::GetIO().DeltaTime;

        ImDrawList *fg = ImGui::GetForegroundDrawList();

        if (targetFound_) {
                // Pulsing highlight border around the target widget
                float  pulse     = 0.6f + 0.4f * sinf(pulseTime_ * 4.0f);
                ImU32  borderCol = ImGui::ColorConvertFloat4ToU32(ImVec4(0.2f, 0.8f, 1.0f, pulse));
                float  pad       = 4.0f;
                ImVec2 rMin(targetMin_.x - pad, targetMin_.y - pad);
                ImVec2 rMax(targetMax_.x + pad, targetMax_.y + pad);
                fg->AddRect(rMin, rMax, borderCol, 6.0f, 0, 3.0f);

                // Semi-transparent fill
                ImU32 fillCol = ImGui::ColorConvertFloat4ToU32(ImVec4(0.2f, 0.8f, 1.0f, 0.08f));
                fg->AddRectFilled(rMin, rMax, fillCol, 6.0f);

                // Arrow pointing down to the tooltip
                float arrowX = (rMin.x + rMax.x) * 0.5f;
                float arrowY = rMax.y + 6.0f;
                fg->AddTriangleFilled(
                    ImVec2(arrowX - 8, arrowY + 12), ImVec2(arrowX + 8, arrowY + 12), ImVec2(arrowX, arrowY), borderCol);
        }

        // Resolve title & description for current language
        const char *title = "";
        const char *desc  = "";
        getStepText(steps_[step_].id, title, desc);

        const float  kWinW = 380.0f;
        const ImVec2 ds    = ImGui::GetIO().DisplaySize;
        ImVec2       tooltipPos;
        if (targetFound_) {
                tooltipPos = ImVec2((targetMin_.x + targetMax_.x) * 0.5f, targetMax_.y + 24.0f);
        } else {
                tooltipPos = ImVec2(ds.x * 0.5f, ds.y * 0.5f);
        }

        // The callout is drawn horizontally centred on tooltipPos (pivot 0.5). Clamp its
        // centre so the whole box stays on screen — otherwise a target near the left edge
        // (e.g. the first menu) pushes the left half off-window where it can't be read.
        const float pad   = 8.0f;
        const float halfW = kWinW * 0.5f;
        const float minX  = halfW + pad;
        const float maxX  = ds.x - halfW - pad;
        if (maxX >= minX)
                tooltipPos.x = (tooltipPos.x < minX) ? minX : (tooltipPos.x > maxX ? maxX : tooltipPos.x);

        ImGui::SetNextWindowPos(tooltipPos, ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(kWinW, 0));
        ImGui::SetNextWindowBgAlpha(0.92f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.8f, 1.0f, 0.6f));

        char winId[64];
        snprintf(winId, sizeof(winId), "##tutorial_step_%d", step_);
        if (ImGui::Begin(winId,
                         nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove)) {

                // Step counter
                ImGui::TextColored(
                    ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s %d / %d", tr("Step", "步骤"), step_ + 1, (int)steps_.size());

                // Title
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 1.0f, 1.0f), "%s", title);

                ImGui::Separator();

                // Description
                ImGui::TextWrapped("%s", desc);

                ImGui::Spacing();

                // Navigation buttons
                if (step_ > 0) {
                        if (ImGui::SmallButton(tr("<< Prev", "<< 上一步"))) {
                                step_--;
                                targetFound_ = false;
                                pulseTime_   = 0;
                        }
                        ImGui::SameLine();
                }

                if (step_ < (int)steps_.size() - 1) {
                        if (ImGui::SmallButton(tr("Next >>", "下一步 >>"))) {
                                step_++;
                                targetFound_ = false;
                                pulseTime_   = 0;
                        }
                } else {
                        if (ImGui::SmallButton(tr("Finish!", "完成！"))) {
                                active_ = false;
                                saveState(appDir_);
                        }
                }

                ImGui::SameLine(0, 20);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                if (ImGui::SmallButton(tr("Skip Tutorial", "跳过引导"))) {
                        active_ = false;
                        saveState(appDir_);
                }
                ImGui::PopStyleColor();
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        // Reset for next frame
        targetFound_ = false;
}

void
TutorialGuide::start()
{
        active_      = true;
        step_        = 0;
        targetFound_ = false;
        pulseTime_   = 0;
}

void
TutorialGuide::loadState(const std::string &appDir)
{
        appDir_ = appDir;
        std::ifstream ifs(appDir + "/tutorial_done.txt");
        if (ifs.good()) {
                std::string line;
                std::getline(ifs, line);
                if (line == "1") {
                        active_ = false;
                        return;
                }
        }
        // First launch — start the tutorial
        start();
}

void
TutorialGuide::saveState(const std::string &appDir)
{
        appDir_ = appDir;
        std::ofstream ofs(appDir + "/tutorial_done.txt", std::ios::trunc);
        if (ofs)
                ofs << "1\n";
}
