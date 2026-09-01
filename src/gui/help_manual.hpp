#ifndef AVA_HELP_MANUAL_HPP
#define AVA_HELP_MANUAL_HPP
#include "gui/i18n.hpp"
#include "gui/platform_style.hpp"
#include "imgui.h"

namespace ui
{
inline bool shortcutManualOpen = false;

inline void
DrawShortcutManual()
{
        if (!shortcutManualOpen)
                return;
        ImGui::SetNextWindowSize(ImVec2(620, 540), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(tr("Keyboard shortcuts###ShortcutManual", "快捷键手册###ShortcutManual"), &shortcutManualOpen)) {
                ImGui::TextWrapped(
                    "%s",
                    tr("Global shortcuts are available when no text editor, active control or popup is using the keyboard.",
                       "全局快捷键在未编辑文本、未操作控件且未打开弹出菜单时生效。"));
                auto row = [](const char *key, const char *description) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(key);
                        ImGui::TableNextColumn();
                        ImGui::TextWrapped("%s", description);
                };
                if (ImGui::BeginTable("Shortcuts", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                        ImGui::TableSetupColumn(
                            tr("Keys", "按键"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 12);
                        ImGui::TableSetupColumn(tr("Action / context", "功能／使用位置"));
                        ImGui::TableHeadersRow();
                        row("Alt+Left / Alt+Right",
                            tr("Back / forward between views; mouse side buttons also work",
                               "视图后退／前进；也可使用鼠标侧键"));
                        row("Alt+Up / Esc",
                            tr("Return to parent / exit one level; popups and editors handle Esc first",
                               "返回上一级／退出当前层；Esc 优先关闭弹窗或取消编辑"));
                        row(ShortcutLabel("Ctrl+Z", "Cmd+Z"),
                            tr("Undo a local edit in the current panel", "撤销当前面板的本地修改"));
                        row(ShortcutLabel("Ctrl+Y / Ctrl+Shift+Z", "Cmd+Shift+Z"), tr("Redo a local edit", "重做本地修改"));
                        row(ShortcutLabel("Ctrl+O", "Cmd+O"), tr("Open a session", "打开会话"));
                        row(ShortcutLabel("Ctrl+S", "Cmd+S"), tr("Save the session", "保存会话"));
                        row(ShortcutLabel("Ctrl+Shift+S", "Cmd+Shift+S"), tr("Save the session as...", "会话另存为"));
                        row(ShortcutLabel("Ctrl+K", "Cmd+K"), tr("Find a command", "查找功能"));
                        row(ShortcutLabel("Ctrl+L", "Cmd+L"),
                            tr("Connect J-Link with the selected settings", "按当前配置连接 J-Link"));
                        row(ShortcutLabel("Ctrl+Shift+D", "Cmd+Shift+D"), tr("Disconnect J-Link", "断开 J-Link"));
                        row("Space", tr("Freeze / resume display; acquisition continues", "冻结／恢复画面，后台继续采集"));
                        row(ShortcutLabel("Ctrl+Shift+Delete", "Cmd+Shift+Delete"),
                            tr("Immediately clear recorded data in all monitors", "立即清空所有监视器已采集的数据"));
                        row("Up / Down / Enter / Esc",
                            tr("Command search: select, execute, close", "查找功能：选择、执行、关闭"));
                        row(ShortcutLabel("Ctrl+A / C / V", "Cmd+A / C / V"),
                            tr("Focused text editor: select all, copy, paste; read-only logs support select and copy",
                               "文本框内：全选、复制、粘贴；只读日志支持选择和复制"));
                        row("F2", tr("Rename a focused variable or monitor tab", "重命名已聚焦的变量或监视器标签"));
                        row("F1", tr("Open the focused panel's guide", "打开当前面板的功能与按钮引导"));
                        ImGui::EndTable();
                }
                ImGui::TextWrapped("%s",
                                   tr("Panel undo covers scope/channel names, order, deletion and parameter field edits. Input "
                                      "editors keep their own undo. Device writes, resets and firmware operations are not "
                                      "undone; parameter read/write or target changes clear draft history.",
                                      "面板撤销支持示波器／通道改名、排序、删除和参数字段编辑。输入框优先使用自己的撤销。设备写"
                                      "入、复位和固件操作不撤销；读取、下发参数或切换目标会清空参数撤销记录。"));
                ImGui::SeparatorText(tr("Mouse operations", "鼠标操作"));
                ImGui::BulletText(
                    "%s",
                    tr("Click J-Link or display actions to open settings; click outside or click again to dismiss.",
                       "点击 J-Link 或画面操作打开设置，点击弹窗外部或再次点击入口关闭。"));
                ImGui::BulletText("%s",
                                  tr("Double-click a symbol to watch it; drag it onto a monitor to plot it.",
                                     "双击符号加入变量列表；拖到监视器中绘制波形。"));
                ImGui::BulletText("%s",
                                  tr("Drag a window tab to dock or detach it; drag dividers to resize panes.",
                                     "拖动窗口标签可停靠或悬浮；拖动分隔线调整面板大小。"));
                ImGui::BulletText("%s",
                                  tr("Select log text to copy it; scroll up to pause automatic following.",
                                     "日志可拖选复制；向上滚动会暂停自动跟随。"));
                ImGui::SeparatorText(tr("Connection and acquisition", "连接与采集"));
                ImGui::TextWrapped(
                    "%s",
                    tr("Open J-Link in the workspace toolbar for connection and acquisition settings. "
                       "Space freezes/resumes the display while acquisition continues. "
                       "Ctrl/Cmd+Shift+Delete immediately clears recorded data.",
                       "点击工作区工具栏的 J-Link，可选择连接并复位、复位 MCU "
                       "和采集设置。空格冻结画面（后台继续采集），Ctrl/Cmd+Shift+Delete 可快速清空已采集数据。"));
        }
        ImGui::End();
}
} // namespace ui
#endif
