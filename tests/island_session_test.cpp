#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "gui/fault_decoder.hpp"
#include "gui/window_corners.hpp"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"
#include <fstream>
#include <stdexcept>

static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
struct VariableRewriteRegression {
        static void accessorSamples() {
                Variable v("sdk-accessor-samples");
                auto source = std::make_unique<Variable::FunctionSource>();
                check(source->loader.load((std::filesystem::current_path() / "sdk_fixture.dll").string()),
                      "Load synthetic accessor DLL");
                source->declarations = parseHeaderFull("int32_t sdk_tick(uint32_t *p); int32_t sdk_fail(uint32_t *p);");
                auto *library = source.get(); v.functionSources_.push_back(std::move(source));
                for (const char *name : {"valid", "offline"}) {
                        v.addLocalVar(name, DataType::U32, 4); v.markLocalSampleSource(name);
                }
                v.startAccessorThread();
                {
                        std::lock_guard lock(v.accessor_->mtx);
                        v.accessor_->reads = {{"valid", DataType::U32, library->declarations.functions[0], library},
                                             {"offline", DataType::U32, library->declarations.functions[1], library}};
                        v.accessor_->readsPending = true;
                        v.accessor_->intervalUs.store(1000);
                }
                v.accessor_->cv.notify_one();
                size_t received = 0;
                for (int i = 0; i < 100 && received < 3; ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        const auto batches = v.drainLocalSamples();
                        check(!batches.contains("offline"), "Failed function accessors never publish samples");
                        if (auto it = batches.find("valid"); it != batches.end()) {
                                received += it->second.values.size();
                                for (float value : it->second.values) check(value == 1, "Successful accessor reads publish actual values");
                        }
                }
                v.stopAccessorThread();
                check(received >= 3, "SDK scalar accessors keep publishing valid repeated reads");
                v.drainLocalSamples();
                check(v.drainLocalSamples().empty() && v.collectLocalChannelValues().empty(),
                      "Stopped SDK accessors cannot be resampled from cached buffers");
        }
        static void prepare(Variable &v) {
                v.parameterFile_.load(R"({"format":"foc-parameters","version":1,"name":"TEST","target":"g_mc.cfg","parameters":[
                    {"path":"mech.motor.poles","type":"uint32_t","value":"7"},
                    {"path":"hw.pwm.frequency","type":"uint32_t","value":"20000"},
                    {"path":"sw.control.gain","type":"float","value":"1.5"}]})");
                v.parameterFilePath_ = "fixture.json";
                v.parameterDirectory_ = std::filesystem::current_path() / "param_lists";
                v.parameterFileOpen_ = true;
                v.parameterFile_.dirty = true;
                v.parameterComponentTab_ = v.parameterNavigateTab_ = 2;
                v.parameterFile_.document.fields.back().row.val = "unfinished edit";
                v.isModified_ = true;
        }
        static bool restored(const Variable &v) {
                return v.parameterFileOpen_ && v.parameterFile_.dirty && v.parameterComponentTab_ == 2 &&
                       v.parameterFile_.document.fields.back().row.val == "unfinished edit" &&
                       v.parameterDirectory_.filename() == "param_lists";
        }
        static void pin(Variable &v) { v.parameterPresetHover_.setPinned(true); }
};
struct FaultDecoderTestAccess {
        static void prepare(FaultDecoder &decoder) {
                decoder.open = true;
                decoder.profilePath_ = "offline-fixture";
                decoder.nextDetection_ = 1e12;
                for (const char *name : {"Motor", "Sensor", "Encoder"}) {
                        FaultDecoder::Group group;
                        group.id = group.title = name;
                        group.root = std::string("g_mc.") + name;
                        group.manager = "Variable 0";
                        FaultDecoder::GroupRow row;
                        row.title = "State"; row.field.path = group.root + ".state"; row.field.type = "uint32_t";
                        group.rows.push_back(row);
                        decoder.groups_.push_back(std::move(group));
                }
        }
        static void select(FaultDecoder &decoder, size_t index) {
                const auto &group = decoder.groups_.at(index);
                decoder.groupTab_ = group.manager + "\n" + group.id + "\n" + group.root;
                decoder.groupTabSelect_ = true;
        }
        static bool onlyRequested(const FaultDecoder &decoder, size_t index) {
                for (size_t i = 0; i < decoder.groups_.size(); ++i)
                        for (const auto &row : decoder.groups_[i].rows)
                                if (row.requested != (i == index)) return false;
                return true;
        }
        static void directory(FaultDecoder &decoder, const std::string &path) {
                decoder.profileDirectory_ = path;
                decoder.scanProfileDirectory();
        }
        static const auto &files(const FaultDecoder &decoder) { return decoder.profileFiles_; }
        static const auto &directory(const FaultDecoder &decoder) { return decoder.profileDirectory_; }
        static const auto &path(const FaultDecoder &decoder) { return decoder.profilePath_; }
        static const auto &name(const FaultDecoder &decoder) { return decoder.profile_.name; }
        static bool unconfigured(const FaultDecoder &decoder) {
                return decoder.profilePath_.empty() && decoder.profile_.decoders.empty() && decoder.profileError_.empty();
        }
        static bool missing(const FaultDecoder &decoder, const std::string &path) {
                return decoder.profilePath_.empty() && !decoder.profileError_.empty() && path == decoder.profileInput_;
        }
};
struct SimSamplerTestAccess {
        inline static FaultDecoder *safetyPreview = nullptr;
        static bool save(Gui &g, const std::string &path) { return g.saveSession(path); }
        static void load(Gui &g, const std::string &path) { g.loadSession(path); }
        static void prepare(Gui &g) { g.skipAutoSave_ = true; ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable; g.addVariableWindow(); g.showHome_ = false; }
        static bool dirty(Gui &g) { return g.hasSessionChanges(); }
        static void flash(Gui &g) { g.flashTool_.open(); }
        static bool flashOpen(Gui &g) { return g.flashTool_.isOpen(); }
        static void toggleInverter(Gui &g) { g.inverterSimulator_.show_ = !g.inverterSimulator_.show_; }
        static bool inverterOpen(Gui &g) { return g.inverterSimulator_.show_; }
        static void frame(Gui &g) {
                ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ui::interaction::beginFrame(); ImGui::NewFrame();
                g.drawBar(); g.drawWorkspaceChrome();
                g.workspaceDock_ = ui::IslandDockSpace(ImGui::GetMainViewport());
                for (auto &[_, variable] : g.vars_) variable->updateDisplay();
                if (safetyPreview) {
                        ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
                        ImGui::SetNextWindowPos(ImVec2(20, 100));
                        ImGui::SetNextWindowSize(ImVec2(1200, 550));
                        safetyPreview->draw();
                }
                ui::DrawDockCorners(); g.dockSession_.afterWindows();
                if (g.sessionLayoutSettle_ > 0 && --g.sessionLayoutSettle_ == 0) g.sessionLayoutBaseline_ = ImGui::SaveIniSettingsToMemory();
                ImGui::Render();
                int w, h; glfwGetFramebufferSize(glfwGetCurrentContext(), &w, &h);
                glViewport(0, 0, w, h); glClearColor(.08f, .08f, .1f, 1); glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); g.updatePlatformWindows();
        }
};
int main()
{
        module_init(); glfwInit(); glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        try {
                MonitorChannel rate("measured-rate");
                const f32 value = 1;
                f64 time = 0;
                rate.pushBatch(&value, &time, 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(550));
                time = .55;
                rate.pushBatch(&value, &time, 1);
                check(rate.getHz() > 1 && rate.getHz() < 2.2f, "Low-rate batches report real received Hz without waiting for 100 points");
                std::this_thread::sleep_for(std::chrono::milliseconds(1550));
                check(rate.getHz() == 0, "Idle channel rate decays to zero");
                Gui gui; SimSamplerTestAccess::prepare(gui);
                VariableRewriteRegression::accessorSamples();
                {
                        Variable sdk("sample-events");
                        sdk.addLocalVar("SDK0.rt.value", DataType::F32, 4);
                        sdk.addLocalVar("manual", DataType::F32, 4);
                        sdk.markLocalSampleSource("SDK0.rt.value");
                        check(sdk.collectLocalChannelValues().size() == 1, "Unconnected SDK buffers are not resampled as LOCAL values");
                        check(sdk.drainLocalSamples().empty(), "Unconnected SDK has no invented samples");
                        float value = 2.5f;
                        std::vector<uint8_t> bytes(4); memcpy(bytes.data(), &value, 4);
                        const double first = sessionTimeSec();
                        sdk.publishLocalSample("SDK0.rt.value", bytes, first);
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        const double second = sessionTimeSec();
                        sdk.publishLocalSample("SDK0.rt.value", bytes, second);
                        const auto samples = sdk.drainLocalSamples();
                        check(samples.at("SDK0.rt.value").times == std::vector<double>({first,second}), "SDK samples retain read timestamps and repeated valid values");
                        check(samples.at("SDK0.rt.value").values == std::vector<float>({2.5f,2.5f}), "SDK sample bytes decode to monitor values");
                        check(sdk.drainLocalSamples().empty(), "Old SDK data is never replayed on a later frame");
                        sdk.publishLocalSample("SDK0.rt.value", bytes, sessionTimeSec());
                        resetSessionTime();
                        check(sdk.drainLocalSamples().empty(), "Pending SDK samples from before a clear are discarded");
                        auto *saved = cJSON_CreateObject(); sdk.save(saved);
                        Variable restored("sample-events-restored"); restored.load(saved); cJSON_Delete(saved);
                        check(restored.collectLocalChannelValues().size() == 1 && restored.drainLocalSamples().empty(), "Restoring a session never turns cached SDK data into live samples");
                }
                auto variable = gui.getVars().begin()->second;
                VariableRewriteRegression::prepare(*variable);
                for (int i = 0; i < 12; ++i) SimSamplerTestAccess::frame(gui);
                const auto session = (std::filesystem::current_path() / "island.ava").string();
                check(SimSamplerTestAccess::save(gui, session), "Save parameter draft session");
                check(!SimSamplerTestAccess::dirty(gui), "Saving establishes a clean baseline");
                for (int i = 0; i < 5; ++i) SimSamplerTestAccess::frame(gui);
                check(!SimSamplerTestAccess::dirty(gui), "Idle rendering does not mark session dirty");
                SimSamplerTestAccess::toggleInverter(gui);
                check(SimSamplerTestAccess::dirty(gui), "Inverter page visibility participates in exit dirty check");
                check(SimSamplerTestAccess::save(gui, session), "Save inverter visibility");
                SimSamplerTestAccess::flash(gui);
                check(SimSamplerTestAccess::dirty(gui), "Opening a previously closed tool marks session dirty");
                check(SimSamplerTestAccess::save(gui, session), "Save tool visibility");
                faultDecoder().open = !faultDecoder().open;
                check(SimSamplerTestAccess::dirty(gui), "Safety page visibility participates in exit dirty check");
                check(SimSamplerTestAccess::save(gui, session), "Save safety visibility");
                const bool safetyOpen = faultDecoder().open;
                SimSamplerTestAccess::load(gui, session);
                variable = gui.getVars().begin()->second;
                check(VariableRewriteRegression::restored(*variable), "Parameter dock, directory, tab and invalid draft text restore");
                check(SimSamplerTestAccess::flashOpen(gui) && faultDecoder().open == safetyOpen, "Tool dock pages restore open state");
                check(SimSamplerTestAccess::inverterOpen(gui), "Inverter page visibility restores");
                for (int i = 0; i < 12; ++i) SimSamplerTestAccess::frame(gui);
                check(!SimSamplerTestAccess::dirty(gui), "Reload settles into a clean baseline");
                ImGuiWindow *editor = nullptr;
                for (auto *window : ImGui::GetCurrentContext()->Windows)
                        if (window->Active && !(window->Flags & ImGuiWindowFlags_ChildWindow) && std::strstr(window->Name, "###ParameterFileEditor")) editor = window;
                check(editor, "Restored parameter dock is rendered");
                if (editor->DockNode) ImGui::DockBuilderRemoveNodeDockedWindows(editor->DockNode->ID, false);
                ImGui::SetWindowSize(editor, ImVec2(editor->Size.x + 42, editor->Size.y + 25));
                ImGui::SetWindowPos(editor, ImVec2(editor->Pos.x + 21, editor->Pos.y + 13));
                check(SimSamplerTestAccess::dirty(gui), "Moving a dock page is detected as unsaved layout");
                VariableRewriteRegression::pin(*variable);
                for (int i = 0; i < 4; ++i) SimSamplerTestAccess::frame(gui);
                bool preview = false;
                for (auto *window : ImGui::GetCurrentContext()->Windows)
                        if (window->Active && std::strstr(window->Name, "ParameterPresets###")) preview = true;
                check(preview, "Parameter preset controls render in the shared hover panel");
                int w, h; glfwGetFramebufferSize(glfwGetCurrentContext(), &w, &h);
                std::vector<unsigned char> pixels(size_t(w) * h * 3); glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                std::ofstream out("parameter-hover.ppm", std::ios::binary); out << "P6\n" << w << ' ' << h << "\n255\n";
                for (int y = h - 1; y >= 0; --y) out.write(reinterpret_cast<char *>(pixels.data() + size_t(y) * w * 3), w * 3);
                FaultDecoder safety;
                FaultDecoderTestAccess::prepare(safety);
                SimSamplerTestAccess::safetyPreview = &safety;
                for (int i = 0; i < 4; ++i) SimSamplerTestAccess::frame(gui);
                check(FaultDecoderTestAccess::onlyRequested(safety, 0), "Safety only samples the active object tab");
                FaultDecoderTestAccess::select(safety, 1);
                for (int i = 0; i < 4; ++i) SimSamplerTestAccess::frame(gui);
                check(FaultDecoderTestAccess::onlyRequested(safety, 1), "Switching safety tabs updates requested fields");
                SimSamplerTestAccess::safetyPreview = nullptr;
                {
                        const auto utf8 = [](const std::filesystem::path &path) {
                                const auto text = path.u8string(); return std::string(text.begin(), text.end());
                        };
                        const std::string json = R"({"format":"safety","version":1,"name":"User profile","decoders":[{"id":"counter","kind":"number","type":"u32"}],"groups":[]})";
                        const auto directory = std::filesystem::current_path() / std::filesystem::u8path("用户配置 with spaces");
                        const auto chosen = directory / std::filesystem::u8path("子目录/diagnostics.JsOn");
                        const auto invalid = directory / "invalid.json";
                        std::filesystem::create_directories(chosen.parent_path());
                        std::filesystem::create_directories("config");
                        std::ofstream(chosen) << json;
                        std::ofstream(invalid) << "{}";
                        std::ofstream(directory / "ignore.txt") << "not JSON";
                        // Even an old default file beside the app is never implicitly loaded.
                        std::ofstream("config/safety.json") << json;
                        FaultDecoder selected;
                        selected.open = true;
                        selected.inspect(0, 1, 0);
                        selected.detectGroups({});
                        SimSamplerTestAccess::safetyPreview = &selected;
                        for (int i = 0; i < 3; ++i) SimSamplerTestAccess::frame(gui);
                        check(FaultDecoderTestAccess::unconfigured(selected), "Opening safety waits for a user-selected JSON without a missing-default error");
                        auto *emptySession = cJSON_CreateObject(); selected.save(emptySession);
                        selected.load(emptySession); cJSON_Delete(emptySession);
                        check(FaultDecoderTestAccess::unconfigured(selected), "An empty saved path never loads a bundled default");
                        auto *legacySession = cJSON_CreateObject();
                        auto *legacyProfile = cJSON_AddObjectToObject(legacySession, "faultDecoder");
                        cJSON_AddStringToObject(legacyProfile, "profile", "config/safety.json");
                        selected.load(legacySession); cJSON_Delete(legacySession);
                        check(FaultDecoderTestAccess::unconfigured(selected), "An old automatically saved default path is no longer loaded");
                        check(selected.loadProfile("config/safety.json"), "A user can still explicitly choose a file with the old default name");
                        auto *explicitSession = cJSON_CreateObject(); selected.save(explicitSession);
                        selected.load(explicitSession); cJSON_Delete(explicitSession);
                        check(FaultDecoderTestAccess::name(selected) == "User profile", "An explicit selection is preserved even with the old file name");
                        selected.load(nullptr);
                        FaultDecoderTestAccess::directory(selected, utf8(directory));
                        check(FaultDecoderTestAccess::files(selected).size() == 2 && FaultDecoderTestAccess::unconfigured(selected),
                              "User directory lists nested JSON files case-insensitively without loading one automatically");
                        auto *directorySession = cJSON_CreateObject(); selected.save(directorySession);
                        selected.load(directorySession); cJSON_Delete(directorySession);
                        check(FaultDecoderTestAccess::directory(selected) == utf8(directory) && FaultDecoderTestAccess::unconfigured(selected),
                              "A directory-only selection persists without selecting a file");
                        check(selected.loadProfile(utf8(chosen)) && FaultDecoderTestAccess::directory(selected) == utf8(directory),
                              "Loading a nested JSON preserves the selected root directory");
                        check(!selected.loadProfile(utf8(invalid)) && FaultDecoderTestAccess::name(selected) == "User profile" &&
                                  FaultDecoderTestAccess::path(selected) == utf8(chosen),
                              "Invalid user JSON leaves the previous valid configuration available");
                        auto *userSession = cJSON_CreateObject(); selected.save(userSession);
                        const auto *savedProfile = cJSON_GetObjectItemCaseSensitive(userSession, "faultDecoder");
                        check(utf8(chosen) == cJSON_GetObjectItemCaseSensitive(savedProfile, "profile")->valuestring &&
                                  utf8(directory) == cJSON_GetObjectItemCaseSensitive(savedProfile, "directory")->valuestring,
                              "Safety saves the original user file and directory paths");
                        selected.load(userSession);
                        check(FaultDecoderTestAccess::name(selected) == "User profile", "Session restore rereads the chosen JSON");
                        check(!std::filesystem::exists(directory / "safety.json") && !std::filesystem::exists("diagnostics.JsOn"),
                              "Loading and saving never create a private copy of the selected JSON");
                        std::filesystem::remove(chosen);
                        selected.load(userSession);
                        check(FaultDecoderTestAccess::missing(selected, utf8(chosen)),
                              "A missing chosen file reports its original path without using the old default");
                        auto *missingSession = cJSON_CreateObject(); selected.save(missingSession);
                        selected.load(missingSession); cJSON_Delete(missingSession);
                        check(FaultDecoderTestAccess::missing(selected, utf8(chosen)), "A missing user path survives another save and restore");
                        std::ofstream(chosen) << json;
                        check(selected.loadProfile(utf8(chosen)), "The original file can be reloaded once restored");
                        cJSON_Delete(userSession);
                        SimSamplerTestAccess::safetyPreview = nullptr;
                }
                puts("PASS: dirty detection, parameter draft/tab/dock persistence, tool visibility, safety tabs and user-only safety JSON paths");
        } catch (const std::exception &e) { fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
