#ifndef AVA_WAVE_INPUT_HPP
#define AVA_WAVE_INPUT_HPP

#include "imgui.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

struct WaveInputDraft {
        char text[64]{};
        bool active{};
};

/**
 * @brief Keep editing separate from the configuration used by the sampler
 */
inline bool
inputWaveFloatOnEnter(const char *label, float &val, WaveInputDraft &draft, const char *format, bool allowUnchanged = false)
{
        if (!draft.active)
                std::snprintf(draft.text, sizeof(draft.text), format, val);
        const bool enter = ImGui::InputText(
            label, draft.text, sizeof(draft.text), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        draft.active = ImGui::IsItemActive();
        if (!enter)
                return false;
        char       *end    = nullptr;
        const float parsed = std::strtof(draft.text, &end);
        if (end == draft.text || !end)
                return false;
        while (end && (*end == ' ' || *end == '\t'))
                ++end;
        if (*end || !std::isfinite(parsed) || (!allowUnchanged && parsed == val))
                return false;
        val = parsed;
        return true;
}

#endif
