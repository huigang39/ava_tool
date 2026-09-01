#include "tutorial_guide.hpp"
#include "design_tokens.hpp"
#include "flow_toolbar.hpp"
#include "i18n.hpp"
#include "imgui_internal.h"
#include "interaction_history.hpp"
#include "search_match.hpp"
#include "ui_theme.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>

TutorialGuide &
TutorialGuide::instance()
{
        static TutorialGuide guide;
        return guide;
}

void
TutorialGuide::mark(const char *id)
{
        if (!active_ || !ImGui::IsItemVisible())
                return;
        const auto &step = ui::guide::Chapters()[static_cast<int>(topic_)].steps[step_];
        if (std::strcmp(step.target, id) != 0)
                return;
        auto *window = ImGui::GetCurrentWindow();
        if (owner_ && topic_ != Topic::Overview && topic_ != Topic::JLink) {
                auto *parent = window;
                while (parent && parent->ID != owner_)
                        parent = parent->ParentWindow;
                if (!parent)
                        return;
        }
        targetFound_    = true;
        targetMin_      = ImGui::GetItemRectMin();
        targetMax_      = ImGui::GetItemRectMax();
        targetViewport_ = window->Viewport->ID;
}

void
TutorialGuide::markJLinkEntry()
{
        if (!active_)
                return;
        const auto *target = ui::guide::Chapters()[static_cast<int>(topic_)].steps[step_].target;
        if (std::strcmp(target, "jlink_entry") == 0 || std::strcmp(target, "device_btn") == 0 ||
            std::strcmp(target, "connect_btn") == 0 || std::strcmp(target, "pause_btn") == 0)
                mark(target);
}

void
TutorialGuide::markDock(Topic topic)
{
        auto *window = ImGui::GetCurrentWindow();
        if (!window || window->Hidden || window->SkipItems)
                return;
        docks_[window->ID] = {topic,
                              window->Pos,
                              ImVec2(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y),
                              window->Viewport->ID,
                              ImGui::GetFrameCount()};
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
                lastDock_ = window->ID;
}

void
TutorialGuide::markKnownDock(const char *name)
{
        static constexpr std::pair<const char *, Topic> known[] = {{"###ParameterFileEditor", Topic::Parameters},
                                                                   {"###RewriteVariable", Topic::Rewrite},
                                                                   {"###FaultDecoder", Topic::Safety},
                                                                   {"###DeviceManager", Topic::Devices},
                                                                   {"###JFlash", Topic::Flash},
                                                                   {"###JLinkRttWindow", Topic::RTT},
                                                                   {"###SwoItmTraceWindow", Topic::Trace},
                                                                   {"###BkpSramCapture", Topic::RingCapture},
                                                                   {"###MotorControl", Topic::Motor},
                                                                   {"###MotorCalc", Topic::Calculator},
                                                                   {"###BodePlot", Topic::Bode},
                                                                   {"###DSOX2024A", Topic::DSO},
                                                                   {"###SeqEditor", Topic::Sequence},
                                                                   {"###MidiTool", Topic::MIDI},
                                                                   {"###InverterSimulation", Topic::Inverter},
                                                                   {"###AsmViewer", Topic::Assembly}};
        for (const auto &[id, topic] : known)
                if (std::strstr(name, id)) {
                        markDock(topic);
                        return;
                }
}

void
TutorialGuide::start(Topic topic, bool resume)
{
        if (topic < Topic::Overview || topic >= Topic::Count)
                return;
        topic_                             = topic;
        step_                              = resume ? progress_[static_cast<int>(topic)] : 0;
        progress_[static_cast<int>(topic)] = step_;
        active_                            = true;
        catalogOpen_                       = false;
        targetFound_                       = false;
        owner_                             = 0;
        if (auto it = docks_.find(lastDock_);
            it != docks_.end() && it->second.topic == topic && it->second.frame >= ImGui::GetFrameCount() - 1)
                owner_ = lastDock_;
}

void
TutorialGuide::openCatalog()
{
        saveState(appDir_);
        active_      = false;
        targetFound_ = false;
        catalogOpen_ = true;
}

void
TutorialGuide::showForCurrentDock()
{
        auto *context = ImGui::GetCurrentContext();
        for (auto *window = context->NavWindow; window; window = window->ParentWindow) {
                auto it = docks_.find(window->ID);
                if (it != docks_.end() && it->second.frame >= context->FrameCount - 1) {
                        lastDock_ = window->ID;
                        start(it->second.topic, true);
                        return;
                }
        }
        if (auto it = docks_.find(lastDock_); it != docks_.end() && it->second.frame >= context->FrameCount - 1) {
                start(it->second.topic, true);
                return;
        }
        openCatalog();
}

void
TutorialGuide::dismiss()
{
        active_ = catalogOpen_ = targetFound_ = false;
}

void
TutorialGuide::moveStep(int next)
{
        const auto &chapter                 = ui::guide::Chapters()[static_cast<int>(topic_)];
        step_                               = std::clamp(next, 0, static_cast<int>(chapter.steps.size()) - 1);
        progress_[static_cast<int>(topic_)] = step_;
        targetFound_                        = false;
        saveState(appDir_);
}

void
TutorialGuide::drawCatalog()
{
        auto        *viewport = ImGui::GetMainViewport();
        const ImVec2 maximum(std::max(1.0f, viewport->WorkSize.x - 24), std::max(1.0f, viewport->WorkSize.y - 24));
        const ImVec2 size(std::min(ImGui::GetFontSize() * 54, maximum.x), std::min(ImGui::GetFontSize() * 36, maximum.y));
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * .5f, viewport->WorkPos.y + viewport->WorkSize.y * .5f),
            ImGuiCond_Appearing,
            ImVec2(.5f, .5f));
        ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(360.0f, maximum.x), std::min(260.0f, maximum.y)), maximum);
        // A live resize does not make an existing window "appear" again. Clamp
        // its saved position as well as its size when the viewport gets smaller.
        if (auto *existing = ImGui::FindWindowByName("###PanelGuide"); existing && existing->WasActive) {
                const ImVec2 extent(std::min(existing->SizeFull.x, maximum.x), std::min(existing->SizeFull.y, maximum.y));
                const ImVec2 minimum(viewport->WorkPos.x + 12, viewport->WorkPos.y + 12);
                ImGui::SetNextWindowPos(ImVec2(std::clamp(existing->Pos.x, minimum.x, minimum.x + maximum.x - extent.x),
                                               std::clamp(existing->Pos.y, minimum.y, minimum.y + maximum.y - extent.y)));
        }
        if (ImGui::Begin(tr("Panel guides###PanelGuide", "面板指南###PanelGuide"), &catalogOpen_, ImGuiWindowFlags_NoDocking)) {
                const auto &chapters = ui::guide::Chapters();
                const bool  wide     = ImGui::GetContentRegionAvail().x >= ImGui::GetFontSize() * 40;
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputTextWithHint(
                    "##guideSearch", tr("Find a panel or button", "搜索面板或按钮"), search_, sizeof(search_));
                const auto query   = ui::search::LowerCopy(search_);
                const bool columns = wide && ImGui::BeginTable("##guideColumns", 2, ImGuiTableFlags_Resizable);
                if (columns) {
                        ImGui::TableSetupColumn("Chapters", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 13);
                        ImGui::TableSetupColumn("Instructions", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableNextColumn();
                }
                if (ImGui::BeginChild("##guideTopics", ImVec2(0, columns ? 0 : ImGui::GetTextLineHeightWithSpacing() * 5))) {
                        bool any = false;
                        for (int i = 0; i < static_cast<int>(chapters.size()); ++i) {
                                const auto &chapter  = chapters[i];
                                std::string haystack = std::string(chapter.titleEn) + chapter.titleZh;
                                for (const auto &step : chapter.steps)
                                        haystack += std::string(step.titleEn) + step.titleZh + step.bodyEn + step.bodyZh;
                                if (!query.empty() && ui::search::LowerCopy(haystack).find(query) == std::string::npos)
                                        continue;
                                any = true;
                                ImGui::PushID(i);
                                if (ImGui::Selectable(tr(chapter.titleEn, chapter.titleZh), static_cast<int>(topic_) == i))
                                        topic_ = static_cast<Topic>(i);
                                ImGui::PopID();
                        }
                        if (!any)
                                ImGui::TextDisabled("%s", tr("No matching guides", "没有匹配的指南"));
                }
                ImGui::EndChild();
                if (columns)
                        ImGui::TableNextColumn();
                if (ImGui::BeginChild("##guideChapter")) {
                        const auto &chapter = chapters[static_cast<int>(topic_)];
                        ImGui::TextUnformatted(tr(chapter.titleEn, chapter.titleZh));
                        ImGui::TextWrapped("%s", tr(chapter.locationEn, chapter.locationZh));
                        ui::FlowToolbar actions;
                        const auto     *resume = tr("Continue guide", "继续引导");
                        actions.next(resume);
                        if (ui::Button(resume, ui::BtnStyle::Primary))
                                start(topic_, true);
                        actions.next(tr("Start over", "从头开始"));
                        if (ImGui::Button(tr("Start over", "从头开始")))
                                start(topic_);
                        if (completed_[static_cast<int>(topic_)]) {
                                actions.next(tr("Completed", "已完成"));
                                ImGui::TextDisabled("%s", tr("Completed", "已完成"));
                        }
                        for (size_t i = 0; i < chapter.steps.size(); ++i) {
                                const auto &step = chapter.steps[i];
                                ImGui::Separator();
                                ImGui::TextWrapped("%zu. %s", i + 1, tr(step.titleEn, step.titleZh));
                                ImGui::TextWrapped("%s", tr(step.bodyEn, step.bodyZh));
                        }
                }
                ImGui::EndChild();
                if (columns)
                        ImGui::EndTable();
                if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() &&
                    ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                        catalogOpen_ = false;
                        ui::interaction::consumeEscape();
                }
        }
        ImGui::End();
}

void
TutorialGuide::draw()
{
        auto &context = *ImGui::GetCurrentContext();
        std::erase_if(docks_, [&](const auto &entry) { return entry.second.frame < context.FrameCount - 1; });
        if (ImGui::GetTopMostPopupModal()) {
                targetFound_ = false;
                return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false) && context.OpenPopupStack.empty())
                showForCurrentDock();
        if (catalogOpen_)
                drawCatalog();
        if (!active_) {
                targetFound_ = false;
                return;
        }
        const auto &chapter = ui::guide::Chapters()[static_cast<int>(topic_)];
        const auto &step    = chapter.steps[step_];
        if (!targetFound_) {
                for (const auto &[id, dock] : docks_)
                        if (dock.topic == topic_ && dock.frame == context.FrameCount && (!owner_ || id == owner_)) {
                                owner_          = id;
                                targetMin_      = dock.min;
                                targetMax_      = dock.max;
                                targetViewport_ = dock.viewport;
                                targetFound_    = true;
                                break;
                        }
        }
        auto *viewport = targetFound_ ? ImGui::FindViewportByID(targetViewport_) : nullptr;
        if (!viewport)
                viewport = ImGui::GetMainViewport();
        const auto &style = ImGui::GetStyle();
        if (targetFound_) {
                auto *draw = ImGui::GetForegroundDrawList(viewport);
                draw->PushClipRect(viewport->Pos,
                                   ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y));
                draw->AddRect(targetMin_,
                              targetMax_,
                              ImGui::GetColorU32(ImGuiCol_CheckMark),
                              style.FrameRounding,
                              0,
                              ui::design::Stroke());
                draw->PopClipRect();
        }
        const float margin    = 12;
        const float width     = std::min(ImGui::GetFontSize() * 30, std::max(1.0f, viewport->WorkSize.x - margin * 2));
        const float textWidth = std::max(1.0f, width - style.WindowPadding.x * 4);
        const float textHeight =
            ImGui::CalcTextSize(tr(step.bodyEn, step.bodyZh), nullptr, false, textWidth).y +
            ImGui::CalcTextSize(tr(step.titleEn, step.titleZh), nullptr, false, textWidth).y +
            (targetFound_ ? 0 : ImGui::CalcTextSize(tr(chapter.locationEn, chapter.locationZh), nullptr, false, textWidth).y);
        const float height = textHeight + ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() * 2 +
                             style.WindowPadding.y * 2 + style.ItemSpacing.y * 5;
        const ImVec2 size(
            width, std::min(std::max(ImGui::GetFontSize() * 11, height), std::max(1.0f, viewport->WorkSize.y - margin * 2)));
        ImVec2 position(viewport->WorkPos.x + viewport->WorkSize.x - size.x - margin,
                        viewport->WorkPos.y + viewport->WorkSize.y - size.y - margin);
        if (targetFound_ && targetMin_.y >= position.y && targetMin_.x >= position.x)
                position.y = viewport->WorkPos.y + margin;
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowPos(position);
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, style.PopupRounding);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, style.Colors[ImGuiCol_PopupBg]);
        char id[64];
        snprintf(id, sizeof(id), "##tutorial_step_%d", step_);
        if (ImGui::Begin(id,
                         nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing)) {
                ImGui::TextDisabled("%s  %d / %zu", tr(chapter.titleEn, chapter.titleZh), step_ + 1, chapter.steps.size());
                const float footer = ImGui::GetFrameHeightWithSpacing() * 2;
                if (ImGui::BeginChild("##guideBody", ImVec2(0, std::max(1.0f, ImGui::GetContentRegionAvail().y - footer)))) {
                        ImGui::TextWrapped("%s", tr(step.titleEn, step.titleZh));
                        ImGui::Separator();
                        ImGui::TextWrapped("%s", tr(step.bodyEn, step.bodyZh));
                        if (!targetFound_) {
                                ImGui::Spacing();
                                ImGui::TextWrapped("%s", tr(chapter.locationEn, chapter.locationZh));
                        }
                }
                ImGui::EndChild();
                ui::FlowToolbar navigation;
                const bool      last = step_ + 1 == static_cast<int>(chapter.steps.size());
                navigation.next(tr("Previous", "上一步"));
                ImGui::BeginDisabled(step_ == 0);
                if (ImGui::Button(tr("Previous", "上一步")))
                        moveStep(step_ - 1);
                ImGui::EndDisabled();
                const char *next = last ? tr("Finish", "完成") : tr("Next", "下一步");
                navigation.next(next);
                if (ui::Button(next, ui::BtnStyle::Primary)) {
                        if (last) {
                                completed_[static_cast<int>(topic_)] = true;
                                active_                              = false;
                                saveState(appDir_);
                        } else
                                moveStep(step_ + 1);
                }
                navigation.next(tr("Chapters", "目录"));
                if (ImGui::Button(tr("Chapters", "目录")))
                        openCatalog();
                navigation.next(tr("Close", "关闭"));
                if (ImGui::Button(tr("Close", "关闭"))) {
                        saveState(appDir_);
                        dismiss();
                }
                if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() &&
                    ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                        saveState(appDir_);
                        dismiss();
                        ui::interaction::consumeEscape();
                }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        targetFound_ = false;
}

void
TutorialGuide::loadState(const std::string &appDir)
{
        appDir_ = appDir;
        progress_.fill(0);
        completed_.fill(false);
        docks_.clear();
        lastDock_ = owner_     = 0;
        const auto   &chapters = ui::guide::Chapters();
        std::ifstream progress(appDir + "/tutorial_progress.txt");
        std::string   key;
        int           step = 0, done = 0;
        while (progress >> key >> step >> done)
                for (int i = 0; i < static_cast<int>(chapters.size()); ++i)
                        if (key == chapters[i].key) {
                                progress_[i]  = std::clamp(step, 0, static_cast<int>(chapters[i].steps.size()) - 1);
                                completed_[i] = done == 1;
                        }
        std::ifstream legacy(appDir + "/tutorial_done.txt");
        done = 0;
        if (legacy >> done; done == 1)
                completed_[0] = true;
        dismiss();
}

void
TutorialGuide::saveState(const std::string &appDir)
{
        if (appDir.empty())
                return;
        std::ofstream progress(appDir + "/tutorial_progress.txt", std::ios::trunc);
        const auto   &chapters = ui::guide::Chapters();
        for (int i = 0; i < static_cast<int>(chapters.size()); ++i)
                progress << chapters[i].key << ' ' << progress_[i] << ' ' << completed_[i] << '\n';
        if (completed_[0]) {
                std::ofstream legacy(appDir + "/tutorial_done.txt", std::ios::trunc);
                legacy << '1';
        }
}
