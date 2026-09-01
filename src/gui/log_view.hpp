#ifndef AVA_LOG_VIEW_HPP
#define AVA_LOG_VIEW_HPP

#include "gui/i18n.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace ui
{
struct LogColorRun {
        size_t begin, end;
        ImU32  color;
};

// One text buffer makes selections and clipboard contents include timestamps,
// values and line breaks. Color runs retain success/failure highlighting
struct LogText {
        std::string              text;
        std::vector<LogColorRun> colors;

        void append(std::string_view val, ImU32 color = ImGui::GetColorU32(ImGuiCol_Text))
        {
                const size_t begin = text.size();
                text.append(val);
                if (!val.empty())
                        colors.push_back({begin, text.size(), color});
        }

        void timestamp(uint64_t milliseconds)
        {
                const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1000);
                std::tm           local{};
#ifdef _WIN32
                localtime_s(&local, &seconds);
#else
                localtime_r(&seconds, &local);
#endif
                char val[40];
                std::snprintf(val,
                              sizeof(val),
                              "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
                              local.tm_year + 1900,
                              local.tm_mon + 1,
                              local.tm_mday,
                              local.tm_hour,
                              local.tm_min,
                              local.tm_sec,
                              static_cast<int>(milliseconds % 1000));
                append(val, ImGui::GetColorU32(ImGuiCol_TextDisabled));
        }
};

inline void
CopyLogButton(const std::string &text)
{
        if (ImGui::SmallButton(tr("Copy all", "复制全部")))
                ImGui::SetClipboardText(text.c_str());
}

inline void
LogView(
    const char *label, std::string &text, ImVec2 size, bool *follow = nullptr, const std::vector<LogColorRun> *colors = nullptr)
{
        const ImGuiID id      = ImGui::GetID(label);
        const ImVec2  origin  = ImGui::GetCursorScreenPos();
        const bool    colored = colors && !colors->empty();
        if (colored)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::InputTextMultiline(label, text.data(), text.size() + 1, size, ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor();
        if (colored)
                ImGui::PopStyleColor();
        const bool  focused = ImGui::IsItemActive();
        const auto *state   = ImGui::GetInputTextState(id);
        for (auto *window : ImGui::GetCurrentWindow()->DC.ChildWindows) {
                if (window->ChildId != id || !window->Active)
                        continue;
                if (colored) {
                        const bool  scrolling = ImGui::GetActiveID() == ImGui::GetWindowScrollbarID(window, ImGuiAxis_Y);
                        const float scrollX   = state && (focused || scrolling) ? state->Scroll.x : 0.0f;
                        ImVec2      pen(origin.x + ImGui::GetStyle().FramePadding.x - scrollX,
                                   origin.y + ImGui::GetStyle().FramePadding.y - window->Scroll.y);
                        const float startX     = pen.x;
                        const float lineHeight = ImGui::GetTextLineHeight();
                        window->DrawList->PushClipRect(window->ClipRect.Min, window->ClipRect.Max, true);
                        for (const auto &run : *colors) {
                                const char *begin = text.data() + run.begin;
                                const char *end   = text.data() + run.end;
                                while (begin < end) {
                                        const char *newline = std::find(begin, end, '\n');
                                        if (pen.y + lineHeight >= window->ClipRect.Min.y && pen.y < window->ClipRect.Max.y)
                                                window->DrawList->AddText(pen, run.color, begin, newline);
                                        pen.x += ImGui::CalcTextSize(begin, newline).x;
                                        if (newline == end)
                                                break;
                                        pen.x  = startX;
                                        pen.y += lineHeight;
                                        begin  = newline + 1;
                                }
                        }
                        window->DrawList->PopClipRect();
                }
                if (follow) {
                        const bool    hovered   = ImGui::GetCurrentContext()->HoveredWindow == window;
                        const float   scrollY   = window->Scroll.y;
                        const bool    atBottom  = scrollY >= window->ScrollMax.y - 1.0f;
                        const ImGuiID scrollKey = ImHashStr("log_previous_scroll");
                        const float   previous  = window->StateStorage.GetFloat(scrollKey, 0.0f);
                        const bool    dragging =
                            hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && std::abs(scrollY - previous) > 0.5f;
                        const bool navigatingUp =
                            focused && (ImGui::IsKeyPressed(ImGuiKey_PageUp) || ImGui::IsKeyPressed(ImGuiKey_Home) ||
                                        ImGui::IsKeyPressed(ImGuiKey_UpArrow));
                        const bool selecting = focused && state && state->HasSelection();
                        if ((hovered && ImGui::GetIO().MouseWheel > 0) || (dragging && !atBottom) || navigatingUp ||
                            selecting || (focused && ImGui::IsMouseDown(ImGuiMouseButton_Left)))
                                *follow = false;
                        else if (!*follow && atBottom)
                                *follow = true;
                        // Use this frame's content extent; oversized sentinel
                        // targets overflow ImGui's pixel rounding
                        if (*follow)
                                ImGui::SetScrollY(window, window->DC.CursorMaxPos.y - window->Pos.y + window->Scroll.y);
                        window->StateStorage.SetFloat(scrollKey, scrollY);
                }
                break;
        }
}

inline void
LogView(const char *label, LogText &log, ImVec2 size, bool *follow = nullptr)
{
        LogView(label, log.text, size, follow, &log.colors);
}
} // namespace ui
#endif
