#include "gui/assembly_viewer.hpp"
#include "gui/gui.hpp"
#include "gui/i18n.hpp"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <thread>

void
AssemblyViewer::draw(Gui *gui)
{
        if (!show_)
                return;

        if (ImGui::Begin(tr("Assembly Viewer###AsmViewer", "汇编查看器###AsmViewer"), &show_)) {
                // Find all ELF paths
                std::vector<std::string> elfPaths;
                for (const auto &[name, var] : gui->getVars()) {
                        const std::string &path = var->getElfPath();
                        if (!path.empty() && std::find(elfPaths.begin(), elfPaths.end(), path) == elfPaths.end()) {
                                elfPaths.push_back(path);
                        }
                }

                if (elfPaths.empty()) {
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                           "%s",
                                           tr("No ELF/AXF files loaded in Variables.", "变量窗口中未加载 ELF/AXF 文件。"));
                        ImGui::End();
                        return;
                }

                // Dropdown to select ELF
                if (currentElfPath_.empty() && !elfPaths.empty()) {
                        currentElfPath_ = elfPaths.front();
                }

                bool elfComboOpen = ImGui::BeginCombo("##SelectElf", currentElfPath_.c_str());
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Select ELF", "选择 ELF"));
                if (elfComboOpen) {
                        for (const auto &path : elfPaths) {
                                bool isSelected = (currentElfPath_ == path);
                                if (ImGui::Selectable(path.c_str(), isSelected)) {
                                        currentElfPath_ = path;
                                        disassembly_.clear();
                                }
                                if (isSelected)
                                        ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                }

                ImGui::SameLine();
                if (ImGui::Button(tr("Disassemble", "反汇编"))) {
                        disassemble(currentElfPath_);
                }

                if (isLoading_) {
                        ImGui::SameLine();
                        ImGui::TextColored(
                            ImVec4(1.0f, 1.0f, 0.4f, 1.0f),
                            "%s",
                            tr("Disassembling... (this may take a moment)", "正在反汇编...（可能需要一些时间）"));
                }

                ImGui::Separator();

                static ImGuiTextFilter filter;
                filter.Draw("##AsmFilter");
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tr("Filter (e.g., function name)", "过滤（例如函数名）"));

                ImGui::Separator();

                // Display disassembly in a scrollable region
                ImGui::BeginChild("AsmRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

                if (!disassembly_.empty()) {
                        // Split into lines for filtering (only if filter is active, otherwise just show all)
                        if (filter.IsActive()) {
                                const char *line_start = disassembly_.c_str();
                                const char *line_end   = disassembly_.c_str();
                                while (*line_end != '\0') {
                                        while (*line_end != '\n' && *line_end != '\0')
                                                line_end++;

                                        if (filter.PassFilter(line_start, line_end)) {
                                                ImGui::TextUnformatted(line_start, line_end);
                                        }

                                        if (*line_end == '\n') {
                                                line_end++;
                                                line_start = line_end;
                                        }
                                }
                        } else {
                                ImGui::TextUnformatted(disassembly_.c_str());
                        }
                }

                ImGui::EndChild();
        }
        ImGui::End();
}

void
AssemblyViewer::disassemble(const std::string &elfPath)
{
        if (isLoading_)
                return;

        isLoading_ = true;
        disassembly_.clear();

        // Run in background thread to avoid blocking GUI
        std::thread([this, elfPath]() {
                std::string            cmd = "arm-none-eabi-objdump -d \"" + elfPath + "\"";
                std::string            result;
                std::array<char, 4096> buffer;

#ifdef _WIN32
                std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(cmd.c_str(), "r"), _pclose);
#else
                std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
#endif
                if (!pipe) {
                        result = "Failed to run arm-none-eabi-objdump. Ensure it is in your PATH.";
                } else {
                        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
                                result += buffer.data();
                        }
                }

                disassembly_ = std::move(result);
                isLoading_   = false;
        }).detach();
}
