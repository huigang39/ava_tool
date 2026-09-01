#include "cJSON.h"
#include "gui/i18n.hpp"
#include "gui/numeric_format.hpp"
#include "gui/variable.hpp"
#include "imgui.h"
#include <cstdio>

void
Variable::addFlashArray(std::shared_ptr<const flasharray::Snapshot> snapshot)
{
        if (!snapshot || snapshot->values.empty())
                return;
        VarEntry entry{};
        entry.name = snapshot->name.empty() ? "FlashArray" : snapshot->name;
        if (watchHasIdentity(entry.name, snapshot->address, DataType::ARRAY))
                return;
        const auto base = entry.name;
        for (int suffix = 2; watchHasName(entry.name); ++suffix)
                entry.name = base + "_" + std::to_string(suffix);
        entry.flashArray = std::move(snapshot);
        entry.type       = DataType::ARRAY;
        entry.port       = PortType::LOCAL;
        entry.addr       = entry.flashArray->address;
        entry.isStruct   = true;
        entry.isArray    = true;
        entry.writable   = false;
        vars_.push_back(std::move(entry));
        isModified_ = true;
}
void
Variable::restoreFlashArray(VarEntry &entry, const cJSON *node)
{
        entry.flashArray = flasharray::load(node);
        if (!entry.flashArray)
                return;
        entry.type     = DataType::ARRAY;
        entry.port     = PortType::LOCAL;
        entry.addr     = entry.flashArray->address;
        entry.isStruct = true;
        entry.isArray  = true;
        entry.isUnion  = false;
        entry.writable = false;
        entry.typeOff  = 0;
        entry.structFields.clear();
        entry.readFunction  = {};
        entry.writeFunction = {};
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(node, "flashArrayExpanded")))
                entry.expandedMembers.insert(entry.name);
}
bool
Variable::drawFlashArrayRow(int index, int &moveSrc, int &moveDst)
{
        auto       &entry  = vars_[index];
        const auto &a      = *entry.flashArray;
        const auto  width  = flasharray::width(a.type);
        const auto *type   = flasharray::names[static_cast<int>(a.type)];
        bool        remove = false;
        ImGui::PushID(index);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const bool wasOpen = entry.expandedMembers.contains(entry.name);
        ImGui::SetNextItemOpen(wasOpen, ImGuiCond_Always);
        const bool open = ImGui::TreeNodeEx("flashArrayRoot",
                                            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                ImGuiTreeNodeFlags_SpanFullWidth,
                                            "%s [%zu]",
                                            entry.name.c_str(),
                                            a.values.size());
        if (open != wasOpen) {
                if (open)
                        entry.expandedMembers.insert(entry.name);
                else
                        entry.expandedMembers.erase(entry.name);
                isModified_ = true;
        }
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                for (auto &v : vars_)
                        v.selected = false;
                prevSelectedIndex_ = -1;
        }
        if (ImGui::BeginDragDropSource()) {
                rowDragSrc_ = index;
                auto &drag  = flasharray::drag();
                if (drag.snapshot != entry.flashArray) {
                        ++drag.id;
                        drag.snapshot = entry.flashArray;
                }
                ImGui::SetDragDropPayload("FLASH_ARRAY", &drag.id, sizeof(drag.id));
                ImGui::Text("%s [%zu]", entry.name.c_str(), a.values.size());
                ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
                const auto *p = ImGui::GetDragDropPayload();
                if (rowDragSrc_ >= 0 && p &&
                    (p->IsDataType("CHANNEL") || p->IsDataType("STRUCT_CHANNEL") || p->IsDataType("FLASH_ARRAY")))
                        if (ImGui::AcceptDragDropPayload(p->DataType)) {
                                moveSrc = rowDragSrc_;
                                moveDst = index;
                        }
                ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem()) {
                drawWatchValueActions(index, entry.name);
                if (ImGui::MenuItem(tr("Modify Variable...", "修改变量...")))
                        openRewrite(entry);
                if (ImGui::MenuItem(tr("Delete array", "删除数组")))
                        remove = true;
                ImGui::EndPopup();
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%zu %s", a.values.size(), tr("elements", "个元素"));
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%s[]", type);
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("0x%08X", a.address);
        ImGui::TableSetColumnIndex(4);
        ImGui::TextUnformatted("FLASH");
        ImGui::TableSetColumnIndex(5);
        ImGui::TextUnformatted("R");
        if (open) {
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(a.values.size()));
                while (clipper.Step())
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::Text("[%d]", i);
                                ImGui::PushID(i);
                                if (ImGui::BeginPopupContextItem("snapshot_element")) {
                                        drawWatchValueActions(index, entry.name + "[" + std::to_string(i) + "]");
                                        if (ImGui::MenuItem(tr("Modify Variable...", "修改变量...")))
                                                openRewriteMember(index,
                                                                  entry.addr + size_t(i) * width,
                                                                  0,
                                                                  entry.name + "[" + std::to_string(i) + "]");
                                        ImGui::EndPopup();
                                }
                                ImGui::PopID();
                                ImGui::TableSetColumnIndex(1);
                                const auto *bytes = a.bytes.data() + size_t(i) * width;
                                const int   radix = entry.radixForPath(entry.name + "[" + std::to_string(i) + "]");
                                const auto  value = radix == 10 ? flasharray::format(bytes, a.type, a.bigEndian)
                                                                : formatIntegerValue(flasharray::bits(bytes, width, a.bigEndian),
                                                                                    false,
                                                                                    static_cast<u32>(width * 8),
                                                                                    radix);
                                ImGui::TextUnformatted(value.c_str());
                                ImGui::TableSetColumnIndex(2);
                                ImGui::TextUnformatted(type);
                                ImGui::TableSetColumnIndex(3);
                                ImGui::Text("0x%08X", a.address + uint32_t(size_t(i) * width));
                                ImGui::TableSetColumnIndex(4);
                                ImGui::TextUnformatted("FLASH");
                                ImGui::TableSetColumnIndex(5);
                                ImGui::TextUnformatted("R");
                        }
                ImGui::TreePop();
        }
        ImGui::PopID();
        return remove;
}
