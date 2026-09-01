#define main    ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef main
#undef WinMain
#include "gui/dock_close.hpp"
#include "gui/island_style.hpp"
#include "gui/tutorial_guide.hpp"
#include "imgui_internal.h"
#include <fstream>
#include <set>
#include <stdexcept>

static void
check(bool ok, const char *message)
{
        if (!ok)
                throw std::runtime_error(message);
}
using Topic                     = TutorialGuide::Topic;
static int         actionCount  = 0;
static Topic       current      = Topic::Variables;
static bool        panelVisible = true, focusPanel = true, modal = false;
static const char *panelName = "Renamed panel###guide-test-owner";

static void
frame()
{
        ui::interaction::beginFrame();
        ImGui::NewFrame();
        if (panelVisible) {
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, 230));
                if (focusPanel) {
                        ImGui::SetNextWindowFocus();
                        focusPanel = false;
                }
                ImGui::Begin(panelName);
                TutorialGuide::instance().markDock(current);
                if (ImGui::Button("Device action"))
                        ++actionCount;
                ImGui::End();
        }
        if (modal) {
                ImGui::OpenPopup("Device confirmation");
                if (ImGui::BeginPopupModal("Device confirmation")) {
                        ImGui::TextUnformatted("Confirm device operation");
                        ImGui::EndPopup();
                }
        }
        TutorialGuide::instance().draw();
        ImGui::Render();
}

static void
key(ImGuiKey key)
{
        ImGui::GetIO().AddKeyEvent(key, true);
        frame();
        ImGui::GetIO().AddKeyEvent(key, false);
        frame();
}

static void
click(const char *label)
{
        auto      &guide  = TutorialGuide::instance();
        const auto id     = "##tutorial_step_" + std::to_string(guide.currentStep());
        auto      *window = ImGui::FindWindowByName(id.c_str());
        check(window && window->Active, "guide window is active before navigation");
        ImGui::ActivateItemByID(window->GetID(label));
        frame();
        frame();
}

int
main(int, char **)
{
        module_init();
        ImGui::CreateContext();
        auto &io                        = ImGui::GetIO();
        io.IniFilename                  = nullptr;
        io.DisplaySize                  = ImVec2(1200, 800);
        io.DeltaTime                    = 1.0f / 60;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char *pixels;
        int            width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        const auto oldLanguage = g_lang;
        g_lang                 = Lang::EN;
        auto &guide            = TutorialGuide::instance();
        int   result           = 0;
        try {
                std::set<std::string> keys;
                const auto           &chapters = ui::guide::Chapters();
                check(chapters.size() >= 22, "all tool families have a guide chapter");
                for (const auto &chapter : chapters) {
                        check(keys.insert(chapter.key).second && chapter.steps.size() >= 2,
                              "chapters have distinct persistence keys and concrete steps");
                        check(chapter.titleEn[0] && chapter.titleZh[0] && chapter.locationEn[0] && chapter.locationZh[0],
                              "chapter names and opening locations are bilingual");
                        for (const auto &step : chapter.steps)
                                check(step.target[0] && step.titleEn[0] && step.titleZh[0] && step.bodyEn[0] && step.bodyZh[0],
                                      "every step explains a real function in both languages");
                }
                const auto profile = std::filesystem::current_path().string();
                guide.loadState(profile);
                frame();
                frame();
                check(!guide.isActive(), "loading guide progress does not cover the workspace automatically");
                for (auto topic :
                     {Topic::Variables, Topic::Recorder, Topic::Library, Topic::Parameters, Topic::Safety, Topic::DSO}) {
                        current    = topic;
                        focusPanel = true;
                        frame();
                        frame();
                        key(ImGuiKey_F1);
                        check(guide.isActive() && guide.topic() == topic,
                              "F1 uses the actual focused dock, including custom names");
                        guide.dismiss();
                        frame();
                }
                guide.start(Topic::Recorder);
                frame();
                frame();
                click("Next");
                click("Next");
                check(guide.currentStep() == 2, "Next advances through panel button descriptions");
                click("Previous");
                check(guide.currentStep() == 1, "Previous revisits the previous function");
                guide.saveState(profile);
                guide.dismiss();
                guide.loadState(profile);
                guide.start(Topic::Recorder, true);
                check(guide.currentStep() == 1, "chapter progress resumes after a fresh load");
                frame();
                frame();
                while (guide.currentStep() + 1 < static_cast<int>(chapters[static_cast<int>(Topic::Recorder)].steps.size()))
                        click("Next");
                click("Finish");
                check(!guide.isActive() && guide.completed(Topic::Recorder), "Finish completes only the current chapter");
                guide.loadState(profile);
                check(guide.completed(Topic::Recorder) && !guide.completed(Topic::Safety),
                      "chapter completion is persisted independently");

                for (bool dark : {false, true})
                        for (auto language : {Lang::EN, Lang::ZH}) {
                                g_lang = language;
                                ui::ApplyAppStyle(ui::ColorScheme::Ocean, 1, false, dark);
                                io.DisplaySize = ImVec2(480, 360);
                                guide.start(Topic::Parameters);
                                frame();
                                frame();
                                auto *window = ImGui::FindWindowByName("##tutorial_step_0");
                                check(window->Pos.x >= 0 && window->Pos.y >= 0 && window->Pos.x + window->Size.x <= 480 &&
                                          window->Pos.y + window->Size.y <= 360,
                                      "translated guide fits small viewports in both themes");
                                check(window->DC.CursorMaxPos.x <= window->InnerRect.Max.x + 1 &&
                                          window->DC.CursorMaxPos.y <= window->InnerRect.Max.y + 1,
                                      "guide navigation remains inside the panel while long text scrolls");
                                guide.openCatalog();
                                frame();
                                frame();
                                auto *catalog = ImGui::FindWindowByName("Panel guides###PanelGuide");
                                check(catalog && catalog->Active && catalog->Size.x <= 480 && catalog->Size.y <= 360,
                                      "chapter library adapts to a narrow viewport");
                                check(catalog->Pos.x >= 0 && catalog->Pos.y >= 0 && catalog->Pos.x + catalog->Size.x <= 480 &&
                                          catalog->Pos.y + catalog->Size.y <= 360,
                                      "chapter library position also stays inside a resized viewport");
                                guide.dismiss();
                                frame();
                        }
                g_lang         = Lang::EN;
                io.DisplaySize = ImVec2(1200, 800);
                guide.start(Topic::Safety);
                frame();
                frame();
                modal = true;
                frame();
                frame();
                check(!ImGui::FindWindowByName("##tutorial_step_0")->Active,
                      "a device confirmation has priority over the tutorial overlay");
                modal = false;
                ImGui::ClosePopupsOverWindow(nullptr, false);
                frame();
                guide.dismiss();
                panelVisible = false;
                frame();
                frame();
                frame();
                key(ImGuiKey_F1);
                check(guide.catalogOpen() && !guide.isActive(), "F1 never targets a closed or stale dock");
                check(actionCount == 0, "reading and navigating guides never executes device controls");
                guide.dismiss();
                puts("PASS: 22 bilingual chapters, context F1, navigation/resume/completion, narrow layouts and modal "
                     "priority; no device actions");
        } catch (const std::exception &e) {
                fprintf(stderr, "FAIL: %s\n", e.what());
                result = 1;
        }
        guide.dismiss();
        g_lang = oldLanguage;
        ImGui::DestroyContext();
        log_deinit(&g_log_ops);
        log_deinit(&g_log_perf);
        return result;
}
