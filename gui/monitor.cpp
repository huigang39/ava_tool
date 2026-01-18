#include <charconv>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "implot.h"

#include "monitor.hpp"

/* -------------------------------------------------------------------------- */
/*                                MonitorScope                                */
/* -------------------------------------------------------------------------- */

void
MonitorScope::menu()
{
        switch (e_draw) {
                case DrawEnum::TABLE: {
                        tableMenu();
                        break;
                }
                case DrawEnum::PLOT: {
                        plotMenu();
                        break;
                }
                default:
                        break;
        }
}

void
MonitorScope::draw()
{
        switch (e_draw) {
                case DrawEnum::TABLE: {
                        tableDraw();
                        break;
                }
                case DrawEnum::PLOT: {
                        plotDraw();
                        break;
                }
                default:
                        break;
        }

        if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CHANNEL")) {
                        const auto droppedChName = static_cast<const char *>(payload->Data);
                        addChannel(droppedChName);
                }
                ImGui::EndDragDropTarget();
        }
}

void
MonitorScope::tableMenu()
{
        if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("switch to PLOT"))
                        e_draw = DrawEnum::PLOT;

                if (ImGui::MenuItem("add channel")) {
                        char nameBuf[32];
                        snprintf(nameBuf, sizeof(nameBuf), "Ch %zu", chs_.size());
                        addChannel(nameBuf);
                }
                ImGui::EndPopup();
        }
}

void
MonitorScope::tableDraw()
{
        if (ImGui::BeginTable((name_ + "##table").c_str(), 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("RValue");
                ImGui::TableSetupColumn("WValue");
                ImGui::TableSetupColumn("Addr");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Device");
                ImGui::TableHeadersRow();

                for (auto &[chName, ch] : chs_)
                        drawTableRow(chName, ch);

                ImGui::EndTable();
        }
}

void
MonitorScope::plotMenu()
{
        if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("switch to TABLE"))
                        e_draw = DrawEnum::TABLE;

                if (ImGui::MenuItem("add channel")) {
                        char nameBuf[32];
                        snprintf(nameBuf, sizeof(nameBuf), "ch_%zu", chs_.size());
                        addChannel(nameBuf);
                }
                ImGui::EndPopup();
        }
}

void
MonitorScope::plotDraw() const
{
        if (ImPlot::BeginPlot((name_ + "##plot").c_str(), ImVec2(-1, -1)))
                ImPlot::EndPlot();
}

int
MonitorScope::addChannel(const std::string &chName)
{
        auto [it, inserted] = chs_.emplace(chName, std::make_unique<MonitorChannel>(chName));
        return inserted ? 0 : -1;
}

int
MonitorScope::setValue(const std::string &chName, f32 val)
{
        if (const auto it = chs_.find(chName); it != chs_.end()) {
                it->second->setRVal(val);
                return 0;
        }
        return -1;
}

MonitorChannel *
MonitorScope::findChannel(const std::string &chName)
{
        if (const auto it = chs_.find(chName); it != chs_.end())
                return it->second.get();

        return nullptr;
}

void
MonitorScope::shmInit(MonitorChannel &ch)
{
        const shm_cfg_t shm_cfg{
            .name   = ch.getName().c_str(),
            .access = SHM_READWRITE,
            .cap    = 1024,
        };

        if (const int ret = shm_init(&ch.getShm(), shm_cfg); ret != 0)
                printf("shm init err\n");

        ch.setAddr(*reinterpret_cast<usize *>(&ch.getShm().lo.base));
        printf("shm_name: %s, addr: 0x%llu, is_creator: %u\n", ch.getName().c_str(), ch.getAddr(), ch.getShm().lo.is_creator);
}

void
MonitorScope::drawTableRow(const std::string &chName, std::unique_ptr<MonitorChannel> &ch)
{
        ImGui::TableNextRow();

        // 绘制 Name 列
        ImGui::TableSetColumnIndex(0);
        char nameBuf[64];
        snprintf(nameBuf, sizeof(nameBuf), "%s", ch->getName().c_str());
        if (ImGui::InputText(
                ("##name" + name_ + chName).c_str(), nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                const std::string oldChName = ch->getName();
                ch->setName(nameBuf);
                if (ch->getDevice() == "LOCAL")
                        shmInit(*ch);

                if (auto nh = chs_.extract(oldChName); !nh.empty()) {
                        nh.key() = nameBuf;
                        chs_.insert(std::move(nh));
                }
        }
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload("CHANNEL", chName.c_str(), chName.size() + 1);
                ImGui::Text("Dragging %s", chName.c_str());
                ImGui::EndDragDropSource();
        }

        // 绘制 RValue 列
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%f", ch->getRVal());

        // 绘制 WValue 列
        ImGui::TableSetColumnIndex(2);
        ImGui::InputFloat(("##val" + name_ + chName).c_str(), &ch->getWVal());

        // 绘制 Addr 列
        ImGui::TableSetColumnIndex(3);
        const u64 addr = ch->getAddr();
        char      buf[32];
        snprintf(buf, sizeof(buf), "0x%016llX", (u64)addr);
        if (ImGui::InputText(("##addr" + name_ + chName).c_str(),
                             buf,
                             sizeof(buf),
                             ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase)) {
                u64 newAddr = 0;
                std::from_chars(buf, buf + strlen(buf), newAddr, 16);
                ch->setAddr(newAddr);
        }

        // 绘制 Type 列
        ImGui::TableSetColumnIndex(4);
        if (ImGui::BeginCombo(("##type" + name_ + chName).c_str(), ch->getType().c_str())) {
                for (const char *types[] = {"F32", "U32", "I32"}; auto &type : types) {
                        const bool is_selected = (ch->getType() == type);
                        if (ImGui::Selectable(type, is_selected))
                                ch->getType() = type;
                        if (is_selected)
                                ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
        }

        // 绘制 Device 列
        ImGui::TableSetColumnIndex(5);
        if (ImGui::BeginCombo(("##device" + name_ + chName).c_str(), ch->getDevice().c_str())) {
                for (const char *devices[] = {"LOCAL", "FSA"}; auto &device : devices) {
                        const bool is_selected = (ch->getDevice() == device);
                        if (ImGui::Selectable(device, is_selected))
                                ch->setDevice(device);
                        if (is_selected)
                                ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
        }
}

/* -------------------------------------------------------------------------- */
/*                                   Monitor                                  */
/* -------------------------------------------------------------------------- */

int
Monitor::addScope(const std::string &scopeName)
{
        auto [it, inserted] = scopes_.emplace(scopeName, std::make_unique<MonitorScope>(scopeName));
        return inserted ? 0 : -1;
}

MonitorChannel *
Monitor::findChannel(const std::string &scopeName, const std::string &chName)
{
        if (const auto it = scopes_.find(scopeName); it != scopes_.end())
                return it->second->findChannel(chName);

        return nullptr;
}

void
Monitor::menu()
{
        if (ImGui::BeginPopupContextWindow()) {
                if (ImGui::MenuItem("add scope")) {
                        char nameBuf[32];
                        snprintf(nameBuf, sizeof(nameBuf), "scope_%zu", scopes_.size());
                        addScope(nameBuf);
                }
                ImGui::EndPopup();
        }
}

void
Monitor::updateDisplay()
{
        if (ImGui::Begin(name_.c_str())) {
                menu();

                if (const int numScopes = static_cast<int>(scopes_.size()); numScopes > 0) {
                        const ImVec2 avail       = ImGui::GetContentRegionAvail();
                        const float  childHeight = avail.y / static_cast<f32>(numScopes);

                        for (auto &[scopeName, scope] : scopes_) {
                                if (ImGui::BeginChild(scopeName.c_str(), ImVec2(avail.x, childHeight), true)) {
                                        if (ImGui::CollapsingHeader(scopeName.c_str())) {
                                                scope->menu();
                                                scope->draw();
                                        }
                                        ImGui::EndChild();
                                }
                        }
                }
                ImGui::End();
        }
}
