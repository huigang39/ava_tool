/**
 * @file  i18n.hpp
 * @brief Minimal inline-translation helper for English / Chinese UI text.
 *
 * Usage — write both translations at the call site:
 *     ImGui::Button(tr("Export CSV", "导出 CSV"));
 *     ImGui::Text("%s", tr("No data", "无数据"));
 *
 * Switching language is just flipping g_lang; every tr() call returns the
 * matching string on the next frame. The font atlas is built once at startup
 * with Chinese glyph ranges, so no font rebuild is needed on toggle.
 *
 * g_lang is only read/written on the UI (main) thread, so a plain global is
 * sufficient — no synchronisation required.
 */
#ifndef I18N_HPP
#define I18N_HPP

enum class Lang { EN, ZH };

// Active UI language. Defined inline (C++17) so every TU shares one instance.
inline Lang g_lang = Lang::EN;

// Returns the string for the active language. The English text doubles as the
// stable key, so untranslated call sites can simply pass the same string twice
// (or be left as plain literals) and still render correctly in English.
inline const char *
tr(const char *en, const char *zh)
{
        return g_lang == Lang::ZH ? zh : en;
}

#endif // !I18N_HPP
