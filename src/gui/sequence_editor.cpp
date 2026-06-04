#include "sequence_editor.hpp"
#include "ImGuiNotify.hpp"
#include "cJSON.h"
#include "gui/i18n.hpp"
#include "imgui.h"
#include "monitor.hpp"
#include "platform/native_dlg.hpp"
#include "timeops.h"
#include <fstream>
#include <sstream>

SequenceEditor::SequenceEditor()
{
        refreshSeqFiles();
}

void
SequenceEditor::refreshSeqFiles()
{
        seqFiles_.clear();
        try {
                std::filesystem::path dirPath(reinterpret_cast<const char8_t *>(seqFolder_.c_str()));
                if (std::filesystem::exists(dirPath) && std::filesystem::is_directory(dirPath)) {
                        for (const auto &entry : std::filesystem::directory_iterator(dirPath)) {
                                if (entry.is_regular_file() && entry.path().extension() == ".seq") {
                                        std::u8string u8str = entry.path().filename().u8string();
                                        seqFiles_.push_back(std::string(u8str.begin(), u8str.end()));
                                }
                        }
                }
        } catch (...) {
        }
        selectedSeqIdx_ = -1;
}

SequenceEditor::~SequenceEditor() {}

void
SequenceEditor::writeAction(const SequenceAction &action)
{
        u8  buf[8] = {0};
        u32 sz     = Parser::typeBytes(action.type);
        if (sz == 0 || sz > 8)
                return;

        // Note: For bitfields, we really should read-modify-write.
        // For simplicity and matching variable.cpp's current behavior, we do a raw write if it's not a bitfield.
        // If it is a bitfield, we WILL implement read-modify-write here because it's safer.
        bool needRmw = (action.bitSize > 0);

        if (needRmw) {
                bool readOk = false;
                if (action.port == "JLINK" && JLinkPort::instance().isConnected()) {
                        readOk = JLinkPort::instance().readMem((u32)action.addr, sz, buf);
                }
                if (!readOk) {
                        LOG_E("SequenceEditor: failed to read memory for RMW bitfield %s", action.name.c_str());
                        return;
                }
        }

        // Parse targetValue to primitive type
        i64 valI = 0;
        f64 valF = 0.0;
        try {
                if (action.isEnum) {
                        bool found = false;
                        for (const auto &[k, v] : action.enumDefs) {
                                if (v == action.targetValue) {
                                        valI  = k;
                                        found = true;
                                        break;
                                }
                        }
                        if (!found)
                                valI = std::stoll(action.targetValue);
                } else {
                        if (action.type == DataType::F32 || action.type == DataType::F64)
                                valF = std::stod(action.targetValue);
                        else
                                valI = std::stoll(action.targetValue);
                }
        } catch (...) {
                LOG_E("SequenceEditor: invalid target value '%s' for %s", action.targetValue.c_str(), action.name.c_str());
                return; // Invalid format
        }

        if (needRmw) {
                // Read-modify-write
                u64 mask       = ((1ULL << action.bitSize) - 1) << action.bitOffset;
                u64 currentVal = 0;
                std::memcpy(&currentVal, buf, sz);
                currentVal &= ~mask;
                currentVal |= ((valI << action.bitOffset) & mask);
                std::memcpy(buf, &currentVal, sz);
        } else {
                if (action.type == DataType::F32)
                        *(f32 *)buf = (f32)valF;
                else if (action.type == DataType::F64)
                        *(f64 *)buf = valF;
                else if (action.type == DataType::U32)
                        *(u32 *)buf = (u32)valI;
                else if (action.type == DataType::I32)
                        *(i32 *)buf = (i32)valI;
                else if (action.type == DataType::U16)
                        *(u16 *)buf = (u16)valI;
                else if (action.type == DataType::I16)
                        *(i16 *)buf = (i16)valI;
                else if (action.type == DataType::U8)
                        *(u8 *)buf = (u8)valI;
                else if (action.type == DataType::I8)
                        *(i8 *)buf = (i8)valI;
                else if (action.type == DataType::U64)
                        *(u64 *)buf = (u64)valI;
                else if (action.type == DataType::I64)
                        *(i64 *)buf = valI;
        }

        if (action.port == "JLINK" && JLinkPort::instance().isConnected()) {
                JLinkPort::instance().writeMem((u32)action.addr, sz, buf);
        } else if (action.port == "SHM") {
                // For simplicity, we just use the global/singleton shm_t if possible,
                // but SHM is usually handled per variable. We will open it ad-hoc here.
                shm_t     shm_handle;
                shm_cfg_t cfg = {action.shmName.c_str(), SHM_READWRITE, 4096};
                if (shm_init(&shm_handle, cfg) == 0) {
                        shm_write(&shm_handle, buf, sz);
                        // we shouldn't close it immediately as it unmaps, wait shm_init handles mapped memory.
                        // Actually in this app shm_t isn't closed, it leaks a mapping if not managed.
                        // But writing is fine.
                }
        }
}

void
SequenceEditor::draw()
{
        if (!show_) {
                if (state_ == State::RUNNING) {
                        state_ = State::IDLE; // Stop if closed
                }
                return;
        }

        ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(tr("Sequence Editor###SeqEditor", "序列编辑器###SeqEditor"), &show_)) {
                if (state_ == State::RUNNING)
                        state_ = State::IDLE;
                ImGui::End();
                return;
        }

        // State Machine Evaluation
        if (state_ == State::RUNNING) {
                if (steps_.empty()) {
                        state_ = State::IDLE;
                } else {
                        u64 now = get_mono_ts_ms();
                        if (currentStepIdx_ < (int)steps_.size()) {
                                if (now - stepStartTime_ >= steps_[currentStepIdx_].delayMs) {
                                        // Execute step
                                        for (const auto &action : steps_[currentStepIdx_].actions) {
                                                writeAction(action);
                                        }
                                        currentStepIdx_++;
                                        stepStartTime_ = now;
                                }
                        } else {
                                state_ = State::IDLE;
                                ImGui::InsertNotification(
                                    {ImGuiToastType::Success, 3000, tr("Sequence execution completed!", "序列执行完成！")});
                        }
                }
        }

        // Toolbar
        if (state_ == State::IDLE) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.8f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
                if (ImGui::Button(tr("Play", "播放"), ImVec2(80, 0))) {
                        if (!steps_.empty()) {
                                state_          = State::RUNNING;
                                currentStepIdx_ = 0;
                                stepStartTime_  = get_mono_ts_ms();
                        }
                }
                ImGui::PopStyleColor(3);
        } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
                if (ImGui::Button(tr("Stop", "停止"), ImVec2(80, 0))) {
                        state_ = State::IDLE;
                }
                ImGui::PopStyleColor(3);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Import", "导入"))) {
                std::string path = nativeDlgOpen("Import Sequence", {{"Sequence Files", {"seq"}}}, "");
                if (!path.empty()) {
                        importFromFile(path);
                }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Export", "导出"))) {
                std::string path = nativeDlgSave("Export Sequence", {{"Sequence Files", {"seq"}}}, "sequence.seq");
                if (!path.empty()) {
                        if (path.find(".seq") == std::string::npos)
                                path += ".seq";
                        exportToFile(path);
                }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Folder", "文件夹"))) {
                std::string path = nativeDlgPickDir("Choose Sequence Folder");
                if (!path.empty()) {
                        seqFolder_ = path;
                        refreshSeqFiles();
                }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        if (ImGui::BeginCombo("##SeqFiles",
                              selectedSeqIdx_ >= 0 && selectedSeqIdx_ < (int)seqFiles_.size()
                                  ? seqFiles_[selectedSeqIdx_].c_str()
                                  : tr("Select seq...", "选择序列..."))) {
                for (int i = 0; i < (int)seqFiles_.size(); i++) {
                        bool isSelected = (selectedSeqIdx_ == i);
                        if (ImGui::Selectable(seqFiles_[i].c_str(), isSelected)) {
                                selectedSeqIdx_      = i;
                                std::string fullPath = seqFolder_ + "/" + seqFiles_[i];
                                importFromFile(fullPath);
                        }
                        if (isSelected) {
                                ImGui::SetItemDefaultFocus();
                        }
                }
                ImGui::EndCombo();
        }

        ImGui::Separator();

        if (ImGui::Button(tr("Add Step", "添加步骤"))) {
                steps_.push_back({"", 1000, {}}); // Default 1000ms delay, empty name
                selectedStep_ = (int)steps_.size() - 1;
                isModified_   = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Clear All", "全部清空"))) {
                steps_.clear();
                selectedStep_ = -1;
                isModified_   = true;
        }

        ImGui::Separator();

        static ImGuiTableFlags splitFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
        if (ImGui::BeginTable("SplitView", 2, splitFlags)) {
                ImGui::TableSetupColumn("Left", ImGuiTableColumnFlags_WidthFixed, 200.0f); // layout-only, not shown
                ImGui::TableSetupColumn("Right", ImGuiTableColumnFlags_WidthStretch);      // layout-only, not shown
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                if (ImGui::BeginChild("StepsList", ImVec2(0, 0), true)) {
                        for (int i = 0; i < (int)steps_.size(); ++i) {
                                char label[64];
                                if (!steps_[i].name.empty()) {
                                        snprintf(label, sizeof(label), "%d: %s", i + 1, steps_[i].name.c_str());
                                } else {
                                        snprintf(label, sizeof(label), tr("Step %d", "步骤 %d"), i + 1);
                                }

                                bool isExecuting = (state_ == State::RUNNING && currentStepIdx_ == i);
                                if (isExecuting) {
                                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));
                                }

                                if (ImGui::Selectable(label, selectedStep_ == i)) {
                                        selectedStep_ = i;
                                }

                                if (isExecuting) {
                                        ImGui::PopStyleColor();
                                }

                                // Drag and drop target to add variable to this step
                                if (ImGui::BeginDragDropTarget()) {
                                        const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("SYMBOL_CHANNEL");
                                        if (!payload)
                                                payload = ImGui::AcceptDragDropPayload("CHANNEL");

                                        if (payload) {
                                                auto          *p = static_cast<ChannelDropPayload *>(payload->Data);
                                                SequenceAction action;
                                                action.name      = p->name;
                                                action.addr      = p->addr;
                                                action.type      = Parser::strToDataType(p->type);
                                                action.port      = p->device;
                                                action.shmName   = p->shmName;
                                                action.typeOff   = p->typeOff;
                                                action.bitOffset = p->bitOffset;
                                                action.bitSize   = p->bitSize;
                                                action.isEnum    = (p->numEnums > 0);
                                                for (int e = 0; e < p->numEnums; ++e) {
                                                        action.enumDefs.push_back({p->enums[e].value, p->enums[e].name});
                                                }
                                                action.targetValue = action.isEnum ? p->enums[0].name : "0"; // default

                                                steps_[i].actions.push_back(action);
                                                selectedStep_ = i;
                                        } else if (const ImGuiPayload *spayload =
                                                       ImGui::AcceptDragDropPayload("STRUCT_CHANNEL")) {
                                                auto *sp = static_cast<StructChannelPayload *>(spayload->Data);
                                                for (int j = 0; j < sp->count; ++j) {
                                                        SequenceAction action;
                                                        action.name      = sp->entries[j].name;
                                                        action.addr      = sp->entries[j].addr;
                                                        action.type      = Parser::strToDataType(sp->entries[j].type);
                                                        action.port      = sp->device;
                                                        action.shmName   = sp->shmName;
                                                        action.typeOff   = 0;
                                                        action.bitOffset = sp->entries[j].bitOffset;
                                                        action.bitSize   = sp->entries[j].bitSize;
                                                        action.isEnum    = (sp->entries[j].numEnums > 0);
                                                        for (int e = 0; e < sp->entries[j].numEnums; ++e) {
                                                                action.enumDefs.push_back({sp->entries[j].enums[e].value,
                                                                                           sp->entries[j].enums[e].name});
                                                        }
                                                        action.targetValue = action.isEnum ? sp->entries[j].enums[0].name : "0";
                                                        steps_[i].actions.push_back(action);
                                                }
                                                selectedStep_ = i;
                                        } else if (const ImGuiPayload *vpayload =
                                                       ImGui::AcceptDragDropPayload("DND_CHANNEL_MOVE")) {
                                                auto           *m_p = static_cast<ChannelMovePayload *>(vpayload->Data);
                                                MonitorChannel *ch  = m_p->srcScope->findChannel(m_p->chName);
                                                if (ch) {
                                                        SequenceAction action;
                                                        action.name      = ch->getName();
                                                        action.addr      = ch->getAddr();
                                                        action.type      = Parser::strToDataType(ch->getType());
                                                        action.port      = ch->getDevice();
                                                        action.shmName   = ch->getShmRegionName();
                                                        action.typeOff   = 0;
                                                        action.bitOffset = ch->getBitOffset();
                                                        action.bitSize   = ch->getBitSize();
                                                        action.isEnum    = !ch->getEnums().empty();
                                                        for (const auto &e : ch->getEnums()) {
                                                                action.enumDefs.push_back({e.value, e.name});
                                                        }
                                                        action.targetValue = action.isEnum ? ch->getEnums()[0].name : "0";
                                                        steps_[i].actions.push_back(action);
                                                        selectedStep_ = i;
                                                }
                                        }
                                        ImGui::EndDragDropTarget();
                                }
                        }
                }
                ImGui::EndChild();

                ImGui::TableSetColumnIndex(1);
                if (ImGui::BeginChild("StepDetails", ImVec2(0, 0), true)) {
                        if (selectedStep_ >= 0 && selectedStep_ < (int)steps_.size()) {
                                SequenceStep &step = steps_[selectedStep_];

                                char nameBuf[128];
                                strncpy(nameBuf, step.name.c_str(), sizeof(nameBuf));
                                ImGui::PushItemWidth(200);
                                if (ImGui::InputTextWithHint("##Name", tr("Step Name", "步骤名称"), nameBuf, sizeof(nameBuf))) {
                                        step.name   = nameBuf;
                                        isModified_ = true;
                                }
                                if (ImGui::IsItemHovered()) {
                                        ImGui::SetTooltip("%s", tr("Step Name", "步骤名称"));
                                }
                                ImGui::PopItemWidth();

                                ImGui::SameLine();
                                if (ImGui::Button(tr("Remove Step", "删除步骤"))) {
                                        steps_.erase(steps_.begin() + selectedStep_);
                                        isModified_ = true;
                                        if (selectedStep_ >= (int)steps_.size()) {
                                                selectedStep_ = (int)steps_.size() - 1;
                                        }
                                }

                                ImGui::Text(tr("Step %d Configuration", "步骤 %d 配置"), selectedStep_ + 1);
                                ImGui::Separator();

                                int delay = step.delayMs;
                                if (ImGui::InputInt(tr("Delay (ms)", "延时 (毫秒)"), &delay, 10, 100)) {
                                        if (delay < 0)
                                                delay = 0;
                                        step.delayMs = delay;
                                        isModified_  = true;
                                }

                                ImGui::Spacing();
                                ImGui::Text("%s", tr("Actions (Drag variables here):", "动作（将变量拖到此处）："));

                                // Add drag target to the action list box
                                if (ImGui::BeginChild("ActionList", ImVec2(0, 0), true)) {
                                        if (ImGui::BeginTable("ActionsTable",
                                                              3,
                                                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                                  ImGuiTableFlags_Resizable)) {
                                                ImGui::TableSetupColumn(tr("Variable###col_seqvar", "变量###col_seqvar"),
                                                                        ImGuiTableColumnFlags_WidthStretch,
                                                                        0.4f);
                                                ImGui::TableSetupColumn(tr("Value###col_seqval", "数值###col_seqval"),
                                                                        ImGuiTableColumnFlags_WidthStretch,
                                                                        0.4f);
                                                ImGui::TableSetupColumn(tr("Action###col_seqact", "操作###col_seqact"),
                                                                        ImGuiTableColumnFlags_WidthFixed,
                                                                        60.0f);
                                                ImGui::TableHeadersRow();

                                                for (int i = 0; i < (int)step.actions.size(); ++i) {
                                                        ImGui::PushID(i);
                                                        ImGui::TableNextRow();

                                                        ImGui::TableNextColumn();
                                                        ImGui::AlignTextToFramePadding();
                                                        ImGui::TextUnformatted(step.actions[i].name.c_str());

                                                        ImGui::TableNextColumn();
                                                        ImGui::SetNextItemWidth(-FLT_MIN);
                                                        if (step.actions[i].isEnum && !step.actions[i].enumDefs.empty()) {
                                                                std::string preview = step.actions[i].targetValue;
                                                                // find the value for the preview to display value
                                                                for (const auto &[val, name] : step.actions[i].enumDefs) {
                                                                        if (name == preview) {
                                                                                preview += " (" + std::to_string(val) + ")";
                                                                                break;
                                                                        }
                                                                }
                                                                if (ImGui::BeginCombo("##val", preview.c_str())) {
                                                                        for (const auto &[val, name] :
                                                                             step.actions[i].enumDefs) {
                                                                                bool is_selected =
                                                                                    (step.actions[i].targetValue == name);
                                                                                std::string label =
                                                                                    name + " (" + std::to_string(val) + ")";
                                                                                if (ImGui::Selectable(label.c_str(),
                                                                                                      is_selected)) {
                                                                                        step.actions[i].targetValue = name;
                                                                                        isModified_                 = true;
                                                                                }
                                                                                if (is_selected) {
                                                                                        ImGui::SetItemDefaultFocus();
                                                                                }
                                                                        }
                                                                        ImGui::EndCombo();
                                                                }
                                                        } else {
                                                                char valBuf[64];
                                                                strncpy(valBuf,
                                                                        step.actions[i].targetValue.c_str(),
                                                                        sizeof(valBuf));

                                                                if (ImGui::InputText("##val", valBuf, sizeof(valBuf))) {
                                                                        step.actions[i].targetValue = valBuf;
                                                                        isModified_                 = true;
                                                                }
                                                        }

                                                        ImGui::TableNextColumn();
                                                        if (ImGui::Button(tr("Remove", "移除"), ImVec2(-FLT_MIN, 0))) {
                                                                step.actions.erase(step.actions.begin() + i);
                                                                isModified_ = true;
                                                                i--;
                                                        }
                                                        ImGui::PopID();
                                                }
                                                ImGui::EndTable();
                                        }
                                }
                                ImGui::EndChild();
                        } else {
                                ImGui::Text("%s", tr("No step selected.", "未选择步骤。"));
                        }
                }
                ImGui::EndChild();

                // Make the entire StepDetails pane a drop target
                if (ImGui::BeginDragDropTarget()) {
                        if (selectedStep_ >= 0 && selectedStep_ < (int)steps_.size()) {
                                if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CHANNEL")) {
                                        auto          *p = static_cast<ChannelDropPayload *>(payload->Data);
                                        SequenceAction action;
                                        action.name      = p->name;
                                        action.addr      = p->addr;
                                        action.type      = Parser::strToDataType(p->type);
                                        action.port      = p->device;
                                        action.shmName   = p->shmName;
                                        action.typeOff   = p->typeOff;
                                        action.bitOffset = p->bitOffset;
                                        action.bitSize   = p->bitSize;
                                        action.isEnum    = (p->numEnums > 0);
                                        for (int e = 0; e < p->numEnums; ++e) {
                                                action.enumDefs.push_back({p->enums[e].value, p->enums[e].name});
                                        }
                                        action.targetValue = action.isEnum ? p->enums[0].name : "0";
                                        steps_[selectedStep_].actions.push_back(action);
                                        isModified_ = true;
                                } else if (const ImGuiPayload *spayload = ImGui::AcceptDragDropPayload("STRUCT_CHANNEL")) {
                                        auto *sp = static_cast<StructChannelPayload *>(spayload->Data);
                                        for (int i = 0; i < sp->count; ++i) {
                                                SequenceAction action;
                                                action.name      = sp->entries[i].name;
                                                action.addr      = sp->entries[i].addr;
                                                action.type      = Parser::strToDataType(sp->entries[i].type);
                                                action.port      = sp->device;
                                                action.shmName   = sp->shmName;
                                                action.typeOff   = 0; // Not fully mapped for structs, but ok
                                                action.bitOffset = sp->entries[i].bitOffset;
                                                action.bitSize   = sp->entries[i].bitSize;
                                                action.isEnum    = (sp->entries[i].numEnums > 0);
                                                for (int e = 0; e < sp->entries[i].numEnums; ++e) {
                                                        action.enumDefs.push_back(
                                                            {sp->entries[i].enums[e].value, sp->entries[i].enums[e].name});
                                                }
                                                action.targetValue = action.isEnum ? sp->entries[i].enums[0].name : "0";
                                                steps_[selectedStep_].actions.push_back(action);
                                                isModified_ = true;
                                        }
                                } else if (const ImGuiPayload *mpayload = ImGui::AcceptDragDropPayload("DND_CHANNEL_MOVE")) {
                                        auto           *m_p = static_cast<ChannelMovePayload *>(mpayload->Data);
                                        MonitorChannel *ch  = m_p->srcScope->findChannel(m_p->chName);
                                        if (ch) {
                                                SequenceAction action;
                                                action.name      = ch->getName();
                                                action.addr      = ch->getAddr();
                                                action.type      = Parser::strToDataType(ch->getType());
                                                action.port      = ch->getDevice();
                                                action.shmName   = ch->getShmRegionName();
                                                action.typeOff   = 0;
                                                action.bitOffset = ch->getBitOffset();
                                                action.bitSize   = ch->getBitSize();
                                                action.isEnum    = !ch->getEnums().empty();
                                                for (const auto &e : ch->getEnums()) {
                                                        action.enumDefs.push_back({e.value, e.name});
                                                }
                                                action.targetValue = action.isEnum ? ch->getEnums()[0].name : "0";
                                                steps_[selectedStep_].actions.push_back(action);
                                                isModified_ = true;
                                        }
                                }
                        }
                        ImGui::EndDragDropTarget();
                }

                ImGui::EndTable();
        }

        ImGui::End();
}

void
SequenceEditor::saveSession(cJSON *root)
{
        cJSON *seqArr = cJSON_CreateArray();
        for (const auto &step : steps_) {
                cJSON *stepObj = cJSON_CreateObject();
                cJSON_AddStringToObject(stepObj, "name", step.name.c_str());
                cJSON_AddNumberToObject(stepObj, "delayMs", step.delayMs);
                cJSON *actionsArr = cJSON_CreateArray();
                for (const auto &action : step.actions) {
                        cJSON *actionObj = cJSON_CreateObject();
                        cJSON_AddStringToObject(actionObj, "name", action.name.c_str());
                        cJSON_AddStringToObject(actionObj, "targetValue", action.targetValue.c_str());
                        cJSON_AddNumberToObject(actionObj, "addr", static_cast<double>(action.addr));
                        cJSON_AddNumberToObject(actionObj, "type", (int)action.type);
                        cJSON_AddStringToObject(actionObj, "port", action.port.c_str());
                        cJSON_AddStringToObject(actionObj, "shmName", action.shmName.c_str());
                        cJSON_AddNumberToObject(actionObj, "typeOff", static_cast<double>(action.typeOff));
                        cJSON_AddNumberToObject(actionObj, "bitOffset", action.bitOffset);
                        cJSON_AddNumberToObject(actionObj, "bitSize", action.bitSize);
                        cJSON_AddBoolToObject(actionObj, "isEnum", action.isEnum);
                        if (action.isEnum) {
                                cJSON *enumDefsObj = cJSON_CreateObject();
                                for (const auto &[val, name] : action.enumDefs) {
                                        cJSON_AddStringToObject(enumDefsObj, std::to_string(val).c_str(), name.c_str());
                                }
                                cJSON_AddItemToObject(actionObj, "enumDefs", enumDefsObj);
                        }
                        cJSON_AddItemToArray(actionsArr, actionObj);
                }
                cJSON_AddItemToObject(stepObj, "actions", actionsArr);
                cJSON_AddItemToArray(seqArr, stepObj);
        }
        cJSON_AddItemToObject(root, "sequenceEditor", seqArr);
        cJSON_AddStringToObject(root, "sequenceEditorFolder", seqFolder_.c_str());
}

void
SequenceEditor::loadSession(cJSON *root)
{
        steps_.clear();
        selectedStep_ = -1;
        isModified_   = false;

        if (const cJSON *f = cJSON_GetObjectItem(root, "sequenceEditorFolder"); cJSON_IsString(f)) {
                seqFolder_ = f->valuestring;
                refreshSeqFiles();
        }

        const cJSON *seqArr = cJSON_GetObjectItem(root, "sequenceEditor");
        if (!cJSON_IsArray(seqArr))
                return;

        for (const cJSON *stepObj = seqArr->child; stepObj; stepObj = stepObj->next) {
                SequenceStep step;
                if (const cJSON *n = cJSON_GetObjectItem(stepObj, "name"); cJSON_IsString(n)) {
                        step.name = n->valuestring;
                }
                if (const cJSON *d = cJSON_GetObjectItem(stepObj, "delayMs"); cJSON_IsNumber(d)) {
                        step.delayMs = d->valueint;
                }
                const cJSON *actionsArr = cJSON_GetObjectItem(stepObj, "actions");
                if (cJSON_IsArray(actionsArr)) {
                        for (const cJSON *actionObj = actionsArr->child; actionObj; actionObj = actionObj->next) {
                                SequenceAction action;
                                if (const cJSON *n = cJSON_GetObjectItem(actionObj, "name"); cJSON_IsString(n))
                                        action.name = n->valuestring;
                                if (const cJSON *tv = cJSON_GetObjectItem(actionObj, "targetValue"); cJSON_IsString(tv))
                                        action.targetValue = tv->valuestring;
                                if (const cJSON *a = cJSON_GetObjectItem(actionObj, "addr"); cJSON_IsNumber(a))
                                        action.addr = static_cast<u64>(a->valuedouble);
                                if (const cJSON *t = cJSON_GetObjectItem(actionObj, "type"); cJSON_IsNumber(t))
                                        action.type = (DataType)t->valueint;
                                if (const cJSON *p = cJSON_GetObjectItem(actionObj, "port"); cJSON_IsString(p))
                                        action.port = p->valuestring;
                                if (const cJSON *shm = cJSON_GetObjectItem(actionObj, "shmName"); cJSON_IsString(shm))
                                        action.shmName = shm->valuestring;
                                if (const cJSON *to = cJSON_GetObjectItem(actionObj, "typeOff"); cJSON_IsNumber(to))
                                        action.typeOff = static_cast<u64>(to->valuedouble);
                                if (const cJSON *bo = cJSON_GetObjectItem(actionObj, "bitOffset"); cJSON_IsNumber(bo))
                                        action.bitOffset = bo->valueint;
                                if (const cJSON *bs = cJSON_GetObjectItem(actionObj, "bitSize"); cJSON_IsNumber(bs))
                                        action.bitSize = bs->valueint;
                                if (const cJSON *ie = cJSON_GetObjectItem(actionObj, "isEnum"); cJSON_IsBool(ie))
                                        action.isEnum = cJSON_IsTrue(ie);

                                const cJSON *enumDefsObj = cJSON_GetObjectItem(actionObj, "enumDefs");
                                if (enumDefsObj && cJSON_IsObject(enumDefsObj)) {
                                        for (const cJSON *e = enumDefsObj->child; e; e = e->next) {
                                                if (cJSON_IsString(e)) {
                                                        action.enumDefs.push_back({std::stoll(e->string), e->valuestring});
                                                }
                                        }
                                }
                                step.actions.push_back(action);
                        }
                }
                steps_.push_back(step);
        }
}

void
SequenceEditor::exportToFile(const std::string &path)
{
        cJSON *root = cJSON_CreateObject();
        saveSession(root);
        char *out = cJSON_Print(root);
        if (out) {
                std::filesystem::path p(reinterpret_cast<const char8_t *>(path.c_str()));
                std::ofstream         ofs(p);
                if (ofs) {
                        ofs << out;
                }
                cJSON_free(out);
        }
        cJSON_Delete(root);
}

void
SequenceEditor::importFromFile(const std::string &path)
{
        std::filesystem::path p(reinterpret_cast<const char8_t *>(path.c_str()));
        std::ifstream         ifs(p);
        if (!ifs)
                return;
        std::stringstream ss;
        ss << ifs.rdbuf();
        const std::string content = ss.str();
        if (content.empty())
                return;

        cJSON *root = cJSON_Parse(content.c_str());
        if (root) {
                loadSession(root);
                cJSON_Delete(root);
        }
}
