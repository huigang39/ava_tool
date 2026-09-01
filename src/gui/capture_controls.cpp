#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif
#include "ImGuiNotify.hpp"
#include "gui/gui.hpp"
#include "gui/ui_icons.hpp"
#include "platform/native_dlg.hpp"
#include <ctime>
#include <filesystem>

void
Gui::drawCaptureControls(bool compact)
{
        if (auto result = recorder_.takeResult()) {
                ImGui::InsertNotification(
                    {result->ok ? ImGuiToastType::Success : ImGuiToastType::Error,
                     7000,
                     "%s\n%s",
                     result->ok ? tr("Recording saved", "录屏已保存") : tr("Recording failed", "录屏失败"),
                     result->ok ? result->path.c_str() : result->error.c_str()});
        }
#ifdef _WIN32
        using State       = app_capture::Recorder::State;
        const auto state  = recorder_.state();
        const bool active = state == State::Starting || state == State::Recording;
        char       label[96];
        if (active) {
                const int seconds = int(recorder_.elapsed());
                snprintf(label, sizeof(label), "%s %02d:%02d###Record", tr("Stop", "停止"), seconds / 60, seconds % 60);
        } else {
                snprintf(label,
                         sizeof(label),
                         "%s###Record",
                         state == State::Finishing ? tr("Saving...", "保存中...")
                         : compact                 ? ""
                                                   : tr("Record", "录屏"));
        }
        const auto &style = ImGui::GetStyle();
        const float right = ImGui::GetWindowContentRegionMax().x;
        auto        width = [&](const char *text) {
                const float textWidth = ImGui::CalcTextSize(text, nullptr, true).x;
                return ImGui::GetFontSize() * 1.1f + style.FramePadding.x * 2 + textWidth +
                       (textWidth > 0 ? style.ItemInnerSpacing.x + 1 : 0);
        };
        const char *screenshotLabel = tr("Screenshot###Screenshot", "截屏###Screenshot");
        if (width(screenshotLabel) + width(label) + style.ItemSpacing.x > right - ImGui::GetCursorPosX())
                compact = true;
        if (compact) {
                screenshotLabel = "###Screenshot";
                if (!active && state != State::Finishing)
                        snprintf(label, sizeof(label), "###Record");
        }
        const float groupWidth = width(screenshotLabel) + width(label) + style.ItemSpacing.x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - groupWidth));
        const ImVec2 buttonSize(0, 0);
        if (ui::IconButton(screenshotLabel, ui::Icon::Camera, ui::BtnStyle::Neutral, buttonSize))
                screenshotRequested_ = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("%s",
                                  tr("Copy the main window image to the clipboard", "截取上位机主窗口，直接复制到剪贴板"));
        ImGui::SameLine();
        ImGui::BeginDisabled(state == State::Finishing);
        if (ui::IconButton(label,
                           active ? ui::Icon::Stop : ui::Icon::Record,
                           active ? ui::BtnStyle::Danger : ui::BtnStyle::Neutral,
                           buttonSize)) {
                if (active)
                        recorder_.stop();
                else {
                        const auto now = std::time(nullptr);
                        std::tm    local{};
                        localtime_s(&local, &now);
                        char filename[80];
                        std::strftime(filename, sizeof(filename), "AvA-test-%Y%m%d-%H%M%S.mp4", &local);
                        auto path = nativeDlgSave(tr("Save recording", "保存录屏"), {{"MP4 Video", {"mp4"}}}, filename);
                        if (!path.empty()) {
                                if (std::filesystem::u8path(path).extension().empty())
                                        path += ".mp4";
                                int width{}, height{};
                                if (window_)
                                        glfwGetFramebufferSize(window_, &width, &height);
                                if (!recorder_.start(path, width, height))
                                        ImGui::InsertNotification(
                                            {ImGuiToastType::Error, 5000, "%s", tr("Cannot start recording", "无法开始录屏")});
                        }
                }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(
                    "%s",
                    tr("Record the main window, up to 1080p / 30 fps, without audio. Stop to save MP4.\nDetached windows are "
                       "outside the recording.",
                       "录制上位机主窗口，最高 1080p / 30 fps，无音频；停止后保存 MP4。\n独立拖出的窗口不在录制范围内。"));
#endif
}

void
Gui::captureRenderedFrame(int width, int height)
{
        const auto now   = app_capture::nowUs();
        const bool video = recorder_.wantsFrame(now);
        if ((!screenshotRequested_ && !video) || width <= 0 || height <= 0 || width > 16384 || height > 16384)
                return;
        try {
                app_capture::Frame frame{width, height, std::vector<uint8_t>(size_t(width) * height * 4), now};
                GLint              alignment{}, readBuffer{}, rowLength{}, skipRows{}, skipPixels{};
                glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
                glGetIntegerv(GL_READ_BUFFER, &readBuffer);
                glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
                glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
                glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glPixelStorei(GL_PACK_ROW_LENGTH, 0);
                glPixelStorei(GL_PACK_SKIP_ROWS, 0);
                glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
                glReadBuffer(GL_BACK);
                glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, frame.rgba.data());
                glReadBuffer(readBuffer);
                glPixelStorei(GL_PACK_ALIGNMENT, alignment);
                glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
                glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
                glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
                if (screenshotRequested_) {
                        screenshotRequested_ = false;
                        std::string error;
#ifdef _WIN32
                        void *owner = glfwGetWin32Window(window_);
#else
                        void *owner = nullptr;
#endif
                        const bool ok = app_capture::copyToClipboard(frame, owner, error);
                        ImGui::InsertNotification({ok ? ImGuiToastType::Success : ImGuiToastType::Error,
                                                   4500,
                                                   "%s%s%s",
                                                   ok ? tr("Screenshot copied to clipboard", "截屏已复制到剪贴板")
                                                      : tr("Screenshot failed", "截屏失败"),
                                                   ok ? "" : "\n",
                                                   ok ? "" : error.c_str()});
                }
                if (video)
                        recorder_.submit(std::move(frame));
        } catch (const std::exception &e) {
                screenshotRequested_ = false;
                recorder_.stop();
                ImGui::InsertNotification(
                    {ImGuiToastType::Error, 5000, "%s: %s", tr("Capture failed", "画面捕获失败"), e.what()});
        }
}
