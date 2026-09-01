#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "gui/interaction_history.hpp"
#include <stdexcept>
static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static void frame(const std::function<void()> &draw)
{
        ui::interaction::beginFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(1100, 800));
        draw(); ui::interaction::process(); ImGui::Render();
}
struct ScopeInteractionTestAccess {
        static void run()
        {
                Monitor monitor("history-monitor"); monitor.addScope("scope");
                auto scope = monitor.getScopes().at("scope"); scope->addChannel("trace");
                auto channel = scope->getChannels().at("trace");
                const float value = 3; const double time = 0;
                channel->pushBatch(&value, &time, 1); channel->publishSnapshot();
                const auto initial = monitor.captureUiState();
                scope->setLabel("renamed");
                monitor.uiHistory_.record(initial, monitor.captureUiState());
                channel->waveEnable_ = true;
                monitor.pendingUiAction_ = ui::interaction::Action::Undo;
                frame([&] { monitor.updateDisplay(); });
                check(scope->getLabel() == "scope" && channel->waveEnable_, "Rename undo preserves unrelated output state");
                monitor.pendingUiAction_ = ui::interaction::Action::Redo;
                frame([&] { monitor.updateDisplay(); });
                check(scope->getLabel() == "renamed", "Rename redo works through panel queue");
                const auto beforeDelete = monitor.captureUiState();
                channel->markPendingDelete();
                monitor.uiHistory_.record(beforeDelete, monitor.captureUiState());
                scope->purgeDeleted();
                check(scope->getChannels().empty(), "Deletion is actually applied");
                monitor.pendingUiAction_ = ui::interaction::Action::Undo;
                frame([&] { monitor.updateDisplay(); });
                check(scope->findChannel("trace") == channel.get() && !channel->isPendingDelete(), "Undo restores the same channel object");
                check(channel->read_.rawSize() == 1 && !channel->waveEnable_, "Undo retains sample history without restarting output");
                monitor.pendingUiAction_ = ui::interaction::Action::Redo;
                frame([&] { monitor.updateDisplay(); });
                check(!scope->findChannel("trace"), "Redo removes the restored channel");
                monitor.toggleScopeFocus("scope");
                monitor.pendingUiAction_ = ui::interaction::Action::Parent;
                frame([&] { monitor.updateDisplay(); });
                check(!monitor.hasFocusedScope(), "Parent exits scope focus");
        }
};
struct VariableRewriteRegression {
        static void run()
        {
                Variable variable("history-parameters");
                variable.parameterFile_.load(R"({"format":"foc-parameters","version":1,"target":"g_foc.cfg","parameters":[{"path":"mode","type":"u32","value":"0"}]})");
                const auto original = variable.captureParameterDraft();
                variable.parameterFile_.document.fields[0].row.val = "1";
                variable.parameterHistory_.record(original, variable.captureParameterDraft());
                variable.parameterFileOpen_ = true;
                variable.parameterAction_ = ui::interaction::Action::Undo;
                frame([&] { variable.drawParameterFileEditor(); });
                check(variable.parameterFile_.document.fields[0].row.val == "0" && variable.parameterFile_.dirty,
                      "Undo restores a local parameter draft and keeps it unsaved");
                variable.parameterAction_ = ui::interaction::Action::Redo;
                frame([&] { variable.drawParameterFileEditor(); });
                check(variable.parameterFile_.document.fields[0].row.val == "1", "Parameter redo restores draft value");
                variable.setParameterTarget("another.cfg");
                check(!variable.parameterHistory_.canUndo() && !variable.parameterHistory_.canRedo(),
                      "Switching device target invalidates old draft history");
        }
};
int main()
{
        module_init(); ImGui::CreateContext(); ImPlot::CreateContext();
        ImGui::GetIO().IniFilename = nullptr; ImGui::GetIO().DisplaySize = ImVec2(1200, 900);
        unsigned char *pixels; int width, height; ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        int result = 0;
        try { ScopeInteractionTestAccess::run(); ui::interaction::reset(); VariableRewriteRegression::run();
              puts("PASS: real panel undo/redo, sample retention, no output restart, parent navigation, parameter target boundary"); }
        catch (const std::exception &e) { fprintf(stderr, "FAIL: %s\n", e.what()); result = 1; }
        ui::interaction::reset(); ImPlot::DestroyContext(); ImGui::DestroyContext();
        return result;
}
