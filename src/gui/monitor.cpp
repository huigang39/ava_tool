#include "core/sample_statistics.hpp"
#include "gui/choice_control.hpp"
#include "gui/dock_close.hpp"
#include "gui/hover_panel.hpp"
#include "gui/table_hierarchy.hpp"
#include "gui/table_island.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

#include "gui/plot_fit.hpp"
#include "gui/scope_layout.hpp"
#include "gui/search_match.hpp"
#include "gui/search_results.hpp"
#include "gui/table_text.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "implot.h"
#include "implot_internal.h"

#include "gui/i18n.hpp"
#include "gui/monitor.hpp"
#include "gui/numeric_format.hpp"
#include "gui/tutorial_guide.hpp"
#include "gui/ui_icons.hpp"
#include "gui/ui_theme.hpp"
#include "gui/unit_conversion_ui.hpp"
#include "gui/value_clipboard.hpp"
#include "platform/native_dlg.hpp"

std::atomic<bool> g_monitorPaused{false};
std::atomic<bool> g_jlinkSamplingPaused{false};
std::atomic<int>  g_maxHssHz{1000};

std::vector<Monitor *> Monitor::sInstances_;
std::mutex             Monitor::sMtxInstances_;

u64
Monitor::getGlobalPointCount()
{
        std::lock_guard lock(sMtxInstances_);
        u64             total = 0;
        for (const auto *monitor : sInstances_)
                for (const auto &[_, scope] : monitor->scopes_)
                        if (scope)
                                for (const auto &[__, channel] : scope->getChannels())
                                        if (channel)
                                                total += channel->arraySnapshot_ ? channel->arraySnapshot_->values.size()
                                                                                 : channel->read_.rawSize();
        return total;
}

static std::string sMonitorFunctionBindingRequest;

void
requestMonitorFunctionBinding(const std::string &variableName)
{
        sMonitorFunctionBindingRequest = variableName;
}

std::string
consumeMonitorFunctionBindingRequest()
{
        std::string result;
        result.swap(sMonitorFunctionBindingRequest);
        return result;
}

class MonitorChannel;
class MonitorScope;

namespace
{
void
tableNumber(double value, int precision = 6)
{
        char text[384];
        snprintf(text, sizeof(text), "%.*f", precision, value);
        ui::TableText(text);
}

float
statisticsTableHeight(size_t dataRows)
{
        return (dataRows + 1) * (ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2) +
               ImGui::GetStyle().ScrollbarSize + 2;
}

float
statisticsMetricWidth()
{
        float width = 0;
        for (const auto *label : {tr("Current", "当前值"),
                                  tr("Min", "最小值"),
                                  tr("Max", "最大值"),
                                  tr("Pk-Pk", "峰峰值"),
                                  tr("Mean", "平均值"),
                                  tr("RMS", "有效值"),
                                  tr("Std dev", "标准差"),
                                  tr("Variance", "方差"),
                                  tr("Median", "中位数"),
                                  "P95",
                                  "N"})
                width = std::max(width, ui::TableColumnWidth(label));
        return width;
}

void
ensureStatisticsMetricWidth()
{
        if (const auto *table = ImGui::GetCurrentTable(); table && table->MinColumnWidth > 0 &&
                                                          table->Columns[0].WidthRequest > 0 &&
                                                          table->Columns[0].WidthRequest < statisticsMetricWidth())
                ImGui::TableSetColumnWidth(0, statisticsMetricWidth());
}

void
drawTimeMeasurementControls(MonitorTimeMeasurement &measurement)
{
        if (ImGui::Checkbox(tr("Measure time", "测量时间"), &measurement.enabled))
                measurement.count = 0;
        if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                                  tr("Click A/B in any time-domain scopes of this monitor, then drag either cursor. "
                                     "Hover a curve to snap to a sample. Middle-drag "
                                     "pans while measuring. Frequency is 1/|B - A|; select one full period.",
                                     "可在本 Monitor 不同的时域 Scope 中依次选择 A/B 点，并在任意图中拖动游标；"
                                     "悬停曲线可吸附采样点。测量时用鼠标中键平移。"
                                     "频率按 1/|B - A| 计算，测量周期频率时请选择一个完整周期。"));
        if (measurement.enabled && measurement.count > 0) {
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Clear##timeMeasure", "清除##timeMeasure")))
                        measurement.count = 0;
        }
}

// Self-contained copy of one ch's series for a background CSV write
struct CsvExportChannel {
        std::string      tag; // "<scope>::<ch>"
        std::vector<f64> ts;
        std::vector<f64> val;
};

std::string
humanSize(u64 bytes)
{
        const char *units[] = {"B", "KB", "MB", "GB", "TB"};
        f64         v       = static_cast<f64>(bytes);
        int         u       = 0;
        while (v >= 1024.0 && u < 4) {
                v /= 1024.0;
                ++u;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f %s", v, units[u]);
        return buf;
}

int
formatTimeOfDay(double val, char *buffer, int size, void *data)
{
        if (!std::isfinite(val) || !buffer || size <= 0)
                return 0;
        if (data)
                val += *static_cast<const double *>(data);
        if (!std::isfinite(val) || std::abs(val) >= static_cast<double>(std::numeric_limits<i64>::max()) / 1000000.0)
                return snprintf(buffer, static_cast<size_t>(size), "--");

        constexpr i64 microsPerSecond = 1000000;
        constexpr i64 microsPerMinute = 60 * microsPerSecond;
        constexpr i64 microsPerHour   = 60 * microsPerMinute;
        constexpr i64 microsPerDay    = 24 * microsPerHour;
        i64           totalMicros     = static_cast<i64>(std::llround(val * static_cast<double>(microsPerSecond)));
        const bool    negative        = totalMicros < 0;
        if (negative)
                totalMicros = -totalMicros;

        const i64 micros = totalMicros % microsPerSecond;

        // Unix tss are shown with their local calendar date. Keeping the
        // date in every label makes a midnight/day boundary explicit
        if (val >= 1.0e8) {
                const std::time_t whole = static_cast<std::time_t>(totalMicros / microsPerSecond);
                std::tm           local{};
#ifdef _WIN32
                const bool validLocalTime = localtime_s(&local, &whole) == 0;
#else
                const bool validLocalTime = localtime_r(&whole, &local) != nullptr;
#endif
                if (validLocalTime)
                        return snprintf(buffer,
                                        static_cast<size_t>(size),
                                        "%04d-%02d-%02d %02d:%02d:%02d.%06lld",
                                        local.tm_year + 1900,
                                        local.tm_mon + 1,
                                        local.tm_mday,
                                        local.tm_hour,
                                        local.tm_min,
                                        local.tm_sec,
                                        static_cast<long long>(micros));
        }

        // Non-epoch tss are durations. Once they pass 24 hours, show an
        // explicit day counter instead of folding silently back to 00:00
        const i64 days     = totalMicros / microsPerDay;
        totalMicros       %= microsPerDay;
        const i64 hours    = totalMicros / microsPerHour;
        totalMicros       %= microsPerHour;
        const i64 minutes  = totalMicros / microsPerMinute;
        totalMicros       %= microsPerMinute;
        const i64 seconds  = totalMicros / microsPerSecond;

        if (days > 0)
                return snprintf(buffer,
                                static_cast<size_t>(size),
                                "%sD+%lld %02lld:%02lld:%02lld.%06lld",
                                negative ? "-" : "",
                                static_cast<long long>(days),
                                static_cast<long long>(hours),
                                static_cast<long long>(minutes),
                                static_cast<long long>(seconds),
                                static_cast<long long>(micros));
        return snprintf(buffer,
                        static_cast<size_t>(size),
                        "%s%02lld:%02lld:%02lld.%06lld",
                        negative ? "-" : "",
                        static_cast<long long>(hours),
                        static_cast<long long>(minutes),
                        static_cast<long long>(seconds),
                        static_cast<long long>(micros));
}
} // namespace

// --------------------------------------------------------------------------

void
MonitorScope::menu(bool recordedData)
{
        recordedData_ = recordedData;
        menuFrame_    = e_draw == DrawEnum::PLOT && !isDisplayHidden() ? ImGui::GetFrameCount() : -1;
        // Visible plots submit their controls over the plot frame, so the menu
        // does not reserve a separate row. Hidden scopes still need a toolbar.
        if (menuFrame_ < 0)
                drawMenu();
}

bool
MonitorScope::matchesSearch(const std::string &name, const MonitorChannel &channel) const
{
        if (searchQuery_.empty())
                return true;
        const auto contains = [&](const std::string &value) {
                return ui::search::LowerCopy(value).find(searchQuery_) != std::string::npos;
        };
        return contains(name_) || contains(getLabel()) || contains(name) || contains(channel.label_) ||
               contains(channel.symbolName_) || contains(channel.device_) ||
               ui::search::MatchesAddress(channel.addr_, searchQuery_);
}

bool
MonitorScope::hasSearchMatch() const
{
        if (searchQuery_.empty())
                return true;
        return std::any_of(chs_.begin(), chs_.end(), [&](const auto &entry) {
                return entry.second && !entry.second->isPendingDelete() && matchesSearch(entry.first, *entry.second);
        });
}

void
MonitorScope::drawMenu(bool overlay, float reservedRight)
{
        bool              renameRequested = false, deleteRequested = false;
        const auto       &style         = ImGui::GetStyle();
        const bool        focused       = parent_ && parent_->isScopeFocused(name_) && !parent_->scopeFocusPreview_.active();
        const char       *focusLabel    = focused ? tr("Restore", "还原") : tr("Focus", "聚焦");
        const char       *moreLabel     = tr("More", "更多");
        const float       gap           = style.ItemInnerSpacing.x;
        const float       focusWidth    = ImGui::CalcTextSize(focusLabel).x + style.FramePadding.x * 2;
        const float       moreWidth     = ImGui::CalcTextSize(moreLabel).x + style.FramePadding.x * 2;
        const ImVec2      origin        = ImGui::GetCursorScreenPos();
        const float       overlayRight  = origin.x + ImGui::GetContentRegionAvail().x - 3;
        const bool        showCollapse  = !overlay && e_draw == DrawEnum::TABLE && !isDisplayHidden() && !chs_.empty();
        const char       *collapseLabel = tr("Collapse all###monitor_collapse_all", "全部折叠###monitor_collapse_all");
        const float       collapseWidth = ImGui::CalcTextSize(collapseLabel, nullptr, true).x + style.FramePadding.x * 2;
        const float       nameLeft      = origin.x + (showCollapse ? collapseWidth + style.ItemSpacing.x : 0);
        const bool        showFocus     = parent_ && overlayRight - nameLeft >= focusWidth + moreWidth + gap + 6;
        const std::string title         = getLabel() + (paused_ ? tr("   Acquisition paused", "   采集已暂停") : "");
        const float       nameRight     = overlayRight - moreWidth - gap - (showFocus ? focusWidth + gap : 0) - reservedRight;
        const float       fullNameWidth = ImGui::CalcTextSize(title.c_str()).x + style.FramePadding.x * 2;
        // Keep the default legend corner clear. Long names are clipped to a
        // compact badge; the tooltip and More menu retain the complete name.
        const float nameWidth =
            overlay
                ? std::max(
                      0.0f,
                      std::min({nameRight - origin.x - ImGui::GetFontSize() * 10, fullNameWidth, ImGui::GetFontSize() * 16}))
                : std::max(0.0f, std::min(fullNameWidth, nameRight - nameLeft));
        const ImVec2 namePos       = ImVec2(nameRight - nameWidth, origin.y + 1);
        const float  buttonY       = namePos.y + (overlay ? 0.0f : style.FramePadding.y);
        ImDrawList  *drawList      = ImGui::GetWindowDrawList();
        auto         overlayButton = [&](const char *idLabel, const ImRect &rect, bool titleBadge = false) {
                const ImGuiID id = ImGui::GetID(idLabel);
                if (!ImGui::ItemAdd(rect, id))
                        return false;
                bool       hovered = false, held = false;
                const bool pressed = ImGui::ButtonBehavior(rect, id, &hovered, &held);
                ImGui::RenderNavCursor(rect, id);
                const ImGuiCol color = held && hovered ? ImGuiCol_ButtonActive
                                               : hovered       ? ImGuiCol_ButtonHovered
                                               : titleBadge    ? ImGuiCol_FrameBg
                                                               : ImGuiCol_Button;
                ImGui::RenderFrame(rect.Min, rect.Max, ImGui::GetColorU32(color), !titleBadge, style.FrameRounding);
                if (!titleBadge)
                        ImGui::RenderTextClipped(rect.Min, rect.Max, idLabel, nullptr, nullptr, ImVec2(0.5f, 0.5f));
                return pressed;
        };
        // Table actions share the scope title row; reserve their width before
        // clipping a long name and center compact buttons against the title.
        if (showCollapse) {
                ImGui::SetCursorScreenPos(ImVec2(origin.x, buttonY));
                if (ImGui::SmallButton(collapseLabel)) {
                        expandedGroups_.clear();
                        if (parent_)
                                parent_->setModified();
                }
                ImGui::SameLine();
        }
        const bool showTitle = overlay ? nameWidth >= std::min(ImGui::GetFontSize() * 3, fullNameWidth) : nameWidth > 0;
        if (overlay && showTitle)
                overlayButton("##scopeTitle", ImRect(namePos, ImVec2(nameRight, namePos.y + ImGui::GetFrameHeight())), true);
        else if (!overlay && showTitle) {
                ImGui::SetCursorScreenPos(namePos);
                ImGui::InvisibleButton("##scopeTitle", ImVec2(nameWidth, ImGui::GetFrameHeight()));
        }
        if (showTitle) {
                drawList->PushClipRect(namePos, ImVec2(namePos.x + nameWidth, namePos.y + ImGui::GetFrameHeight()), true);
                drawList->AddText(
                    ImVec2(namePos.x + (overlay ? style.FramePadding.x : 0), namePos.y + style.FramePadding.y),
                    paused_ ? ImGui::GetColorU32(ui::SemanticButtonPalette(ui::BtnStyle::Warning, ImGui::GetStyle()).text)
                            : ImGui::GetColorU32(ImGuiCol_Text),
                    title.c_str());
                drawList->PopClipRect();
                if (ImGui::IsItemHovered()) {
                        if (fullNameWidth > nameWidth)
                                ImGui::SetTooltip("%s", getLabel().c_str());
                        renameRequested = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                }
                if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("SCOPE_REORDER", name_.c_str(), name_.size() + 1);
                        ImGui::TextUnformatted(getLabel().c_str());
                        ImGui::EndDragDropSource();
                }
        }
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_NoPopupHierarchy) &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
            ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] <
                ImGui::GetIO().MouseDragThreshold * ImGui::GetIO().MouseDragThreshold)
                ImGui::OpenPopup("##scopeOptions");
        if (!overlay)
                ImGui::SameLine();
        if (showFocus) {
                const ImVec2 pos(overlayRight - moreWidth - gap - focusWidth, buttonY);
                if (!overlay)
                        ImGui::SetCursorScreenPos(pos);
                if (overlay
                        ? overlayButton(focusLabel, ImRect(pos, ImVec2(pos.x + focusWidth, pos.y + ImGui::GetFrameHeight())))
                        : ImGui::SmallButton(focusLabel))
                        parent_->toggleScopeFocus(name_);
                else if (!focused && parent_->scopeFocusPreview_.consider(
                                         name_, parent_->focusedScope_, name_, ImGui::GetCurrentWindow()->ParentWindow))
                        parent_->scopeFocusScrollReset_ = true;
                TutorialGuide::instance().mark("recorder.focus");

                if (!overlay)
                        ImGui::SameLine();
        }
        const ImVec2 morePos(overlayRight - moreWidth, buttonY);
        if (!overlay)
                ImGui::SetCursorScreenPos(morePos);
        const bool activateOptions =
            overlay ? overlayButton(moreLabel, ImRect(morePos, ImVec2(overlayRight, morePos.y + ImGui::GetFrameHeight())))
                    : ImGui::SmallButton(moreLabel);
        char optionsName[80];
        snprintf(optionsName, sizeof(optionsName), "##ScopeOptions_%p", static_cast<void *>(this));
        const bool contextOptions = ImGui::BeginPopup("##scopeOptions");
        if (contextOptions)
                optionsReveal_.close();
        const bool hoverOptions = !contextOptions && ui::BeginHoverPanel(optionsName,
                                                                         tr("Scope options", "示波器选项"),
                                                                         optionsReveal_,
                                                                         activateOptions,
                                                                         ImGui::GetFontSize() * 22,
                                                                         ImGui::GetWindowViewport());
        if (contextOptions || hoverOptions) {
                if (overlay || !showTitle || (parent_ && !showFocus)) {
                        ImGui::TextUnformatted(title.c_str());
                        if (parent_ && !showFocus && ImGui::MenuItem(focusLabel))
                                parent_->toggleScopeFocus(name_);
                        ImGui::Separator();
                }
                ImGui::TextUnformatted(tr("View", "显示方式"));
                const char *viewNames[] = {tr("Waveform", "波形"), tr("Table", "表格")};
                int         view        = e_draw == DrawEnum::PLOT ? 0 : 1;
                if (ui::Choice("##draw_mode", &view, viewNames, 2)) {
                        e_draw = view == 0 ? DrawEnum::PLOT : DrawEnum::TABLE;
                        if (parent_)
                                parent_->setModified();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(
                        tr("Frequency analysis", "频域分析"), nullptr, showFft_, !recordedData_ && e_draw == DrawEnum::PLOT)) {
                        showFft_ = !showFft_;
                        if (parent_)
                                parent_->setModified();
                }
                if (ImGui::BeginMenu(tr("FFT settings", "FFT 设置"), showFft_ && !recordedData_ && e_draw == DrawEnum::PLOT)) {
                        const char *pointOptions[] = {"256",
                                                      "512",
                                                      "1024",
                                                      "2048",
                                                      "4096",
                                                      "8192",
                                                      "16384",
                                                      "32768",
                                                      "65536",
                                                      "131072",
                                                      "262144",
                                                      "524288",
                                                      "1048576"};
                        int         current        = 0;
                        for (int i = 0; i < IM_ARRAYSIZE(pointOptions); ++i)
                                if (fftPoints_ == atoi(pointOptions[i]))
                                        current = i;
                        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
                        if (ui::Choice(tr("Resolution", "分辨率"), &current, pointOptions, IM_ARRAYSIZE(pointOptions))) {
                                reinitFft(atoi(pointOptions[current]));
                                if (parent_)
                                        parent_->setModified();
                        }
                        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
                        int peaks = fftPeakCount_;
                        if (ImGui::InputInt(tr("Peaks", "峰值数量"), &peaks)) {
                                fftPeakCount_ = std::clamp(peaks, 0, 20);
                                if (parent_)
                                        parent_->setModified();
                        }
                        if (ImGui::Checkbox(tr("Bar chart", "柱状图"), &fftBars_) && parent_)
                                parent_->setModified();
                        ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(tr("Statistics / peaks", "统计 / 峰值"), nullptr, showSidePanel_)) {
                        showSidePanel_ = !showSidePanel_;
                        if (parent_)
                                parent_->setModified();
                }
                bool anyVisible = std::any_of(
                    chs_.begin(), chs_.end(), [](const auto &entry) { return entry.second && entry.second->show_; });
                if (ImGui::MenuItem(tr("Show curves", "显示曲线"), nullptr, anyVisible)) {
                        for (auto &[_, ch] : chs_)
                                if (ch)
                                        ch->show_ = !anyVisible;
                        if (parent_)
                                parent_->setModified();
                }
                if (!parent_ && !showFft_ && e_draw == DrawEnum::PLOT)
                        drawTimeMeasurementControls(timeMeasurement());
                if (!recordedData_) {
                        if (ImGui::MenuItem(hidden_ ? tr("Show scope", "显示示波器") : tr("Hide scope", "隐藏示波器"))) {
                                setHidden(!hidden_);
                                if (parent_)
                                        parent_->setModified();
                        }
                        if (ImGui::MenuItem(paused_ ? tr("Resume acquisition", "恢复采集")
                                                    : tr("Pause acquisition", "暂停采集"))) {
                                paused_ = !paused_;
                                if (parent_)
                                        parent_->setModified();
                        }
                }
                ImGui::Separator();
                renameRequested |= ImGui::MenuItem(tr("Rename...", "重命名..."));
                if (!label_.empty() && ImGui::MenuItem(tr("Reset name", "恢复默认名称"))) {
                        label_.clear();
                        if (parent_)
                                parent_->setModified();
                }
                if (!recordedData_)
                        deleteRequested = ImGui::MenuItem(tr("Delete scope...", "删除示波器..."));
                if (contextOptions)
                        ImGui::EndPopup();
                else
                        ImGui::End();
        }
        if (renameRequested) {
                snprintf(renameBuf_, sizeof(renameBuf_), "%s", getLabel().c_str());
                ImGui::OpenPopup("Rename Scope");
        }
        if (ImGui::BeginPopup("Rename Scope")) {
                ImGui::TextUnformatted(tr("Scope name", "示波器名称"));
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 15);
                if (ImGui::IsWindowAppearing())
                        ImGui::SetKeyboardFocusHere();
                bool commit = ImGui::InputText("##renameScope",
                                               renameBuf_,
                                               sizeof(renameBuf_),
                                               ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                if (ImGui::Button(tr("Save", "保存")))
                        commit = true;
                if (commit && renameBuf_[0]) {
                        setLabel(renameBuf_);
                        if (parent_)
                                parent_->setModified();
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }
        const char *deletePopup = tr("Delete scope?###delete_scope_confirm", "删除示波器？###delete_scope_confirm");
        if (deleteRequested)
                ImGui::OpenPopup(deletePopup);
        if (ImGui::BeginPopupModal(deletePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(getLabel().c_str());
                ImGui::TextUnformatted(
                    tr("This removes the scope and its captured data.", "此操作会移除此示波器及其已采集的数据。"));
                if (ImGui::Button(tr("Cancel", "取消")))
                        ImGui::CloseCurrentPopup();
                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (ui::Button(tr("Delete", "删除"), ui::BtnStyle::Danger)) {
                        markPendingDelete();
                        if (parent_)
                                parent_->setModified();
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
        }
        if (!overlay)
                ImGui::Separator();
}

void
MonitorScope::draw(double *linkXMin, double *linkXMax, u32 maxDisplayPoints, MonitorViewMode &mode)
{
        if (searchScroll_)
                ImGui::SetScrollHereY(0.5f);
        if (discardUnavailableReason())
                cancelDiscardSelection();
        if (e_draw == DrawEnum::PLOT)
                plotDraw(linkXMin, linkXMax, maxDisplayPoints, mode);
        else if (e_draw == DrawEnum::TABLE)
                tableDraw();
}

void
MonitorScope::tableMenu()
{
}

void
MonitorScope::focusTableGroup(const std::string &path)
{
        tableFocusPreview_.commit();
        if (tableFocusPath_ == path)
                return;
        tableFocusPath_        = path;
        tableFocusScrollReset_ = true;
}

static std::vector<std::string>
scopePathParts(const std::string &path)
{
        std::vector<std::string> parts;
        for (size_t at = 0; at < path.size();) {
                if (path[at] == '.') {
                        ++at;
                        continue;
                }
                const bool index = path[at] == '[';
                size_t     end   = index ? path.find(']', at) : path.find_first_of(".[", at);
                if (end == std::string::npos)
                        end = path.size();
                else if (index)
                        ++end;
                parts.push_back(path.substr(at, end - at));
                at = end;
        }
        return parts;
}

static std::string
scopeChildPath(const std::string &parent, const std::string &child)
{
        return parent.empty() ? child : parent + (child.starts_with('[') ? "" : ".") + child;
}

static bool
scopePathContains(const std::string &parent, const std::string &child)
{
        return child == parent || (child.size() > parent.size() && child.starts_with(parent) &&
                                   (child[parent.size()] == '.' || child[parent.size()] == '['));
}

void
MonitorScope::tableDraw()
{
        if (tableFocusPreview_.maintain(tableFocusPath_))
                tableFocusScrollReset_ = false;
        auto chOrder = [&](const std::string &k) -> i64 {
                auto it = chs_.find(k);
                return it != chs_.end() ? it->second->getOrder() : 0;
        };

        std::vector<std::string> keys;
        for (auto &pair : chs_)
                keys.push_back(pair.first);
        // Order chs by insertion order (e.g. CSV column order) rather than
        // alphabetically; fall back to name for a stable tiebreak
        std::sort(keys.begin(), keys.end(), [&](const std::string &a, const std::string &b) {
                const i64 oa = chOrder(a), ob = chOrder(b);
                return oa != ob ? oa < ob : a < b;
        });
        const bool searching = !searchQuery_.empty();
        if (searching)
                std::erase_if(keys, [&](const auto &key) {
                        const auto found = chs_.find(key);
                        return found == chs_.end() || !matchesSearch(key, *found->second);
                });
        if (keys.empty()) {
                ImGui::TextDisabled("%s", tr("No matching variables in this scope", "此示波器中没有匹配变量"));
                return;
        }

        // Structure members and array indices share a canonical-path tree.
        struct TNode {
                std::map<std::string, TNode> children;
                std::string                  leafKey;                                // non-empty → this node is a ch
                i64                          order{std::numeric_limits<i64>::max()}; // min insertion order of leaves beneath
        };
        TNode root;
        for (const auto &k : keys) {
                const i64 ord = chOrder(k);
                TNode    *cur = &root;
                cur->order    = std::min(cur->order, ord);
                for (const auto &part : scopePathParts(k)) {
                        cur        = &cur->children[part];
                        cur->order = std::min(cur->order, ord);
                }
                cur->leafKey = k;
        }

        auto resolveGroup = [&](const std::string &path) -> TNode * {
                TNode *node = &root;
                for (const auto &part : scopePathParts(path)) {
                        const auto child = node->children.find(part);
                        if (child == node->children.end() || !child->second.leafKey.empty())
                                return nullptr;
                        node = &child->second;
                }
                return node;
        };

        if (!tableFocusPath_.empty() && !resolveGroup(tableFocusPath_))
                focusTableGroup({});
        if (!tableFocusPath_.empty()) {
                // Keep the breadcrumbs stable while navigation changes the table root
                const std::string navigationPath = tableFocusPath_;
                if (ImGui::SmallButton(tr("< Back###monitor_focus_back", "< 返回上级###monitor_focus_back"))) {
                        const size_t parentEnd = navigationPath.find_last_of(".[");
                        focusTableGroup(parentEnd == std::string::npos ? std::string{} : navigationPath.substr(0, parentEnd));
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("All variables###monitor_focus_all", "全部变量###monitor_focus_all")))
                        focusTableGroup({});
                ImGui::SameLine();
                ImGui::TextDisabled("%s", tr("Focused view", "聚焦视图"));

                // Wrap between path segments, including repeated names such as lo.ctrl.pd.lo
                const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
                std::string path;
                for (const auto &label : scopePathParts(navigationPath)) {
                        const bool first  = path.empty();
                        path              = scopeChildPath(path, label);
                        const float width = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
                        if (!first && ImGui::GetItemRectMax().x + width + ImGui::CalcTextSize(">").x +
                                              ImGui::GetStyle().ItemSpacing.x * 3 <=
                                          right) {
                                ImGui::SameLine();
                                ImGui::TextUnformatted(">");
                                ImGui::SameLine();
                        }
                        ImGui::PushID(path.c_str());
                        if (ImGui::Button((label + "###monitor_focus_crumb").c_str(),
                                          ImVec2(std::max(1.0f, std::min(width, ImGui::GetContentRegionAvail().x)), 0)))
                                focusTableGroup(path);
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", path.c_str());
                        ImGui::PopID();
                }
        }
        if (tableFocusScrollReset_) {
                ImGui::SetScrollY(0.0f);
                tableFocusScrollReset_ = false;
        }
        const std::string viewPath = tableFocusPath_;
        TNode            *viewRoot = resolveGroup(viewPath);

        ui::HierarchyTable tableCard;
        if (!tableCard.begin("MonitorTable",
                             6,
                             ui::HierarchyTableFlags | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                                 ImGuiTableFlags_SortTristate | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                 ImGuiTableFlags_SizingFixedFit,
                             ImVec2(0, ImGui::GetContentRegionAvail().y)))
                return;

        // No DefaultSort: the table starts in manual (drag-reordered) order. Clicking a
        // column header sorts; clicking until it clears (tristate) returns to manual order
        const float defaultWidths[] = {ui::TableColumnWidth(tr("Name", "名称"), "configuration.controller", 18),
                                       ui::TableColumnWidth(tr("Value", "数值"), "-1234.567890", 8),
                                       ui::TableColumnWidth(tr("Type", "类型"), "enum (uint32)", 8),
                                       ui::TableColumnWidth(tr("Address", "地址"), "0xFFFFFFFFFFFFFFFF", 10),
                                       ui::TableColumnWidth(tr("Port", "端口"), "J-Link", 6),
                                       ui::TableColumnWidth(tr("Wave", "波形"), "...", 5)};
        const char *columnNames[]   = {tr("Name###col_name", "名称###col_name"),
                                       tr("Value###col_value", "数值###col_value"),
                                       tr("Type###col_type", "类型###col_type"),
                                       tr("Address###col_addr", "地址###col_addr"),
                                       tr("Port###col_port", "端口###col_port"),
                                       tr("Wave###col_wave", "波形###col_wave")};
        for (int i = 0; i < 6; ++i)
                ImGui::TableSetupColumn(columnNames[i],
                                        ImGuiTableColumnFlags_WidthFixed |
                                            (i == 5 ? ImGuiTableColumnFlags_NoSort : ImGuiTableColumnFlags_None),
                                        defaultWidths[i]);
        const bool freezeName = ImGui::GetWindowWidth() >= defaultWidths[0] + defaultWidths[1] + ImGui::GetFontSize() * 4;
        ImGui::TableSetupScrollFreeze(freezeName ? 1 : 0, 1);

        if (restoreTableWidths_) {
                ImGuiTable *table = ImGui::GetCurrentTable();
                if (table && !table->IsLayoutLocked) {
                        for (int i = 0; i < 6 && i < table->ColumnsCount; ++i) {
                                const float width =
                                    std::max(tableColumnWidths_[i],
                                             std::min(defaultWidths[i], ImGui::GetFontSize() * (i == 0 ? 12.0f : 5.0f)));
                                if (width <= 1.0f)
                                        continue;
                                ImGuiTableColumn &column = table->Columns[i];
                                if (column.Flags & ImGuiTableColumnFlags_WidthStretch)
                                        column.StretchWeight = width;
                                else
                                        column.WidthRequest = width;
                                column.AutoFitQueue = 0;
                        }
                        restoreTableWidths_ = false;
                }
        }
        if (restoreTableSort_) {
                if (tableSortColumn_ >= 0 && tableSortColumn_ < 5)
                        ImGui::TableSetColumnSortDirection(
                            tableSortColumn_, static_cast<ImGuiSortDirection>(tableSortDirection_), false);
                else
                        ImGui::TableSetColumnSortDirection(0, ImGuiSortDirection_None, false);
                restoreTableSort_ = false;
        }
        ui::HierarchyHeadersRow();

        ImGuiTableSortSpecs *sortSpecs = ImGui::TableGetSortSpecs();
        if (sortSpecs && sortSpecs->SpecsDirty) {
                int nextColumn    = -1;
                int nextDirection = ImGuiSortDirection_Ascending;
                if (sortSpecs->SpecsCount > 0) {
                        nextColumn             = sortSpecs->Specs[0].ColumnIndex;
                        nextDirection          = sortSpecs->Specs[0].SortDirection;
                        forceManualTableOrder_ = false;
                } else {
                        forceManualTableOrder_ = true;
                }
                if (tableSortColumn_ != nextColumn || tableSortDirection_ != nextDirection) {
                        tableSortColumn_    = nextColumn;
                        tableSortDirection_ = nextDirection;
                        if (parent_)
                                parent_->setModified();
                }
                sortSpecs->SpecsDirty = false;
        }

        // Children of a trie node ordered by min insertion order (then label), or by column sort specs
        auto orderedChildren = [&](TNode &n) {
                std::vector<std::pair<const std::string *, TNode *>> v;
                v.reserve(n.children.size());
                for (auto &[lbl, child] : n.children)
                        v.emplace_back(&lbl, &child);

                if (!forceManualTableOrder_ && sortSpecs && sortSpecs->SpecsCount > 0) {
                        const auto *spec = &sortSpecs->Specs[0];
                        std::sort(v.begin(), v.end(), [&](const auto &a, const auto &b) -> bool {
                                bool isLeafA = !a.second->leafKey.empty();
                                bool isLeafB = !b.second->leafKey.empty();
                                if (isLeafA != isLeafB)
                                        return isLeafB; // Groups before leaves

                                int cmp = 0;
                                if (isLeafA && isLeafB) {
                                        auto &chA = chs_[a.second->leafKey];
                                        auto &chB = chs_[b.second->leafKey];
                                        switch (spec->ColumnIndex) {
                                                case 0: // Name
                                                        cmp = a.second->leafKey.compare(b.second->leafKey);
                                                        break;
                                                case 1: // Value
                                                        cmp = (chA->getDispVal() < chB->getDispVal())
                                                                  ? -1
                                                                  : (chA->getDispVal() > chB->getDispVal() ? 1 : 0);
                                                        break;
                                                case 2: // Type
                                                        cmp = chA->getType().compare(chB->getType());
                                                        break;
                                                case 3: // Address
                                                        cmp = (chA->getAddr() < chB->getAddr())
                                                                  ? -1
                                                                  : (chA->getAddr() > chB->getAddr() ? 1 : 0);
                                                        break;
                                                case 4: // Port
                                                        cmp = chA->getDevice().compare(chB->getDevice());
                                                        break;
                                        }
                                } else {
                                        cmp = a.first->compare(*b.first);
                                }

                                if (cmp == 0) {
                                        cmp = a.second->order < b.second->order ? -1
                                                                                : (a.second->order > b.second->order ? 1 : 0);
                                }
                                return spec->SortDirection == ImGuiSortDirection_Ascending ? (cmp < 0) : (cmp > 0);
                        });
                } else {
                        std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) {
                                return a.second->order != b.second->order ? a.second->order < b.second->order
                                                                          : *a.first < *b.first;
                        });
                }
                return v;
        };

        // Collect all leaf ch keys under a trie node (recursive)
        std::function<void(TNode &, std::vector<std::string> &)> collectLeaves;
        collectLeaves = [&](TNode &n, std::vector<std::string> &out) {
                if (!n.leafKey.empty()) {
                        out.push_back(n.leafKey);
                } else {
                        for (auto &[_, child] : n.children)
                                collectLeaves(child, out);
                }
        };

        // A group is highlighted if it equals a selected path or is a descendant of one
        auto isGroupHighlighted = [&](const std::string &path) {
                for (const auto &sel : selectedGroupPaths_) {
                        if (path == sel)
                                return true;
                        if (path.size() > sel.size() + 1 && path.compare(0, sel.size(), sel) == 0 && path[sel.size()] == '.')
                                return true;
                }
                return false;
        };

        // Keep the visible row set for focus/search diagnostics.
        std::vector<std::string>                          visibleKeys;
        std::function<void(TNode &, const std::string &)> collectVisible;
        collectVisible = [&](TNode &node, const std::string &path) {
                for (auto &[childLabel, childNode] : orderedChildren(node)) {
                        const std::string childPath = path.empty() ? *childLabel : path + "." + *childLabel;
                        if (!childNode->leafKey.empty())
                                visibleKeys.push_back(childNode->leafKey);
                        else if (searching || expandedGroups_.count(childPath))
                                collectVisible(*childNode, childPath);
                }
        };
        collectVisible(*viewRoot, viewPath);
        tableVisibleKeys_ = std::move(visibleKeys);

        std::function<void(const std::string &, TNode &, const std::string &, int)> drawNode;
        drawNode = [&](const std::string &label, TNode &node, const std::string &fullPath, int depth) {
                if (!node.leafKey.empty()) {
                        // Leaf — draw a full interactive row, but show only the short label
                        drawTableRow(node.leafKey, chs_[node.leafKey], label);
                        if (depth > 0)
                                ui::HierarchyRow(depth - 1);
                } else {
                        // Collect all leaves under this group (for selection/deletion)
                        std::vector<std::string> groupLeaves;
                        collectLeaves(node, groupLeaves);

                        bool isGroupSelected = isGroupHighlighted(fullPath);

                        // Group header — show collapsible tree node, empty other columns
                        if (depth == 0)
                                ui::HierarchyGap();
                        ui::HierarchyNextRow();
                        ImGui::TableNextColumn();

                        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 0.0f);
                        ImGuiTreeNodeFlags treeFlags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
                        const bool         wasOpen   = searching || expandedGroups_.count(fullPath) > 0;
                        ImGui::SetNextItemOpen(wasOpen);
                        // Suppress the hover/selected highlight on the group header (matches leaf rows)
                        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
                        ui::HierarchyName();
                        const char *treeLabel = label.empty() ? tr("Unnamed", "未命名") : label.c_str();
                        const float labelX    = ImGui::GetCursorScreenPos().x + ImGui::GetTreeNodeToLabelSpacing();
                        bool        open      = ui::HierarchyNode(treeLabel, treeFlags);
                        ImGui::PopStyleColor(3);
                        ui::HierarchyRow(open ? depth : depth - 1);
                        ui::HierarchyCard structureCard;
                        if (depth == 0 || open)
                                structureCard.begin(depth);
                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                                ChannelMovePayload payload;
                                payload.srcScope = this;
                                payload.isGroup  = true;
                                snprintf(payload.chName, sizeof(payload.chName), "%s", fullPath.c_str());
                                ImGui::SetDragDropPayload("DND_CHANNEL_MOVE", &payload, sizeof(ChannelMovePayload));
                                ImGui::Text(tr("Drag: %s", "拖动: %s"), label.c_str());
                                ImGui::EndDragDropSource();
                        }
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", tr("Drag: reorder / move to scope", "拖动：排序 / 移动到示波器"));
                        if (ImGui::BeginDragDropTarget()) {
                                if (const ImGuiPayload *mv = ImGui::AcceptDragDropPayload("DND_CHANNEL_MOVE")) {
                                        std::string anchor = groupAnchorKey(fullPath);
                                        if (!anchor.empty())
                                                applyChannelMoveDrop(static_cast<ChannelMovePayload *>(mv->Data), anchor);
                                }
                                ImGui::EndDragDropTarget();
                        }

                        if (ImGui::IsItemHovered()) {
                                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen() &&
                                    ImGui::GetIO().MousePos.x >= labelX &&
                                    ImGui::GetIO().MousePos.x < labelX + ImGui::CalcTextSize(treeLabel, nullptr, true).x)
                                        focusTableGroup(fullPath);
                                ImGui::SetTooltip("%s\n%s",
                                                  fullPath.c_str(),
                                                  tr("Double-click name to focus; click arrow to expand/collapse",
                                                     "双击名称聚焦；单击箭头展开/折叠"));
                        }

                        if (!searching && open != wasOpen) {
                                if (open)
                                        expandedGroups_.insert(fullPath);
                                else
                                        expandedGroups_.erase(fullPath);
                                if (parent_)
                                        parent_->setModified();
                        }

                        // Left click only clears any right-click context selection.
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
                                for (auto &pair : chs_)
                                        pair.second->selected_ = false;
                                selectedGroupPaths_.clear();
                        }

                        // Right-click: auto-select this group when not selected, then show unified menu
                        if (ui::HierarchyContextItem()) {
                                if (ImGui::MenuItem(tr("Copy", "复制")))
                                        ImGui::SetClipboardText(clipboardText(groupLeaves).c_str());
                                ImGui::Separator();
                                if (!isGroupSelected) {
                                        for (auto &pair : chs_)
                                                pair.second->selected_ = false;
                                        selectedGroupPaths_.clear();
                                        selectedGroupPaths_.insert(fullPath);
                                        for (auto &lk : groupLeaves) {
                                                auto it = chs_.find(lk);
                                                if (it != chs_.end())
                                                        it->second->selected_ = true;
                                        }
                                }
                                if (ImGui::MenuItem(tr("Delete", "删除"))) {
                                        for (auto &pair : chs_)
                                                if (pair.second->selected_)
                                                        pair.second->markPendingDelete();
                                        selectedGroupPaths_.clear();
                                }
                                ImGui::BeginDisabled();
                                ImGui::MenuItem(tr("Edit Properties (select a leaf)", "编辑属性（请选择叶子变量）"));
                                ImGui::EndDisabled();
                                ImGui::Separator();
                                ImGui::TextDisabled("%s", tr("Access mode", "读写模式"));
                                auto groupSupportsWrite = [](const MonitorChannel &ch) {
                                        const std::string &device = ch.getDevice();
                                        return device == "JLINK" || device == "SHM" || device == "LOCAL" ||
                                               (device == ava_sim::device && ava_sim::writableField(ch.getSimField()));
                                };
                                auto setGroupWritable = [&](bool writable) {
                                        for (auto &pair : chs_) {
                                                auto &selected = *pair.second;
                                                if (!selected.selected_ || (writable && !groupSupportsWrite(selected)))
                                                        continue;
                                                selected.setWritable(writable);
                                                if (!writable)
                                                        selected.waveEnable_ = false;
                                        }
                                        if (parent_)
                                                parent_->setModified();
                                };
                                bool allGroupWritable = true;
                                for (const auto &leaf : groupLeaves) {
                                        auto it = chs_.find(leaf);
                                        if (it != chs_.end() && !groupSupportsWrite(*it->second)) {
                                                allGroupWritable = false;
                                                break;
                                        }
                                }
                                int access = -2;
                                for (const auto &leaf : groupLeaves) {
                                        const auto it = chs_.find(leaf);
                                        if (it == chs_.end())
                                                continue;
                                        const int value = it->second->isWritable() ? 1 : 0;
                                        if (access == -2)
                                                access = value;
                                        else if (access != value) {
                                                access = -1;
                                                break;
                                        }
                                }
                                const char *accessNames[]   = {tr("Read only", "只读"), tr("Read / write", "读写")};
                                const bool  accessEnabled[] = {true, allGroupWritable};
                                if (ui::Choice("##access", &access, accessNames, 2, accessEnabled))
                                        setGroupWritable(access == 1);
                                ImGui::Separator();
                                if (ImGui::BeginMenu(tr("Value display format", "数值显示格式"))) {
                                        const int   radixValues[] = {10, 16, 8, 2};
                                        const char *radixNames[]  = {tr("Decimal", "十进制"),
                                                                     tr("Hexadecimal", "十六进制"),
                                                                     tr("Octal", "八进制"),
                                                                     tr("Binary", "二进制")};
                                        int         selected      = -1;
                                        for (int i = 0; i < 4; ++i) {
                                                bool allMatch = true;
                                                for (const auto &leaf : groupLeaves) {
                                                        const auto it = chs_.find(leaf);
                                                        if (it != chs_.end() && it->second->getDisplayRadix() != radixValues[i])
                                                                allMatch = false;
                                                }
                                                if (allMatch)
                                                        selected = i;
                                        }
                                        if (ui::Choice("##value_format", &selected, radixNames, 4)) {
                                                for (const auto &leaf : groupLeaves) {
                                                        const auto it = chs_.find(leaf);
                                                        if (it != chs_.end())
                                                                it->second->setDisplayRadix(radixValues[selected]);
                                                }
                                                if (parent_)
                                                        parent_->setModified();
                                        }
                                        ImGui::EndMenu();
                                }
                                ImGui::Separator();
                                ImGui::BeginDisabled();
                                ImGui::MenuItem(tr("DWT trace (select a leaf)", "DWT 跟踪（请选择叶子变量）"));
                                ImGui::EndDisabled();
                                ImGui::EndPopup();
                        }

                        ImGui::TableNextColumn();
                        ImGui::PushID(fullPath.c_str());
                        if (ImGui::SmallButton(tr("Focus###focus_group", "聚焦###focus_group")))
                                focusTableGroup(fullPath);
                        else if (parent_ && tableFocusPreview_.consider(fullPath, tableFocusPath_, fullPath))
                                tableFocusScrollReset_ = true;
                        ImGui::PopID();
                        for (int column = 2; column < 6; ++column)
                                ImGui::TableNextColumn();
                        if (open) {
                                for (auto &[childLabel, childNode] : orderedChildren(node))
                                        drawNode(*childLabel, *childNode, scopeChildPath(fullPath, *childLabel), depth + 1);
                                ImGui::TreePop();
                        }
                        ImGui::PopStyleVar();
                }
        };

        ui::HierarchyCard focusedCard;
        if (!viewPath.empty()) {
                ui::HierarchyGap();
                focusedCard.begin(0, true);
        }
        for (auto &[childLabel, childNode] : orderedChildren(*viewRoot))
                drawNode(*childLabel, *childNode, scopeChildPath(viewPath, *childLabel), viewPath.empty() ? 0 : 1);
        focusedCard.end();

        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered()) {
                for (auto &pair : chs_)
                        pair.second->selected_ = false;
                selectedGroupPaths_.clear();
        }

        drawChannelPropertiesPopup();

        if (const ImGuiTable *table = ImGui::GetCurrentTable()) {
                const bool userResizing = table->ResizedColumn >= 0;
                for (int i = 0; i < 6 && i < table->ColumnsCount; ++i) {
                        const float width = table->Columns[i].WidthGiven;
                        if (width <= 1.0f)
                                continue;
                        if (tableWidthsInitialized_ && userResizing && std::abs(tableColumnWidths_[i] - width) > 0.5f &&
                            parent_)
                                parent_->setModified();
                        tableColumnWidths_[i] = width;
                }
                tableWidthsInitialized_ = true;
        }

        tableCard.end();
}

static bool
isIntegerType(const std::string &t)
{
        if (t.empty())
                return false;
        char c = t[0];
        return c == 'u' || c == 'i';
}

static bool
integerTypeInfo(const std::string &type, bool &signedValue, u32 &bitWidth)
{
        if (type.size() < 2 || (type[0] != 'u' && type[0] != 'i'))
                return false;
        signedValue = type[0] == 'i';
        if (type == "u8" || type == "i8")
                bitWidth = 8;
        else if (type == "u16" || type == "i16")
                bitWidth = 16;
        else if (type == "u32" || type == "i32")
                bitWidth = 32;
        else if (type == "u64" || type == "i64")
                bitWidth = 64;
        else
                return false;
        return true;
}

static std::string
formatChannelInteger(MonitorChannel &ch, int radix)
{
        bool signedValue = false;
        u32  bitWidth    = 32;
        if (!integerTypeInfo(ch.getType(), signedValue, bitWidth))
                return {};
        if (ch.getBitSize() > 0)
                bitWidth = std::min(ch.getBitSize(), bitWidth);
        u64 raw = 0;
        if (ch.getDisplayBits(raw) && (radix != 10 || (ch.getGain() == 1 && ch.getBias() == 0))) {
                if (ch.getBitSize() > 0)
                        raw = ch.getBitOffset() < 64 ? raw >> ch.getBitOffset() : 0;
                return formatIntegerValue(raw, signedValue, bitWidth, radix);
        }
        if (!std::isfinite(ch.getDispVal()))
                return "...";
        const double number = ch.getDispVal();
        if (signedValue && (number < static_cast<double>(INT64_MIN) || number >= -static_cast<double>(INT64_MIN)))
                return "...";
        if (!signedValue && (number < 0 || number >= static_cast<double>(UINT64_MAX)))
                return "...";
        if (!signedValue)
                return formatIntegerValue(static_cast<u64>(number), false, bitWidth, radix);
        const i64 val = static_cast<i64>(std::llround(ch.getDispVal()));
        return formatIntegerValue(static_cast<u64>(val), signedValue, bitWidth, radix);
}

static std::string
formatChannelValue(MonitorChannel &ch, int radix)
{
        if (ch.isAddrUnknown())
                return "UNKNOWN";
        u64        bits  = 0;
        const bool exact = ch.getDisplayBits(bits);
        if (!exact && !ch.arraySnapshot_ && ch.read_.rawEmpty())
                return tr("Not sampled", "未采样");
        if (isIntegerType(ch.getType())) {
                const auto numeric = formatChannelInteger(ch, radix);
                if (ch.isEnum()) {
                        i64  enumValue = 0;
                        bool valid     = exact;
                        if (exact) {
                                bool signedValue = false;
                                u32  width       = 32;
                                integerTypeInfo(ch.getType(), signedValue, width);
                                if (ch.getBitSize()) {
                                        bits  = ch.getBitOffset() < 64 ? bits >> ch.getBitOffset() : 0;
                                        width = std::min(width, ch.getBitSize());
                                }
                                if (width < 64) {
                                        const u64 mask  = (u64{1} << width) - 1;
                                        bits           &= mask;
                                        if (signedValue && (bits & (u64{1} << (width - 1))))
                                                bits |= ~mask;
                                }
                                std::memcpy(&enumValue, &bits, sizeof(enumValue));
                        } else if (std::isfinite(ch.getDispVal()) && ch.getDispVal() >= static_cast<double>(INT64_MIN) &&
                                   ch.getDispVal() < -static_cast<double>(INT64_MIN)) {
                                enumValue = static_cast<i64>(ch.getDispVal());
                                valid     = true;
                        }
                        const char *label = valid ? ch.findEnumName(enumValue) : nullptr;
                        if (label)
                                return std::string(label) + " (" + numeric + ")";
                }
                return numeric;
        }
        const auto width = typeBytes(ch.getType()) * 8;
        if (radix != 10) {
                if (!exact) {
                        if (ch.getType() != "f32")
                                return tr("Raw bits unavailable", "无原始位数据");
                        const float value = ch.getDispRawVal();
                        std::memcpy(&bits, &value, sizeof(value));
                }
                return formatIntegerValue(bits, false, width, radix);
        }
        double value = ch.getDispVal();
        if (exact && ch.getType() == "f64") {
                std::memcpy(&value, &bits, sizeof(value));
                value = value * ch.getGain() + ch.getBias();
        }
        char text[128];
        snprintf(text, sizeof(text), ch.getType() == "f64" ? "%.8f" : "%.6f", value);
        return text;
}

std::string
MonitorScope::clipboardText(const std::vector<std::string> &names) const
{
        std::string text;
        ui::appendClipboardRow(text, tr("Name", "变量名"), tr("Value", "变量值"), tr("Address", "地址"));
        std::vector<std::string> ordered = names;
        std::sort(ordered.begin(), ordered.end(), [&](const auto &a, const auto &b) {
                const auto ia = chs_.find(a), ib = chs_.find(b);
                return ia != chs_.end() && ib != chs_.end() ? ia->second->getOrder() < ib->second->getOrder() : a < b;
        });
        for (const auto &name : ordered) {
                const auto found = chs_.find(name);
                if (found == chs_.end() || !found->second)
                        continue;
                auto       &channel    = *found->second;
                const auto &sourceName = channel.getSymbolName().empty() ? name : channel.getSymbolName();
                if (channel.arraySnapshot_) {
                        const auto &array = *channel.arraySnapshot_;
                        const auto  size  = flasharray::width(array.type);
                        for (size_t i = 0; i < array.values.size(); ++i) {
                                const auto *bytes = array.bytes.data() + i * size;
                                char        address[32];
                                snprintf(address,
                                         sizeof(address),
                                         "0x%08llX",
                                         static_cast<unsigned long long>(array.address) + i * size);
                                const auto value = channel.getDisplayRadix() == 10
                                                       ? flasharray::format(bytes, array.type, array.bigEndian)
                                                       : formatIntegerValue(flasharray::bits(bytes, size, array.bigEndian),
                                                                            false,
                                                                            static_cast<u32>(size * 8),
                                                                            channel.getDisplayRadix());
                                ui::appendClipboardRow(text, sourceName + "[" + std::to_string(i) + "]", value, address);
                        }
                } else {
                        char address[32]{};
                        if (!channel.isAddrUnknown() && channel.getDevice() == "JLINK")
                                snprintf(
                                    address, sizeof(address), "0x%08llX", static_cast<unsigned long long>(channel.getAddr()));
                        ui::appendClipboardRow(
                            text, sourceName, formatChannelValue(channel, channel.getDisplayRadix()), address);
                }
        }
        return text;
}

void
MonitorScope::drawDisplayFormatMenu(MonitorChannel &channel)
{
        if (!ImGui::BeginMenu(tr("Value display format", "数值显示格式")))
                return;
        const int   radices[] = {10, 16, 8, 2};
        const char *labels[]  = {
            tr("Decimal", "十进制"), tr("Hexadecimal", "十六进制"), tr("Octal", "八进制"), tr("Binary", "二进制")};
        int selected = 0;
        for (int i = 0; i < 4; ++i)
                if (channel.getDisplayRadix() == radices[i])
                        selected = i;
        if (ui::Choice("##value_format", &selected, labels, 4)) {
                channel.setDisplayRadix(radices[selected]);
                if (parent_)
                        parent_->setModified();
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("Floating point: raw IEEE 754 bits", "浮点数：显示 IEEE 754 原始位模式"));
        ImGui::EndMenu();
}

static f32
integerRawToFloat(u64 raw, bool signedValue, u32 bitWidth)
{
        bitWidth        = std::clamp<u32>(bitWidth, 1, 64);
        const u64 mask  = bitWidth == 64 ? ~u64{0} : ((u64{1} << bitWidth) - 1);
        raw            &= mask;
        if (!signedValue)
                return static_cast<f32>(raw);
        i64 val = static_cast<i64>(raw);
        if (bitWidth < 64 && (raw & (u64{1} << (bitWidth - 1))))
                val = static_cast<i64>(raw | ~mask);
        return static_cast<f32>(val);
}

void
MonitorScope::beginChannelProperties(const std::string &chName)
{
        auto it = chs_.find(chName);
        if (it == chs_.end() || !it->second)
                return;
        auto &ch                   = *it->second;
        chPropertyEdit_            = {};
        chPropertyEdit_.targetName = chName;
        snprintf(chPropertyEdit_.name, sizeof(chPropertyEdit_.name), "%s", chName.c_str());
        if (ch.getLabel() != chName)
                snprintf(chPropertyEdit_.alias, sizeof(chPropertyEdit_.alias), "%s", ch.getLabel().c_str());
        snprintf(chPropertyEdit_.address, sizeof(chPropertyEdit_.address), "%zX", ch.getAddr());
        snprintf(chPropertyEdit_.shmName, sizeof(chPropertyEdit_.shmName), "%s", ch.getShmRegionName().c_str());
        chPropertyEdit_.simField   = static_cast<int>(ch.getSimField());
        static const char *types[] = {"u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f16", "f32", "f64"};
        chPropertyEdit_.typeIndex  = 2;
        for (int i = 0; i < IM_ARRAYSIZE(types); ++i)
                if (ch.getType() == types[i]) {
                        chPropertyEdit_.typeIndex = i;
                        break;
                }
        static const char *devices[] = {"JLINK", "SHM", "LOCAL", "AUDIO", "UDP", "SIM_SHM"};
        for (int i = 0; i < IM_ARRAYSIZE(devices); ++i)
                if (ch.getDevice() == devices[i]) {
                        chPropertyEdit_.deviceIndex = i;
                        break;
                }
        chPropertyEdit_.writable       = ch.isWritable();
        chPropertyEdit_.enums          = ch.getEnums();
        chPropertyEdit_.unitConversion = ch.unitConversion_;
}

void
MonitorScope::drawChannelPropertiesPopup()
{
        if (chPropertyEdit_.targetName.empty())
                return;
        ImGui::OpenPopup("###EditMonitorChannelProperties");
        if (!ImGui::BeginPopupModal(
                tr("Edit Variable Properties###EditMonitorChannelProperties", "编辑变量属性###EditMonitorChannelProperties"),
                nullptr,
                ImGuiWindowFlags_AlwaysAutoResize))
                return;

        ImGui::TextUnformatted(tr("Name", "名称"));
        ImGui::SameLine(100);
        ImGui::SetNextItemWidth(280);
        ImGui::InputText("##monitorPropName", chPropertyEdit_.name, sizeof(chPropertyEdit_.name));
        ImGui::TextUnformatted(tr("Alias", "别名"));
        ImGui::SameLine(100);
        ImGui::SetNextItemWidth(280);
        ImGui::InputText("##monitorPropAlias", chPropertyEdit_.alias, sizeof(chPropertyEdit_.alias));

        static const char *types[] = {"u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f16", "f32", "f64"};
        ImGui::TextUnformatted(tr("Type", "类型"));
        ImGui::SameLine(100);
        ImGui::SetNextItemWidth(280);
        ui::Choice("##monitorPropType", &chPropertyEdit_.typeIndex, types, IM_ARRAYSIZE(types));

        ImGui::TextUnformatted(tr("Address", "地址"));
        ImGui::SameLine(100);
        ImGui::SetNextItemWidth(280);
        ImGui::InputText("##monitorPropAddress", chPropertyEdit_.address, sizeof(chPropertyEdit_.address));

        static const char *devices[] = {"JLINK", "SHM", "LOCAL", "AUDIO", "UDP", "SIM_SHM"};
        ImGui::TextUnformatted(tr("Port", "端口"));
        ImGui::SameLine(100);
        ImGui::SetNextItemWidth(280);
        ui::Choice("##monitorPropPort", &chPropertyEdit_.deviceIndex, devices, IM_ARRAYSIZE(devices));
        if (chPropertyEdit_.deviceIndex == 1 || chPropertyEdit_.deviceIndex == 5) {
                ImGui::TextUnformatted(tr("SHM Name", "SHM 名称"));
                ImGui::SameLine(100);
                ImGui::SetNextItemWidth(280);
                ImGui::InputText("##monitorPropShm", chPropertyEdit_.shmName, sizeof(chPropertyEdit_.shmName));
        }
        if (chPropertyEdit_.deviceIndex == 5) {
                ImGui::TextUnformatted(tr("Frame field", "数据字段"));
                ImGui::SameLine(100);
                ImGui::SetNextItemWidth(280);
                ui::Choice("##monitorPropSimField", &chPropertyEdit_.simField, ava_sim::fields, ava_sim::fieldCount);
        }
        if (chPropertyEdit_.deviceIndex == 3 ||
            (chPropertyEdit_.deviceIndex == 5 && !ava_sim::writableField(static_cast<unsigned>(chPropertyEdit_.simField))))
                chPropertyEdit_.writable = false;
        else
                ui::AccessChoice(tr("Access mode", "读写模式"), chPropertyEdit_.writable);

        ImGui::Separator();
        if (ImGui::CollapsingHeader(tr("Unit conversion", "单位换算"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox(tr("Enable conversion", "启用单位换算"), &chPropertyEdit_.unitConversion.enabled);
                ImGui::BeginDisabled(!chPropertyEdit_.unitConversion.enabled);
                ui::UnitConversionFields(chPropertyEdit_.unitConversion);
                ImGui::EndDisabled();
        }

        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("Enum definition", "枚举定义"));
        int removeEnum = -1;
        for (int i = 0; i < static_cast<int>(chPropertyEdit_.enums.size()); ++i) {
                auto &entry = chPropertyEdit_.enums[i];
                ImGui::PushID(i);
                char enumName[128]{};
                snprintf(enumName, sizeof(enumName), "%s", entry.name.c_str());
                ImGui::SetNextItemWidth(180);
                if (ImGui::InputText("##enumName", enumName, sizeof(enumName)))
                        entry.name = enumName;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90);
                if (ImGui::InputScalar("##enumValue", ImGuiDataType_S64, &entry.val)) {
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("X"))
                        removeEnum = i;
                ImGui::PopID();
        }
        if (removeEnum >= 0)
                chPropertyEdit_.enums.erase(chPropertyEdit_.enums.begin() + removeEnum);
        if (ImGui::SmallButton(tr("+ Enum", "+ 枚举")))
                chPropertyEdit_.enums.push_back({"VALUE", 0});

        ImGui::Separator();
        if (ui::Button(tr("Apply", "应用"), ui::BtnStyle::Success, ImVec2(100, 0))) {
                auto it = chs_.find(chPropertyEdit_.targetName);
                if (it != chs_.end() && it->second) {
                        auto              ch      = it->second;
                        const std::string newName = chPropertyEdit_.name[0] ? chPropertyEdit_.name : chPropertyEdit_.targetName;
                        ch->setLabel(chPropertyEdit_.alias);
                        ch->setType(types[std::clamp(chPropertyEdit_.typeIndex, 0, IM_ARRAYSIZE(types) - 1)]);
                        try {
                                ch->setAddr(static_cast<usize>(std::stoull(chPropertyEdit_.address, nullptr, 16)));
                        } catch (...) {
                        }
                        const int deviceIndex = std::clamp(chPropertyEdit_.deviceIndex, 0, IM_ARRAYSIZE(devices) - 1);
                        ch->setDevice(devices[deviceIndex]);
                        ch->setShmRegionName(chPropertyEdit_.shmName);
                        ch->setSimField(static_cast<unsigned>(chPropertyEdit_.simField));
                        ch->setWritable(deviceIndex != 3 && chPropertyEdit_.writable);
                        if (deviceIndex == 5) {
                                ch->setType("f32");
                                ch->setAddrUnknown(false);
                        }
                        ch->setEnums(std::move(chPropertyEdit_.enums));
                        ch->unitConversion_ = chPropertyEdit_.unitConversion;
                        if (ch->getDevice() == "SHM")
                                shmInit(*ch);
                        if (newName != chPropertyEdit_.targetName && chs_.find(newName) == chs_.end()) {
                                auto node  = chs_.extract(it);
                                node.key() = newName;
                                ch->setName(newName);
                                chs_.insert(std::move(node));
                        }
                        if (parent_)
                                parent_->setModified();
                }
                chPropertyEdit_.targetName.clear();
                ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel", "取消"), ImVec2(100, 0))) {
                chPropertyEdit_.targetName.clear();
                ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
}

void
MonitorScope::drawTableRow(const std::string &chName, std::shared_ptr<MonitorChannel> &ch, const std::string &displayLabel)
{
        ImGui::PushID(chName.c_str());
        ui::HierarchyNextRow();

        // 1. Name (clickable for drag and context-menu hit testing only)
        ImGui::TableNextColumn();
        ui::HierarchyName();
        // Match the parent tree's compact indentation and arrow gutter.
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetTreeNodeToLabelSpacing());

        // Use alias when set; fall back to the trie display label (short segment)
        const bool  hasAlias = (ch->getLabel() != ch->getName());
        const char *label = hasAlias ? ch->getLabel().c_str() : (displayLabel.empty() ? chName.c_str() : displayLabel.c_str());
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
        if (ImGui::Selectable(
                label, false, ImGuiSelectableFlags_AllowOverlap, ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x), 0))) {
                selectedGroupPaths_.clear();
                for (auto &pair : chs_)
                        pair.second->selected_ = false;
        }
        ui::HierarchyLeaf(label);
        if (auto *row = ui::CurrentHierarchyRow()) {
                if (ImGui::IsItemHovered())
                        row->emphasis = std::max(row->emphasis, 1);
                if (searchSelectedChannel_ == chName)
                        row->emphasis = std::max(row->emphasis, 2);
        }
        if (searchScroll_ && searchSelectedChannel_ == chName) {
                ImGui::SetScrollHereY(0.5f);
                searchScroll_ = false;
        }
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ChannelMovePayload payload;
                payload.srcScope = this;
                payload.isGroup  = false;
                snprintf(payload.chName, sizeof(payload.chName), "%s", chName.c_str());
                ImGui::SetDragDropPayload("DND_CHANNEL_MOVE", &payload, sizeof(ChannelMovePayload));
                ImGui::Text(tr("Drag: %s", "拖动: %s"), chName.c_str());
                ImGui::EndDragDropSource();
        }
        // Drag or drop directly on the channel name.
        if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload *mv = ImGui::AcceptDragDropPayload("DND_CHANNEL_MOVE"))
                        applyChannelMoveDrop(static_cast<ChannelMovePayload *>(mv->Data), chName);
                ImGui::EndDragDropTarget();
        }
        ImGui::PopStyleColor(3);

        if (ui::HierarchyContextItem()) {
                if (ImGui::MenuItem(tr("Copy", "复制")))
                        ImGui::SetClipboardText(clipboardText({chName}).c_str());
                ImGui::Separator();
                // Right-click implies selection — auto-select this row so Delete works
                // without requiring a prior left-click
                if (!ch->selected_) {
                        for (auto &pair : chs_)
                                pair.second->selected_ = false;
                        ch->selected_ = true;
                }
                if (ImGui::MenuItem(tr("Delete", "删除"))) {
                        for (auto &pair : chs_)
                                if (pair.second->selected_)
                                        pair.second->markPendingDelete();
                }
                if (ImGui::MenuItem(tr("Edit Properties...", "编辑属性...")))
                        beginChannelProperties(chName);
                const u32  traceSize = typeBytes(ch->getType());
                const bool canTrace  = ch->getDevice() == "JLINK" && traceSize > 0;
                ImGui::BeginDisabled(!canTrace);
                if (ImGui::MenuItem(canTrace ? tr("Trace writes with DWT", "使用 DWT 跟踪写入")
                                             : tr("DWT trace (JLINK scalar only)", "DWT 跟踪（仅 JLINK 标量）")))
                        JLinkPort::instance().configureDwtWriteTrace(static_cast<u32>(ch->getAddr()),
                                                                     traceSize,
                                                                     ch->getSymbolName().empty() ? chName
                                                                                                 : ch->getSymbolName());
                ImGui::EndDisabled();
                ImGui::Separator();
                ImGui::TextDisabled("%s", tr("Access mode", "读写模式"));
                auto supportsWrite = [](const MonitorChannel &ch) {
                        const std::string &device = ch.getDevice();
                        return device == "JLINK" || device == "SHM" || device == "LOCAL" ||
                               (device == ava_sim::device && ava_sim::writableField(ch.getSimField()));
                };
                auto applyWritable = [&](bool writable) {
                        for (auto &pair : chs_) {
                                auto &selected = *pair.second;
                                if (!selected.selected_ || (writable && !supportsWrite(selected)))
                                        continue;
                                selected.setWritable(writable);
                                if (!writable) {
                                        selected.waveEnable_ = false;
                                        float discarded      = 0.0f;
                                        selected.consumeWValDirty(discarded);
                                }
                        }
                        if (parent_)
                                parent_->setModified();
                };
                bool accessWritable = ch->isWritable();
                if (ui::AccessChoice("##access", accessWritable, supportsWrite(*ch)))
                        applyWritable(accessWritable);
                ImGui::Separator();
                drawDisplayFormatMenu(*ch);
                // ── Alias / rename ───────────────────────────────────────────
                ImGui::Separator();
                ImGui::TextDisabled("%s", tr("Alias", "别名"));
                static char tableAliasBuf[256]{};
                if (ImGui::IsWindowAppearing())
                        snprintf(tableAliasBuf, sizeof(tableAliasBuf), "%s", hasAlias ? ch->getLabel().c_str() : "");
                ImGui::SetNextItemWidth(180);
                bool commitAlias =
                    ImGui::InputText("##tblAlias", tableAliasBuf, sizeof(tableAliasBuf), ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if (ImGui::SmallButton("OK"))
                        commitAlias = true;
                if (commitAlias) {
                        ch->setLabel(tableAliasBuf[0] != '\0' ? tableAliasBuf : "");
                        if (parent_)
                                parent_->setModified();
                }
                if (hasAlias) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton(tr("Reset##tblAlias", "重置##tblAlias"))) {
                                ch->setLabel("");
                                if (parent_)
                                        parent_->setModified();
                        }
                }
                ImGui::EndPopup();
        }

        // 2. Value (Interactive if writable)
        ImGui::TableNextColumn();
        const int displayRadix    = ch->getDisplayRadix();
        auto      formatEnumValue = [&](i64 val) {
                bool signedValue = true;
                u32  bitWidth    = 32;
                integerTypeInfo(ch->getType(), signedValue, bitWidth);
                if (ch->getBitSize() > 0)
                        bitWidth = std::min(ch->getBitSize(), bitWidth);
                return formatIntegerValue(static_cast<u64>(val), signedValue, bitWidth, displayRadix);
        };

        u64         flashBits    = 0;
        const bool  flashSampled = ch->getDisplayBits(flashBits) || !ch->read_.rawEmpty();
        const float changeFlash  = valueFlash_.update(chName + "#" + std::to_string(displayRadix),
                                                     flashSampled ? formatChannelValue(*ch, displayRadix) : "—");
        const bool  flashFrame   = changeFlash > 0 && ImGui::GetActiveID() != ImGui::GetID("##val");
        if (flashFrame) {
                ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
                accent.w      = .12f + .24f * changeFlash;
                const bool textOnly =
                    ch->isWritable() == false && (isIntegerType(ch->getType()) || ch->isEnum() || displayRadix == 10);
                ui::TableCellHighlight(changeFlash, textOnly ? ImGui::GetTextLineHeight() : ImGui::GetFrameHeight());
                ImGui::PushStyleColor(
                    ImGuiCol_FrameBg,
                    ui::MixSurface(ImGui::GetStyleColorVec4(ImGuiCol_FrameBg), accent, .15f + .25f * changeFlash));
        }
        if (!isIntegerType(ch->getType()) && !ch->isEnum() && displayRadix != 10) {
                std::string value = formatChannelValue(*ch, displayRadix);
                char        display[128];
                snprintf(display, sizeof(display), "%s", value.c_str());
                ImGui::InputText("##val", display, sizeof(display), ImGuiInputTextFlags_ReadOnly);
                if (ch->isWritable() && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        snprintf(ch->valueEditBuf_, sizeof(ch->valueEditBuf_), "%.9g", ch->getDispVal());
                        ImGui::OpenPopup("##editDecimalValue");
                }
                if (ImGui::BeginPopup("##editDecimalValue")) {
                        ImGui::TextUnformatted(tr("Value (decimal)", "数值（十进制）"));
                        if (ImGui::IsWindowAppearing())
                                ImGui::SetKeyboardFocusHere();
                        bool commit =
                            ImGui::InputText("##decimal",
                                             ch->valueEditBuf_,
                                             sizeof(ch->valueEditBuf_),
                                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                        commit |= ImGui::Button(tr("Apply", "应用"));
                        if (commit) {
                                char       *end     = nullptr;
                                const float numeric = std::strtof(ch->valueEditBuf_, &end);
                                if (end != ch->valueEditBuf_ && *end == '\0') {
                                        ch->setWVal(numeric);
                                        ch->markWValDirty();
                                        ImGui::CloseCurrentPopup();
                                }
                        }
                        ImGui::EndPopup();
                }
        } else if (!ch->isWritable()) {
                ui::TableText(formatChannelValue(*ch, displayRadix));
        } else if (ch->getDevice() == ava_sim::device) {
                // Retain the user's text while live telemetry changes underneath it
                if (!ch->valueEditActive_)
                        snprintf(ch->valueEditBuf_, sizeof(ch->valueEditBuf_), "%.6f", ch->simInputValue());
                ImGui::SetNextItemWidth(-1);
                const bool commit = ImGui::InputText("##val",
                                                     ch->valueEditBuf_,
                                                     sizeof(ch->valueEditBuf_),
                                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                if (ImGui::IsItemActivated())
                        ch->valueEditActive_ = true;
                if (commit || ImGui::IsItemDeactivatedAfterEdit()) {
                        char       *end       = nullptr;
                        const float displayed = std::strtof(ch->valueEditBuf_, &end);
                        const float raw       = ch->getGain() != 0 ? (displayed - ch->getBias()) / ch->getGain() : NAN;
                        if (end != ch->valueEditBuf_ && *end == '\0' && std::isfinite(raw)) {
                                ch->setWVal(raw);
                                ch->markWValDirty();
                        }
                }
                if (ImGui::IsItemDeactivated())
                        ch->valueEditActive_ = false;
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\n%s: %.6f",
                                          tr("Enter or leaving the input sends the target and disables its waveform generator. "
                                             "Position: rad, velocity: rad/s.",
                                             "回车或离开输入框即下发目标，并关闭该目标的波形发生器。位置 rad，速度 rad/s。"),
                                          ch->simWritePending() ? tr("Pending; applied", "等待应用；当前已应用")
                                                                : tr("Applied", "已应用"),
                                          ch->getDispVal());
        } else if (ch->isEnum()) {
                const auto              &entries  = ch->getEnums();
                const auto               current  = formatChannelInteger(*ch, displayRadix);
                u64                      rawBits  = 0;
                const bool               sampled  = ch->getDisplayBits(rawBits) || !ch->read_.rawEmpty();
                int                      selected = -1;
                std::vector<std::string> labels;
                labels.reserve(entries.size());
                for (size_t i = 0; i < entries.size(); ++i) {
                        const auto value = formatEnumValue(entries[i].val);
                        if (sampled && value == current)
                                selected = static_cast<int>(i);
                        labels.push_back(entries[i].name + " (" + value + ")");
                }
                const std::string preview = selected >= 0 ? labels[selected]
                                            : sampled     ? std::string(tr("Unknown", "未知")) + " (" + current + ")"
                                                          : tr("Not sampled", "未采样");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##enum", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
                        for (size_t i = 0; i < entries.size(); ++i) {
                                ImGui::PushID(static_cast<int>(i));
                                if (ImGui::Selectable(labels[i].c_str(), selected == static_cast<int>(i))) {
                                        ch->setWVal(static_cast<f32>(entries[i].val));
                                        ch->markWValDirty();
                                }
                                if (selected == static_cast<int>(i))
                                        ImGui::SetItemDefaultFocus();
                                ImGui::PopID();
                        }
                        ImGui::EndCombo();
                }
        } else {
                const std::string &t = ch->getType();
                ImGui::SetNextItemWidth(-1);

                if (isIntegerType(t)) {
                        bool signedValue = false;
                        u32  bitWidth    = 32;
                        integerTypeInfo(t, signedValue, bitWidth);
                        if (ch->getBitSize() > 0)
                                bitWidth = std::min(ch->getBitSize(), bitWidth);
                        if (!ch->valueEditActive_) {
                                const std::string current = formatChannelInteger(*ch, displayRadix);
                                snprintf(ch->valueEditBuf_, sizeof(ch->valueEditBuf_), "%s", current.c_str());
                        }
                        const bool commit =
                            ImGui::InputText("##val",
                                             ch->valueEditBuf_,
                                             sizeof(ch->valueEditBuf_),
                                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                        if (ImGui::IsItemActivated())
                                ch->valueEditActive_ = true;
                        if (commit) {
                                u64 raw = 0;
                                if (parseIntegerValue(ch->valueEditBuf_, displayRadix, signedValue, bitWidth, raw)) {
                                        ch->setWVal(integerRawToFloat(raw, signedValue, bitWidth));
                                        ch->markWValDirty();
                                }
                        }
                        if (ImGui::IsItemDeactivated())
                                ch->valueEditActive_ = false;
                } else {
                        f32 v = ch->getDispVal();
                        ImGui::InputFloat("##val", &v, 0, 0, "%.6f");
                        if (ImGui::IsItemDeactivated() &&
                            (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                                ch->setWVal(v);
                                ch->markWValDirty();
                        }
                }
        }
        if (flashFrame)
                ImGui::PopStyleColor();
        // 3. Type
        ImGui::TableNextColumn();
        ui::TableText(ch->isEnum() ? "enum (" + ch->getType() + ")" : ch->getType());

        // 4. Address
        ImGui::TableNextColumn();
        if (ch->isAddrUnknown())
                ui::TableText(tr("UNKNOWN", "未知"), ui::SemanticButtonPalette(ui::BtnStyle::Danger, ImGui::GetStyle()).text);
        else if (ch->getDevice() == ava_sim::device || ch->getDevice() == "SHM") {
                const char *region = ch->getShmRegionName().c_str();
                if (ch->getDevice() == ava_sim::device && !*region)
                        region = ava_sim::defaultRegion;
                ui::TableText(*region ? region : tr("Not configured", "未设置"));
                if (ch->getDevice() == ava_sim::device && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s: %s", tr("Field", "字段"), ava_sim::fields[ch->getSimField()]);
        } else if (ch->getDevice() == "LOCAL" || ch->getDevice() == "AUDIO" || ch->getDevice() == "UDP") {
                ImGui::TextDisabled("—");
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "%s: %s", tr("Source without a memory address", "此数据源无数值地址"), ch->getDevice().c_str());
        } else {
                char address[32];
                snprintf(address, sizeof(address), "0x%zX", ch->getAddr());
                ui::TableText(address);
        }

        // 5. Port
        ImGui::TableNextColumn();
        const std::string &dev = ch->getDevice();
        ui::TableText(dev == "JLINK" ? "J-Link" : dev.c_str());

        // 6. Wave Control
        ImGui::TableNextColumn();
        if (dev == ava_sim::device && !ch->isWritable()) {
                ImGui::TextDisabled("--");
        } else {
                f32 availX = ImGui::GetContentRegionAvail().x;
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0));
                const bool openWave = ui::Button("...",
                                                 ch->waveEnable_ ? ui::BtnStyle::Success : ui::BtnStyle::Neutral,
                                                 ImVec2(availX, ImGui::GetTextLineHeight()));
                ImGui::PopStyleVar();
                if (openWave)
                        ImGui::OpenPopup("WaveCfg");
        }

        if ((dev != ava_sim::device || ch->isWritable()) && ImGui::BeginPopup("WaveCfg")) {
                // Read current values from atomic shadow fields
                auto &pending = ch->waveCfgPending_;
                ImGui::Text(tr("Wave Generator: %s", "波形发生器: %s"), chName.c_str());
                ImGui::Separator();

                // ON / OFF toggle at the top of the popup
                if (ch->waveEnable_) {
                        if (ui::Button(
                                tr("ON  (click to disable)", "ON（点击关闭）"), ui::BtnStyle::Success, ImVec2(-FLT_MIN, 0)))
                                ch->waveEnable_ = false;
                } else {
                        if (ui::Button(
                                tr("OFF (click to enable)", "OFF（点击开启）"), ui::BtnStyle::Muted, ImVec2(-FLT_MIN, 0))) {
                                ch->waveEnable_ = true;
                                pending.restart.store(true);
                        }
                }
                ImGui::Separator();

                const char *modes[]     = {tr("Basic waveform", "基础波形"), tr("Dynamic waveform", "动态波形")};
                i32         currentMode = pending.mode.load();
                ImGui::SetNextItemWidth(220.0f);
                if (ui::Choice(tr("Mode", "模式"), &currentMode, modes, IM_ARRAYSIZE(modes))) {
                        if (currentMode == static_cast<i32>(WaveGeneratorMode::DYNAMIC) &&
                            !pending.dynamicInitialized.exchange(true)) {
                                const f32 basic[] = {
                                    pending.freq.load(), pending.amp.load(), 0.0f, pending.offset.load(), pending.duty.load()};
                                const f32 steps[] = {1.0f, 1.0f, 15.0f, 1.0f, 0.05f};
                                for (usize i = 0; i < MonitorChannel::WaveCfgPending::PARAM_COUNT; ++i) {
                                        pending.params[i].start.store(basic[i]);
                                        pending.params[i].end.store(basic[i]);
                                        pending.params[i].step.store(steps[i]);
                                }
                        }
                        pending.mode.store(currentMode);
                        pending.dirty.store(true);
                        pending.restart.store(true);
                }

                const char *types[]     = {tr("Sine", "正弦"), tr("Square", "方波"), tr("Tritheta", "三角")};
                i32         currentType = pending.type.load();
                ImGui::SetNextItemWidth(220.0f);
                if (ui::Choice(tr("Wave type", "波形类型"), &currentType, types, IM_ARRAYSIZE(types))) {
                        pending.type.store(currentType);
                        pending.dirty.store(true);
                        pending.restart.store(true);
                }

                auto inputBasic = [&](const char *label, std::atomic<f32> &val, const char *format) {
                        f32 v = val.load();
                        ImGui::SetNextItemWidth(140.0f);
                        if (inputWaveFloatOnEnter(label, v, ch->waveInputDrafts_[ImGui::GetID(label)], format)) {
                                val.store(v);
                                pending.dirty.store(true);
                        }
                };

                if (currentMode == static_cast<i32>(WaveGeneratorMode::BASIC)) {
                        inputBasic(tr("Frequency (Hz)", "频率 (Hz)"), pending.freq, "%.3f");
                        inputBasic(tr("Amplitude", "幅值"), pending.amp, "%.3f");
                        inputBasic(tr("Offset", "偏置"), pending.offset, "%.3f");
                        if (currentType != WAVE_TYPE_SINE)
                                inputBasic(tr("Duty (0..1)", "占空比 (0..1)"), pending.duty, "%.3f");
                } else {
                        ImGui::TextDisabled("%s",
                                            tr("Each parameter has its own time law; edits restart the timeline.",
                                               "每个参数可独立设置变化规律；修改参数会重新计时。"));

                        auto drawDynamicParam = [&](const char                                *label,
                                                    MonitorChannel::WaveCfgPending::ParamIndex index,
                                                    const char                                *valueFormat) {
                                auto &param = pending.params[index];
                                ImGui::PushID(static_cast<int>(index));
                                if (ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) {
                                        const char *laws[] = {
                                            tr("Fixed", "固定"), tr("Linear", "线性变化"), tr("Step", "阶跃变化")};
                                        i32 law = param.law.load();
                                        ImGui::SetNextItemWidth(140.0f);
                                        if (ui::Choice(tr("Law", "规律"), &law, laws, IM_ARRAYSIZE(laws))) {
                                                param.law.store(law);
                                                pending.restart.store(true);
                                        }

                                        auto inputParam =
                                            [&](const char *field, std::atomic<f32> &val, const char *format, bool positive) {
                                                    f32 v = val.load();
                                                    ImGui::SetNextItemWidth(140.0f);
                                                    if (inputWaveFloatOnEnter(
                                                            field, v, ch->waveInputDrafts_[ImGui::GetID(field)], format)) {
                                                            if (positive)
                                                                    v = std::max(v, 0.000001f);
                                                            val.store(v);
                                                            pending.restart.store(true);
                                                    }
                                            };

                                        inputParam(tr("Value / Start", "固定值 / 起点"), param.start, valueFormat, false);
                                        if (law != static_cast<i32>(WaveDynamicLaw::FIXED))
                                                inputParam(tr("End", "终点"), param.end, valueFormat, false);
                                        if (law == static_cast<i32>(WaveDynamicLaw::LINEAR))
                                                inputParam(tr("Duration (s)", "持续时间 (s)"), param.duration, "%.3f", true);
                                        if (law == static_cast<i32>(WaveDynamicLaw::STEP)) {
                                                inputParam(tr("Step increment", "每次增量"), param.step, valueFormat, false);
                                                inputParam(
                                                    tr("Step interval (s)", "阶跃间隔 (s)"), param.interval, "%.3f", true);
                                        }
                                        if (law != static_cast<i32>(WaveDynamicLaw::FIXED)) {
                                                bool repeat = param.repeat.load();
                                                if (ImGui::Checkbox(tr("Repeat", "循环"), &repeat)) {
                                                        param.repeat.store(repeat);
                                                        pending.restart.store(true);
                                                }
                                        }
                                }
                                ImGui::PopID();
                        };

                        drawDynamicParam(tr("Frequency (Hz)", "频率 (Hz)"), MonitorChannel::WaveCfgPending::FREQUENCY, "%.3f");
                        drawDynamicParam(tr("Amplitude", "幅值"), MonitorChannel::WaveCfgPending::AMPLITUDE, "%.3f");
                        drawDynamicParam(tr("Phase (deg)", "相位 (度)"), MonitorChannel::WaveCfgPending::PHASE, "%.3f");
                        drawDynamicParam(tr("Offset", "偏置"), MonitorChannel::WaveCfgPending::OFFSET, "%.3f");
                        if (currentType != WAVE_TYPE_SINE)
                                drawDynamicParam(
                                    tr("Duty (0..1)", "占空比 (0..1)"), MonitorChannel::WaveCfgPending::DUTY, "%.3f");
                }

                ImGui::Separator();
                if (ImGui::Button(tr("Close", "关闭")))
                        ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
        }

        ImGui::PopID();
}

bool
MonitorScope::isDisplayHidden() const
{
        // Hover focus is a temporary view. Keep the saved visibility untouched
        // until the user clicks to commit; leaving the trigger restores it.
        return hidden_ && (!parent_ || !parent_->isScopeFocused(name_));
}

void
MonitorScope::setHidden(bool hidden)
{
        if (hidden_ == hidden)
                return;
        hidden_ = hidden;
        if (hidden)
                cancelDiscardSelection();
        if (parent_)
                parent_->requestScopeLayout();
}

void
MonitorScope::plotMenu()
{
}

const char *
MonitorScope::discardUnavailableReason() const
{
        if (!parent_)
                return tr("This data source does not support discarding intervals.", "此数据源不支持按区间丢弃。");
        if (pendingDiscardTimeRange_)
                return tr("The selected interval is being discarded. Please wait.", "区间数据正在丢弃，请稍候。");
        if (showFft_ || e_draw != DrawEnum::PLOT)
                return tr("Select an interval in the time-domain plot.", "请在时域图形视图中框选时间段。");
        if (hidden_)
                return tr("Show this scope to select an interval.", "显示此 Scope 后可框选时间段。");
        for (const auto &[_, ch] : chs_)
                if (ch && !ch->isPendingDelete() && ch->arraySnapshot_)
                        return tr("This scope contains Flash array data with an index axis and cannot discard a time interval.",
                                  "此 Scope 包含以索引为横轴的 Flash 数组，无法按时间段丢弃。");
        return nullptr;
}

void
MonitorScope::beginDiscardSelection()
{
        if (discardUnavailableReason())
                return;
        discardSelection_         = {};
        discardSelection_.enabled = true;
        discardSelection_.viewMin = lastPlotXMin_;
        discardSelection_.viewMax = lastPlotXMax_;
        discardResultUntilMs_     = 0;
}

bool
MonitorScope::discardDataAvailable() const
{
        if (isPendingDelete())
                return false;
        for (const auto &[_, ch] : chs_)
                if (ch && !ch->isPendingDelete() && ch->arraySnapshot_)
                        return false;
        return true;
}

usize
MonitorScope::discardPointCount(double minimum, double maximum) const
{
        if (!discardDataAvailable())
                return 0;
        usize count = 0;
        for (const auto &[_, ch] : chs_)
                if (ch && !ch->isPendingDelete()) {
                        const auto &data  = ch->read_;
                        const usize begin = data.rawLowerBound(minimum), end = data.rawUpperBound(maximum);
                        if (end > begin)
                                count += end - begin;
                }
        return count;
}

usize
MonitorScope::selectedDiscardPointCount() const
{
        if (!discardSelection_.hasRange && !discardSelection_.dragging)
                return 0;
        const double minimum = std::min(discardSelection_.start, discardSelection_.end);
        const double maximum = std::max(discardSelection_.start, discardSelection_.end);
        if (!std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum)
                return 0;
        if (parent_ && parent_->linkScopeCursor_) {
                usize count = 0;
                for (const auto &[_, scope] : parent_->scopes_)
                        if (scope)
                                count += scope->discardPointCount(minimum, maximum);
                return count;
        }
        return discardPointCount(minimum, maximum);
}

bool
MonitorScope::submitDiscardSelection()
{
        if (discardUnavailableReason() || !discardSelection_.hasRange || discardSelection_.dragging ||
            selectedDiscardPointCount() == 0)
                return false;
        const double minimum = std::min(discardSelection_.start, discardSelection_.end);
        const double maximum = std::max(discardSelection_.start, discardSelection_.end);
        const auto   queue   = [&](MonitorScope &scope) {
                scope.pendingDiscardMin_       = minimum;
                scope.pendingDiscardMax_       = maximum;
                scope.pendingDiscardTimeRange_ = true;
        };
        // Capture the target scopes now; a later sync toggle cannot change a queued operation
        if (parent_ && parent_->linkScopeCursor_) {
                for (const auto &[_, scope] : parent_->scopes_)
                        if (scope && scope->discardDataAvailable() && scope->pendingDiscardTimeRange_)
                                return false;
                for (auto &[_, scope] : parent_->scopes_)
                        if (scope && scope->discardDataAvailable())
                                queue(*scope);
        } else {
                queue(*this);
        }
        if (parent_)
                parent_->setDiscardSelectionEnabled(false);
        else
                cancelDiscardSelection();
        return true;
}

bool
MonitorScope::consumeDiscardTimeRange(double &minimum, double &maximum)
{
        if (!pendingDiscardTimeRange_)
                return false;
        minimum                  = pendingDiscardMin_;
        maximum                  = pendingDiscardMax_;
        pendingDiscardTimeRange_ = false;
        return true;
}

usize
MonitorScope::processDiscardTimeRangeRequest()
{
        double minimum, maximum;
        if (!consumeDiscardTimeRange(minimum, maximum))
                return 0;
        discardRejected_ = !std::isfinite(minimum) || !std::isfinite(maximum) || maximum < minimum;
        for (const auto &[_, ch] : chs_)
                if (ch && !ch->isPendingDelete() && ch->arraySnapshot_)
                        discardRejected_ = true;
        lastDiscardCount_ = 0;
        if (!discardRejected_) {
                for (auto &[_, ch] : chs_)
                        if (ch && !ch->isPendingDelete())
                                lastDiscardCount_ += ch->discardTimeRange(minimum, maximum);
                chStats_.clear();
                chPeaks_.clear();
                hoverSample_ = {};
                hoverValues_.clear();
                dxs_.clear();
                dys_.clear();
                {
                        std::lock_guard lock(fftPubMtx_);
                        fftDataGeneration_.fetch_add(1, std::memory_order_relaxed);
                        fftPublished_.clear();
                }
                fftLastRunMs_.store(0, std::memory_order_relaxed);
        }
        discardResultUntilMs_ = get_mono_ts_ms() + 4000;
        return lastDiscardCount_;
}

void
MonitorScope::drawDiscardSelectionControls()
{
        if (discardSelection_.enabled && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                if (parent_)
                        parent_->setDiscardSelectionEnabled(false);
                else
                        cancelDiscardSelection();
        }
        if (discardSelection_.enabled && !discardSelection_.hasRange && !discardSelection_.dragging)
                return;
        if (discardSelection_.enabled) {
                const usize count = selectedDiscardPointCount();
                if (discardSelection_.hasRange || discardSelection_.dragging) {
                        ImGui::TextWrapped(
                            parent_ && parent_->linkScopeCursor_
                                ? tr("Selected X interval: %.9g to %.9g | %llu points across this monitor",
                                     "已选横轴区间：%.9g 至 %.9g | 当前 Monitor 全部 Scope 共 %llu 个点")
                                : tr("Selected X interval: %.9g to %.9g | %llu points across all channels in this scope",
                                     "已选横轴区间：%.9g 至 %.9g | 当前 Scope 全部通道共 %llu 个点"),
                            std::min(discardSelection_.start, discardSelection_.end),
                            std::max(discardSelection_.start, discardSelection_.end),
                            static_cast<unsigned long long>(count));
                }
                ImGui::BeginDisabled(!discardSelection_.hasRange || discardSelection_.dragging || count == 0);
                const bool discard = ui::Button(tr("Discard selected interval###discardApply", "丢弃选中区间###discardApply"),
                                                ui::BtnStyle::Danger);
                ImGui::EndDisabled();
                if (discard)
                        submitDiscardSelection();
                ImGui::SameLine();
                if (ImGui::Button(tr("Cancel###discardCancel", "取消###discardCancel"))) {
                        if (parent_)
                                parent_->setDiscardSelectionEnabled(false);
                        else
                                cancelDiscardSelection();
                }
        } else if (pendingDiscardTimeRange_) {
                ImGui::TextDisabled("%s", tr("Discarding the selected interval...", "正在丢弃选中区间…"));
        } else if (get_mono_ts_ms() < discardResultUntilMs_) {
                if (discardRejected_)
                        ImGui::TextWrapped("%s",
                                           tr("The scope now contains array data; no interval was discarded.",
                                              "此 Scope 现包含数组数据，未丢弃时间段。"));
                else
                        ImGui::TextDisabled(tr("This scope discarded %llu points; original timestamps retained.",
                                               "此 Scope 已丢弃 %llu 个点，保留原时间戳。"),
                                            static_cast<unsigned long long>(lastDiscardCount_));
        }
}

void
MonitorScope::plotDiscardSelection()
{
        auto        &selection = discardSelection_;
        const auto   mouse     = ImGui::GetMousePos();
        const auto   limits    = ImPlot::GetPlotLimits();
        const bool   hovered   = ImPlot::IsPlotHovered() && !ImPlot::GetCurrentPlot()->Items.Legend.Rect.Contains(mouse);
        const double mouseX    = ImPlot::GetPlotMousePos().x;
        if (hovered)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (hovered && std::isfinite(mouseX) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (parent_)
                        for (auto &[_, scope] : parent_->scopes_)
                                if (scope.get() != this) {
                                        scope->discardSelection_.hasRange = false;
                                        scope->discardSelection_.dragging = false;
                                }
                selection.dragging = true;
                selection.hasRange = false;
                selection.start = selection.end = std::clamp(mouseX, limits.X.Min, limits.X.Max);
                selection.startPixel            = mouse.x;
        }
        if (selection.dragging) {
                if (std::isfinite(mouseX))
                        selection.end = std::clamp(mouseX, limits.X.Min, limits.X.Max);
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        selection.dragging = false;
                        selection.hasRange =
                            std::abs(mouse.x - selection.startPixel) >= 3.0f && selection.start != selection.end;
                }
        }
        const auto *displaySelection = &selection;
        if (!selection.dragging && !selection.hasRange && parent_ && parent_->linkScopeCursor_)
                for (const auto &[_, scope] : parent_->scopes_)
                        if (scope && !scope->isPendingDelete() &&
                            (scope->discardSelection_.dragging || scope->discardSelection_.hasRange)) {
                                displaySelection = &scope->discardSelection_;
                                break;
                        }
        if (!displaySelection->dragging && !displaySelection->hasRange)
                return;
        const auto  pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
        const float left  = ImPlot::PlotToPixels(std::min(displaySelection->start, displaySelection->end), limits.Y.Min).x;
        const float right = ImPlot::PlotToPixels(std::max(displaySelection->start, displaySelection->end), limits.Y.Min).x;
        ImPlot::PushPlotClipRect();
        auto *draw = ImPlot::GetPlotDrawList();
        draw->AddRectFilled(
            ImVec2(left, pos.y), ImVec2(right, pos.y + size.y), ImGui::GetColorU32(ImVec4(1.0f, .35f, .25f, .20f)));
        draw->AddRect(
            ImVec2(left, pos.y), ImVec2(right, pos.y + size.y), ImGui::GetColorU32(ImVec4(1.0f, .45f, .30f, .95f)), 0, 0, 1.5f);
        ImPlot::PopPlotClipRect();
}

/**
 * @brief Runs on the dedicated FFT worker thread. Copies the visible window for each
 *        shown ch (briefly holding monitorMtx), transforms off-lock, and publishes
 *        freqs/mags/peaks for the GUI to draw. Keeps fft_exec off the render thread
 */
void
MonitorScope::fftWorkerStep(std::mutex &monitorMtx)
{
        if (!showFft_) {
                // Drop any stale results so old spectra don't linger after switching to TIME
                std::lock_guard lk(fftPubMtx_);
                if (!fftPublished_.empty())
                        fftPublished_.clear();
                return;
        }

        const f64 xmin = fftWinMin_.load(std::memory_order_relaxed);
        const f64 xmax = fftWinMax_.load(std::memory_order_relaxed);
        if (fftPoints_ <= 0)
                return;

        const u64 nowMs       = get_mono_ts_ms();
        const u64 minPeriodMs = (fftPoints_ >= 1048576)  ? 500
                                : (fftPoints_ >= 524288) ? 250
                                : (fftPoints_ >= 131072) ? 100
                                : (fftPoints_ >= 32768)  ? 33
                                                         : 16;
        const u64 prevRunMs   = fftLastRunMs_.load(std::memory_order_relaxed);
        if (prevRunMs != 0 && nowMs - prevRunMs < minPeriodMs)
                return;
        fftLastRunMs_.store(nowMs, std::memory_order_relaxed);

        // 1. Gather visible samples per shown ch (short lock on the monitor mtx)
        struct Job {
                std::string      name;
                std::vector<f32> samples; // already mean-detrended
                f32              fs;
        };
        std::vector<Job> jobs;
        u64              dataGeneration = 0;
        {
                std::unique_lock lk(monitorMtx, std::try_to_lock);
                if (!lk.owns_lock())
                        return;
                dataGeneration = fftDataGeneration_.load(std::memory_order_relaxed);
                const int fftN = fftPoints_;
                for (auto &[name, ch] : chs_) {
                        if (!ch || !ch->show_)
                                continue;
                        if (ch->arraySnapshot_) {
                                const auto  &a = *ch->arraySnapshot_;
                                const size_t first =
                                    static_cast<size_t>(std::clamp(std::ceil(xmin / a.step), 0.0, double(a.values.size())));
                                const size_t prev = static_cast<size_t>(
                                    std::clamp(std::floor(xmax / a.step) + 1, 0.0, double(a.values.size())));
                                if (prev <= first)
                                        continue;
                                const size_t count = std::min(prev - first, static_cast<size_t>(fftN));
                                Job          j;
                                j.name = name;
                                j.fs   = static_cast<float>(std::clamp(1.0 / a.step, 0.1, 10000000.0));
                                j.samples.reserve(count);
                                double sum = 0;
                                for (size_t i = prev - count; i < prev; ++i) {
                                        const float v = static_cast<float>(a.values[i]);
                                        j.samples.push_back(v);
                                        sum += v;
                                }
                                const float mean = static_cast<float>(sum / count);
                                for (auto &v : j.samples)
                                        v -= mean;
                                jobs.push_back(std::move(j));
                                continue;
                        }
                        const auto &rd           = ch->read_;
                        const usize startIdx     = rd.rawLowerBound(xmin);
                        const usize endIdx       = rd.rawUpperBound(xmax);
                        const usize visibleCount = (endIdx > startIdx) ? (endIdx - startIdx) : 0;
                        if (visibleCount == 0)
                                continue;

                        const usize copyCount  = std::min(visibleCount, static_cast<usize>(fftN));
                        const usize readOffset = (visibleCount > copyCount) ? (visibleCount - copyCount) : 0;

                        Job j;
                        j.name = name;
                        j.samples.resize(copyCount);
                        f64 sum = 0;
                        for (usize i = 0; i < copyCount; ++i) {
                                f32 v         = rd.rawVal(startIdx + readOffset + i);
                                j.samples[i]  = v;
                                sum          += v;
                        }
                        const f32 mean = static_cast<f32>(sum / static_cast<f64>(copyCount));
                        for (auto &v : j.samples)
                                v -= mean;

                        f32 fs = (parent_) ? (f32)parent_->getHz() : 1000.0f;
                        if (copyCount > 1) {
                                f64 totalTime =
                                    rd.rawTs(startIdx + readOffset + copyCount - 1) - rd.rawTs(startIdx + readOffset);
                                if (totalTime > 1e-9)
                                        fs = static_cast<f32>((f64)(copyCount - 1) / totalTime);
                        }
                        if (fs < 0.1f)
                                fs = 0.1f;
                        if (fs > 10000000.0f)
                                fs = 10000000.0f;
                        j.fs = fs;
                        jobs.push_back(std::move(j));
                }
        }

        if (jobs.empty()) {
                std::lock_guard lk(fftPubMtx_);
                if (dataGeneration == fftDataGeneration_.load(std::memory_order_relaxed))
                        fftPublished_.clear();
                return;
        }

        // 2. Transform off-lock. fft_ + its buffers are guarded by fftObjMtx_ so a
        // concurrent reinitFft() can't pull the buffers out from under us
        std::map<std::string, FftResult> out;
        {
                std::lock_guard lk(fftObjMtx_);
                const int       N    = fftPoints_; // re-read under the same lock as reinitFft
                const int       half = N / 2 + 1;
                if (N <= 0 || !fft_.lo.p || (int)fftMagF32_.size() < half || (int)fftLoBuf_.size() < N)
                        return;

                for (auto &j : jobs) {
                        std::fill(fftInBuf_.begin(), fftInBuf_.end(), 0.0f);
                        const usize cc = std::min(j.samples.size(), static_cast<usize>(N));
                        for (usize i = 0; i < cc; ++i)
                                fftInBuf_[i] = j.samples[i];

                        // Submit a complete, zero-padded frame through the input queue
                        // Writing its backing buffer alone leaves the queue empty
                        spsc_reset(&fft_.lo.spsc);
                        const usize frameBytes = static_cast<usize>(N) * sizeof(f32);
                        if (spsc_write(&fft_.lo.spsc, fftInBuf_.data(), frameBytes) != frameBytes)
                                continue;
                        fft_.cfg.fs          = j.fs;
                        fft_.lo.exec_pending = 1;
                        if (fft_exec(&fft_) < 0)
                                continue;

                        const f32 df = j.fs / (f32)N;
                        FftResult r;
                        auto      magAt = [&](int i) { return (f64)fftMagF32_[i] * 2.0 / (f64)N; };

                        if (fftPeakCount_ > 0) {
                                for (int i = 2; i < half - 2; ++i)
                                        if (magAt(i) > magAt(i - 1) && magAt(i) > magAt(i + 1))
                                                r.peaks.push_back({(f64)i * (f64)df, magAt(i)});
                                std::sort(
                                    r.peaks.begin(), r.peaks.end(), [](const Peak &a, const Peak &b) { return a.mag > b.mag; });
                                if ((int)r.peaks.size() > fftPeakCount_)
                                        r.peaks.resize(fftPeakCount_);
                        }

                        constexpr int kMaxFftPlotBins = 8192;
                        if (half <= kMaxFftPlotBins) {
                                r.df = df;
                                r.freqs.resize(half);
                                r.mags.resize(half);
                                for (int i = 0; i < half; ++i) {
                                        r.freqs[i] = (f64)i * (f64)df;
                                        r.mags[i]  = magAt(i);
                                }
                        } else {
                                r.df = (f64)df * (f64)half / (f64)kMaxFftPlotBins;
                                r.freqs.reserve(kMaxFftPlotBins);
                                r.mags.reserve(kMaxFftPlotBins);
                                for (int b = 0; b < kMaxFftPlotBins; ++b) {
                                        const int b0 = (int)((i64)b * half / kMaxFftPlotBins);
                                        const int b1 = std::max(b0 + 1, (int)(((i64)b + 1) * half / kMaxFftPlotBins));
                                        int       mi = b0;
                                        f64       mm = magAt(b0);
                                        for (int i = b0 + 1; i < b1 && i < half; ++i) {
                                                const f64 m = magAt(i);
                                                if (m > mm) {
                                                        mm = m;
                                                        mi = i;
                                                }
                                        }
                                        r.freqs.push_back((f64)mi * (f64)df);
                                        r.mags.push_back(mm);
                                }
                        }
                        out[j.name] = std::move(r);
                }
        }

        // 3. Publish
        {
                std::lock_guard lk(fftPubMtx_);
                if (dataGeneration == fftDataGeneration_.load(std::memory_order_relaxed))
                        fftPublished_ = std::move(out);
        }
}

void
MonitorScope::collectCursorValues(double                                                                  mouseX,
                                  const std::function<void(double, double, double, const std::string &)> &onSample)
{
        hoverValues_.clear();
        hoverCursorX_ = mouseX;
        for (const auto &[name, ch] : chs_) {
                if (!ch || !ch->show_)
                        continue;
                const auto *color = ch->getColor();
                hoverValues_.push_back({false,
                                        0,
                                        ch->getLabel(),
                                        ImGui::ColorConvertFloat4ToU32(ImVec4(color[0], color[1], color[2], color[3])),
                                        ch->getOrder(),
                                        name});
                auto &val      = hoverValues_.back();
                auto  consider = [&](double x, double y, double raw) {
                        if (std::isfinite(x) && std::isfinite(raw) &&
                            (!std::isfinite(val.sampleX) || std::abs(x - mouseX) < std::abs(val.sampleX - mouseX))) {
                                val.sampleX = x;
                                val.raw     = raw;
                        }
                        onSample(x, y, raw, name);
                };
                // Use a common X for every series, not a different nearest timestamp per channel
                // Do not extrapolate beyond recorded data or bridge a non-finite sample
                auto sampleAt = [&](size_t count, size_t right, auto xAt, auto yAt, bool stairs) {
                        if (!count)
                                return;
                        double y = std::numeric_limits<double>::quiet_NaN();
                        if (right < count && xAt(right) == mouseX)
                                y = yAt(right);
                        else if (right > 0 && right < count) {
                                const double x0 = xAt(right - 1), x1 = xAt(right);
                                const double y0 = yAt(right - 1), y1 = yAt(right);
                                if (std::isfinite(x0) && std::isfinite(x1) && x0 < x1 && x0 <= mouseX && mouseX <= x1 &&
                                    std::isfinite(y0) && std::isfinite(y1))
                                        y = stairs ? y0 : std::lerp(y0, y1, (mouseX - x0) / (x1 - x0));
                        }
                        val.valid = std::isfinite(y);
                        val.y     = y;
                };
                // Inspect original samples, never the min/max LOD envelope used for drawing
                // Only the two neighbours of the mouse time are needed; history can be very large
                if (showFft_) {
                        std::lock_guard lock(fftPubMtx_);
                        auto            it = fftPublished_.find(name);
                        if (it == fftPublished_.end())
                                continue;
                        const auto  &r     = it->second;
                        const size_t count = std::min(r.freqs.size(), r.mags.size());
                        const size_t i = std::lower_bound(r.freqs.begin(), r.freqs.begin() + count, mouseX) - r.freqs.begin();
                        if (fftBars_ && count) {
                                size_t bin = std::min(i, count - 1);
                                if (i > 0 && std::abs(r.freqs[i - 1] - mouseX) < std::abs(r.freqs[bin] - mouseX))
                                        bin = i - 1;
                                val.y     = r.mags[bin];
                                val.valid = std::isfinite(val.y) && r.df > 0 && std::abs(r.freqs[bin] - mouseX) <= r.df * 0.45;
                        } else
                                sampleAt(
                                    count, i, [&](size_t j) { return r.freqs[j]; }, [&](size_t j) { return r.mags[j]; }, false);
                        if (i < count)
                                consider(r.freqs[i], r.mags[i], r.mags[i]);
                        if (i > 0)
                                consider(r.freqs[i - 1], r.mags[i - 1], r.mags[i - 1]);
                } else if (ch->arraySnapshot_) {
                        const auto &a = *ch->arraySnapshot_;
                        if (a.values.empty() || !(a.step > 0) || !std::isfinite(a.step))
                                continue;
                        const size_t i =
                            static_cast<size_t>(std::clamp(std::ceil(mouseX / a.step), 0.0, double(a.values.size())));
                        sampleAt(
                            a.values.size(),
                            i,
                            [&](size_t j) { return j * a.step; },
                            [&](size_t j) { return a.values[j] * ch->getGain() + ch->getBias(); },
                            ch->getPlotStyle() == 1);
                        if (i < a.values.size())
                                consider(i * a.step, a.values[i] * ch->getGain() + ch->getBias(), a.values[i]);
                        if (i > 0)
                                consider((i - 1) * a.step, a.values[i - 1] * ch->getGain() + ch->getBias(), a.values[i - 1]);
                } else {
                        const auto  &data = ch->read_;
                        const size_t i    = data.rawLowerBound(mouseX);
                        sampleAt(
                            data.rawSize(),
                            i,
                            [&](size_t j) { return data.rawTs(j); },
                            [&](size_t j) { return double(data.rawVal(j)) * ch->getGain() + ch->getBias(); },
                            ch->getPlotStyle() == 1);
                        if (i < data.rawSize())
                                consider(data.rawTs(i), double(data.rawVal(i)) * ch->getGain() + ch->getBias(), data.rawVal(i));
                        if (i > 0)
                                consider(data.rawTs(i - 1),
                                         double(data.rawVal(i - 1)) * ch->getGain() + ch->getBias(),
                                         data.rawVal(i - 1));
                }
        }
        std::sort(hoverValues_.begin(), hoverValues_.end(), [](const auto &a, const auto &b) {
                return a.order != b.order ? a.order < b.order : a.label < b.label;
        });
}

MonitorTimeMeasurement &
MonitorScope::timeMeasurement()
{
        return parent_ ? parent_->timeMeasurement_ : localTimeMeasurement_;
}

const MonitorTimeMeasurement &
MonitorScope::timeMeasurement() const
{
        return parent_ ? parent_->timeMeasurement_ : localTimeMeasurement_;
}

void
MonitorScope::plotInspect(MonitorViewMode &mode)
{
        hoverSample_ = {};
        hoverValues_.clear();
        const auto   mouse        = ImGui::GetMousePos();
        const bool   hovered      = ImPlot::IsPlotHovered() && !ImPlot::GetCurrentPlot()->Items.Legend.Rect.Contains(mouse);
        const double mouseX       = ImPlot::GetPlotMousePos().x;
        const auto   limits       = ImPlot::GetPlotLimits();
        double       bestDistance = 12.0 * 12.0;
        if (hovered && std::isfinite(mouseX)) {
                hoverCursorX_ = mouseX;
                auto consider = [&](double x, double y, double raw, const std::string &channel) {
                        if (!std::isfinite(x) || !std::isfinite(y) || !limits.Contains(x, y))
                                return;
                        const auto   pixel = ImPlot::PlotToPixels(x, y);
                        const double dx = pixel.x - mouse.x, dy = pixel.y - mouse.y;
                        const double distance = dx * dx + dy * dy;
                        if (distance < bestDistance) {
                                bestDistance = distance;
                                hoverSample_ = {true, x, y, chs_.at(channel)->getLabel(), channel, raw};
                        }
                };
                collectCursorValues(mouseX, consider);
        }

        const auto formatHoverValue = [&](const HoverValue &value, const char *unit) {
                if (!value.valid)
                        return std::string("---");
                char text[192];
                snprintf(text, sizeof(text), "%.7g %s", value.y, unit ? unit : "");
                std::string result  = text;
                const auto  channel = chs_.find(value.channel);
                if (channel != chs_.end() && channel->second)
                        result += ui::UnitConversionSuffix(value.raw, channel->second->unitConversion_);
                return result;
        };

        const bool linked = parent_ && parent_->linkScopeCursor_ && !showFft_;
        if (linked) {
                if (hovered && std::isfinite(mouseX)) {
                        parent_->linkedCursorActive_ = true;
                        parent_->linkedCursorTime_   = mouseX;
                }
                const auto   pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
                const ImRect rect(pos, ImVec2(pos.x + size.x, pos.y + size.y));
                ImRect       clip = rect;
                clip.ClipWith(ImGui::GetCurrentWindow()->ClipRect);
                const auto low  = ImPlot::PlotToPixels(limits.X.Min, limits.Y.Min);
                const auto high = ImPlot::PlotToPixels(limits.X.Max, limits.Y.Max);
                auto      *draw = ImPlot::GetPlotDrawList();
                // Draw after every scope has been visited, so hovering a lower
                // scope updates earlier scopes at the same time in this frame
                parent_->linkedCursorDraws_.push_back([this, rect, clip, limits, low, high, draw, formatHoverValue](double x) {
                        collectCursorValues(x, [](double, double, double, const std::string &) {});
                        if (clip.GetWidth() <= 0 || clip.GetHeight() <= 0 || hoverValues_.empty())
                                return;
                        const float px = low.x + float((x - limits.X.Min) / limits.X.Size()) * (high.x - low.x);
                        draw->PushClipRect(clip.Min, clip.Max, false);
                        draw->AddLine(
                            ImVec2(px, rect.Min.y), ImVec2(px, rect.Max.y), ImGui::GetColorU32(ImGuiCol_TextDisabled));
                        for (const auto &v : hoverValues_)
                                if (v.valid && limits.Contains(x, v.y)) {
                                        const float py =
                                            low.y + float((v.y - limits.Y.Min) / limits.Y.Size()) * (high.y - low.y);
                                        draw->AddCircleFilled(ImVec2(px, py), 4, v.color);
                                }
                        std::vector<std::string> rows;
                        char                     text[160];
                        snprintf(text, sizeof(text), tr("Time: %.7g s", "时间：%.7g s"), x);
                        rows.emplace_back(text);
                        float width = ImGui::CalcTextSize(text).x;
                        for (const auto &v : hoverValues_) {
                                rows.push_back(v.label + ": " + formatHoverValue(v, yAxisLabel_.c_str()));
                                width = std::max(width, ImGui::CalcTextSize(rows.back().c_str()).x);
                        }
                        const float line  = ImGui::GetTextLineHeightWithSpacing();
                        width            += 12;
                        float left        = px + 10;
                        if (left + width > clip.Max.x)
                                left = px - width - 10;
                        left = std::clamp(left, clip.Min.x, std::max(clip.Min.x, clip.Max.x - width));
                        const ImVec2 origin(left, clip.Min.y + 4);
                        draw->AddRectFilled(origin,
                                            ImVec2(left + width, origin.y + rows.size() * line + 8),
                                            ImGui::GetColorU32(ImGuiCol_PopupBg));
                        for (size_t i = 0; i < rows.size(); ++i)
                                draw->AddText(ImVec2(left + 6, origin.y + 4 + i * line),
                                              i ? ImGui::GetColorU32(ui::ReadableTextColor(
                                                      ImGui::ColorConvertU32ToFloat4(hoverValues_[i - 1].color),
                                                      ImGui::GetStyleColorVec4(ImGuiCol_PopupBg)))
                                                : ImGui::GetColorU32(ImGuiCol_Text),
                                              rows[i].c_str());
                        draw->PopClipRect();
                });
        }

        if (!linked && !hoverValues_.empty()) {
                std::sort(hoverValues_.begin(), hoverValues_.end(), [](const auto &a, const auto &b) {
                        return a.order != b.order ? a.order < b.order : a.label < b.label;
                });
                const auto  plotPos = ImPlot::GetPlotPos(), plotSize = ImPlot::GetPlotSize();
                const float cursorPixel = ImPlot::PlotToPixels(mouseX, limits.Y.Min).x;
                ImPlot::PushPlotClipRect();
                auto *draw = ImPlot::GetPlotDrawList();
                draw->AddLine(ImVec2(cursorPixel, plotPos.y),
                              ImVec2(cursorPixel, plotPos.y + plotSize.y),
                              ImGui::GetColorU32(ImGuiCol_TextDisabled),
                              1.0f);
                for (const auto &val : hoverValues_)
                        if (val.valid && limits.Contains(mouseX, val.y))
                                draw->AddCircleFilled(ImPlot::PlotToPixels(mouseX, val.y), 4, val.color);
                ImPlot::PopPlotClipRect();
                // One readout beside the vertical cursor; coincident curves cannot cover each other's values
                std::vector<std::string> rows;
                float                    width = 0;
                auto                     row   = [&](std::string text) {
                        width = std::max(width, ImGui::CalcTextSize(text.c_str()).x);
                        rows.push_back(std::move(text));
                };
                char text[128];
                snprintf(text,
                         sizeof(text),
                         showFft_ ? tr("Frequency: %.7g Hz", "频率：%.7g Hz") : tr("Time: %.7g s", "时间：%.7g s"),
                         mouseX);
                row(text);
                if (hasTimeAxis() && parent_ && parent_->formattedTimeAxis()) {
                        axisTimeOriginSeconds_ = parent_->timeAxisOriginSeconds();
                        formatTimeOfDay(mouseX, text, sizeof(text), &axisTimeOriginSeconds_);
                        row(text);
                }
                const size_t headerRows = rows.size();
                for (const auto &val : hoverValues_)
                        row(val.label + ": " + formatHoverValue(val, showFft_ ? "" : yAxisLabel_.c_str()));
                const auto *viewport  = ImGui::GetWindowViewport();
                const auto  padding   = ImGui::GetStyle().WindowPadding;
                width                += padding.x * 2;
                const float height    = rows.size() * ImGui::GetTextLineHeightWithSpacing() + padding.y * 2;
                const float right     = viewport->WorkPos.x + viewport->WorkSize.x;
                float       left      = cursorPixel + 12;
                if (left + width > right)
                        left = cursorPixel - width - 12;
                left = std::max(viewport->WorkPos.x, left);
                const float top =
                    std::clamp(mouse.y + 12,
                               viewport->WorkPos.y,
                               std::max(viewport->WorkPos.y, viewport->WorkPos.y + viewport->WorkSize.y - height));
                ImGui::SetNextWindowPos(ImVec2(left, top), ImGuiCond_Always);
                ImGui::BeginTooltip();
                for (size_t i = 0; i < rows.size(); ++i)
                        if (i < headerRows)
                                ImGui::TextUnformatted(rows[i].c_str());
                        else
                                ImGui::TextColored(
                                    ui::ReadableTextColor(ImGui::ColorConvertU32ToFloat4(hoverValues_[i - headerRows].color),
                                                          ImGui::GetStyleColorVec4(ImGuiCol_PopupBg)),
                                    "%s",
                                    rows[i].c_str());
                ImGui::EndTooltip();
        }

        auto &measurement = timeMeasurement();
        if (!measurement.enabled || showFft_)
                return;
        if (hovered && std::isfinite(mouseX) && measurement.count < 2 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                measurement.cursors[measurement.count++] = hoverSample_.valid ? hoverSample_.x : mouseX;
                // Hold the displayed window for measurement; acquisition keeps running
                mode = MonitorViewMode::MANUAL;
        }
        const ImVec4 colors[] = {ui::SemanticButtonPalette(ui::BtnStyle::Warning, ImGui::GetStyle()).text,
                                 ImGui::GetStyleColorVec4(ImGuiCol_CheckMark)};
        for (int i = 0; i < measurement.count; ++i) {
                const auto flags = ImPlotDragToolFlags_NoFit | (measurement.count < 2 ? ImPlotDragToolFlags_NoInputs : 0);
                if (ImPlot::DragLineX(0x54494D00 + i, &measurement.cursors[i], colors[i], 1, flags) && hoverSample_.valid)
                        measurement.cursors[i] = hoverSample_.x;
                if (showXAxis_)
                        ImPlot::TagX(measurement.cursors[i], colors[i], "%c: %.7g s", 'A' + i, measurement.cursors[i]);
        }
        char text[160]{};
        if (measurement.count == 2) {
                const double dt        = std::abs(measurement.cursors[1] - measurement.cursors[0]);
                const double frequency = dt > 0.0 ? 1.0 / dt : 0.0;
                if (dt > 0.0 && std::isfinite(dt) && std::isfinite(frequency))
                        snprintf(text, sizeof(text), "|B - A| = %.7g s  (%.7g ms)\nf = %.7g Hz", dt, dt * 1000.0, frequency);
                else
                        snprintf(text, sizeof(text), "|B - A| = %.7g s  (%.7g ms)\nf = -- Hz", dt, dt * 1000.0);
        } else {
                snprintf(text,
                         sizeof(text),
                         "%s",
                         measurement.count == 0 ? tr("Click point A", "点击选择 A 点") : tr("Click point B", "点击选择 B 点"));
        }
        // A rectangular readout uses the application's existing theme and stays clear of the legend
        const auto   pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize(), textSize = ImGui::CalcTextSize(text);
        const ImVec2 origin(pos.x + std::max(4.0f, size.x - textSize.x - 8), pos.y + 4);
        ImPlot::PushPlotClipRect();
        auto *draw = ImPlot::GetPlotDrawList();
        draw->AddRectFilled(ImVec2(origin.x - 3, origin.y - 2),
                            ImVec2(origin.x + textSize.x + 3, origin.y + textSize.y + 2),
                            ImGui::GetColorU32(ImGuiCol_PopupBg));
        draw->AddText(origin, ImGui::GetColorU32(ImGuiCol_Text), text);
        ImPlot::PopPlotClipRect();
}

void
MonitorScope::plotDraw(double *linkXMin, double *linkXMax, u32 maxDisplayPoints, MonitorViewMode &sharedMode)
{
        if (discardUnavailableReason() || (parent_ && !parent_->discardSelectionEnabled_))
                cancelDiscardSelection();
        else if (parent_ && parent_->discardSelectionEnabled_ && !discardSelection_.enabled)
                beginDiscardSelection();
        drawDiscardSelectionControls();
        const bool       discarding = discardSelection_.enabled;
        MonitorViewMode  localMode  = MonitorViewMode::MANUAL;
        MonitorViewMode &mode       = discarding ? localMode : sharedMode;
        if (discarding) {
                linkXMin = &discardSelection_.viewMin;
                linkXMax = &discardSelection_.viewMax;
        }
        if (showFft_ && mode != MonitorViewMode::MANUAL) {
                // We keep a single-shot auto-fit when NOT in manual mode?
                // Actually the user said "NEVER align automatically even for the first time"
                // But let's keep it linked to the mode. If they are in FOLLOW/FULL, it fits
                ImPlot::SetNextAxesToFit();
        }

        const bool fixedPanel = showSidePanel_;
        // Keep column identities while auto-hidden, otherwise a 2 -> 1 -> 2
        // column rebuild discards the user's manually chosen sidebar width.
        const int   layoutCols   = 2;
        const float layoutHeight = ImGui::GetContentRegionAvail().y;
        if (layoutHeight <= ImGui::GetStyle().CellPadding.y * 2) {
                statisticsPeek_.reset();
                return;
        }
        const float contentHeight   = std::max(1.0f, layoutHeight - ImGui::GetStyle().CellPadding.y * 2);
        const auto  visibleChannels = std::count_if(chs_.begin(), chs_.end(), [](const auto &entry) {
                return entry.second && entry.second->show() && !entry.second->isPendingDelete();
        });
        const float metricWidth     = statisticsMetricWidth();
        const float valueWidth      = ui::TableColumnWidth(tr("Value", "数值"), "-1234.567890", 8);
        const float singlePanelWidth =
            metricWidth + valueWidth + ImGui::GetStyle().ScrollbarSize + ImGui::GetStyle().WindowPadding.x * 2;
        const float preferredPanelWidth =
            !showFft_ && compareStatistics_
                ? metricWidth + valueWidth * static_cast<float>(std::max(1, static_cast<int>(visibleChannels))) +
                      ImGui::GetStyle().ScrollbarSize + ImGui::GetStyle().WindowPadding.x * 2
                : singlePanelWidth;
        const float panelWidth =
            std::min(preferredPanelWidth, std::max(singlePanelWidth, ImGui::GetContentRegionAvail().x * 0.5f));
        auto        *scopeWindow = ImGui::GetCurrentWindow();
        auto        &context     = *ImGui::GetCurrentContext();
        const ImVec2 areaMin     = ImGui::GetCursorScreenPos();
        const ImVec2 areaMax(areaMin.x + ImGui::GetContentRegionAvail().x, areaMin.y + layoutHeight);
        const char  *peekLabel = showFft_ ? tr("Peaks", "峰值") : tr("Stats", "统计");
        const ImVec2 tabSize(ImGui::CalcTextSize(peekLabel).x + ImGui::GetStyle().FramePadding.x * 2, ImGui::GetFrameHeight());
        const float  tabTop    = areaMin.y + ImGui::GetStyle().CellPadding.y + 1;
        const auto  &peekStyle = ImGui::GetStyle();
        const float  afterStats =
            ImGui::CalcTextSize(tr("More", "更多")).x + peekStyle.FramePadding.x * 2 + peekStyle.ItemInnerSpacing.x +
            (parent_ ? std::max(ImGui::CalcTextSize(tr("Focus", "聚焦")).x, ImGui::CalcTextSize(tr("Restore", "还原")).x) +
                           peekStyle.FramePadding.x * 2 + peekStyle.ItemInnerSpacing.x
                     : 0);
        const float  statsRight = areaMax.x - 3 - afterStats;
        const ImRect peekTab(ImVec2(statsRight - tabSize.x, tabTop), ImVec2(statsRight, tabTop + tabSize.y));
        char         peekName[64];
        snprintf(peekName, sizeof(peekName), "##ScopeStatsPeek_%08X", scopeWindow->ID);
        auto *peekWindow = ImGui::FindWindowByName(peekName);
        if (statisticsPeekFrame_ != context.FrameCount - 1 || fixedPanel)
                statisticsPeek_.reset();
        statisticsPeekFrame_ = context.FrameCount;
        auto belongsToPeek   = [&](ImGuiWindow *window) {
                return peekWindow && window && ImGui::IsWindowWithinBeginStackOf(window, peekWindow);
        };
        bool peekHovered = false, peekHeld = false;
        if (!fixedPanel && ImGui::ItemAdd(peekTab, ImGui::GetID("##statsPeekTab"), nullptr, ImGuiItemFlags_NoNav)) {
                if (ImGui::ButtonBehavior(
                        peekTab, ImGui::GetID("##statsPeekTab"), &peekHovered, &peekHeld, ImGuiButtonFlags_MouseButtonLeft)) {
                        showSidePanel_ = true;
                        statisticsPeek_.close();
                        if (parent_)
                                parent_->setModified();
                }
        }
        bool childPopup = false, otherPopup = false;
        for (const auto &popup : context.OpenPopupStack) {
                if (belongsToPeek(popup.Window))
                        childPopup = true;
                else
                        otherPopup = true;
        }
        const bool peekActive  = belongsToPeek(context.ActiveIdWindow) && context.ActiveId != 0;
        const bool dismissPeek = statisticsPeek_.isOpen() && !childPopup && !otherPopup &&
                                 !ui::interaction::activeAtFrameStart && !ui::interaction::popupAtFrameStart &&
                                 !ui::interaction::escapeConsumed && !ImGui::IsAnyItemActive() &&
                                 ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (dismissPeek)
                ui::interaction::consumeEscape();
        if (!fixedPanel && !showSidePanel_)
                statisticsPeek_.update({ImGui::GetTime(),
                                        peekHovered,
                                        belongsToPeek(context.HoveredWindow),
                                        peekActive || childPopup,
                                        false,
                                        dismissPeek,
                                        parent_ && !ImGui::IsAnyItemActive() && !context.DragDropActive &&
                                            !context.IO.AppFocusLost && context.OpenPopupStack.empty() &&
                                            !ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1) && !ImGui::IsMouseDown(2)});
        const ImGuiID menuId = scopeWindow->IDStack.back();
        if (!ImGui::BeginTable("##plotLayoutTable",
                               layoutCols,
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                               ImVec2(0, layoutHeight))) {
                return;
        }
        ImGui::TableSetupColumn(tr("Plot###col_plot", "图形###col_plot"), ImGuiTableColumnFlags_WidthStretch, 0.75f);
        ImGui::TableSetupColumn(showFft_ ? tr("Peaks###col_panel", "峰值###col_panel")
                                         : tr("Stats###col_panel", "统计###col_panel"),
                                ImGuiTableColumnFlags_WidthFixed | (fixedPanel ? 0 : ImGuiTableColumnFlags_Disabled),
                                panelWidth);
        ui::HierarchyNextRow();
        ImGui::TableSetColumnIndex(0);
        const float timeAxisWidth = ImGui::GetContentRegionAvail().x;

        // Submit hit targets before ImPlot handles mouse input, then paint the
        // controls above its background. A separate splitter nests safely with
        // the table and ImPlot without advancing the layout cursor.
        ImDrawListSplitter menuLayers;
        const bool         overlayMenu = menuFrame_ == context.FrameCount;
        if (overlayMenu) {
                menuLayers.Split(scopeWindow->DrawList, 2);
                menuLayers.SetCurrentChannel(scopeWindow->DrawList, 1);
                ImGui::PushOverrideID(menuId);
                drawMenu(true, fixedPanel ? 0.0f : tabSize.x + ImGui::GetStyle().ItemInnerSpacing.x);
                ImGui::PopID();
                menuLayers.SetCurrentChannel(scopeWindow->DrawList, 0);
        }

        const bool measuring     = timeMeasurement().enabled && !showFft_;
        const auto savedInputMap = ImPlot::GetInputMap();
        if (measuring || discarding) {
                ImPlot::GetInputMap().Pan = ImGuiMouseButton_Middle;
                ImPlot::GetInputMap().Fit = ImGuiMouseButton_Middle;
        }
        if (ImPlot::BeginPlot("##plot", ImVec2(-1, contentHeight), ImPlotFlags_NoTitle | ImPlotFlags_NoFrame)) {
                if (!showFft_ && linkXMin && linkXMax)
                        ImPlot::SetupAxisLinks(ImAxis_X1, linkXMin, linkXMax);

                ImPlot::PushStyleVar(ImPlotStyleVar_FitPadding, ImVec2(0, ui::yFitPadding));

                if (showFft_) {
                        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_None, ImPlotAxisFlags_None);
                } else {
                        const bool hideLabels = parent_ && parent_->hideAxisLabels_;
                        ImPlot::SetupAxes(hideLabels || !showXAxis_ || xAxisLabel_.empty() ? nullptr : xAxisLabel_.c_str(),
                                          hideLabels || yAxisLabel_.empty() ? nullptr : yAxisLabel_.c_str(),
                                          (showXAxis_ ? ImPlotAxisFlags_None : ImPlotAxisFlags_NoTickLabels) |
                                              (parent_ && hasTimeAxis() ? ImPlotAxisFlags_NoMenus : 0),
                                          ImPlotAxisFlags_None);
                        if (parent_ && parent_->triggerTimeAxis_)
                                ImPlot::SetupAxisFormat(ImAxis_X1, "%.4g");
                        if (hasTimeAxis() && parent_ && parent_->formattedTimeAxis()) {
                                axisTimeOriginSeconds_ = parent_->timeAxisOriginSeconds();
                                double *origin         = &axisTimeOriginSeconds_;
                                ImPlot::SetupAxisFormat(ImAxis_X1, formatTimeOfDay, origin);

                                if (linkXMin && linkXMax && std::isfinite(*linkXMin) && std::isfinite(*linkXMax)) {
                                        const double xMin = *linkXMin;
                                        const double xMax = *linkXMax;
                                        const double span = std::max(0.0, xMax - xMin);
                                        char         sampleLabel[64]{};
                                        formatTimeOfDay(
                                            xMin + span * 0.5, sampleLabel, static_cast<int>(sizeof(sampleLabel)), origin);
                                        const float labelSpace = std::max(1.0f, timeAxisWidth - ImGui::GetFontSize() * 4);
                                        const bool  twoLines   = ImGui::CalcTextSize(sampleLabel).x + 24.0f > labelSpace;
                                        if (twoLines)
                                                if (auto *space = std::strchr(sampleLabel, ' '))
                                                        *space = '\n';
                                        const float labelWidth = ImGui::CalcTextSize(sampleLabel).x + 24.0f;
                                        const int   tickCount =
                                            span > 0.0
                                                  ? std::clamp(static_cast<int>(labelSpace / std::max(1.0f, labelWidth)), 1, 6)
                                                  : 1;
                                        std::array<double, 6>               tickValues{};
                                        std::array<std::array<char, 64>, 6> tickLabels{};
                                        std::array<const char *, 6>         tickLabelPtrs{};
                                        for (int i = 0; i < tickCount; ++i) {
                                                // Center each label in its share of the plot width; endpoint
                                                // labels would lose half their date outside the plot card.
                                                tickValues[i] = xMin + span * (i + .5) / tickCount;
                                                formatTimeOfDay(tickValues[i], tickLabels[i].data(), 64, origin);
                                                if (twoLines)
                                                        if (auto *space = std::strchr(tickLabels[i].data(), ' '))
                                                                *space = '\n';
                                                tickLabelPtrs[i] = tickLabels[i].data();
                                        }
                                        ImPlot::SetupAxisTicks(
                                            ImAxis_X1, tickValues.data(), tickCount, tickLabelPtrs.data(), false);
                                }
                        }
                }

                const auto modeBeforeInteraction = mode;
                // Interaction Detection (Area or Axes)
                bool plotHovered      = ImPlot::IsPlotHovered();
                bool xAxisHovered     = ImPlot::IsAxisHovered(ImAxis_X1);
                bool yAxisHovered     = ImPlot::IsAxisHovered(ImAxis_Y1);
                bool yAxisInteraction = yAxisHovered || ImPlot::GetCurrentPlot()->Axes[ImAxis_Y1].Held;
                bool anyHovered       = plotHovered || xAxisHovered || yAxisHovered;
                if (parent_ && hasTimeAxis()) {
                        if (xAxisHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
                            ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] <
                                ImGui::GetIO().MouseDragThreshold * ImGui::GetIO().MouseDragThreshold)
                                ImGui::OpenPopup("##time_axis_menu");
                        if (ImGui::BeginPopup("##time_axis_menu")) {
                                parent_->drawTimeAxisOptions();
                                ImGui::EndPopup();
                        }
                }

                if (!showFft_ && !discarding) {
                        const auto limits = ImPlot::GetPlotLimits();
                        lastPlotXMin_     = limits.X.Min;
                        lastPlotXMax_     = limits.X.Max;
                }

                // Capture if the plot is being interacted with (dragged/panned/zoomed)
                bool plotActive = ImGui::IsItemActive();

                bool isDoubleClick = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                if (anyHovered && !yAxisInteraction && !measuring && !discarding) {
                        if (isDoubleClick) {
                                if (recordedData_ && !showFft_) {
                                        double first = std::numeric_limits<double>::infinity(), last = -first;
                                        for (const auto &[_, ch] : chs_) {
                                                if (ch->read_.rawSize() == 0)
                                                        continue;
                                                first = std::min(first, ch->earliestTs());
                                                last  = std::max(last, ch->latestTs());
                                        }
                                        if (std::isfinite(first) && last > first) {
                                                if (linkXMin)
                                                        *linkXMin = first;
                                                if (linkXMax)
                                                        *linkXMax = last;
                                                ImPlot::GetCurrentPlot()->Axes[ImAxis_X1].SetRange(first, last);
                                        }
                                        mode = MonitorViewMode::FULL;
                                        yFit_.reset();
                                } else if (mode == MonitorViewMode::MANUAL)
                                        mode = MonitorViewMode::FOLLOW;
                                else if (mode == MonitorViewMode::FOLLOW)
                                        mode = MonitorViewMode::FULL;
                        } else if (ImGui::GetIO().MouseWheel != 0) {
                                if (mode == MonitorViewMode::FULL) {
                                        mode = MonitorViewMode::FOLLOW;
                                }
                        }
                }

                if (!measuring && !discarding && !isDoubleClick && plotActive && !yAxisInteraction &&
                    ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
                        if (mode == MonitorViewMode::FULL || mode == MonitorViewMode::FOLLOW) {
                                mode = MonitorViewMode::MANUAL;
                        }
                }

                if (!showFft_) {
                        if (mode == MonitorViewMode::FOLLOW && modeBeforeInteraction != MonitorViewMode::FOLLOW)
                                yFit_.reset();
                        yFit_.update(mode == MonitorViewMode::FULL || mode == MonitorViewMode::FOLLOW,
                                     mode == MonitorViewMode::FOLLOW);
                }
                chPeaks_.clear();
                chStats_.clear();

                // Collect (ch, item) pairs here, inside the BeginPlot context
                // where ImGui ID stack is correct. Used after EndPlot to sync
                // legend-click state back to ch->show_ via raw pointer (no ID lookup)
                std::vector<std::pair<MonitorChannel *, ImPlotItem *>> syncItems;
                syncItems.reserve(chs_.size());

                // Iterate chs in insertion order so the legend/plot follow the
                // order chs were added (e.g. CSV column order), not hash order
                std::vector<ChannelMapType::value_type *> orderedChs;
                orderedChs.reserve(chs_.size());
                for (auto &pair : chs_)
                        orderedChs.push_back(&pair);
                std::sort(orderedChs.begin(), orderedChs.end(), [](const auto *a, const auto *b) {
                        return a->second->getOrder() < b->second->getOrder();
                });

                for (auto *pairPtr : orderedChs) {
                        auto      &chName      = pairPtr->first;
                        auto      &ch          = pairPtr->second;
                        const bool searchMatch = matchesSearch(chName, *ch);

                        // Build display label: "alias###internalName" so ImPlot shows
                        // the user-facing label in the legend while the ID stays stable
                        const std::string &dispLabel = ch->getLabel();
                        const std::string  plotId    = dispLabel + "###" + chName;

                        // 1. Handle Visibility
                        // For existing items: directly set Show so hide/show line
                        // takes effect immediately (bypasses ImPlotCond limitations)
                        // For brand-new items: HideNextItem(Once) sets the initial state
                        if (auto *gp = ImPlot::GetCurrentContext(); gp && gp->CurrentPlot) {
                                if (ImPlotItem *item = gp->CurrentPlot->Items.GetItem(
                                        ImPlot::GetCurrentPlot()->Items.GetItemID(plotId.c_str()))) {
                                        item->Show = ch->show_ && searchMatch;
                                        if (searchMatch)
                                                syncItems.emplace_back(ch.get(), item);
                                }
                        }
                        ImPlot::HideNextItem(!ch->show_ || !searchMatch, ImPlotCond_Once);
                        if (!searchMatch)
                                continue;

                        // 2. Handle Style & Color
                        const f32 *col = ch->getColor();
                        ImPlot::SetNextLineStyle(ImVec4(col[0], col[1], col[2], col[3]), ch->getLineWeight());
                        if (ch->showMarkers())
                                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f);

                        // 3. Prepare and Plot Data
                        bool        plotted = false;
                        const auto &rd      = ch->read_;
                        if (showFft_) {
                                // Publish the current time window for the FFT worker thread, then draw
                                // the latest transform it produced — no fft_exec on the render thread
                                const f64 xmin = (linkXMin) ? *linkXMin : 0.0;
                                const f64 xmax = (linkXMax) ? *linkXMax : 1.0;
                                fftWinMin_.store(xmin, std::memory_order_relaxed);
                                fftWinMax_.store(xmax, std::memory_order_relaxed);

                                if (ch->show_) {
                                        std::lock_guard lk(fftPubMtx_);
                                        auto            it = fftPublished_.find(chName);
                                        if (it != fftPublished_.end() && !it->second.freqs.empty()) {
                                                const FftResult &r = it->second;
                                                if (fftBars_) {
                                                        ImPlot::SetNextFillStyle(ImVec4(col[0], col[1], col[2], col[3]));
                                                        ImPlot::PlotBars(plotId.c_str(),
                                                                         r.freqs.data(),
                                                                         r.mags.data(),
                                                                         (int)r.freqs.size(),
                                                                         r.df * 0.9);
                                                } else {
                                                        ImPlot::PlotLine(
                                                            plotId.c_str(), r.freqs.data(), r.mags.data(), (int)r.freqs.size());
                                                }
                                                plotted = true;
                                                if (fftPeakCount_ > 0 && !r.peaks.empty())
                                                        chPeaks_[chName] = r.peaks;
                                        }
                                }
                        } else if (ch->arraySnapshot_) {
                                const auto &a = *ch->arraySnapshot_;
                                dxs_.clear();
                                dys_.clear();
                                const double xmin  = linkXMin ? *linkXMin : 0.0;
                                const double xmax  = linkXMax ? *linkXMax : double(a.values.size() - 1) * a.step;
                                const size_t begin = static_cast<size_t>(
                                    std::clamp(std::floor(xmin / a.step) - 1, 0.0, double(a.values.size())));
                                const size_t end =
                                    static_cast<size_t>(std::clamp(std::ceil(xmax / a.step) + 2, 0.0, double(a.values.size())));
                                const size_t budget = std::max<size_t>(4, maxDisplayPoints);
                                const size_t stride =
                                    end > begin ? std::max<size_t>(1, (end - begin + budget / 2 - 1) / (budget / 2)) : 1;
                                auto emit = [&](size_t i) {
                                        dxs_.push_back(double(i) * a.step);
                                        dys_.push_back(a.values[i] * ch->getGain() + ch->getBias());
                                };
                                for (size_t i = begin; i < end; i += stride) {
                                        const size_t stop = std::min(end, i + stride);
                                        size_t       lo = i, hi = i;
                                        bool         gap = false;
                                        for (size_t j = i; j < stop; ++j) {
                                                if (!std::isfinite(a.values[j])) {
                                                        gap = true;
                                                        continue;
                                                }
                                                if (!std::isfinite(a.values[lo]) || a.values[j] < a.values[lo])
                                                        lo = j;
                                                if (!std::isfinite(a.values[hi]) || a.values[j] > a.values[hi])
                                                        hi = j;
                                        }
                                        emit(std::min(lo, hi));
                                        if (lo != hi)
                                                emit(std::max(lo, hi));
                                        if (gap) {
                                                dxs_.push_back(double(stop - 1) * a.step);
                                                dys_.push_back(std::numeric_limits<double>::quiet_NaN());
                                        }
                                }
                                if (!dxs_.empty()) {
                                        if (ch->getPlotStyle() == 1)
                                                ImPlot::PlotStairs(
                                                    plotId.c_str(), dxs_.data(), dys_.data(), static_cast<int>(dxs_.size()));
                                        else
                                                ImPlot::PlotLine(
                                                    plotId.c_str(), dxs_.data(), dys_.data(), static_cast<int>(dxs_.size()));
                                        plotted = true;
                                }
                        } else {
                                // Time-domain: pick an LOD level whose visible bucket count fits the
                                // display budget. -1 = use raw level directly
                                dxs_.clear();
                                dys_.clear();

                                const f64 xmin   = (linkXMin) ? *linkXMin : 0.0;
                                const f64 xmax   = (linkXMax) ? *linkXMax : 1.0;
                                const u32 budget = (maxDisplayPoints > 0) ? maxDisplayPoints : 5000;
                                const int level  = rd.pickLevel(xmin, xmax, budget);

                                const f64 chGain = static_cast<f64>(ch->getGain());
                                const f64 chBias = static_cast<f64>(ch->getBias());
                                if (level < 0) {
                                        // Raw level fits — iterate the raw ring directly
                                        const usize si = rd.rawLowerBound(xmin);
                                        const usize ei = rd.rawUpperBound(xmax);
                                        if (ei > si) {
                                                const usize n = ei - si;
                                                dxs_.reserve(n);
                                                dys_.reserve(n);
                                                for (usize i = si; i < ei; ++i) {
                                                        dxs_.push_back(rd.rawTs(i));
                                                        dys_.push_back(static_cast<f64>(rd.rawVal(i)) * chGain + chBias);
                                                }
                                        }
                                } else {
                                        // Walk LOD level: each bucket emits (t, vmin) then (t, vmax)
                                        const usize si = rd.lodLowerBound(level, xmin);
                                        const usize ei = rd.lodUpperBound(level, xmax);
                                        if (ei > si) {
                                                const usize n = ei - si;
                                                dxs_.reserve(n * 2);
                                                dys_.reserve(n * 2);
                                                for (usize i = si; i < ei; ++i) {
                                                        const LodSample &s   = rd.lodAt(level, i);
                                                        f64              ylo = static_cast<f64>(s.vmin) * chGain + chBias;
                                                        f64              yhi = static_cast<f64>(s.vmax) * chGain + chBias;
                                                        if (chGain < 0.0)
                                                                std::swap(ylo, yhi);
                                                        dxs_.push_back(s.t);
                                                        dys_.push_back(ylo);
                                                        dxs_.push_back(s.t);
                                                        dys_.push_back(yhi);
                                                }
                                        }
                                }

                                if (!dxs_.empty()) {
                                        if (ch->getPlotStyle() == 1 && level < 0)
                                                ImPlot::PlotStairs(
                                                    plotId.c_str(), dxs_.data(), dys_.data(), static_cast<i32>(dxs_.size()));
                                        else
                                                ImPlot::PlotLine(
                                                    plotId.c_str(), dxs_.data(), dys_.data(), static_cast<i32>(dxs_.size()));
                                        plotted = true;
                                }

                                // Window stats: always computed regardless of ch visibility so
                                // external consumers (e.g. efficiency calculator) can read them even
                                // when the ch line is hidden in the legend
                                {
                                        Stats                           s;
                                        sample_statistics::Distribution distribution;
                                        s.approximate = level >= 0;
                                        s.min         = std::numeric_limits<f64>::infinity();
                                        s.max         = -std::numeric_limits<f64>::infinity();
                                        f64 sum       = 0.0;
                                        f64 sumSq     = 0.0;
                                        if (level < 0) {
                                                const usize si = rd.rawLowerBound(xmin);
                                                const usize ei = rd.rawUpperBound(xmax);
                                                for (usize i = si; i < ei; ++i) {
                                                        const f64 v = static_cast<f64>(rd.rawVal(i));
                                                        if (!std::isfinite(v))
                                                                continue;
                                                        if (v < s.min)
                                                                s.min = v;
                                                        if (v > s.max)
                                                                s.max = v;
                                                        if (fixedPanel || statisticsPeek_.isOpen())
                                                                distribution.add(v * chGain + chBias);
                                                        sum   += v;
                                                        sumSq += v * v;
                                                        ++s.count;
                                                }
                                        } else {
                                                const usize si = rd.lodLowerBound(level, xmin);
                                                const usize ei = rd.lodUpperBound(level, xmax);
                                                for (usize i = si; i < ei; ++i) {
                                                        const LodSample &ls = rd.lodAt(level, i);
                                                        if (!std::isfinite(ls.vmin) || !std::isfinite(ls.vmax))
                                                                continue;
                                                        if (ls.vmin < s.min)
                                                                s.min = ls.vmin;
                                                        if (ls.vmax > s.max)
                                                                s.max = ls.vmax;
                                                        const f64 mid =
                                                            (static_cast<f64>(ls.vmin) + static_cast<f64>(ls.vmax)) * 0.5;
                                                        if (fixedPanel || statisticsPeek_.isOpen())
                                                                distribution.add(mid * chGain + chBias);
                                                        sum   += mid;
                                                        sumSq += mid * mid;
                                                        ++s.count;
                                                }
                                        }
                                        if (s.count > 0) {
                                                s.pkpk = s.max - s.min;
                                                s.mean = sum / static_cast<f64>(s.count);
                                                s.rms  = std::sqrt(sumSq / static_cast<f64>(s.count));
                                                // Apply ch gain & bias to all statistics:
                                                // transformed value y = x * gain + bias
                                                if (chGain != 1.0 || chBias != 0.0) {
                                                        // RMS(y) = sqrt(g^2*E[x^2] + 2*g*b*E[x] + b^2),
                                                        // computed from the raw mean (s.mean) and
                                                        // raw mean-square (s.rms^2) before transforming
                                                        const f64 meanRaw = s.mean;
                                                        const f64 msRaw   = s.rms * s.rms;
                                                        s.rms             = std::sqrt(chGain * chGain * msRaw +
                                                                          2.0 * chGain * chBias * meanRaw + chBias * chBias);
                                                        s.min             = s.min * chGain + chBias;
                                                        s.max             = s.max * chGain + chBias;
                                                        s.mean            = meanRaw * chGain + chBias;
                                                        if (chGain < 0.0)
                                                                std::swap(s.min, s.max);
                                                        s.pkpk = s.max - s.min;
                                                }
                                                if (fixedPanel || statisticsPeek_.isOpen()) {
                                                        distribution.finish();
                                                        s.stddev   = distribution.stddev;
                                                        s.variance = distribution.variance;
                                                        s.median   = distribution.median;
                                                        s.p95      = distribution.p95;
                                                }
                                                chStats_[chName] = s;
                                        }
                                }
                        }

                        if (!plotted) {
                                ImPlot::PlotLine(plotId.c_str(), (const f64 *)nullptr, (const f64 *)nullptr, 0);
                        }

                        // Drag the legend label itself to move this ch to another
                        // scope (including a scope owned by a different Monitor)
                        if (ImPlot::BeginDragDropSourceItem(plotId.c_str())) {
                                ChannelMovePayload payload{};
                                payload.srcScope = this;
                                payload.isGroup  = false;
                                snprintf(payload.chName, sizeof(payload.chName), "%s", chName.c_str());
                                ImGui::SetDragDropPayload("DND_CHANNEL_MOVE", &payload, sizeof(payload));
                                ImPlot::ItemIcon(ImVec4(col[0], col[1], col[2], col[3]));
                                ImGui::SameLine();
                                ImGui::Text(ImGui::GetIO().KeyShift ? tr("Copy: %s", "复制: %s") : tr("Move: %s", "移动: %s"),
                                            dispLabel.c_str());
                                ImPlot::EndDragDropSource();
                        }

                        // 4. Update Legend Toggle
                        if (ImPlotContext *gp = ImPlot::GetCurrentContext(); gp && gp->CurrentPlot) {
                                if (ImPlotItem *item = gp->CurrentPlot->Items.GetItem(
                                        ImPlot::GetCurrentPlot()->Items.GetItemID(plotId.c_str()))) {
                                        ch->show_ = item->Show;
                                }
                        }

                        // 5. Popup Configuration
                        if (ImPlot::BeginLegendPopup(plotId.c_str())) {
                                if (ImGui::MenuItem(tr("Copy", "复制")))
                                        ImGui::SetClipboardText(clipboardText({chName}).c_str());
                                drawDisplayFormatMenu(*ch);
                                ImGui::Separator();
                                ImGui::Text(tr("Channel: %s", "通道: %s"), dispLabel.c_str());
                                ImGui::Text(
                                    "%s: %s", tr("Value", "数值"), formatChannelValue(*ch, ch->getDisplayRadix()).c_str());
                                ImGui::Separator();

                                ImVec4 curCol = ImVec4(col[0], col[1], col[2], col[3]);
                                if (ch->useAutoColor()) {
                                        if (ImPlotContext *gp = ImPlot::GetCurrentContext(); gp && gp->CurrentPlot) {
                                                if (ImPlotItem *item = gp->CurrentPlot->Items.GetItem(
                                                        ImPlot::GetCurrentPlot()->Items.GetItemID(plotId.c_str()))) {
                                                        curCol = ImGui::ColorConvertU32ToFloat4(item->Color);
                                                }
                                        }
                                }
                                f32 colArr[4] = {curCol.x, curCol.y, curCol.z, curCol.w};
                                if (ImGui::ColorEdit4(tr("Color", "颜色"), colArr, ImGuiColorEditFlags_NoInputs)) {
                                        ch->useAutoColor() = false;
                                        memcpy(ch->getColor(), colArr, sizeof(colArr));
                                }
                                ImGui::SameLine();
                                if (ImGui::Checkbox(tr("Auto", "自动"), &ch->useAutoColor())) {
                                        if (ch->useAutoColor()) {
                                                static i32 shuffleIdx = 0;
                                                shuffleIdx            = (shuffleIdx + 1) % ImPlot::GetColormapSize();
                                                ImVec4 newCol         = ImPlot::GetColormapColor(shuffleIdx);
                                                memcpy(ch->getColor(), &newCol.x, sizeof(f32) * 4);
                                        }
                                }

                                ImGui::SetNextItemWidth(100);
                                const char *styleNames[] = {tr("Line", "折线"), tr("Stairs", "阶梯")};
                                i32         currentStyle = ch->getPlotStyle();
                                if (ui::Choice("##PlotStyle", &currentStyle, styleNames, 2)) {
                                        ch->getPlotStyle() = currentStyle;
                                }
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s", tr("Style", "样式"));
                                ImGui::Checkbox(tr("Markers", "标记点"), &ch->showMarkers());
                                ImGui::Separator();
                                ImGui::SetNextItemWidth(120);
                                if (ImGui::DragFloat(tr("Gain##chg", "增益##chg"), &ch->getGain(), 0.001f, -1e6f, 1e6f, "%.4g"))
                                        if (parent_)
                                                parent_->setModified();
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s",
                                                          tr("Multiplier applied to all values (display & calculation)",
                                                             "所有数值的乘数（显示与计算均生效）"));
                                ImGui::SameLine();
                                if (ImGui::SmallButton(tr("Reset##gainrst", "重置##gainrst"))) {
                                        ch->getGain() = 1.0f;
                                        if (parent_)
                                                parent_->setModified();
                                }
                                ImGui::SetNextItemWidth(120);
                                if (ImGui::DragFloat(tr("Bias##chb", "偏置##chb"), &ch->getBias(), 0.001f, -1e6f, 1e6f, "%.4g"))
                                        if (parent_)
                                                parent_->setModified();
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s",
                                                          tr("Offset added after gain (value * gain + bias)",
                                                             "增益之后叠加的偏置（数值 * 增益 + 偏置）"));
                                ImGui::SameLine();
                                if (ImGui::SmallButton(tr("Reset##biasrst", "重置##biasrst"))) {
                                        ch->getBias() = 0.0f;
                                        if (parent_)
                                                parent_->setModified();
                                }
                                ImGui::SameLine();
                                if (ImGui::SmallButton(tr("Zero##biaszero", "置零##biaszero"))) {
                                        ch->getBias() -= ch->getDispVal();
                                        if (parent_)
                                                parent_->setModified();
                                }
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s",
                                                          tr("Set bias so the current reading displays as 0",
                                                             "将偏置设为当前显示值的相反数，使当前读数归零"));
                                if (ui::Button(tr("Delete Channel", "删除通道"), ui::BtnStyle::Danger))
                                        ch->markPendingDelete();

                                // ── Rename ch ──────────────────────────────────
                                ImGui::Separator();
                                ImGui::TextDisabled("%s", tr("Rename", "重命名"));
                                static char chRenameBuf[128]{};
                                // Sync buffer once when popup first appears
                                if (ImGui::IsWindowAppearing())
                                        snprintf(chRenameBuf, sizeof(chRenameBuf), "%s", dispLabel.c_str());
                                ImGui::SetNextItemWidth(180);
                                bool commitRename = ImGui::InputText(
                                    "##chRename", chRenameBuf, sizeof(chRenameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
                                ImGui::SameLine();
                                if (ImGui::SmallButton("OK"))
                                        commitRename = true;
                                if (commitRename && chRenameBuf[0] != '\0') {
                                        ch->setLabel(chRenameBuf);
                                        if (parent_)
                                                parent_->setModified();
                                }
                                if (!ch->getLabel().empty() && ch->getLabel() != ch->getName()) {
                                        ImGui::SameLine();
                                        if (ImGui::SmallButton(tr("Reset##chLblRst", "重置##chLblRst"))) {
                                                ch->setLabel("");
                                                if (parent_)
                                                        parent_->setModified();
                                        }
                                }

                                ImPlot::EndLegendPopup();
                        }

                        // Keep the ch swatch/curve color unchanged. Only highlight
                        // the legend label when gain or bias differs from its default
                        if (ImPlotContext *gp = ImPlot::GetCurrentContext(); gp && gp->CurrentPlot) {
                                if (ImPlotItem *item = gp->CurrentPlot->Items.GetItem(
                                        ImPlot::GetCurrentPlot()->Items.GetItemID(plotId.c_str()))) {
                                        const bool transformed =
                                            std::abs(ch->getGain() - 1.0f) > 1e-6f || std::abs(ch->getBias()) > 1e-6f;
                                        ImPlot::SetLegendTextColor(
                                            item->ID,
                                            transformed
                                                ? ImGui::GetColorU32(
                                                      ui::SemanticButtonPalette(ui::BtnStyle::Warning, ImGui::GetStyle()).text)
                                                : 0);
                                }
                        }
                }
                if (!showFft_)
                        ui::fitConstantValueAxis(ImPlot::GetCurrentPlot()->Axes[ImAxis_Y1]);
                if (discarding) {
                        hoverSample_ = {};
                        hoverValues_.clear();
                        plotDiscardSelection();
                } else {
                        plotInspect(mode);
                }
                ImPlot::EndPlot();

                // Sync legend-click state back to ch->show_
                // EndPlot() is where ImPlot processes legend clicks (toggles item->Show)
                // We use the raw pointers saved before EndPlot to avoid re-doing
                // the ID lookup (which would use the wrong ImGui context after EndPlot)
                for (auto &[ch, item] : syncItems)
                        ch->show_ = item->Show;

                ImPlot::PopStyleVar();
        }
        ImPlot::GetInputMap() = savedInputMap;

        if (overlayMenu)
                menuLayers.Merge(scopeWindow->DrawList);

        if (fixedPanel) {
                ImGui::TableSetColumnIndex(1);
                if (ImGui::SmallButton(tr("Hide", "收起"))) {
                        showSidePanel_ = false;
                        statisticsPeek_.reset();
                        if (parent_)
                                parent_->setModified();
                }
                drawStatisticsPanel(std::max(1.0f, contentHeight - ImGui::GetFrameHeightWithSpacing()));
        }
        ImGui::EndTable();
        if (!fixedPanel) {
                // Draw an overlay hit tab without moving the layout cursor or
                // reducing the plot's width when the panel is only previewed.
                const ImGuiCol tabColor = peekHeld                                  ? ImGuiCol_ButtonActive
                                          : peekHovered || statisticsPeek_.isOpen() ? ImGuiCol_ButtonHovered
                                                                                    : ImGuiCol_Button;
                scopeWindow->DrawList->AddRectFilled(
                    peekTab.Min, peekTab.Max, ImGui::GetColorU32(tabColor), ImGui::GetStyle().FrameRounding);
                scopeWindow->DrawList->AddRect(
                    peekTab.Min, peekTab.Max, ImGui::GetColorU32(ImGuiCol_Border), ImGui::GetStyle().FrameRounding);
                scopeWindow->DrawList->AddText(
                    ImVec2(peekTab.Min.x + ImGui::GetStyle().FramePadding.x, peekTab.Min.y + ImGui::GetStyle().FramePadding.y),
                    ImGui::GetColorU32(ImGuiCol_Text),
                    peekLabel);
        }
        if (!fixedPanel && statisticsPeek_.isOpen() && !context.IO.AppFocusLost &&
            (!ImGui::GetTopMostPopupModal() || childPopup)) {
                const auto *viewport = scopeWindow->Viewport;
                if (viewport->WorkSize.x <= 16 || viewport->WorkSize.y <= 16)
                        return;
                const ImVec2 maximum(std::max(1.0f, viewport->WorkSize.x - 12), std::max(1.0f, viewport->WorkSize.y - 12));
                const ImVec2 minimum(std::min(maximum.x, ImGui::GetFontSize() * 14),
                                     std::min(maximum.y, ImGui::GetFrameHeight() * 5));
                const ImVec2 initial(
                    std::min(maximum.x, std::max(minimum.x, preferredPanelWidth)),
                    std::min(maximum.y, std::max({minimum.y, layoutHeight, ImGui::GetFrameHeightWithSpacing() * 10.0f})));
                const float actualWidth = peekWindow ? std::clamp(peekWindow->Size.x, minimum.x, maximum.x) : initial.x;
                const float right       = std::clamp(
                    areaMax.x, viewport->WorkPos.x + 6 + actualWidth, viewport->WorkPos.x + viewport->WorkSize.x - 6);
                const float actualHeight = peekWindow ? std::min(maximum.y, peekWindow->Size.y) : initial.y;
                const float top          = std::clamp(areaMin.y,
                                             viewport->WorkPos.y + 6,
                                             viewport->WorkPos.y + std::max(6.0f, viewport->WorkSize.y - actualHeight - 6));
                ImGui::SetNextWindowViewport(viewport->ID);
                ImGui::SetNextWindowPos(ImVec2(right, top), ImGuiCond_Always, ImVec2(1, 0));
                ImGui::SetNextWindowSize(initial, ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSizeConstraints(minimum, maximum);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
                if (ImGui::Begin(peekName,
                                 nullptr,
                                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing)) {
                        if (!otherPopup && !childPopup)
                                ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(peekLabel);
                        ImGui::SameLine();
                        if (ui::SmallButton(tr("Pin", "固定"), ui::BtnStyle::Primary)) {
                                showSidePanel_ = true;
                                statisticsPeek_.close();
                                if (parent_)
                                        parent_->setModified();
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton(tr("Close", "收起")))
                                statisticsPeek_.close();
                        ImGui::Separator();
                        drawStatisticsPanel(ImGui::GetContentRegionAvail().y);
                }
                ImGui::End();
                ImGui::PopStyleColor();
        }
}

void
MonitorScope::drawStatisticsPanel(float contentHeight)
{
        const float panelTop              = ImGui::GetCursorScreenPos().y;
        bool        statisticsModeChanged = false;
        if (!showFft_) {
                ImGui::SetNextItemWidth(-1);
                int         statisticsMode    = compareStatistics_ ? 0 : 1;
                const char *statisticsModes[] = {tr("Compare channels", "通道对比"), tr("Per channel", "逐通道")};
                if (ui::Choice("##statisticsMode", &statisticsMode, statisticsModes, IM_ARRAYSIZE(statisticsModes))) {
                        compareStatistics_    = statisticsMode == 0;
                        statisticsModeChanged = true;
                        if (parent_)
                                parent_->setModified();
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "%s",
                            tr("Visible time range; non-finite values excluded. Variance uses N; quantiles use linear "
                               "interpolation. At reduced detail, statistics use bucket midpoints and N counts buckets "
                               "(approximate). Zoom in for raw-sample statistics.",
                               "统计当前时间范围，排除非有限值。方差分母为 "
                               "N；分位数使用线性插值。缩略显示时使用聚合桶中点估算，N 为桶数；放大后使用原始采样。"));
        }
        const float panelContentHeight = std::max(
            1.0f, std::min(ImGui::GetContentRegionAvail().y, contentHeight - (ImGui::GetCursorScreenPos().y - panelTop)));
        // The selector stays visible while its statistics scroll independently
        if (statisticsModeChanged)
                ImGui::SetNextWindowScroll(ImVec2(0, 0));
        if (ImGui::BeginChild("##plotSidePanel", ImVec2(0, panelContentHeight))) {
                const float tableViewportHeight = std::max(1.0f, ImGui::GetContentRegionAvail().y);
                if (showFft_) {
                        if (!chPeaks_.empty()) {
                                for (auto &[chName, peaks] : chPeaks_) {
                                        if (peaks.empty())
                                                continue;
                                        ui::TableText(std::string(tr("Peaks: ", "峰值: ")) + chName);
                                        if (ui::TableIsland tableCard;
                                            tableCard.begin(("##peaksTable_" + chName).c_str(),
                                                            2,
                                                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                                ImGuiTableFlags_PadOuterX | ImGuiTableFlags_Resizable |
                                                                ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit,
                                                            ImVec2(0, statisticsTableHeight(peaks.size())))) {
                                                ImGui::TableSetupColumn("Freq (Hz)",
                                                                        ImGuiTableColumnFlags_WidthFixed,
                                                                        ui::TableColumnWidth("Freq (Hz)", "123456.1", 8));
                                                ImGui::TableSetupColumn(
                                                    tr("Mag", "幅值"),
                                                    ImGuiTableColumnFlags_WidthFixed,
                                                    ui::TableColumnWidth(tr("Mag", "幅值"), "-123456.123456", 12));
                                                ImGui::TableHeadersRow();

                                                for (const auto &p : peaks) {
                                                        ui::HierarchyNextRow();
                                                        ImGui::TableSetColumnIndex(0);
                                                        tableNumber(p.freq, 1);
                                                        ImGui::TableSetColumnIndex(1);
                                                        tableNumber(p.mag);
                                                }
                                                tableCard.end();
                                        }
                                        ImGui::Spacing();
                                }
                        } else {
                                ImGui::Text("%s", tr("No peaks detected", "未检测到峰值"));
                        }
                } else {
                        std::vector<std::pair<std::string, MonitorChannel *>> channels;
                        for (const auto &[name, ch] : chs_)
                                if (ch && ch->show() && !ch->isPendingDelete())
                                        channels.emplace_back(name, ch.get());
                        std::sort(channels.begin(), channels.end(), [](const auto &a, const auto &b) {
                                return a.second->getOrder() != b.second->getOrder()
                                           ? a.second->getOrder() < b.second->getOrder()
                                           : a.first < b.first;
                        });
                        if (channels.empty()) {
                                ImGui::TextUnformatted(tr("No visible channels", "没有显示的通道"));
                        } else if (!compareStatistics_) {
                                for (const auto &[name, ch] : channels) {
                                        const auto stats = chStats_.find(name);
                                        if (stats == chStats_.end())
                                                continue;
                                        const auto &val = stats->second;
                                        ui::TableText(ch->getLabel() + (val.approximate ? " (approx.)" : ""));
                                        if (ui::TableIsland tableCard;
                                            tableCard.begin(("##statsTable_" + name).c_str(),
                                                            2,
                                                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                                ImGuiTableFlags_PadOuterX | ImGuiTableFlags_Resizable |
                                                                ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit,
                                                            ImVec2(0, statisticsTableHeight(11)))) {
                                                ImGui::TableSetupColumn(tr("Metric###col_metric", "指标###col_metric"),
                                                                        ImGuiTableColumnFlags_WidthFixed,
                                                                        statisticsMetricWidth());
                                                ImGui::TableSetupColumn(
                                                    tr("Value###col_statval", "数值###col_statval"),
                                                    ImGuiTableColumnFlags_WidthFixed,
                                                    ui::TableColumnWidth(tr("Value", "数值"), "-1234.567890", 8));
                                                ensureStatisticsMetricWidth();
                                                ImGui::TableHeadersRow();
                                                auto row = [](const char *label, auto draw) {
                                                        ui::HierarchyNextRow();
                                                        ImGui::TableSetColumnIndex(0);
                                                        ui::TableText(label);
                                                        ImGui::TableSetColumnIndex(1);
                                                        draw();
                                                };
                                                row(tr("Current", "当前值"),
                                                    [&] { ui::TableText(formatChannelValue(*ch, ch->getDisplayRadix())); });
                                                row(tr("Min", "最小值"), [&] { tableNumber(val.min); });
                                                row(tr("Max", "最大值"), [&] { tableNumber(val.max); });
                                                row(tr("Pk-Pk", "峰峰值"), [&] { tableNumber(val.pkpk); });
                                                row(tr("Mean", "平均值"), [&] { tableNumber(val.mean); });
                                                row(tr("RMS", "有效值"), [&] { tableNumber(val.rms); });
                                                row(tr("Std dev", "标准差"), [&] { tableNumber(val.stddev); });
                                                row(tr("Variance", "方差"), [&] { tableNumber(val.variance); });
                                                row(tr("Median", "中位数"), [&] { tableNumber(val.median); });
                                                row("P95", [&] { tableNumber(val.p95); });
                                                row("N", [&] { ui::TableText(std::to_string(val.count)); });
                                                tableCard.end();
                                        }
                                        ImGui::Spacing();
                                }
                                if (chStats_.empty())
                                        ImGui::TextUnformatted(tr("No data in window", "窗口内无数据"));
                        } else {
                                const char *metrics[]      = {tr("Current", "当前值"),
                                                              tr("Min", "最小值"),
                                                              tr("Max", "最大值"),
                                                              tr("Pk-Pk", "峰峰值"),
                                                              tr("Mean", "平均值"),
                                                              tr("RMS", "有效值"),
                                                              tr("Std dev", "标准差"),
                                                              tr("Variance", "方差"),
                                                              tr("Median", "中位数"),
                                                              "P95",
                                                              "N"};
                                const f64 Stats::*values[] = {&Stats::min,
                                                              &Stats::max,
                                                              &Stats::pkpk,
                                                              &Stats::mean,
                                                              &Stats::rms,
                                                              &Stats::stddev,
                                                              &Stats::variance,
                                                              &Stats::median,
                                                              &Stats::p95};
                                // Very large scopes span multiple matrices rather than exceeding
                                // ImGui's column limit or dropping channels from the comparison
                                constexpr size_t perTable = IMGUI_TABLE_MAX_COLUMNS - 2;
                                for (size_t first = 0; first < channels.size(); first += perTable) {
                                        const size_t      count = std::min(perTable, channels.size() - first);
                                        const std::string id    = "##statsComparison_" + std::to_string(first);
                                        ui::TableIsland   tableCard;
                                        if (!tableCard.begin(id.c_str(),
                                                             static_cast<int>(count + 1),
                                                             ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                                 ImGuiTableFlags_PadOuterX | ImGuiTableFlags_Resizable |
                                                                 ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                                                 ImGuiTableFlags_SizingFixedFit,
                                                             ImVec2(0, tableViewportHeight)))
                                                continue;
                                        ImGui::TableSetupScrollFreeze(1, 1);
                                        ImGui::TableSetupColumn(tr("Metric###metric", "指标###metric"),
                                                                ImGuiTableColumnFlags_WidthFixed,
                                                                statisticsMetricWidth());
                                        for (size_t i = 0; i < count; ++i) {
                                                const auto &[name, ch] = channels[first + i];
                                                ImGui::TableSetupColumn(ch->getLabel().c_str(),
                                                                        ImGuiTableColumnFlags_WidthFixed,
                                                                        ui::TableColumnWidth("", "-1234.567890", 8),
                                                                        ImHashStr(name.c_str()));
                                        }
                                        ensureStatisticsMetricWidth();
                                        ui::HierarchyNextRow(ImGuiTableRowFlags_Headers);
                                        ImGui::TableSetColumnIndex(0);
                                        ImGui::TableHeader(tr("Metric", "指标"));
                                        for (size_t i = 0; i < count; ++i) {
                                                const auto &[name, ch] = channels[first + i];
                                                ImGui::TableSetColumnIndex(static_cast<int>(i + 1));
                                                ImGui::PushID(name.c_str());
                                                ImGui::PushStyleColor(
                                                    ImGuiCol_Text,
                                                    ui::ReadableTextColor(
                                                        ImVec4(ch->getColor()[0], ch->getColor()[1], ch->getColor()[2], 1),
                                                        ImGui::GetStyleColorVec4(ImGuiCol_TableHeaderBg)));
                                                const auto        summary   = chStats_.find(name);
                                                const std::string fullLabel = ch->getLabel();
                                                const size_t      leaf      = fullLabel.find_last_of('.');
                                                const std::string shortLabel =
                                                    ch->getLabel() == ch->getName() && leaf != std::string::npos
                                                        ? fullLabel.substr(leaf + 1)
                                                        : fullLabel;
                                                const std::string heading =
                                                    shortLabel + (summary != chStats_.end() && summary->second.approximate
                                                                      ? " (approx.)"
                                                                      : "");
                                                ImGui::TableHeader(heading.c_str());
                                                ImGui::PopStyleColor();
                                                if (ImGui::IsItemHovered() &&
                                                    ImGui::BeginTooltipEx(ImGuiTooltipFlags_OverridePrevious,
                                                                          ImGuiWindowFlags_None)) {
                                                        ImGui::PushTextWrapPos(
                                                            std::min(ImGui::GetFontSize() * 30,
                                                                     ImGui::GetWindowViewport()->WorkSize.x -
                                                                         ImGui::GetStyle().WindowPadding.x * 2));
                                                        ImGui::TextUnformatted(fullLabel.c_str());
                                                        if (fullLabel != name)
                                                                ImGui::TextUnformatted(name.c_str());
                                                        ImGui::PopTextWrapPos();
                                                        ImGui::EndTooltip();
                                                }
                                                ImGui::PopID();
                                        }
                                        for (int metric = 0; metric < IM_ARRAYSIZE(metrics); ++metric) {
                                                ui::HierarchyNextRow();
                                                ImGui::TableSetColumnIndex(0);
                                                ui::TableText(metrics[metric]);
                                                for (size_t i = 0; i < count; ++i) {
                                                        if (!ImGui::TableSetColumnIndex(static_cast<int>(i + 1)))
                                                                continue;
                                                        const auto &[name, ch] = channels[first + i];
                                                        const auto stats       = chStats_.find(name);
                                                        if (metric == 0) {
                                                                if (!ch->arraySnapshot_ && ch->read_.rawEmpty())
                                                                        ImGui::TextDisabled("--");
                                                                else
                                                                        ui::TableText(
                                                                            formatChannelValue(*ch, ch->getDisplayRadix()));
                                                        } else if (metric == IM_ARRAYSIZE(metrics) - 1) {
                                                                ui::TableText(std::to_string(
                                                                    stats == chStats_.end() ? size_t(0) : stats->second.count));
                                                        } else if (stats == chStats_.end()) {
                                                                ImGui::TextDisabled("--");
                                                        } else {
                                                                tableNumber(stats->second.*values[metric - 1]);
                                                        }
                                                }
                                        }
                                        tableCard.end();
                                }
                        }
                }
        }
        ImGui::EndChild();
}

void
MonitorScope::reorderChannelTo(const std::string &src, const std::string &dst)
{
        if (src == dst || chs_.find(src) == chs_.end() || chs_.find(dst) == chs_.end())
                return;

        // Current display order
        std::vector<std::string> keys;
        keys.reserve(chs_.size());
        for (auto &[k, _] : chs_)
                keys.push_back(k);
        std::sort(keys.begin(), keys.end(), [&](const std::string &a, const std::string &b) {
                const i64 oa = chs_[a]->getOrder(), ob = chs_[b]->getOrder();
                return oa != ob ? oa < ob : a < b;
        });

        int sidx = -1, didx = -1;
        for (int i = 0; i < (int)keys.size(); ++i) {
                if (keys[i] == src)
                        sidx = i;
                if (keys[i] == dst)
                        didx = i;
        }
        if (sidx < 0 || didx < 0)
                return;

        std::string moved = keys[sidx];
        keys.erase(keys.begin() + sidx);
        // didx is the destination row in the original list. After removing an
        // earlier source, inserting at that same numeric index places the item
        // after the destination row, which is the expected one-step downward
        // swap. Decrementing didx here would put adjacent items back unchanged
        keys.insert(keys.begin() + didx, moved);

        for (int i = 0; i < (int)keys.size(); ++i)
                chs_[keys[i]]->setOrder(i);
        nextChannelOrder_      = (i64)keys.size();
        forceManualTableOrder_ = true;
        tableSortColumn_       = -1;
        if (parent_)
                parent_->setModified();
}

std::string
MonitorScope::groupAnchorKey(const std::string &groupPath) const
{
        std::string best;
        i64         bestOrd = std::numeric_limits<i64>::max();
        for (const auto &[k, ch] : chs_) {
                if (scopePathContains(groupPath, k)) {
                        i64 o = ch->getOrder();
                        if (o < bestOrd) {
                                bestOrd = o;
                                best    = k;
                        }
                }
        }
        return best;
}

void
MonitorScope::reorderGroupBefore(const std::string &srcGroup, const std::string &dst)
{
        if (chs_.find(dst) == chs_.end())
                return;

        // Collect the leaves that belong to the dragged group
        std::set<std::string> srcSet;
        for (const auto &[k, _] : chs_)
                if (scopePathContains(srcGroup, k))
                        srcSet.insert(k);
        if (srcSet.empty() || srcSet.count(dst)) // empty, or dropping a group onto its own member
                return;

        // Current display order
        std::vector<std::string> keys;
        keys.reserve(chs_.size());
        for (auto &[k, _] : chs_)
                keys.push_back(k);
        std::sort(keys.begin(), keys.end(), [&](const std::string &a, const std::string &b) {
                const i64 oa = chs_[a]->getOrder(), ob = chs_[b]->getOrder();
                return oa != ob ? oa < ob : a < b;
        });

        // Pull the group's members out, preserving their relative order
        std::vector<std::string> moved;
        moved.reserve(srcSet.size());
        for (const auto &k : keys)
                if (srcSet.count(k))
                        moved.push_back(k);
        keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const std::string &k) { return srcSet.count(k) > 0; }),
                   keys.end());

        // Re-insert before the destination
        int didx = (int)keys.size();
        for (int i = 0; i < (int)keys.size(); ++i)
                if (keys[i] == dst) {
                        didx = i;
                        break;
                }
        keys.insert(keys.begin() + didx, moved.begin(), moved.end());

        for (int i = 0; i < (int)keys.size(); ++i)
                chs_[keys[i]]->setOrder(i);
        nextChannelOrder_      = (i64)keys.size();
        forceManualTableOrder_ = true;
        tableSortColumn_       = -1;
        if (parent_)
                parent_->setModified();
}

int
MonitorScope::addChannel(const std::string &chName)
{
        if (chName.empty())
                return -1;
        if (chs_.find(chName) != chs_.end())
                return -1;
        auto ch = std::make_shared<MonitorChannel>(chName);

        // Assign an initial color from the colormap
        static i32 globalColorIdx = 0;
        ImVec4     c              = ImPlot::GetColormapColor(globalColorIdx);
        globalColorIdx            = (globalColorIdx + 1) % ImPlot::GetColormapSize();
        memcpy(ch->getColor(), &c.x, sizeof(f32) * 4);
        ch->minKeepPoints_ = fftPoints_;
        ch->setOrder(nextChannelOrder_++); // preserve insertion order for display

        chs_[chName] = std::move(ch);
        return 0;
}

int
MonitorScope::setValue(const std::string &chName, const f32 val)
{
        auto it = chs_.find(chName);
        if (it == chs_.end())
                return -1;
        it->second->setRVal(val, sessionTimeSec());
        return 0;
}

MonitorChannel *
MonitorScope::findChannel(const std::string &chName)
{
        auto it = chs_.find(chName);
        if (it == chs_.end())
                return nullptr;
        return it->second.get();
}

void
MonitorScope::shmInit(MonitorChannel &ch)
{
        if (ch.getDevice() != "SHM")
                return;
        // Use the dedicated SHM region name if set; fall back to ch name
        // Use SHM_READWRITE because spsc_read_buf updates spsc->rp (read pointer)
        // Use 4096 as the minimum region capacity to accommodate the spsc header
        const std::string &regionName = ch.getShmRegionName().empty() ? ch.getName() : ch.getShmRegionName();
        shm_cfg_t          cfg        = {regionName.c_str(), SHM_READWRITE, 4096};
        shm_init(&ch.getShm(), cfg);
}

void
MonitorScope::dropTarget()
{
        ImGuiWindow  *scopeWindow = ImGui::GetCurrentWindow();
        const ImGuiID dragId      = ImGui::GetID("##scopeBackgroundDrag");
        const bool    dragging    = ImGui::GetActiveID() == dragId;
        // This hit region overlays existing content; it must not participate
        // in layout. Moving/restoring the cursor here left IsSetPos set at
        // EndChild and could extend the parent by trailing item spacing.
        if ((dragging || (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive())) &&
            ImGui::ItemAdd(scopeWindow->InnerRect, dragId, nullptr, ImGuiItemFlags_NoNav)) {
                bool hovered, held;
                ImGui::ButtonBehavior(scopeWindow->InnerRect, dragId, &hovered, &held, ImGuiButtonFlags_MouseButtonLeft);
                if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("SCOPE_REORDER", name_.c_str(), name_.size() + 1);
                        ImGui::TextUnformatted(getLabel().c_str());
                        ImGui::EndDragDropSource();
                }
        }
        if (const auto *pending = ImGui::GetDragDropPayload(); pending && pending->IsDataType("SCOPE_REORDER")) {
                if (ImGui::BeginDragDropTargetCustom(scopeWindow->InnerRect, ImGui::GetID("##scopeReorderTarget"))) {
                        if (const auto *payload = ImGui::AcceptDragDropPayload("SCOPE_REORDER"))
                                requestSwap(static_cast<const char *>(payload->Data));
                        ImGui::EndDragDropTarget();
                }
                return;
        }

        if (const ImGuiPayload *pending = ImGui::GetDragDropPayload();
            parent_ && parent_->externalFeed_ && pending && !pending->IsDataType("DND_CHANNEL_MOVE"))
                return;
        if (const ImGuiPayload *pending = ImGui::GetDragDropPayload();
            pending && pending->IsDataType("DND_CHANNEL_MOVE") && pending->DataSize == sizeof(ChannelMovePayload)) {
                auto *data = static_cast<ChannelMovePayload *>(pending->Data);
                if (data && data->srcScope == this)
                        return;
        }

        ImGuiWindow  *window = ImGui::GetCurrentWindow();
        const ImRect  targetRect(window->InnerRect.Min, window->InnerRect.Max);
        const ImGuiID targetId = window->GetID("##scope_ch_drop_target");
        if (ImGui::BeginDragDropTargetCustom(targetRect, targetId)) {
                if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("FLASH_ARRAY")) {
                        if (payload->DataSize == sizeof(uint64_t)) {
                                uint64_t id;
                                memcpy(&id, payload->Data, sizeof(id));
                                const auto &drag = flasharray::drag();
                                if (id == drag.id && drag.snapshot && !drag.snapshot->values.empty()) {
                                        auto        snapshot = drag.snapshot;
                                        std::string key      = snapshot->name;
                                        for (int suffix = 2; findChannel(key); ++suffix)
                                                key = snapshot->name + "_" + std::to_string(suffix);
                                        if (addChannel(key) == 0) {
                                                auto *ch = findChannel(key);
                                                ch->setDevice("FLASH");
                                                ch->setType("f64");
                                                ch->setWritable(false);
                                                ch->arraySnapshot_ = std::move(snapshot);
                                                showFft_           = false;
                                                if (parent_) {
                                                        parent_->showFullData();
                                                        parent_->setModified();
                                                }
                                        }
                                }
                        }
                }
                if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CHANNEL")) {
                        if (payload->DataSize == sizeof(ChannelDropPayload)) {
                                auto *chPayload = static_cast<ChannelDropPayload *>(payload->Data);
                                LOG_I("Scope[%s] drop CHANNEL name='%s' dev='%s' addr=0x%llx",
                                      name_.c_str(),
                                      chPayload->name,
                                      chPayload->device,
                                      (unsigned long long)chPayload->addr);
                                if (parent_ &&
                                    parent_->containsChannelIdentity(chPayload->name, chPayload->addr, chPayload->type)) {
                                        LOG_I("Scope[%s] ignored duplicate CHANNEL '%s'", name_.c_str(), chPayload->name);
                                        return;
                                }
                                if (addChannel(chPayload->name) == 0) {
                                        MonitorChannel *ch = findChannel(chPayload->name);
                                        if (ch) {
                                                ch->setType(chPayload->type);
                                                ch->setAddr(chPayload->addr);
                                                ch->getSymbolName() = chPayload->name;
                                                ch->setDevice(chPayload->device);
                                                if (chPayload->shmName[0] != '\0')
                                                        ch->setShmRegionName(chPayload->shmName);
                                                ch->setBitOffset(chPayload->bitOffset);
                                                ch->setBitSize(chPayload->bitSize);
                                                ch->unitConversion_ = chPayload->unitConversion;
                                                if (chPayload->numEnums > 0) {
                                                        std::vector<MonitorChannel::EnumEntry> ents;
                                                        for (int i = 0; i < chPayload->numEnums; ++i)
                                                                ents.push_back(
                                                                    {chPayload->enums[i].name, chPayload->enums[i].val});
                                                        ch->setEnums(std::move(ents));
                                                }
                                                if (ch->getDevice() == "SHM")
                                                        shmInit(*ch);
                                                if (parent_)
                                                        parent_->setModified();
                                        }
                                }
                                // Mirror the symbol back into the originating Variable's watch
                                // list (only symbol-browser drags tag srcWatch)
                                if (chPayload->srcWatch) {
                                        WatchMirrorRequest req;
                                        req.target   = chPayload->srcWatch;
                                        req.isStruct = false;
                                        req.scalar   = *chPayload;
                                        watchMirrorQueue().push_back(req);
                                }
                        }
                }

                if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("STRUCT_CHANNEL")) {
                        if (payload->DataSize == sizeof(StructChannelPayload)) {
                                auto *sp = static_cast<const StructChannelPayload *>(payload->Data);
                                LOG_I("Scope[%s] drop STRUCT_CHANNEL root='%s' dev='%s' count=%d",
                                      name_.c_str(),
                                      sp->rootName,
                                      sp->device,
                                      (int)sp->count);
                                for (int ei = 0; ei < sp->count; ++ei) {
                                        const auto &e = sp->entries[ei];
                                        if (parent_ && parent_->containsChannelIdentity(e.name, e.addr, e.type))
                                                continue;
                                        if (addChannel(e.name) == 0) {
                                                MonitorChannel *ch = findChannel(e.name);
                                                if (ch) {
                                                        ch->setType(e.type);
                                                        ch->setAddr(e.addr);
                                                        ch->getSymbolName() = e.name;
                                                        ch->setDevice(sp->device);
                                                        if (sp->shmName[0] != '\0')
                                                                ch->setShmRegionName(sp->shmName);
                                                        ch->setBitOffset(e.bitOffset);
                                                        ch->setBitSize(e.bitSize);
                                                        ch->unitConversion_ = e.unitConversion;
                                                        if (e.numEnums > 0) {
                                                                std::vector<MonitorChannel::EnumEntry> ents;
                                                                for (int k = 0; k < e.numEnums; ++k)
                                                                        ents.push_back({e.enums[k].name, e.enums[k].val});
                                                                ch->setEnums(std::move(ents));
                                                        }
                                                        if (ch->getDevice() == "SHM")
                                                                shmInit(*ch);
                                                        if (parent_)
                                                                parent_->setModified();
                                                }
                                        }
                                }
                                // Mirror the whole struct/array back into the source watch list
                                if (sp->srcWatch) {
                                        WatchMirrorRequest req;
                                        req.target   = sp->srcWatch;
                                        req.isStruct = true;
                                        req.group    = *sp;
                                        watchMirrorQueue().push_back(req);
                                }
                        }
                }

                if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("DND_CHANNEL_MOVE"))
                        moveChannelsFrom(static_cast<ChannelMovePayload *>(payload->Data), ImGui::GetIO().KeyShift);
                ImGui::EndDragDropTarget();
        }
}

bool
MonitorScope::moveChannelsFrom(ChannelMovePayload *data, bool copy)
{
        if (!data || !data->srcScope || data->srcScope == this)
                return false;
        if (copy && parent_ && data->srcScope->parent_ == parent_)
                return false;
        // Externally fed channels belong to their controller and its sampling lock
        const bool external =
            (parent_ && parent_->externalFeed_) || (data->srcScope->parent_ && data->srcScope->parent_->externalFeed_);
        if (external && (copy || parent_ != data->srcScope->parent_))
                return false;
        auto &src      = data->srcScope->getChannels();
        bool  movedAny = false;
        if (data->isGroup) {
                // Move every ch under the group: key == prefix or "prefix."
                const std::string        exact = data->chName;
                std::vector<std::string> toMove;
                for (auto &[k, _] : src)
                        if (scopePathContains(exact, k))
                                toMove.push_back(k);
                std::sort(toMove.begin(), toMove.end(), [&](const std::string &a, const std::string &b) {
                        const i64 oa = src[a]->getOrder(), ob = src[b]->getOrder();
                        return oa != ob ? oa < ob : a < b;
                });
                for (auto &k : toMove) {
                        auto it = src.find(k);
                        if (it != src.end() && chs_.find(k) == chs_.end()) {
                                auto ch = copy ? it->second->cloneWithHistory() : std::move(it->second);
                                ch->setOrder(nextChannelOrder_++);
                                if (copy && ch->getDevice() == "SHM")
                                        shmInit(*ch);
                                this->getChannels()[k] = std::move(ch);
                                if (!copy)
                                        src.erase(it);
                                movedAny = true;
                        }
                }
        } else {
                auto it = src.find(data->chName);
                if (it != src.end() && chs_.find(data->chName) == chs_.end()) {
                        auto ch = copy ? it->second->cloneWithHistory() : std::move(it->second);
                        ch->setOrder(nextChannelOrder_++);
                        if (copy && ch->getDevice() == "SHM")
                                shmInit(*ch);
                        this->getChannels()[data->chName] = std::move(ch);
                        if (!copy)
                                src.erase(it);
                        movedAny = true;
                }
        }
        if (!movedAny)
                return false;
        if (!copy && data->srcScope->parent_)
                data->srcScope->parent_->setModified();
        if (parent_)
                parent_->setModified();
        return true;
}

void
MonitorScope::applyChannelMoveDrop(ChannelMovePayload *data, const std::string &dst)
{
        if (!data)
                return;
        if (data->srcScope == this) {
                if (ImGui::GetIO().KeyShift)
                        return; // same scope already contains the ch; copying would collide by name
                if (data->isGroup)
                        reorderGroupBefore(data->chName, dst);
                else
                        reorderChannelTo(data->chName, dst);
                return;
        }
        const bool        movedGroup = data->isGroup;
        const std::string movedName  = data->chName;
        if (!moveChannelsFrom(data, ImGui::GetIO().KeyShift))
                return;
        if (movedGroup)
                reorderGroupBefore(movedName, dst);
        else
                reorderChannelTo(movedName, dst);
}

void
MonitorScope::reinitFft(i32 newPoints)
{
        // Saved sessions may contain invalid sizes. Preserve the current FFT in that case
        if (newPoints < 256 || newPoints > 1048576 || !IS_POWER_OF_2(newPoints)) {
                LOG_W("Scope[%s] invalid FFT point count: %d", name_.c_str(), newPoints);
                return;
        }
        // Guard against the FFT worker thread using fft_ / its buffers mid-transform
        std::lock_guard lk(fftObjMtx_);

        fftPoints_ = newPoints;
        fft_destroy(&fft_);

        fftInBuf_.assign(fftPoints_, 0.0f);
        fftMagF32_.assign(fftPoints_ / 2 + 1, 0.0f);
        fftMagBuf_.assign(fftPoints_ / 2 + 1, 0.0);
        fftOutBuf_.assign((fftPoints_ / 2 + 1) * 2, 0.0f);
        fftLoBuf_.assign(fftPoints_, 0.0f);

        fft_cfg_t cfg{};
        cfg.point_cnt = fftPoints_;
        // The measured rate is zero during session restoration / before sampling
        // Plan creation only needs a valid nominal rate; fftWorkerStep supplies the
        // actual rate from sample timestamps for each transform
        cfg.fs            = 1000.0f;
        cfg.window        = FFT_WINDOW_HANNING;
        cfg.in_buf        = fftInBuf_.data();
        cfg.magnitude_buf = fftMagF32_.data();
        cfg.out_buf       = (decltype(cfg.out_buf))fftOutBuf_.data();
        cfg.buf           = fftLoBuf_.data();
        const int ret     = fft_init(&fft_, cfg);
        if (ret < 0)
                LOG_E("Scope[%s] FFT initialization failed (%d points): %d", name_.c_str(), fftPoints_, ret);
        fftLastRunMs_.store(0, std::memory_order_relaxed);

        for (auto &pair : chs_) {
                pair.second->minKeepPoints_ = static_cast<usize>(newPoints);
        }
}

// --------------------------------------------------------------------------
// Monitor
// --------------------------------------------------------------------------

bool
Monitor::formattedTimeAxis() const
{
        return (timeAxisFormat_ == 1 && absoluteTimeAvailable()) || (timeAxisFormat_ < 0 && importFormatTimeOfDay_);
}

double
Monitor::timeAxisOriginSeconds() const
{
        return importSourcePath_.empty() ? sessionTimeOriginSeconds() : importTimeOriginSeconds_;
}

bool
Monitor::absoluteTimeAvailable() const
{
        return !triggerTimeAxis_ && (importSourcePath_.empty() || importTimeOriginSeconds_ >= 1.0e8);
}

void
Monitor::drawTimeAxisOptions()
{
        ImGui::TextUnformatted(tr("Time axis", "时间轴"));
        const char *labels[]  = {tr("Relative time", "相对时间"), tr("Absolute timestamp", "绝对时间戳")};
        const bool  enabled[] = {true, absoluteTimeAvailable()};
        int         selected  = formattedTimeAxis() && absoluteTimeAvailable() ? 1 : 0;
        if (ui::Choice("##time_axis_format", &selected, labels, 2, enabled)) {
                timeAxisFormat_ = selected;
                isModified_     = true;
        }
        if (selected == 1)
                ImGui::TextDisabled("%s", tr("Local time  YYYY-MM-DD HH:mm:ss.ffffff", "本地时间  年-月-日 时:分:秒.微秒"));
        else if (!enabled[1])
                ImGui::TextDisabled("%s", tr("This data has no calendar-time origin", "此数据没有绝对时间基准"));
}

bool
MonitorScope::hasTimeAxis() const
{
        return !showFft_ && std::none_of(chs_.begin(), chs_.end(), [](const auto &entry) {
                return entry.second && entry.second->arraySnapshot_;
        });
}

void
Monitor::setDiscardSelectionEnabled(bool enabled)
{
        discardSelectionEnabled_ = enabled;
        for (auto &[_, scope] : scopes_) {
                scope->cancelDiscardSelection();
                if (enabled && !scope->isPendingDelete() && !scope->discardUnavailableReason())
                        scope->beginDiscardSelection();
        }
}

void
Monitor::drawDiscardSelectionControls()
{
        bool available = false;
        for (const auto &[_, scope] : scopes_)
                if (scope && !scope->isPendingDelete() && !scope->discardUnavailableReason()) {
                        available = true;
                        break;
                }
        if (discardSelectionEnabled_ && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_Escape))
                setDiscardSelectionEnabled(false);
        ImGui::BeginDisabled(!available && !discardSelectionEnabled_);
        if (ui::Button(tr("Select to discard###discardSelect", "框选丢弃###discardSelect"),
                       discardSelectionEnabled_ ? ui::BtnStyle::Warning : ui::BtnStyle::Neutral))
                setDiscardSelectionEnabled(!discardSelectionEnabled_);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(
                    "%s",
                    (linkScopeCursor_
                         ? tr("Select an interval in any time-domain scope. Confirming discards that interval "
                              "across this monitor, including hidden scopes and curves. "
                              "Flash array scopes are excluded. Esc cancels.",
                              "在任意时域 Scope 中左键拖拽选择时间段，确认后丢弃当前 Monitor 全部 Scope 的同一区间数据，"
                              "包括隐藏 Scope 和曲线；Flash 数组 Scope 不参与。Esc 取消。")
                         : tr("Enable, then drag in a time-domain scope to select an interval. "
                              "Only that scope's channels are affected, including hidden curves. Esc cancels.",
                              "开启后，在需要处理的时域 Scope 中左键拖拽选择时间段。"
                              "仅丢弃该 Scope 的全部通道数据（包括隐藏曲线）。Esc 取消。")));
}

void
Monitor::menu()
{
}

int
Monitor::addScope(const std::string &scopeName)
{
        // Scope names are also used by persistence and drag/drop.  Never admit an
        // empty key: old/corrupt sessions used to pass it to ImGui::BeginChild("")
        // and trip ImGui's empty-root-ID assertion during application startup
        if (scopeName.empty())
                return -1;
        if (scopes_.find(scopeName) != scopes_.end())
                return -1;
        auto scope     = std::make_shared<MonitorScope>(scopeName);
        scope->parent_ = this;
        scope->setOrder(allocScopeOrder()); // default display order = insertion order
        scopes_[scopeName] = scope;
        needsLayout_       = true;
        for (auto &pair : scopes_)
                pair.second->setManual(false); // Reset layout mode
        return 0;
}

MonitorChannel *
Monitor::findChannel(const std::string &scopeName, const std::string &chName)
{
        auto it = scopes_.find(scopeName);
        if (it == scopes_.end())
                return nullptr;
        return it->second->findChannel(chName);
}

bool
Monitor::containsChannelIdentity(const std::string &name, u64 address, const std::string &type) const
{
        const std::string normalizedType = normalizeTypeName(type);
        for (const auto &[_, scope] : scopes_) {
                if (!scope)
                        continue;
                for (const auto &[key, channel] : scope->chs_) {
                        if (!channel || channel->isPendingDelete())
                                continue;
                        if (channel->addr_ == address && channel->type_ == normalizedType &&
                            (key == name || channel->symbolName_ == name))
                                return true;
                }
        }
        return false;
}

void
Monitor::updateUnitConversion(const std::string &name, std::optional<u64> address, const unit_conversion::Settings &settings)
{
        std::lock_guard lock(sMtxInstances_);
        for (auto *monitor : sInstances_) {
                if (!monitor)
                        continue;
                bool changed = false;
                for (auto &[_, scope] : monitor->scopes_) {
                        for (auto &[key, channel] : scope->chs_) {
                                if (!channel || channel->isPendingDelete())
                                        continue;
                                if ((!address || channel->addr_ == *address) && (key == name || channel->symbolName_ == name)) {
                                        channel->unitConversion_ = settings;
                                        changed                  = true;
                                }
                        }
                }
                if (changed)
                        monitor->setModified();
        }
}

// --------------------------------------------------------------------------

static std::vector<std::pair<std::string, MonitorScope *>>
orderedExportScopes(const Monitor::ScopeMapType &scopes)
{
        std::vector<std::pair<std::string, MonitorScope *>> ordered;
        for (const auto &[name, scope] : scopes)
                if (scope && !scope->isPendingDelete())
                        ordered.emplace_back(name, scope.get());
        std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
                return a.second->getOrder() != b.second->getOrder() ? a.second->getOrder() < b.second->getOrder()
                                                                    : a.first < b.first;
        });
        return ordered;
}

static std::vector<std::pair<std::string, MonitorChannel *>>
orderedExportChannels(MonitorScope &scope)
{
        std::vector<std::pair<std::string, MonitorChannel *>> ordered;
        for (const auto &[name, channel] : scope.getChannels())
                if (channel && !channel->isPendingDelete())
                        ordered.emplace_back(name, channel.get());
        std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
                return a.second->getOrder() != b.second->getOrder() ? a.second->getOrder() < b.second->getOrder()
                                                                    : a.first < b.first;
        });
        return ordered;
}

std::optional<Monitor::CsvExportTarget>
Monitor::drawCsvExportMenu()
{
        u64        maxRows = 0, cells = 0;
        size_t     channelCount = 0;
        const auto scopes       = orderedExportScopes(scopes_);
        for (const auto &[_, scope] : scopes)
                for (const auto &[__, channel] : scope->getChannels()) {
                        if (!channel || channel->isPendingDelete())
                                continue;
                        const u64 n =
                            channel->arraySnapshot_ ? channel->arraySnapshot_->values.size() : channel->read_.rawSize();
                        maxRows  = std::max(maxRows, n);
                        cells   += n;
                        ++channelCount;
                }
        const bool exporting  = csvExport_->running.load(std::memory_order_acquire);
        const bool discarding = hasPendingDataDiscard();
        ImGui::BeginDisabled(exporting || discarding || channelCount == 0);
        if (ui::Button(tr("Export CSV", "导出 CSV"), ui::BtnStyle::Success))
                ImGui::OpenPopup("##csvExportRange");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (exporting)
                        ImGui::SetTooltip("%s", tr("CSV export is running", "正在导出 CSV"));
                else if (discarding)
                        ImGui::SetTooltip(
                            "%s", tr("The selected interval is being discarded. Please wait.", "区间数据正在丢弃，请稍候。"));
                else if (channelCount == 0)
                        ImGui::SetTooltip("%s", tr("No channels to export", "没有可导出的通道"));
                else
                        ImGui::SetTooltip(tr("Choose all data, a scope or a channel.\nAll data: ~%s | %zu channels | %llu rows",
                                             "选择导出全部、Scope 或单个通道。\n全部数据：约 %s | %zu 个通道 | %llu 行"),
                                          humanSize(cells * 18 + maxRows * (channelCount * 2 + 1)).c_str(),
                                          channelCount,
                                          static_cast<unsigned long long>(maxRows));
        }
        std::optional<CsvExportTarget> chosen;
        if (ImGui::BeginPopup("##csvExportRange")) {
                ImGui::BeginDisabled(exporting || discarding || channelCount == 0);
                if (ImGui::MenuItem(tr("Export all scopes###csvExportAll", "导出全部 Scope###csvExportAll")))
                        chosen = CsvExportTarget{};
                ImGui::Separator();
                ImGui::TextDisabled("%s", tr("Choose a scope or channel", "选择 Scope 或通道"));
                for (const auto &[name, scope] : scopes) {
                        const auto channels = orderedExportChannels(*scope);
                        const auto label    = scope->getLabel() + "###csvScope_" + name;
                        if (ImGui::BeginMenu(label.c_str(), !channels.empty())) {
                                ImGui::PushID(name.c_str());
                                if (ImGui::MenuItem(tr("Export this scope###csvExportScope", "导出此 Scope###csvExportScope")))
                                        chosen = CsvExportTarget{CsvExportKind::Scope, name, {}};
                                ImGui::Separator();
                                ImGui::TextDisabled("%s", tr("Export one channel", "导出单个通道"));
                                for (const auto &[key, channel] : channels) {
                                        ImGui::PushID(key.c_str());
                                        if (ImGui::MenuItem((channel->getLabel() + "###csvExportChannel").c_str()))
                                                chosen = CsvExportTarget{CsvExportKind::Channel, name, key};
                                        if (ImGui::IsItemHovered())
                                                ImGui::SetTooltip("%s", key.c_str());
                                        ImGui::PopID();
                                }
                                ImGui::PopID();
                                ImGui::EndMenu();
                        }
                }
                ImGui::EndDisabled();
                ImGui::EndPopup();
        }
        return chosen;
}

void
Monitor::exportCsvDialog(const CsvExportTarget &target)
{
        std::string suffix = getTitle();
        if (target.kind != CsvExportKind::Monitor) {
                const auto scope = scopes_.find(target.scope);
                if (scope == scopes_.end() || !scope->second || scope->second->isPendingDelete())
                        return;
                suffix += "_" + scope->second->getLabel();
                if (target.kind == CsvExportKind::Channel) {
                        auto *channel = scope->second->findChannel(target.channel);
                        if (!channel || channel->isPendingDelete())
                                return;
                        suffix += "_" + channel->getLabel();
                }
        }
        for (char &c : suffix)
                if (static_cast<unsigned char>(c) < 32 || std::strchr("\\/:*?\"<>|", c))
                        c = '_';
        char       prefix[64];
        const auto now = std::time(nullptr);
        std::strftime(prefix, sizeof(prefix), "%Y%m%d_%H%M%S_", std::localtime(&now));
        auto path =
            nativeDlgSave(tr("Export CSV", "导出 CSV"), {{"CSV Files", {"csv"}}}, std::string(prefix) + suffix + ".csv");
        if (path.empty())
                return;
        auto extension = path.size() >= 4 ? path.substr(path.size() - 4) : std::string{};
        for (char &c : extension)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (extension != ".csv")
                path += ".csv";
        exportCsvFile(path, target);
}

bool
Monitor::exportCsvFile(const std::string &fullpath, const CsvExportTarget &target)
{
        if (fullpath.empty() || csvExport_->running.load(std::memory_order_acquire) || hasPendingDataDiscard())
                return false;
        // Resolve exactly the requested keys. A deleted/missing target must
        // never turn a scoped export into an export of the entire monitor
        auto   snapshot = std::make_shared<std::vector<CsvExportChannel>>();
        size_t maxLen   = 0;
        for (const auto &[scopeName, scope] : orderedExportScopes(scopes_)) {
                if (target.kind != CsvExportKind::Monitor && scopeName != target.scope)
                        continue;
                for (const auto &[chName, ch] : orderedExportChannels(*scope)) {
                        if (target.kind == CsvExportKind::Channel && chName != target.channel)
                                continue;
                        CsvExportChannel copy;
                        copy.tag       = scopeName + "::" + chName;
                        const size_t n = ch->arraySnapshot_ ? ch->arraySnapshot_->values.size() : ch->read_.rawSize();
                        copy.ts.reserve(n);
                        copy.val.reserve(n);
                        for (size_t i = 0; i < n; ++i) {
                                copy.ts.push_back(ch->arraySnapshot_ ? double(i) * ch->arraySnapshot_->step
                                                                     : ch->read_.rawTs(i));
                                copy.val.push_back(ch->arraySnapshot_ ? ch->arraySnapshot_->values[i]
                                                                      : double(ch->read_.rawVal(i)));
                        }
                        maxLen = std::max(maxLen, n);
                        snapshot->push_back(std::move(copy));
                }
        }
        if (snapshot->empty())
                return false;
        auto state = csvExport_; // shared_ptr keeps progress state alive past Monitor
        state->rows.store(0, std::memory_order_release);
        state->total.store(static_cast<u64>(maxLen), std::memory_order_release);
        state->running.store(true, std::memory_order_release);

        std::string monName = name_;
        std::thread([snapshot, state, fullpath, maxLen, monName]() {
                FILE *f = nativeFopen(fullpath, "w");
                if (!f) {
                        LOG_E("Monitor[%s] failed to open %s for export", monName.c_str(), fullpath.c_str());
                        state->running.store(false, std::memory_order_release);
                        return;
                }
                const auto &chs = *snapshot;

                // Header
                for (size_t i = 0; i < chs.size(); ++i) {
                        fprintf(f,
                                "%s::Time,%s::Value%s",
                                chs[i].tag.c_str(),
                                chs[i].tag.c_str(),
                                (i + 1 == chs.size()) ? "" : ",");
                }
                fprintf(f, "\n");

                // Data
                for (size_t row = 0; row < maxLen; ++row) {
                        for (size_t i = 0; i < chs.size(); ++i) {
                                if (row < chs[i].ts.size()) {
                                        fprintf(f, "%.17g,%.9g", chs[i].ts[row], chs[i].val[row]);
                                } else {
                                        fprintf(f, ",");
                                }
                                if (i + 1 < chs.size())
                                        fprintf(f, ",");
                        }
                        fprintf(f, "\n");
                        if ((row & 0x3FF) == 0)
                                state->rows.store(static_cast<u64>(row), std::memory_order_release);
                }
                fclose(f);
                state->rows.store(static_cast<u64>(maxLen), std::memory_order_release);
                state->running.store(false, std::memory_order_release);
                LOG_I("Monitor[%s] exported to %s (%zu rows)", monName.c_str(), fullpath.c_str(), maxLen);
        }).detach();
        return true;
}

Monitor::UiState
Monitor::captureUiState() const
{
        UiState result{title_, scopeLayout_};
        for (const auto &[key, scope] : scopes_) {
                if (!scope || scope->isPendingDelete())
                        continue;
                ScopeUiState entry{key, scope->label_, scope, scope->getOrder()};
                for (const auto &[name, channel] : scope->chs_)
                        if (channel && !channel->isPendingDelete())
                                entry.channels.push_back({name, channel->label_, channel, channel->getOrder()});
                std::sort(
                    entry.channels.begin(), entry.channels.end(), [](const auto &a, const auto &b) { return a.key < b.key; });
                result.scopes.push_back(std::move(entry));
        }
        std::sort(result.scopes.begin(), result.scopes.end(), [](const auto &a, const auto &b) { return a.key < b.key; });
        return result;
}

void
Monitor::restoreUiState(const UiState &state)
{
        // Metadata and shared ownership only: sample buffers are never rewound.
        std::unordered_set<MonitorChannel *> live;
        for (const auto &[_, scope] : scopes_)
                if (!scope->isPendingDelete())
                        for (const auto &[__, channel] : scope->chs_)
                                if (!channel->isPendingDelete())
                                        live.insert(channel.get());
        for (auto &[_, scope] : scopes_) {
                scope->pendingDelete_.store(true);
                for (auto &[__, channel] : scope->chs_)
                        channel->markPendingDelete();
        }
        ScopeMapType restored;
        for (const auto &entry : state.scopes) {
                auto scope = entry.scope;
                scope->pendingDelete_.store(false);
                scope->label_ = entry.label;
                scope->setOrder(entry.order);
                MonitorScope::ChannelMapType channels;
                for (const auto &item : entry.channels) {
                        auto channel = item.channel;
                        // Restoring a deleted trace never restarts a signal generator
                        // or replays an outstanding device write.
                        if (!live.contains(channel.get())) {
                                channel->waveEnable_ = false;
                                channel->wValDirty_.store(false);
                        }
                        channel->pendingDelete_.store(false);
                        channel->label_ = item.label;
                        channel->setOrder(item.order);
                        channels.emplace(item.key, channel);
                }
                scope->chs_ = std::move(channels);
                restored.emplace(entry.key, scope);
        }
        scopes_      = std::move(restored);
        title_       = state.title;
        scopeLayout_ = state.layout;
        if (!scopes_.contains(focusedScope_))
                focusedScope_.clear();
        needsLayout_ = true;
        setModified();
}

void
Monitor::updateDisplay(bool embedded)
{
        using ui::interaction::Action;
        if (pendingUiAction_ == Action::Undo) {
                if (const auto *state = uiHistory_.undo())
                        restoreUiState(*state);
        } else if (pendingUiAction_ == Action::Redo) {
                if (const auto *state = uiHistory_.redo())
                        restoreUiState(*state);
        } else if (pendingUiAction_ == Action::Parent || pendingUiAction_ == Action::Escape) {
                restoreScopeOverview();
        }
        pendingUiAction_ = Action::None;
        if (pendingUiRoute_) {
                focusedScope_          = scopes_.contains(*pendingUiRoute_) ? *pendingUiRoute_ : std::string{};
                scopeFocusScrollReset_ = true;
                pendingUiRoute_.reset();
        }
        const auto beforeUi = captureUiState();

        linkedCursorDraws_.clear();
        linkedCursorActive_ = false;
        for (auto &[_, scope] : scopes_)
                scope->hoverValues_.clear();
        if (mujocoView_)
                mujocoView_->tick();
        bool open = true;
        // "VisibleTitle###name_": the visible label tracks the user-editable title
        // while the trailing id keeps the ImGui window id (and dock layout) stable
        const std::string stableName   = name_.empty() ? "monitor_" + std::to_string(reinterpret_cast<uintptr_t>(this)) : name_;
        const std::string visibleTitle = getTitle().empty() ? tr("Variable Recorder", "变量记录器") : getTitle();
        const bool        acquisitionPaused = isSamplingPaused();
        const bool        displayFrozen     = g_monitorPaused.load(std::memory_order_acquire);
        const std::string stateLabel        = (acquisitionPaused ? tr("  Acquisition paused", "  采集已暂停") : "") +
                                       std::string(displayFrozen ? tr("  Display frozen", "  画面已冻结") : "");
        const std::string winLabel = visibleTitle + stateLabel + "###" + stableName;
        if (!embedded) {
                const ImGuiViewport *viewport = ImGui::GetMainViewport();
                const float          margin   = ImGui::GetFontSize();
                const ImVec2         initialSize(
                    std::min(std::max(1.0f, viewport->WorkSize.x - margin * 2), std::max(640.0f, margin * 58)),
                    std::min(std::max(1.0f, viewport->WorkSize.y - margin * 2), std::max(480.0f, margin * 38)));
                // A child-based workspace cannot supply useful auto-fit dimensions.
                // Respect caller placement and saved/manual geometry; size only a new panel.
                const auto requested = ImGui::GetCurrentContext()->NextWindowData.HasFlags;
                if (!(requested & ImGuiNextWindowDataFlags_HasSize))
                        ImGui::SetNextWindowSize(initialSize, ImGuiCond_FirstUseEver);
                if (!(requested & ImGuiNextWindowDataFlags_HasPos))
                        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                                       viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                                ImGuiCond_FirstUseEver,
                                                ImVec2(0.5f, 0.5f));
        }
        if (!embedded && fillAppOnNextDraw_) {
                const ImGuiViewport *viewport = ImGui::GetMainViewport();
                ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
                ImGui::SetNextWindowViewport(viewport->ID);
                ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
                ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
                fillAppOnNextDraw_ = false;
        }
        const int tabColours =
            ui::PushPausedDockTabStyle(acquisitionPaused || displayFrozen, displayFrozen && !acquisitionPaused);
        const bool visible =
            embedded
                ? ImGui::BeginChild(stableName.c_str(),
                                    ImVec2(0, 0),
                                    ImGuiChildFlags_None,
                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)
                : dock_close::begin(winLabel.c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor(tabColours);
        if (!open)
                markPendingDelete();
        if (visible) {
                TutorialGuide::instance().markDock(mujocoEnabled_ ? TutorialGuide::Topic::MuJoCo
                                                                  : TutorialGuide::Topic::Recorder);
                const bool previewing = scopeFocusPreview_.active();
                if (scopeFocusPreview_.maintain(focusedScope_))
                        scopeFocusScrollReset_ = false;
                else if (previewing && !scopeFocusPreview_.active())
                        revealFocusedScope();

                // Double-click or F2 renames dock tabs; only empty title-bar space folds panes
                if (!embedded) {
                        ImGuiWindow *win       = ImGui::GetCurrentWindow();
                        ImRect       titleRect = win->DockIsActive ? win->DC.DockTabItemRect : win->TitleBarRect();
                        if ((win->DockIsActive && dock_fold::isTabLabelHovered(win) &&
                             (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_F2, false))) ||
                            (!win->DockIsActive && ImGui::IsMouseHoveringRect(titleRect.Min, titleRect.Max, false) &&
                             ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
                                snprintf(renameBuf_, sizeof(renameBuf_), "%s", getTitle().c_str());
                                ImGui::OpenPopup("Rename Dock");
                        }
                        if (ImGui::BeginPopup("Rename Dock")) {
                                ImGui::TextDisabled("%s", tr("Rename", "重命名"));
                                ImGui::SetNextItemWidth(220);
                                if (ImGui::IsWindowAppearing())
                                        ImGui::SetKeyboardFocusHere();
                                bool commit =
                                    ImGui::InputText("##renameDock",
                                                     renameBuf_,
                                                     sizeof(renameBuf_),
                                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                                ImGui::SameLine();
                                if (ImGui::Button("OK"))
                                        commit = true; // OK keeps its label across languages
                                if (commit) {
                                        if (renameBuf_[0] != '\0')
                                                setTitle(renameBuf_);
                                        ImGui::CloseCurrentPopup();
                                }
                                ImGui::EndPopup();
                        }
                }

                if (csvLoading_.load(std::memory_order_acquire)) {
                        const char *frames[] = {"|", "/", "-", "\\"};
                        int         fi       = static_cast<int>(ImGui::GetTime() * 8.0) % 4;
                        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark),
                                           tr("%s  Importing data...", "%s  正在导入数据..."),
                                           frames[fi]);
                        if (embedded)
                                ImGui::EndChild();
                        else
                                ImGui::End();
                        return;
                }

                if (importFormatTimeOfDay_)
                        ImGui::TextDisabled("%s",
                                            tr("Time axis: YYYY-MM-DD HH:MM:SS.ffffff", "时间轴：年-月-日 时:分:秒.毫秒微秒"));

                // ---- Flow layout for the toolbar ----------------------------------------
                // ImGui's SameLine() never wraps, so a row of widgets overflows past the
                // window edge (and gets clipped / disappears) on a narrow window. tbFlow()
                // is called before each item with that item's width: it keeps the item on
                // the current row when it fits, otherwise lets it fall onto a new row
                ImGuiStyle &tbStyle  = ImGui::GetStyle();
                const float tbRightX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                auto        tbBtnW   = [&](const char *label) {
                        return ImGui::CalcTextSize(label, nullptr, true).x + tbStyle.FramePadding.x * 2.0f;
                };
                auto tbIconW = [&](const char *label) {
                        return tbBtnW(label) + ImGui::GetFontSize() * 1.1f + tbStyle.ItemInnerSpacing.x + 1;
                };
                bool inToolbarOptions = false;
                auto tbFlow           = [&](float nextW) {
                        if (inToolbarOptions)
                                return;
                        // GetItemRectMax() is the right edge of the prevly submitted item
                        if (ImGui::GetItemRectMax().x + tbStyle.ItemSpacing.x + nextW < tbRightX)
                                ImGui::SameLine();
                        // else: fall through to the default cursor advance, i.e. a new row
                };

                if (ui::IconButton(tr("Add Scope", "添加示波器"), ui::Icon::Add)) {
                        char   nameBuf[32];
                        size_t idx = scopes_.size();
                        do {
                                snprintf(nameBuf, sizeof(nameBuf), "scope_%zu", idx++);
                        } while (scopes_.find(nameBuf) != scopes_.end());
                        addScope(nameBuf);
                }
                TutorialGuide::instance().mark("monitor_window");

                const float searchWidth = std::clamp(tbRightX - ImGui::GetItemRectMax().x - tbStyle.ItemSpacing.x,
                                                     ImGui::GetFontSize() * 8,
                                                     ImGui::GetFontSize() * 13);
                tbFlow(searchWidth);
                ImGui::SetNextItemWidth(searchWidth);
                ui::SearchInput("##monitorSearch",
                                tr("Search variable or address", "搜索变量或地址"),
                                channelSearch_,
                                sizeof(channelSearch_));
                TutorialGuide::instance().mark("recorder.search");

                const char *layoutNames[] = {tr("Stacked", "纵向排列"), tr("Grid", "网格排列")};
                tbFlow(ui::ChoiceWidth(layoutNames, 2));
                int layoutChoice = hasFocusedScope() ? -1 : static_cast<int>(scopeLayout_);
                if (ui::Choice("##scopeLayout", &layoutChoice, layoutNames, 2))
                        setScopeLayout(static_cast<ScopeLayout>(layoutChoice));
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "%s",
                            tr("Scope layout; focusing keeps all acquisition running", "示波器布局；聚焦不会停止其他采集"));
                if (hasFocusedScope() && !scopeFocusPreview_.active()) {
                        tbFlow(ImGui::GetFontSize() * 10);
                        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
                        const auto  focused = scopes_.find(focusedScope_);
                        const char *label   = focused != scopes_.end() ? focused->second->getLabel().c_str() : "";
                        if (ImGui::BeginCombo("##focusedScope", label)) {
                                for (const auto &entry : orderedExportScopes(scopes_)) {
                                        if (ImGui::Selectable(entry.second->getLabel().c_str(), entry.first == focusedScope_)) {
                                                entry.second->setHidden(false);
                                                focusedScope_          = entry.first;
                                                scopeFocusScrollReset_ = true;
                                        }
                                }
                                ImGui::EndCombo();
                        }
                }

                bool hasSimChannels = externalFeed_;
                for (const auto &[_, scope] : scopes_)
                        for (const auto &[__, ch] : scope->getChannels())
                                hasSimChannels |= ch->getDevice() == ava_sim::device;
                // A simulation pause stops storage while continuing to drain telemetry
                const bool  monSampPaused = isSamplingPaused();
                const char *pauseAllLabel =
                    monSampPaused ? tr("Resume acquisition", "恢复采集") : tr("Pause acquisition", "暂停采集");
                tbFlow(tbIconW(pauseAllLabel));
                // Resume = go (green); Pause = caution (amber)
                if (ui::IconButton(pauseAllLabel,
                                   monSampPaused ? ui::Icon::Play : ui::Icon::Pause,
                                   monSampPaused ? ui::BtnStyle::Success : ui::BtnStyle::Warning)) {
                        setSamplingPaused(!monSampPaused);
                }
                if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s",
                                          hasSimChannels ? tr("Pause storage; simulation frames continue draining",
                                                              "暂停存储；仿真数据仍被读取，恢复后显示新数据")
                                                         : tr("Pause/resume J-Link acquisition for all scopes in this monitor",
                                                              "暂停/恢复此监视器内所有示波器的J-Link采样"));

                if (displayFrozen) {
                        tbFlow(tbBtnW(tr("Resume display", "恢复画面")));
                        if (ui::Button(tr("Resume display", "恢复画面"), ui::BtnStyle::Primary))
                                g_monitorPaused.store(false);
                }

                tbFlow(tbBtnW(tr("Export CSV", "导出 CSV")));

                const bool exporting = csvExport_->running.load(std::memory_order_acquire);
                if (auto target = drawCsvExportMenu())
                        exportCsvDialog(*target);

                if (exporting) {
                        const u64   done     = csvExport_->rows.load(std::memory_order_acquire);
                        const u64   total    = csvExport_->total.load(std::memory_order_acquire);
                        const char *frames[] = {"|", "/", "-", "\\"};
                        const int   fi       = static_cast<int>(ImGui::GetTime() * 8.0) % 4;
                        const float pct      = total ? (100.0f * static_cast<float>(done) / static_cast<float>(total)) : 0.0f;
                        char        prog[96];
                        snprintf(prog,
                                 sizeof(prog),
                                 tr("%s  Exporting CSV in background... %.0f%%", "%s  正在后台导出 CSV... %.0f%%"),
                                 frames[fi],
                                 pct);
                        tbFlow(ImGui::CalcTextSize(prog).x);
                        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s", prog);
                }

                tbFlow(tbIconW(tr("Options", "选项")));
                const bool        activateOptions = ui::IconButton(tr("Options", "选项"), ui::Icon::Settings);
                const auto        optionsItem     = ImGui::GetCurrentContext()->LastItemData;
                bool              clearRequested  = false;
                const std::string optionsName     = "##MonitorOptions_" + name_;
                if (ui::BeginHoverPanel(optionsName.c_str(),
                                        tr("Recorder options", "记录器选项"),
                                        optionsReveal_,
                                        activateOptions,
                                        ImGui::GetFontSize() * 23,
                                        ImGui::GetWindowViewport())) {
                        inToolbarOptions = true;
                        ImGui::TextDisabled("%s", tr("Display and acquisition", "显示与采集"));
                        ImGui::Separator();
                        ImGui::TextUnformatted(tr("History to keep (seconds; 0 = all)", "保留历史时长（秒；0 为全部）"));
                        // Logarithmic sliders (same style as the J-Link speed control): drag to
                        // set, Ctrl+click to type. Units are baked into the format string
                        tbFlow(120);
                        ImGui::SetNextItemWidth(-1);
                        f32 h = historySeconds_;
                        if (ImGui::SliderFloat("##History", &h, 0.0f, 3600.0f, "%.1f s", ImGuiSliderFlags_Logarithmic)) {
                                historySeconds_ = h;
                                isModified_     = true;
                        }
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", tr("History(s)", "历史时长(秒)"));

                        ImGui::TextUnformatted(tr("Maximum displayed points", "最大显示点数"));
                        tbFlow(120);
                        ImGui::SetNextItemWidth(-1);
                        i32 maxPts = static_cast<i32>(maxDisplayPoints_);
                        if (ImGui::SliderInt("##MaxPts", &maxPts, 100, 100000, "%d pts", ImGuiSliderFlags_Logarithmic)) {
                                maxDisplayPoints_ = static_cast<u32>(maxPts);
                                isModified_       = true;
                        }
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", tr("Max Pts", "最大显示点数"));

                        const auto  cursorLabel   = tr("Sync operations", "同步操作");
                        const float checkboxWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
                        // Keep the two related controls together when the toolbar wraps
                        tbFlow(2 * checkboxWidth + ImGui::CalcTextSize(cursorLabel).x + ImGui::GetStyle().ItemSpacing.x +
                               ImGui::CalcTextSize(tr("Measure time", "测量时间")).x);
                        if (ImGui::Checkbox(cursorLabel, &linkScopeCursor_))
                                isModified_ = true;
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip(
                                    "%s",
                                    tr("Synchronize cursor readouts and interval discard across this monitor. "
                                       "Discard includes hidden scopes and curves; Flash array scopes are excluded.",
                                       "同步当前 Monitor 的游标读数和框选丢弃操作。"
                                       "丢弃同一时间段内全部 Scope 的数据，包括隐藏 Scope 和曲线；Flash 数组 Scope 不参与。"));
                        ImGui::SameLine();
                        drawTimeMeasurementControls(timeMeasurement_);

                        tbFlow(ImGui::CalcTextSize(tr("Select to discard", "框选丢弃")).x +
                               ImGui::GetStyle().FramePadding.x * 2);
                        drawDiscardSelectionControls();

                        ImGui::Spacing();
                        drawTimeAxisOptions();

                        // POLL-mode sample rate: only shown when this monitor is in POLL mode
                        // HSS rate is global and lives in the app top bar
                        if (!externalFeed_ && samplingMode_ == SamplingMode::POLL) {
                                tbFlow(120);
                                ImGui::TextUnformatted(tr("Target sample rate", "目标采样率"));
                                ImGui::SetNextItemWidth(-1);
                                if (ImGui::SliderInt("##MaxHz", &maxSampleHz_, 1, 50000, "%d Hz", ImGuiSliderFlags_Logarithmic))
                                        isModified_ = true;
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s",
                                                          tr("POLL sample rate (Hz). Limited by USB latency (~1-2 kHz).",
                                                             "POLL 采样率 (Hz)，受 USB 延迟限制（约 1-2 kHz）。"));
                        }

                        if (hasSimChannels && !externalFeed_) {
                                char status[192];
                                snprintf(status,
                                         sizeof(status),
                                         tr("SHM %s | %llu frames | %llu dropped", "SHM %s | %llu 帧 | 丢帧 %llu"),
                                         simConnected_.load() ? tr("Live", "接收中") : tr("Waiting", "等待数据"),
                                         static_cast<unsigned long long>(simFramesReceived_.load()),
                                         static_cast<unsigned long long>(simDroppedFrames_.load()));
                                tbFlow(ImGui::CalcTextSize(status).x);
                                ImGui::TextDisabled("%s", status);
                        }

                        ImGui::Separator();
                        if (!externalFeed_) {
                                const char *sampModes[] = {"HSS", "POLL"};
                                i32         curSamp     = (samplingMode_ == SamplingMode::HSS) ? 0 : 1;
                                ImGui::TextUnformatted(
                                    tr("Acquisition mode (changing clears data)", "采集模式（更改将清空数据）"));
                                ImGui::SetNextItemWidth(-1);
                                if (ui::Choice("##SampMode", &curSamp, sampModes, 2))
                                        pendingSamplingMode_ = curSamp;
                                if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%s", tr("Sampling Mode", "采样模式"));
                        }
                        // Single MODE combo — targets FFT axis when any scope is in FFT view,
                        // otherwise targets the time-domain axis
                        bool anyFft = false;
                        for (const auto &sp : scopes_)
                                if (sp.second && sp.second->isFftEnabled()) {
                                        anyFft = true;
                                        break;
                                }

                        const char *viewModeNames[] = {tr("FULL", "全览"), tr("FOLLOW", "跟随"), tr("MANUAL", "手动")};
                        i32         curMode         = anyFft ? (i32)fftViewMode_ : (i32)viewMode_;

                        ImGui::TextUnformatted(tr("Visible range", "显示范围"));
                        ImGui::SetNextItemWidth(-1);
                        if (ui::Choice("##Mode", &curMode, viewModeNames, 3)) {
                                if (anyFft)
                                        fftViewMode_ = static_cast<MonitorViewMode>(curMode);
                                else if (static_cast<MonitorViewMode>(curMode) == MonitorViewMode::FULL)
                                        showFullData();
                                else
                                        viewMode_ = static_cast<MonitorViewMode>(curMode);
                        }
                        if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s",
                                                  anyFft ? tr("Frequency Domain View Mode", "频域视图模式")
                                                         : tr("Time Domain View Mode", "时域视图模式"));

                        ImGui::Separator();
                        clearRequested = ImGui::MenuItem(tr("Clear captured data...", "清空已采集数据..."));
                        ImGui::End();
                }
                inToolbarOptions                         = false;
                ImGui::GetCurrentContext()->LastItemData = optionsItem;
                for (auto &[_, scope] : scopes_)
                        for (auto &[__, ch] : scope->getChannels())
                                ch->historySeconds_ = historySeconds_;
                // Keep the active selection mode visible and cancellable after the options popup closes.
                if (discardSelectionEnabled_) {
                        tbFlow(tbBtnW(tr("Select to discard", "框选丢弃")));
                        drawDiscardSelectionControls();
                }
                float  minHz = std::numeric_limits<float>::max(), maxHz = 0;
                size_t rateChannels = 0;
                for (const auto &[_, scope] : scopes_) {
                        if (!scope || scope->isPendingDelete())
                                continue;
                        for (const auto &[__, channel] : scope->getChannels()) {
                                if (!channel || channel->isPendingDelete() || channel->arraySnapshot_)
                                        continue;
                                const float hz = isSamplingPaused() || scope->isPaused() ? 0 : channel->getHz();
                                minHz          = std::min(minHz, hz);
                                maxHz          = std::max(maxHz, hz);
                                ++rateChannels;
                        }
                }
                char rateText[64];
                snprintf(rateText, sizeof(rateText), "%.1f Hz", maxHz);
                const float rateWidth = ImGui::CalcTextSize(rateText).x;
                tbFlow(rateWidth);
                ImGui::SetCursorScreenPos(
                    ImVec2(std::max(ImGui::GetCursorScreenPos().x, tbRightX - rateWidth), ImGui::GetCursorScreenPos().y));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(rateText);
                if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            tr("Received samples per channel per second; fastest channel shown.\n%zu channels: %.1f - %.1f Hz",
                               "每个通道每秒收到的数据点数，显示最快通道。\n%zu 个通道：%.1f - %.1f Hz"),
                            rateChannels,
                            rateChannels ? minHz : 0.0f,
                            maxHz);
                }
                const char *modePopup =
                    tr("Change acquisition mode?###change_mode_confirm", "更改采集模式？###change_mode_confirm");
                if (pendingSamplingMode_ >= 0 && !ImGui::IsPopupOpen(modePopup))
                        ImGui::OpenPopup(modePopup);
                if (ImGui::BeginPopupModal(modePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                        ImGui::TextUnformatted(
                            tr("Changing mode clears this monitor's captured data.", "更改模式会清空当前监视器已采集的数据。"));
                        if (ImGui::Button(tr("Cancel", "取消"))) {
                                pendingSamplingMode_ = -1;
                                ImGui::CloseCurrentPopup();
                        }
                        ImGui::SetItemDefaultFocus();
                        ImGui::SameLine();
                        if (ui::Button(tr("Change mode and clear", "更改模式并清空"), ui::BtnStyle::Danger)) {
                                samplingMode_        = pendingSamplingMode_ == 0 ? SamplingMode::HSS : SamplingMode::POLL;
                                pendingSamplingMode_ = -1;
                                requestClearData();
                                if (!externalFeed_)
                                        JLinkPort::instance().reqRestart();
                                isModified_ = true;
                                ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                }
                const char *clearPopup =
                    tr("Clear captured data?###clear_monitor_confirm", "清空已采集数据？###clear_monitor_confirm");
                if (clearRequested)
                        ImGui::OpenPopup(clearPopup);
                if (ImGui::BeginPopupModal(clearPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                        ImGui::TextUnformatted(
                            tr("This clears recorded points in this monitor. Export CSV first to keep a copy.",
                               "此操作会清空当前监视器的记录。需要保留时，请先导出 CSV。"));
                        if (ImGui::Button(tr("Cancel", "取消")))
                                ImGui::CloseCurrentPopup();
                        ImGui::SetItemDefaultFocus();
                        ImGui::SameLine();
                        if (ui::Button(tr("Clear data", "清空数据"), ui::BtnStyle::Danger)) {
                                requestClearData();
                                if (!externalFeed_)
                                        JLinkPort::instance().reqRestart();
                                ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                }
                ImGui::Separator();

                channelSearchQuery_ = ui::search::LowerCopy(channelSearch_);
                if (ui::SearchOpen("##monitorSearch")) {
                        std::vector<ui::SearchResult>                    results;
                        std::vector<std::pair<std::string, std::string>> targets;
                        for (const auto &[scopeName, scope] : orderedExportScopes(scopes_)) {
                                scope->setSearchQuery(channelSearchQuery_);
                                std::vector<std::string> names;
                                for (const auto &[name, channel] : scope->chs_)
                                        if (scope->matchesSearch(name, *channel))
                                                names.push_back(name);
                                std::sort(names.begin(), names.end());
                                for (const auto &name : names) {
                                        const auto &channel = scope->chs_.at(name);
                                        results.push_back({scope->getLabel() + " / " + channel->getLabel(),
                                                           name + " / " + ui::search::AddressText(channel->addr_)});
                                        targets.emplace_back(scopeName, name);
                                }
                        }
                        const int selected = ui::SearchResults("##monitorSearch", results);
                        if (selected >= 0) {
                                const auto &[scopeName, name] = targets[selected];
                                restoreScopeOverview();
                                auto &scope = scopes_.at(scopeName);
                                scope->setHidden(false);
                                scope->setDraw(MonitorScope::DrawEnum::TABLE);
                                scope->tableFocusPath_.clear();
                                for (size_t i = 0; i < name.size(); ++i)
                                        if (name[i] == '.' || name[i] == '[')
                                                scope->expandedGroups_.insert(name.substr(0, i));
                                scope->searchSelectedChannel_ = name;
                                scope->searchScroll_          = true;
                                needsLayout_                  = true;
                        }
                }
                // Searching navigates without hiding other scopes or interrupting acquisition.
                for (const auto &[_, scope] : scopes_)
                        scope->setSearchQuery({});

                if (mujocoEnabled_ && mujocoView_) {
                        mujocoControl_ = mujocoView_->control();
                        for (auto &[name, scope] : scopes_)
                                if ((name == "Error" || name == "Error (rad)") &&
                                    (scope->getLabel() == name ||
                                     scope->getLabel() == tr("Position error (rad)", "位置误差 (rad)") ||
                                     scope->getLabel() == tr("Velocity error (rad/s)", "速度误差 (rad/s)")))
                                        scope->setLabel(mujocoControl_.mode == ava_mujoco::Mode::Velocity
                                                            ? tr("Velocity error (rad/s)", "速度误差 (rad/s)")
                                                            : tr("Position error (rad)", "位置误差 (rad)"));
                }
                std::vector<std::string> keys;
                for (auto &pair : scopes_)
                        keys.push_back(pair.first);
                // Draw scopes in user-defined display order (reorderable via the toolbar)
                std::sort(keys.begin(), keys.end(), [&](const std::string &a, const std::string &b) {
                        return scopes_[a]->getOrder() < scopes_[b]->getOrder();
                });

                if (!focusedScope_.empty()) {
                        const auto focused = scopes_.find(focusedScope_);
                        if (focused == scopes_.end() || focused->second->isPendingDelete() ||
                            (focused->second->isHidden() && !scopeFocusPreview_.active()))
                                restoreScopeOverview();
                }

                std::vector<std::string> controlKeys;
                const bool               mujocoLayout = mujocoEnabled_ && !hasFocusedScope() &&
                                          ImGui::BeginTable("##mujoco_layout",
                                                            2,
                                                            ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp |
                                                                ImGuiTableFlags_BordersInnerV);
                if (mujocoLayout) {
                        ImGui::TableSetupColumn(tr("Waveforms", "波形"), ImGuiTableColumnFlags_WidthStretch, 0.55f);
                        ImGui::TableSetupColumn(
                            tr("MuJoCo model / controls", "MuJoCo 模型 / 控制"), ImGuiTableColumnFlags_WidthStretch, 0.45f);
                        ui::HierarchyNextRow();
                        ImGui::TableSetColumnIndex(0);
                        for (const auto &key : keys)
                                if ((key == "Targets" || key == "PD Parameters") &&
                                    scopes_[key]->getDraw() == MonitorScope::DrawEnum::TABLE)
                                        controlKeys.push_back(key);
                        std::erase_if(keys, [&](const auto &key) {
                                return std::find(controlKeys.begin(), controlKeys.end(), key) != controlKeys.end();
                        });
                }
                if (hasFocusedScope())
                        std::erase_if(keys, [&](const auto &key) { return key != focusedScope_; });
                ImVec2    avail       = ImGui::GetContentRegionAvail();
                const int gridColumns = scopeLayout_ == ScopeLayout::Grid && !hasFocusedScope()
                                            ? ui::ScopeGridColumns(avail.x, ImGui::GetFontSize())
                                            : 1;
                // Time plots share their range already; only the last visible time plot
                // needs the X decorations. FFT axes use different units and remain independent
                MonitorScope *bottomTimeScope = nullptr;
                for (const auto &key : keys) {
                        const auto &scope = scopes_[key];
                        scope->showXAxis_ = true;
                        if (!scope->isPendingDelete() && !scope->isDisplayHidden() &&
                            scope->getDraw() == MonitorScope::DrawEnum::PLOT && !scope->isFftEnabled())
                                bottomTimeScope = scope.get();
                }
                for (const auto &key : keys) {
                        auto &scope = scopes_[key];
                        if (!scope->isFftEnabled() && scope->getDraw() == MonitorScope::DrawEnum::PLOT)
                                scope->showXAxis_ = gridColumns > 1 || scope.get() == bottomTimeScope;
                }
                // Adaptive Layout Engine
                bool anyManual = false;
                for (auto &key : keys) {
                        if (!scopes_[key]->isDisplayHidden() && scopes_[key]->isManual()) {
                                anyManual = true;
                                break;
                        }
                }

                const f32 hiddenHeight = ImGui::GetFrameHeightWithSpacing() + 15.0f;
                size_t    visibleCount = 0;
                for (const auto &key : keys)
                        visibleCount += !scopes_[key]->isDisplayHidden();
                const float minimumHeight = ui::ScopeMinimumHeight(ImGui::GetFontSize(), hiddenHeight);
                // Hidden scopes retain a compact toolbar for restoring them. Reserve
                // those toolbars and all splitters before sharing the remaining area
                auto computeEqualHeight = [&](f32 totalY) -> f32 {
                        const auto N = keys.size();
                        if (N == 0 || visibleCount == 0)
                                return 10.0f;
                        const f32 overhead = static_cast<f32>(N - 1) * 5.0f + static_cast<f32>(N - visibleCount) * hiddenHeight;
                        const int rows     = static_cast<int>((visibleCount + gridColumns - 1) / gridColumns);
                        return ui::ScopePaneHeight(totalY, rows, overhead / gridColumns, minimumHeight);
                };
                const bool resetScopeScroll = needsLayout_ || scopeFocusScrollReset_;
                scopeFocusScrollReset_      = false;
                if (!hasFocusedScope() &&
                    (visibleCount == 1 || needsLayout_ || (!anyManual && std::abs(avail.y - prevAvailY_) > 1.0f))) {
                        const f32 equalHeight = computeEqualHeight(avail.y);
                        for (const auto &key : keys) {
                                auto &scope = scopes_[key];
                                if (scope->isDisplayHidden())
                                        continue;
                                scope->getHeight() = equalHeight;
                                if (needsLayout_)
                                        scope->setManual(false);
                        }
                        needsLayout_ = false;
                }
                prevAvailY_ = avail.y;

                // Compute data bounds ONCE per frame (was O(scopes² × chs) — hoisted out of the per-scope loop)
                const bool isPaused = g_monitorPaused.load();
                const f64  now      = sessionTimeSec();
                f64        earliest = now;
                f64        latest   = 0.0;
                bool       hasData  = false;
                for (const auto &[_, sc] : scopes_) {
                        for (const auto &[__, ch] : sc->getChannels()) {
                                const f64  e = ch->earliestTs();
                                const f64  l = ch->latestTs();
                                const bool populated =
                                    ch->arraySnapshot_ ? !ch->arraySnapshot_->values.empty() : ch->read_.rawSize() != 0;
                                if (populated && std::isfinite(e) && (triggerTimeAxis_ || e >= 0.0f)) {
                                        if (!hasData) {
                                                earliest = e;
                                                latest   = l;
                                        }
                                        hasData = true;
                                        if (e < earliest)
                                                earliest = e;
                                        if (l > latest)
                                                latest = l;
                                }
                        }
                }

                // Drive linkXMin_ / linkXMax_ from the view mode (also once per frame)
                // Each simulation run starts its clock again. A manual viewport
                // from the previous run can otherwise exclude every new sample
                const u64 simulationEpoch = simEpoch_.load();
                if (mujocoEnabled_ && simulationEpoch && simulationEpoch != displayedSimEpoch_ && hasData) {
                        displayedSimEpoch_ = simulationEpoch;
                        dataStartTime_     = earliest;
                        viewMode_          = MonitorViewMode::FULL;
                }
                if (viewMode_ == MonitorViewMode::FULL) {
                        // FULL is a view operation, so it must remain available while
                        // display publishing is paused. In that state read_ is already
                        // frozen; recomputing its bounds changes only the viewport
                        if (hasData) {
                                if (latest < dataStartTime_)
                                        latest = dataStartTime_;
                                linkXMin_                = earliest;
                                linkXMax_                = latest;
                                const double minimumSpan = triggerTimeAxis_ ? 1e-15 : 1.0;
                                if (linkXMax_ - linkXMin_ < minimumSpan)
                                        linkXMax_ = linkXMin_ + minimumSpan;
                        } else {
                                linkXMin_ = dataStartTime_;
                                linkXMax_ = dataStartTime_ + 1.0;
                        }
                        prevNow_ = now;
                } else if (!isPaused && hasData) {
                        // SDK/LOCAL and other external feeds have valid timestamps
                        // even when no J-Link HSS acquisition is running
                        // FOLLOW mode: don't auto-update while the user is interacting with the plot
                        if (viewMode_ == MonitorViewMode::FOLLOW && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                            ImGui::GetIO().MouseWheel == 0) {
                                f64 span        = linkXMax_ - linkXMin_;
                                f64 currentSpan = (span > (triggerTimeAxis_ ? 1e-15 : 0.001)) ? span : 1.0;
                                if (historySeconds_ > 0.0f && currentSpan > historySeconds_) {
                                        currentSpan = historySeconds_;
                                }
                                f64 refTime = latest;
                                linkXMax_   = refTime;
                                linkXMin_   = refTime - currentSpan;
                                // Retain the full span while the acquisition clock is
                                // younger than the window. Clipping at zero feeds a
                                // smaller span into the next frame and collapses it
                                prevNow_ = refTime;
                        }
                }
                wasPaused_ = isPaused;

                // Scrollable scope area: outer window has NoScrollbar so avail.y is stable;
                // this inner child can scroll when the user manually enlarges a scope
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                if (ImGui::BeginChild("##scope_area", ImVec2(avail.x, avail.y), false)) {
                        ImGui::PopStyleVar();
                        if (resetScopeScroll)
                                ImGui::SetScrollY(0.0f);
                        if (keys.empty() && !channelSearchQuery_.empty())
                                ImGui::TextDisabled("%s", tr("No matching variables", "没有匹配的变量"));
                        const ImVec2 normalItemSpacing = ImGui::GetStyle().ItemSpacing;
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(normalItemSpacing.x, 1));
                        const bool gridTable =
                            gridColumns > 1 && ImGui::BeginTable("##scopeGrid", gridColumns, ImGuiTableFlags_SizingStretchSame);
                        const bool alignedPlots = !gridTable && ImPlot::BeginAlignedPlots("##monitorScopeAxes", true);
                        for (size_t i = 0; i < keys.size(); ++i) {
                                auto &scope = scopes_[keys[i]];

                                if (gridTable)
                                        ImGui::TableNextColumn();
                                const bool  scopeHidden = scope->isDisplayHidden();
                                const float innerW      = ImGui::GetContentRegionAvail().x;
                                const float childHeight =
                                    scopeHidden         ? hiddenHeight
                                    : hasFocusedScope() ? std::max(minimumHeight, avail.y - ImGui::GetStyle().ItemSpacing.y)
                                    : gridTable         ? computeEqualHeight(avail.y)
                                                        : std::max(minimumHeight, scope->getHeight());
                                // The visible scope name must not be the child-window ID.  Besides
                                // allowing legacy empty names to crash, renaming a scope discarded
                                // its ImGui state.  The scope object has a stable lifetime/address
                                ImGui::PushID(static_cast<const void *>(scope.get()));
                                const auto scopeFlags = scope->getDraw() == MonitorScope::DrawEnum::PLOT
                                                            ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                                                            : ImGuiWindowFlags_None;
                                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2, 2));
                                if (scope->searchScroll_)
                                        ImGui::SetScrollHereY(0.5f);
                                const bool scopeVisible =
                                    ImGui::BeginChild("##scope", ImVec2(innerW, childHeight), true, scopeFlags);
                                ImGui::PopStyleVar();
                                // Only the gaps between scopes are compact. Menus and
                                // combo popups inherit the same spacing as the manager.
                                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, normalItemSpacing);
                                if (scopeVisible) {
                                        if (scope->getDraw() == MonitorScope::DrawEnum::PLOT)
                                                ImGui::SetScrollY(0.0f);
                                        scope->menu();

                                        if (!scopeHidden) {
                                                if (scope->isFftEnabled()) {
                                                        scope->draw(&linkXMin_, &linkXMax_, maxDisplayPoints_, fftViewMode_);
                                                } else {
                                                        scope->draw(&linkXMin_, &linkXMax_, maxDisplayPoints_, viewMode_);
                                                }
                                        }
                                        scope->dropTarget();
                                }
                                ImGui::PopStyleVar();
                                ImGui::EndChild();
                                ImGui::PopID();

                                if (!gridTable && !hasFocusedScope() && i + 1 < keys.size()) {
                                        ImGui::PushID(static_cast<i32>(i));
                                        ImGui::InvisibleButton("##splitter", ImVec2(-1, 3.0f));
                                        if (ImGui::IsItemActive() && !scope->isDisplayHidden()) {
                                                scope->getHeight() += ImGui::GetIO().MouseDelta.y;
                                                if (scope->getHeight() < minimumHeight)
                                                        scope->getHeight() = minimumHeight;
                                                scope->setManual(true);
                                        }
                                        if (ImGui::IsItemHovered()) {
                                                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                                                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                                        const f32 eqH = computeEqualHeight(avail.y);
                                                        for (auto &[k, sc] : scopes_) {
                                                                if (!sc->isDisplayHidden())
                                                                        sc->getHeight() = eqH;
                                                                sc->setManual(false);
                                                        }
                                                        needsLayout_ = false;
                                                        ImGui::SetScrollY(0.0f); // reset inner child scroll
                                                }
                                        }
                                        ImGui::PopID();
                                }
                        }
                        if (alignedPlots)
                                ImPlot::EndAlignedPlots();
                        if (gridTable)
                                ImGui::EndTable();
                        ImGui::PopStyleVar();
                } else {
                        ImGui::PopStyleVar();
                }
                ImGui::EndChild();

                if (linkedCursorActive_)
                        for (const auto &drawCursor : linkedCursorDraws_)
                                drawCursor(linkedCursorTime_);
                linkedCursorDraws_.clear();

                if (mujocoLayout) {
                        ImGui::TableSetColumnIndex(1);
                        if (ImGui::BeginChild("##mujoco_right", ImVec2(0, 0))) {
                                if (!mujocoView_) {
                                        mujocoView_ = std::make_unique<MuJoCoView>();
                                        if (mujocoModelPath_.empty())
                                                mujocoModelPath_ = MuJoCoView::defaultModelPath();
                                        if (!mujocoModelPath_.empty())
                                                mujocoView_->loadModel(mujocoModelPath_);
                                        mujocoView_->setControl(mujocoControl_);
                                }
                                std::string region   = ava_sim::defaultRegion;
                                double      position = 0;
                                for (auto &[scopeName, scope] : scopes_)
                                        for (auto &[name, ch] : scope->getChannels())
                                                if (ch->getDevice() == ava_sim::device) {
                                                        if (!ch->getShmRegionName().empty())
                                                                region = ch->getShmRegionName();
                                                        if (ch->getSimField() == 1)
                                                                position = ch->getDispRawVal();
                                                }
                                const float available   = ImGui::GetContentRegionAvail().y;
                                const float sceneHeight = std::min(mujocoHeight_, std::max(120.0f, available * 0.65f));
                                if (ImGui::BeginChild("##mujoco_scene", ImVec2(0, sceneHeight), true))
                                        mujocoView_->draw(region, simConnected_.load(), position);
                                ImGui::EndChild();
                                if (!mujocoView_->modelPath().empty() && mujocoModelPath_ != mujocoView_->modelPath()) {
                                        mujocoModelPath_ = mujocoView_->modelPath();
                                        setModified();
                                }
                                {
                                        ImGui::InvisibleButton("##mujoco_split", ImVec2(-1, 6));
                                        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                                                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                                        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                                                mujocoHeight_ =
                                                    std::clamp(mujocoHeight_ + ImGui::GetIO().MouseDelta.y, 120.0f, 1000.0f);
                                                setModified();
                                        }
                                        if (ImGui::BeginChild("##mujoco_controls", ImVec2(0, 0), true)) {
                                                const unsigned edited = mujocoView_->drawControls();
                                                mujocoControl_        = mujocoView_->control();
                                                if (edited) {
                                                        setModified();
                                                        for (auto &[_, scope] : scopes_)
                                                                for (auto &[__, channel] : scope->getChannels())
                                                                        if (channel->getDevice() == ava_sim::device &&
                                                                            (channel->getShmRegionName().empty() ||
                                                                             channel->getShmRegionName() == region) &&
                                                                            (edited & (1u << channel->getSimField()))) {
                                                                                channel->waveEnable_ = false;
                                                                                if (channel->isWritable()) {
                                                                                        const unsigned field =
                                                                                            channel->getSimField();
                                                                                        const double val =
                                                                                            field == 0
                                                                                                ? mujocoControl_.positionTarget
                                                                                            : field == 2
                                                                                                ? (mujocoControl_.mode ==
                                                                                                           ava_mujoco::Mode::
                                                                                                               Position
                                                                                                       ? mujocoControl_
                                                                                                             .velocityFeedforward
                                                                                                       : mujocoControl_
                                                                                                             .velocityTarget)
                                                                                            : field == 6 ? ava_mujoco::activeKp(
                                                                                                               mujocoControl_)
                                                                                                         : mujocoControl_.pdKd;
                                                                                        channel->setWVal(
                                                                                            static_cast<float>(val));
                                                                                        channel->markWValDirty();
                                                                                }
                                                                        }
                                                }
                                                if (!controlKeys.empty() &&
                                                    ImGui::CollapsingHeader(tr("SHM command tables", "共享内存命令表"))) {
                                                        for (const auto &key : controlKeys) {
                                                                auto &scope = scopes_[key];
                                                                ImGui::PushID(scope.get());
                                                                scope->menu();
                                                                if (!scope->isHidden())
                                                                        scope->draw(&linkXMin_,
                                                                                    &linkXMax_,
                                                                                    maxDisplayPoints_,
                                                                                    viewMode_);
                                                                ImGui::PopID();
                                                                ImGui::Separator();
                                                        }
                                                }
                                        }
                                        ImGui::EndChild();
                                }
                        }
                        ImGui::EndChild();
                        ImGui::EndTable();
                }

                // Apply a pending reorder request: swap this scope's display order with
                // its neighbour in the current order. `keys` is sorted by order above, so
                // index i is the on-screen position. One move per frame
                for (size_t i = 0; i < keys.size(); ++i) {
                        const int dir = scopes_[keys[i]]->consumeMoveRequest();
                        if (dir == 0)
                                continue;
                        const int j = static_cast<int>(i) + dir;
                        if (j >= 0 && j < static_cast<int>(keys.size())) {
                                const i64 oi = scopes_[keys[i]]->getOrder();
                                const i64 oj = scopes_[keys[j]]->getOrder();
                                scopes_[keys[i]]->setOrder(oj);
                                scopes_[keys[j]]->setOrder(oi);
                                setModified();
                        }
                        break;
                }

                // Apply drag-drop reorder: swap two scopes' display orders by name
                for (auto &[scopeName, scope] : scopes_) {
                        std::string swapWith = scope->consumeSwap();
                        if (swapWith.empty() || swapWith == scopeName)
                                continue;
                        auto it = scopes_.find(swapWith);
                        if (it != scopes_.end() && it->second) {
                                const i64 oi = scope->getOrder();
                                const i64 oj = it->second->getOrder();
                                scope->setOrder(oj);
                                it->second->setOrder(oi);
                                setModified();
                        }
                        break;
                }

                bool anyDeleted = false;
                for (auto it = scopes_.begin(); it != scopes_.end();) {
                        if (it->second && it->second->isPendingDelete()) {
                                it         = scopes_.erase(it);
                                anyDeleted = true;
                        } else {
                                ++it;
                        }
                }
                if (anyDeleted) {
                        needsLayout_ = true;
                        for (auto &pair : scopes_)
                                pair.second->setManual(false);
                }
        }
        uiHistory_.record(beforeUi, captureUiState());
        if (!isPendingDelete())
                ui::interaction::registerRegion(
                    focusedScope_,
                    uiHistory_.canUndo(),
                    uiHistory_.canRedo(),
                    hasFocusedScope(),
                    [this](Action action) { pendingUiAction_ = action; },
                    [this](const std::string &route) { pendingUiRoute_ = route; });
        if (embedded)
                ImGui::EndChild();
        else
                ImGui::End();
}
